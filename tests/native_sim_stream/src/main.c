/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * End-to-end test: put real bytes through a real UART device and decode them
 * on the host. Phase C's definition of done, minus the hardware.
 *
 * The outpost UART is native_sim's PTY UART bound to stdout, and the Zephyr
 * console is off, so the process's stdout *is* the trace stream and
 * `zephyr.exe > outpost.bin` captures it exactly as a serial capture would.
 *
 * The workload deliberately produces every record kind the module can emit:
 * two named threads ping-ponging (switch in/out), a k_timer ISR (isr
 * enter/exit), sleeps (idle), markers, and a tight burst sized to overflow the
 * ring so a gap record is exercised rather than assumed.
 */

#include <embarch/outpost.h>

#include <zephyr/kernel.h>
#include <posix_board_if.h>

#define STACK 1024
#define PRIO  5

static K_SEM_DEFINE(ping, 1, 1);
static K_SEM_DEFINE(pong, 0, 1);

static void ping_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (int i = 0; i < 200; i++) {
		k_sem_take(&ping, K_FOREVER);
		OUTPOST_EVT(WORK_BEGIN, i);
		k_busy_wait(50);
		OUTPOST_EVT(WORK_END, i);
		k_sem_give(&pong);
	}
}

static void pong_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (int i = 0; i < 200; i++) {
		k_sem_take(&pong, K_FOREVER);
		k_sleep(K_MSEC(1));
		k_sem_give(&ping);
	}
}

K_THREAD_DEFINE(outpost_ping, STACK, ping_fn, NULL, NULL, NULL, PRIO, 0, 200);
K_THREAD_DEFINE(outpost_pong, STACK, pong_fn, NULL, NULL, NULL, PRIO, 0, 200);

static void tick(struct k_timer *t)
{
	ARG_UNUSED(t);
	/* Runs in the timer ISR: markers are legal from interrupt context. */
	OUTPOST_EVT(BURST, 0);
}

static K_TIMER_DEFINE(ticker, tick, NULL);

int main(void)
{
	k_timer_start(&ticker, K_MSEC(5), K_MSEC(5));

	/* Let the ping-pong and the timer run. */
	k_sleep(K_MSEC(400));

	/* Now overflow the ring on purpose: far more markers back to back than
	 * the drain thread can possibly clear, so the stream carries a real
	 * gap record rather than a hypothetical one.
	 */
	for (int i = 0; i < 20000; i++) {
		OUTPOST_EVT(BURST, i);
	}

	/* Let the drain thread clear the ring and report the gap. */
	k_sleep(K_MSEC(400));

	/* main() returning does not end a native_sim run — the drain thread is
	 * still alive and the process would idle forever. Exit explicitly so
	 * the capture is a finite file with a defined end.
	 */
	posix_exit(0);
	return 0;
}
