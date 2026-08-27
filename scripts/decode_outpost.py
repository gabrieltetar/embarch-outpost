#!/usr/bin/env python3
"""Reference decoder for an outpost stream.

The wire format is specified once, in src/outpost_priv.h. This is the second
of three implementations of it (the firmware encoder and embarch-core's
decoder are the others), and it exists so the native_sim end-to-end test can
assert that real UART bytes decode, without a Core in the loop.

Reads a raw stream on stdin or from a file; writes the decoded records as CSV,
or as JSON with --json. With a manifest (--manifest), IDs are resolved to
names — and a manifest whose build_id disagrees with the stream's header is
**refused**, not applied, which is the whole point of there being a build ID.

Manifests older than schema 2 carry no `devices` or `functions` table, so GPIO
records from a newer firmware decode into structure and render with their
pointers unnamed. That is the designed outcome, not a failure: the record shape
is fixed so kinds can be appended without a layout change.
"""

from __future__ import annotations

import argparse
import binascii
import json
import sys

FRAME_RECORDS = 0x01
FRAME_HEADER = 0x02

KIND_NAMES = {
    0: "thread_switch_in",
    1: "thread_switch_out",
    2: "isr_enter",
    3: "isr_exit",
    4: "idle",
    5: "thread_create",
    6: "thread_name",
    7: "marker",
    8: "gap",
    9: "gpio_dispatch",
    10: "gpio_callback_done",
}

# `a` for these is a port `struct device *` / a callback handler function
# pointer, resolved out of the manifest's own ELF reads.
KIND_THREAD = (0, 1, 5, 6)
KIND_IRQ = (2, 3)
KIND_MARKER = 7
KIND_GPIO_DISPATCH = 9
KIND_GPIO_CALLBACK_DONE = 10

IRQ_UNKNOWN = 0xFFFFFFFF


class Truncated(Exception):
    pass


def cobs_decode(frame: bytes) -> bytes | None:
    out = bytearray()
    i = 0
    n = len(frame)
    while i < n:
        code = frame[i]
        if code == 0:
            return None
        i += 1
        end = i + code - 1
        if end > n:
            return None
        out.extend(frame[i:end])
        i = end
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


def take_varint(buf: bytes, pos: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise Truncated
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if byte & 0x80 == 0:
            return value & 0xFFFFFFFF, pos
        shift += 7
        if shift > 35:
            raise Truncated


def take_string(buf: bytes, pos: int) -> tuple[str, int]:
    length, pos = take_varint(buf, pos)
    if pos + length > len(buf):
        raise Truncated
    return buf[pos:pos + length].decode("utf-8", "replace"), pos + length


def decode_stream(raw: bytes):
    """Yields ('header', dict) and ('records', frame_index, seq, [record...]).

    A frame that fails its CRC or its structure is reported and skipped, never
    partially applied: the stream is designed so a corrupt frame costs exactly
    that frame.
    """
    stats = {"frames": 0, "bad_crc": 0, "bad_cobs": 0, "bad_body": 0, "lost_frames": 0}
    last_seq = None
    frame_index = -1
    for chunk in raw.split(b"\x00"):
        if not chunk:
            continue
        # Consumed before any validity check: a frame that fails its CRC below
        # still burned an index while the receiver was stamping, so skipping
        # one here would shift every arrival stamp after it.
        frame_index += 1
        stats["frames"] += 1
        body = cobs_decode(chunk)
        if body is None or len(body) < 6:
            stats["bad_cobs"] += 1
            continue
        payload, crc_bytes = body[:-4], body[-4:]
        want = int.from_bytes(crc_bytes, "little")
        if binascii.crc32(payload) & 0xFFFFFFFF != want:
            stats["bad_crc"] += 1
            continue

        ftype = payload[0]
        seq = payload[1]
        if last_seq is not None:
            missed = (seq - last_seq - 1) & 0xFF
            stats["lost_frames"] += missed
        last_seq = seq

        try:
            if ftype == FRAME_HEADER:
                pos = 2
                layout = payload[pos]
                flags = payload[pos + 1]
                pos += 2
                cycles_per_sec, pos = take_varint(payload, pos)
                version, pos = take_string(payload, pos)
                build_id, pos = take_string(payload, pos)
                yield "header", {
                    "seq": seq,
                    "record_layout_version": layout,
                    "flags": flags,
                    "cycles_per_sec": cycles_per_sec,
                    "outpost_version": version,
                    "build_id": build_id,
                }
            elif ftype == FRAME_RECORDS:
                pos = 2
                count, pos = take_varint(payload, pos)
                records = []
                for _ in range(count):
                    cycles, pos = take_varint(payload, pos)
                    kind = payload[pos]
                    pos += 1
                    a, pos = take_varint(payload, pos)
                    b, pos = take_varint(payload, pos)
                    records.append({"cycles": cycles, "kind": kind, "a": a, "b": b})
                yield "records", frame_index, seq, records
            else:
                stats["bad_body"] += 1
        except (Truncated, IndexError):
            stats["bad_body"] += 1
    yield "stats", stats


def read_arrivals(path: str) -> dict[int, int]:
    """`frame_index,rx_utc_ms` -> a lookup, keyed by frame index.

    Frame index counts **non-empty delimiter-separated chunks** from the start
    of the capture -- the same thing this decoder counts, and the same thing the
    receiver counted while stamping. A frame that later fails its CRC still
    consumed an index on both sides, which is what keeps the two in step.
    """
    stamps: dict[int, int] = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("frame_index"):
                continue
            parts = line.split(",")
            if len(parts) < 2:
                continue
            try:
                stamps[int(parts[0])] = int(parts[1])
            except ValueError:
                continue
    return stamps


def render(records, frame_index, seq, header, manifest, unwrap_state, stamps):
    """One decoded row per record, with names resolved where the manifest has them."""
    markers = (manifest or {}).get("markers", {})
    threads = (manifest or {}).get("threads", {})
    isrs = (manifest or {}).get("isrs", {})
    isr_args = (manifest or {}).get("isr_args", {})
    devices = (manifest or {}).get("devices", {})
    functions = (manifest or {}).get("functions", {})
    rate = (header or {}).get("cycles_per_sec") or 0
    # The frame's arrival stamp, shared by every record it carried. Records
    # inside a frame are ordered by `cycles` and not spread across the
    # interval: that would be an interpolation nobody measured.
    rx = stamps.get(frame_index, "") if stamps else ""

    rows = []
    for rec in records:
        cycles = rec["cycles"]
        kind = rec["kind"]
        # Absolute 32-bit counts, unwrapped host-side: every record is
        # independently interpretable after a drop, which is the property
        # decision 4 paid four bytes a record for.
        #
        # A wrap is a backwards step of nearly the whole counter, not any
        # backwards step. Gap records are stamped when their losses started
        # rather than when they were reported, so they legitimately go
        # backwards by a little — treating that as a wrap threw every
        # subsequent timestamp forward by 2**32, which is how this rule got
        # written.
        if unwrap_state["last"] is not None and cycles < unwrap_state["last"]:
            if (unwrap_state["last"] - cycles) > (1 << 31):
                unwrap_state["wraps"] += 1
                unwrap_state["last"] = cycles
        else:
            unwrap_state["last"] = cycles
        absolute = unwrap_state["wraps"] * (1 << 32) + cycles

        # An unknown kind renders as itself rather than failing the row: the
        # record shape is fixed so that kinds can be appended, and a stream
        # from a newer firmware stays readable here.
        name = KIND_NAMES.get(kind, f"unknown_{kind}")
        label = ""
        if kind in KIND_THREAD:
            label = threads.get(f"0x{rec['a']:08x}", "")
        elif kind in KIND_IRQ:
            if rec["a"] != IRQ_UNKNOWN:
                label = isrs.get(str(rec["a"]), "")
                # A shared trampoline's own name says nothing about which
                # peripheral fired; the handler it was given does.
                inner = isr_args.get(str(rec["a"]))
                if label and inner:
                    label = f"{label}({inner})"
        elif kind == KIND_MARKER:
            label = markers.get(str(rec["a"]), "")
        elif kind == KIND_GPIO_DISPATCH:
            label = devices.get(f"0x{rec['a']:08x}", "")
        elif kind == KIND_GPIO_CALLBACK_DONE:
            # Thumb function pointers carry bit 0 set; the manifest keys its
            # symbol addresses with it masked off, and the two have to agree or
            # every handler renders unnamed.
            label = functions.get(f"0x{rec['a'] & ~1:08x}", "")

        rows.append({
            "frame_index": frame_index,
            "frame_seq": seq,
            "rx_utc_ms": rx,
            "cycles": absolute,
            # f"{...:.3f}", not round(): embarch-study-designer's renderer
            # formats this column with `{:.3}` and always emits three decimals,
            # so round() disagrees with it on every value with a shorter
            # fraction (1234.5 vs 1234.500). tests/cross_decoder.py diffs the
            # two decoders line by line and that difference is the only thing
            # it ever caught -- which is the check working, not a nuisance.
            "us": f"{absolute * 1_000_000 / rate:.3f}" if rate else "",
            "kind": name,
            "a": rec["a"],
            "b": rec["b"],
            "name": label,
        })
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stream", nargs="?", help="raw stream file; stdin if omitted")
    ap.add_argument("--manifest")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--arrival", help="frame_index,rx_utc_ms CSV from whoever received the "
                                      "bytes. Without it rx_utc_ms is empty, which is a "
                                      "trace that is ordered and untimed on the host's "
                                      "clock -- a real answer, honestly distinguishable "
                                      "from a timed one.")
    ap.add_argument("--allow-build-id-mismatch", action="store_true",
                    help="apply the manifest anyway. Renders a plausible, wrong trace; "
                         "exists only so a mismatch can be inspected.")
    args = ap.parse_args()

    raw = open(args.stream, "rb").read() if args.stream else sys.stdin.buffer.read()
    manifest = json.load(open(args.manifest, encoding="utf-8")) if args.manifest else None
    stamps = read_arrivals(args.arrival) if args.arrival else None

    header = None
    all_rows = []
    stats = {}
    unwrap_state = {"last": None, "wraps": 0}
    refused = False

    for item in decode_stream(raw):
        if item[0] == "header":
            new_header = item[1]
            if header is None:
                header = new_header
                if manifest and manifest.get("build_id") != header["build_id"]:
                    sys.stderr.write(
                        "embarch-outpost: REFUSING to decode against this manifest.\n"
                        f"  manifest build_id: {manifest.get('build_id')!r}\n"
                        f"  firmware build_id: {header['build_id']!r}\n"
                        "  The raw stream is intact; a mismatched manifest would relabel "
                        "every marker and thread and produce a trace that is entirely "
                        "readable and entirely wrong.\n"
                    )
                    if not args.allow_build_id_mismatch:
                        manifest = None
                        refused = True
        elif item[0] == "records":
            all_rows.extend(
                render(item[3], item[1], item[2], header, manifest, unwrap_state, stamps)
            )
        elif item[0] == "stats":
            stats = item[1]

    if header is None:
        sys.stderr.write("embarch-outpost: no header frame in this stream; nothing decodable\n")
        return 2

    if args.json:
        json.dump({"header": header, "stats": stats, "manifest_refused": refused,
                   "records": all_rows}, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        print("frame_index,frame_seq,rx_utc_ms,cycles,us,kind,a,b,name")
        for row in all_rows:
            print(f"{row['frame_index']},{row['frame_seq']},{row['rx_utc_ms']},"
                  f"{row['cycles']},{row['us']},{row['kind']},{row['a']},{row['b']},{row['name']}")
        sys.stderr.write(f"embarch-outpost: {json.dumps(stats)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
