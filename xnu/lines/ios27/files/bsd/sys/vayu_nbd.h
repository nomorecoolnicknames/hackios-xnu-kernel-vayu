/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * A disk of the computer at the other end of the USB cable (or of QEMU's host) as /dev/nbd0, over
 * the network block device protocol, and a second one of another export as /dev/nbd1. Only with vayu27=1
 * and boot-arg nbd=1 (and nbd1=1 for the second). See bsd/dev/vayu_nbd.c.
 * Origin: the macOS line (6a96ea3) bsd/sys/vayu_nbd.h (the 8796 line).
 */
#ifndef _SYS_VAYU_NBD_H_
#define _SYS_VAYU_NBD_H_

#include <stdbool.h>
#include <sys/types.h>

void vayu_nbd_init(void);
bool vayu_nbd_dev(dev_t dev);           /* /dev/nbd0, nbd1 (a request costs a round trip: worth putting together) */

#endif /* _SYS_VAYU_NBD_H_ */
