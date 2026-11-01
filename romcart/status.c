#include "status.h"
#include "screen.h"
#include <neslib.h>

#define STATUS_FRAMES 120
#define EMPTY 0

static unsigned char active, timer;
static unsigned char saved[COLS];
static unsigned adr;

void show_status(const char *msg) {
  unsigned char buf[COLS], i = 0;
  if (!active) {
    adr = screen_row_adr(ROWS - 1);
    ppu_off();
    vram_adr(adr);
    vram_read(saved, COLS);
  } else {
    ppu_off();
  }
  while (msg[i] && i < COLS) buf[i] = msg[i], i++;
  while (i < COLS) buf[i++] = EMPTY;
  vram_adr(adr);
  vram_write(buf, COLS);
  ppu_on_all();
  active = 1;
  timer = STATUS_FRAMES;
}

void status_restore(void) {
  if (!active) return;
  active = 0;
  screen_queue_write(adr, saved, COLS);
}

void status_tick(void) {
  if (active && !--timer) status_restore();
}

void status_guard(unsigned addr) {
  if (active && addr >= adr && addr < adr + COLS) status_restore();
}

void status_reset(void) {
  active = 0;
}
