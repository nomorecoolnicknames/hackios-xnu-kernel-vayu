/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU, K7: the touch screen's samples into iOS's HID (sys/vayu_input.h). Scion's SweetHID publishes the
 * digitizer (SweetHIDTouch, an IOHIDEventService under IOResources, iokit/Kernel/arm/hid/SweetHID.cpp) that
 * backboardd takes the touches of; it wants panel pixels through sweet_hid_touch(). The vayu drivers of the 8796
 * line posted 0..65535 to hakios_touch_post_at() (bsd/dev/hakios.input.c, its /dev/hakios-touch queue and a
 * backboardd plugin of its own); here the same samples go straight to the native service, and nothing else.
 *
 *  - one lock serializes the producers (the touch thread, vayu-ctl's synthetic input), so the collections SweetHID
 *    dispatches are in the order the samples came;
 *  - 0..65535 -> pixels: (v * (W - 1) + 32767) / 65535, so 0 and 65535 are the first and the last pixel and the
 *    centre stays the centre; SweetHID maps the panel onto what SweetFramebuffer presents (the 1125x2436 canvas, or
 *    the panel itself with sweetfb_native=1) -- no crop or flip here (touch_flip= is the driver's, once);
 *  - SweetHID may start after the first sample (IOKit matching is not ordered with the VAYU hooks): until it is
 *    there the fingers' state is kept and nothing is dispatched; on the first sample after it, the fingers held
 *    down are given to it first (a DOWN each), so it never sees a MOVE or an UP without its DOWN;
 *  - vayu-ctl's touch/swipe/key (the loader's "oem touch") are registered here (vayu_ctl_set_input) and take the
 *    same path; buttons go to sweet_hid_key();
 *  - touch_log=1: every DOWN and UP, and one MOVE in 16, into the log (the live log on 5140 with usbnet_log=1);
 *    without it the first 16 events only. A line a minute either way: the counts.
 *
 * Boot-args (vayu27=1 for the hook): touchscreen=1 nvt_qup=1 (bsd/dev/vayu_nvt.c); vayu_hidtest=1: no hardware,
 * a synthetic DOWN / MOVE / MOVE (the same place: nothing) / UP of finger 0 and two fingers down and up, once SweetHID
 * is there, its dispatched collections counted -- "VAYU HIDTEST: PASS" when there are the 7 expected (QEMU); and the
 * firmware reader once on /fw/j20s_novatek_ts_fw02.bin of the root, if it is there.
 *
 * The K7 hook after devfs is this file's, one for all of it (sys/vayu_12377_abi.h: no order between hooks).
 */
/* (the kernel builds with warnings on and not as errors: these two are this file's errors -- run ak7-t2) */
#pragma clang diagnostic error "-Wuninitialized"
#pragma clang diagnostic error "-Wsometimes-uninitialized"
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/crc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_ctl.h>
#include <sys/vayu_input.h>

extern void IOSleep(unsigned int milliseconds);

static LCK_GRP_DECLARE(vin_lck_grp, "vayu-input");
static LCK_MTX_DECLARE(vin_lock, &vin_lck_grp);

static struct {
	bool down;
	int px, py;
} vin_finger[VAYU_TOUCH_FINGERS];
static bool vin_ready;                  /* SweetHID was there at the last sample */
static bool vin_log;                    /* touch_log=1 */
static uint64_t vin_idle_since;         /* absolute time the last finger lifted; 0: never touched */
static uint32_t vin_held;               /* fingers down now */
static struct {
	uint64_t downs, moves, ups, keys, bad, early, resynced, logged;
} vin_count;
static const char *vin_source;
static uint32_t vin_src_w, vin_src_h;

static int
vin_pixel(uint32_t v, int size)
{
	if (v > 65535) {
		v = 65535;
	}
	return (int)(((uint64_t)v * (uint64_t)(size - 1) + 32767) / 65535);
}

/* the fingers held down, to a SweetHID that has just appeared (vin_lock held) */
static void
vin_resync(void)
{
	for (int f = 0; f < VAYU_TOUCH_FINGERS; f++) {
		if (vin_finger[f].down) {
			sweet_hid_touch(f, 1, vin_finger[f].px, vin_finger[f].py);
			vin_count.resynced++;
		}
	}
}

void
vayu_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time)
{
	bool log = false;
	int px, py;

	(void)time;     /* (SweetHID stamps its events itself: sweet_hid_touch() has no time) */
	if (type == VAYU_BUTTON_DOWN || type == VAYU_BUTTON_UP) {
		if (finger < 1 || finger > 4) {
			vin_count.bad++;
			return;
		}
		lck_mtx_lock(&vin_lock);
		vin_count.keys++;
		sweet_hid_key((int)finger - 1, type == VAYU_BUTTON_DOWN);
		lck_mtx_unlock(&vin_lock);
		return;
	}
	if (finger >= VAYU_TOUCH_FINGERS || type < VAYU_TOUCH_DOWN || type > VAYU_TOUCH_UP) {
		vin_count.bad++;
		return;
	}
	px = vin_pixel(x, VAYU_PANEL_W);
	py = vin_pixel(y, VAYU_PANEL_H);

	lck_mtx_lock(&vin_lock);
	if (!vin_ready && sweet_hid_touch_ready()) {
		vin_ready = true;
		vin_resync();
	}
	switch (type) {
	case VAYU_TOUCH_DOWN:
	case VAYU_TOUCH_MOVE:
		if (!vin_finger[finger].down) {
			vin_finger[finger].down = true;
			vin_held++;
			vin_count.downs++;
			log = true;
			type = VAYU_TOUCH_DOWN;
		} else {
			vin_count.moves++;
			log = (vin_count.moves & 15) == 1;
		}
		vin_finger[finger].px = px;
		vin_finger[finger].py = py;
		if (vin_ready) {
			sweet_hid_touch((int)finger, 1, px, py);
		}
		break;
	case VAYU_TOUCH_UP:
		if (!vin_finger[finger].down) {
			vin_count.bad++;        /* an UP without its DOWN: nothing is made of it */
			lck_mtx_unlock(&vin_lock);
			return;
		}
		vin_finger[finger].down = false;
		vin_count.ups++;
		log = true;
		if (--vin_held == 0) {
			vin_idle_since = mach_absolute_time();
		}
		if (vin_ready) {
			sweet_hid_touch((int)finger, 0, vin_finger[finger].px, vin_finger[finger].py);
		}
		break;
	}
	if (!vin_ready) {
		vin_count.early++;
	}
	log = log && (vin_log || vin_count.logged < 16);
	if (log) {
		vin_count.logged++;
	}
	lck_mtx_unlock(&vin_lock);
	if (log) {
		printf("VAYU TOUCH: finger %u %s at %d,%d (raw %u,%u of 65535)%s\n", finger,
		    type == VAYU_TOUCH_DOWN ? "down" : type == VAYU_TOUCH_UP ? "up" : "moved", px, py, x, y,
		    vin_ready ? "" : " -- the HID service is not there yet, kept");
	}
}

uint64_t
vayu_touch_idle_abs(void)
{
	uint64_t since = vin_idle_since;

	if (vin_held != 0) {
		return 0;
	}
	return since == 0 ? ~0ULL : mach_absolute_time() - since;
}

void
vayu_input_touch_source(const char *name, uint32_t width, uint32_t height)
{
	vin_source = name;
	vin_src_w = width;
	vin_src_h = height;
	printf("VAYU TOUCH: source %s, %u x %u in its own units, to the %u x %u panel; the HID service %s\n", name, width,
	    height, VAYU_PANEL_W, VAYU_PANEL_H, sweet_hid_touch_ready() ? "is there" : "is not there yet");
}

/* ---- vayu-ctl's synthetic input (the loader's "oem touch", sys/vayu_ctl.h) ---- */

static void
vin_ctl_post(uint32_t type, uint32_t which, uint32_t x, uint32_t y)
{
	vayu_touch_post(type, which, x, y, 0);
}

static void
vin_ctl_panel(uint32_t *width, uint32_t *height)
{
	*width = VAYU_PANEL_W;
	*height = VAYU_PANEL_H;
}

static const struct vayu_ctl_input vin_ctl = { .post = vin_ctl_post, .panel_size = vin_ctl_panel };

/* ---- firmware files of the root (sys/vayu_input.h) ---- */

#define VIN_FW_MAX      (8u << 20)

const uint8_t *
vayu_input_fw(const char *dir, const char *name, uint64_t *size, uint32_t wait_s)
{
	char path[128];
	vfs_context_t ctx = vfs_context_kernel();
	struct vnode_attr va;
	vnode_t vp = NULLVP;
	uint8_t *data = NULL;
	int error = ENOENT, resid = 0;

	snprintf(path, sizeof(path), "/%s/%s", dir, name);
	for (uint32_t waited = 0;; waited++) {
		error = vnode_lookup(path, 0, &vp, ctx);
		if (error == 0 || waited >= wait_s) {
			break;
		}
		if (waited == 0) {
			printf("VAYU TOUCH: %s is not in the root yet; waiting up to %u s\n", path, wait_s);
		}
		IOSleep(1000);
	}
	if (error != 0) {
		printf("VAYU TOUCH: %s: %d\n", path, error);
		return NULL;
	}
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	error = vnode_getattr(vp, &va, ctx);
	if (error == 0 && (va.va_data_size == 0 || va.va_data_size > VIN_FW_MAX || vnode_vtype(vp) != VREG)) {
		error = EFBIG;
	}
	if (error == 0) {
		data = kalloc_data((vm_size_t)va.va_data_size, Z_WAITOK);
		error = data == NULL ? ENOMEM : vn_rdwr(UIO_READ, vp, (caddr_t)data, (int)va.va_data_size, 0, UIO_SYSSPACE, 0,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		if (error == 0 && resid != 0) {
			error = EIO;
		}
	}
	vnode_put(vp);
	if (error != 0) {
		printf("VAYU TOUCH: %s not read: %d\n", path, error);
		if (data != NULL) {
			kfree_data(data, (vm_size_t)va.va_data_size);
		}
		return NULL;
	}
	*size = va.va_data_size;
	printf("VAYU TOUCH: %s: %llu bytes, crc32 %08x\n", path, *size, crc32(0, data, (size_t)*size));
	return data;
}

/* ---- vayu_hidtest=1 (QEMU: no hardware) ---- */

static void
vin_hidtest_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t before;
	int waited;

	for (waited = 0; waited < 120 && !sweet_hid_touch_ready(); waited++) {
		IOSleep(500);
	}
	if (!sweet_hid_touch_ready()) {
		printf("VAYU HIDTEST: FAIL: no SweetHID digitizer in 60 s\n");
		return;
	}
	before = sweet_hid_touch_events();
	vayu_touch_post(VAYU_TOUCH_DOWN, 0, 32768, 32768, 0);          /* 1: down at the centre */
	vayu_touch_post(VAYU_TOUCH_MOVE, 0, 40000, 30000, 0);          /* 2: moved */
	vayu_touch_post(VAYU_TOUCH_MOVE, 0, 40000, 30000, 0);          /* (the same place: no event) */
	vayu_touch_post(VAYU_TOUCH_UP, 0, 40000, 30000, 0);            /* 3: up */
	vayu_touch_post(VAYU_TOUCH_UP, 0, 40000, 30000, 0);            /* (an UP without a DOWN: counted bad) */
	vayu_touch_post(VAYU_TOUCH_DOWN, 1, 0, 0, 0);                  /* 4: a corner */
	vayu_touch_post(VAYU_TOUCH_DOWN, 2, 65535, 65535, 0);          /* 5: the other corner */
	vayu_touch_post(VAYU_TOUCH_UP, 1, 0, 0, 0);                    /* 6 */
	vayu_touch_post(VAYU_TOUCH_UP, 2, 65535, 65535, 0);            /* 7 */
	{
		uint64_t made = sweet_hid_touch_events() - before;
		bool pixels = vin_pixel(0, VAYU_PANEL_W) == 0 && vin_pixel(65535, VAYU_PANEL_W) == VAYU_PANEL_W - 1 &&
		    vin_pixel(32768, VAYU_PANEL_W) == 540 && vin_pixel(65535, VAYU_PANEL_H) == VAYU_PANEL_H - 1 &&
		    vin_pixel(32768, VAYU_PANEL_H) == 1200;

		uint64_t fwsize = 0;

		/* the firmware reader too, when the root has the file (no wait): what the touch driver would load */
		(void)vayu_input_fw("fw", "j20s_novatek_ts_fw02.bin", &fwsize, 0);
		printf("VAYU HIDTEST: %llu digitizer events dispatched (7 expected), %llu downs %llu moves %llu ups %llu bad, "
		    "pixels %s: %s\n", made, vin_count.downs, vin_count.moves, vin_count.ups, vin_count.bad,
		    pixels ? "ok" : "WRONG", made == 7 && vin_count.bad == 1 && vin_held == 0 && pixels ? "PASS" : "FAIL");
	}
}

/* the counts, a line a minute while a touch source runs */
static void
vin_status_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		IOSleep(60000);
		printf("VAYU TOUCH: %llu downs, %llu moves, %llu ups, %llu buttons; %llu before the HID service, %llu given "
		    "to it again; %llu refused; %u fingers down; HID %s, %llu digitizer events\n", vin_count.downs,
		    vin_count.moves, vin_count.ups, vin_count.keys, vin_count.early, vin_count.resynced, vin_count.bad,
		    vin_held, sweet_hid_touch_ready() ? "there" : "not there", sweet_hid_touch_events());
	}
}

/* ---- the K7 hook ---- */

static void
vayu_input_after_devfs(void)
{
	uint32_t on = 0;
	thread_t thread;
	bool touch = vayu_arg("touchscreen");

	vin_log = PE_parse_boot_argn("touch_log", &on, sizeof(on)) && on != 0;
	if (touch || vayu_arg("usbctl")) {
		vayu_ctl_set_input(&vin_ctl);
	}
	if (vayu_arg("vayu_hidtest") && kernel_thread_start(vin_hidtest_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	if (!touch) {
		return;
	}
	printf("VAYU TOUCH: into SweetHID's digitizer (%s now), %u x %u panel pixels%s\n",
	    sweet_hid_touch_ready() ? "there" : "not there yet", VAYU_PANEL_W, VAYU_PANEL_H, vin_log ? ", every event logged" : "");
	if (kernel_thread_start(vin_status_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	vayu_nvt_init();
}

VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, input, vayu_input_after_devfs);
