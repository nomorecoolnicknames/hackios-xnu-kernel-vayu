/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * The contract between the VAYU board (POCO X3 Pro, SM8150) of this xnu-12377 tree and the vayu drivers brought over
 * from the 8796 line (the iOS 16.5 line (xnu/ of the repository)). One header, owned by the integrator (K0), so that the drivers can be ported
 * in parallel without touching the same files of the kernel (PORTING.md at the top of the tree).
 *
 * 1. Where a driver starts. No driver adds a call to bsd/kern/bsd_init.c, iokit/bsddev/IOKitBSDInit.cpp or the
 *    platform expert. It declares itself with VAYU_HOOK(phase, name, fn) in its own file, and bsd_init.c runs every
 *    hook of a phase at one fixed point (vayu_hooks_run(), bsd/dev/vayu_board.c):
 *      VAYU_PHASE_BEFORE_ROOT  after vfsinit() and the MAC policies, before the root is mounted: block devices that a
 *                              root can be on (UFS, the NBD over USB), and nothing that needs /dev
 *      VAYU_PHASE_AFTER_DEVFS  /dev is mounted, the root is there: input, display, audio, network, radio, sensors
 *    Within a phase hooks run in link order (bsd/conf/files): a hook does not depend on another hook of its phase.
 *    Earlier than BEFORE_ROOT is XNU's own STARTUP() (kern/startup.h): the machine layer (GIC, timer, PSCI, the
 *    loader's log) uses that, not this.
 *    A root on the UFS: rd=ufsroot0 asks sweet_ufs_rootdev() (iokit/bsddev/IOKitBSDInit.cpp), which the UFS port
 *    defines (bsd/dev/vayu_stubs.c says ENODEV until then).
 *
 * 2. What turns a driver on. Every hook is off unless the boot arguments say otherwise: vayu27=1 for the new 12377 paths
 *    as a whole (plans/ios27-macos: every new path behind vayu27=1; without it vayu_hooks_run() runs no hook at all),
 *    and then the driver's own argument, with the
 *    names and defaults the 8796 line has (touchscreen=1, nvt_qup=1, gpu=1 vayu_gpu=1, ufs=2 ...). vayu_arg() reads
 *    one; a hook returns at once when its argument is not there. Nothing that writes a PMIC, a pin or the flash runs
 *    without an argument of its own.
 *
 * 3. Memory. 16K pages (pexpert/pexpert/arm64/VAYU.h): PAGE_SIZE is 16384 and an ml_io_map() of a 4K register block
 *    maps the whole 16K granule around it; check the neighbours of every block that is mapped. Physically contiguous
 *    DMA memory is kmem_alloc_contig(), which on this base is declared in vm/vm_kern_xnu.h, not vm/vm_kern.h: include
 *    <sys/vayu_dma_compat.h> for it and for the cache maintenance the drivers use (K3 owns that header).
 *
 * 4. Logs. printf()/kprintf() as on 8796; the line starts with the driver's name in capitals ("VAYU NVT: ...") so that
 *    the loader's ring and the QEMU serial log read the same.
 */
#ifndef _SYS_VAYU_12377_ABI_H_
#define _SYS_VAYU_12377_ABI_H_

#if defined(KERNEL_PRIVATE) || defined(XNU_KERNEL_PRIVATE)

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <libkern/section_keywords.h>

__BEGIN_DECLS

/* Bumped when a phase, the hook record or a rule above changes meaning. */
#define VAYU_12377_ABI_VERSION          1

enum vayu_phase {
	VAYU_PHASE_BEFORE_ROOT = 1,
	VAYU_PHASE_AFTER_DEVFS = 2,
};

struct vayu_hook {
	uint32_t        vh_phase;       /* enum vayu_phase */
	const char     *vh_name;        /* "nvt", "ufs", ...: the log names it */
	void          (*vh_fn)(void);
};

#define VAYU_HOOK_SEGMENT               "__DATA_CONST"
#define VAYU_HOOK_SECTION               "__vayu_hooks"

/* VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, nvt, vayu_nvt_start): one per driver entry point, at file scope */
#define VAYU_HOOK(phase, name, fn) \
	static __used __attribute__((section(VAYU_HOOK_SEGMENT "," VAYU_HOOK_SECTION))) \
	const struct vayu_hook __vayu_hook_ ## name = { (phase), #name, (fn) }

/* bsd/kern/bsd_init.c: every hook of the phase, in link order (bsd/dev/vayu_board.c) */
void vayu_hooks_run(enum vayu_phase phase);

/* Is the boot argument there and not 0? (vayu27=1, touchscreen=1, ...) */
bool vayu_arg(const char *name);
/* The new 12377 paths as a whole: vayu27=1 (read once) */
bool vayu27_enabled(void);

/* The UFS root (rd=ufsroot0, iokit/bsddev/IOKitBSDInit.cpp); the UFS port defines it */
int sweet_ufs_rootdev(dev_t *dev);

__END_DECLS

#endif /* KERNEL_PRIVATE || XNU_KERNEL_PRIVATE */
#endif /* _SYS_VAYU_12377_ABI_H_ */
