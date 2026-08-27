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

	/* The worst case decision 4 pays for: an absolute 32-bit cycle count
	 * near the top of its range costs five bytes, not the four the design
	 * originally quoted.
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
		.cycles = 300,
		.kind = OUTPOST_KIND_MARKER,
		.a = 1,
		.b = 128,
	};
	uint8_t buf[OUTPOST_RECORD_MAX_BYTES];
	size_t n = outpost_put_record(buf, sizeof(buf), &rec);

	/* varint(300) | kind | varint(1) | varint(128) */
	zassert_equal(n, 6);
	zassert_equal(buf[0], 0xAC);
	zassert_equal(buf[1], 0x02);
	zassert_equal(buf[2], OUTPOST_KIND_MARKER);
	zassert_equal(buf[3], 0x01);
	zassert_equal(buf[4], 0x80);
	zassert_equal(buf[5], 0x01);
}

/* The suite's both-languages rule applied to the two GPIO kinds: these exact
 * bytes are asserted again, as a literal, by
 * embarch-study-designer/src/outpost.rs. Produced by the firmware encoder here
 * and decoded there — not round-tripped through either side's own inverse,
 * which would agree with itself no matter what the other did.
 */
ZTEST(outpost_wire, test_gpio_records_literal_bytes)
{
	uint8_t buf[OUTPOST_RECORD_MAX_BYTES];

	/* The port's `struct device *`, and a `b` of 0 because Zephyr's own hook
	 * has already truncated the pin mask to 8 bits.
	 */
	struct outpost_slot dispatch = {
		.cycles = 1000,
		.kind = OUTPOST_KIND_GPIO_DISPATCH,
		.a = 0x00071624,
		.b = 0,
	};
	const uint8_t want_dispatch[] = {0xE8, 0x07, 0x09, 0xA4, 0xAC, 0x1C, 0x00};

	zassert_equal(outpost_put_record(buf, sizeof(buf), &dispatch), sizeof(want_dispatch));
	zassert_mem_equal(buf, want_dispatch, sizeof(want_dispatch));

	/* A Thumb handler pointer keeps its low bit on the wire; masking it is
	 * the host's job, against a symbol address that does not carry one.
	 */
	struct outpost_slot done = {
		.cycles = 1040,
		.kind = OUTPOST_KIND_GPIO_CALLBACK_DONE,
		.a = 0x0000A4D9,
		.b = 0x0008,
	};
	const uint8_t want_done[] = {0x90, 0x08, 0x0A, 0xD9, 0xC9, 0x02, 0x08};

	zassert_equal(outpost_put_record(buf, sizeof(buf), &done), sizeof(want_done));
	zassert_mem_equal(buf, want_done, sizeof(want_done));
}

ZTEST(outpost_wire, test_record_worst_case_fits_its_bound)
{
	struct outpost_slot rec = {
		.cycles = 0xFFFFFFFFu,
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
	uint32_t first;
	uint32_t span;

	zassert_false(outpost_ring_take_gap(&dropped, &first, &span),
		      "a fresh ring should have no gap to report");

	for (uint32_t i = 0; i < slots + 5; i++) {
		outpost_ring_put(OUTPOST_KIND_MARKER, i, 0);
	}

	zassert_true(outpost_ring_take_gap(&dropped, &first, &span));
	zassert_equal(dropped, 5, "expected exactly the overflow to be counted");

	/* Drop-the-newest, never overwrite-oldest: the beginning of the burst
	 * is what survives, which is the half a timeline needs.
	 */
	zassert_true(outpost_ring_get(&got));
	zassert_equal(got.a, 0, "the oldest record was overwritten");

	/* And the account is cleared by taking it. */
	zassert_false(outpost_ring_take_gap(&dropped, &first, &span));
}

/* The reservation counter is monotonic and 32-bit, so it wraps, and the slot
 * publish sequence used to be reservation + 1 with no guard — which made the
 * one reservation at UINT32_MAX publish 0, the value that means "not yet
 * published". The consumer parked there read its own record as unpublished and
 * stalled for the rest of the boot: every record after it dropped, silently, at
 * one exact point in 2^32.
 *
 * Seeded a few slots short of the wrap so the whole thing is reachable in a
 * handful of puts rather than 4 billion.
 */
ZTEST(outpost_ring, test_records_survive_the_reservation_counter_wrapping)
{
	const uint32_t before_wrap = 3;
	const uint32_t after_wrap = 3;
	struct outpost_slot got;

	outpost_ring_init_at(UINT32_MAX - before_wrap);

	for (uint32_t i = 0; i < before_wrap + after_wrap; i++) {
		outpost_ring_put(OUTPOST_KIND_MARKER, i, 0);
	}

	for (uint32_t i = 0; i < before_wrap + after_wrap; i++) {
		zassert_true(outpost_ring_get(&got),
			     "the ring stalled at record %u, %s the counter wrapped", i,
			     (i < before_wrap) ? "before" : "after");
		zassert_equal(got.a, i, "record %u came back out of order", i);
	}
	zassert_false(outpost_ring_get(&got), "ring should be empty");
}

ZTEST(outpost_ring, test_ring_is_a_power_of_two_of_slots)
{
	uint32_t slots = outpost_ring_slots();

	zassert_true(slots > 0);
	zassert_equal(slots & (slots - 1), 0, "%u slots is not a power of two", slots);
	zassert_true(slots * OUTPOST_SLOT_BYTES <= CONFIG_EMBARCH_OUTPOST_RING_BYTES);
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
