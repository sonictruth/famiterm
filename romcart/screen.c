#include "screen.h"
#include <neslib.h>
#include <mapper.h>
#include "status.h"

#define EMPTY 0 /* blank tile (a typed space is ' ') */
/* Stop consuming input past this; one input byte adds < 40 (a status_restore()
 * row plus the cell that displaced it), so ulen peaks near UBUF_SOFT + 37.
 *
 * The real ceiling is the NMI, not ubuf[]: flush_vram_update2() copies a tile
 * every 16 cycles, and after OAM DMA there are only ~1700 of vblank left, so
 * past ~100 queued bytes the copy runs on into rendering. 170 predates
 * MAX_PER_FRAME being large enough to reach it -- at 8 bytes a frame the byte
 * cap bound first -- and a sustained ESC K/ESC Y repaint now would: it queued
 * 198 and finished 13 scanlines into the picture. 96 holds that to 99, inside
 * vblank, and leaves the ordinary text path (a margin blank plus a row of
 * glyphs, 94-96) untouched.
 *
 * It is also as low as is worth going, because it is not the last word on the
 * budget: screen_begin_frame() queues up to ERASE_ROWS_PER_FRAME * 33 = 132 on
 * its own, before screen_can_accept() is consulted at all. Getting the whole
 * frame inside vblank means shrinking that and the 66-byte two-row margin
 * blank, not lowering this further. */
#define UBUF_SOFT 96
#define ERASE_ROWS_PER_FRAME 4
#define CURSOR_TILE 0xFF /* standalone heart with a transparent background (see gen-fonts.py) */
#define BLINK_FRAMES 30

/* Palettes as {background, foreground}; index 0 is the default. */
static const unsigned char palettes[5][2] = {
    {0x12, 0x38}, /* yellow on blue */
    {0x0F, 0x2A}, /* green on black */
    {0x0F, 0x27}, /* amber on black */
    {0x0F, 0x30}, /* white on black */
    {0x30, 0x0F}, /* black on white */
};
#define NUM_FONTS 7
#define DEFAULT_FONT 5 /* ZX Chicago */
static unsigned char pal_idx, font_idx = DEFAULT_FONT;

/* Display names, in CHR bank order (see chr.S) */
static const char *const font_names[NUM_FONTS] = {
    "Altered Chrome", "Around", "Bauhaus", "Computing 60s",
    "Security Concepts", "ZX Chicago", "ZX OCR-A",
};

/* CHR bank 0 is the splash, fonts follow (see chr.S); the font lives in pattern table 0. */
static void apply_font(void) {
  set_chr_bank_0(font_idx + 1);
  set_chr_bank_1(font_idx + 1); /* cursor sprite reads the same font */
  bank_bg(0);
  bank_spr(1);
}

static void show_font_name(void) {
  static char msg[COLS + 1];
  const char *n = font_names[font_idx];
  unsigned char i = 0, j = 0;
  static const char pre[] = "Font: ";
  while (pre[j]) msg[i++] = pre[j++];
  for (j = 0; n[j] && i < COLS; j++) msg[i++] = n[j];
  msg[i] = 0;
  show_status(msg);
}

static void apply_palette(void) {
  unsigned char buf[16], i;
  for (i = 0; i < 16; i += 4)
    buf[i] = buf[i + 1] = buf[i + 2] = palettes[pal_idx][0], buf[i + 3] = palettes[pal_idx][1];
  pal_bg(buf);
  /* The cursor sprite uses palette 0 and tile 0xFF (pixels are colour 2, background is
   * colour 0 = transparent).  Colour 2 is red; the other entries are unused. */
  buf[0] = buf[1] = buf[3] = 0x0F;
  buf[2] = 0x16; /* red */
  pal_spr(buf);
}

/* D-pad up/down cycle the palette; right/left cycle the font. */
void screen_poll_input(void) {
  static unsigned char prev;
  unsigned char pad = pad_poll(0), hit = pad & ~prev;
  if (hit & (PAD_UP | PAD_DOWN)) {
    if (hit & PAD_UP) pal_idx = pal_idx + 1 >= 5 ? 0 : pal_idx + 1;
    else pal_idx = pal_idx ? pal_idx - 1 : 4;
    apply_palette();
  }
  if (hit & (PAD_RIGHT | PAD_LEFT)) {
    if (hit & PAD_RIGHT) font_idx = font_idx + 1 >= NUM_FONTS ? 0 : font_idx + 1;
    else font_idx = font_idx ? font_idx - 1 : NUM_FONTS - 1;
    apply_font();
    show_font_name();
  }
  prev = pad;
}

/* VT52 special graphics set, 0x5E-0x7E -> CP437 tile */
static const unsigned char gfx_map[33] = {
    ' ', ' ', ' ',                           /* 5E-60 reserved */
    0xDB, '1', '3', '5', '7',                /* solid block, fraction numerators */
    0xF8, 0xF1, 0x1A, 0xFA, 0xF6, 0x19,      /* degree, +-, ->, ellipsis, divide, down arrow */
    0xDF, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4, 0xC4, '_', /* scan lines 0-7 */
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', /* subscripts */
    0x14,                                    /* paragraph */
};

/* Scrolling: a 60-row ring over two stacked nametables. `top` is the ring row shown at the
 * top of the screen; `row` is the cursor's terminal row (0..ROWS-1).  The terminal is
 * drawn inset by X0/Y0 tiles so the outer border stays blank (TV overscan). */
#define NT_ROWS 30
#define X0 1
#define Y0 1
unsigned char col, row, cls;
unsigned char saved_col, saved_row, gfx, wrap;
static unsigned char top, blink;
/* Pending multi-frame erase of whole rows [er_a, er_b) */
static unsigned char er_a, er_b;
/* A scroll moved text into the top or bottom margin row; blank both next frame. */
static unsigned char border;

/* VRAM update buffer for neslib's NMI handler */
static unsigned char ubuf[250];
static unsigned char ulen, run_open, run_hdr;
static unsigned run_end;

/* Address of tile column x on visible screen row v (0..NT_ROWS-1). */
static unsigned view_adr(unsigned char x, unsigned char v) {
  unsigned char rr = top + v;
  if (rr >= 2 * NT_ROWS) rr -= 2 * NT_ROWS;
  if (rr >= NT_ROWS) return NAMETABLE_C | ((unsigned)(rr - NT_ROWS) << 5) | x;
  return NAMETABLE_A | ((unsigned)rr << 5) | x;
}

static unsigned cell_adr(unsigned char c, unsigned char r) {
  return view_adr(c + X0, r + Y0);
}

/* Address of the first cell of a screen row. */
unsigned screen_row_adr(unsigned char r) {
  return cell_adr(0, r);
}

/* Raw run of tiles through the update buffer. */
void screen_queue_write(unsigned addr, const unsigned char *data, unsigned char n) {
  run_open = 0;
  ubuf[ulen++] = (addr >> 8) | NT_UPD_HORZ;
  ubuf[ulen++] = addr & 0xFF;
  ubuf[ulen++] = n;
  while (n--) ubuf[ulen++] = *data++;
}

static void put_cell(unsigned char c) {
  unsigned addr = cell_adr(col, row);
  status_guard(addr);
  /* Same cell as the last write: with autowrap off (the VT52 default) every
   * character past COLS lands on the last column, so a host line wider than the
   * screen comes through as one address repeated. Overwrite the byte already
   * queued for it instead of opening a fresh 4-byte run per character -- that
   * grew ubuf by 4 a byte and pushed the NMI's flush past the end of vblank.
   * Safe to index ulen - 1: nothing else appends to ubuf while a run is open
   * (blank_run() and screen_queue_write() both close it first, including via
   * the status_guard() above). */
  if (run_open && run_end == addr + 1) {
    ubuf[ulen - 1] = c;
    return;
  }
  if (run_open && run_end == addr) {
    ubuf[run_hdr + 2]++;
    ubuf[ulen++] = c;
  } else {
    run_hdr = ulen;
    ubuf[ulen++] = (addr >> 8) | NT_UPD_HORZ;
    ubuf[ulen++] = addr & 0xFF;
    ubuf[ulen++] = 1;
    ubuf[ulen++] = c;
    run_open = 1;
  }
  run_end = addr + 1;
}

static void blank_run(unsigned addr, unsigned char n) {
  run_open = 0;
  ubuf[ulen++] = (addr >> 8) | NT_UPD_HORZ;
  ubuf[ulen++] = addr & 0xFF;
  ubuf[ulen++] = n;
  while (n--)
    ubuf[ulen++] = EMPTY;
}

void spaces(unsigned char c0, unsigned char r, unsigned char n) {
  unsigned addr = cell_adr(c0, r);
  status_guard(addr);
  blank_run(addr, n);
}

/* Scrolling (either way) brings the margin row on the far side into the terminal; it is
 * blank because `border` blocks input until both margin rows have been cleared. */
void linefeed(void) {
  if (row < ROWS - 1) {
    row++;
    return;
  }
  status_restore();
  if (++top >= 2 * NT_ROWS) top = 0;
  border = 1;
}

void put_char(unsigned char c) {
  if (!c || c == 7) return; /* NUL ignored, BEL handled by the parser; everything else is a CP437 glyph */
  if (gfx && c >= 0x5E && c <= 0x7E) c = gfx_map[c - 0x5E];
  if (col >= COLS) {
    if (!wrap) col = COLS - 1; /* VT52: no autowrap, last column is overwritten */
    else {
      col = 0;
      linefeed();
    }
  }
  put_cell(c);
  if (col < COLS) col++;
  if (!wrap && col >= COLS) col = COLS - 1;
}

void reverse_linefeed(void) {
  if (row) {
    row--;
    return;
  }
  status_restore();
  top = top ? top - 1 : 2 * NT_ROWS - 1;
  border = 1;
}

void erase_rows(unsigned char a, unsigned char b) {
  if (a < b) {
    if (er_a == er_b) { er_a = a; er_b = b; }
    else { /* merge (only ever contiguous with the rest of the screen) */
      if (a < er_a) er_a = a;
      if (b > er_b) er_b = b;
    }
  }
}

static void clear_vram(void) {
  vram_adr(NAMETABLE_A);
  vram_fill(EMPTY, 960);
  vram_fill(0, 64);
  vram_adr(NAMETABLE_C);
  vram_fill(EMPTY, 960);
  vram_fill(0, 64);
  top = 0;
  er_a = er_b = 0;
  border = 0;
  status_reset();
}

static void clear_screen(void) {
  set_vram_update(0);
  ppu_off();
  clear_vram();
  ppu_on_all();
}

void screen_init(void) {
  ppu_off();
  apply_font(); /* back from the splash picture to the font */
  apply_palette();
  clear_vram();
  vram_adr(screen_row_adr(0));
  vram_write("UP/DOWN - Change palette", 24);
  vram_adr(screen_row_adr(1));
  vram_write("LEFT/RIGHT - Change font", 24);
  vram_adr(screen_row_adr(2));
  vram_write("Ready.", 6);
  col = 0;
  row = 3;
  wrap = 0;
  ppu_on_all();
}

/* Call after ppu_wait_nmi(): the previous frame's buffer has been applied. */
void screen_begin_frame(void) {
  unsigned char n;
  ulen = 0;
  run_open = 0;
  status_tick();
  if (border) {
    blank_run(view_adr(X0, 0), COLS);
    blank_run(view_adr(X0, NT_ROWS - 1), COLS);
    border = 0;
  }
  for (n = 0; n < ERASE_ROWS_PER_FRAME && er_a < er_b; n++)
    spaces(0, er_a++, COLS);
}

unsigned char screen_can_accept(void) {
  return ulen < UBUF_SOFT && !cls && !border && er_a == er_b;
}

void screen_end_frame(void) {
  ubuf[ulen] = NT_UPD_EOF;
  if (cls) {
    cls = 0;
    clear_screen();
    ubuf[0] = NT_UPD_EOF;
  }
  set_vram_update(ubuf);
  scroll(0, top * 8);
  oam_clear();
  if (++blink >= 2 * BLINK_FRAMES) blink = 0;
  if (blink < BLINK_FRAMES)
    oam_spr((X0 + (col < COLS ? col : COLS - 1)) * 8, (Y0 + row) * 8 - 1, CURSOR_TILE, 0);
}
