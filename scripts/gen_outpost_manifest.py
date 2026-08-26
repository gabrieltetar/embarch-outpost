#!/usr/bin/env python3
"""Emit outpost-manifest.json from a linked Zephyr ELF.

../embarch-doc/embarch-outpost/design.md §3 decisions 6, 7, 8, 9 and §5.4.

Everything this reads is a fact recorded in the image by the build that
produced it. Nothing here matches heuristically, and nothing is guessed:

  markers  the `outpost_marker_table` the application's own OUTPOST_MARKERS(X)
           declaration generated, walked to its {0, NULL} sentinel
  threads  symbols named `_k_thread_obj_<name>`, which is literally what
           K_THREAD_DEFINE(name, ...) expands its `struct k_thread` to, plus
           the kernel's own `z_main_thread` and `z_idle_threads`
  isrs     `_sw_isr_table[]`, "an array of these structures indexed by the
           irq line" (include/zephyr/sw_isr_table.h), resolved against the
           symbol table at exactly the index the firmware reports -- and its
           `arg` too, because on this suite's first real target most entries
           dispatch through a shared `nrfx_isr` trampoline whose argument is
           the driver handler that actually runs

Where a fact is not available, this emits nothing for it and records why in
`notes`. It never fills a gap with a plausible answer — a trace that is
entirely readable and entirely wrong is the outcome the whole manifest
mechanism exists to prevent.
"""

from __future__ import annotations

import argparse
import binascii
import json
import struct
import sys

try:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
except ImportError:  # pragma: no cover - Zephyr's own build requires pyelftools
    sys.stderr.write(
        "embarch-outpost: pyelftools is required to generate outpost-manifest.json "
        "(Zephyr's own gen_isr_tables.py needs it too, so this normally cannot happen)\n"
    )
    raise

MANIFEST_SCHEMA = 1


class Elf:
    """Just enough ELF to read symbols and the bytes behind them."""

    def __init__(self, path: str) -> None:
        self._fh = open(path, "rb")
        self.elf = ELFFile(self._fh)
        self.little = self.elf.little_endian
        self.ptr_bytes = self.elf.elfclass // 8
        self.arch = self.elf.get_machine_arch()
        self.symbols: dict[str, tuple[int, int]] = {}
        # addr -> name, for resolving a function pointer back to a symbol.
        self.func_by_addr: dict[int, str] = {}
        for section in self.elf.iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue
            for sym in section.iter_symbols():
                if not sym.name:
                    continue
                info = sym["st_info"]
                addr = sym["st_value"]
                self.symbols.setdefault(sym.name, (addr, sym["st_size"]))
                if info["type"] == "STT_FUNC":
                    # Thumb function symbols carry bit 0 set; the pointer
                    # stored in the table does too, so mask both consistently.
                    self.func_by_addr.setdefault(addr & ~1, sym.name)

    def close(self) -> None:
        self._fh.close()

    def read(self, addr: int, size: int) -> bytes | None:
        """Bytes at a virtual address, from whichever segment contains them."""
        if size <= 0:
            return b""
        for seg in self.elf.iter_segments():
            if seg["p_type"] != "PT_LOAD":
                continue
            start = seg["p_vaddr"]
            filesz = seg["p_filesz"]
            if start <= addr and addr + size <= start + filesz:
                data = seg.data()
                off = addr - start
                return data[off:off + size]
        return None

    def word(self, addr: int) -> int | None:
        raw = self.read(addr, self.ptr_bytes)
        if raw is None:
            return None
        fmt = ("<" if self.little else ">") + ("I" if self.ptr_bytes == 4 else "Q")
        return struct.unpack(fmt, raw)[0]

    def u32(self, addr: int) -> int | None:
        raw = self.read(addr, 4)
        if raw is None:
            return None
        return struct.unpack(("<" if self.little else ">") + "I", raw)[0]

    def cstring(self, addr: int, limit: int = 128) -> str | None:
        raw = self.read(addr, limit)
        if raw is None:
            # A string that runs off the end of its segment: read what fits.
            for shrink in (64, 32, 16, 8):
                raw = self.read(addr, shrink)
                if raw is not None:
                    break
        if raw is None:
            return None
        end = raw.find(b"\x00")
        if end < 0:
            return None
        try:
            return raw[:end].decode("utf-8")
        except UnicodeDecodeError:
            return None


def read_markers(elf: Elf, notes: list[str]) -> dict[str, str]:
    """The application's marker IDs and names, from `outpost_marker_table`."""
    table = elf.symbols.get("outpost_marker_table")
    if table is None:
        notes.append(
            "no outpost_marker_table in the ELF: this image registers no markers, "
            "or was linked without the outpost"
        )
        return {}

    base, size = table
    # struct { uint32_t id; const char *name; } -- one pointer's worth of
    # alignment padding after the u32 on a 64-bit target.
    row = 8 if elf.ptr_bytes == 4 else 16
    rows = (size // row) if size else 0
    if rows == 0:
        notes.append(
            "outpost_marker_table has no size in the symbol table; markers are omitted "
            "rather than read at a guessed length"
        )
        return {}

    out: dict[str, str] = {}
    for i in range(rows):
        addr = base + i * row
        ident = elf.u32(addr)
        name_ptr = elf.word(addr + (row - elf.ptr_bytes))
        if ident is None or name_ptr is None:
            notes.append(f"marker row {i} was not readable from the ELF")
            break
        if name_ptr == 0:
            break  # the sentinel row
        name = elf.cstring(name_ptr)
        if name is None:
            notes.append(f"marker {ident}'s name string was not readable from the ELF")
            continue
        out[str(ident)] = name
    return out


def read_threads(elf: Elf, notes: list[str]) -> dict[str, str]:
    """K_THREAD_DEFINE(name, ...) expands to `struct k_thread _k_thread_obj_name`."""
    prefix = "_k_thread_obj_"
    out: dict[str, str] = {}
    for sym, (addr, _size) in elf.symbols.items():
        if sym.startswith(prefix):
            out[f"0x{addr:08x}"] = sym[len(prefix):]
    if not out:
        notes.append("no _k_thread_obj_* symbols found")

    # The kernel's own two, which no K_THREAD_DEFINE produces and which
    # between them account for most of a quiet trace.
    for sym, label in (("z_main_thread", "main"), ("z_idle_threads", "idle")):
        entry = elf.symbols.get(sym)
        if entry is not None:
            out[f"0x{entry[0]:08x}"] = label
    idle = elf.symbols.get("z_idle_threads")
    if idle is not None and idle[1] > 0:
        notes.append(
            "z_idle_threads is an array, one per CPU; only the first is named here, "
            "because its stride is sizeof(struct k_thread) and that is not something "
            "the symbol table states"
        )
    notes.append(
        "threads created with k_thread_create() into a plain `static struct k_thread` "
        "have no distinguishing symbol and are absent here by design; they render as "
        "raw pointers rather than as a guess"
    )
    return out


def read_isrs(elf: Elf, notes: list[str]) -> tuple[dict[str, str], dict[str, str]]:
    """`_sw_isr_table[]` is indexed by irq line; each entry is { arg, isr }."""
    table = elf.symbols.get("_sw_isr_table")
    if table is None:
        notes.append(
            "no _sw_isr_table in the ELF (CONFIG_GEN_SW_ISR_TABLE=n, or an "
            "architecture that does not use one): ISR records carry their raw "
            "vector number and resolve to no name"
        )
        return {}, {}

    base, size = table
    entry = elf.ptr_bytes * 2
    if size == 0 or entry == 0:
        notes.append("_sw_isr_table has no size in the symbol table")
        return {}, {}

    out: dict[str, str] = {}
    args: dict[str, str] = {}
    unresolved = 0
    for idx in range(size // entry):
        arg_ptr = elf.word(base + idx * entry)
        isr_ptr = elf.word(base + idx * entry + elf.ptr_bytes)
        if isr_ptr is None:
            break
        if isr_ptr == 0:
            continue
        name = elf.func_by_addr.get(isr_ptr & ~1)
        if name is None:
            unresolved += 1
            continue
        if name == "z_irq_spurious":
            # An unclaimed line, not a handler. Emitting it would name every
            # unused IRQ after the placeholder that fills the table.
            continue
        out[str(idx)] = name
        # An entry's `arg` is a `const void *`, so most of the time it is data
        # and resolves to nothing. When it resolves to a *function*, the entry
        # is a trampoline and this is the handler that actually runs -- which
        # on Nordic is the common case, not the exception: nrfx registers one
        # `nrfx_isr` for every peripheral and passes the driver's handler.
        if arg_ptr:
            arg_name = elf.func_by_addr.get(arg_ptr & ~1)
            if arg_name is not None and arg_name != name:
                args[str(idx)] = arg_name
    if unresolved:
        notes.append(
            f"{unresolved} _sw_isr_table entries pointed at an address with no function "
            "symbol; those IRQs render as bare numbers"
        )
    shared = sum(1 for i in out if i in args)
    if shared:
        notes.append(
            f"{shared} of {len(out)} IRQs dispatch through a shared trampoline; "
            "isr_args names the handler each one actually calls"
        )
    return out, args


def config_warnings(elf: Elf, notes: list[str]) -> None:
    """Build variants that defeat ISR resolution, named rather than assumed away."""
    if "z_shared_isr" in elf.symbols:
        notes.append(
            "CONFIG_SHARED_INTERRUPTS appears to be on (z_shared_isr is linked in): "
            "a shared line's table entry points at the dispatcher, not at the "
            "application handler, so those IRQs resolve to z_shared_isr"
        )
    if "z_isr_install" in elf.symbols:
        notes.append(
            "CONFIG_DYNAMIC_INTERRUPTS appears to be on (z_isr_install is linked in): "
            "_sw_isr_table is filled at runtime, so entries still holding their "
            "link-time placeholder are absent here rather than named wrongly"
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--build-id", required=True)
    ap.add_argument("--outpost-version", required=True)
    ap.add_argument("--layout-version", type=int, required=True)
    ap.add_argument("--cycles-per-sec-config", type=int, default=0)
    args = ap.parse_args()

    elf = Elf(args.elf)
    notes: list[str] = []
    try:
        manifest = {
            "schema": MANIFEST_SCHEMA,
            "build_id": args.build_id,
            "outpost_version": args.outpost_version,
            "record_layout_version": args.layout_version,
            # Present for reference only. The host uses the rate the firmware
            # reports in its header frame, because this Kconfig legitimately
            # defaults to 0 on targets that read their timer frequency at
            # runtime (decision 4).
            "cycles_per_sec_config": args.cycles_per_sec_config,
            "arch": elf.arch,
            "markers": read_markers(elf, notes),
            "threads": read_threads(elf, notes),
        }
        manifest["isrs"], manifest["isr_args"] = read_isrs(elf, notes)
        config_warnings(elf, notes)
    finally:
        elf.close()

    manifest["notes"] = notes

    # A CRC over the manifest's own canonical body. NOT what the firmware
    # reports — decision 9's rework replaced the post-link CRC patch with the
    # compile-time build ID above, and a manifest generated from the linked
    # image has no CRC the firmware could have been built knowing. This is a
    # content fingerprint, for telling two manifests apart.
    body = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    manifest["manifest_crc"] = binascii.crc32(body) & 0xFFFFFFFF

    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
        fh.write("\n")

    print(
        f"embarch-outpost: {args.out} "
        f"({len(manifest['markers'])} markers, {len(manifest['threads'])} threads, "
        f"{len(manifest['isrs'])} ISRs)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
