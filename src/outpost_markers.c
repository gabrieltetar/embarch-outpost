/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief The marker table the manifest generator reads out of the ELF.
 *
 * design.md §3 decision 6. The application's one OUTPOST_MARKERS(X)
 * declaration produces both the enumerators OUTPOST_EVT() needs and this
 * table. Names live in the image so the generator can resolve them; they never
 * reach the wire.
 *
 * **Nothing in the firmware reads this table**, which makes keeping it in the
 * image a real problem rather than a formality: `-fdata-sections
 * --gc-sections` deletes it, the build still succeeds, and the manifest comes
 * out with no marker names at all — a trace whose markers are bare integers,
 * with no error anywhere to say why. outpost.c holds it down with a volatile
 * reference; see the comment there for why that and not a linker KEEP().
 */

#include <embarch/outpost.h>

#include <zephyr/sys/util.h>

/* The parameter is _n, not `name`: a parameter called `name` would also expand
 * inside the designated initialiser `.name =`, which the compiler then reads
 * as a member that does not exist.
 */
#define _OUTPOST_MARKER_ROW(_n)                                                                    \
	{                                                                                          \
		.id = OUTPOST_MARKER_##_n, .name = #_n                                             \
	},

/* The trailing {0, NULL} is a sentinel, not padding. It keeps this a valid
 * array in an image that registers no markers at all, and it is how the
 * generator finds the end: the table's own symbol size gives the row count,
 * and the sentinel is the row it stops at.
 */
const struct outpost_marker_def outpost_marker_table[] = {
	OUTPOST_MARKERS(_OUTPOST_MARKER_ROW){.id = 0, .name = NULL},
};

#undef _OUTPOST_MARKER_ROW
