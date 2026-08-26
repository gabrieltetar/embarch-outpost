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

#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_THREADS)

void sys_trace_thread_switched_in_user(void)
{
	outpost_ring_put(OUTPOST_KIND_THREAD_SWITCH_IN, (uint32_t)(uintptr_t)k_current_get(), 0);
}

void sys_trace_thread_switched_out_user(void)
{
	outpost_ring_put(OUTPOST_KIND_THREAD_SWITCH_OUT, (uint32_t)(uintptr_t)k_current_get(), 0);
}

/* Thread create and name-set carry the pointer only. The name itself never
 * crosses the wire: the manifest resolves _k_thread_obj_* symbol addresses out
 * of the ELF at zero wire cost (decision 8). A thread created into a plain
 * `static struct k_thread` has no distinguishing symbol and renders as a raw
 * pointer — stated in the design rather than papered over with runtime
 * name-registration records, which would put strings back on the wire.
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
	outpost_ring_put(OUTPOST_KIND_ISR_ENTER, active_vector(), 0);
}

void sys_trace_isr_exit_user(void)
{
	outpost_ring_put(OUTPOST_KIND_ISR_EXIT, active_vector(), 0);
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
