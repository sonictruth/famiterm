#ifndef SOUND_H
#define SOUND_H

void sound_init(void);
void sound_bell(void); /* queue a BEL */
void sound_tick(void); /* call once per frame */

/* Short blip played while the splash picture is up. */
void sound_intro_start(void);
void sound_intro_tick(void); /* call once per frame */
void sound_intro_stop(void); /* silence; safe to call when not playing */

#endif
