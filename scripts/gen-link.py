#!/usr/bin/env python3
"""Write the ROM's cycle-timed UART for the nesterm link (PROTOCOL.md).

    scripts/gen-link.py firmware/link-proto.h build/gen/link_rxtx.S [--early=N]

The numbers come from link-proto.h. For LINK_PERIOD CPU cycles a bit this
writes, in llvm-mos assembly:

    unsigned char link_rx_frame(unsigned char tries)
        Receive one reply into link_rx, however long its LEN says it is (cut
        to LINK_NES_RX_MASK). Returns 0 if a whole frame came, 1 if the wait
        for a start bit ran out: `tries` passes of 256 counts of 29 cycles.
    void link_tx_byte(unsigned char byte)
        Send a byte as 8N1.
    void link_seal_tx(unsigned char end)
        Put the CRC of link_tx[1 .. end) at link_tx[end] and [end + 1].
    unsigned char link_check_rx(void)
        0 if the frame link_rx_frame() just received has the right CRC.
    link_tx[], link_rx[]
        The request and reply buffers.

The line (PROTOCOL.md, "The line"): the ESP32-S3 sends on the expansion
port's P2 D1, $4017 bit 1, which the console inverts, so a start bit reads
1. The ROM sends on OUT1, $4016 bit 1, which rests at 0 and is mark: the
ESP32-S3's UART inverts its RX. The controller strobe is bit 0 and writes
bit 1 as 0, so reading the pads never disturbs the line.

Each routine sits on a page of its own (.p2align 8) and must end on it, so
no branch pays the page-crossing cycle; scripts/check-link-pages.py checks
that after linking. Their timing also counts on the zero-page names below
being zero page, which their section makes them.

This is nes-wiki's / nesp32's gen_link.py (the counting receiver), moved
from D0 to D1: a sample is lda / lsr / lsr / ror, two cycles more than
theirs, and a look tests bit 1 with A = 2.

Receiving. t0 is the cycle on which a look's `bit` read the start bit. A
look that comes 6 cycles after another saw an edge that fell 0 to 6 cycles
before, 3 on average; the hunt's first look comes 11 after the one before
it, across the count, and its edge fell 5.5 before on average. Data bit k's
centre is then t0 - that + (1.5 + k) * period. Where the bytes of a frame
follow one another, as a UART sends them, the next start bit is known to a
few cycles, so it is looked for with WINDOW looks 6 cycles apart where it
is due, and only a frame's first byte, or one after a gap, is hunted for by
the loop that counts.

A reply is a header of LINK_HEADER bytes, whose last is the payload's
length, then the payload and two CRC bytes. The receiver counts to the end
of the header and there works out where the frame ends: the length, cut to
LINK_NES_RX_MASK so that no reply can run past the buffer, plus the
overhead. That is the one byte after which the window opens later than
after the others, and it still opens inside the two stop bits.

Sending. The byte is inverted once, then each bit is lsr'd into carry and
made 0 or 2 (rol / asl): the value written to $4016 has only bit 1 set or
none, so the strobe (bit 0) and OUT2 stay low throughout. The stop bit is
held for two bits by the time the routine has returned: the ESP32-S3's UART
has one stop-bit setting for both ways, and it is two for what it sends.
The caller's loop makes the gap longer, which a UART does not mind.
"""

import re
import sys

READ_AT = 3             # lda / bit abs read on their 4th cycle

# Ways to pass time: (cycles, bytes, lines). None of them touch carry or
# A; `bit $00` and `pla` change N, V and Z, which nothing here carries over.
PADS = ((2, 1, ["    nop"]), (3, 2, ["    bit $00"]), (7, 2, ["    pha", "    pla"]))
LOOP_BYTES = 5          # ldy #n, then dey / bne: 5n + 1 cycles

LOOK = 6                # bit abs, bne not taken
COUNTED = LOOK + 5      # and dex, bne: the look that follows the hunt's count
CLOSE = 3               # looks of the hunt that follow another, 6 cycles on
WINDOW = 6              # looks, back to back, where a frame's next byte is due
LOOK_TO_CODE = 4        # from a look's read: the bne taken, to the next instruction
BIT_LOOP = 18           # lda abs, lsr, lsr, ror zp, dex, bne: a sample, less its padding
# A sample is aimed this many cycles before the middle of its bit. nesp32
# measured 0 to be the best on real consoles (a real line's edges are not
# square); --early=N is there to try others.
EARLY = 0
# From a byte's last sample read to the window's first look read: the rest
# of the lda, lsr, lsr, ror, dex, bne not taken, lda zp, eor #, sta abs+y,
# iny, cpy zp, bne taken, lda #, and the look's own read.
TAIL = 1 + 2 + 2 + 5 + 2 + 2 + 3 + 2 + 5 + 2 + 3 + 3 + 2 + READ_AT
# And after the header's last byte: bne not taken, cpy #, bne not taken,
# lda abs, and #, adc #, sta zp, in place of the bne taken.
HEADER_TAIL = TAIL - 3 + 2 + 2 + 2 + 4 + 2 + 2 + 3

JOY1 = "$4016"
JOY2 = "$4017"
LINE_MASK = 2           # bit 1 of both


def read_proto(path):
    values = {}
    with open(path) as handle:
        for line in handle:
            match = re.match(r"\s*#define\s+(LINK_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", line)
            if match:
                values[match.group(1)] = int(match.group(2), 0)
    return values


def delay(cycles, use_y=True):
    """Lines of code that take exactly this many cycles, in the fewest
    bytes. With use_y, counts in Y from 12 cycles up."""
    if cycles == 1 or cycles < 0:
        raise ValueError("cannot pad %d cycles" % cycles)
    return list(_pad(cycles, use_y, {0: (0, [])})[1])


def _pad(cycles, use_y, memo):
    if cycles not in memo:
        choices = []
        for cost, size, lines in PADS:
            if cycles - cost >= 0 and cycles - cost != 1:
                rest = _pad(cycles - cost, use_y, memo)
                choices.append((size + rest[0], lines + rest[1]))
        loops, rest = divmod(cycles - 1, 5)
        if rest == 1:
            loops, rest = loops - 1, 6
        if use_y and 1 <= loops <= 255 and cycles >= 12:
            tail = _pad(rest, use_y, memo)
            label = "1"
            choices.append((LOOP_BYTES + tail[0],
                            ["    ldy #%d" % loops, "%s:  dey" % label, "    bne %sb" % label]
                            + tail[1]))
        memo[cycles] = min(choices, key=lambda choice: choice[0])
    return memo[cycles]


def receiver(proto, early=None):
    period = proto["LINK_PERIOD"]
    to_nes_bits = proto["LINK_TO_NES_BITS"]
    pad = period - BIT_LOOP
    early = EARLY if early is None else early
    # From the look that saw the start bit to the first sample's read:
    # LOOK_TO_CODE, the delay, ldx #, the loop's padding and the lda's read.
    first = int(1.5 * period - early - LOOK / 2 - LOOK_TO_CODE - 2 - pad - READ_AT + 0.5)
    skip = 2
    if pad < 0 or pad == 1 or first - skip < 0 or first - skip == 1:
        raise ValueError("at %d cycles a bit the receiver has no room" % period)
    # The window has to begin before the next byte's start bit can, and go
    # on until after it must have: a sample, and so the byte, may be as much
    # as a look out, either way.
    edge = 0.5 * period + (to_nes_bits - 9) * period    # from a last sample on time
    slack = COUNTED / 2 + 4
    if TAIL > edge - slack:
        raise ValueError("at %d cycles a bit the window opens late" % period)
    if TAIL + (WINDOW - 1) * LOOK < edge + slack:
        raise ValueError("at %d cycles a bit the window closes early" % period)
    if HEADER_TAIL > edge + period - slack:
        raise ValueError("at %d cycles a bit the look after the header is late" % period)

    lines = ['    .section .text.link_rx,"ax",@progbits', "    .p2align 8",
             "; unsigned char link_rx_frame(unsigned char tries)",
             "; Receive a reply into link_rx. %d cycles a bit, %d baud, 8N2."
             % (period, proto["LINK_BAUD"]),
             "; tries: passes of 256 counts of %d cycles to wait for a start bit;"
             % (COUNTED + CLOSE * LOOK),
             "; not wound up again between bytes. Returns 0 if the frame came,",
             "; 1 if the wait ran out. Destroys A, X and Y.",
             "    .globl link_rx_frame", "link_rx_frame:",
             "    sta link_tries",
             "    lda #%d" % proto["LINK_HEADER"],
             "    ldy #0",
             "    sta link_rx_end",
             "    lda #%d                      ; what the looks test the port with" % LINE_MASK,
             "    ldx #0",
             ".Lrx_hunt:                      ; the harness skips ahead while this sees mark",
             "    bit %s" % JOY2, "    bne .Lrx_first"]
    for _ in range(CLOSE):
        lines += ["    bit %s" % JOY2, "    bne .Lrx_byte"]
    lines += ["    dex", "    bne .Lrx_hunt",
              "    dec link_tries", "    bne .Lrx_hunt",
              "    lda #1", "    rts",
              ".Lrx_byte:                      ; from a look 6 cycles after another"]
    lines += delay(skip, use_y=False)
    lines += [".Lrx_first:                     ; from one 11 after"]
    lines += delay(first - skip, use_y=False)
    lines += ["    ldx #8", ".Lrx_bit:"]
    lines += delay(pad, use_y=False)
    lines += ["    lda %s" % JOY2,
              "    lsr a",
              "    lsr a                       ; bit 1 into carry",
              "    ror link_rx_data",
              "    dex", "    bne .Lrx_bit",
              "    lda link_rx_data",
              "    eor #$FF                    ; the console inverts D1",
              "    sta link_rx,y",
              "    iny",
              "    cpy link_rx_end",
              "    bne .Lrx_next",
              "    cpy #%d" % proto["LINK_HEADER"],
              "    bne .Lrx_done",
              "    lda link_rx+%d               ; the payload's length" % (proto["LINK_HEADER"] - 1),
              "    and #$%02X" % proto["LINK_NES_RX_MASK"],
              "    adc #%d                      ; and the carry the compare left"
              % (proto["LINK_OVERHEAD"] - 1),
              "    sta link_rx_end",
              ".Lrx_next:",
              "    lda #%d" % LINE_MASK]
    for _ in range(WINDOW):
        lines += ["    bit %s" % JOY2, "    bne .Lrx_byte"]
    lines += ["    ldx #0                      ; not where it was due: a gap, or the end",
              "    jmp .Lrx_hunt",
              ".Lrx_done:",
              "    lda #0",
              "    rts",
              "    .globl link_rx_frame_end", "link_rx_frame_end:"]
    return lines


def sender(proto):
    period = proto["LINK_PERIOD"]
    compute = 5 + 2 + 2 + 2         # lsr zp, lda #, rol a, asl a
    if period - 4 - compute < 0 or period - 4 - compute == 1:
        raise ValueError("at %d cycles a bit the sender has no room" % period)
    lines = ["", '    .section .text.link_tx,"ax",@progbits', "    .p2align 8",
             "; void link_tx_byte(unsigned char byte)",
             "; Send a byte as 8N1, %d cycles a bit, on $4016 bit 1 (1 = space)." % period,
             "; OUT1 must be at mark (0), and is left there for two bits",
             "; by the time this has returned. Destroys A and Y.",
             "    .globl link_tx_byte", "link_tx_byte:",
             "    eor #$FF                    ; a 0 bit is space, written as 2",
             "    sta link_tx_data",
             "    lda #%d" % LINE_MASK,
             "    sta %s                   ; start bit" % JOY1]
    for bit in range(8):
        lines += ["    lsr link_tx_data", "    lda #0", "    rol a", "    asl a"]
        lines += delay(period - 4 - compute)
        lines += ["    sta %s                   ; bit %d" % (JOY1, bit)]
    lines += ["    lda #0"]
    lines += delay(period - 4 - 2)
    lines += ["    sta %s                   ; stop bit" % JOY1]
    # Two bits of mark, with the rts: the ESP32-S3's stop-bit setting is one
    # for both ways, and two for what it sends.
    lines += delay(2 * period - 6)
    lines += ["    rts", "    .globl link_tx_byte_end", "link_tx_byte_end:"]
    return lines


def crc(proto):
    """The CRC a byte at a time from the table, 34 cycles a byte: what the
    compiler made of the same loop in C took twice that."""
    def loop(buf, label):
        return ["%s:                        ; A = the CRC's high byte" % label,
                "    eor %s,y" % buf, "    tax",
                "    lda link_crc_lo_byte", "    eor link_crc_hi,x", "    sta link_crc_hi_byte",
                "    lda link_crc_lo,x", "    sta link_crc_lo_byte",
                "    lda link_crc_hi_byte",
                "    iny", "    cpy link_crc_end", "    bne %s" % label]
    lines = ["", '    .section .text.link_crc,"ax",@progbits',
             "; void link_seal_tx(unsigned char end): the CRC of link_tx[1 .. end)",
             "; into link_tx[end] and [end + 1]. Destroys A, X and Y.",
             "    .globl link_seal_tx", "link_seal_tx:",
             "    sta link_crc_end", "    ldy #1", "    lda #$FF", "    sta link_crc_lo_byte"]
    lines += loop("link_tx", ".Lseal")
    lines += ["    sta link_tx,y", "    lda link_crc_lo_byte", "    sta link_tx+1,y", "    rts",
              "",
              "; unsigned char link_check_rx(void): 0 if link_rx's CRC is right, 1 if",
              "; not. The frame ends where link_rx_frame() left link_rx_end.",
              "; Destroys A, X and Y.",
              "    .globl link_check_rx", "link_check_rx:",
              "    ldx link_rx_end", "    dex", "    dex", "    stx link_crc_end",
              "    ldy #1", "    lda #$FF", "    sta link_crc_lo_byte"]
    lines += loop("link_rx", ".Lcheck")
    lines += ["    cmp link_rx,y", "    bne .Lcheck_bad",
              "    lda link_crc_lo_byte", "    cmp link_rx+1,y", "    bne .Lcheck_bad",
              "    lda #0", "    rts",
              ".Lcheck_bad:", "    lda #1", "    rts"]
    return lines


def crc_table(poly):
    """T[i]: the CRC register after i << 8 has been through eight rounds.
    Per byte: index = crc_hi ^ byte; crc_hi = crc_lo ^ hi[index];
    crc_lo = lo[index]."""
    entries = []
    for index in range(256):
        crc = index << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
        entries.append(crc)
    return entries


def include(proto, early=None):
    rx_size = proto["LINK_OVERHEAD"] + proto["LINK_NES_RX_MASK"]
    lines = ["; Generated by scripts/gen-link.py from firmware/link-proto.h. Do not edit.",
             "",
             '    .section .zp.bss,"zaw",@nobits',
             "; The four the routines count cycles by: zero page, or the timing is off.",
             "link_rx_data: .zero 1", "link_rx_end: .zero 1",
             "link_tries: .zero 1", "link_tx_data: .zero 1",
             "link_crc_hi_byte: .zero 1", "link_crc_lo_byte: .zero 1", "link_crc_end: .zero 1",
             "",
             '    .section .bss.link_buffers,"aw",@nobits',
             "    .globl link_rx, link_tx",
             "link_rx: .zero %d" % rx_size,
             "link_tx: .zero %d" % (proto["LINK_OVERHEAD"] + proto["LINK_NES_TX_MAX"]),
             ""]
    lines += receiver(proto, early)
    lines += sender(proto)
    lines += crc(proto)
    lines += ["", '    .section .rodata.link_crc,"a",@progbits']
    entries = crc_table(proto["LINK_CRC_POLY"])
    for name, part in (("link_crc_hi", lambda value: value >> 8),
                       ("link_crc_lo", lambda value: value & 0xFF)):
        lines += ["    .globl %s" % name, "%s:" % name]
        for row in range(0, 256, 16):
            lines.append("    .byte " + ", ".join("$%02X" % part(value)
                                                  for value in entries[row:row + 16]))
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    proto = read_proto(argv[1])
    early = [float(option.split("=")[1]) for option in argv[3:] if option.startswith("--early=")]
    text = include(proto, early=early[0] if early else None)
    with open(argv[2], "w") as handle:
        handle.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
