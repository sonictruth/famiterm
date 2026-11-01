#!/usr/bin/env bash
# Download the LLVM-MOS SDK into ./.sdk (skipped if already present).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/.sdk"
[ -x "$DEST/bin/mos-nes-nrom-clang" ] && { echo "SDK already at $DEST"; exit 0; }

case "$(uname -s)" in
  Darwin) ASSET=llvm-mos-macos.tar.xz ;;
  Linux)  ASSET=llvm-mos-linux.tar.xz ;;
  *) echo "unsupported OS; install the SDK manually and set LLVM_MOS_HOME" >&2; exit 1 ;;
esac

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
curl -fL "https://github.com/llvm-mos/llvm-mos-sdk/releases/latest/download/$ASSET" -o "$TMP/sdk.tar.xz"
tar -xJf "$TMP/sdk.tar.xz" -C "$TMP"
rm -rf "$DEST"; mv "$TMP/llvm-mos" "$DEST"
echo "SDK installed at $DEST"
