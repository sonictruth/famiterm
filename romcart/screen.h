#ifndef SCREEN_H
#define SCREEN_H

#define COLS 30 /* terminal size; a 1-tile margin around it keeps text out of TV overscan */
#define ROWS 28

/* Cursor and modes; the terminal parser reads and writes these directly. */
extern unsigned char col, row;
extern unsigned char saved_col, saved_row;
extern unsigned char gfx;  /* VT52 special graphics set active */
extern unsigned char wrap; /* autowrap */
extern unsigned char cls;  /* full clear pending */

void screen_init(void);
void screen_begin_frame(void);
void screen_poll_input(void); /* up/down: palette, left/right: font */
unsigned char screen_can_accept(void); /* room for more input this frame */
void screen_end_frame(void);

unsigned screen_row_adr(unsigned char r);
void screen_queue_write(unsigned addr, const unsigned char *data, unsigned char n);

void put_char(unsigned char c);
void linefeed(void);
void reverse_linefeed(void);
void spaces(unsigned char c0, unsigned char r, unsigned char n);
void erase_rows(unsigned char a, unsigned char b);

#endif
