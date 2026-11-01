#include "sound.h"

#define APU_P1_CTRL (*(volatile unsigned char *)0x4000)
#define APU_P1_SWEEP (*(volatile unsigned char *)0x4001)
#define APU_P1_LO (*(volatile unsigned char *)0x4002)
#define APU_P1_HI (*(volatile unsigned char *)0x4003)
#define APU_STATUS (*(volatile unsigned char *)0x4015)

static unsigned char bell;

/* Pulse 1 blip with a decaying envelope; period = 1789773 / (16 * Hz) - 1 */
static void beep(unsigned char period) {
  APU_P1_CTRL = 0x84; /* 50% duty, envelope decay */
  APU_P1_SWEEP = 0x08;
  APU_P1_LO = period;
  APU_P1_HI = 0x08;
}

void sound_init(void) {
  APU_STATUS = 0x01; /* enable pulse 1 */
}

void sound_bell(void) {
  bell = 1;
}

/* BEL: two-note "ding" (E6 then A6, 5 frames apart) */
void sound_tick(void) {
  if (bell == 1) {
    beep(83);
    bell = 2;
  } else if (bell && ++bell == 7) {
    beep(63);
    bell = 0;
  }
}

/* --- splash blip: the two-note "boop" from a Nintendo menu ----------------- */

static unsigned char intro_step, intro_left;

/* Soft B5 (period 112) then E6 (period 84): a short rising blip, not a tune. */
void sound_intro_start(void) {
  APU_STATUS = 0x01; /* pulse 1 only */
  APU_P1_SWEEP = 0x00;
  APU_P1_CTRL = 0x84; /* 50% duty, quiet decaying envelope */
  APU_P1_LO = 112;
  APU_P1_HI = 0x08;
  intro_step = 1;
  intro_left = 3;
}

void sound_intro_tick(void) {
  if (!intro_step) return;
  if (--intro_left == 0) {
    if (intro_step == 1) {
      beep(84);
      intro_step = 2;
      intro_left = 4;
    } else {
      sound_intro_stop();
    }
  }
}

void sound_intro_stop(void) {
  intro_step = 0;
  APU_P1_CTRL = 0xB0; /* constant volume 0 */
  APU_STATUS = 0x00;
}
