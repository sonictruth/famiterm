# The nesterm link

How the Famicom talks to the ESP32-S3 running fami-modem: a UART on two
wires of the expansion port, framed requests and replies with a CRC, a few
numbered services, and a hello by which the ROM says what it is.

The design is nesp32's link protocol, taken to the Famicom's expansion port:
the same frames, the same CRC, the same rules for lost and repeated frames.

Every number in this document is in
[`firmware/link-proto.h`](firmware/link-proto.h), which the firmware and
the ROM include and [`scripts/gen-link.py`](scripts/gen-link.py) reads.

## 1. Wiring

The same two wires the first protocol used, so a board built for it needs no
change. Pin numbers are in [`firmware/pins.h`](firmware/pins.h), shared
with the Family BASIC keyboard emulator (selected by holding ESC at boot).

| Famicom pin | Signal | Famicom register | ESP32-S3 | Direction | Link |
| --- | --- | --- | --- | --- | --- |
| 11 | OUT1 | `$4016` bit 1 (write) | GPIO5 | Fam -> ESP | UART RX |
| 7 | P2 D1 | `$4017` bit 1 (read) | GPIO7 | ESP -> Fam | UART TX |

The full wiring, covering both modes:

| Famicom Pin | Signal | ESP32-S3 Pin | Direction | Keyboard emulator | Modem |
| --- | --- | --- | --- | --- | --- |
|wh12| OUT0 | GPIO4 | Fam -> ESP | row reset | - |
|gr11| OUT1 | GPIO5 | Fam -> ESP | column / row advance | **UART RX** |
|yl10| OUT2 | GPIO6 | Fam -> ESP | keyboard enable | - |
|br7| P2 D1 | GPIO7 | ESP -> Fam | `$4017` bit 1 | **UART TX** |
|pl6| P2 D2 | GPIO15 | ESP -> Fam | `$4017` bit 2 | - |
|gr5| P2 D3 | GPIO16 | ESP -> Fam | `$4017` bit 3 | - |
|bl4| P2 D4 | GPIO17 | ESP -> Fam | `$4017` bit 4 | - |

### Level shifting

The Famicom peripheral port drives 5V logic; the ESP32-S3 is a 3.3V part and
its GPIOs are **not** 5V-tolerant (absolute max ~3.6V). Every signal in the
table above goes through an **8-channel bidirectional level shifter**:

* **HV side:** the Famicom expansion-port lines, with HV referenced to the
  Famicom's 5V.
* **LV side:** the ESP32-S3 GPIOs, with LV referenced to the ESP32-S3's 3.3V.
* **Channels:** the seven signals above use seven of the eight channels; one
  is spare.
* **GND:** the Famicom, the level shifter and the ESP32-S3 must share a
  ground reference.

The link runs at 85227 baud, a bit every 11.7 µs. A shifter whose pull-ups
make an edge take a microsecond or two is still well inside that, but it is
the first thing to suspect if replies arrive damaged.

### Polarity

`$4017` bits 1-4 are **inverted by the console**: a GPIO driven HIGH reads 0
at `$4017`, LOW reads 1 (NESdev, "Family BASIC Keyboard"; the keyboard
emulator relies on it). So the ESP32-S3's TX is left as it is: it rests high
(mark), which the ROM reads as 0, and a start bit reads 1.

OUT1 is the CPU's own output and is not inverted. The ROM rests it at 0, and
so does the controller strobe, which writes `$4016` with bit 1 clear. So for
OUT1, 0 is mark, and the ESP32-S3's UART inverts its RX.

Both are build settings in
[`famicom-link.cpp`](firmware/famicom-link.cpp): `LINK_TX_INVERTED` (0)
and `LINK_RX_INVERTED` (1), for a board whose shifter inverts. The firmware
prints both at boot.

### Sharing `$4016`

The ROM's line is **OUT1, `$4016` bit 1**, and not OUT0: bit 0 is the
controller strobe, which `pad_poll()` writes 1 and then 0 every frame. Those
writes have bit 1 clear, which is mark, so reading the pads never disturbs
the line. The ROM's sender writes only 0 or 2, so the strobe and OUT2 stay
low while it sends.

## 2. The line

| | |
|---|---|
| Rate | 21 Famicom CPU cycles a bit, 85227 baud, both ways |
| Famicom to ESP32-S3 | 8N1, with at least two bits of mark between bytes |
| ESP32-S3 to Famicom | 8N2: the second stop bit is the ROM's time to store a byte |
| CRC | CRC-16/CCITT-FALSE (polynomial `$1021`, start `$FFFF`), high byte first |

The ROM bit-bangs both directions by counting CPU cycles: its routines are
written for the rate by [`scripts/gen-link.py`](scripts/gen-link.py)
(section 7). The ESP32-S3 uses UART1 through the GPIO matrix, so its side of
the timing is hardware. The ESP32-S3's UART has one stop-bit setting for both
ways, two, and the ROM leaves two bits of mark after every byte it sends.

## 3. Frames

The console is in charge. The ESP32-S3 sends only in answer to a request,
and answers every request it acts on with exactly one reply.

```text
request   $4E, SVC, OP,     SEQ, LEN, PAYLOAD[LEN], CRC_HI, CRC_LO     Famicom to ESP32-S3
reply     $A5, SVC, STATUS, SEQ, LEN, PAYLOAD[LEN], CRC_HI, CRC_LO     ESP32-S3 to Famicom
```

| Field | Meaning |
|---|---|
| `SVC` | the service asked (section 6). A reply repeats it |
| `OP` | what is asked of it |
| `SEQ` | the request's sequence number. A reply repeats it |
| `LEN` | bytes of payload, 0 to 127 |
| `STATUS` | how it went (section 4) |
| CRC | over `SVC` through the last byte of the payload |

**A new request has a new `SEQ`.** The ROM counts it up by one for each.

**The same request again gets the same reply again.** If a request has the
`OP`, `SEQ`, `LEN` and CRC of the last one the ESP32-S3 answered for that
service, it sends the bytes it sent then and does nothing else. So a console
that heard no reply may send the request again unchanged, and nothing is done
twice. Replies are held this way unless a service says otherwise (a hello's,
and TERM READ's, are not).

**Only the newest request is acted on.** If the ESP32-S3 finds several
requests waiting, it drops all but the last: the console listens only just
after it asks, so the older ones are past answering.

**The ESP32-S3 is never silent on purpose.** It answers from what it holds,
at once; nothing it does on the way to a reply waits for the network.

**A reply that is not for the request is dropped.** The ROM checks the
marker, the service, the sequence number, the length against its buffer, and
the CRC. Anything else is the same as no reply, and the next frame's request
asks again.

The ESP32-S3's decoder looks for `$4E` and a good CRC, and passes over
anything else a byte at a time. A marker in the noise with a plausible length
does not hold up the request behind it: every position is tried for a whole
frame with a good CRC, and what can no longer begin one is dropped.

nesp32 chose the CRC over two running sums by measurement: on 60,000 damaged
frames of each kind the sums let two wrong bits through one time in 150, and
the CRC none. The ROM works it out from a 512-byte table at 34 cycles a byte.

## 4. Status

| Value | Name | Meaning |
|---|---|---|
| `$00` | OK | |
| `$40` | ATTENTION | a flag on any status; unused so far. The ROM takes it off |
| `$80` | NO_SERVICE | this firmware has no such service |
| `$81` | NO_OP | the service has no such op |
| `$82` | BAD_ARG | the payload is not what the op takes |
| `$83` | BUSY | not now: ask again, with a new `SEQ` |
| `$84` | INACTIVE | no hello since the ESP32-S3 started |
| `$88` | VERSION | the hello speaks a protocol this firmware does not |

Bit 7 marks an error.

**INACTIVE tells the ROM that the ESP32-S3 was switched off and on.** One
that starts knows of no hello, so the ROM's next TERM request is refused with
it, and the ROM says hello again.

## 5. Hello

The first request the ROM makes after reset, and after any INACTIVE, is SYS
HELLO. Until one is answered it asks again every frame.

```text
request payload   PROTOCOL, APP[4], APP_VERSION, REPLY_MAX
reply payload     PROTOCOL, MAJOR, MINOR, PATCH, REQUEST_MAX
```

| Field | Meaning |
|---|---|
| `PROTOCOL` | 1 |
| `APP` | four characters that name the ROM: `TERM` |
| `REPLY_MAX` | the longest reply payload the ROM takes, 16 to 127: the ROM's is 63. Until a hello no reply carries more than 16 |
| `MAJOR`, `MINOR`, `PATCH` | the firmware's version |
| `REQUEST_MAX` | the longest request payload the ESP32-S3 takes: 127 |

The status is VERSION, with the same payload, if the protocol is not one the
firmware speaks. A hello forgets every held reply and starts the terminal
stream's count from 0 (TERM, below).

A reset of the console is a new hello, so nothing needs doing on the
ESP32-S3.

## 6. Services

| `SVC` | Name | For |
|---|---|---|
| `$00` | SYS | hello, echo |
| `$10` | TERM | the terminal stream, and typing |
| `$80`-`$FF` | | experiments |

### SYS `$00`

| `OP` | Name | Request | Reply |
|---|---|---|---|
| `$00` | HELLO | section 5 | section 5 |
| `$02` | ECHO | anything | the same, cut to `REPLY_MAX` |

### TERM `$10`

| `OP` | Name | Request | Reply |
|---|---|---|---|
| `$00` | READ | `POS`, `ROOM` | the stream's bytes from `POS`, up to `ROOM` |
| `$01` | WRITE | bytes | `COUNT`: how many were taken, as if typed |

Before a hello both are INACTIVE.

**READ is "I have this much, and room for that much more".** The stream is
everything the modem has for the screen: its echo and results, and what the
remote sends. `POS` is the count of the stream's bytes the console has taken
since its hello, mod 256, and the reply's bytes are the stream's from there,
as many as `ROOM` and `REPLY_MAX` allow. The ESP32-S3 keeps what it sent until
a request's `POS` has moved past it. So:

- a reply that was lost is sent again by the next request, which has the same
  `POS`;
- a request that was lost changes nothing;
- neither needs handling of its own, and READ's reply is never held for a
  repeat: the same request again is a new question.

The console asks every frame, also with no room: that is how what it has
taken is let go of. A `POS` that claims more than was sent is taken as it
stands. A hello starts the count from 0 again; what had been sent and not
acknowledged is sent again from there, since a console that was reset lost
it.

The modem's output goes through a 512-byte queue
([`term.cpp`](firmware/term.cpp)): it waits while the queue is full, which
paces the network to what the console draws, and if no one has taken any for
half a second it drops its output until someone does.

**WRITE** is the way back: its bytes go to the modem as if typed on the USB
keyboard. The reply is held for a repeat, so a press sent again for want of a
reply is typed once. The ROM does not use it yet: the keyboard is on the
ESP32-S3.

## 7. The ROM's side

[`romcart/link.c`](romcart/link.c), with routines that
[`scripts/gen-link.py`](scripts/gen-link.py) writes into
`build/gen/link_rxtx.S` for `LINK_PERIOD`.

- `link_poll()`, once a frame, straight after `ppu_wait_nmi()`: a hello if one
  is needed, then TERM READ with as much room as its 128-byte ring has, up to
  24 bytes. The exchange is cycle-timed and must not be cut by an NMI; right
  after one it has the whole frame.
- `link_getc()`: the next byte from the ring, for `terminal_feed()`. The main
  loop draws up to `MAX_PER_FRAME` (24) a frame; what a busy frame could not
  draw waits in the ring, and READ's `ROOM` keeps the ring from overflowing.
- `link_call(svc, op, len)`: any request, with its payload in
  `link_request`. It returns the reply's status, with the payload in
  `link_reply`, or `LINK_NO_REPLY`.

What `gen-link.py` writes:

| | |
|---|---|
| `link_tx_byte` | a byte as 8N1 on `$4016` bit 1. The byte is inverted once, then each bit is shifted into carry and made 0 or 2: 15 cycles of work and 6 of padding a bit |
| `link_rx_frame` | a reply into `link_rx`, however long its `LEN` says, cut to `LINK_NES_RX_MASK` (63) so that nothing is stored past the buffer. Returns 1 if no start bit came within `LINK_NES_TRIES` passes of 4.15 ms |
| `link_seal_tx`, `link_check_rx` | the CRC, from the table |

The receiver is nesp32's counting receiver moved from D0 to D1. A look for a
start bit is `bit $4017` with A = 2, then `bne`: 6 cycles. A frame's first
byte is hunted for by a loop of four looks to a count, 6 cycles apart and 11
across the count; every byte after it is looked for with six looks, 6 cycles
apart, where it is due, since the ESP32-S3's UART sends a reply's bytes back
to back. Samples are `lda $4017 / lsr / lsr / ror`, aimed at the middle of
each bit. After the header's last byte the receiver works out where the frame
ends, and the window for the next byte still opens inside the two stop bits.
The generator checks those margins for the rate, and refuses a rate they do
not fit.

Each timed routine has a page to itself, so no branch pays the
page-crossing cycle; [`scripts/check-link-pages.py`](scripts/check-link-pages.py)
fails the build if one crosses. The four zero-page bytes the routines count
cycles by are defined with them.

## 8. Timing

Measured in Mesen 2 (`make bench`, `make check`); a frame is 29780 cycles.

- **The ESP32-S3 answers no sooner than 100 µs** after it has a request's
  last byte. The ROM is listening within about 50 µs of its last stop bit's
  start.
- **The ROM waits 4.15 ms** for a reply's first start bit. The ESP32-S3's
  link task has core 1 and the highest priority there, so its answer should
  take a fraction of a millisecond (`ATI7` shows the last and the slowest);
  the emulated line passes with replies up to 2.8 ms late.
- An exchange is 9 bytes out and up to 31 back: 10145 cycles with 24 bytes of
  text, ~4000 of them the request, the header and the CRC. A full frame of
  text, exchange and drawing, takes 26398 of the 29780 cycles.
- With no ESP32-S3 the ROM pays the hello and the wait, ~10000 cycles a
  frame, and draws nothing.
- The emulated line delivers every byte with the ESP32-S3's clock 3% out
  either way, with one reply in five lost, and with one in four damaged.

## 9. The firmware's side

[`firmware/link-core.cpp`](firmware/link-core.cpp) is the protocol, with
no Arduino or FreeRTOS in it: the decoder, the held replies, SYS and TERM.
[`firmware/famicom-link.cpp`](firmware/famicom-link.cpp) puts it on
UART1: an interrupt for every byte, one task that hands what came to the core
and writes the reply straight into the UART's 128-byte FIFO, so its bytes
leave back to back. A request partway in for 2 ms is dropped as noise.

`ATI7` prints the link's counts to the serial log: requests, repeats,
requests dropped for newer ones, noise bytes, hellos, framing errors and the
time to answer, with a line on what they point to.

## 10. Testing

| | |
|---|---|
| `make check` | the core's unit tests on this computer, then `tools/check.lua` in Mesen 2 five times: a clean line; late, jittery replies with the clock 2% fast and 2% slow; one reply in four lost and one in five damaged; the ESP32-S3 power-cycled mid-stream. Each streams a few kilobytes and compares the screen they leave, cell by cell |
| `make check-core` | the core's unit tests alone |
| `make bench` | cycles a frame, bytes a frame and vblank, with an endless stream (`BENCH_PAT=lines`, ...) |

[`tools/esp-link.lua`](tools/esp-link.lua) is the ESP32-S3 in Mesen: a UART
at the level of CPU cycles on `$4016`/`$4017` bit 1, and the same core as
`link-core.cpp`, in Lua. It also checks the ROM's own sending: every edge it
makes must fall a whole number of bits after its start bit. `make run` and
`make test` use it too.

## 11. Adding to it

- A new service takes a number, lists its ops in `link-proto.h`, and is
  served in `LinkCore::serve`, which must never wait.
- A new op is a new number. An op is never given a new meaning.
- A reply may grow at its end; the ROM reads the bytes it knows.
- Anything that breaks those rules is a new `PROTOCOL`, which a hello finds
  out.
