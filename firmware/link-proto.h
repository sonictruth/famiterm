/* The nesterm link: the numbers both ends share (PROTOCOL.md).
 *
 * Included by the ESP32-S3 firmware (link-core.*, link.*), by the ROM
 * (romcart/link.c, with -Ifirmware) and read by scripts/gen-link.py, which
 * writes the ROM's cycle-timed UART routines for LINK_PERIOD. Plain
 * #defines only, one per line, so that all three can read it. */
#ifndef LINK_PROTO_H
#define LINK_PROTO_H

/* --- The line ------------------------------------------------------------- */

#define LINK_CPU_HZ       1789773   /* NTSC 2A03 */
#define LINK_PERIOD       21        /* CPU cycles a bit, both ways */
#define LINK_BAUD         85227     /* LINK_CPU_HZ / LINK_PERIOD, rounded */
#define LINK_TO_NES_BITS  11        /* 8N2: the 2nd stop bit is the ROM's time to store a byte */
#define LINK_TO_ESP_BITS  10        /* 8N1 */

/* --- Frames ----------------------------------------------------------------
 * request  $4E, SVC, OP,     SEQ, LEN, PAYLOAD[LEN], CRC_HI, CRC_LO
 * reply    $A5, SVC, STATUS, SEQ, LEN, PAYLOAD[LEN], CRC_HI, CRC_LO
 * CRC-16/CCITT-FALSE over SVC .. the last payload byte. */

#define LINK_REQUEST      0x4E
#define LINK_REPLY        0xA5
#define LINK_HEADER       5         /* marker .. LEN */
#define LINK_OVERHEAD     7         /* header + CRC */
#define LINK_PAYLOAD_MAX  127
#define LINK_FRAME_MAX    (LINK_OVERHEAD + LINK_PAYLOAD_MAX)
#define LINK_CRC_POLY     0x1021
#define LINK_CRC_INIT     0xFFFF

/* Until a hello says otherwise, no reply payload is longer than this. */
#define LINK_REPLY_MAX_DEFAULT 16

/* The ROM's own limits: the receiver cuts a reply's LEN to LINK_NES_RX_MASK so
 * that nothing is stored past its buffer, and its hello says so. */
#define LINK_NES_RX_MASK  0x3F      /* replies of up to 63 bytes of payload */
#define LINK_NES_TX_MAX   16        /* the longest request payload it sends */
/* How long the ROM waits for a reply's first start bit, in passes of the
 * receiver's count (gen-link.py: 256 x 29 cycles, 4.15 ms a pass). */
#define LINK_NES_TRIES    1

/* --- Status ---------------------------------------------------------------- */

#define LINK_OK           0x00
#define LINK_ATTENTION    0x40      /* a flag on any status; unused so far */
#define LINK_NO_SERVICE   0x80
#define LINK_NO_OP        0x81
#define LINK_BAD_ARG      0x82
#define LINK_BUSY         0x83
#define LINK_INACTIVE     0x84      /* no hello since the ESP32-S3 started */
#define LINK_VERSION      0x88      /* the hello's PROTOCOL is not this firmware's */

/* --- Services -------------------------------------------------------------- */

#define LINK_PROTOCOL     1

#define LINK_SVC_SYS      0x00
#define LINK_SYS_HELLO    0x00      /* PROTOCOL, APP[4], APP_VERSION, REPLY_MAX ->
                                       PROTOCOL, MAJOR, MINOR, PATCH, REQUEST_MAX */
#define LINK_SYS_ECHO     0x02      /* anything -> the same, cut to REPLY_MAX */

#define LINK_SVC_TERM     0x10
#define LINK_TERM_READ    0x00      /* POS, ROOM -> the stream's bytes from POS */
#define LINK_TERM_WRITE   0x01      /* bytes -> COUNT taken, as if typed */

#define LINK_FW_MAJOR     1
#define LINK_FW_MINOR     0
#define LINK_FW_PATCH     0

#endif
