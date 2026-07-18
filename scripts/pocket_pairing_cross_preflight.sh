#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 /path/to/hari-at-3ad9deee99f49099de19b5052264dc02e6273080" >&2
  exit 2
}

test "$#" -eq 1 || usage
FIRMWARE_REPO="$(git rev-parse --show-toplevel)"
HARI_REPO="$(cd "$1" && pwd -P)"
EXPECTED_FIRMWARE_SHA='69c732613ff36afed4fddcd2ec6458f3a3de917c'
EXPECTED_HARI_SHA='3ad9deee99f49099de19b5052264dc02e6273080'

test -x "$HARI_REPO/node_modules/tsx/dist/cli.mjs" || {
  echo 'FAIL Hari dependencies are not installed' >&2
  exit 1
}

BUILD_DIR="$(mktemp -d /tmp/pocket-pairing-preflight.XXXXXX)"
cleanup() { rm -rf "$BUILD_DIR"; }
trap cleanup EXIT

git clone --quiet --shared --no-checkout "$FIRMWARE_REPO" "$BUILD_DIR/firmware"
git -C "$BUILD_DIR/firmware" checkout --quiet --detach "$EXPECTED_FIRMWARE_SHA"
git clone --quiet --shared --no-checkout "$HARI_REPO" "$BUILD_DIR/hari-source"
git -C "$BUILD_DIR/hari-source" checkout --quiet --detach "$EXPECTED_HARI_SHA"
ln -s "$HARI_REPO/node_modules" "$BUILD_DIR/hari-source/node_modules"
cmake -S "$BUILD_DIR/firmware/test" -B "$BUILD_DIR/build" >/dev/null
cmake --build "$BUILD_DIR/build" --target PocketPairingPreflight --parallel >/dev/null

NODE20="${POCKET_PREFLIGHT_NODE:-}"
if test -z "$NODE20"; then
  NODE20="$(command -v node)"
fi
test "$($NODE20 --version)" = 'v20.20.2' || {
  echo 'FAIL set POCKET_PREFLIGHT_NODE to Node v20.20.2' >&2
  exit 1
}

  "$NODE20" "$HARI_REPO/node_modules/tsx/dist/cli.mjs" \
  "$HARI_REPO/scripts/pocket-pairing-cross-preflight.ts" \
  "$BUILD_DIR/build/pocket_pairing/PocketPairingPreflight" \
  "$BUILD_DIR/hari-source"
