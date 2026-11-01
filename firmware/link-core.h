// The link's core: frames in, replies out (PROTOCOL.md). No Arduino and no
// FreeRTOS in here, so that it builds and is tested on a computer
// (test/link-core-test.cpp, `make check-core`); link.cpp is what puts it on
// the UART.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "link-proto.h"

// What the core asks of the rest of the firmware. Called from the link task;
// neither may wait.
struct LinkHost {
  // Up to `n` bytes of terminal output for the console, into `out`: how many.
  size_t (*take)(uint8_t *out, size_t n);
  // Bytes the console typed (TERM WRITE), for the modem: how many it took.
  size_t (*typed)(const uint8_t *in, size_t n);
};

struct LinkCounters {
  uint32_t requests;   // requests acted on
  uint32_t repeats;    // of those, answered from a held reply
  uint32_t dropped;    // requests passed over because a newer one was waiting
  uint32_t noise;      // bytes that were in no good frame
  uint32_t hellos;
};

class LinkCore {
public:
  explicit LinkCore(const LinkHost &host);

  // Bytes off the line. Returns the length of the reply put in `reply`
  // (room for LINK_FRAME_MAX), or 0 if there is none to send yet.
  size_t feed(const uint8_t *data, size_t len, uint8_t *reply);

  // What has come of a request so far is noise: nothing more came for it.
  void flush();
  bool partial() const { return rxLen_ != 0; }

  const LinkCounters &counters() const { return counters_; }
  bool helloed() const { return helloed_; }
  uint8_t replyMax() const { return replyMax_; }
  // Stream bytes taken from the host and not yet acknowledged by the console.
  size_t unacknowledged() const { return heldLen_; }

  // CRC-16/CCITT-FALSE.
  static uint16_t crc(const uint8_t *data, size_t len);

private:
  struct Held {               // a reply kept for the same request again
    bool valid;
    uint8_t op, seq, len;
    uint16_t crc;
    uint8_t frame[LINK_FRAME_MAX];
    size_t size;
  };

  size_t serve(const uint8_t *request, uint8_t *reply);
  size_t frame(uint8_t *reply, uint8_t svc, uint8_t status, uint8_t seq,
               const uint8_t *payload, uint8_t len);
  uint8_t termRead(const uint8_t *payload, uint8_t len, uint8_t *out, uint8_t *outLen);
  Held *heldFor(uint8_t svc);

  LinkHost host_;
  LinkCounters counters_ = {};
  bool helloed_ = false;
  uint8_t replyMax_ = LINK_REPLY_MAX_DEFAULT;

  // Bytes off the line not yet in a frame: room for two whole frames.
  uint8_t rx_[2 * LINK_FRAME_MAX];
  size_t rxLen_ = 0;

  // The terminal stream: bytes taken from the host and sent at least once,
  // not yet acknowledged; heldBase_ is the console's position of the first.
  uint8_t held_[LINK_PAYLOAD_MAX];
  size_t heldLen_ = 0;
  uint8_t heldBase_ = 0;

  Held sysHeld_ = {}, termHeld_ = {};
};
