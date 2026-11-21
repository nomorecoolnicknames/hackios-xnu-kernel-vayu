/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (POCO X3 Pro): where the vayu drivers start (sys/vayu_12377_abi.h). bsd_init.c calls vayu_hooks_run() at its two
 * fixed points; every driver's VAYU_HOOK() record is in one section, and runs from here in link order. Touches no
 * hardware itself. Owner: K0 (the integrator).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>
#include <pexpert/pexpert.h>

extern const struct vayu_hook vayu_hooks_start[] __SECTION_START_SYM(VAYU_HOOK_SEGMENT, VAYU_HOOK_SECTION);
extern const struct vayu_hook vayu_hooks_end[] __SECTION_END_SYM(VAYU_HOOK_SEGMENT, VAYU_HOOK_SECTION);

/* The section exists even with no driver in it: bsd_init.c's call then finds nothing to run. */
static __used __attribute__((section(VAYU_HOOK_SEGMENT "," VAYU_HOOK_SECTION)))
const struct vayu_hook __vayu_hook_none = { 0, "none", NULL };

bool
vayu_arg(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

bool
vayu27_enabled(void)
{
	static int known = -1;

	if (known < 0) {
		known = vayu_arg("vayu27") ? 1 : 0;
	}
	return known == 1;
}

void
vayu_hooks_run(enum vayu_phase phase)
{
	const char *what = phase == VAYU_PHASE_BEFORE_ROOT ? "before root" : "after devfs";
	unsigned int ran = 0;

	if (!vayu27_enabled()) {
		printf("VAYU: %s: no hook runs without vayu27=1\n", what);
		return;
	}
	for (const struct vayu_hook *h = vayu_hooks_start; h < vayu_hooks_end; h++) {
		if (h->vh_phase != (uint32_t)phase || h->vh_fn == NULL) {
			continue;
		}
		printf("VAYU: %s: %s\n", what, h->vh_name);
		h->vh_fn();
		ran++;
	}
	printf("VAYU: %s: %u hook(s), ABI %d, vayu27=%d\n", what, ran, VAYU_12377_ABI_VERSION, vayu27_enabled());
}
