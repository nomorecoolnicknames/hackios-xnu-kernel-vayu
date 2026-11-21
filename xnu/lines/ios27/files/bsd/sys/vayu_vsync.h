/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/sys/vayu_vsync.h; K5 (xnu-12377): started by its VAYU_HOOK.
 */
/*
 * The display controller's timing engine, bsd/dev/vayu_vsync.c: where the panel's refresh is
 * really counted. Step 1 only reads; see that file for the facts and the boot-arg.
 */
#ifndef _SYS_VAYU_VSYNC_H_
#define _SYS_VAYU_VSYNC_H_

#include <stdint.h>

/* vayu27=1 vayu_vsync=1: a look at INTF 1/2; =2: and the panel's timing is published (hakios_vsync_next/frame) */
uint64_t vayu_vsync_next(uint64_t now); /* the absolute time of the panel's next frame start; 0 until step 2 armed */
uint64_t vayu_vsync_frame(void);        /* one frame in absolute-time units; 0 until step 2 armed */

#endif /* _SYS_VAYU_VSYNC_H_ */
