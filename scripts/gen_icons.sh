#!/usr/bin/env bash
# Regenerate src/components/icons/generated_icons.h from icons.manifest, using the
# FreeInk SDK icon generator and its vendored Lucide submodule. generated_icons.h is
# a committed build product of these inputs — re-run this after editing the manifest.
#
# Requires: rsvg-convert (librsvg) + Pillow, and the Lucide submodule fetched
# (git submodule update --init in the SDK).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/freeink-sdk"
SVGDIR="$SDK/libs/assets/Icons/lucide/icons"

if [ ! -d "$SVGDIR" ]; then
  echo "Lucide SVGs not found at $SVGDIR" >&2
  echo "Fetch the submodule:  git -C \"$SDK\" submodule update --init libs/assets/Icons/lucide" >&2
  exit 1
fi

python3 "$SDK/libs/assets/Icons/tools/gen_icons.py" \
  --manifest "$ROOT/src/components/icons/icons.manifest" \
  --svgdir   "$SVGDIR" \
  --sizes    16,24,32,40,48 \
  --out      "$ROOT/src/components/icons/generated_icons.h"

echo "Regenerated src/components/icons/generated_icons.h"
