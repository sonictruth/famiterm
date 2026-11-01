#include "link-core.h"
#include <string.h>

LinkCore::LinkCore(const LinkHost &host) : host_(host) {}

uint16_t LinkCore::crc(const uint8_t *data, size_t len) {
  uint16_t crc = LINK_CRC_INIT;
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ LINK_CRC_POLY) : (uint16_t)(crc << 1);
  }
  return crc;
}

void LinkCore::flush() {
  counters_.noise += rxLen_;
  rxLen_ = 0;
}

// A whole request with a good CRC at rx_[at]: its length, or 0.
static size_t goodFrame(const uint8_t *rx, size_t avail) {
  if (avail < LINK_OVERHEAD || rx[0] != LINK_REQUEST) return 0;
  uint8_t len = rx[LINK_HEADER - 1];
  if (len > LINK_PAYLOAD_MAX || avail < (size_t)(LINK_OVERHEAD + len)) return 0;
  uint16_t want = LinkCore::crc(rx + 1, LINK_HEADER - 1 + len);
  const uint8_t *got = rx + LINK_HEADER + len;
  return (got[0] == (want >> 8) && got[1] == (want & 0xFF)) ? LINK_OVERHEAD + len : 0;
}

// Could a frame still begin at rx_[at], once more bytes have come?
static bool mayBecomeFrame(const uint8_t *rx, size_t avail) {
  if (rx[0] != LINK_REQUEST) return false;
  if (avail < LINK_HEADER) return true;
  uint8_t len = rx[LINK_HEADER - 1];
  return len <= LINK_PAYLOAD_MAX && avail < (size_t)(LINK_OVERHEAD + len);
}

// The decoder finds a request by its marker and its CRC, wherever it is in
// what came: a marker in the noise with a plausible length does not hold up
// the request behind it. Of several requests waiting only the newest is
// acted on (PROTOCOL.md, "Frames").
size_t LinkCore::feed(const uint8_t *data, size_t len, uint8_t *reply) {
  uint8_t request[LINK_FRAME_MAX];
  bool have = false;

  while (len) {
    size_t room = sizeof(rx_) - rxLen_;
    if (room == 0) {                      // nothing in it can be a frame now
      size_t drop = LINK_FRAME_MAX;
      counters_.noise += drop;
      memmove(rx_, rx_ + drop, rxLen_ - drop);
      rxLen_ -= drop;
      room = drop;
    }
    size_t n = len < room ? len : room;
    memcpy(rx_ + rxLen_, data, n);
    rxLen_ += n;
    data += n;
    len -= n;

    for (;;) {
      size_t at = 0, size = 0;
      for (; at < rxLen_; at++) {
        if ((size = goodFrame(rx_ + at, rxLen_ - at)) != 0) break;
      }
      if (size) {
        if (have) counters_.dropped++;
        memcpy(request, rx_ + at, size);
        have = true;
        counters_.noise += at;
        memmove(rx_, rx_ + at + size, rxLen_ - at - size);
        rxLen_ -= at + size;
        continue;
      }
      // No whole frame: keep only what could still begin one.
      size_t keep = 0;
      while (keep < rxLen_ && !mayBecomeFrame(rx_ + keep, rxLen_ - keep)) keep++;
      counters_.noise += keep;
      memmove(rx_, rx_ + keep, rxLen_ - keep);
      rxLen_ -= keep;
      break;
    }
  }
  return have ? serve(request, reply) : 0;
}

size_t LinkCore::frame(uint8_t *reply, uint8_t svc, uint8_t status, uint8_t seq,
                       const uint8_t *payload, uint8_t len) {
  reply[0] = LINK_REPLY;
  reply[1] = svc;
  reply[2] = status;
  reply[3] = seq;
  reply[4] = len;
  if (len) memmove(reply + LINK_HEADER, payload, len);
  uint16_t c = crc(reply + 1, LINK_HEADER - 1 + len);
  reply[LINK_HEADER + len] = c >> 8;
  reply[LINK_HEADER + len + 1] = c & 0xFF;
  return LINK_OVERHEAD + len;
}

LinkCore::Held *LinkCore::heldFor(uint8_t svc) {
  switch (svc) {
  case LINK_SVC_SYS:  return &sysHeld_;
  case LINK_SVC_TERM: return &termHeld_;
  default:            return nullptr;
  }
}

// TERM READ: POS is the count of stream bytes the console has taken, mod 256.
// What it has taken is let go of; then as much as ROOM and REPLY_MAX allow is
// sent from POS. So a lost reply is sent again by the next request, which has
// the same POS, and a lost request changes nothing.
uint8_t LinkCore::termRead(const uint8_t *payload, uint8_t len, uint8_t *out, uint8_t *outLen) {
  *outLen = 0;
  if (len != 2) return LINK_BAD_ARG;
  uint8_t pos = payload[0], room = payload[1];
  uint8_t taken = (uint8_t)(pos - heldBase_);
  if (taken <= heldLen_) {
    memmove(held_, held_ + taken, heldLen_ - taken);
    heldLen_ -= taken;
  }
  // A POS past what was sent: the console said hello to another firmware's
  // stream, or this one restarted. Its count is the one that goes on.
  heldBase_ = pos;

  size_t want = room < replyMax_ ? room : replyMax_;
  if (heldLen_ < want) heldLen_ += host_.take(held_ + heldLen_, want - heldLen_);
  size_t n = heldLen_ < want ? heldLen_ : want;
  memcpy(out, held_, n);
  *outLen = (uint8_t)n;
  return LINK_OK;
}

size_t LinkCore::serve(const uint8_t *request, uint8_t *reply) {
  uint8_t svc = request[1], op = request[2], seq = request[3], len = request[4];
  const uint8_t *payload = request + LINK_HEADER;
  uint16_t reqCrc = (uint16_t)(payload[len] << 8 | payload[len + 1]);
  counters_.requests++;

  // The same request again gets the same reply again, and nothing is done
  // twice.
  Held *held = heldFor(svc);
  if (held && held->valid && held->op == op && held->seq == seq && held->len == len &&
      held->crc == reqCrc) {
    counters_.repeats++;
    memcpy(reply, held->frame, held->size);
    return held->size;
  }

  uint8_t out[LINK_PAYLOAD_MAX];
  uint8_t outLen = 0, status;
  bool hold = true;                      // replies are held unless said otherwise

  if (svc == LINK_SVC_SYS && op == LINK_SYS_HELLO) {
    hold = false;
    out[0] = LINK_PROTOCOL;
    out[1] = LINK_FW_MAJOR;
    out[2] = LINK_FW_MINOR;
    out[3] = LINK_FW_PATCH;
    out[4] = LINK_PAYLOAD_MAX;
    outLen = 5;
    if (len < 7) {
      status = LINK_BAD_ARG;
      outLen = 0;
    } else if (payload[0] != LINK_PROTOCOL) {
      status = LINK_VERSION;
    } else {
      uint8_t max = payload[6];
      replyMax_ = max < LINK_REPLY_MAX_DEFAULT ? LINK_REPLY_MAX_DEFAULT
                : max > LINK_PAYLOAD_MAX       ? LINK_PAYLOAD_MAX : max;
      helloed_ = true;
      // A new console, or the same one reset: its count starts again. What
      // was sent and not acknowledged is sent again, from 0.
      heldBase_ = 0;
      sysHeld_.valid = termHeld_.valid = false;
      counters_.hellos++;
      status = LINK_OK;
    }
  } else if (svc == LINK_SVC_SYS && op == LINK_SYS_ECHO) {
    outLen = len < replyMax_ ? len : replyMax_;
    memcpy(out, payload, outLen);
    status = LINK_OK;
  } else if (svc == LINK_SVC_SYS) {
    status = LINK_NO_OP;
  } else if (svc == LINK_SVC_TERM && !helloed_) {
    // Switched off and on since the console's hello: it says hello again.
    status = LINK_INACTIVE;
    hold = false;
  } else if (svc == LINK_SVC_TERM && op == LINK_TERM_READ) {
    status = termRead(payload, len, out, &outLen);
    hold = false;                        // POS makes it safe to ask again
  } else if (svc == LINK_SVC_TERM && op == LINK_TERM_WRITE) {
    out[0] = (uint8_t)host_.typed(payload, len);
    outLen = 1;
    status = LINK_OK;
  } else if (svc == LINK_SVC_TERM) {
    status = LINK_NO_OP;
  } else {
    status = LINK_NO_SERVICE;
  }

  size_t size = frame(reply, svc, status, seq, out, outLen);
  if (held) {
    held->valid = hold && status != LINK_BUSY;
    if (held->valid) {
      held->op = op;
      held->seq = seq;
      held->len = len;
      held->crc = reqCrc;
      memcpy(held->frame, reply, size);
      held->size = size;
    }
  }
  return size;
}
