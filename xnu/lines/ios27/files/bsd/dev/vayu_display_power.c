/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/vayu_display_power.c (the 8796 line, same authors). Ported to xnu-12377 by
 * K5: called by SweetFramebuffer (RequestPowerChange, iokit/Kernel/arm/SweetFramebuffer.cpp) instead of
 * MiniIOMobileFramebuffer; nothing is mapped or written without vayu27=1 and display_off_intf=1, and only on the phone
 * (/arm-io "vayu"); B's tee_printf gone (printf reaches the loader's log through the console); a timing engine stopped
 * by us is started again on the way to the loader (loader_quiesce_register).
 */
/*
 * The screen off beyond its backlight (POCO X3 Pro, vayu, SM8150), bsd/dev/vayu_display_power.c.
 *
 * iOS locks and powers the display down the native way: backboardd asks the framebuffer for it
 * (MiniIOMobileFramebuffer, RequestPowerChange 0), and with vayu_wled=2 the PM8150L's WLED goes
 * off (vayu_wled.c). The power button brings it back: hakios.buttons.c lights the backlight
 * (hakios_display_power(true)) and hands iOS its Lock press, and iOS asks for the display again.
 * That much only darkens the panel. Behind it the display controller still scans the panel out of
 * memory 120 times a second (VIG0 -> LM0 -> DSC0 -> INTF1 -> DSI0, as vayu_mdp.c found UEFI's
 * video mode): 1080 x 2400 x 4 bytes x 120, about 1.2 GB/s of DRAM read for a picture nobody
 * sees; and the touch panel is still read touch_hz (240) times a second.
 *
 * Boot-args, each 0 by default (then nothing here does anything):
 *
 *   display_off_intf=1  while iOS has the display off, INTF1's timing engine is stopped: INTF1 is at
 *                       0x0ae6b800 (sm8150-sde.dtsi:17,89-92: mdp 0x0ae00000 + 0x6b800; vayu_vsync.c
 *                       reads its frame counter there), INTF_TIMING_ENGINE_EN at +0x000, 1 running,
 *                       0 stopped (mainline dpu_hw_intf.c: INTF_TIMING_ENGINE_EN 0x000,
 *                       dpu_hw_intf_enable_timing_engine()). No frame is fetched or sent then. It is
 *                       stopped only if it reads 1 (UEFI's state, as the boot left it) and only once
 *                       the backlight is off; it is started again before the backlight comes back,
 *                       which then waits two frames (17 ms at 120 Hz) so that the first light is a
 *                       whole picture. The frame counter (+0x0ac) is read before the stop and after
 *                       the start: the line says whether frames run again. Nothing else of the
 *                       display controller is written: CTL, LM, DSC and the DSI link stay as they are.
 *                       HYPOTHESIS until a run shows the panel's picture back: a video-mode panel
 *                       without its stream for a while (NT36672C here) is expected to follow the
 *                       stream again from the next frame start; if it does not, the next boot (or
 *                       the command port's reboot) gives it UEFI's sequence again.
 *   touch_off_hz=N      while the display is off, the touch panel is looked at N times a second
 *                       (1..60) instead of touch_hz (vayu_nvt.c asks vayu_display_touch_hz()); from the
 *                       moment the display is lit again (the button lights it first), touch_hz again.
 *
 * Not here (yet): the panel's own sleep (DCS 28h display off, 10h sleep in, through DSI0's command
 * DMA engine, and 11h/29h with the panel's 120 ms on the way back -- the j20s node's ON and POST-OFF
 * command sets in recovery-live.dts:3065-3074 are the sequence), DSI ULPS, and the MDP's own clocks
 * and GDSC (dispcc): each is a step of its own after this one is seen to come back.
 *
 * Lines: "display-power: off: INTF1 stopped (frame N)", "display-power: on: INTF1 running again
 * (frames N -> M)" or "... INTF1 DOES NOT COUNT FRAMES after the start".
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_display_power.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_loader.h>
#include <sys/vayu_mdp.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void IOSleep(unsigned int milliseconds);


#define INTF1_PHYS              0x0ae6b800UL    /* mdp 0x0ae00000 + 0x6b800 (sm8150-sde.dtsi:17,89-92) */
#define INTF_MAP_SIZE           0x1000UL
#define INTF_TIMING_ENGINE_EN   0x000           /* dpu_hw_intf.c */
#define INTF_FRAME_COUNT        0x0ac
#define INTF_FRAME_MS           9               /* one frame at 120 Hz is 8.33 ms */

static LCK_GRP_DECLARE(display_power_group, "vayu-display-power");
static LCK_MTX_DECLARE(display_power_lock, &display_power_group);

static uint32_t intf_off, touch_off_hz;
static bool configured;
static vm_offset_t intf;
static bool intf_stopped;                       /* by us: started again before the light comes back */
static volatile bool display_off;               /* iOS has the display off (and the button has not lit it) */

static void
configure(void)
{
	if (configured) {
		return;
	}
	configured = true;
	PE_parse_boot_argn("display_off_intf", &intf_off, sizeof(intf_off));
	PE_parse_boot_argn("touch_off_hz", &touch_off_hz, sizeof(touch_off_hz));
	if (touch_off_hz > 60) {
		touch_off_hz = 60;
	}
	if (intf_off != 0 && (!vayu27_enabled() || !vayu_display_hw_present())) {
		printf("display-power: display_off_intf=1 needs vayu27=1 and the phone: left off\n");
		intf_off = 0;
	}
	if (intf_off != 0) {
		intf = ml_io_map(INTF1_PHYS, INTF_MAP_SIZE);
		if (intf == 0 || loader_quiesce_register("display_power", vayu_display_power_quiesce) != 0) {
			printf("display-power: INTF1 not mapped or no quiesce slot: display_off_intf left off\n");
			intf_off = 0;
		}
	}
	if (intf_off != 0 || touch_off_hz != 0) {
		printf("display-power: while iOS has the display off:%s%s%s\n", intf_off != 0 ? " INTF1's timing engine stopped" : "",
		    intf_off != 0 && touch_off_hz != 0 ? ";" : "", touch_off_hz != 0 ? " the touch panel read touch_off_hz times a second" : "");
	}
}

static inline uint32_t
intf_read(uint32_t reg)
{
	return *(volatile uint32_t *)(intf + reg);
}

static inline void
intf_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(intf + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* With the lock held: the timing engine back on if we stopped it; true if it was stopped. */
static bool
intf_start(void)
{
	uint32_t before, after;

	if (!intf_stopped) {
		return false;
	}
	before = intf_read(INTF_FRAME_COUNT);
	intf_write(INTF_TIMING_ENGINE_EN, 1);
	intf_stopped = false;
	IOSleep(2 * INTF_FRAME_MS);
	after = intf_read(INTF_FRAME_COUNT);
	if (after != before) {
		printf("display-power: on: INTF1 running again (frames %u -> %u)\n", before, after);
	} else {
		printf("display-power: on: INTF1 DOES NOT COUNT FRAMES after the start (frame %u, enable reads %#x)\n", after,
		    intf_read(INTF_TIMING_ENGINE_EN));
	}
	return true;
}

/*
 * iOS's display power, from the framebuffer's RequestPowerChange, after the backlight has been
 * switched (MiniIOMobileFramebuffer.cpp).
 */
void
vayu_display_power_state(bool on)
{
	lck_mtx_lock(&display_power_lock);
	configure();
	if (on) {
		(void)intf_start();
		os_atomic_store(&display_off, false, release);
	} else {
		os_atomic_store(&display_off, true, release);
		if (intf_off != 0 && intf != 0 && !intf_stopped) {
			uint32_t enabled = intf_read(INTF_TIMING_ENGINE_EN), frame = intf_read(INTF_FRAME_COUNT);

			if (enabled & 1) {
				intf_write(INTF_TIMING_ENGINE_EN, 0);
				intf_stopped = true;
				printf("display-power: off: INTF1 stopped (frame %u)\n", frame);
			} else {
				printf("display-power: off: INTF1's timing engine reads %#x, not running: left alone\n", enabled);
			}
		}
	}
	lck_mtx_unlock(&display_power_lock);
}

/* Before the backlight is lit, by whoever lights it (vayu_wled_power(true): iOS, the power button). */
void
vayu_display_power_wake(void)
{
	if (!os_atomic_load(&display_off, acquire) && !intf_stopped) {
		return;                 /* (the usual case: a brightness change on a lit screen) */
	}
	lck_mtx_lock(&display_power_lock);
	configure();
	(void)intf_start();
	os_atomic_store(&display_off, false, release);
	lck_mtx_unlock(&display_power_lock);
}

/* vayu_nvt.c: how many looks a second the touch panel gets now; 0: its own rate. */
uint32_t
vayu_display_touch_hz(void)
{
	return os_atomic_load(&display_off, relaxed) ? touch_off_hz : 0;
}

/* The display as iOS left it, for the power line (hakios.battery.c). */
bool
vayu_display_is_off(void)
{
	return os_atomic_load(&display_off, relaxed);
}

/* On the way to the loader (interrupts off): a timing engine we stopped runs again, no waiting. */
void
vayu_display_power_quiesce(void)
{
	if (intf_stopped && intf != 0) {
		*(volatile uint32_t *)(intf + INTF_TIMING_ENGINE_EN) = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
		intf_stopped = false;
	}
}
