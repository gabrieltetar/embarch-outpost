/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief postcard encoding, COBS framing, and the frame CRC.
 *
 * The format itself is specified once, in outpost_priv.h. This file is only
 * the encoder; scripts/decode_outpost.py and embarch-core hold the decoders,
 * and all three are pinned against the same literal bytes by tests.
 */

#include "outpost_priv.h"

#include <string.h>

#include <zephyr/sys/crc.h>

size_t outpost_put_varint(uint8_t *buf, size_t cap, uint32_t value)
{
	size_t n = 0;

	do {
		if (n >= cap) {
			return 0;
		}
		uint8_t byte = (uint8_t)(value & 0x7Fu);

		value >>= 7;
		if (value != 0) {
			byte |= 0x80u;
		}
		buf[n++] = byte;
	} while (value != 0);

	return n;
}

size_t outpost_put_string(uint8_t *buf, size_t cap, const char *s)
{
	size_t len = (s != NULL) ? strlen(s) : 0;
	/* A zero-length string still costs its one length byte, so a 0 here
	 * only ever means "cap was 0".
	 */
	size_t n = outpost_put_varint(buf, cap, (uint32_t)len);

	if (n == 0 || n + len > cap) {
		return 0;
	}
	if (len > 0) {
		memcpy(&buf[n], s, len);
	}
	return n + len;
}

size_t outpost_put_record(uint8_t *buf, size_t cap, const struct outpost_slot *rec)
{
	size_t n = 0;
	size_t w;

	w = outpost_put_varint(&buf[n], cap - n, rec->cycles);
	if (w == 0) {
		return 0;
	}
	n += w;

	if (n >= cap) {
		return 0;
	}
	buf[n++] = rec->kind;

	w = outpost_put_varint(&buf[n], cap - n, rec->a);
	if (w == 0) {
		return 0;
	}
	n += w;

	w = outpost_put_varint(&buf[n], cap - n, rec->b);
	if (w == 0) {
		return 0;
	}
	n += w;

	return n;
}

/* Standard Consistent Overhead Byte Stuffing, byte-for-byte the same routine
 * embarch-dev-bench's serial_protocol.c uses, so Core sees one framing
 * convention on both of its links. The trailing 0x00 delimiter is appended by
 * outpost_frame(), once, not here.
 */
size_t outpost_cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
	size_t read_index = 0;
	size_t write_index = 1;
	size_t code_index = 0;
	uint8_t code = 1;

	while (read_index < len) {
		if (in[read_index] == 0) {
			out[code_index] = code;
			code = 1;
			code_index = write_index++;
			read_index++;
		} else {
			out[write_index++] = in[read_index++];
			code++;
			if (code == 0xFF) {
				out[code_index] = code;
				code = 1;
				code_index = write_index++;
			}
		}
	}
	out[code_index] = code;
	return write_index;
}

/* Static, not automatic, and the reason is the drain thread's stack.
 *
 * A batch-sized array here is CONFIG_EMBARCH_OUTPOST_BATCH_BYTES + 8 bytes of
 * frame, and it is the only large object on that thread: at the default 256/
 * 1024 it was a quarter of CONFIG_EMBARCH_OUTPOST_THREAD_STACK_SIZE, and it
 * scaled 1:1 with the batch size — so raising the batch to cut the tracer's own
 * frame rate silently walked the stack toward an overflow that reports as a
 * reboot with no fault dump.
 *
 * Static costs the same RAM without spending it on a stack that has to be sized
 * for the worst case anyway, and it is safe for the same reason batch_buf is:
 * only the drain thread ever reaches outpost_frame().
 */
static uint8_t sealed[CONFIG_EMBARCH_OUTPOST_BATCH_BYTES + 8];

size_t outpost_frame(const uint8_t *body, size_t body_len, uint8_t *out, size_t out_cap)
{
	/* The CRC covers the body only. Sealing before COBS rather than after
	 * means a host that finds a delimiter early still checks the same bytes
	 * the firmware sealed.
	 */
	uint32_t crc = crc32_ieee(body, body_len);
	size_t sealed_len = body_len + 4;

	if (sealed_len > sizeof(sealed)) {
		return 0;
	}
	/* COBS worst case, plus the delimiter. */
	if (sealed_len + (sealed_len / 254) + 2 + 1 > out_cap) {
		return 0;
	}

	memcpy(sealed, body, body_len);
	sealed[body_len + 0] = (uint8_t)(crc & 0xFFu);
	sealed[body_len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
	sealed[body_len + 2] = (uint8_t)((crc >> 16) & 0xFFu);
	sealed[body_len + 3] = (uint8_t)((crc >> 24) & 0xFFu);

	size_t n = outpost_cobs_encode(sealed, sealed_len, out);

	out[n] = 0x00;
	return n + 1;
}
