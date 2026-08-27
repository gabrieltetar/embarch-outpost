/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Init, the drain thread, and the UART transport.
 *
 * design.md §3 decisions 3, 4, 5 and §5.2.
 *
 * The outpost owns its whole emit path. CONFIG_TRACING_USER does not select
 * TRACING_CORE (subsys/tracing/CMakeLists.txt, verified), so Zephyr's ring,
 * format layer, drain thread, backends and drop counter are all compiled out —
 * there is nothing here to reuse or to replace a backend of.
 *
 * The UART is a dedicated devicetree node the DUT repo declares as
 * `chosen { embarch,outpost-uart = ... }`, deliberately the same shape
 * Zephyr's own tracing backend uses for `zephyr,tracing-uart`. It is not the
 * console: Zephyr's stock backend writes with uart_poll_out() one byte at a
 * time (subsys/tracing/tracing_backend_uart.c, verified), and spinning per
 * byte inside the drain path burns exactly the CPU time the trace exists to
 * measure.
 */

#include "outpost_priv.h"
#include <embarch/outpost.h>
#include "outpost_build_id.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys_clock.h>

LOG_MODULE_REGISTER(embarch_outpost, CONFIG_EMBARCH_OUTPOST_LOG_LEVEL);

#if !defined(CONFIG_TRACING_USER)
#error "CONFIG_EMBARCH_OUTPOST needs CONFIG_TRACING_USER=y. It cannot select it: \
TRACING_USER is a Kconfig choice member, and neither select nor configdefault \
reaches one. Set both symbols in the same overlay."
#endif

#if !DT_HAS_CHOSEN(embarch_outpost_uart)
#error "CONFIG_EMBARCH_OUTPOST needs a chosen node: / { chosen { embarch,outpost-uart = &uartN; }; }"
#endif

#define OUTPOST_UART_NODE DT_CHOSEN(embarch_outpost_uart)

static const struct device *const outpost_uart = DEVICE_DT_GET(OUTPOST_UART_NODE);

#define BATCH_BYTES CONFIG_EMBARCH_OUTPOST_BATCH_BYTES
/* COBS worst case over (body + 4-byte CRC), plus the delimiter. */
#define FRAME_BYTES (BATCH_BYTES + 4 + ((BATCH_BYTES + 4) / 254) + 3)

/* How many records it takes to fill a batch, as a target for the fill wait
 * (design.md §3 decision 20). Deliberately computed against the *worst-case*
 * record size, so the target is reached before the batch could overflow and a
 * frame is never cut short by having waited for one record too many. On a real
 * trace a record averages well under the worst case, so a full batch holds more
 * than this and `build_records_frame()` simply takes them all.
 *
 * The 3 accounts for the frame type, seq and count bytes the body carries ahead
 * of the first record.
 */
#define BATCH_RECORDS_TARGET ((BATCH_BYTES - 3u) / OUTPOST_RECORD_MAX_BYTES)

static uint8_t batch_buf[BATCH_BYTES];
static uint8_t frame_buf[FRAME_BYTES];

static K_SEM_DEFINE(tx_done, 0, 1);

/* Holds the marker registration table in the image. Nothing at runtime reads
 * it — the manifest generator reads it out of the linked file — so
 * -fdata-sections --gc-sections deletes it, the build succeeds, and the
 * manifest comes out with no marker names: a trace whose markers are bare
 * integers, with no error anywhere.
 *
 * A `volatile` pointer read once at init, rather than a linker KEEP(), because
 * the KEEP has to survive two links on a native build and does not: the
 * snippet works on the Zephyr relocatable link and the host link that produces
 * zephyr.exe collects the section out again. Verified by building it both
 * ways. A relocation from a section the compiler may not elide is the one
 * mechanism that holds on every target.
 */
static const struct outpost_marker_def *volatile marker_table_keep = outpost_marker_table;

/* Worst-case header body: type, seq, layout version, flags, a 5-byte cycle
 * rate, and two length-prefixed strings. A batch too small to hold one is a
 * configuration that silently produces an undecodable stream, so it fails to
 * build instead.
 */
#define HEADER_MAX_BYTES (9 + 2 * (2 + CONFIG_EMBARCH_OUTPOST_BUILD_ID_MAX))
BUILD_ASSERT(BATCH_BYTES >= HEADER_MAX_BYTES,
	     "CONFIG_EMBARCH_OUTPOST_BATCH_BYTES cannot hold a header frame; "
	     "raise it or lower CONFIG_EMBARCH_OUTPOST_BUILD_ID_MAX");

/* ---- transport ---------------------------------------------------------- */

#if defined(CONFIG_EMBARCH_OUTPOST_UART_ASYNC)

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		k_sem_give(&tx_done);
		break;
	default:
		break;
	}
}

static int transport_init(void)
{
	return uart_callback_set(outpost_uart, uart_cb, NULL);
}

static void transport_send(const uint8_t *buf, size_t len)
{
	/* Discard any completion left over from a transfer that timed out below,
	 * so this send waits for its own UART_TX_DONE and not the previous
	 * frame's. Without it one timeout desynchronises the semaphore from the
	 * transfers for the rest of the boot: every later take returns on the
	 * stale give, and every later uart_tx() then lands on a driver that is
	 * still busy.
	 */
	k_sem_reset(&tx_done);

	if (uart_tx(outpost_uart, buf, len, SYS_FOREVER_US) != 0) {
		return;
	}

	/* The drain thread blocks here rather than double-buffering: records
	 * keep accumulating in the ring meanwhile, and a full ring is already a
	 * case with a defined, visible answer (a gap record). A second buffer
	 * would buy throughput at the cost of a second failure mode.
	 *
	 * Bounded, not K_FOREVER. There is no flow control on this link and
	 * uart_tx() was handed SYS_FOREVER_US, so the driver arms no timeout of
	 * its own — nothing but this take stands between a completion that never
	 * arrives and a drain thread parked for the rest of the boot. The
	 * failure is silent by construction: the trace simply stops, on a build
	 * that by design has no console to say so. Whether a stalled UARTE can
	 * happen here is the wrong question to answer with an unbounded wait.
	 */
	if (k_sem_take(&tx_done, K_MSEC(CONFIG_EMBARCH_OUTPOST_TX_TIMEOUT_MS)) == 0) {
		return;
	}

	/* Take the peripheral back and reap the UART_TX_ABORTED the abort
	 * raises, so the next frame starts from a clean transfer. If even that
	 * does not land, the k_sem_reset() above is what recovers the frame
	 * after it.
	 */
	(void)uart_tx_abort(outpost_uart);
	(void)k_sem_take(&tx_done, K_MSEC(CONFIG_EMBARCH_OUTPOST_TX_TIMEOUT_MS));
}

#else /* uart_poll_out fallback */

static int transport_init(void)
{
	return 0;
}

static void transport_send(const uint8_t *buf, size_t len)
{
	/* Per-byte and blocking. Present so a port whose driver has no async
	 * support still produces a stream, not because this is a good way to
	 * drain a trace — it is the exact cost decision 3 rejected Zephyr's
	 * stock UART backend over.
	 */
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(outpost_uart, buf[i]);
	}
}

#endif

static void send_body(const uint8_t *body, size_t body_len)
{
	size_t framed = outpost_frame(body, body_len, frame_buf, sizeof(frame_buf));

	if (framed == 0) {
		return;
	}
	transport_send(frame_buf, framed);
}

/* ---- frames ------------------------------------------------------------- */

static uint8_t frame_seq;

static uint8_t header_flags(void)
{
	uint8_t flags = 0;

#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_THREADS)
	flags |= OUTPOST_FLAG_TRACE_THREADS;
#endif
#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_ISRS)
	flags |= OUTPOST_FLAG_TRACE_ISRS;
#endif
#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_IDLE)
	flags |= OUTPOST_FLAG_TRACE_IDLE;
#endif
#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_MARKERS)
	flags |= OUTPOST_FLAG_TRACE_MARKERS;
#endif
#if defined(CONFIG_EMBARCH_OUTPOST_ISR_IDENTIFY)
	flags |= OUTPOST_FLAG_ISR_IDENTIFY;
#endif
#if defined(CONFIG_EMBARCH_OUTPOST_OVERFLOW_BLOCK)
	flags |= OUTPOST_FLAG_OVERFLOW_BLOCK;
#endif
#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_GPIO)
	flags |= OUTPOST_FLAG_TRACE_GPIO;
#endif
#if defined(CONFIG_EMBARCH_OUTPOST_TRACE_SELF)
	flags |= OUTPOST_FLAG_TRACE_SELF;
#endif
	return flags;
}

/* Reuses batch_buf: only the drain thread builds frames, and send_body() has
 * copied the body out before it returns, so the two never overlap.
 */
static void send_header(void)
{
	uint8_t *body = batch_buf;
	const size_t cap = sizeof(batch_buf);
	size_t n = 0;
	size_t w;

	body[n++] = OUTPOST_FRAME_HEADER;
	body[n++] = frame_seq++;
	body[n++] = OUTPOST_RECORD_LAYOUT_VERSION;
	body[n++] = header_flags();

	/* Read at runtime, never from CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC:
	 * that Kconfig legitimately defaults to 0 when the timer reads its own
	 * frequency at runtime (kernel/Kconfig), so a build-time rate would be
	 * silently zero on exactly those targets.
	 */
	w = outpost_put_varint(&body[n], cap - n, (uint32_t)sys_clock_hw_cycles_per_sec());
	if (w == 0) {
		return;
	}
	n += w;

	w = outpost_put_string(&body[n], cap - n, outpost_version());
	if (w == 0) {
		return;
	}
	n += w;

	w = outpost_put_string(&body[n], cap - n, outpost_build_id());
	if (w == 0) {
		return;
	}
	n += w;

	send_body(body, n);
}

/* Fills `batch_buf` with a records frame. Returns the body length, or 0 when
 * there was nothing to send.
 */
static size_t build_records_frame(void)
{
	/* postcard encodes a sequence as a varint count then the elements, and
	 * the count is not known until the batch is full. Reserving the widest
	 * varint would waste four bytes per frame on a count that never exceeds
	 * a couple of hundred; reserving one byte and capping the batch at 127
	 * records keeps the count a single byte, which is what this does.
	 */
	const size_t count_off = 2;
	size_t n = count_off + 1;
	uint32_t count = 0;
	struct outpost_slot rec;

	batch_buf[0] = OUTPOST_FRAME_RECORDS;
	batch_buf[1] = frame_seq;

	/* The gap record goes into the frame directly, never through the ring:
	 * the ring being full is the reason it exists, so it must not need
	 * space there to be reported.
	 */
	uint32_t dropped;
	uint32_t first;
	uint32_t span;

	if (outpost_ring_take_gap(&dropped, &first, &span)) {
		/* Stamped when the losses started, not when the drain thread
		 * got around to reporting them. That makes a gap record the one
		 * record whose cycles can precede the records printed after it,
		 * which is stated on OUTPOST_KIND_GAP and handled host-side.
		 */
		rec.cycles = first;
		rec.kind = OUTPOST_KIND_GAP;
		rec.a = dropped;
		rec.b = span;
		size_t w = outpost_put_record(&batch_buf[n], sizeof(batch_buf) - n, &rec);

		if (w > 0) {
			n += w;
			count++;
		}
	}

	while (count < 127 && (sizeof(batch_buf) - n) >= OUTPOST_RECORD_MAX_BYTES) {
		if (!outpost_ring_get(&rec)) {
			break;
		}
		size_t w = outpost_put_record(&batch_buf[n], sizeof(batch_buf) - n, &rec);

		if (w == 0) {
			break;
		}
		n += w;
		count++;
	}

	if (count == 0) {
		return 0;
	}

	batch_buf[count_off] = (uint8_t)count;
	frame_seq++;
	return n;
}

/* ---- drain thread ------------------------------------------------------- */

static void drain_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

#if CONFIG_EMBARCH_OUTPOST_HEADER_INTERVAL_MS > 0
	int64_t last_header = k_uptime_get();
#endif

	send_header();

	for (;;) {
#if CONFIG_EMBARCH_OUTPOST_HEADER_INTERVAL_MS > 0
		int64_t now = k_uptime_get();

		if ((now - last_header) >= CONFIG_EMBARCH_OUTPOST_HEADER_INTERVAL_MS) {
			last_header = now;
			send_header();
		}
#endif

#if CONFIG_EMBARCH_OUTPOST_FILL_WAIT_MS > 0
		/* Let the batch fill before paying for a frame (design.md §3
		 * decision 20).
		 *
		 * Without this the loop is a fixed point at the link rate: it
		 * sends, blocks on the transmit, and then drains exactly the
		 * records that arrived while it was blocked. Measured at
		 * 460800 on a real DUT that settled at 3.3 records per frame
		 * against a 256-byte batch, so ~9 bytes of framing rode on ~32
		 * bytes of records and a fifth of the link carried nothing.
		 *
		 * One sleep, not a loop: two waits would double the latency
		 * bound to buy a second-order saving, and a bounded wait that
		 * is obviously bounded is worth more here than a full frame.
		 */
		if (outpost_ring_pending() < BATCH_RECORDS_TARGET) {
			k_sleep(K_MSEC(CONFIG_EMBARCH_OUTPOST_FILL_WAIT_MS));
		}
#endif

		size_t body_len = build_records_frame();

		if (body_len == 0) {
			k_sleep(K_MSEC(CONFIG_EMBARCH_OUTPOST_THREAD_WAIT_MS));
			continue;
		}
		send_body(batch_buf, body_len);
	}
}

K_THREAD_STACK_DEFINE(drain_stack, CONFIG_EMBARCH_OUTPOST_THREAD_STACK_SIZE);

/* Not `static`, and named for what it is rather than for where it lives: the
 * trace hooks compare `k_current_get()` against `&outpost_drain_thread` to
 * keep the instrument out of its own trace (design.md §3 decision 19), and a
 * file-static cannot be reached from outpost_hooks.c. The declaration and the
 * reasoning are in outpost_priv.h.
 *
 * A second, unlooked-for benefit of the name: this object is a plain
 * `struct k_thread`, so the manifest generator's exact-address symbol match
 * resolves it to `outpost_drain_thread` in the trace instead of to a bare
 * pointer -- which is the one lane an engineer reading an outpost trace is
 * most likely to want named. It was `drain_thread` when it was static, and
 * that would have resolved too; the rename is so a symbol in someone else's
 * ELF says whose it is.
 */
struct k_thread outpost_drain_thread;

/* ---- init --------------------------------------------------------------- */

static int outpost_init(void)
{
	/* The ring is deliberately not initialised here — it is correct from
	 * BSS by construction, and the trace hooks have been filling it since
	 * the kernel's first context switch, long before this runs.
	 */
	if (!device_is_ready(outpost_uart)) {
		LOG_ERR("outpost UART %s not ready; no trace will be emitted",
			outpost_uart->name);
		return -ENODEV;
	}

#if CONFIG_EMBARCH_OUTPOST_BAUD > 0
	{
		struct uart_config cfg;
		int err = uart_config_get(outpost_uart, &cfg);

		if (err == 0) {
			cfg.baudrate = CONFIG_EMBARCH_OUTPOST_BAUD;
			err = uart_configure(outpost_uart, &cfg);
		}
		if (err != 0) {
			LOG_WRN("could not set outpost baud to %d (%d); "
				"leaving the port as devicetree configured it",
				CONFIG_EMBARCH_OUTPOST_BAUD, err);
		}
	}
#endif

	(void)marker_table_keep;

	int err = transport_init();

	if (err != 0) {
		LOG_ERR("outpost UART transport init failed (%d)", err);
		return err;
	}

	k_thread_create(&outpost_drain_thread, drain_stack, K_THREAD_STACK_SIZEOF(drain_stack),
			drain_thread_fn, NULL, NULL, NULL,
			CONFIG_EMBARCH_OUTPOST_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&outpost_drain_thread, "outpost");

	LOG_INF("outpost up on %s: %u slots, %u-byte batches, build %s", outpost_uart->name,
		outpost_ring_slots(), (unsigned int)BATCH_BYTES, outpost_build_id());
	return 0;
}

/* APPLICATION level, last: the trace should be running before application
 * threads start, and the UART driver must already be initialised.
 */
SYS_INIT(outpost_init, APPLICATION, 0);

const char *outpost_build_id(void)
{
	return EMBARCH_OUTPOST_BUILD_ID;
}

const char *outpost_version(void)
{
	return EMBARCH_OUTPOST_VERSION;
}
