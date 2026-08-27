/*
 * Copyright (c) 2026 Intercreate / EmbArch
 * SPDX-License-Identifier: Apache-2.0
 */

/* An application that traces in one build and ships in another, compiled here
 * in the shipping configuration. Both lines below must compile with
 * CONFIG_EMBARCH_OUTPOST unset:
 *
 *   - the include, because library code that carries a marker has no way to
 *     know which build it is in;
 *   - the call, because wrapping every site in #ifdef is precisely what the
 *     header's no-op tier exists to spare the application.
 *
 * The marker name is deliberately one no OUTPOST_MARKERS() list registers.
 * With the module off, no registration header was included, so no ID exists
 * to check against and the name is unchecked *here* — see the third tier in
 * outpost.h. The check is real in both builds that have the module, which is
 * every build where a marker has a reason to exist.
 */

#include <embarch/outpost.h>

#include <zephyr/kernel.h>

int main(void)
{
	OUTPOST_EVT(A_NAME_NOTHING_REGISTERED, 42);
	return 0;
}
