#include <ines.h>
#include <mapper.h>
#include <neslib.h>
#include "link.h"
#include "screen.h"
#include "splash.h"
#include "sound.h"
#include "terminal.h"

MAPPER_PRG_ROM_KB(32);
MAPPER_CHR_ROM_KB(32); /* old EverDrive N8: MMC1 CHR banks past 32 KB read garbage */
MAPPER_USE_HORIZONTAL_MIRRORING; /* $2000/$2400 = A, $2800/$2C00 = B: two screens stacked vertically */

/* Bytes drawn per frame, and the ROM's real throughput knob: the burst hits
 * this cap on every frame, so text arrives at up to 60 * MAX_PER_FRAME bytes/s.
 * link_poll() asks the ESP32-S3 for as many (LINK_READ_MAX in link.c), and its
 * ring keeps what a frame could not draw.
 *
 * The exchange costs ~4000 cycles a frame and ~265 a byte (231 on the wire,
 * 34 of CRC), and terminal_feed() and the put_char() path behind it ~460 a
 * byte more. Two ceilings close in on that, and 24 is inside both:
 *
 *   - CPU. Work has to finish before the NMI. Overshooting is not fatal --
 *     neslib applies the update buffer only when ppu_wait_nmi() armed it, so a
 *     long frame is delivered one frame late rather than corrupted -- but it
 *     halves the rate, and it would cut the link's exchange, which must not
 *     be. At 24 a solid run of text spends 26398 cycles of the 29780.
 *   - Vblank. Whatever the burst queues, the NMI has to copy out in ~2270
 *     cycles at 16 cycles a tile. At 32 the autowrap case ended its flush on
 *     the pre-render line, i.e. outside vblank, on 390 frames in 400; at 28
 *     every pattern measured finished by scanline 260.
 *
 * Measured in Mesen (`make bench`, an endless stream, 400 frames): text
 * 1442 B/s, CRLF-terminated lines 811, autowrap 901, a TUI repaint 1082. The
 * line cases stall because a line longer than this is two frames: one row is
 * up to COLS + CR + LF = 32 bytes, and screen_can_accept() ends the burst at
 * the scroll a bottom-row linefeed queues, after which the next
 * screen_begin_frame() blanks the two margin rows. Lifting those to one row a
 * frame means raising this to 32, which needs the vblank ceiling raised first
 * -- the 66-byte margin blank is the bulk of it -- and the CPU's: at 32 the
 * exchange alone is ~12500 cycles. */
#ifndef MAX_PER_FRAME /* -DMAX_PER_FRAME=n overrides */
#define MAX_PER_FRAME 24
#endif

int main(void) {
  splash_picture();
  screen_init();
  sound_init();

  for (;;) {
    unsigned char n;
    ppu_wait_nmi();
    /* The exchange with the ESP32-S3 first: it is cycle-timed, and here it
     * has the whole frame before the next NMI could cut it. */
    link_poll();
    sound_tick();
    screen_poll_input();
    screen_begin_frame();
    for (n = 0; n < MAX_PER_FRAME && screen_can_accept(); n++) {
      int b = link_getc();
      if (b < 0) break;
      terminal_feed((unsigned char)b);
    }
    screen_end_frame();
  }
}
