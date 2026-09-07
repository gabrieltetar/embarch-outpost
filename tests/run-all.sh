#!/usr/bin/env bash
# Every test this repo has. The first two legs are host Python and run
# anywhere; the three after them need ZEPHYR_BASE and WEST, since neither
# `west` nor the Zephyr SDK is reliably on a bare PATH.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$(cd "$HERE/.." && pwd)"

CROSS_DECODER_LOG="$(mktemp)"
CROSS_DECODER_RESULT="ran"
SUMMARY_PRINTED="no"

# The skip note has to survive an exit that never reaches the summary block,
# because the commonest such exit is the one this file's whole ordering fix is
# about: a bare checkout with no WEST set runs both host legs and then stops at
# the guard below, forty lines above the summary. Printing the note from the
# EXIT trap instead of only from the summary is what makes "a run that checked
# nothing against the sibling fixtures says so at the end" true on *every* exit
# path rather than only on a full toolchain run.
cross_decoder_note() {
    rm -f "$CROSS_DECODER_LOG"
    if [[ "$SUMMARY_PRINTED" == "no" && "$CROSS_DECODER_RESULT" != "ran" ]]; then
        echo
        echo "NOTE: cross-decoder was skipped, not passed — this run checked nothing"
        echo "against embarch-core/embarch-ui's committed fixtures. See decisions/module.md"
        echo "decision 22 for why that stays a skip rather than a failure."
    fi
}
trap cross_decoder_note EXIT

# Both of the next two legs are deliberately ahead of the west guard below.
# Neither needs a toolchain, ZEPHYR_BASE or (for the first) sibling repos, so
# putting them first is what makes the host half of this repo *always*
# exercised, rather than exercised only where a Zephyr checkout happens to
# exist. This was itself the bug decisions/module.md decision 22 records: the
# cross-decoder leg used to sit *after* the guard, so a bare checkout with no
# WEST set never reached it at all, silently, and README.md's claim that "only
# the three Zephyr legs need a toolchain" was false on this file's own
# ordering.
echo "=== decoder unit (host Python; no west, no ZEPHYR_BASE, no siblings) ==="
"${PYTHON:-python3}" "$HERE/decoder_unit.py"
echo

echo "=== cross-decoder (this repo's decoder vs embarch-core's, same bytes) ==="
# Needs neither Zephyr nor west — it compares two host decoders over the
# committed fixtures, and skips loudly (exit 0) if the sibling repos are not
# present. See decisions/module.md decision 22 for why a skip here stays a
# skip rather than becoming a failure, and the final summary below for how a
# skipped run is still visible in the exit summary rather than only in a line
# of mid-stream stdout.
"${PYTHON:-python3}" "$HERE/cross_decoder.py" | tee "$CROSS_DECODER_LOG"
if grep -q '^SKIP:' "$CROSS_DECODER_LOG"; then
    CROSS_DECODER_RESULT="SKIPPED (sibling-repo fixtures not present)"
else
    CROSS_DECODER_RESULT="ran"
fi
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
SUMMARY_PRINTED="yes"
echo "=== summary ==="
echo "decoder unit:    ran"
echo "cross-decoder:   $CROSS_DECODER_RESULT"
echo "unit (ztest):    ran"
echo "module off:      ran"
echo "e2e stream:      ran"
if [[ "$CROSS_DECODER_RESULT" != "ran" ]]; then
    echo
    echo "NOTE: cross-decoder was skipped, not passed — this run checked nothing"
    echo "against embarch-core/embarch-ui's committed fixtures. See decisions/module.md"
    echo "decision 22 for why that stays a skip rather than a failure."
fi
