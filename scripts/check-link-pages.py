#!/usr/bin/env python3
"""Fail if a cycle-timed link routine crosses a page (scripts/gen-link.py).

    scripts/check-link-pages.py path/to/llvm-nm rom.nes.elf

A branch taken across a page boundary costs a cycle more, which the
routines do not count on, so each must lie on one page.
"""
import subprocess
import sys

ROUTINES = ("link_rx_frame", "link_tx_byte")


def main(argv):
    out = subprocess.run([argv[1], argv[2]], check=True, capture_output=True, text=True).stdout
    symbols = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    bad = 0
    for name in ROUTINES:
        start, end = symbols[name], symbols[name + "_end"]
        if start >> 8 != (end - 1) >> 8:
            print("%s crosses a page: $%04X-$%04X" % (name, start, end - 1), file=sys.stderr)
            bad = 1
    return bad


if __name__ == "__main__":
    sys.exit(main(sys.argv))
