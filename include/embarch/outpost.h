/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief embarch-outpost public surface: marker registration and OUTPOST_EVT.
 *
 * Design: ../embarch-doc/embarch-outpost/design.md §3 decisions 6 and 9.
 *
 * An application declares its markers once, as an X-macro list, in a header
 * named by CONFIG_EMBARCH_OUTPOST_MARKER_HEADER:
 *
 *     #define OUTPOST_MARKERS(X)  \
 *         X(PPG_FRAME_BEGIN)      \
 *         X(PPG_FRAME_END)
 *
 * That single declaration is simultaneously what makes OUTPOST_EVT() compile
 * and what puts the name in outpost-manifest.json. An unregistered ID does not
 * reach the host as a mystery integer — it fails to compile, because the
 * enumerator it expands to does not exist.
 */

#ifndef EMBARCH_OUTPOST_H_
#define EMBARCH_OUTPOST_H_

#include <stdint.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pull in the application's registration list, if it declared one. Kconfig
 * strings arrive quoted, which is exactly what #include wants — but an unset
 * one arrives as "", which #include cannot take, and no preprocessor test
 * distinguishes the two. So CMakeLists.txt defines OUTPOST_HAS_MARKER_HEADER
 * only when the string is non-empty, and the decision is made where the value
 * is actually inspectable.
 */
#ifdef OUTPOST_HAS_MARKER_HEADER
#include CONFIG_EMBARCH_OUTPOST_MARKER_HEADER
#endif

#ifndef OUTPOST_MARKERS
/** No markers registered in this image. */
#define OUTPOST_MARKERS(X)
#endif

/**
 * @brief Marker IDs, generated from the application's OUTPOST_MARKERS list.
 *
 * IDs are positional and therefore stable only within one build — which is
 * precisely why the manifest is CRC/build-ID matched against the running
 * firmware before anything is decoded (design.md §3 decision 9).
 */
enum outpost_marker_id {
#define _OUTPOST_MARKER_ENUM(name) OUTPOST_MARKER_##name,
	OUTPOST_MARKERS(_OUTPOST_MARKER_ENUM)
#undef _OUTPOST_MARKER_ENUM
	OUTPOST_MARKER_COUNT
};

/**
 * @brief One row of the marker table the manifest generator reads out of the ELF.
 *
 * The names live in the image, never on the wire.
 */
struct outpost_marker_def {
	uint32_t id;
	const char *name;
};

/** Read by scripts/gen_outpost_manifest.py out of the linked image, and by
 *  nothing at runtime. Terminated by a {0, NULL} sentinel row.
 */
extern const struct outpost_marker_def outpost_marker_table[];

#if defined(CONFIG_EMBARCH_OUTPOST) && defined(CONFIG_EMBARCH_OUTPOST_TRACE_MARKERS)

/** Emit one marker record. Safe from any context, including an ISR. */
void outpost_marker(uint32_t id, uint32_t arg);

/**
 * @brief Mark an application-defined event.
 *
 * @param id  A bare name from this image's OUTPOST_MARKERS list — not a
 *            string, not a number. An unregistered name is a build error.
 * @param arg One 32-bit payload, whatever the engineer chose it to mean.
 */
#define OUTPOST_EVT(id, arg) outpost_marker(OUTPOST_MARKER_##id, (uint32_t)(arg))

#else

/* Markers compiled out. The ID must still resolve, so an unregistered name is
 * a build error whether or not markers are enabled — the failure mode this is
 * arranged to prevent does not depend on a Kconfig.
 */
#define OUTPOST_EVT(id, arg)                                                                       \
	do {                                                                                       \
		(void)OUTPOST_MARKER_##id;                                                         \
		(void)(arg);                                                                       \
	} while (0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* EMBARCH_OUTPOST_H_ */
