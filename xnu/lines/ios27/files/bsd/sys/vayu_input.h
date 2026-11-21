/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU, K7 (touch): what the touch driver (bsd/dev/vayu_nvt.c), its bus (vayu_qup_spi.c) and the bridge into iOS's
 * HID (bsd/dev/vayu_input.c -> SweetHID's digitizer) say to each other. A header of the port's own: Scion's
 * sys/hakios.input.h stays Scion's (its HAKIOS_TOUCH_* numbers are the same as these).
 */
#ifndef _SYS_VAYU_INPUT_H_
#define _SYS_VAYU_INPUT_H_

#if defined(KERNEL_PRIVATE) || defined(XNU_KERNEL_PRIVATE)

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

__BEGIN_DECLS

#define VAYU_TOUCH_DOWN         1       /* a finger has landed */
#define VAYU_TOUCH_MOVE         2
#define VAYU_TOUCH_UP           3       /* it has lifted (x, y: where it was last) */
#define VAYU_BUTTON_DOWN        4       /* finger = 1 lock, 2 volume up, 3 volume down, 4 home */
#define VAYU_BUTTON_UP          5
#define VAYU_TOUCH_FINGERS      10

#define VAYU_PANEL_W            1080    /* the panel's pixels: SweetHID's and SweetFramebuffer's */
#define VAYU_PANEL_H            2400

/*
 * One touch sample: x and y 0..65535 over the panel (0 = left/top, 65535 = right/bottom), as the 8796 line's
 * hakios_touch_post_at() took them; time in mach_absolute_time() units (0: now). Producers may be any thread;
 * the bridge serializes them and hands the panel's pixels to SweetHID. Not from interrupt context.
 */
void vayu_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time);
/* how long no finger has been down (absolute time); 0: one is down now, ~0: never touched */
uint64_t vayu_touch_idle_abs(void);
/* a touch driver that came up says so (its name, the panel size its firmware reports) */
void vayu_input_touch_source(const char *name, uint32_t width, uint32_t height);

/*
 * A firmware file of the running root, DIR/NAME (the 8796 line's hakios_image_file("fw", NAME) read the RAM disks; on
 * 12377 the root is a file system, the RAM root or the one kern.hakios_switch_root made): waited for up to
 * wait_s seconds (the root may still be switched), read whole into memory that stays (the driver keeps using it), at
 * most 8 MB. NULL: not there.
 */
const uint8_t *vayu_input_fw(const char *dir, const char *name, uint64_t *size, uint32_t wait_s);

/* bsd/dev/vayu_nvt.c, bsd/dev/vayu_qup_spi.c */
void vayu_nvt_init(void);
void hakios_nvt_stats(uint64_t *fresh, uint64_t *repeats, uint64_t *max_gap_abs);
bool vayu_qup_spi_init(void);
bool vayu_qup_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length);
void vayu_qup_spi_fini(void);
bool vayu_qup_nvt_resx(bool high);

/* iokit/Kernel/arm/hid/SweetHID.cpp */
void sweet_hid_touch(int finger, int down, int x, int y);
void sweet_hid_key(int key, int down);
bool sweet_hid_touch_ready(void);
uint64_t sweet_hid_touch_events(void);

__END_DECLS

#endif /* KERNEL_PRIVATE || XNU_KERNEL_PRIVATE */
#endif /* _SYS_VAYU_INPUT_H_ */
