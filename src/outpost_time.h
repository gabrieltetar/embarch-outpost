/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The record timestamp source.
 *
 * design.md §3 decisions 3 and 4.
 *
 * outpost_cycles() is read once per record, by every thread and every ISR in
 * the image. It is the hottest line in the module, and on nRF it was also the
 * only line in the emit path that disabled interrupts.
 *
 * k_cycle_get_32() looks like the portable answer and is the wrong one here.
 * On a GRTC-timed nRF it resolves to sys_clock_cycle_get_32()
 * (zephyr/drivers/timer/nrf_grtc_timer.c), which takes k_spin_lock() — an
 * irq_lock on a uniprocessor Cortex-M — around nrfx_grtc_syscounter_get(),
 * which takes NRFX_CRITICAL_SECTION_ENTER(), a *second* irq_lock, around a
 * 64-bit SYSCOUNTER read in a retry loop (nrfx haly/nrfy_grtc.h).
 *
 * So every traced event globally disabled interrupts, twice, from inside ISRs
 * at arbitrary priority — reintroducing one line above outpost_ring.c exactly
 * the cost its lock-free CAS design exists to avoid. A trace whose emit path
 * is an interrupt-latency floor on unrelated ISRs distorts the thing it is
 * measuring.
 *
 * The record field is a uint32, and the GRTC SYSCOUNTER's low word is exactly
 * what sys_clock_cycle_get_32() extracts from all of that apparatus. Reading
 * SYSCOUNTERL directly is one load from one register: same value, same units,
 * same phase, and therefore the same header cycles_per_sec — with no lock and
 * no retry.
 *
 * SYSCOUNTERL is valid on its own. The BUSY/OVERFLOW bits the nrfy retry loop
 * spins on live in SYSCOUNTERH, and they exist to detect that L wrapped
 * *between* a paired L-then-H read. A reader that wants only the low word has
 * no such pair and nothing to retry.
 */

#ifndef EMBARCH_OUTPOST_TIME_H_
#define EMBARCH_OUTPOST_TIME_H_

#include <stdint.h>

#include <zephyr/kernel.h>

#if defined(CONFIG_NRF_GRTC_TIMER)
#include <hal/nrf_grtc.h>
#endif

/**
 * @brief Read the record timestamp. Any context, including an ISR.
 *
 * @return The same 32-bit count k_cycle_get_32() would return, at
 *         sys_clock_hw_cycles_per_sec() ticks per second.
 */
static inline uint32_t outpost_cycles(void)
{
#if defined(CONFIG_NRF_GRTC_TIMER)
	/* Gated on the *timer driver*, not on the SoC. CONFIG_NRF_GRTC_TIMER is
	 * what makes the GRTC SYSCOUNTER the kernel's clock, and therefore what
	 * makes this low word and k_cycle_get_32() the same number under the
	 * same cycles_per_sec. On a build where some other timer drives the
	 * kernel, reading the GRTC would put a second, unrelated time base on
	 * the wire beneath a header that describes the first.
	 */
	return nrf_grtc_sys_counter_low_get(NRF_GRTC);
#else
	/* Every other target. Whatever the platform's cycle counter costs is
	 * what a record costs there; this file is where that gets fixed when a
	 * platform turns out to make it expensive, and the fix is per-platform
	 * because the cheap read always is.
	 */
	return k_cycle_get_32();
#endif
}

#endif /* EMBARCH_OUTPOST_TIME_H_ */
