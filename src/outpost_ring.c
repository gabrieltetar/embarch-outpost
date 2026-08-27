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
 * That argument is only worth anything if the whole emit path honours it, and
 * for a while this one did not: the timestamp on the first line of
 * outpost_ring_put() used to be k_cycle_get_32(), which on nRF disables
 * interrupts twice. See outpost_time.h.
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
#include "outpost_time.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* CONFIG_EMBARCH_OUTPOST_RING_BYTES rounded down to a power-of-two slot count.
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

/* Drop account. `gap_dropped` is the authoritative count; the two cycle
 * stamps bound the span it was lost across. `gap_first` is only written by
 * the producer that takes the count from 0 to 1, so the span starts at the
 * first loss rather than the last.
 */
static atomic_t gap_dropped;
static uint32_t gap_first;
static uint32_t gap_last;

/* seq 0 means "reserved, but the producer has not finished writing the slot".
 *
 * A slot holding reservation `idx` therefore publishes `idx + 1`, never `idx`,
 * and that +1 is what makes the whole ring correct straight out of BSS with no
 * initialiser to run. Zero-initialised memory would otherwise read as
 * "reservation 0 is published" to a consumer sitting at tail 0, and it would be
 * wrong for exactly the window that matters: a producer that has CASed head
 * 0 -> 1 and been preempted before writing the slot.
 *
 * This matters more than it looks. The first hooks fire during kernel startup,
 * long before any SYS_INIT the outpost could register, so there is no moment at
 * which running an initialiser would be safe rather than a race against live
 * producers. The ring is correct before anyone has touched it.
 */
#define SEQ_UNPUBLISHED 0u

/* The one reservation the +1 alone gets wrong, and it fails permanently rather
 * than transiently: idx == UINT32_MAX publishes UINT32_MAX + 1 == 0, which is
 * the sentinel. The consumer parked at that tail reads its own published record
 * as unpublished, stalls there forever, and every record after it drops for the
 * life of the boot — the stream dies silently at one exact point in 2^32.
 *
 * So that single value publishes SEQ_WRAPPED instead. It aliases seq_for(0),
 * and the alias is harmless because those two reservations are adjacent and
 * never share a slot: UINT32_MAX & RING_MASK is RING_MASK, 0 & RING_MASK is 0,
 * and RING_SLOTS >= 2 keeps them distinct. Nothing else can collide, because
 * every other seq is still its own reservation number plus one.
 */
#define SEQ_WRAPPED (SEQ_UNPUBLISHED + 1u)

BUILD_ASSERT(RING_SLOTS >= 2, "the seq wraparound alias needs at least two slots");

static inline uint32_t seq_for(uint32_t idx)
{
	uint32_t seq = idx + 1u;

	return (seq == SEQ_UNPUBLISHED) ? SEQ_WRAPPED : seq;
}

/* Not called during normal operation, and deliberately not from
 * outpost_init(): re-seeding a ring that hooks are already writing into is a
 * race, and BSS zero-init is already correct (see SEQ_UNPUBLISHED). This exists
 * so a test can start from a known state.
 *
 * `start` seeds head and tail. A test needs it to reach the 32-bit wraparound
 * of the reservation counter in finitely many puts; nothing else has any
 * business calling it with a non-zero value.
 */
void outpost_ring_init_at(uint32_t start)
{
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		atomic_set(&ring[i].seq, (atomic_val_t)SEQ_UNPUBLISHED);
	}
	atomic_set(&ring_head, (atomic_val_t)start);
	atomic_set(&ring_tail, (atomic_val_t)start);
	atomic_set(&gap_dropped, 0);
	gap_first = 0;
	gap_last = 0;
}

void outpost_ring_init(void)
{
	outpost_ring_init_at(0);
}

uint32_t outpost_ring_slots(void)
{
	return RING_SLOTS;
}

static void note_drop(uint32_t cycles)
{
	atomic_val_t before = atomic_inc(&gap_dropped);

	if (before == 0) {
		gap_first = cycles;
	}
	gap_last = cycles;
}

void outpost_ring_put(uint8_t kind, uint32_t a, uint32_t b)
{
	uint32_t cycles = outpost_cycles();
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
			note_drop(cycles);
			return;
		}

		if (atomic_cas(&ring_head, (atomic_val_t)head, (atomic_val_t)(head + 1))) {
			break;
		}
	}

	struct outpost_slot *slot = &ring[head & RING_MASK];

	slot->cycles = cycles;
	slot->kind = kind;
	slot->a = a;
	slot->b = b;

	/* Publish last. Everything above must be visible to the consumer before
	 * the sequence that tells it to look.
	 */
	atomic_set(&slot->seq, (atomic_val_t)seq_for(head));
}

bool outpost_ring_get(struct outpost_slot *out)
{
	uint32_t tail = (uint32_t)atomic_get(&ring_tail);
	uint32_t head = (uint32_t)atomic_get(&ring_head);

	if (tail == head) {
		return false;
	}

	struct outpost_slot *slot = &ring[tail & RING_MASK];

	if ((uint32_t)atomic_get(&slot->seq) != seq_for(tail)) {
		/* Reserved but not yet published: a producer was preempted
		 * mid-write. Stall here rather than skipping it — out-of-order
		 * records would be worse than a few microseconds of latency.
		 */
		return false;
	}

	out->cycles = slot->cycles;
	out->kind = slot->kind;
	out->a = slot->a;
	out->b = slot->b;

	atomic_set(&ring_tail, (atomic_val_t)(tail + 1));
	return true;
}

uint32_t outpost_ring_pending(void)
{
	/* head - tail in reservation numbers, which is unsigned-correct across
	 * the 32-bit wraparound the whole ring is built to survive. It can read
	 * *above* RING_SLOTS: a producer that has reserved past a full ring
	 * increments head and then drops its record, so head can run ahead of
	 * what any consumer will ever see. Clamped, because the only caller uses
	 * this to size a batch and a count larger than the ring would size it
	 * for records that are not there.
	 *
	 * A hint, not a fact, and only ever used as one: it can be stale by the
	 * time the caller reads it (a hook can fire in between) and it can count
	 * a slot that is reserved but not yet published. Both errors are in the
	 * direction of "the drain thread waits slightly less than it meant to",
	 * which costs nothing — `build_records_frame()` still takes whatever is
	 * actually published and no more.
	 */
	uint32_t pending = (uint32_t)atomic_get(&ring_head) - (uint32_t)atomic_get(&ring_tail);

	return (pending > RING_SLOTS) ? RING_SLOTS : pending;
}

bool outpost_ring_take_gap(uint32_t *dropped, uint32_t *first_cycles, uint32_t *cycle_span)
{
	/* Read the bounds before clearing the count, so a drop landing
	 * mid-call is counted in the next gap record rather than producing a
	 * span that starts after it. The span is a bound on where the losses
	 * were, not an exact measure of them, and is documented as such.
	 */
	uint32_t first = gap_first;
	uint32_t last = gap_last;
	atomic_val_t count = atomic_clear(&gap_dropped);

	if (count == 0) {
		return false;
	}

	*dropped = (uint32_t)count;
	*first_cycles = first;
	*cycle_span = last - first;
	return true;
}
