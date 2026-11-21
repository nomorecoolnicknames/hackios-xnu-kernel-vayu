/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/sys/vayu_mdp.h; K5 (xnu-12377).
 */
/*
 * The display controller as UEFI left it (vayu), bsd/dev/vayu_mdp.c: vayu27=1 vayu_mdp=1, a look, reading only;
 * vayu_mdp=2: and the zero-copy scanout of SweetFramebuffer's frames.
 */
#ifndef _SYS_VAYU_MDP_H_
#define _SYS_VAYU_MDP_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * vayu27=1 vayu_mdp=2 (SweetFramebuffer's fb_zero=1): VIG0 fetches a frame at phys (contiguous, below 4 GB, 64-byte
 * aligned) with a row of stride bytes, cropped at (x, y) to the panel; true once the hardware has taken it (CTL0 flush
 * done). false: not allowed (argument, platform, VIG0/CTL0 not as UEFI left them) or not taken: copy instead.
 */
bool vayu_mdp_scanout(uint64_t phys, uint32_t stride, uint32_t x, uint32_t y);
bool vayu_mdp_unscanout(void);          /* the boot framebuffer back on VIG0 (its stride, offset); true when taken */
void vayu_mdp_quiesce(void);            /* the same on the way to the loader (registered with loader_quiesce_register) */

/*
 * K8 (after ak8-g2, 21.11: vayu_mdp=2 fb_zero=1 with the GPU's frame hung the whole phone, no log): vayu_crumb=1
 * (default 0) -- a line straight into the loader's log and the log written back to RAM (loader_log_sync), as K5's
 * WLED steps, so that the recovery reading the RAM after a hang (loader_live "XNULIVE!") shows the last step begun.
 * "VAYU CRUMB: ...". Any context (interrupts off too); nothing on the way blocks. vayu_crumb=2: and 250 ms of sleep after
 * each where sleeping is allowed, for the live log on 5140 to send it first (ak8-g2b: a hang left RAM unread).
 */
void vayu_crumb(const char *format, ...) __printflike(1, 2);
int vayu_crumb_level(void);             /* 0, 1 or 2: what vayu_crumb= says */

/* the SM8150's MMIO is there: the loader's device tree is the phone's (/arm-io device_type "vayu"), not QEMU's */
bool vayu_display_hw_present(void);

#endif /* _SYS_VAYU_MDP_H_ */
