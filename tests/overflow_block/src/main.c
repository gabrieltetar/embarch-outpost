/*
 * Copyright (c) 2026 Gabriel Tetar
 * SPDX-License-Identifier: MIT
 */

/* CONFIG_EMBARCH_OUTPOST_OVERFLOW_BLOCK=y, ring at its 16-slot minimum.
 *
 * The producer runs at priority 1, numerically below (so more urgent than) the
 * drain thread's 10. Before the wait became a sleep, a producer emitting into a
 * full ring called k_yield(), which never reaches a lower-priority thread, so
 * it spun forever and the drain thread that would make room never ran.
 */

#include <zephyr/ztest.h>

#include <embarch/outpost.h>
#include <outpost_priv.h>

#define PRODUCER_PRIORITY 1
#define PRODUCER_STACK_BYTES 2048
#define BURST_SLOTS 8 /* burst = BURST_SLOTS x the ring, so it must wrap many times */
#define PRODUCER_DEADLINE_MS 20000

K_THREAD_STACK_DEFINE(producer_stack, PRODUCER_STACK_BYTES);
static struct k_thread producer;

static void producer_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (uint32_t i = 0; i < outpost_ring_slots() * BURST_SLOTS; i++) {
		outpost_ring_put(OUTPOST_KIND_MARKER, i, 0);
	}
}

ZTEST(outpost_overflow_block, test_a_higher_priority_producer_returns_with_nothing_dropped)
{
	uint32_t dropped;
	uint32_t first;
	uint32_t span;

	zassert_true(CONFIG_EMBARCH_OUTPOST_THREAD_PRIORITY > PRODUCER_PRIORITY);
	zassert_equal(outpost_ring_slots() * OUTPOST_SLOT_BYTES,
		      CONFIG_EMBARCH_OUTPOST_RING_BYTES, "320 B is exactly 16 slots");

	/* Take whatever the drain thread has already accounted before we start. */
	(void)outpost_ring_take_gap(&dropped, &first, &span);

	k_thread_create(&producer, producer_stack, K_THREAD_STACK_SIZEOF(producer_stack),
			producer_fn, NULL, NULL, NULL, PRODUCER_PRIORITY, 0, K_NO_WAIT);
	zassert_ok(k_thread_join(&producer, K_MSEC(PRODUCER_DEADLINE_MS)),
		   "producer did not return: the drain thread never made room");

	zassert_false(outpost_ring_take_gap(&dropped, &first, &span),
		      "blocking mode dropped %u records from a thread", dropped);
}

ZTEST_SUITE(outpost_overflow_block, NULL, NULL, NULL, NULL, NULL);
