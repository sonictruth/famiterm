#include "terminal.h"
#include "screen.h"
#include "sound.h"

enum { S_NORM, S_ESC, S_Y1, S_Y2, S_CSI };
static unsigned char st;

static void esc(unsigned char c) {
  st = S_NORM;
  switch (c) {
  case 'A': if (row) row--; break;
  case 'B': if (row < ROWS - 1) row++; break;
  case 'C': if (col < COLS - 1) col++; break;
  case 'D': if (col) col--; break;
  case 'F': gfx = 1; break;
  case 'G': gfx = 0; break;
  case 'H': row = col = 0; break;
  case 'I': reverse_linefeed(); break;
  case 'J': /* erase to end of screen */
    if (!row && !col) cls = 1;
    else {
      if (col < COLS) spaces(col, row, COLS - col);
      erase_rows(row + 1, ROWS);
    }
    break;
  case 'K': if (col < COLS) spaces(col, row, COLS - col); break;
  case 'Y': st = S_Y1; break;
  case 0x1B: st = S_ESC; break;
  /* ANSI CSI (e.g. bash's ESC[?2004h bracketed paste) is not VT52: swallow it */
  case '[': st = S_CSI; break;
  /* Atari/Heath extensions */
  case 'E': cls = 1; row = col = 0; break;
  case 'd': /* erase to start of screen */
    if (col) spaces(0, row, col + 1 > COLS ? COLS : col + 1);
    erase_rows(0, row);
    break;
  case 'l': spaces(0, row, COLS); col = 0; break;
  case 'o': if (col) spaces(0, row, col + 1 > COLS ? COLS : col + 1); break;
  case 'j': saved_col = col; saved_row = row; break;
  case 'k': col = saved_col; row = saved_row; break;
  case 'v': wrap = 1; break;
  case 'w': wrap = 0; break;
  /* ignored: Z (identify, no TX path), = > (keypad), < (ANSI mode), \ ] ^ _ (hardcopy,
     print), e f (cursor), p q (reverse video), b c (colour) */
  }
}

void terminal_feed(unsigned char c) {
  switch (st) {
  case S_ESC:
    esc(c);
    return;
  case S_Y1:
    if (c == 0x1B) { st = S_ESC; return; }
    row = c < 0x20 ? 0 : c - 0x20;
    if (row >= ROWS) row = ROWS - 1;
    st = S_Y2;
    return;
  case S_Y2:
    if (c == 0x1B) { st = S_ESC; return; }
    col = c < 0x20 ? 0 : c - 0x20;
    if (col >= COLS) col = COLS - 1;
    st = S_NORM;
    return;
  case S_CSI:
    /* parameter/intermediate bytes 0x20-0x3F continue, final byte 0x40-0x7E ends */
    if (c == 0x1B) st = S_ESC;
    else if (c >= 0x40 && c <= 0x7E) st = S_NORM;
    else if (c < 0x20 || c > 0x7E) { st = S_NORM; break; }
    return;
  }
  if (c == 0x1B) st = S_ESC;
  else if (c == 7) sound_bell();
  else if (c == '\r') col = 0;
  else if (c == '\n') linefeed();
  else if (c == '\b') { if (col) col--; }
  else if (c == '\t') { col = (col + 8) & ~7; if (col >= COLS) col = COLS - 1; }
  else put_char(c);
}
