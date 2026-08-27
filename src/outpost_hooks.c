/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Zephyr TRACING_USER hook implementations.
 *
 * design.md §3 decisions 2 and 7.
 *
 * subsys/tracing/user/tracing_user.c declares every sys_trace_*_user() as
 * __weak and empty; defining a strong one here is the entire integration
 * contract. Nothing patches Zephyr, and nothing here is called unless the
 * kernel itself calls it.
 *
 * These run in the hottest paths in the system — inside the context switch and
 * inside _isr_wrapper(). Each one reads the cycle counter, writes one slot,
 * and returns.
 */

#include "outpost_priv.h"
#include <embarch/outpost.h>

#include <zephyr/kernel.h>
#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_GPIO)
/* Before <tracing_user.h>, matching gpio_utils.h's own order. That header
 * forward-declares `struct gpio_callback` and typedefs the gpio scalar types
 * itself rather than including gpio.h, so both definitions land in any
 * translation unit that needs the struct laid out — which is every GPIO driver
 * in the tree already, and is why the duplicate typedefs are fine.
 */
#include <zephyr/drivers/gpio.h>
#endif
#include <tracing_user.h>

#if defined(CONFIG_EMBARCH_OUTPOST_ISR_IDENTIFY)
#include <cmsis_core.h>

/* The identical expression arch/arm/core/cortex_m/isr_wrapper.c evaluates
 * three lines after calling sys_trace_isr_enter(), at the same instant, in the
 * same exception context. Not a heuristic that usually works — the number the
 * wrapper is about to dispatch on. IPSR still names the same exception at
 * sys_trace_isr_exit(), so nesting and tail-chaining fall out of the record
 * stream instead of having to be reconstructed by the host.
 */
static inline uint32_t active_vector(void)
{
	return (uint32_t)(__get_IPSR() - 16u);
}
#else
static inline uint32_t active_vector(void)
{
	return OUTPOST_IRQ_UNKNOWN;
}
#endif

/* ---- self-exclusion (design.md §3 decision 19) --------------------------
 *
 * Two predicates, both compiled out entirely when
 * CONFIG_EMBARCH_OUTPOST_TRACE_SELF=y, so the honest-but-expensive setting
 * costs nothing on the hot path either.
 *
 * The measurement that put these here: on a quiet nRF54L15 the steady state
 * was a closed loop of ten records per frame, all ten caused by transmitting
 * the previous frame, and 50.4% of the reference capture's records were the
 * drain thread plus its own UART's ISR.
 */
#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_SELF)
static inline bool is_self_thread(struct k_thread *t)
{
	ARG_UNUSED(t);
	return false;
}
static inline bool is_self_vector(uint32_t v)
{
	ARG_UNUSED(v);
	return false;
}
#else
static inline bool is_self_thread(struct k_thread *t)
{
	return t == &outpost_drain_thread;
}
static inline bool is_self_vector(uint32_t v)
{
	/* OUTPOST_SELF_IRQ is OUTPOST_IRQ_UNKNOWN on a build that cannot know
	 * the number, and an ISR record carrying OUTPOST_IRQ_UNKNOWN means
	 * "this build could not name the active vector" -- so the guard is
	 * needed to stop excluding every anonymous ISR on such a build, which
	 * would silently drop the one class of record that says the identity
	 * was unavailable.
	 */
	return OUTPOST_SELF_IRQ != OUTPOST_IRQ_UNKNOWN && v == OUTPOST_SELF_IRQ;
}
#endif

#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_THREADS)

void sys_trace_thread_switched_in_user(void)
{
	struct k_thread *self = k_current_get();

	if (is_self_thread(self)) {
		return;
	}
	outpost_ring_put(OUTPOST_KIND_THREAD_SWITCH_IN, (uint32_t)(uintptr_t)self, 0);
}

void sys_trace_thread_switched_out_user(void)
{
	/* `_current` is still the OUTGOING thread here -- verified in the
	 * Zephyr tree, see outpost_priv.h's note on outpost_drain_thread --
	 * so the same comparison excludes both ends of the drain thread's run.
	 */
	struct k_thread *self = k_current_get();

	if (is_self_thread(self)) {
		return;
	}
	outpost_ring_put(OUTPOST_KIND_THREAD_SWITCH_OUT, (uint32_t)(uintptr_t)self, 0);
}

/* Thread create and name-set carry the pointer only. The name itself never
 * crosses the wire: the manifest resolves _k_thread_obj_* symbol addresses out
 * of the ELF at zero wire cost (decision 8). A thread created into a plain
 * `static struct k_thread` has no distinguishing symbol and renders as a raw
 * pointer — stated in the design rather than papered over with runtime
 * name-registration records, which would put strings back on the wire.
 */
/* Create and name-set are NOT self-excluded, deliberately. They are two
 * records for the whole capture rather than two per frame, so they cost
 * nothing on the wire — and between them they are the only thing that names
 * the excluded subject. A self-excluded trace that also hid the drain thread's
 * existence would leave a host with unattributed intervals and nothing to
 * attribute them to.
 */
void sys_trace_thread_create_user(struct k_thread *thread)
{
	outpost_ring_put(OUTPOST_KIND_THREAD_CREATE, (uint32_t)(uintptr_t)thread, 0);
}

void sys_trace_thread_name_set_user(struct k_thread *thread)
{
	outpost_ring_put(OUTPOST_KIND_THREAD_NAME, (uint32_t)(uintptr_t)thread, 0);
}

#endif /* CONFIG_EMBARCH_OUTPOST_TRACE_THREADS */

#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_ISRS)

void sys_trace_isr_enter_user(void)
{
	uint32_t vector = active_vector();

	if (is_self_vector(vector)) {
		return;
	}
	outpost_ring_put(OUTPOST_KIND_ISR_ENTER, vector, 0);
}

void sys_trace_isr_exit_user(void)
{
	uint32_t vector = active_vector();

	if (is_self_vector(vector)) {
		return;
	}
	outpost_ring_put(OUTPOST_KIND_ISR_EXIT, vector, 0);
}

#endif /* CONFIG_EMBARCH_OUTPOST_TRACE_ISRS */

#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_IDLE)

/* Idle entry only. Zephyr calls sys_trace_idle_exit() but routes it to
 * CONFIG_CPU_LOAD alone — there is no sys_trace_idle_exit_user() to define
 * (subsys/tracing/user/tracing_user.c, verified). Idle exit is recovered
 * host-side from the next thread-switch-in, which is what actually ends the
 * idle span.
 */
void sys_trace_idle_user(void)
{
	outpost_ring_put(OUTPOST_KIND_IDLE, 0, 0);
}

#endif /* CONFIG_EMBARCH_OUTPOST_TRACE_IDLE */

#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_GPIO)

/* The one hook family Zephyr instruments outside the kernel, and the reason it
 * is worth having: a GPIO interrupt already shows up as an ISR_ENTER/ISR_EXIT
 * pair on the GPIOTE vector, but that pair says nothing about *which* of the
 * callbacks registered on that port ran inside it, or for how long. On a board
 * where one GPIOTE line carries a sensor's data-ready, that is the whole
 * question.
 *
 * These two calls are already in the image whenever CONFIG_TRACING is on —
 * gpio_fire_callbacks() makes them unconditionally, and without this file they
 * land in the empty __weak definitions in subsys/tracing/user/tracing_user.c.
 * Defining them costs the records, not the calls.
 */
void sys_trace_gpio_fire_callbacks_enter_user(sys_slist_t *list, const struct device *port,
					      gpio_pin_t pins)
{
	ARG_UNUSED(list);
	/* `pins` is deliberately not recorded — Zephyr's own hook signature has
	 * already truncated it to 8 bits. See OUTPOST_KIND_GPIO_DISPATCH.
	 */
	ARG_UNUSED(pins);
	outpost_ring_put(OUTPOST_KIND_GPIO_DISPATCH, (uint32_t)(uintptr_t)port, 0);
}

/* The handler pointer, not the `struct gpio_callback *`. Both identify the
 * callback, but only one of them reliably has a name: a handler is a function
 * and always resolves against the ELF's symbol table, whereas the callback
 * struct is usually a member of some driver's private data singleton and has
 * no symbol of its own. Decision 8's rule — names come out of the image, never
 * off the wire — only pays if the pointer that crosses the wire is the one the
 * image can name.
 */
void sys_trace_gpio_fire_callback_user(const struct device *port, struct gpio_callback *callback)
{
	ARG_UNUSED(port);
	outpost_ring_put(OUTPOST_KIND_GPIO_CALLBACK_DONE, (uint32_t)(uintptr_t)callback->handler,
			 (uint32_t)callback->pin_mask);
}

#endif /* CONFIG_EMBARCH_OUTPOST_TRACE_GPIO */

#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_MARKERS)

void outpost_marker(uint32_t id, uint32_t arg)
{
	/* Unreachable through OUTPOST_EVT(), which cannot name an unregistered
	 * marker at all — this guards a direct call with a computed ID, and
	 * OUTPOST_MARKER_COUNT is a compile-time constant, so it costs a
	 * compare against an immediate on the hot path.
	 */
	if (id >= (uint32_t)OUTPOST_MARKER_COUNT) {
		return;
	}
	outpost_ring_put(OUTPOST_KIND_MARKER, id, arg);
}

#endif /* CONFIG_EMBARCH_OUTPOST_TRACE_MARKERS */
