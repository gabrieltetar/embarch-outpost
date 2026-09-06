#!/usr/bin/env python3
"""Unit tests for `scripts/decode_outpost.py`, over synthesised bytes.

**Why this exists at all.** This repo's other three checks need `west` and a
Zephyr checkout, and `cross_decoder.py` needs two sibling repos' committed
fixtures and skips loudly without them. Net, before this file: the reference
decoder for a wire with "three implementations that must agree"
(`../interfaces/wire.md`, in embarch-doc) had **no test guaranteed to execute**.
`cross_decoder.py`'s own opening argument — a check nobody is forced to run is
not a check — applied to itself.

So the hard constraint on this file is what it may depend on: **stdlib only, no
pytest, no `west`, no `ZEPHYR_BASE`, no sibling repos, no fixtures.** It builds
every byte it decodes. `run-all.sh` runs it first, *before* the west guard, so
the host half of this repo is always exercised.

What it covers is not "the decoder" in general — it is the rules that already
have scar tissue written beside them in `decode_outpost.py`, because those are
the ones a rewrite silently undoes:

- COBS round-trip, including the 0xFF-run block that carries no trailing zero.
- A bad CRC costs **exactly one frame** and still **consumes a `frame_index`**,
  since the receiver burned one stamping it.
- A truncated batch counts `bad_body` and yields nothing from that frame.
- An unknown kind renders as `unknown_N` rather than failing the row.
- A **small** backwards step (a gap record, stamped when its losses started)
  is **not** a wrap; a step of nearly the whole counter is.
- `us` is `f"{...:.3f}"` — three decimals always — not `round()`.

The last two are the ones with a history: the naive `cycles < last` threw every
subsequent timestamp forward by 2**32, and `round()` disagrees with
embarch-study-designer's renderer on every value whose fraction is shorter than
three digits. Both are asserted here in the form that fails if reverted.
"""

from __future__ import annotations

import binascii
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(MODULE, "scripts"))

import decode_outpost as dec  # noqa: E402


# --- byte builders -------------------------------------------------------
#
# Deliberately written out here rather than imported from the decoder: a test
# that encodes with the code under test's own inverse asserts only that the
# function is self-consistent, which is the one thing a rewrite keeps.

def cobs_encode(data: bytes) -> bytes:
    """Standard COBS. Emits a 0xFF code, and no implicit zero, per 254-byte run."""
    out = bytearray(b"\x00")
    code_at = 0
    code = 1
    for byte in data:
        if byte != 0:
            out.append(byte)
            code += 1
            if code == 0xFF:
                out[code_at] = code
                code_at = len(out)
                out.append(0)
                code = 1
        else:
            out[code_at] = code
            code_at = len(out)
            out.append(0)
            code = 1
    out[code_at] = code
    return bytes(out)


def varint(value: int) -> bytes:
    out = bytearray()
    value &= 0xFFFFFFFF
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def wire_string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return varint(len(raw)) + raw


def frame(body: bytes, *, break_crc: bool = False) -> bytes:
    """body || crc32(body) LE, COBS'd, zero-terminated. `break_crc` flips one CRC bit."""
    crc = binascii.crc32(body) & 0xFFFFFFFF
    if break_crc:
        crc ^= 0x01
    return cobs_encode(body + crc.to_bytes(4, "little")) + b"\x00"


def header_frame(seq: int = 0, *, layout: int = 3, flags: int = 0,
                 cycles_per_sec: int = 1_000_000, version: str = "0.1.0",
                 build_id: str = "test-build") -> bytes:
    body = (bytes([dec.FRAME_HEADER, seq, layout, flags])
            + varint(cycles_per_sec) + wire_string(version) + wire_string(build_id))
    return frame(body)


def records_frame(seq: int, records, *, count: int | None = None,
                  break_crc: bool = False) -> bytes:
    """`count` overrides the declared record count, which is how a batch is truncated."""
    body = bytes([dec.FRAME_RECORDS, seq])
    body += varint(len(records) if count is None else count)
    for cycles, kind, a, b in records:
        body += varint(cycles) + bytes([kind]) + varint(a) + varint(b)
    return frame(body, break_crc=break_crc)


def decode(raw: bytes):
    """Runs the decoder end to end the way `main()` does. -> (header, rows, stats)."""
    header = None
    rows = []
    stats = {}
    unwrap = {"last": None, "wraps": 0}
    for item in dec.decode_stream(raw):
        if item[0] == "header":
            if header is None:
                header = item[1]
        elif item[0] == "records":
            rows.extend(dec.render(item[3], item[1], item[2], header, None, unwrap, None))
        elif item[0] == "stats":
            stats = item[1]
    return header, rows, stats


# --- tests ---------------------------------------------------------------

class TestCobs(unittest.TestCase):
    def test_round_trip(self):
        for name, payload in [
            ("empty", b""),
            ("one byte", b"\x01"),
            ("no zeros", bytes(range(1, 200))),
            ("leading zero", b"\x00\x01\x02"),
            ("trailing zero", b"\x01\x02\x00"),
            ("all zeros", b"\x00" * 5),
            ("mixed", b"\x11\x00\x00\x22\x33\x00"),
            ("253 nonzero", b"\xaa" * 253),
            ("254 nonzero", b"\xaa" * 254),
            ("255 nonzero", b"\xaa" * 255),
            ("two full runs", b"\xaa" * 510),
            ("run then zero", b"\xaa" * 254 + b"\x00" + b"\xbb"),
        ]:
            with self.subTest(name):
                encoded = cobs_encode(payload)
                self.assertNotIn(0, encoded, "a COBS frame carries no zero byte")
                self.assertEqual(dec.cobs_decode(encoded), payload)

    def test_ff_run_emits_no_implicit_zero(self):
        # The case the `code != 0xFF` guard exists for: a full 254-byte run's
        # block is not zero-terminated, so a decoder that appends unconditionally
        # inserts a zero that was never on the wire.
        encoded = cobs_encode(b"\xaa" * 254)
        self.assertEqual(encoded[0], 0xFF)
        self.assertEqual(dec.cobs_decode(encoded), b"\xaa" * 254)

    def test_malformed_is_rejected_not_guessed(self):
        self.assertIsNone(dec.cobs_decode(b"\x00\x01"), "a zero code byte is invalid")
        self.assertIsNone(dec.cobs_decode(b"\x05\x01"), "a code overrunning the frame")


class TestFrameAccounting(unittest.TestCase):
    def test_bad_crc_costs_one_frame_and_still_consumes_an_index(self):
        # Four frames. The third fails its CRC. The fourth must still be
        # frame_index 3 -- the receiver burned an index stamping it, so skipping
        # one here would shift every arrival stamp after it.
        raw = (header_frame(seq=0)
               + records_frame(1, [(1000, 0, 0x2000_0000, 0)])
               + records_frame(2, [(2000, 0, 0x2000_0000, 0)], break_crc=True)
               + records_frame(3, [(3000, 0, 0x2000_0000, 0)]))
        header, rows, stats = decode(raw)

        self.assertIsNotNone(header)
        self.assertEqual(stats["frames"], 4)
        self.assertEqual(stats["bad_crc"], 1)
        self.assertEqual(stats["bad_cobs"], 0)
        self.assertEqual(stats["bad_body"], 0)
        self.assertEqual([r["frame_index"] for r in rows], [1, 3])
        self.assertEqual([r["cycles"] for r in rows], [1000, 3000])

    def test_bad_cobs_is_counted_separately(self):
        raw = header_frame(seq=0) + b"\x05\x01\x00"
        _, rows, stats = decode(raw)
        self.assertEqual(stats["frames"], 2)
        self.assertEqual(stats["bad_cobs"], 1)
        self.assertEqual(stats["bad_crc"], 0)
        self.assertEqual(rows, [])

    def test_lost_frames_counts_the_seq_gap(self):
        raw = (header_frame(seq=0)
               + records_frame(1, [(10, 0, 1, 0)])
               + records_frame(5, [(20, 0, 1, 0)]))
        _, _, stats = decode(raw)
        self.assertEqual(stats["lost_frames"], 3)

    def test_truncated_batch_counts_bad_body(self):
        # Declares three records, carries one. The frame is structurally short,
        # not corrupt, so its CRC is good and only the body parse fails.
        raw = (header_frame(seq=0)
               + records_frame(1, [(1000, 0, 1, 0)], count=3)
               + records_frame(2, [(2000, 0, 1, 0)]))
        _, rows, stats = decode(raw)
        self.assertEqual(stats["bad_body"], 1)
        self.assertEqual(stats["bad_crc"], 0)
        self.assertEqual([r["frame_index"] for r in rows], [2],
                         "a truncated batch is skipped whole, never partially applied")


class TestKindRendering(unittest.TestCase):
    def test_known_kinds_render_by_name(self):
        raw = header_frame(seq=0) + records_frame(1, [
            (10, 0, 0x2000_0000, 0),
            (20, 2, 17, 0),
            (30, 7, 5, 99),
            (40, 8, 12, 0),
        ])
        _, rows, _ = decode(raw)
        self.assertEqual([r["kind"] for r in rows],
                         ["thread_switch_in", "isr_enter", "marker", "gap"])

    def test_unknown_kind_renders_as_itself(self):
        # The record shape is fixed so kinds can be appended; a stream from a
        # newer firmware stays readable rather than failing the row.
        raw = header_frame(seq=0) + records_frame(1, [(10, 200, 7, 8)])
        _, rows, _ = decode(raw)
        self.assertEqual(rows[0]["kind"], "unknown_200")
        self.assertEqual(rows[0]["a"], 7)
        self.assertEqual(rows[0]["b"], 8)
        self.assertEqual(rows[0]["name"], "")

    def test_no_manifest_leaves_names_empty_and_rows_intact(self):
        raw = header_frame(seq=0) + records_frame(1, [(10, 0, 0x2000_0000, 0)])
        _, rows, _ = decode(raw)
        self.assertEqual(rows[0]["name"], "")
        self.assertEqual(rows[0]["cycles"], 10)


class TestWrapVersusGap(unittest.TestCase):
    """The rule at `decode_outpost.py`'s unwrap block, in both directions.

    Asserting only the wrap half passes a naive `cycles < last`; asserting only
    the gap half passes a decoder that never unwraps at all. Both, or neither is
    a test of the rule.
    """

    def test_small_backwards_step_from_a_gap_is_not_a_wrap(self):
        # A gap record is stamped when its losses *started*, not when they were
        # reported, so it legitimately goes backwards by a little. Treating that
        # as a wrap threw every subsequent timestamp forward by 2**32.
        raw = header_frame(seq=0) + records_frame(1, [
            (1000, 0, 1, 0),
            (5000, 1, 1, 0),
            (4990, 8, 3, 0),   # gap: 10 cycles backwards
            (6000, 0, 1, 0),
        ])
        _, rows, _ = decode(raw)
        self.assertEqual(rows[2]["kind"], "gap")
        self.assertEqual([r["cycles"] for r in rows], [1000, 5000, 4990, 6000])
        for row in rows:
            self.assertLess(row["cycles"], 1 << 32,
                            "no record here crossed a wrap; none may be shifted by 2**32")

    def test_a_real_wrap_does_unwrap(self):
        raw = header_frame(seq=0) + records_frame(1, [
            (0xFFFF_FF00, 0, 1, 0),
            (0x0000_0100, 0, 1, 0),   # backwards by nearly the whole counter
            (0x0000_0200, 0, 1, 0),
        ])
        _, rows, _ = decode(raw)
        self.assertEqual([r["cycles"] for r in rows],
                         [0xFFFF_FF00, (1 << 32) + 0x100, (1 << 32) + 0x200])

    def test_a_gap_after_a_wrap_still_does_not_re_wrap(self):
        raw = header_frame(seq=0) + records_frame(1, [
            (0xFFFF_FF00, 0, 1, 0),
            (0x0000_0100, 0, 1, 0),
            (0x0000_00F0, 8, 4, 0),   # gap, 16 cycles backwards, past the wrap
            (0x0000_0300, 0, 1, 0),
        ])
        _, rows, _ = decode(raw)
        self.assertEqual([r["cycles"] for r in rows],
                         [0xFFFF_FF00, (1 << 32) + 0x100,
                          (1 << 32) + 0xF0, (1 << 32) + 0x300])


class TestUsFormatting(unittest.TestCase):
    """`us` is a **string** of exactly three decimals, not a rounded float.

    embarch-study-designer's renderer formats this column with `{:.3}` and always
    emits three decimals, so `round()` disagrees with it on every value whose
    fraction is shorter than that -- 1234.5 against 1234.500. That is the only
    difference `cross_decoder.py` has ever caught, and it caught it twice.
    """

    def test_a_whole_number_still_carries_three_decimals(self):
        raw = header_frame(seq=0, cycles_per_sec=1_000_000) + records_frame(
            1, [(1234, 0, 1, 0)])
        _, rows, _ = decode(raw)
        self.assertIsInstance(rows[0]["us"], str)
        self.assertEqual(rows[0]["us"], "1234.000")

    def test_a_half_value_pads_rather_than_truncating(self):
        raw = header_frame(seq=0, cycles_per_sec=2_000_000) + records_frame(
            1, [(2469, 0, 1, 0)])
        _, rows, _ = decode(raw)
        self.assertEqual(rows[0]["us"], "1234.500")

    def test_a_long_fraction_is_cut_to_three(self):
        raw = header_frame(seq=0, cycles_per_sec=3) + records_frame(1, [(1, 0, 1, 0)])
        _, rows, _ = decode(raw)
        self.assertEqual(rows[0]["us"], "333333.333")

    def test_every_us_is_a_string_with_exactly_three_decimals(self):
        raw = header_frame(seq=0, cycles_per_sec=1_000_000) + records_frame(1, [
            (0, 0, 1, 0), (1, 0, 1, 0), (500, 0, 1, 0), (1_000_000, 0, 1, 0),
        ])
        _, rows, _ = decode(raw)
        for row in rows:
            with self.subTest(cycles=row["cycles"]):
                self.assertIsInstance(row["us"], str)
                self.assertRegex(row["us"], r"^\d+\.\d{3}$")

    def test_no_rate_leaves_us_empty_rather_than_guessing(self):
        raw = header_frame(seq=0, cycles_per_sec=0) + records_frame(1, [(10, 0, 1, 0)])
        _, rows, _ = decode(raw)
        self.assertEqual(rows[0]["us"], "")


class TestHeader(unittest.TestCase):
    def test_header_fields_decode(self):
        raw = header_frame(seq=0, layout=3, flags=0x2A, cycles_per_sec=128_000_000,
                           version="1.2.3", build_id="abc123-dirty")
        header, _, _ = decode(raw)
        self.assertEqual(header["record_layout_version"], 3)
        self.assertEqual(header["flags"], 0x2A)
        self.assertEqual(header["cycles_per_sec"], 128_000_000)
        self.assertEqual(header["outpost_version"], "1.2.3")
        self.assertEqual(header["build_id"], "abc123-dirty")

    def test_an_unknown_frame_type_counts_bad_body(self):
        _, rows, stats = decode(header_frame(seq=0) + frame(b"\x7f\x01payload"))
        self.assertEqual(stats["bad_body"], 1)
        self.assertEqual(rows, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
