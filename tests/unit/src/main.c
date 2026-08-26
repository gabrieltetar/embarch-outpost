/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for the outpost's wire encoder and record ring.
 *
 * The encoder tests pin literal bytes rather than round-tripping through the
 * encoder's own inverse, because the format has three implementations (here,
 * scripts/decode_outpost.py, and embarch-core) and a round-trip test agrees
 * with itself no matter what the other two do. This is the suite's standing
 * both-languages rule applied to a third language.
 */

#include <zephyr/ztest.h>

#include <embarch/outpost.h>
#include <outpost_priv.h>

/* ---- varint ------------------------------------------------------------- */

ZTEST(outpost_wire, test_varint_literal_bytes)
{
	uint8_t buf[8];

	zassert_equal(outpost_put_varint(buf, sizeof(buf), 0), 1);
	zassert_equal(buf[0], 0x00);

	zassert_equal(outpost_put_varint(buf, sizeof(buf), 127), 1);
	zassert_equal(buf[0], 0x7F);

	zassert_equal(outpost_put_varint(buf, sizeof(buf), 128), 2);
	zassert_equal(buf[0], 0x80);
	zassert_equal(buf[1], 0x01);

	zassert_equal(outpost_put_varint(buf, sizeof(buf), 300), 2);
	zassert_equal(buf[0], 0xAC);
	zassert_equal(buf[1], 0x02);

	/* The five-byte worst case. Layout 1 paid it once per record for an
	 * absolute cycle count near the top of its range; layout 2 has no
	 * timestamp, so the only fields that can reach it are a thread pointer
	 * and an engineer's own marker argument.
	 */
	zassert_equal(outpost_put_varint(buf, sizeof(buf), 0xFFFFFFFFu), 5);
	zassert_equal(buf[0], 0xFF);
	zassert_equal(buf[1], 0xFF);
	zassert_equal(buf[2], 0xFF);
	zassert_equal(buf[3], 0xFF);
	zassert_equal(buf[4], 0x0F);
}

ZTEST(outpost_wire, test_varint_refuses_to_overrun)
{
	uint8_t buf[2];

	zassert_equal(outpost_put_varint(buf, 0, 1), 0);
	zassert_equal(outpost_put_varint(buf, 2, 0xFFFFFFFFu), 0);
}

ZTEST(outpost_wire, test_string_is_length_prefixed)
{
	uint8_t buf[16];
	size_t n = outpost_put_string(buf, sizeof(buf), "abc");

	zassert_equal(n, 4);
	zassert_equal(buf[0], 3);
	zassert_equal(buf[1], 'a');
	zassert_equal(buf[3], 'c');

	zassert_equal(outpost_put_string(buf, sizeof(buf), ""), 1);
	zassert_equal(buf[0], 0);

	zassert_equal(outpost_put_string(buf, 2, "abc"), 0);
}

/* ---- records ------------------------------------------------------------ */

ZTEST(outpost_wire, test_record_literal_bytes)
{
	struct outpost_slot rec = {
		.kind = OUTPOST_KIND_MARKER,
		.a = 1,
		.b = 128,
	};
	uint8_t buf[OUTPOST_RECORD_MAX_BYTES];
	size_t n = outpost_put_record(buf, sizeof(buf), &rec);

	/* kind | varint(1) | varint(128). No timestamp: layout 2 opens a
	 * record with its kind (outpost_priv.h, "No clock on this side").
	 */
	zassert_equal(n, 4);
	zassert_equal(buf[0], OUTPOST_KIND_MARKER);
	zassert_equal(buf[1], 0x01);
	zassert_equal(buf[2], 0x80);
	zassert_equal(buf[3], 0x01);
}

/* The record shape is what sizes the ring, and a slot that outgrows
 * OUTPOST_SLOT_BYTES makes every ring quietly larger than its Kconfig asked
 * for — which is exactly what a 20-byte layout-1 slot did against this same
 * 16. outpost_priv.h BUILD_ASSERTs it; this is the runtime half, so a reader
 * of the test suite sees the constraint stated where the layout is pinned.
 */
ZTEST(outpost_wire, test_a_slot_is_exactly_the_size_the_ring_is_sized_by)
{
	zassert_equal(sizeof(struct outpost_slot), OUTPOST_SLOT_BYTES);
}

ZTEST(outpost_wire, test_record_worst_case_fits_its_bound)
{
	struct outpost_slot rec = {
		.kind = 0xFF,
		.a = 0xFFFFFFFFu,
		.b = 0xFFFFFFFFu,
	};
	uint8_t buf[OUTPOST_RECORD_MAX_BYTES];

	zassert_equal(outpost_put_record(buf, sizeof(buf), &rec), OUTPOST_RECORD_MAX_BYTES);
}

/* ---- COBS + frame ------------------------------------------------------- */

ZTEST(outpost_wire, test_cobs_literal_bytes)
{
	const uint8_t in[] = {0x11, 0x00, 0x22};
	uint8_t out[8];
	size_t n = outpost_cobs_encode(in, sizeof(in), out);

	zassert_equal(n, 4);
	zassert_equal(out[0], 0x02);
	zassert_equal(out[1], 0x11);
	zassert_equal(out[2], 0x02);
	zassert_equal(out[3], 0x22);
}

ZTEST(outpost_wire, test_frame_is_delimited_and_zero_free)
{
	const uint8_t body[] = {OUTPOST_FRAME_RECORDS, 0x00, 0x01, 0x00, 0x02};
	uint8_t out[32];
	size_t n = outpost_frame(body, sizeof(body), out, sizeof(out));

	zassert_true(n > sizeof(body));
	zassert_equal(out[n - 1], 0x00, "a frame must end with the delimiter");
	for (size_t i = 0; i < n - 1; i++) {
		zassert_not_equal(out[i], 0x00, "byte %zu inside the frame is a delimiter", i);
	}
}

ZTEST(outpost_wire, test_frame_refuses_an_output_that_would_not_fit)
{
	const uint8_t body[] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t out[4];

	zassert_equal(outpost_frame(body, sizeof(body), out, sizeof(out)), 0);
}

ZTEST_SUITE(outpost_wire, NULL, NULL, NULL, NULL, NULL);

/* ---- ring --------------------------------------------------------------- */

static void ring_before(void *unused)
{
	ARG_UNUSED(unused);
	outpost_ring_init();
}

ZTEST(outpost_ring, test_records_come_back_in_order)
{
	struct outpost_slot got;

	for (uint32_t i = 0; i < 8; i++) {
		outpost_ring_put(OUTPOST_KIND_MARKER, i, i * 2);
	}
	for (uint32_t i = 0; i < 8; i++) {
		zassert_true(outpost_ring_get(&got), "ring emptied early at %u", i);
		zassert_equal(got.a, i);
		zassert_equal(got.b, i * 2);
		zassert_equal(got.kind, OUTPOST_KIND_MARKER);
	}
	zassert_false(outpost_ring_get(&got), "ring should be empty");
}

ZTEST(outpost_ring, test_overflow_drops_the_newest_and_counts_it)
{
	const uint32_t slots = outpost_ring_slots();
	struct outpost_slot got;
	uint32_t dropped;

	zassert_false(outpost_ring_take_gap(&dropped),
		      "a fresh ring should have no gap to report");

	for (uint32_t i = 0; i < slots + 5; i++) {
		outpost_ring_put(OUTPOST_KIND_MARKER, i, 0);
	}

	zassert_true(outpost_ring_take_gap(&dropped));
	zassert_equal(dropped, 5, "expected exactly the overflow to be counted");

	/* Drop-the-newest, never overwrite-oldest: the beginning of the burst
	 * is what survives, which is the half a timeline needs.
	 */
	zassert_true(outpost_ring_get(&got));
	zassert_equal(got.a, 0, "the oldest record was overwritten");

	/* And the account is cleared by taking it. */
	zassert_false(outpost_ring_take_gap(&dropped));
}

ZTEST(outpost_ring, test_ring_is_a_power_of_two_of_slots)
{
	uint32_t slots = outpost_ring_slots();

	zassert_true(slots > 0);
	zassert_equal(slots & (slots - 1), 0, "%u slots is not a power of two", slots);
	/* An equality on the slot size, not just this bound, is what stops the
	 * ring being bigger than its Kconfig: see
	 * test_a_slot_is_exactly_the_size_the_ring_is_sized_by.
	 */
	zassert_true(slots * sizeof(struct outpost_slot) <= CONFIG_EMBARCH_OUTPOST_RING_BYTES);
}

ZTEST(outpost_ring, test_marker_ids_come_from_the_registration_list)
{
	zassert_equal(OUTPOST_MARKER_ALPHA, 0);
	zassert_equal(OUTPOST_MARKER_BETA, 1);
	zassert_equal(OUTPOST_MARKER_COUNT, 2);
	zassert_equal(outpost_marker_table[OUTPOST_MARKER_ALPHA].id, OUTPOST_MARKER_ALPHA);
	zassert_str_equal(outpost_marker_table[OUTPOST_MARKER_ALPHA].name, "ALPHA");
	zassert_str_equal(outpost_marker_table[OUTPOST_MARKER_BETA].name, "BETA");
	zassert_is_null(outpost_marker_table[OUTPOST_MARKER_COUNT].name,
			"the sentinel row must terminate the table");
}

ZTEST(outpost_ring, test_an_unregistered_marker_id_is_dropped)
{
	struct outpost_slot got;

	OUTPOST_EVT(ALPHA, 7);
	zassert_true(outpost_ring_get(&got));
	zassert_equal(got.a, OUTPOST_MARKER_ALPHA);
	zassert_equal(got.b, 7);

	/* OUTPOST_EVT() cannot express this — an unregistered name does not
	 * compile — so it takes a direct call to reach the guard at all.
	 */
	outpost_marker(OUTPOST_MARKER_COUNT, 1);
	zassert_false(outpost_ring_get(&got), "an out-of-range marker ID reached the ring");
}

ZTEST_SUITE(outpost_ring, NULL, NULL, ring_before, NULL, NULL);
