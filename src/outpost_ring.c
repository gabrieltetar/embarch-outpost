/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The record ring — lock-free, multi-producer, single-consumer.
 *
 * design.md §3 decisions 3 and 5.
 *
 * Producers are every thread and every ISR in the image; the consumer is the
 * one drain thread. The ring is a Vyukov-style bounded MPSC queue: a producer
 * CASes a monotonic `head` to reserve a slot, writes it, then publishes by
 * storing the reservation number into the slot's own `seq`. The consumer
 * advances `tail` only over slots whose `seq` equals the index it is reading,
 * so a producer preempted between reservation and publish stalls the drain at
 * that slot rather than letting a later record overtake an earlier one.
 *
 * Why not a spinlock. On a uniprocessor Cortex-M `k_spin_lock()` is an
 * irq_lock, and the critical section here is ~10 instructions — genuinely
 * cheap. It is not used because the emit path runs *inside interrupt context
 * at arbitrary priority*, and an irq_lock there raises the interrupt latency
 * of every other ISR in the system by the length of the trace write. A trace
 * whose cost is a latency floor on unrelated interrupts distorts exactly the
 * thing it is measuring. The CAS loop costs the producer, not the system.
 *
 * Overflow is drop-and-count, never overwrite-oldest: overwriting discards the
 * beginning of a busy burst and keeps the aftermath, which is backwards for a
 * continuous timeline. The account is handed to the drain thread as an
 * explicit gap record, because a trace that silently omits its own losses is
 * worse than one that admits them — the losses correlate with load, which is
 * exactly when the trace matters. Zephyr's own tracing increments a drop
 * counter that never reaches the wire; closing that gap is this module's.
 */

#include "outpost_priv.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* CONFIG_EMBARCH_OUTPOST_RING_BYTES rounded down to a power-of-two slot count.
 * OUTPOST_SLOT_BYTES is BUILD_ASSERTed against the real slot size in
 * outpost_priv.h, so this division cannot silently under-report the RAM the
 * ring takes the way it did while a slot was 20 bytes against a 16-byte
 * constant.
 * A power of two is what makes `index & MASK` correct across the 32-bit
 * wraparound of `head`/`tail`, which is what makes the whole scheme survive a
 * long run.
 */
#define RING_RAW_SLOTS (CONFIG_EMBARCH_OUTPOST_RING_BYTES / OUTPOST_SLOT_BYTES)

#if RING_RAW_SLOTS >= 8192
#define RING_SLOTS 8192
#elif RING_RAW_SLOTS >= 4096
#define RING_SLOTS 4096
#elif RING_RAW_SLOTS >= 2048
#define RING_SLOTS 2048
#elif RING_RAW_SLOTS >= 1024
#define RING_SLOTS 1024
#elif RING_RAW_SLOTS >= 512
#define RING_SLOTS 512
#elif RING_RAW_SLOTS >= 256
#define RING_SLOTS 256
#elif RING_RAW_SLOTS >= 128
#define RING_SLOTS 128
#elif RING_RAW_SLOTS >= 64
#define RING_SLOTS 64
#elif RING_RAW_SLOTS >= 32
#define RING_SLOTS 32
#else
#define RING_SLOTS 16
#endif

#define RING_MASK (RING_SLOTS - 1u)

static struct outpost_slot ring[RING_SLOTS];
static atomic_t ring_head;
static atomic_t ring_tail;

/* Drop account: a count, and nothing else.
 *
 * Layout 1 also kept the cycle stamps of the first and last loss, to bound the
 * span they fell across. There is no clock on this side any more (design.md §3
 * decision 4), so the bound is the host's to draw: a gap record is always the
 * first record of the next frame, which puts the losses between two arrival
 * stamps.
 */
static atomic_t gap_dropped;

/* A slot holding reservation `idx` publishes `idx + 1`, never `idx`.
 *
 * That +1 is what makes the whole ring correct straight out of BSS, with no
 * initialiser to run. Zero-initialised memory would otherwise read as
 * "reservation 0 is published" to a consumer sitting at tail 0, and it would
 * be wrong for exactly the window that matters: a producer that has CASed
 * head 0 -> 1 and been preempted before writing the slot. With the offset,
 * seq 0 is a value no reservation ever publishes.
 *
 * This matters more than it looks. The first hooks fire during kernel
 * startup, long before any SYS_INIT the outpost could register, so there is
 * no moment at which running an initialiser would be safe rather than a race
 * against live producers. The ring is correct before anyone has touched it.
 */
#define SEQ_FOR(idx) ((uint32_t)((idx) + 1u))

/* Not called during normal operation, and deliberately not from
 * outpost_init(): re-seeding a ring that hooks are already writing into is a
 * race, and BSS zero-init is already correct (see SEQ_FOR). This exists so a
 * test can start from a known state.
 */
void outpost_ring_init(void)
{
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		atomic_set(&ring[i].seq, 0);
	}
	atomic_set(&ring_head, 0);
	atomic_set(&ring_tail, 0);
	atomic_set(&gap_dropped, 0);
}

uint32_t outpost_ring_slots(void)
{
	return RING_SLOTS;
}

void outpost_ring_put(uint8_t kind, uint32_t a, uint32_t b)
{
	uint32_t head;

	for (;;) {
		head = (uint32_t)atomic_get(&ring_head);
		uint32_t tail = (uint32_t)atomic_get(&ring_tail);

		/* Wrapping subtraction: correct across the 32-bit rollover of
		 * both counters, which is the whole reason they are monotonic
		 * rather than already-masked indices.
		 */
		if ((head - tail) >= RING_SLOTS) {
#if defined(CONFIG_EMBARCH_OUTPOST_OVERFLOW_BLOCK)
			/* Blocking is a deliberate high-fidelity mode and it
			 * perturbs the timing being measured. It is not
			 * available in interrupt context, which can never
			 * block: those records still drop.
			 */
			if (!k_is_in_isr() && !k_is_pre_kernel()) {
				k_yield();
				continue;
			}
#endif
			atomic_inc(&gap_dropped);
			return;
		}

		if (atomic_cas(&ring_head, (atomic_val_t)head, (atomic_val_t)(head + 1))) {
			break;
		}
	}

	struct outpost_slot *slot = &ring[head & RING_MASK];

	slot->kind = kind;
	slot->a = a;
	slot->b = b;

	/* Publish last. Everything above must be visible to the consumer before
	 * the sequence that tells it to look.
	 */
	atomic_set(&slot->seq, (atomic_val_t)SEQ_FOR(head));
}

bool outpost_ring_get(struct outpost_slot *out)
{
	uint32_t tail = (uint32_t)atomic_get(&ring_tail);
	uint32_t head = (uint32_t)atomic_get(&ring_head);

	if (tail == head) {
		return false;
	}

	struct outpost_slot *slot = &ring[tail & RING_MASK];

	if ((uint32_t)atomic_get(&slot->seq) != SEQ_FOR(tail)) {
		/* Reserved but not yet published: a producer was preempted
		 * mid-write. Stall here rather than skipping it — out-of-order
		 * records would be worse than a few microseconds of latency.
		 */
		return false;
	}

	out->kind = slot->kind;
	out->a = slot->a;
	out->b = slot->b;

	atomic_set(&ring_tail, (atomic_val_t)(tail + 1));
	return true;
}

bool outpost_ring_take_gap(uint32_t *dropped)
{
	/* A single atomic read-and-clear: a drop landing mid-call is counted in
	 * the next gap record rather than being lost between the two.
	 */
	atomic_val_t count = atomic_clear(&gap_dropped);

	if (count == 0) {
		return false;
	}

	*dropped = (uint32_t)count;
	return true;
}
