#include "splash.h"
#include "chr/splash_pic.h"
#include "sound.h"
#include <neslib.h>
#include <mapper.h>

/* Full-screen bitmap: the picture tiles live in the second 4 KB of CHR-ROM, so
 * bank_bg(1) switches the background to pattern table 1.  Attributes give each
 * 16x16 block one of the four picture palettes.  Display is off while writing. */
void splash_picture(void)
{
  unsigned t;
  ppu_off();
  set_chr_bank_1(0);
  bank_bg(1);
  pal_bg(splash_pic_pal);
  vram_adr(NAMETABLE_A);
  vram_write(splash_pic_tiles, 960);
  vram_adr(NAMETABLE_A + 960); /* attribute table: $23C0 */
  vram_write(splash_pic_attrs, 64);
  scroll(0, 0);
  ppu_on_bg();
  sound_intro_start();
  /* START, A, B or SELECT skips at once; otherwise auto-advance after ~5 s. */
  for (t = 0; t < 300; t++) {
    ppu_wait_nmi();
    if (pad_poll(0) & (PAD_START | PAD_A | PAD_B | PAD_SELECT))
      break;
    sound_intro_tick();
  }
  sound_intro_stop(); /* also cuts the blip short when skipped */
}
