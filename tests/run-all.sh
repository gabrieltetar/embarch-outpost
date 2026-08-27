#!/usr/bin/env bash
# Every test this repo has. Needs ZEPHYR_BASE and WEST; neither `west` nor the
# Zephyr SDK is reliably on a bare PATH.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$(cd "$HERE/.." && pwd)"
WEST="${WEST:?set WEST to a west executable}"
: "${ZEPHYR_BASE:?set ZEPHYR_BASE to a Zephyr checkout}"
BUILD_ROOT="${BUILD_ROOT:-$HERE/build}"

echo "=== unit (ztest, native_sim) ==="
"$WEST" build -p always -b native_sim -d "$BUILD_ROOT/unit" "$HERE/unit" -- \
    -DZEPHYR_EXTRA_MODULES="$MODULE"
"$BUILD_ROOT/unit/zephyr/zephyr.exe"

echo
echo "=== end-to-end stream (native_sim) ==="
BUILD_DIR="$BUILD_ROOT/native_sim_stream" "$HERE/native_sim_stream/run.sh"

echo
echo "=== cross-decoder (this repo's decoder vs embarch-core's, same bytes) ==="
# Needs neither Zephyr nor west — it compares two host decoders over the
# committed fixtures, and skips loudly if the sibling repos are not present.
"${PYTHON:-python3}" "$HERE/cross_decoder.py"
