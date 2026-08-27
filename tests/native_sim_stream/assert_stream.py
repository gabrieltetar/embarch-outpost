#!/usr/bin/env python3
"""Assertions over a captured native_sim stream. Fails loudly, prints why."""
from __future__ import annotations

import json
import subprocess
import sys
from collections import Counter

trace_path, manifest_path, raw_path, module_dir = sys.argv[1:5]
trace = json.load(open(trace_path, encoding="utf-8"))
manifest = json.load(open(manifest_path, encoding="utf-8"))

failures: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        failures.append(msg)


header = trace["header"]
stats = trace["stats"]
kinds = Counter(r["kind"] for r in trace["records"])

check(header["record_layout_version"] == manifest["record_layout_version"],
      f"layout version {header['record_layout_version']} != manifest "
      f"{manifest['record_layout_version']}")
check(header["build_id"] == manifest["build_id"],
      f"stream build_id {header['build_id']!r} != manifest {manifest['build_id']!r}")
check(header["record_layout_version"] == 3,
      f"header reports record layout {header['record_layout_version']}, not 3")
check(header["cycles_per_sec"] > 0,
      f"header reports cycles_per_sec={header['cycles_per_sec']}")
check(not trace["manifest_refused"], "the matching manifest was refused")

# Frame indices must be non-decreasing and must cover more than one frame,
# because that index is the only thing a host has to join arrival stamps onto
# (design.md §3 decision 18). Kept from the layout-2 test suite: it was written
# alongside the host-stamp columns and has nothing to do with what a record
# contains, so it survives layout 2's withdrawal.
frame_indices = [r["frame_index"] for r in trace["records"]]
check(all(b >= a for a, b in zip(frame_indices, frame_indices[1:])),
      "records are not in frame order")
check(len(set(frame_indices)) > 1, "every record landed in one frame")

# This capture went to stdout, not through a receiver, so nothing stamped it.
# An untimed trace is a real answer and must render as one -- an empty column,
# never a fabricated time. Layout 3 keeps the DUT's own clock as well, so this
# is now a check that the two clocks stay separate rather than that there is
# only one.
check(all(r["rx_utc_ms"] == "" for r in trace["records"]),
      "a capture nobody stamped came out with arrival times in it")

check(stats["bad_crc"] == 0, f"{stats['bad_crc']} frames failed their CRC")
check(stats["bad_cobs"] == 0, f"{stats['bad_cobs']} frames failed COBS decoding")
check(stats["bad_body"] == 0, f"{stats['bad_body']} frames had an undecodable body")
check(stats["lost_frames"] == 0, f"{stats['lost_frames']} frames lost (seq gap)")
check(stats["frames"] > 1, "only one frame in the whole stream")

for kind in ("thread_switch_in", "thread_switch_out", "isr_enter", "isr_exit",
             "idle", "marker", "gap"):
    check(kinds[kind] > 0, f"no {kind} records in the stream")

named_threads = {r["name"] for r in trace["records"]
                 if r["kind"].startswith("thread_") and r["name"]}
check("outpost_ping" in named_threads and "outpost_pong" in named_threads,
      f"manifest did not resolve the test's K_THREAD_DEFINE threads: {sorted(named_threads)}")

named_markers = {r["name"] for r in trace["records"] if r["kind"] == "marker" and r["name"]}
check({"WORK_BEGIN", "WORK_END", "BURST"} <= named_markers,
      f"manifest did not resolve every registered marker: {sorted(named_markers)}")

check(sorted(manifest["markers"].values()) == ["BURST", "WORK_BEGIN", "WORK_END"],
      f"manifest markers wrong: {manifest['markers']}")

# ---- self-exclusion (design.md §3 decision 19) -----------------------------
#
# CONFIG_EMBARCH_OUTPOST_TRACE_SELF defaults n, and this test does not set it,
# so the header must SAY the trace is self-excluded and the trace must actually
# be. Both halves matter: a build that excluded itself without setting the flag
# would hand a host a silently incomplete timeline, and a build that set the
# flag without excluding anything would be the same lie the other way round.
FLAG_TRACE_SELF = 1 << 7
check(header["flags"] & FLAG_TRACE_SELF == 0,
      f"header flags 0x{header['flags']:02x} claim the outpost traces itself, but "
      "CONFIG_EMBARCH_OUTPOST_TRACE_SELF is not set in this test's prj.conf")

# The drain thread names itself at runtime, so the manifest resolves it by
# exact-address symbol match and its switch records would be plainly visible.
drain_ptrs = {r["a"] for r in trace["records"] if r["kind"] == "thread_create"
              and r["name"] in ("outpost_drain_thread", "outpost")}
switch_ptrs = {r["a"] for r in trace["records"]
               if r["kind"] in ("thread_switch_in", "thread_switch_out")}
check(bool(drain_ptrs),
      "no thread_create record identifies the outpost's own drain thread, so this check "
      "cannot tell whether it was excluded — create/name records are deliberately NOT "
      "self-excluded precisely so it can")
check(not (drain_ptrs & switch_ptrs),
      f"the outpost's own drain thread {sorted(hex(p) for p in drain_ptrs & switch_ptrs)} "
      "has context-switch records in a self-excluded trace")

gaps = [r for r in trace["records"] if r["kind"] == "gap"]
check(any(r["a"] > 0 for r in gaps), "a gap record was emitted but reported zero drops")

# Cycles must be non-decreasing once unwrapped -- the ring publishes in
# reservation order, so anything else means it reordered records. Gap records
# are the documented exception: they are stamped when their losses started and
# emitted when there was room to report them.
cycles = [r["cycles"] for r in trace["records"] if r["kind"] != "gap"]
check(all(b >= a for a, b in zip(cycles, cycles[1:])),
      "non-gap records are not in non-decreasing cycle order")

# And the unwrap must not have run away: this test lasts under a second at
# 1 MHz, so nothing should be anywhere near a 2**32 wrap.
check(max(cycles) < (1 << 32),
      f"unwrapped cycles reached {max(cycles)}, which means a wrap was inferred "
      "from a backwards step that was not one")

# And a manifest that does not belong to this build must be refused, not
# applied. This is the failure mode the whole build-ID mechanism exists for.
bogus = dict(manifest)
bogus["build_id"] = "not-this-build"
bogus_path = manifest_path + ".bogus"
json.dump(bogus, open(bogus_path, "w", encoding="utf-8"))
out = subprocess.run(
    [sys.executable, f"{module_dir}/scripts/decode_outpost.py",
     "--manifest", bogus_path, "--json", raw_path],
    capture_output=True, text=True, check=True)
refused = json.loads(out.stdout)
check(refused["manifest_refused"], "a mismatched manifest was NOT refused")
check(all(r["name"] == "" for r in refused["records"]),
      "a refused manifest still labelled records")

if failures:
    print("FAIL:")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)

print(f"PASS: {stats['frames']} frames, {len(trace['records'])} records, "
      f"kinds={dict(kinds)}, "
      f"{len(manifest['threads'])} threads / {len(manifest['isrs'])} ISRs in the manifest")
