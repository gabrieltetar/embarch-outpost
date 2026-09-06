#!/usr/bin/env bash
# Every test this repo has. The first leg is host Python and runs anywhere; the
# three after it need ZEPHYR_BASE and WEST, since neither `west` nor the Zephyr
# SDK is reliably on a bare PATH.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$(cd "$HERE/.." && pwd)"

# Deliberately ahead of the west guard below. This leg needs no toolchain, no
# ZEPHYR_BASE and no sibling repos, so putting it first is what makes the host
# half of this repo *always* exercised rather than exercised where a Zephyr
# checkout happens to exist.
echo "=== decoder unit (host Python; no west, no ZEPHYR_BASE, no siblings) ==="
"${PYTHON:-python3}" "$HERE/decoder_unit.py"
echo

WEST="${WEST:?set WEST to a west executable}"
: "${ZEPHYR_BASE:?set ZEPHYR_BASE to a Zephyr checkout}"
BUILD_ROOT="${BUILD_ROOT:-$HERE/build}"

echo "=== unit (ztest, native_sim) ==="
"$WEST" build -p always -b native_sim -d "$BUILD_ROOT/unit" "$HERE/unit" -- \
    -DZEPHYR_EXTRA_MODULES="$MODULE"
"$BUILD_ROOT/unit/zephyr/zephyr.exe"

echo
echo "=== module off: <embarch/outpost.h> still compiles (compile-only) ==="
"$WEST" build -p always -b native_sim -d "$BUILD_ROOT/module_off" "$HERE/module_off" -- \
    -DZEPHYR_EXTRA_MODULES="$MODULE"
echo "PASS: header includes and OUTPOST_EVT compiles with CONFIG_EMBARCH_OUTPOST=n"

echo
echo "=== end-to-end stream (native_sim) ==="
BUILD_DIR="$BUILD_ROOT/native_sim_stream" "$HERE/native_sim_stream/run.sh"

echo
echo "=== cross-decoder (this repo's decoder vs embarch-core's, same bytes) ==="
# Needs neither Zephyr nor west — it compares two host decoders over the
# committed fixtures, and skips loudly if the sibling repos are not present.
"${PYTHON:-python3}" "$HERE/cross_decoder.py"
