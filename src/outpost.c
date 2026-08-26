/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Init, the drain thread, and the UART transport.
 *
 * design.md §3 decisions 3, 4, 5, 17 and §5.2.
 *
 * The drain thread reads k_uptime_get() to pace the header frame and nothing
 * else reads a clock in this module. That one is not a timestamp: it never
 * reaches the wire, and it runs in the drain thread rather than in the emit
 * path (design.md §3 decision 4).
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

/* Worst-case header body: type, seq, layout version, flags, and two
 * length-prefixed strings. A batch too small to hold one is a configuration
 * that silently produces an undecodable stream, so it fails to build instead.
 */
#define HEADER_MAX_BYTES (4 + 2 * (2 + CONFIG_EMBARCH_OUTPOST_BUILD_ID_MAX))
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
	if (uart_tx(outpost_uart, buf, len, SYS_FOREVER_US) != 0) {
		return;
	}
	/* The drain thread blocks here rather than double-buffering: records
	 * keep accumulating in the ring meanwhile, and a full ring is already a
	 * case with a defined, visible answer (a gap record). A second buffer
	 * would buy throughput at the cost of a second failure mode.
	 */
	k_sem_take(&tx_done, K_FOREVER);
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

	/* Layout 1 put sys_clock_hw_cycles_per_sec() here, to give the host a
	 * rate for the per-record cycle counts. Both are gone: there is no DUT
	 * clock in this module any more, and reporting a rate next to the host's
	 * own timestamps would only invite arithmetic across two clocks.
	 */
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

	if (outpost_ring_take_gap(&dropped)) {
		/* First record of this frame, always — that position is the
		 * whole of what a host has to bound the losses with now that no
		 * record carries a time (OUTPOST_KIND_GAP). It is emitted before
		 * the loop below so a full ring cannot push it to the back of
		 * the batch.
		 */
		rec.kind = OUTPOST_KIND_GAP;
		rec.a = dropped;
		rec.b = 0;
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

		size_t body_len = build_records_frame();

		if (body_len == 0) {
			k_sleep(K_MSEC(CONFIG_EMBARCH_OUTPOST_THREAD_WAIT_MS));
			continue;
		}
		send_body(batch_buf, body_len);
	}
}

K_THREAD_STACK_DEFINE(drain_stack, CONFIG_EMBARCH_OUTPOST_THREAD_STACK_SIZE);
static struct k_thread drain_thread;

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

	k_thread_create(&drain_thread, drain_stack, K_THREAD_STACK_SIZEOF(drain_stack),
			drain_thread_fn, NULL, NULL, NULL,
			CONFIG_EMBARCH_OUTPOST_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&drain_thread, "outpost");

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
