#!/usr/bin/env bash
# Build, run, and decode. Phase C's definition of done, minus the hardware.
#
# Environment (see ../../README.md): WEST, ZEPHYR_BASE. Neither `west` nor the
# Zephyr SDK is on a bare PATH on the bench this was written on, so both are
# passed explicitly rather than assumed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD_DIR:-$HERE/build}"
: "${ZEPHYR_BASE:?set ZEPHYR_BASE to a Zephyr checkout}"
WEST="${WEST:?set WEST to a west executable}"

"$WEST" build -p always -b native_sim -d "$BUILD" "$HERE" -- \
    -DZEPHYR_EXTRA_MODULES="$MODULE"

"$BUILD/zephyr/zephyr.exe" > "$BUILD/outpost.bin" 2>"$BUILD/run.log" || true

echo "captured $(stat -c%s "$BUILD/outpost.bin") bytes"

"${PYTHON:-python3}" "$MODULE/scripts/decode_outpost.py" \
    --manifest "$BUILD/zephyr/outpost-manifest.json" --json \
    "$BUILD/outpost.bin" > "$BUILD/outpost.trace.json"

"${PYTHON:-python3}" "$MODULE/tests/native_sim_stream/assert_stream.py" \
    "$BUILD/outpost.trace.json" "$BUILD/zephyr/outpost-manifest.json" "$BUILD/outpost.bin" "$MODULE"
