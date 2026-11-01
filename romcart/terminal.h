#ifndef TERMINAL_H
#define TERMINAL_H

/* Feed one received byte through the VT52 parser. */
void terminal_feed(unsigned char c);

#endif
