/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU, K6: where storage and the USB network start (sys/vayu_storage.h). One hook per phase, so that the order
 * of the steps is this file's and not the link order of their files (sys/vayu_12377_abi.h: within a phase hooks
 * run in link order). The UFS block devices themselves are registered before the root by bsd/dev/vayu_ufs.c's own
 * hook (rd=ufsroot0 needs them then); their /dev nodes come here, first. The NBD disks after the interfaces: their thread waits for nbd_if= anyway, but an interface
 * that exists before it looks costs it nothing.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_ctl.h>
#include <sys/vayu_nbd.h>
#include <sys/vayu_storage.h>
#include <sys/vayu_usbnet.h>

static void
vayu_storage_after_devfs(void)
{
	vayu_ufs_init();
	vayu_usbnet_init();
	vayu_qemu_net_init();
	vayu_nbd_init();
	vayu_ctl_init();
}

VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, storage, vayu_storage_after_devfs);
