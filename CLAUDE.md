# embarch-outpost

## Docs

**Four files, not one.** Current truth: [spec.md](../embarch-doc/embarch-outpost/spec.md). Why it is that way: [decisions.md](../embarch-doc/embarch-outpost/decisions.md) — an index over `decisions/`, and a decision number addresses this sub-project, not a file. Unresolved: [open.md](../embarch-doc/embarch-outpost/open.md). Reference: [interfaces/](../embarch-doc/embarch-outpost/interfaces/).

Update them proactively per [../embarch-doc/DOC-PROTOCOL.md](../embarch-doc/DOC-PROTOCOL.md) whenever a notable design decision, feature, or status change happens here — §4 says when, §5 says how, and history goes in a `changelog.d/` fragment rather than into a doc.

## Git

**Work on a branch, land when the owner asks — no PRs (2026-11-03).** Work in a worktree, with one branch name across every repo the change touches, and push the branch. Land on `main` only when the repo owner asks: rebase on `origin/main`, confirm it builds and its tests and lint are clean, fast-forward, push, delete the branch. The owner's explicit call ended the earlier commit-straight-to-`main` rule, and changing this one is the owner's call too, not an agent's read of what the project needs. The fleet lands under its own protocol. Reasoning, setup and the landing order across repos: [../embarch-doc/embarch-dev-workflow.md](../embarch-doc/embarch-dev-workflow.md) §6.

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
- `tests/decoder_unit.py` — host unit tests for that decoder; stdlib only, no west, no siblings
- `tests/unit/` — ztest suite (ring, varint, COBS, CRC, frame shape)
- `tests/native_sim_stream/` — the end-to-end test: real UART bytes out, decoded on the host
