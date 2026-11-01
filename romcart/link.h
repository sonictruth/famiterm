#ifndef LINK_H
#define LINK_H

/* The ROM's end of the link (PROTOCOL.md): framed requests to the ESP32-S3
 * over a bit-banged UART, and the terminal stream they carry.
 *
 * Call link_poll() once a frame, straight after ppu_wait_nmi(): the exchange
 * is cycle-timed and must not be cut by the next NMI, and there it has the
 * whole frame. Then take bytes with link_getc(). */

#include "link-proto.h"

/* link_call()'s answer when no good reply came: none, or one that failed its
 * checks. Not a status the ESP32-S3 can send. */
#define LINK_NO_REPLY 0xFF

/* A request's payload goes here before link_call() (gen-link.py's buffer). */
extern unsigned char link_tx[];
#define link_request (link_tx + LINK_HEADER)

/* The last reply as received (gen-link.py); its payload and length. */
extern unsigned char link_rx[];
#define link_reply (link_rx + LINK_HEADER)
#define link_reply_len (link_rx[LINK_HEADER - 1])

/* Send a request with `len` bytes of link_request and wait for its reply:
 * its status (LINK_ATTENTION taken off), or LINK_NO_REPLY. */
unsigned char link_call(unsigned char svc, unsigned char op, unsigned char len);

/* Say hello if need be, then ask for as much of the terminal stream as there
 * is room for. Once a frame. */
void link_poll(void);

/* The next byte of the terminal stream, or -1 if none has come. */
int link_getc(void);

/* 1 once a hello has been answered, 0 until then (and after the ESP32-S3
 * says it has not had one). */
extern unsigned char link_ready;

#endif
