#!/usr/bin/env bash
# Expose the emulated ESP32-S3 as a PTY: connect with `screen /tmp/ttyEMU 115200`.
# Bytes written to the PTY are appended to /tmp/ttyEMU.in, which faketty.lua polls.
set -euo pipefail
IN="${ESP32_S3_IN:-/tmp/ttyEMU.in}"
command -v socat >/dev/null || { echo "socat missing: brew install socat"; exit 1; }
touch "$IN"
echo "PTY: /tmp/ttyEMU  ->  $IN (start the emulator first)"
exec socat -d -d -u "pty,raw,echo=0,link=/tmp/ttyEMU" "open:$IN,append"
