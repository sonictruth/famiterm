// Unit tests for firmware/link-core.cpp, on the computer: `make check-core`.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "link-core.h"

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);                  \
      failures++;                                                              \
    }                                                                          \
  } while (0)

typedef std::vector<uint8_t> Bytes;

static std::string stream;           // terminal output waiting for the console
static Bytes typedBytes;

static size_t take(uint8_t *out, size_t n) {
  if (n > stream.size()) n = stream.size();
  memcpy(out, stream.data(), n);
  stream.erase(0, n);
  return n;
}
static size_t typed(const uint8_t *in, size_t n) {
  typedBytes.insert(typedBytes.end(), in, in + n);
  return n;
}

static Bytes request(uint8_t svc, uint8_t op, uint8_t seq, const Bytes &payload) {
  Bytes f = {LINK_REQUEST, svc, op, seq, (uint8_t)payload.size()};
  f.insert(f.end(), payload.begin(), payload.end());
  uint16_t c = LinkCore::crc(f.data() + 1, f.size() - 1);
  f.push_back(c >> 8);
  f.push_back(c & 0xFF);
  return f;
}

struct Reply {
  bool ok = false;                   // a well-formed reply came
  uint8_t svc = 0, status = 0, seq = 0;
  Bytes payload;
  Bytes raw;
};

static Reply feed(LinkCore &core, const Bytes &bytes) {
  uint8_t out[LINK_FRAME_MAX];
  Reply r;
  size_t n = core.feed(bytes.data(), bytes.size(), out);
  if (n == 0) return r;
  r.raw.assign(out, out + n);
  uint16_t c = LinkCore::crc(out + 1, n - 3);
  r.ok = n >= LINK_OVERHEAD && out[0] == LINK_REPLY && out[4] == n - LINK_OVERHEAD &&
         out[n - 2] == (c >> 8) && out[n - 1] == (c & 0xFF);
  r.svc = out[1];
  r.status = out[2];
  r.seq = out[3];
  r.payload.assign(out + LINK_HEADER, out + n - 2);
  return r;
}

static Bytes hello(uint8_t protocol = LINK_PROTOCOL, uint8_t replyMax = LINK_NES_RX_MASK) {
  return {protocol, 'T', 'E', 'R', 'M', 1, replyMax};
}

static std::string str(const Bytes &b) { return std::string(b.begin(), b.end()); }

int main() {
  const LinkHost host = {take, typed};

  // The check value of CRC-16/CCITT-FALSE.
  const uint8_t check[] = "123456789";
  CHECK(LinkCore::crc(check, 9) == 0x29B1);

  {  // Before a hello, TERM is INACTIVE; SYS still answers.
    LinkCore core(host);
    Reply r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 1, {0, 16}));
    CHECK(r.ok && r.svc == LINK_SVC_TERM && r.seq == 1 && r.status == LINK_INACTIVE);
    r = feed(core, request(LINK_SVC_SYS, LINK_SYS_ECHO, 2, {1, 2, 3}));
    CHECK(r.ok && r.status == LINK_OK && r.payload == Bytes({1, 2, 3}));
  }

  {  // Hello: version, reply size, and a protocol it does not speak.
    LinkCore core(host);
    Reply r = feed(core, request(LINK_SVC_SYS, LINK_SYS_HELLO, 1, hello(2)));
    CHECK(r.ok && r.status == LINK_VERSION && r.payload.size() == 5 && r.payload[0] == 1);
    CHECK(!core.helloed());
    r = feed(core, request(LINK_SVC_SYS, LINK_SYS_HELLO, 2, hello()));
    CHECK(r.ok && r.status == LINK_OK);
    CHECK(r.payload == Bytes({LINK_PROTOCOL, LINK_FW_MAJOR, LINK_FW_MINOR, LINK_FW_PATCH,
                              LINK_PAYLOAD_MAX}));
    CHECK(core.helloed() && core.replyMax() == LINK_NES_RX_MASK);
    r = feed(core, request(LINK_SVC_SYS, LINK_SYS_HELLO, 3, {1, 2}));
    CHECK(r.ok && r.status == LINK_BAD_ARG);
    feed(core, request(LINK_SVC_SYS, LINK_SYS_HELLO, 4, hello(1, 3)));
    CHECK(core.replyMax() == LINK_REPLY_MAX_DEFAULT);       // never under 16
    Bytes big(40, 7);
    r = feed(core, request(LINK_SVC_SYS, LINK_SYS_ECHO, 5, big));
    CHECK(r.ok && r.payload.size() == LINK_REPLY_MAX_DEFAULT);
  }

  {  // TERM READ: by position, so a lost reply or request costs nothing.
    LinkCore core(host);
    stream = "The quick brown fox jumps over the lazy dog";
    feed(core, request(LINK_SVC_SYS, LINK_SYS_HELLO, 1, hello()));
    Reply r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 2, {0, 4}));
    CHECK(r.ok && r.status == LINK_OK && str(r.payload) == "The ");
    // reply lost: the console asks from 0 again, and gets the same, and more room
    r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 3, {0, 10}));
    CHECK(r.ok && str(r.payload) == "The quick ");
    r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 4, {10, 6}));
    CHECK(str(r.payload) == "brown ");
    CHECK(core.unacknowledged() == 6);
    // no room: nothing sent, but what was had is let go of
    r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 5, {16, 0}));
    CHECK(r.ok && r.payload.empty() && core.unacknowledged() == 0);
    r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 6, {16, 100}));
    CHECK(str(r.payload) == "fox jumps over the lazy dog");
    // the count wraps at 256
    stream = std::string(300, 'x');
    uint8_t pos = 16 + 27;
    size_t got = 0;
    for (uint8_t seq = 7; got < 300; seq++) {
      r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, seq, {pos, 50}));
      CHECK(r.ok && r.status == LINK_OK);
      pos += r.payload.size();
      got += r.payload.size();
      if (r.payload.empty()) break;
    }
    CHECK(got == 300 && stream.empty());
    // the console resets: hello, and what it had not acknowledged comes again
    stream = "abcdef";
    r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 40, {pos, 3}));
    CHECK(str(r.payload) == "abc");
    feed(core, request(LINK_SVC_SYS, LINK_SYS_HELLO, 41, hello()));
    r = feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 42, {0, 10}));
    CHECK(str(r.payload) == "abcdef");
    CHECK(feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 43, {0, 1})).ok);
    CHECK(feed(core, request(LINK_SVC_TERM, LINK_TERM_READ, 44, {1})).status == LINK_BAD_ARG);
  }

  {  // Held replies: the same request again is answered the same, done once.
    LinkCore core(host);
    feed(core, request(LINK_SVC_SYS, LINK_SYS_HELLO, 1, hello()));
    typedBytes.clear();
    Bytes w = request(LINK_SVC_TERM, LINK_TERM_WRITE, 9, {'l', 's', '\r'});
    Reply a = feed(core, w), b = feed(core, w);
    CHECK(a.ok && a.status == LINK_OK && a.payload == Bytes({3}));
    CHECK(a.raw == b.raw && core.counters().repeats == 1 && str(typedBytes) == "ls\r");
    Reply c = feed(core, request(LINK_SVC_TERM, LINK_TERM_WRITE, 10, {'l', 's', '\r'}));
    CHECK(c.ok && str(typedBytes) == "ls\rls\r");
    CHECK(feed(core, request(LINK_SVC_TERM, 0x7F, 11, {})).status == LINK_NO_OP);
    CHECK(feed(core, request(0x42, 0, 12, {})).status == LINK_NO_SERVICE);
  }

  {  // The decoder: noise, a false marker, split frames, several waiting.
    LinkCore core(host);
    Bytes noisy = {0x00, 0xFF, LINK_REQUEST, 0x10, 0x00, 0x00, 100, 0x55};  // a marker, len 100
    Bytes e = request(LINK_SVC_SYS, LINK_SYS_ECHO, 7, {'h', 'i'});
    noisy.insert(noisy.end(), e.begin(), e.end());
    Reply r = feed(core, noisy);
    CHECK(r.ok && r.seq == 7 && str(r.payload) == "hi");

    Bytes f = request(LINK_SVC_SYS, LINK_SYS_ECHO, 8, {'o', 'k'});
    uint8_t out[LINK_FRAME_MAX];
    core.flush();
    CHECK(core.feed(f.data(), 4, out) == 0 && core.partial());
    r = feed(core, Bytes(f.begin() + 4, f.end()));
    CHECK(r.ok && r.seq == 8);

    uint32_t dropped = core.counters().dropped;
    Bytes two = request(LINK_SVC_SYS, LINK_SYS_ECHO, 20, {'a'});
    Bytes newer = request(LINK_SVC_SYS, LINK_SYS_ECHO, 21, {'b'});
    two.insert(two.end(), newer.begin(), newer.end());
    r = feed(core, two);
    CHECK(r.ok && r.seq == 21 && str(r.payload) == "b");
    CHECK(core.counters().dropped == dropped + 1);

    Bytes bad = request(LINK_SVC_SYS, LINK_SYS_ECHO, 30, {'x'});
    bad[5] ^= 0x10;
    CHECK(!feed(core, bad).ok);
    core.flush();
    CHECK(!core.partial());

    // More than the buffer holds, all of it markers whose "lengths" are
    // plausible: the request behind them is still found.
    Bytes junk(1000, LINK_REQUEST);
    CHECK(!feed(core, junk).ok);
    r = feed(core, request(LINK_SVC_SYS, LINK_SYS_ECHO, 32, {'z'}));
    CHECK(r.ok && r.seq == 32);
  }

  printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
