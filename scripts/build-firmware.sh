#!/usr/bin/env bash
# Build (and optionally flash) the firmware/ sketch (ESP32-S3 firmware).
# The board and its serial port are auto-detected via `arduino-cli board list`;
# set ESP32_S3_FQBN / ESP32_S3_PORT to override either one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ESP32_S3_CLI:-$ROOT/.esp32-s3-sdk/bin/arduino-cli}"
export ARDUINO_DIRECTORIES_DATA="${ESP32_S3_DATA:-$ROOT/.esp32-s3-sdk/data}"
SKETCH="$ROOT/firmware"
OUT="${ESP32_S3_BUILD:-$ROOT/build/esp32-s3}"
DEFAULT_FQBN="esp32:esp32:esp32s3"

UPLOAD=0
[ "${1:-}" = "--upload" ] && UPLOAD=1

[ -x "$CLI" ] || { echo "arduino-cli not found: $CLI (run 'make sdk-esp32-s3')" >&2; exit 1; }

# Prints "<port>\t<fqbn>" for the connected board; either field may be empty.
# An explicit port is used even when arduino-cli can't identify the board.
detect() {
  "$CLI" board list --format json 2>/dev/null | python3 -c '
import json, sys
want = sys.argv[1] if len(sys.argv) > 1 else ""
try:
    doc = json.load(sys.stdin)
except Exception:
    print("\t"); raise SystemExit

rows = []
for entry in (doc.get("detected_ports") or doc.get("ports") or []):
    p = entry.get("port") or {}
    port = p.get("address", "")
    props = p.get("properties") or {}
    fqbns = [b.get("fqbn", "") for b in (entry.get("matching_boards") or []) if b.get("fqbn")]
    if port:
        rows.append((port, fqbns, props.get("vid", "").upper(),
                     p.get("hardware_id") or props.get("serialNumber") or port))

def first_esp32(fqbns):
    return next((f for f in fqbns if f.startswith("esp32:esp32:")), fqbns[0] if fqbns else "")

# Espressif native USB, then common USB-serial bridges on ESP32 boards
# (WCH CH34x, Silabs CP210x, FTDI)
KNOWN_VIDS = ("0X303A", "0X1A86", "0X10C4", "0X0403")

def pick_usb():
    # one entry per physical device: macOS lists a CH34x twice (cu.usbmodem*
    # via the Apple driver, cu.wchusbserial* via the WCH one); prefer the
    # vendor-driver node. Non-USB ports (Bluetooth, debug-console) are skipped.
    devices = {}
    for r in sorted(rows, key=lambda r: "usbmodem" in r[0]):
        if r[2]:
            devices.setdefault(r[3], r)
    devs = list(devices.values())
    known = [r for r in devs if r[2] in KNOWN_VIDS]
    for cand in (known, devs):
        if len(cand) == 1:
            return cand[0]
    return None

if want:
    port, fqbns = want, next((r[1] for r in rows if r[0] == want), [])
else:
    # an identified ESP32 board wins; otherwise a single USB serial device
    # (typical USB-serial bridge) is used; several are ambiguous -> ask for
    # ESP32_S3_PORT
    esp32 = [r for r in rows if first_esp32(r[1]).startswith("esp32:esp32:")]
    usb = pick_usb()
    if esp32:
        port, fqbns = esp32[0][0], esp32[0][1]
    elif usb:
        port, fqbns = usb[0], usb[1]
    else:
        port, fqbns = "", []

print("%s\t%s" % (port, first_esp32(fqbns)))
' "${ESP32_S3_PORT:-}"
}

IFS=$'\t' read -r PORT DETECTED_FQBN < <(detect)

if [ -n "${ESP32_S3_FQBN:-}" ]; then
  FQBN="$ESP32_S3_FQBN"
  WHY="from ESP32_S3_FQBN"
elif [ -n "$DETECTED_FQBN" ]; then
  FQBN="$DETECTED_FQBN"
  WHY="detected${PORT:+ on $PORT}"
else
  FQBN="$DEFAULT_FQBN"
  WHY="default, board not identified"
fi

echo "board: $FQBN ($WHY)"

# The esp32 core's postsavehex hooks copy the binaries into
# <sketch>/build/<fqbn>/ on every build, whatever --output-dir says. Each hook
# begins with fqbn_to_dir && ..., so making that step fail turns them all off
# and everything stays in $OUT.
"$CLI" compile --fqbn "$FQBN" --build-path "$OUT/cache" --output-dir "$OUT" \
  --build-property "recipe.hooks.savehex.fqbn_to_dir=false" "$SKETCH"
echo "built: $OUT/firmware.ino.bin"

if [ "$UPLOAD" = 1 ]; then
  [ -n "$PORT" ] || {
    echo "no serial port found; plug the board in or set ESP32_S3_PORT (ls /dev/cu.*)" >&2
    exit 1
  }
  echo "port:  $PORT"
  [ -n "$DETECTED_FQBN" ] || echo "note:  $PORT was not reported as an ESP32; set ESP32_S3_PORT to target another port"
  "$CLI" upload --fqbn "$FQBN" -p "$PORT" --input-dir "$OUT" "$SKETCH"
fi
