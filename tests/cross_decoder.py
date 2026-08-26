#!/usr/bin/env python3
"""Diff this repo's reference decoder against embarch-core's, over identical bytes.

This wire has three implementations that must agree — the firmware encoder, the
Rust decoder in `embarch-study-designer` (which `embarch-core` renders through),
and `scripts/decode_outpost.py`. The firmware encoder is the one both decoders
are fed *from*, so the check available here is decoder-against-decoder, and it is
the only one that catches a decoder that drifted while the encoder stayed put.

**Why this is a script and not a note in a design doc.** The same comparison was
done by hand on 2026-08-25, recorded as agreeing on all 848 rows, and then went
un-rerun across the layout-1 -> layout-2 rework that rewrote *both* decoders. A
one-off human act that has to be remembered is not a check. This one runs with
the rest of the suite.

Inputs are the committed fixtures in the two sibling repos, not a fresh capture:
a fresh `native_sim` run has a different build ID every dirty tree, and the point
is to compare two decoders on **one** set of bytes. Skips loudly — never fails —
when the siblings are not there, since DOC-PROTOCOL §2's sibling layout is a
convention this repo's own test suite should not require.
"""

from __future__ import annotations

import difflib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
SUITE = os.path.dirname(MODULE)

RAW = os.path.join(SUITE, "embarch-core", "tests", "fixtures", "outpost-native-sim.bin")
MANIFEST = os.path.join(
    SUITE, "embarch-core", "tests", "fixtures", "outpost-native-sim-manifest.json"
)
# embarch-core's own renderer wrote this one, from those bytes, through
# embarch-study-designer's decoder: see that repo's
# `outpost_manifest::regenerate_the_ui_trace_fixtures`.
CORE_RENDERED = os.path.join(
    SUITE, "embarch-ui", "tests", "fixtures", "outpost-native-sim-stamped.trace.csv"
)

# The stamps that fixture was generated with. Synthesised, and the generator says
# so — no receiver has ever stamped a real outpost capture. Reproduced here
# rather than read from a file so the two sides cannot silently drift apart.
FIRST_MS = 1_700_000_000_000
SPACING_MS = 20


def main() -> int:
    missing = [p for p in (RAW, MANIFEST, CORE_RENDERED) if not os.path.exists(p)]
    if missing:
        print("SKIP: cross-decoder check needs the sibling repos' committed fixtures:")
        for p in missing:
            print(f"  - {p}")
        return 0

    raw = open(RAW, "rb").read()
    arrival = os.path.join(HERE, "build", "cross_decoder.arrival.csv")
    os.makedirs(os.path.dirname(arrival), exist_ok=True)
    frames = 0
    with open(arrival, "w", encoding="utf-8") as fh:
        fh.write("frame_index,rx_utc_ms,frame_bytes\n")
        for chunk in raw.split(b"\x00"):
            if not chunk:
                continue
            fh.write(f"{frames},{FIRST_MS + frames * SPACING_MS},{len(chunk)}\n")
            frames += 1

    ours = subprocess.run(
        [sys.executable, os.path.join(MODULE, "scripts", "decode_outpost.py"),
         "--manifest", MANIFEST, "--arrival", arrival, RAW],
        capture_output=True, text=True, check=True,
    ).stdout.splitlines()
    theirs = open(CORE_RENDERED, encoding="utf-8").read().splitlines()

    if ours == theirs:
        print(f"PASS: both decoders agree on all {len(theirs) - 1} rows of "
              f"{frames} frames, header line included")
        return 0

    print(f"FAIL: the two decoders disagree ({len(ours)} rows here, "
          f"{len(theirs)} in embarch-core's rendering)")
    shown = 0
    for line in difflib.unified_diff(theirs, ours, "embarch-core", "decode_outpost.py",
                                     lineterm="", n=1):
        print("  " + line)
        shown += 1
        if shown > 40:
            print("  ... (truncated)")
            break
    return 1


if __name__ == "__main__":
    sys.exit(main())
