#include "link.h"

/* gen-link.py's routines (build/gen/link_rxtx.S). */
unsigned char link_rx_frame(unsigned char tries);
void link_tx_byte(unsigned char byte);
void link_seal_tx(unsigned char end);
unsigned char link_check_rx(void);

/* How much of the stream one exchange asks for. Every byte of a reply costs
 * the frame 231 cycles on the wire and ~40 more for its CRC, on top of what
 * terminal_feed() then spends on it, so this is kept near what the screen
 * takes in a frame (MAX_PER_FRAME in main.c). The ring holds what a busy
 * frame could not draw. */
#ifndef LINK_READ_MAX
#define LINK_READ_MAX 24
#endif
#define RING 128 /* a power of two */

unsigned char link_ready;

static unsigned char seq;

static unsigned char ring[RING];
static unsigned char ring_in, ring_out; /* free-running; their difference is the fill */
static unsigned char pos;               /* stream bytes taken since the hello, mod 256 */

unsigned char link_call(unsigned char svc, unsigned char op, unsigned char len) {
  unsigned char i, end, n;

  link_tx[0] = LINK_REQUEST;
  link_tx[1] = svc;
  link_tx[2] = op;
  link_tx[3] = ++seq;
  link_tx[4] = len;
  end = LINK_HEADER + len;
  link_seal_tx(end);

  /* Nothing between the last stop bit and the receiver's first look but the
   * loop's exit. The ESP32-S3 answers no sooner than 100 us after the last
   * byte (PROTOCOL.md, "Timing"); a start bit that began before the receiver
   * looked would be sampled late. */
  end += 2;
  for (i = 0; i < end; i++) link_tx_byte(link_tx[i]);
  if (link_rx_frame(LINK_NES_TRIES)) return LINK_NO_REPLY;

  n = link_reply_len;
  if (link_rx[0] != LINK_REPLY || link_rx[1] != svc || link_rx[3] != seq ||
      n > LINK_NES_RX_MASK || link_check_rx())
    return LINK_NO_REPLY;
  return link_rx[2] & ~LINK_ATTENTION;
}

static void hello(void) {
  unsigned char *p = link_request;
  p[0] = LINK_PROTOCOL;
  p[1] = 'T';
  p[2] = 'E';
  p[3] = 'R';
  p[4] = 'M';
  p[5] = 1; /* APP_VERSION */
  p[6] = LINK_NES_RX_MASK;
  if (link_call(LINK_SVC_SYS, LINK_SYS_HELLO, 7) == LINK_OK) {
    link_ready = 1;
    pos = 0;
  }
}

void link_poll(void) {
  unsigned char room, n, i, st;

  if (!link_ready) {
    hello();
    return;
  }
  room = RING - (unsigned char)(ring_in - ring_out);
  if (room > LINK_READ_MAX) room = LINK_READ_MAX;
  link_request[0] = pos;
  link_request[1] = room;
  st = link_call(LINK_SVC_TERM, LINK_TERM_READ, 2);
  if (st == LINK_INACTIVE) { /* the ESP32-S3 was switched off and on */
    link_ready = 0;
    return;
  }
  if (st != LINK_OK) return;
  n = link_reply_len;
  if (n > room) n = room; /* never, from a firmware that keeps to ROOM */
  for (i = 0; i < n; i++) ring[(unsigned char)(ring_in + i) & (RING - 1)] = link_reply[i];
  ring_in += n;
  pos += n;
}

int link_getc(void) {
  unsigned char c;
  if (ring_in == ring_out) return -1;
  c = ring[ring_out & (RING - 1)];
  ring_out++;
  return c;
}
