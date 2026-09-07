#!/usr/bin/env python3
"""Check that the record-kind and header-flag vocabulary agrees everywhere it
is spelled out, with `src/outpost_priv.h` as the definition.

**Why `outpost_priv.h` is the definition and not a generator's source file.**
It is the producer: the firmware that emits a `kind` byte or a `flags` bit is
built from it, so a name or value it does not have cannot appear on the wire,
and a name or value it does have but nowhere else mirrors is a silent gap
rather than a silent lie. Every other copy is a restatement of a fact this
file states first. Generating the others *from* it was considered and
declined here -- see decisions/wire.md decision 23 -- mainly because one of
the four copies (`embarch-study-designer/src/outpost.rs`) is in a repo this
one may read but not write, so a generator could only ever close three of the
four gaps and would still need this same read-and-diff logic for the fourth.
A single check that reads all four and diffs them against the same reference
covers every copy uniformly, including the one it cannot regenerate.

**What this checks:**

- `scripts/decode_outpost.py`'s `KIND_NAMES` and `FLAG_NAMES` dicts, in this
  repo, imported and diffed directly -- no re-parsing, so this leg cannot
  itself drift from what the decoder actually uses.
- `embarch-study-designer/src/outpost.rs`'s `RecordKind` enum (paired with its
  `as_str()` match arms) and `HeaderFlags` constants, in the sibling repo,
  read as text and pattern-matched. **Read-only, and skipped loudly rather
  than failed** if the sibling is not checked out beside this one -- the same
  convention `cross_decoder.py` uses and decisions/module.md decision 22
  explains: a solo clone of `embarch-outpost` is entitled not to have
  `embarch-study-designer` beside it.

`tests/native_sim_stream/assert_stream.py`'s former fourth copy of
`FLAG_TRACE_SELF` is not re-checked here: it was deleted in favour of
importing `decode_outpost.FLAG_TRACE_SELF`, so there is nothing left to drift
independently -- checking `decode_outpost.py` against the producer already
covers it.
"""

from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
SUITE = os.path.dirname(MODULE)

sys.path.insert(0, os.path.join(MODULE, "scripts"))
import decode_outpost as dec  # noqa: E402

PRIV_HEADER = os.path.join(MODULE, "src", "outpost_priv.h")
# Read-only: this repo may read this sibling file as an input to a check, but
# may never write it (../../embarch-fleet/protocol.md §3, §5 -- it belongs to
# embarch-study-designer).
SIBLING_RS = os.path.join(SUITE, "embarch-study-designer", "src", "outpost.rs")

KIND_RE = re.compile(r"OUTPOST_KIND_(\w+)\s*=\s*(\d+)")
FLAG_RE = re.compile(r"OUTPOST_FLAG_(\w+)\s*=\s*BIT\((\d+)\)")

RS_ENUM_VARIANT_RE = re.compile(r"^\s*(\w+)\s*=\s*(\d+),", re.MULTILINE)
RS_AS_STR_ARM_RE = re.compile(r"Self::(\w+)\s*=>\s*\"([a-z0-9_]+)\"")
RS_FLAG_CONST_RE = re.compile(r"pub const (\w+):\s*u8\s*=\s*1\s*<<\s*(\d+);")


def parse_c_header(text: str) -> tuple[dict[int, str], dict[int, str]]:
    """-> ({kind_value: name}, {flag_bit: name}), names lower_snake_case."""
    kinds = {int(v): k.lower() for k, v in KIND_RE.findall(text)}
    flags = {1 << int(bit): k.lower() for k, bit in FLAG_RE.findall(text)}
    return kinds, flags


def parse_rust_outpost(text: str) -> tuple[dict[int, str], dict[int, str]]:
    """-> ({kind_value: name}, {flag_bit: name}) from RecordKind/HeaderFlags.

    Two passes over `RecordKind`: the enum body gives Variant -> discriminant,
    and the `as_str()` match arms give Variant -> the same lower_snake_case
    string every other copy uses, so the two are joined on the variant name
    rather than trusting either alone.
    """
    kind_block_start = text.index("pub enum RecordKind")
    kind_block_end = text.index("\n}", kind_block_start)
    variant_values = dict(
        (name, int(value))
        for name, value in RS_ENUM_VARIANT_RE.findall(text[kind_block_start:kind_block_end])
    )

    as_str_start = text.index("pub fn as_str")
    as_str_end = text.index("\n    }", as_str_start)
    variant_names = dict(RS_AS_STR_ARM_RE.findall(text[as_str_start:as_str_end]))

    kinds = {
        variant_values[variant]: name
        for variant, name in variant_names.items()
        if variant in variant_values
    }

    flags_block_start = text.index("impl HeaderFlags")
    flags_block_end = text.index("\n}", flags_block_start)
    flags = {
        1 << int(bit): name.lower()
        for name, bit in RS_FLAG_CONST_RE.findall(text[flags_block_start:flags_block_end])
    }
    return kinds, flags


def diff(label: str, reference: dict[int, str], other: dict[int, str], other_label: str,
         failures: list[str]) -> None:
    for value, name in reference.items():
        if value not in other:
            failures.append(f"{label} {value} ({name}) is missing from {other_label}")
        elif other[value] != name:
            failures.append(
                f"{label} {value}: outpost_priv.h says {name!r}, {other_label} says "
                f"{other[value]!r}")
    for value, name in other.items():
        if value not in reference:
            failures.append(
                f"{other_label} has {label} {value} ({name}) that outpost_priv.h does not define")


def main() -> int:
    header_text = open(PRIV_HEADER, encoding="utf-8").read()
    c_kinds, c_flags = parse_c_header(header_text)
    if not c_kinds or not c_flags:
        print(f"FAIL: could not parse any kinds/flags out of {PRIV_HEADER} -- "
              "regex out of step with the header's own syntax")
        return 1

    failures: list[str] = []

    diff("kind", c_kinds, dec.KIND_NAMES, "scripts/decode_outpost.py's KIND_NAMES", failures)
    diff("flag bit", c_flags, dec.FLAG_NAMES, "scripts/decode_outpost.py's FLAG_NAMES", failures)

    if os.path.exists(SIBLING_RS):
        rs_kinds, rs_flags = parse_rust_outpost(open(SIBLING_RS, encoding="utf-8").read())
        diff("kind", c_kinds, rs_kinds, "embarch-study-designer/src/outpost.rs's RecordKind",
             failures)
        diff("flag bit", c_flags, rs_flags,
             "embarch-study-designer/src/outpost.rs's HeaderFlags", failures)
    else:
        print(f"SKIP: embarch-study-designer not checked out beside this repo "
              f"({SIBLING_RS} not found) -- comparing only outpost_priv.h against "
              "scripts/decode_outpost.py")

    if failures:
        print("FAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"PASS: {len(c_kinds)} kinds and {len(c_flags)} flag bits agree between "
          f"outpost_priv.h, decode_outpost.py"
          + (", and outpost.rs" if os.path.exists(SIBLING_RS) else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
