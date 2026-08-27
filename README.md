# embarch-outpost

A Zephyr module you compile into your **own** DUT firmware, for debug builds.
It implements Zephyr's `CONFIG_TRACING_USER` hooks plus engineer-placed
`OUTPOST_EVT` markers and emits a running account of what the MCU is actually
doing — which thread ran, when, for how long, when it was in interrupt context
— out a dedicated TX-only UART, to be recorded and rendered host-side.

**Two clocks, and they answer different questions.** A record is
`{cycles, kind, a, b}`: the DUT stamps every record from its own counter, which
is what orders events *within* a frame and measures how long an ISR actually
took. The host stamps every **frame** on arrival, which is what places the trace
against the other streams in a study. Neither substitutes for the other, and a
rendered row carries both columns named for what they are.

Record layout **3**. Layout 2 removed the DUT's clock outright, on the grounds
that reading it happened inside the context switch and inside `_isr_wrapper()` —
the instrument charging its cost to the code it measures. That objection was
right and `src/outpost_time.h` answers it instead: `k_cycle_get_32()` on a
GRTC-timed nRF goes through `sys_clock_cycle_get_32()`, which takes a
`k_spin_lock` around `nrfx_grtc_syscounter_get()`, which takes a *second*
critical section around a 64-bit read in a retry loop — to hand back 32 bits
this module reads straight out of the SYSCOUNTER's low word. So the clock stays
and the locks go. The version number went 2 → 3 rather than back to 1 because a
version byte exists so a host can say "I decode up to N", and a number reused
after an incompatible wire has worn a higher one cannot say that.

Design and rationale: [../embarch-doc/embarch-outpost/design.md](../embarch-doc/embarch-outpost/design.md).
This README is the operating manual; that doc is why.

## What it costs you to adopt

Four things, all in your repo, none of them here:

**1. The module, pinned.** In your `west.yml`:

```yaml
  - name: embarch-outpost
    url: https://github.com/gabrieltetar/embarch-outpost
    revision: v0.1.0
    path: modules/embarch-outpost
```

or `-DZEPHYR_EXTRA_MODULES=/path/to/embarch-outpost` for a one-off build.

**2. Two Kconfig symbols**, in a debug overlay — not `prj.conf`:

```
CONFIG_EMBARCH_OUTPOST=y
CONFIG_TRACING_USER=y
```

Two, not one. `TRACING_USER` is a Kconfig *choice* member and nothing outside
the choice can select one — `select` and `configdefault` were both tried and
both warn and do nothing. `src/outpost.c` refuses to compile without it, so the
pair cannot be half-enabled.

**3. A devicetree `chosen` node** naming the UART, in your own board overlay —
the same shape Zephyr's own tracing backend uses for `zephyr,tracing-uart`:

```dts
/ {
    chosen { embarch,outpost-uart = &uart21; };
};
&uart21 {
    status = "okay";
    current-speed = <1000000>;
    pinctrl-0 = <&uart21_default>;
};
```

Which UART, which pins, what baud, how big the ring: every one of those is a
fact about **your** board, declared in your repo. This module ships mechanism
and defaults and no opinions about hardware it cannot see.

**4. Markers, if you want spans the kernel cannot see.** One registration list
in one header:

```c
/* app_outpost_markers.h */
#define OUTPOST_MARKERS(X)  \
    X(PPG_FRAME_BEGIN)      \
    X(PPG_FRAME_END)
```

point `CONFIG_EMBARCH_OUTPOST_MARKER_HEADER="app_outpost_markers.h"` at it, add
its directory with `zephyr_include_directories()` (not
`target_include_directories(app ...)` — this module compiles that header too),
and then, anywhere including inside an ISR:

```c
OUTPOST_EVT(PPG_FRAME_BEGIN, frame_no);
```

An unregistered name is a **build error**, not a mystery integer on the host.

## What you get out

Two artifacts, both produced by the build, neither of which you handle:

- **`outpost-manifest.json`**, beside `zephyr.elf`. Marker IDs → names,
  `_k_thread_obj_*` → thread names, `_sw_isr_table[]` index → handler names
  (and the handler behind a shared trampoline — on Nordic most IRQs dispatch
  through `nrfx_isr`, and the manifest names what it actually calls), the
  build ID, and the record layout version.
- **the stream itself**, out the UART: COBS-framed, postcard-encoded, CRC'd
  per frame, with a header frame repeated so a host attaching mid-stream can
  decode, and explicit **gap records** wherever the ring overflowed. A gap
  record is always the first record of its frame, which is what lets a host
  bound the losses between two arrivals.

`embarch-api` picks the manifest up from the build and hands it to
`embarch-core` alongside the firmware, because the failure mode of forgetting
is not a visible error but a silently mislabelled trace.

Decode it yourself with `scripts/decode_outpost.py`:

```
python3 scripts/decode_outpost.py --manifest build/zephyr/outpost-manifest.json build/outpost.bin
```

Add `--arrival <frame_index,rx_utc_ms CSV>` to fill in the host half of the row.
Without it the `rx_utc_ms` column comes out empty, which is a trace that is
**ordered and timed on the DUT's clock but unplaced on the host's** — a real
answer, and honestly distinguishable from a fully placed one. `embarch-core`
writes exactly that CSV beside every capture (`<tap>.arrival.csv`).

It **refuses** to decode against a manifest whose `build_id` does not match the
running firmware's, or whose record layout version is not this decoder's. Both
refusals are the feature.

## Tests

```
export ZEPHYR_BASE=/path/to/zephyr
export WEST=/path/to/west          # west is often not on a bare PATH
./tests/run-all.sh
```

- `tests/unit` — 15 ztests on `native_sim`: varint, COBS, record and frame
  layout pinned against **literal bytes** (not round-tripped through this
  encoder's own inverse — the format has three implementations and a round trip
  agrees with itself no matter what the other two do), plus ring ordering,
  overflow accounting, the marker registration contract, the GPIO record's
  literal bytes, and that the ring's 32-bit reservation counter survives its own
  wraparound (reachable only because `outpost_ring_init_at()` exists to seed it
  near the top).
- `tests/native_sim_stream` — the end-to-end one: a real Zephyr app whose only
  stdout writer is the outpost UART, run, captured, and decoded against the
  manifest its own build produced. Asserts that every frame passes its CRC,
  that every record kind appears (gap included — the app overflows the ring on
  purpose), that real thread and marker names resolve, and that a manifest with
  the wrong build ID is refused rather than applied.

## Status

Built, `native_sim`-verified end to end, and built for the real nRF54L15
reference-dut target with its manifest resolving that image's real threads and
ISRs. **No outpost byte has ever crossed a real UART.** Every wire constant in
`Kconfig` is a provisional default nobody has measured, and the instrumentation
overhead is deliberately uncharacterised — see `design.md` §7.
