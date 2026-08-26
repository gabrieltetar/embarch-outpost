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
check(header["record_layout_version"] == 2,
      f"header reports record layout {header['record_layout_version']}, not 2")
check("cycles_per_sec" not in header,
      "the header still carries a cycle rate; layout 2 has no DUT clock to describe")
check(not trace["manifest_refused"], "the matching manifest was refused")

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

gaps = [r for r in trace["records"] if r["kind"] == "gap"]
check(any(r["a"] > 0 for r in gaps), "a gap record was emitted but reported zero drops")

# No record carries a timestamp at all any more, and nothing downstream may
# start expecting one back: a `cycles` key reappearing in a decoded row would
# mean layout 1 crept back in somewhere.
check(all("cycles" not in r for r in trace["records"]),
      "a decoded record still carries a `cycles` field")

# Frame indices must be non-decreasing and must cover every frame that
# decoded, because that index is the only thing a host has to join arrival
# stamps onto (design.md §3 decision 18).
frame_indices = [r["frame_index"] for r in trace["records"]]
check(all(b >= a for a, b in zip(frame_indices, frame_indices[1:])),
      "records are not in frame order")
check(len(set(frame_indices)) > 1, "every record landed in one frame")

# This capture went to stdout, not through a receiver, so nothing stamped it.
# An untimed trace is a real answer and must render as one -- an empty column,
# never a fabricated time.
check(all(r["rx_utc_ms"] == "" for r in trace["records"]),
      "a capture nobody stamped came out with arrival times in it")
check(stats["stamped_frames"] == 0,
      f"{stats['stamped_frames']} frames were stamped in a capture with no receiver")

# Every gap record must be the first record of its frame -- that position is
# the whole of what bounds the losses in time now (OUTPOST_KIND_GAP).
by_frame: dict[int, list] = {}
for r in trace["records"]:
    by_frame.setdefault(r["frame_index"], []).append(r)
for r in trace["records"]:
    if r["kind"] == "gap":
        first = by_frame[r["frame_index"]][0]
        check(first is r or first["kind"] == "gap",
              f"a gap record sits mid-frame in frame {r['frame_index']}")
check(all(r["b"] == 0 for r in trace["records"] if r["kind"] == "gap"),
      "a gap record still reports a cycle span in `b`")

# And the arrival join has to work when stamps *are* supplied. Synthesised
# here, and said to be: no outpost byte has crossed a real UART, so there is no
# real set of stamps in this repo to use instead.
arrival_path = raw_path + ".arrival.csv"
with open(arrival_path, "w", encoding="utf-8") as fh:
    fh.write("frame_index,rx_utc_ms\n")
    for i in range(stats["frames"]):
        fh.write(f"{i},{1_700_000_000_000 + i * 20}\n")
stamped = json.loads(subprocess.run(
    [sys.executable, f"{module_dir}/scripts/decode_outpost.py",
     "--manifest", manifest_path, "--arrival", arrival_path, "--json", raw_path],
    capture_output=True, text=True, check=True).stdout)
check(all(r["rx_utc_ms"] != "" for r in stamped["records"]),
      "supplied arrival stamps did not reach every row")
check(all(r["rx_utc_ms"] == 1_700_000_000_000 + r["frame_index"] * 20
          for r in stamped["records"]),
      "an arrival stamp landed on the wrong frame")

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
