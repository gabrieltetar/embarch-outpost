#!/usr/bin/env python3
"""Emit outpost-manifest.json from a linked Zephyr ELF.

../embarch-doc/embarch-outpost/design.md §3 decisions 6, 7, 8, 9 and §5.4.

Everything this reads is a fact recorded in the image by the build that
produced it. Nothing here matches heuristically, and nothing is guessed:

  markers  the `outpost_marker_table` the application's own OUTPOST_MARKERS(X)
           declaration generated, walked to its {0, NULL} sentinel
  threads  every variable whose DWARF *type* is `struct k_thread`, or a struct
           whose first member is one (`struct k_work_q` is the common case),
           paired with that variable's address out of the symbol table. DWARF
           says what a thing is; the symbol table says where it lives; neither
           is guessed. Falls back to the old `_k_thread_obj_<name>` name match
           on an image linked without debug info, and says so in `notes`
  isrs     `_sw_isr_table[]`, "an array of these structures indexed by the
           irq line" (include/zephyr/sw_isr_table.h), resolved against the
           symbol table at exactly the index the firmware reports -- and its
           `arg` too, because on this suite's first real target most entries
           dispatch through a shared `nrfx_isr` trampoline whose argument is
           the driver handler that actually runs
  devices  `__device_dts_ord_*` symbols, each read for the `const char *name`
           that is the first member of `struct device`, so a device pointer on
           the wire renders as the node it is
  functions
           every STT_FUNC symbol, address to name. Only with --functions,
           because it is by far the largest table here (~4k entries, ~160 kB on
           a real image) and only one record kind needs it: a GPIO callback's
           handler pointer, which cannot be predicted at link time because
           gpio_init_callback() runs at runtime

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

# 2 added the `devices` and `functions` tables. Additive: both are optional and
# a reader that does not know them is unaffected, which is why no
# record_layout_version moved with it.
MANIFEST_SCHEMA = 2


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

    # ---- DWARF: which variables are threads ------------------------------
    #
    # The symbol table says a name and an address; it does not say a type. So
    # `drain_thread`, `bt_long_wq` and `k_sys_work_q` are indistinguishable
    # from any other object in .bss by symbol alone, and all three are threads
    # a real reference-dut trace spends real time in.
    #
    # Reading the type out of DWARF is the exact answer, and it is the same
    # class of fact as everything else here: something the build recorded, not
    # something matched by shape. Two deliberate choices in how it is read.
    #
    # **Type from DWARF, address from the symbol table.** Only definition DIEs
    # carry a location, and Zephyr's own kernel objects -- `z_main_thread`,
    # `z_idle_threads`, `k_sys_work_q`, `_thread_dummy` -- appear in this image
    # *only* as `DW_AT_declaration` DIEs: typed, with no address. Insisting on
    # a DWARF location loses exactly the objects a quiet trace spends most of
    # its time in. A name is enough to join on, and the join is exact.
    #
    # **First-member recursion, at offset 0 only.** `struct k_work_q`'s first
    # member is a `struct k_thread`, so a work queue's address *is* its
    # thread's address, and that is the pointer `k_thread_create()` hands the
    # kernel and the hooks see. A member at any non-zero offset stops the
    # recursion, because then the outer object's address is not the thread's.

    _TYPE_SKIP = ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type")

    @classmethod
    def _strip(cls, die, depth: int = 0):
        """The underlying DIE behind any chain of typedef/const/volatile."""
        while die is not None and depth < 16:
            if die.tag in cls._TYPE_SKIP:
                if "DW_AT_type" not in die.attributes:
                    return None
                die = die.get_DIE_from_attribute("DW_AT_type")
                depth += 1
                continue
            return die
        return None

    @staticmethod
    def _die_name(die):
        if die is None:
            return None
        attr = die.attributes.get("DW_AT_name")
        return attr.value.decode() if attr else None

    @classmethod
    def _is_thread_shaped(cls, die, depth: int = 0) -> bool:
        if die is None or depth > 4 or die.tag != "DW_TAG_structure_type":
            return False
        if cls._die_name(die) == "k_thread":
            return True
        for child in die.iter_children():
            if child.tag != "DW_TAG_member":
                continue
            offset = child.attributes.get("DW_AT_data_member_location")
            if offset is not None and getattr(offset, "value", 0) != 0:
                return False
            if "DW_AT_type" not in child.attributes:
                return False
            try:
                inner = cls._strip(child.get_DIE_from_attribute("DW_AT_type"))
            except Exception:
                return False
            return cls._is_thread_shaped(inner, depth + 1)
        return False

    def thread_objects(self):
        """Address to variable name for every thread object, plus a reason."""
        if not self.elf.has_dwarf_info():
            return {}, "this ELF carries no DWARF"
        dwarf = self.elf.get_dwarf_info()
        if dwarf is None:
            return {}, "this ELF carries no DWARF"

        singles = set()
        arrays = {}
        seen = set()
        try:
            for cu in dwarf.iter_CUs():
                for die in cu.iter_DIEs():
                    if die.tag != "DW_TAG_variable":
                        continue
                    name_attr = die.attributes.get("DW_AT_name")
                    if name_attr is None or "DW_AT_type" not in die.attributes:
                        continue
                    name = name_attr.value.decode()
                    if name in seen:
                        continue
                    try:
                        typ = self._strip(die.get_DIE_from_attribute("DW_AT_type"))
                    except Exception:
                        continue
                    if typ is None:
                        continue
                    if typ.tag == "DW_TAG_array_type":
                        # `struct k_thread z_idle_threads[CONFIG_MP_MAX_NUM_CPUS]`
                        # is why this branch exists: one symbol, one thread per
                        # CPU, and the stride is the element type's own recorded
                        # byte size rather than anything inferred.
                        try:
                            element = self._strip(typ.get_DIE_from_attribute("DW_AT_type"))
                        except Exception:
                            continue
                        if self._is_thread_shaped(element):
                            size_attr = element.attributes.get("DW_AT_byte_size")
                            if size_attr is not None and size_attr.value:
                                arrays[name] = int(size_attr.value)
                                seen.add(name)
                    elif self._is_thread_shaped(typ):
                        singles.add(name)
                        seen.add(name)
        except Exception as exc:
            return {}, "this ELF's DWARF could not be walked (%s)" % (exc,)

        out = {}
        for name in singles:
            entry = self.symbols.get(name)
            if entry is not None:
                out[entry[0]] = name
        for name, stride in arrays.items():
            entry = self.symbols.get(name)
            if entry is None:
                continue
            addr, size = entry
            count = max(1, size // stride) if size else 1
            for i in range(count):
                # A one-element array keeps the plain symbol name: an `[0]` on
                # every single-CPU build is noise in a lane label.
                out[addr + i * stride] = name if count == 1 else "%s[%d]" % (name, i)
        return out, None


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
    """Every thread object in the image, by address.

    A thread pointer is what crosses the wire (design.md §3 decision 8), and
    what makes it a *name* is this table. Until 2026-08-27 the table came from
    symbol names alone -- `_k_thread_obj_<name>`, which is literally what
    `K_THREAD_DEFINE(name, ...)` expands its `struct k_thread` to, plus the
    kernel's own two -- and on the real reference-dut that resolved **5 of 20**.
    The other fifteen rendered as raw pointers, including every one of these,
    which between them are most of what a quiet trace does:

        drain_thread            the outpost's own, 778 runs in the reference
                                capture and the lane an engineer debugging the
                                tracer would look for first
        bt_long_wq, bt_workq    the BLE stack's work queues, 12 ms a run
        k_sys_work_q            the system work queue
        prio_recv_thread_data,  the controller's receive threads
        recv_thread_data
        app_sm_thread_data      the application's own state machine

    None of those is a `K_THREAD_DEFINE`, and four of them are not even a
    `struct k_thread` -- a `struct k_work_q` starts with one, so a work queue's
    address *is* its thread's address. No naming convention could have found
    them, and the previous version's own note said as much: "threads created
    with k_thread_create() into a plain `static struct k_thread` have no
    distinguishing symbol and are absent here by design".

    What replaced "by design" is [`Elf.thread_objects`]: the DWARF type says
    which variables are threads, the symbol table says where they are. Both are
    facts the build recorded. The name-match path is kept as the fallback for an
    image linked without debug info, and which path ran is in `notes`.
    """
    # The kernel's own two get the names everyone calls them, not their C
    # symbols. Everything else keeps the identifier its author chose, since a
    # trace is read alongside that author's source.
    friendly = {"z_main_thread": "main", "z_idle_threads": "idle"}

    def label(symbol: str) -> str:
        if symbol in friendly:
            return friendly[symbol]
        prefix = "_k_thread_obj_"
        return symbol[len(prefix):] if symbol.startswith(prefix) else symbol

    typed, why_not = elf.thread_objects()
    if typed:
        out = {f"0x{addr:08x}": label(symbol) for addr, symbol in typed.items()}
        notes.append(
            f"{len(out)} thread objects resolved by DWARF type (struct k_thread, or a struct "
            "whose first member is one) joined to the symbol table for their addresses"
        )
        missing = [
            symbol
            for symbol in elf.symbols
            if symbol.startswith("_k_thread_obj_") and elf.symbols[symbol][0] not in typed
        ]
        if missing:
            # A K_THREAD_DEFINE the type walk missed is a real gap in the walk,
            # not something to paper over silently: the name match is exact for
            # exactly these, so they are added and the disagreement is reported.
            for symbol in missing:
                out[f"0x{elf.symbols[symbol][0]:08x}"] = label(symbol)
            notes.append(
                "the DWARF walk missed these K_THREAD_DEFINE threads, which the symbol-name "
                f"match then supplied: {sorted(missing)}"
            )
        return out

    # ---- fallback: symbol names only -------------------------------------
    notes.append(
        f"{why_not or 'no thread types were readable'}, so threads are resolved by symbol name "
        "alone: only K_THREAD_DEFINE's `_k_thread_obj_*` and the kernel's own two. A thread "
        "created into a plain `static struct k_thread` or wrapped in a `struct k_work_q` has no "
        "distinguishing name and renders as the pointer it is, rather than as a guess"
    )
    out = {}
    for symbol, (addr, _size) in elf.symbols.items():
        if symbol.startswith("_k_thread_obj_"):
            out[f"0x{addr:08x}"] = label(symbol)
    if not out:
        notes.append("no _k_thread_obj_* symbols found either")
    for symbol in ("z_main_thread", "z_idle_threads"):
        entry = elf.symbols.get(symbol)
        if entry is not None:
            out[f"0x{entry[0]:08x}"] = label(symbol)
    idle = elf.symbols.get("z_idle_threads")
    if idle is not None and idle[1] > 0:
        notes.append(
            "z_idle_threads is an array, one per CPU; only the first is named on this path, "
            "because its stride is sizeof(struct k_thread) and the symbol table does not "
            "state it. The DWARF path reads the stride off the element type and names them all"
        )
    return out


def read_isrs(elf: Elf, notes: list[str]) -> tuple[dict[str, str], dict[str, str]]:
    """Every named vector in the image, from the two tables that hold them.

    `_sw_isr_table[]` is indexed by irq line and each entry is { arg, isr }
    (`include/zephyr/sw_isr_table.h`) -- that covers every IRQ dispatched
    through `_isr_wrapper()`. `_irq_vector_table[]` covers the ones that are
    not: an `IRQ_DIRECT_CONNECT` line has an empty `_sw_isr_table` row and its
    handler wired straight into the hardware vector table.
    """
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
    # ---- IRQ_DIRECT_CONNECT: handlers that never reach _sw_isr_table -------
    #
    # `_sw_isr_table` holds only the IRQs that dispatch through
    # `_isr_wrapper()`. An `IRQ_DIRECT_CONNECT` line puts its handler straight
    # into the hardware vector table and leaves its `_sw_isr_table` row empty,
    # so it resolves to nothing above -- and on this suite's first real target
    # the one line that does this is **the radio**, which is the single most
    # interesting vector in a BLE trace.
    #
    # `_irq_vector_table[irq]` is a function address under
    # CONFIG_IRQ_VECTOR_TABLE_JUMP_BY_ADDRESS, which is what the nRF54L15 build
    # uses. Any entry that is not `_isr_wrapper` is a direct handler, and the
    # comparison against `_isr_wrapper`'s own symbol is what makes this exact
    # rather than a guess about which entries are interesting.
    #
    # Deliberately additive and deliberately second: a line present in both
    # tables keeps its `_sw_isr_table` name, since that is the dispatch that
    # actually runs.
    vector_table = elf.symbols.get("_irq_vector_table")
    wrapper = elf.symbols.get("_isr_wrapper")
    if vector_table is None:
        notes.append(
            "no _irq_vector_table in the ELF, so an IRQ_DIRECT_CONNECT handler "
            "(the radio, on a Nordic BLE build) resolves to no name"
        )
    elif wrapper is None:
        notes.append(
            "no _isr_wrapper symbol, so _irq_vector_table entries cannot be told from the "
            "shared dispatcher and no direct handler is named from it"
        )
    else:
        vbase, vsize = vector_table
        wrapper_addr = wrapper[0] & ~1
        direct = 0
        for idx in range(vsize // elf.ptr_bytes):
            handler = elf.word(vbase + idx * elf.ptr_bytes)
            if handler is None:
                break
            handler &= ~1
            if handler in (0, wrapper_addr):
                continue
            name = elf.func_by_addr.get(handler)
            if name is None or name == "z_irq_spurious":
                continue
            if str(idx) in out:
                continue
            out[str(idx)] = name
            direct += 1
        if direct:
            notes.append(
                f"{direct} IRQ_DIRECT_CONNECT handler(s) named from _irq_vector_table, which "
                "_sw_isr_table does not carry at all"
            )

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


def read_devices(elf: Elf, notes: list[str]) -> dict[str, str]:
    """`struct device`'s first member is `const char *name` (zephyr/device.h)."""
    prefix = "__device_dts_ord_"
    out: dict[str, str] = {}
    unnamed = 0
    for sym, (addr, _size) in elf.symbols.items():
        if not sym.startswith(prefix):
            continue
        name_ptr = elf.word(addr)
        name = elf.cstring(name_ptr) if name_ptr else None
        if name is None:
            # The ordinal alone is not a device name, and a manifest that
            # printed it as one would be naming a device after a build-order
            # artefact. Left out; counted.
            unnamed += 1
            continue
        out[f"0x{addr:08x}"] = name
    if not out:
        notes.append(
            "no __device_dts_ord_* symbols were readable: GPIO dispatch records carry "
            "their raw port pointer and resolve to no name"
        )
    if unnamed:
        notes.append(
            f"{unnamed} device symbols had no readable name string and are omitted rather "
            "than named after their devicetree ordinal"
        )
    return out


def read_functions(elf: Elf) -> dict[str, str]:
    """Every function symbol, address to name.

    The one table here that is not a targeted read, and the only one a record
    kind needs *because* it cannot be targeted: a GPIO callback's handler is
    installed by gpio_init_callback() at runtime, so nothing at link time knows
    which functions will appear on the wire. The honest answer is all of them.
    """
    return {f"0x{addr:08x}": name for addr, name in sorted(elf.func_by_addr.items())}


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
    ap.add_argument(
        "--functions",
        action="store_true",
        help="also emit the full function symbol table, which GPIO callback records "
             "need to resolve a handler pointer to a name. Off by default because it "
             "is ~10x the rest of the manifest; CMakeLists.txt passes it exactly when "
             "CONFIG_EMBARCH_OUTPOST_TRACE_GPIO is set.",
    )
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
        manifest["devices"] = read_devices(elf, notes)
        if args.functions:
            manifest["functions"] = read_functions(elf)
        else:
            notes.append(
                "the function symbol table was not emitted (--functions not passed, i.e. "
                "CONFIG_EMBARCH_OUTPOST_TRACE_GPIO=n): any record whose `a` is a code "
                "pointer renders as that pointer"
            )
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
        f"{len(manifest['isrs'])} ISRs, {len(manifest['devices'])} devices, "
        f"{len(manifest.get('functions', {}))} functions)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
