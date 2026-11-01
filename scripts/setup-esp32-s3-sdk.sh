#!/usr/bin/env bash
# Install the ESP32-S3 SDK (arduino-cli + esp32:esp32 core) into ./.esp32-s3-sdk.
# The core and its toolchain live in this project's own data dir, so the
# user's Arduino IDE install (~/Library/Arduino15) is left untouched. To reuse
# an existing install instead, run with ARDUINO_DIRECTORIES_DATA=~/Library/Arduino15.
# (arduino-cli is the build tool regardless of target board; the ESP32-S3
# support itself comes from the esp32:esp32 core.)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/.esp32-s3-sdk"
CLI="$DEST/bin/arduino-cli"
VERSION="${ARDUINO_CLI_VERSION:-1.5.1}"
# core to build against; override with ESP32_S3_CORE=esp32:esp32:...
CORE="${ESP32_S3_CORE:-esp32:esp32}"

export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$DEST/data}"

if [ ! -x "$CLI" ]; then
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)  ASSET=macOS_ARM64 ;;
    Darwin-x86_64) ASSET=macOS_64bit ;;
    Linux-aarch64) ASSET=Linux_ARM64 ;;
    Linux-x86_64)  ASSET=Linux_64bit ;;
    *) echo "unsupported OS/arch; install arduino-cli manually and set ESP32_S3_CLI" >&2; exit 1 ;;
  esac

  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  URL="https://github.com/arduino/arduino-cli/releases/download/v$VERSION/arduino-cli_${VERSION}_${ASSET}.tar.gz"
  echo "downloading arduino-cli $VERSION ($ASSET)"
  curl -fL "$URL" -o "$TMP/cli.tar.gz"
  tar -xzf "$TMP/cli.tar.gz" -C "$TMP"
  mkdir -p "$DEST/bin"
  mv "$TMP/arduino-cli" "$CLI"
fi

if ! "$CLI" core list 2>/dev/null | grep -q "^$CORE "; then
  echo "installing core $CORE"
  "$CLI" core update-index >/dev/null 2>&1 || true
  "$CLI" core install "$CORE"
fi

if ! "$CLI" lib list 2>/dev/null | grep -q "^EspUsbHost "; then
  echo "installing library EspUsbHost"
  "$CLI" lib update-index >/dev/null 2>&1 || true
  "$CLI" lib install EspUsbHost
fi

echo "ESP32-S3 SDK ready: $CLI (core $CORE)"
