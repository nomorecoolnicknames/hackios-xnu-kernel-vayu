/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/vayu_mdp.c (the 8796 line, same authors), the read-only look (look(),
 * show_sspp(), dump(), the block offsets and register names). Ported to xnu-12377 by K5: the boot framebuffer's
 * address comes from the loader's hand-off (PE_current_console), not the constant 0x9c000000; a VAYU_HOOK start.
 * K5 2nd step: B's zero-copy scanout (vayu_mdp_scanout/unscanout, vayu_mdp_quiesce, B:349-405) behind vayu_mdp=2,
 * with the state check of B's flip_test (B:217-226) before the first write and the quiesce registered with the loader
 * (loader_quiesce_register). B's copy-and-flip test (flip_test) is not brought: SweetFramebuffer's fb_zero is its test.
 */
/*
 * The display controller as UEFI left it (POCO X3 Pro, vayu, SM8150 "DPU 5.0"): a look, reading only.
 *
 * Where everything is (mainline drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_5_0_sm8150.h, offsets from the MDP at
 * 0x0ae01000, sm8150.dtsi:3807 "display-controller@ae01000"; INTF1 at +0x6a800 = 0x0ae6b800 is what vayu_vsync.c reads):
 *   CTL 0-5      +0x1000 + 0x200 n          SSPP VIG0-3  +0x4000 + 0x2000 n   SSPP DMA0-3 +0x24000 + 0x2000 n
 *   LM 0-5       +0x44000 + 0x1000 n        PINGPONG 0-5 +0x70000 + 0x800 n   DSC 0-2     +0x80000 + 0x400 n
 * Registers (mainline dpu_hw_ctl.c:15-48, dpu_hw_sspp.c:21-64, dpu_hw_lm.c:15-42):
 *   CTL   LAYER(lm) 0x00 + 4 lm (0x24 for lm 5), TOP 0x14, FLUSH 0x18, INTF_ACTIVE 0xf4, FETCH_PIPE_ACTIVE 0xfc,
 *         PIPE_ACTIVE 0x12c, LAYER_ACTIVE 0x130, INTF_MASTER 0x134, DSC_ACTIVE 0xe8
 *   SSPP  SRC_SIZE 0x00, SRC_XY 0x08, OUT_SIZE 0x0c, OUT_XY 0x10, SRC0..3_ADDR 0x14-0x20, YSTRIDE0/1 0x24/0x28,
 *         FORMAT 0x30, UNPACK 0x34, OP_MODE 0x38, FETCH_CONFIG 0x48, ADDR_SW_STATUS 0x70
 *   LM    OP_MODE 0x00, OUT_SIZE 0x04 (and the blend stages after it, dumped raw)
 * The 8796 line's run 033326 saw VIG0 alone scan out 1080x2400 from 0x9c000000 (ystride 4320, format 0x237ff), CTL0
 * with it on LM0, INTF1 and DSC0 active.
 *
 * vayu27=1 vayu_mdp=1: a thread looks vayu_mdp_delay seconds (default 2) after the hook (before the root), and 60 s
 * later again: CTL, SSPP, LM, PINGPONG and DSC blocks; then the one line the display tests need (K5): which source
 * pipe fetches the framebuffer the kernel draws into ("VAYU MDP: scanout: vig_0 fetches the boot framebuffer ...: YES"),
 * and INTF1's frame counter over one second (the panel is being refreshed from it). Nothing is written; the MDP is
 * mapped only on the phone (/arm-io device_type "vayu"): QEMU has no SM8150 MMIO.
 * vayu27=1 vayu_mdp=2: the look as with 1, and the scanout of a frame of SweetFramebuffer's (fb_zero=1) is allowed:
 * VIG0's SRC0_ADDR, SRC_YSTRIDE0 and SRC_XY are pointed at a contiguous surface below 4 GB (the crop of a 1125x2436
 * picture to the panel's 1080x2400 centre), then CTL0's FLUSH bit 0 (VIG0), and the flush watched until the hardware
 * takes it at the next frame start (at most 50 ms); SRC_SIZE, OUT_SIZE, LM, DSC, INTF and the CTL routing are never
 * written. Before the first write VIG0 and CTL0 must still be as UEFI left them (run 033326 of the 8796 line): VIG0
 * fetches the boot framebuffer at the panel's size and stride, CTL0 has a layer on LM0 and INTF1 active; else nothing
 * is ever written. vayu_mdp_unscanout() and, on the way back to the loader, vayu_mdp_quiesce() put the boot
 * framebuffer, its stride and offset back.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_mdp.h>
#include <sys/vayu_loader.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void IOSleep(unsigned int milliseconds);
extern int get_preemption_level(void);           /* osfmk/arm/cpu_data.h */
extern void delay(int usec);

#define MDP_PHYS                0x0ae01000UL    /* sm8150.dtsi:3807 */
#define MDP_SIZE                0x90000UL       /* through DSC 2 (+0x80800 + 0x140) */

#define CTL(n)                  (0x1000UL + 0x200UL * (n))
#define SSPP_VIG(n)             (0x4000UL + 0x2000UL * (n))
#define SSPP_DMA(n)             (0x24000UL + 0x2000UL * (n))
#define LM(n)                   (0x44000UL + 0x1000UL * (n))
#define PINGPONG(n)             (0x70000UL + 0x800UL * (n))
#define DSC(n)                  (0x80000UL + 0x400UL * (n))
#define INTF1                   0x6a800UL       /* dpu_5_0_sm8150.h intf_1; vayu_vsync.c */
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_FRAME_COUNT        0x0ac

static vm_offset_t mdp;
static uint32_t mdp_level;                      /* vayu_mdp=N (0 until the hook or the first scanout read it) */
static uint64_t fb_phys;                        /* the loader's hand-off: what the kernel draws into */
static uint32_t fb_width, fb_height, fb_rowbytes;

static inline uint32_t
rd(vm_offset_t offset)
{
	return *(volatile uint32_t *)(mdp + offset);
}

/*
 * The SM8150 is there: the loader's device tree says which platform it wrote (/arm-io device_type, mkadt_vayu.py), and
 * only "vayu" has this MMIO. Shared with vayu_vsync.c.
 */
bool
vayu_display_hw_present(void)
{
	DTEntry entry;
	const char *type = NULL;
	unsigned int size = 0;

	if (SecureDTLookupEntry(NULL, "/arm-io", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "device_type", (const void **)&type, &size) != kSuccess || type == NULL) {
		return false;
	}
	return size >= 5 && strncmp(type, "vayu", 5) == 0;
}

static void
dump(const char *name, vm_offset_t base, uint32_t from, uint32_t to)
{
	for (uint32_t o = from; o < to; o += 0x20) {
		printf("vayu-mdp: %-9s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", name, o, rd(base + o), rd(base + o + 4),
		    rd(base + o + 8), rd(base + o + 12), rd(base + o + 16), rd(base + o + 20), rd(base + o + 24), rd(base + o + 28));
	}
}

static void
show_sspp(const char *name, vm_offset_t base)
{
	uint32_t src_size = rd(base + 0x00), addr = rd(base + 0x14);

	if (src_size == 0 && addr == 0) {
		printf("vayu-mdp: %s: idle (size 0, address 0)\n", name);
		return;
	}
	printf("vayu-mdp: %s: src %ux%u at (%u,%u), out %ux%u at (%u,%u), addr0 %#x%s, ystride0 %u, format %#x, unpack %#x, "
	    "op_mode %#x, fetch_config %#x, sw_status %#x\n", name, src_size & 0xffff, src_size >> 16, rd(base + 0x08) & 0xffff,
	    rd(base + 0x08) >> 16, rd(base + 0x0c) & 0xffff, rd(base + 0x0c) >> 16, rd(base + 0x10) & 0xffff, rd(base + 0x10) >> 16,
	    addr, (uint64_t)addr == fb_phys ? " (= the boot framebuffer's physical address)" : "", rd(base + 0x24) & 0xffff,
	    rd(base + 0x30), rd(base + 0x34), rd(base + 0x38), rd(base + 0x48), rd(base + 0x70));
	dump(name, base, 0x00, 0x80);
}

static void
look(unsigned when)
{
	char name[16];

	printf("vayu-mdp: look %u: MDP at %#lx, read only\n", when, MDP_PHYS);
	for (unsigned n = 0; n < 6; n++) {
		vm_offset_t c = CTL(n);
		uint32_t top = rd(c + 0x14), intf = rd(c + 0xf4);

		if (top == 0 && intf == 0 && rd(c + 0x00) == 0) {
			printf("vayu-mdp: ctl_%u: idle\n", n);
			continue;
		}
		printf("vayu-mdp: ctl_%u: top %#x, flush %#x, intf_active %#x, intf_master %#x, dsc_active %#x, fetch_pipe_active %#x, "
		    "pipe_active %#x, layer_active %#x; layer(lm0..5) %#x %#x %#x %#x %#x %#x\n", n, top, rd(c + 0x18), intf,
		    rd(c + 0x134), rd(c + 0xe8), rd(c + 0xfc), rd(c + 0x12c), rd(c + 0x130), rd(c + 0x00), rd(c + 0x04), rd(c + 0x08),
		    rd(c + 0x0c), rd(c + 0x10), rd(c + 0x24));
		snprintf(name, sizeof(name), "ctl_%u", n);
		dump(name, c, 0x00, 0x140);
	}
	for (unsigned n = 0; n < 4; n++) {
		snprintf(name, sizeof(name), "vig_%u", n);
		show_sspp(name, SSPP_VIG(n));
	}
	for (unsigned n = 0; n < 4; n++) {
		snprintf(name, sizeof(name), "dma_%u", n);
		show_sspp(name, SSPP_DMA(n));
	}
	for (unsigned n = 0; n < 6; n++) {
		uint32_t out = rd(LM(n) + 0x04);

		if (out == 0) {
			printf("vayu-mdp: lm_%u: idle\n", n);
			continue;
		}
		printf("vayu-mdp: lm_%u: op_mode %#x, out %ux%u\n", n, rd(LM(n) + 0x00), out & 0xffff, out >> 16);
		snprintf(name, sizeof(name), "lm_%u", n);
		dump(name, LM(n), 0x00, 0x100);
	}
	for (unsigned n = 0; n < 6; n++) {
		snprintf(name, sizeof(name), "pp_%u", n);
		if (rd(PINGPONG(n) + 0x00) != 0 || rd(PINGPONG(n) + 0x04) != 0) {
			dump(name, PINGPONG(n), 0x00, 0x40);
		}
	}
	for (unsigned n = 0; n < 3; n++) {
		snprintf(name, sizeof(name), "dsc_%u", n);
		if (rd(DSC(n) + 0x00) != 0) {
			dump(name, DSC(n), 0x00, 0x40);
		}
	}
	printf("vayu-mdp: look %u done, nothing written\n", when);
}

/*
 * The proof the display tests need from the hardware: a source pipe whose plane 0 is the framebuffer the kernel draws
 * into, at the panel's size and stride, and the timing engine of INTF1 counting frames. SRC0_ADDR is 32 bits (the MDP
 * fetches below 4 GB without an SMMU here): a framebuffer above 4 GB cannot be the one scanned out.
 */
static void
scanout_check(unsigned when)
{
	const char *who = NULL;
	char name[16];
	uint32_t f0, f1, engine;
	uint64_t t0, t1, us;

	for (unsigned n = 0; n < 8 && who == NULL; n++) {
		vm_offset_t b = n < 4 ? SSPP_VIG(n) : SSPP_DMA(n - 4);
		uint32_t size = rd(b + 0x00);

		if ((uint64_t)rd(b + 0x14) == fb_phys && (size & 0xffff) == fb_width && (size >> 16) == fb_height &&
		    (rd(b + 0x24) & 0xffff) == fb_rowbytes) {
			snprintf(name, sizeof(name), "%s_%u", n < 4 ? "vig" : "dma", n < 4 ? n : n - 4);
			who = name;
		}
	}
	engine = rd(INTF1 + INTF_TIMING_ENGINE_EN);
	f0 = rd(INTF1 + INTF_FRAME_COUNT);
	t0 = mach_absolute_time();
	IOSleep(1000);
	f1 = rd(INTF1 + INTF_FRAME_COUNT);
	t1 = mach_absolute_time();
	absolutetime_to_nanoseconds(t1 - t0, &us);
	us /= 1000;
	printf("VAYU MDP: scanout %u: %s fetches the boot framebuffer %#llx %ux%u stride %u: %s; INTF1 timing engine %s, "
	    "%u frames in %llu us\n", when, who != NULL ? who : "no pipe", fb_phys, fb_width, fb_height, fb_rowbytes,
	    who != NULL ? "YES" : "NO", (engine & 1) ? "on" : "OFF", f1 - f0, us);
}

/* ---- vayu_mdp=2: SweetFramebuffer's zero-copy scanout (B:349-405) ------------------------------------------ */

#define VIG0_SRC_SIZE           (SSPP_VIG(0) + 0x00)
#define VIG0_SRC_XY             (SSPP_VIG(0) + 0x08)
#define VIG0_SRC0_ADDR          (SSPP_VIG(0) + 0x14)
#define VIG0_SRC_YSTRIDE0       (SSPP_VIG(0) + 0x24)
#define CTL0_FLUSH              (CTL(0) + 0x18)
#define FLUSH_VIG0              (1u << 0)

static LCK_GRP_DECLARE(vayu_mdp_grp, "vayu_mdp");
static LCK_MTX_DECLARE(vayu_mdp_lock, &vayu_mdp_grp);   /* the scanout state; one caller at a time */
static uint32_t boot_src0, boot_ystride0, boot_src_xy;  /* VIG0 as UEFI left it */
static int scanout_state;                               /* 0 not looked at, 1 allowed, -1 refused for the run */
static volatile bool flipped;                           /* VIG0 not on the boot framebuffer: quiesce puts it back */
static uint32_t scanout_flips, scanout_late, scanout_restores;

/* vayu_crumb=1: sys/vayu_mdp.h */
extern void loader_log_sync(void);              /* osfmk/arm64/loader_return.c */
static int crumb_on = -1;

int
vayu_crumb_level(void)
{
	if (crumb_on < 0) {
		uint32_t v = 0;

		(void)PE_parse_boot_argn("vayu_crumb", &v, sizeof(v));
		crumb_on = !vayu27_enabled() || v == 0 ? 0 : v == 2 ? 2 : 1;
	}
	return crumb_on;
}

void
vayu_crumb(const char *format, ...)
{
	char line[200];
	va_list list;
	int n;

	if (vayu_crumb_level() == 0) {
		return;
	}
	n = snprintf(line, sizeof(line), "VAYU CRUMB: ");
	va_start(list, format);
	vsnprintf(line + n, sizeof(line) - (size_t)n, format, list);
	va_end(list);
	loader_log_line(line);
	loader_log_sync();
	/*
	 * vayu_crumb=2: and a pause where one may sleep (not on the way to the loader, not with interrupts off), so that
	 * the live log on 5140 (bsd/dev/vayu_usbnet_ecm.c, which looks at the loader's log every 100 ms) has sent the line
	 * before the step it names begins: a hang that takes the whole phone still leaves it on the host.
	 */
	if (crumb_on == 2 && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
		IOSleep(250);
	}
}

static inline void
wr(vm_offset_t offset, uint32_t value)
{
	*(volatile uint32_t *)(mdp + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* the CTL0 flush asked for and watched (at most 50 ms: the next frame start is 8.3 ms away at 120 Hz) */
static bool
scanout_flush(uint32_t *waited_us)
{
	wr(CTL0_FLUSH, FLUSH_VIG0);
	for (uint32_t us = 0; us < 50000; us += 50) {
		if ((rd(CTL0_FLUSH) & FLUSH_VIG0) == 0) {
			*waited_us = us;
			return true;
		}
		delay(50);
	}
	*waited_us = 50000;
	return false;
}

/* with the lock: may the scanout be written at all? Once per run; the boot state saved. */
static bool
scanout_allowed(void)
{
	PE_Video video;
	uint32_t layer, intf, size;

	if (scanout_state != 0) {
		return scanout_state > 0;
	}
	scanout_state = -1;
	vayu_crumb("mdp: scanout allowed? (the first time: VIG0, CTL0 read)\n");
	if (!vayu27_enabled() || !PE_parse_boot_argn("vayu_mdp", &mdp_level, sizeof(mdp_level)) || mdp_level < 2) {
		return false;           /* (silent: the caller asks without the argument too) */
	}
	if (!vayu_display_hw_present()) {
		printf("vayu-mdp: 2: not the phone: no scanout\n");
		return false;
	}
	if (mdp == 0) {
		mdp = ml_io_map(MDP_PHYS, MDP_SIZE);
	}
	PE_current_console(&video);
	fb_phys = video.v_baseAddr;
	fb_width = (uint32_t)video.v_width;
	fb_height = (uint32_t)video.v_height;
	fb_rowbytes = (uint32_t)video.v_rowBytes;
	if (mdp == 0) {
		printf("vayu-mdp: 2: the MDP cannot be mapped: no scanout\n");
		return false;
	}
	layer = rd(CTL(0) + 0x00);
	intf = rd(CTL(0) + 0xf4);
	size = rd(VIG0_SRC_SIZE);
	boot_src0 = rd(VIG0_SRC0_ADDR);
	boot_ystride0 = rd(VIG0_SRC_YSTRIDE0);
	boot_src_xy = rd(VIG0_SRC_XY);
	if ((uint64_t)boot_src0 != fb_phys || size != (fb_height << 16 | fb_width) || (boot_ystride0 & 0xffff) != fb_rowbytes ||
	    (layer & 0x7) == 0 || (intf & 0x2) == 0 || boot_src_xy != 0) {
		printf("vayu-mdp: 2: VIG0/CTL0 are not as UEFI leaves them (addr %#x for %#llx, size %#x, ystride %#x, xy %#x, "
		    "layer %#x, intf %#x): nothing will be written\n", boot_src0, fb_phys, size, boot_ystride0, boot_src_xy, layer,
		    intf);
		return false;
	}
	if (loader_quiesce_register("vayu_mdp", vayu_mdp_quiesce) != 0) {
		printf("vayu-mdp: 2: the loader's quiesce list is full: no scanout (it could not be put back)\n");
		return false;
	}
	scanout_state = 1;
	printf("vayu-mdp: 2: scanout allowed: VIG0 fetches %#llx %ux%u stride %u, CTL0 layer %#x intf %#x; the boot "
	    "framebuffer goes back on the way to the loader\n", fb_phys, fb_width, fb_height, fb_rowbytes, layer, intf);
	return true;
}

bool
vayu_mdp_scanout(uint64_t phys, uint32_t stride, uint32_t x, uint32_t y)
{
	uint32_t waited = 0;
	bool taken;

	if (phys == 0 || (phys & 0x3f) != 0 || stride < fb_width * 4 || stride > 0xffff || (stride & 0x3f) != 0 ||
	    x > 0xffff || y > 0xffff) {
		return false;
	}
	vayu_crumb("mdp: scanout %#llx stride %u at (%u,%u): the lock\n", phys, stride, x, y);
	lck_mtx_lock(&vayu_mdp_lock);
	if (!scanout_allowed() || phys + (uint64_t)stride * (y + fb_height) > (4ULL << 30)) {
		lck_mtx_unlock(&vayu_mdp_lock);
		vayu_crumb("mdp: scanout refused\n");
		return false;
	}
	vayu_crumb("mdp: VIG0 SRC0 <- %#llx (was %#x), YSTRIDE0 <- %#x, XY <- %#x\n", phys, rd(VIG0_SRC0_ADDR),
	    (boot_ystride0 & 0xffff0000u) | stride, y << 16 | x);
	wr(VIG0_SRC0_ADDR, (uint32_t)phys);
	wr(VIG0_SRC_YSTRIDE0, (boot_ystride0 & 0xffff0000u) | stride);
	wr(VIG0_SRC_XY, y << 16 | x);
	flipped = true;
	vayu_crumb("mdp: VIG0 written (SRC0 reads %#x); CTL0 flush\n", rd(VIG0_SRC0_ADDR));
	taken = rd(VIG0_SRC0_ADDR) == (uint32_t)phys && scanout_flush(&waited);
	vayu_crumb("mdp: flush %s after %u us; INTF1 frame count %u\n", taken ? "taken" : "NOT taken", waited,
	    rd(INTF1 + INTF_FRAME_COUNT));
	if (taken) {
		scanout_flips++;
	} else {
		scanout_late++;
	}
	if (scanout_flips + scanout_late <= 8 || !taken) {
		printf("vayu-mdp: 2: VIG0 <- %#llx stride %u at (%u,%u): %s after %u us (flips %u, not taken %u)\n", phys, stride, x,
		    y, taken ? "TAKEN" : "NOT TAKEN", waited, scanout_flips, scanout_late);
	}
	lck_mtx_unlock(&vayu_mdp_lock);
	return taken;
}

bool
vayu_mdp_unscanout(void)
{
	uint32_t waited = 0;
	bool taken = true;

	vayu_crumb("mdp: unscanout: the lock\n");
	lck_mtx_lock(&vayu_mdp_lock);
	if (flipped && scanout_state > 0) {
		vayu_crumb("mdp: unscanout: VIG0 back to %#x\n", boot_src0);
		wr(VIG0_SRC0_ADDR, boot_src0);
		wr(VIG0_SRC_YSTRIDE0, boot_ystride0);
		wr(VIG0_SRC_XY, boot_src_xy);
		taken = scanout_flush(&waited);
		flipped = !taken;
		scanout_restores++;
		printf("vayu-mdp: 2: VIG0 back on the boot framebuffer %#x: %s after %u us\n", boot_src0,
		    taken ? "TAKEN" : "NOT TAKEN (put back again at the run's end)", waited);
	}
	lck_mtx_unlock(&vayu_mdp_lock);
	vayu_crumb("mdp: unscanout: %s\n", taken ? "done" : "NOT taken");
	return taken;
}

/* On the way to the loader (loader_return.c, interrupts off): the boot framebuffer on the panel again, no waiting. */
void
vayu_mdp_quiesce(void)
{
	if (flipped && mdp != 0 && scanout_state > 0) {
		vayu_crumb("mdp: quiesce: VIG0 back to %#x\n", boot_src0);
		*(volatile uint32_t *)(mdp + VIG0_SRC0_ADDR) = boot_src0;
		*(volatile uint32_t *)(mdp + VIG0_SRC_YSTRIDE0) = boot_ystride0;
		*(volatile uint32_t *)(mdp + VIG0_SRC_XY) = boot_src_xy;
		*(volatile uint32_t *)(mdp + CTL0_FLUSH) = FLUSH_VIG0;
		__asm__ volatile ("dsb sy" ::: "memory");
		flipped = false;
	}
}

static void
vayu_mdp_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t level = (uint32_t)(uintptr_t)parameter, delay_s = 2;

	PE_parse_boot_argn("vayu_mdp_delay", &delay_s, sizeof(delay_s));
	lck_mtx_lock(&vayu_mdp_lock);
	if (mdp == 0) {
		mdp = ml_io_map(MDP_PHYS, MDP_SIZE);
	}
	lck_mtx_unlock(&vayu_mdp_lock);
	if (mdp == 0) {
		printf("vayu-mdp: the MDP cannot be mapped\n");
		return;
	}
	IOSleep(delay_s * 1000);
	vayu_crumb("mdp: look 1\n");
	look(1);
	scanout_check(1);
	vayu_crumb("mdp: look 1 done\n");
	IOSleep(60000);
	vayu_crumb("mdp: look 2\n");
	look(2);
	scanout_check(2);
	vayu_crumb("mdp: look 2 done\n");
	/* (returning ends the thread) */
}

static void
vayu_mdp_start(void)
{
	thread_t thread;
	uint32_t level = 0;
	PE_Video video;

	if (!PE_parse_boot_argn("vayu_mdp", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (!vayu_display_hw_present()) {
		printf("vayu-mdp: not the phone (/arm-io is not \"vayu\"): no MDP to look at\n");
		return;
	}
	PE_current_console(&video);
	fb_phys = video.v_baseAddr;
	fb_width = (uint32_t)video.v_width;
	fb_height = (uint32_t)video.v_height;
	fb_rowbytes = (uint32_t)video.v_rowBytes;
	if (kernel_thread_start(vayu_mdp_thread, (void *)(uintptr_t)level, &thread) != KERN_SUCCESS) {
		printf("vayu-mdp: the thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}

VAYU_HOOK(VAYU_PHASE_BEFORE_ROOT, mdp, vayu_mdp_start);
