#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 /path/to/hari-repository" >&2
  echo "required: POCKET_PREFLIGHT_FIRMWARE_SHA POCKET_PREFLIGHT_HARI_SHA POCKET_PREFLIGHT_HARNESS_SHA POCKET_PREFLIGHT_NODE" >&2
  exit 2
}

test "$#" -eq 1 || usage
FIRMWARE_REPO="$(git rev-parse --show-toplevel)"
HARI_REPO="$(cd "$1" && pwd -P)"
FIRMWARE_SHA="${POCKET_PREFLIGHT_FIRMWARE_SHA:-}"
HARI_SHA="${POCKET_PREFLIGHT_HARI_SHA:-}"
HARNESS_SHA="${POCKET_PREFLIGHT_HARNESS_SHA:-}"
NODE20="${POCKET_PREFLIGHT_NODE:-}"

require_sha() {
  local name="$1" value="$2"
  case "$value" in
    (????????????????????????????????????????)
      case "$value" in (*[!0-9a-f]*) echo "FAIL $name must be 40 lowercase hex characters" >&2; exit 2;; esac
      ;;
    (*) echo "FAIL $name must be 40 lowercase hex characters" >&2; exit 2;;
  esac
}

require_sha POCKET_PREFLIGHT_FIRMWARE_SHA "$FIRMWARE_SHA"
require_sha POCKET_PREFLIGHT_HARI_SHA "$HARI_SHA"
require_sha POCKET_PREFLIGHT_HARNESS_SHA "$HARNESS_SHA"
test -x "$NODE20" || { echo 'FAIL POCKET_PREFLIGHT_NODE must be an executable Node binary' >&2; exit 2; }
test "$($NODE20 --version)" = 'v20.20.2' || { echo 'FAIL POCKET_PREFLIGHT_NODE must be Node v20.20.2' >&2; exit 2; }

NPM="$(dirname "$NODE20")/npm"
test -x "$NPM" || { echo 'FAIL npm is not installed beside POCKET_PREFLIGHT_NODE' >&2; exit 2; }

BUILD_DIR="$(mktemp -d /tmp/pocket-pairing-preflight.XXXXXX)"
cleanup() { rm -rf "$BUILD_DIR"; }
trap cleanup EXIT

git clone --quiet --shared --no-checkout "$FIRMWARE_REPO" "$BUILD_DIR/firmware"
git -C "$BUILD_DIR/firmware" checkout --quiet --detach "$FIRMWARE_SHA"
test "$(git -C "$BUILD_DIR/firmware" rev-parse HEAD)" = "$FIRMWARE_SHA"

git clone --quiet --shared --no-checkout "$HARI_REPO" "$BUILD_DIR/hari-harness"
git -C "$BUILD_DIR/hari-harness" checkout --quiet --detach "$HARNESS_SHA"
test "$(git -C "$BUILD_DIR/hari-harness" rev-parse HEAD)" = "$HARNESS_SHA"
(cd "$BUILD_DIR/hari-harness" && "$NPM" ci --no-audit --no-fund >/dev/null)

if test "$HARI_SHA" = "$HARNESS_SHA"; then
  HARI_SOURCE="$BUILD_DIR/hari-harness"
else
  HARI_SOURCE="$BUILD_DIR/hari-source"
  git clone --quiet --shared --no-checkout "$HARI_REPO" "$HARI_SOURCE"
  git -C "$HARI_SOURCE" checkout --quiet --detach "$HARI_SHA"
  test "$(git -C "$HARI_SOURCE" rev-parse HEAD)" = "$HARI_SHA"
  (cd "$HARI_SOURCE" && "$NPM" ci --no-audit --no-fund >/dev/null)
fi

cmake -S "$BUILD_DIR/firmware/test" -B "$BUILD_DIR/build" >/dev/null
cmake --build "$BUILD_DIR/build" --target PocketPairingPreflight --parallel >/dev/null

"$NODE20" "$BUILD_DIR/hari-harness/node_modules/tsx/dist/cli.mjs" \
  "$BUILD_DIR/hari-harness/scripts/pocket-pairing-cross-preflight.ts" \
  "$BUILD_DIR/build/pocket_pairing/PocketPairingPreflight" \
  "$HARI_SOURCE" \
  "$FIRMWARE_SHA" \
  "$HARI_SHA" \
  "$HARNESS_SHA"
