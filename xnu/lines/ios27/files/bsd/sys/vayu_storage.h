/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU, K6 (storage and the USB network): what the ports' files say to each other, and the one order they start
 * in. A narrow header of this port's own, so that Scion's sys/hakios.input.h (which the 8796 line's storage drivers
 * included for a few prototypes) stays Scion's.
 *
 * The order (bsd/dev/vayu_storage.c, one VAYU_HOOK per phase -- not the link order of several):
 *   before the root   the UFS mailbox's block devices, read only (ufs=2), for rd=ufsroot0
 *   after devfs       /dev/ufs_* nodes; the USB controller and en2 (vayu_usbnet=, usb_handover=1); QEMU's test NIC
 *                     (qemu_net=1); the NBD disks (nbd=1), which wait for their interface by themselves; vayu-ctl
 *                     (usbctl=1)
 * Every step returns at once without its own boot argument; none runs without vayu27=1.
 */
#ifndef _SYS_VAYU_STORAGE_H_
#define _SYS_VAYU_STORAGE_H_

#if defined(KERNEL_PRIVATE) || defined(XNU_KERNEL_PRIVATE)

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__BEGIN_DECLS

/*
 * bsd/dev/vayu_ufs.c, the storage through the loader (ufs=2): its devfs nodes (the step after devfs; the majors and the
 * table are registered before the root, for rd=ufsroot0), and for the kernel's own users -- the 8796 line's names, read
 * only: a partition by its GPT name, and bytes of it.
 */
void vayu_ufs_init(void);
bool vayu_ufs_dev(dev_t dev);
bool hakios_ufs_part(const char *name, uint32_t *part, uint64_t *bytes);
int hakios_ufs_part_read(uint32_t part, uint64_t offset, void *dst, uint64_t len);

/* bsd/dev/vayu_qemu_net.c: QEMU's virtio NIC as en0, a test device (qemu_net=1, QEMU's virt machine only) */
void vayu_qemu_net_init(void);

/*
 * misc's bootloader message (BCB) through the UFS mailbox (bsd/dev/vayu_ufs.c), for the USB command port. 0, or an
 * errno: ENODEV without the UFS driver or its table, EPERM for a write without bcb_reboot=1.
 *   get     the command field ("boot-recovery" or "")
 *   fields  the status and recovery fields, and how many bytes of the block past the command are not zero
 *   set     the command set to "boot-recovery" (true) or cleared (false), and read back
 */
int vayu_ufs_bcb_get(char *out, size_t size);
int vayu_ufs_bcb_fields(char *status, size_t ssize, char *recovery, size_t rsize, uint32_t *dirty);
int vayu_ufs_bcb_set(bool recovery);

__END_DECLS

#endif /* KERNEL_PRIVATE || XNU_KERNEL_PRIVATE */
#endif /* _SYS_VAYU_STORAGE_H_ */
