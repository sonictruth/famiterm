#ifndef STATUS_H
#define STATUS_H

/* Temporary full-width status line on the bottom visible screen row.  The row it covers
 * is saved and put back after ~2 s, or as soon as the terminal touches that row. */
void show_status(const char *msg);

/* Called by screen.c */
void status_tick(void);                  /* once per frame, after the update buffer is reset */
void status_guard(unsigned addr);        /* a cell at addr is about to be written */
void status_restore(void);               /* put the saved row back now (no-op when idle) */
void status_reset(void);                 /* VRAM was cleared; forget the saved row */

#endif
