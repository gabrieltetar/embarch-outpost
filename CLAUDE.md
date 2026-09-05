# embarch-outpost

## Docs

**Four files, not one.** Current truth: [spec.md](../embarch-doc/embarch-outpost/spec.md). Why it is that way: [decisions.md](../embarch-doc/embarch-outpost/decisions.md) — an index over `decisions/`, and a decision number addresses this sub-project, not a file. Unresolved: [open.md](../embarch-doc/embarch-outpost/open.md). Reference: [interfaces/](../embarch-doc/embarch-outpost/interfaces/).

Update them proactively per [../embarch-doc/DOC-PROTOCOL.md](../embarch-doc/DOC-PROTOCOL.md) whenever a notable design decision, feature, or status change happens here — §4 says when, §5 says how, and history goes in a `changelog.d/` fragment rather than into a doc.

## Git

**Work directly on `main` — no feature branches, no PRs (2026-08-25).** Commit and push straight to `main` once the change builds and its tests and lint are clean. This **overrides** the general "if you're on the default branch, branch first" default, for this suite only. It ends when the repo owner explicitly says it does, and on no other condition — not on an agent's read of whether the project has outgrown it. Reasoning, the sequencing rules that keep it safe, and the one case that still warrants a branch: [../embarch-doc/embarch-dev-workflow.md](../embarch-doc/embarch-dev-workflow.md) §6.

## What this repo is

A Zephyr module compiled into a DUT's *own* debug firmware. It implements Zephyr's
`CONFIG_TRACING_USER` hooks plus engineer-placed `OUTPOST_EVT` markers, and emits a
thread/ISR/marker timeline out a dedicated TX-only UART, framed COBS + postcard.
A post-link CMake step emits `outpost-manifest.json` beside `zephyr.elf`; the host
decodes the stream against it and refuses to decode against the wrong one.

This repo ships **mechanism and defaults only**. Every board fact — which UART,
which pins, what baud, how big the ring — is declared in the consuming DUT repo
(a `chosen { embarch,outpost-uart }` node plus Kconfig). An outpost that shipped
an opinion about someone else's board would be asserting a fact it cannot know.

## Layout

- `Kconfig`, `CMakeLists.txt`, `zephyr/module.yml` — the module surface
- `include/embarch/outpost.h` — `OUTPOST_EVT`, the marker registration contract
- `src/` — ring, drain thread, UART transport, `TRACING_USER` hooks, wire encoder
- `scripts/gen_outpost_manifest.py` — the post-link manifest generator (reads the ELF)
- `scripts/decode_outpost.py` — reference host decoder, used by the native_sim test
- `tests/unit/` — ztest suite (ring, varint, COBS, CRC, frame shape)
- `tests/native_sim_stream/` — the end-to-end test: real UART bytes out, decoded on the host
