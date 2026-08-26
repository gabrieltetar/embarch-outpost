/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internal shape of the outpost: records, ring, and the wire format.
 *
 * THE WIRE FORMAT IS PINNED IN TWO LANGUAGES. Anything changed here must be
 * changed in `embarch-core`'s decoder and in `scripts/decode_outpost.py`, and
 * OUTPOST_RECORD_LAYOUT_VERSION must be bumped. The suite's rule (see
 * ../embarch-doc/embarch-outpost/design.md §4) is that every wire record is
 * pinned by a test on both sides, against identical literal bytes.
 *
 * ---- No clock on this side --------------------------------------------------
 *
 * **A record carries no time.** Layout 2 removed the per-record
 * `k_cycle_get_32()` stamp outright: the emit path runs inside the context
 * switch and inside `_isr_wrapper()`, and a counter read there is cost the
 * instrument charges to the exact path it is measuring (design.md §3 decision
 * 4, reworked 2026-08-26). Nothing in this module reads a clock any more —
 * not per record, not per frame.
 *
 * The clock is the host's: Core stamps each frame with its own receipt time as
 * the bytes arrive (`embarch-core/design.md` §3 decision 30), so **every
 * record in a frame shares one timestamp**, and a frame is the finest interval
 * this wire can resolve. Ordering inside a frame is real; duration inside one
 * is not measurable and must never be interpolated (design.md §3 decision 17).
 *
 * ---- Frame ----------------------------------------------------------------
 *
 *   frame := COBS(body || crc32_ieee(body) as 4 bytes LE) || 0x00
 *
 * COBS is the same framing the Core<->dev-bench link already uses
 * (embarch-study-designer/design.md §3 decision 10), so Core's framing code
 * shape applies unchanged. The CRC lets a host discard a corrupt or partial
 * frame rather than mis-decode it.
 *
 *   body := frame_type: u8
 *           seq:        u8      -- wraps; a gap in seq is a lost frame
 *           payload
 *
 * frame_type 0x01 -- Records. payload is a postcard `Vec<Record>`:
 *
 *   count: varint(u32), then `count` records, each:
 *       kind:   u8            -- enum outpost_kind
 *       a:      varint(u32)
 *       b:      varint(u32)
 *
 * frame_type 0x02 -- Header. Emitted at startup and every
 * CONFIG_EMBARCH_OUTPOST_HEADER_INTERVAL_MS thereafter, so a host attaching
 * mid-stream can decode. payload:
 *
 *       layout_version:  u8
 *       flags:           u8     -- enum outpost_header_flag
 *       outpost_version: postcard string (varint len, then bytes)
 *       build_id:        postcard string
 *
 * The header carried a `cycles_per_sec` field through layout 1 and does not
 * any more: there is no DUT clock left for it to describe, and a rate reported
 * beside timestamps that are not the DUT's would invite exactly the cross-clock
 * arithmetic decision 17 forbids.
 *
 * The header carries no manifest CRC. It cannot: decision 9's rework replaced
 * the post-link CRC patch with a compile-time build ID, and a manifest
 * generated *from the linked ELF* has no CRC the firmware could have been
 * built knowing. Verification is `build_id` against the manifest's copy.
 *
 * Strings on the wire: decision 2 rejected CTF over a 20-byte inline thread
 * name *per event*. `build_id` is one string per header frame, at most once a
 * second, and it is what makes the whole manifest check possible. The rule it
 * looks like it breaks is a rule about per-record cost.
 */

#ifndef EMBARCH_OUTPOST_PRIV_H_
#define EMBARCH_OUTPOST_PRIV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/** Bump on ANY change to the record or frame layout above.
 *
 *  2 -- records and the header lost every timestamp; the host stamps frames.
 *  1 -- absolute k_cycle_get_32() in every record, cycles_per_sec in the
 *       header. Not decoded by anything current: a layout-1 stream against a
 *       layout-2 decoder is refused rather than reinterpreted.
 */
#define OUTPOST_RECORD_LAYOUT_VERSION 2

#define OUTPOST_FRAME_RECORDS 0x01u
#define OUTPOST_FRAME_HEADER  0x02u

/** Record kinds. Append-only: a host decoding an unknown kind must skip it,
 *  which the fixed {kind, a, b} shape is what makes possible.
 */
enum outpost_kind {
	OUTPOST_KIND_THREAD_SWITCH_IN = 0,  /* a = thread pointer */
	OUTPOST_KIND_THREAD_SWITCH_OUT = 1, /* a = thread pointer */
	OUTPOST_KIND_ISR_ENTER = 2,         /* a = vector number, or OUTPOST_IRQ_UNKNOWN */
	OUTPOST_KIND_ISR_EXIT = 3,          /* a = vector number, or OUTPOST_IRQ_UNKNOWN */
	OUTPOST_KIND_IDLE = 4,              /* a = 0 */
	OUTPOST_KIND_THREAD_CREATE = 5,     /* a = thread pointer */
	OUTPOST_KIND_THREAD_NAME = 6,       /* a = thread pointer */
	OUTPOST_KIND_MARKER = 7,            /* a = marker ID, b = engineer's arg */
	/* a = records dropped. b = 0, reserved.
	 *
	 * `b` carried the cycle span the losses were lost across through layout
	 * 1 and cannot any more — there is no clock here to measure it with.
	 * What replaces it is a position guarantee: **a gap record is always the
	 * first record of the next records frame**, so a host bounds the losses
	 * by the arrival stamps of the frame before it and the frame carrying
	 * it. That is a bound of one frame, stated as a bound.
	 *
	 * It also ends the one anomaly layout 1 had: a gap's timestamp could
	 * precede the records printed after it (it was stamped when the losses
	 * started, emitted when the ring next had room), which broke the host's
	 * unwrap on the first real capture. With no timestamps there is no
	 * unwrap and no backwards step to misread.
	 */
	OUTPOST_KIND_GAP = 8,
};

/** ISR identity unavailable on this build (CONFIG_EMBARCH_OUTPOST_ISR_IDENTIFY=n,
 *  or an arch whose active vector this module will not guess at).
 */
#define OUTPOST_IRQ_UNKNOWN 0xFFFFFFFFu

/** Header `flags`: what the running firmware actually has compiled in, so a
 *  host never has to infer it from the absence of records.
 */
enum outpost_header_flag {
	OUTPOST_FLAG_TRACE_THREADS = BIT(0),
	OUTPOST_FLAG_TRACE_ISRS = BIT(1),
	OUTPOST_FLAG_TRACE_IDLE = BIT(2),
	OUTPOST_FLAG_TRACE_MARKERS = BIT(3),
	OUTPOST_FLAG_ISR_IDENTIFY = BIT(4),
	OUTPOST_FLAG_OVERFLOW_BLOCK = BIT(5),
};

/** One ring slot: two 32-bit payload fields, a kind, and the publish sequence
 *  that makes the ring lock-free (see outpost_ring.c).
 */
struct outpost_slot {
	uint32_t a;
	uint32_t b;
	uint8_t kind;
	uint8_t _pad[3];
	atomic_t seq;
};

#define OUTPOST_SLOT_BYTES 16

/* This constant is what CONFIG_EMBARCH_OUTPOST_RING_BYTES is divided by to
 * size the ring, so a slot larger than it means the ring quietly allocates
 * more RAM than the Kconfig asked for. It did: a layout-1 slot was 20 bytes
 * against this same 16, so every ring was 1.25x its configured size and the
 * unit test asserting `slots * OUTPOST_SLOT_BYTES <= RING_BYTES` passed
 * anyway. Dropping the timestamp made the two agree; this is what keeps them
 * agreeing.
 */
BUILD_ASSERT(sizeof(struct outpost_slot) == OUTPOST_SLOT_BYTES,
	     "OUTPOST_SLOT_BYTES no longer matches the slot it sizes the ring by");

/* ---- ring (outpost_ring.c) ---- */

void outpost_ring_init(void);

/** Emit one record. Any context, including an ISR. Never blocks unless
 *  CONFIG_EMBARCH_OUTPOST_OVERFLOW_BLOCK and the caller is a thread.
 *
 *  Reads no clock. The whole body is a CAS, three stores and a publish.
 */
void outpost_ring_put(uint8_t kind, uint32_t a, uint32_t b);

/** Pop the next in-order record. false when the ring is empty or the next
 *  record's producer has not finished publishing it.
 */
bool outpost_ring_get(struct outpost_slot *out);

/** Take and clear the accumulated drop account. Returns false when nothing
 *  has been dropped since the last call.
 *
 *  The count is all there is: where in time the losses fell is bounded
 *  host-side by the frames bracketing the gap record, per OUTPOST_KIND_GAP.
 */
bool outpost_ring_take_gap(uint32_t *dropped);

/** Total slots. Exposed for the tests. */
uint32_t outpost_ring_slots(void);

/* ---- wire (outpost_wire.c) ---- */

/** Append a postcard varint. Returns bytes written, 0 if it would not fit. */
size_t outpost_put_varint(uint8_t *buf, size_t cap, uint32_t value);

/** Append a postcard string (varint length, then bytes). */
size_t outpost_put_string(uint8_t *buf, size_t cap, const char *s);

/** Encode one record's postcard body. Returns bytes written, 0 if it would
 *  not fit.
 */
size_t outpost_put_record(uint8_t *buf, size_t cap, const struct outpost_slot *rec);

/** Worst case for one record: 1 + 5 + 5. */
#define OUTPOST_RECORD_MAX_BYTES 11

/** COBS-encode `len` bytes. Does not append the trailing 0x00 delimiter.
 *  Worst-case output is len + len/254 + 2.
 */
size_t outpost_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);

/** Seal `body` with its CRC, COBS it into `out`, and append the delimiter.
 *  Returns the framed length, or 0 if `out_cap` is too small.
 */
size_t outpost_frame(const uint8_t *body, size_t body_len, uint8_t *out, size_t out_cap);

/* ---- transport (outpost.c) ---- */

/** Build-ID string this image was compiled with (generated header). */
const char *outpost_build_id(void);
const char *outpost_version(void);

#endif /* EMBARCH_OUTPOST_PRIV_H_ */
