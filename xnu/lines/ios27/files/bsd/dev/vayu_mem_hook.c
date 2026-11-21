/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (K3): what the memory rules of this kernel report, once the console is up and before the root (vayu27=1; the
 * hook runner runs nothing without it). New work of this tree.
 *
 *   vayu_wcomb_safe=1   which RAM WCOMB/RT mappings may be Normal NC, and what was made Device (osfmk/arm/pmap/pmap.c)
 *   vayu_mem_diag=1     the window, the aperture, the extra RAM, the build's memory defines, and a test of the DMA
 *                       calls below and above 4 GiB: "VAYU MEM: diag PASS" or FAIL (osfmk/arm64/vayu_mem.c)
 */
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>

extern bool vayu_wcomb_safe;            /* osfmk/arm/pmap/pmap.c */
void vayu_wcomb_report(void);
void vayu_mem_diag_run(void);

static void
vayu_mem_start(void)
{
	if (vayu_arg("vayu_mem_diag")) {
		vayu_mem_diag_run();            /* (the wcomb_safe lines among its own) */
	} else if (vayu_wcomb_safe) {
		vayu_wcomb_report();
	}
}

VAYU_HOOK(VAYU_PHASE_BEFORE_ROOT, mem, vayu_mem_start);
