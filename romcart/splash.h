#ifndef SPLASH_H
#define SPLASH_H

/* Full-screen colour picture from CHR bank 1; returns on START/A/B/SELECT or
 * after about five seconds.  Leaves rendering on and CHR bank 1 selected. */
void splash_picture(void);

#endif
