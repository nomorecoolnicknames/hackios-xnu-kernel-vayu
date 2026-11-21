/*
 * The display controller as UEFI left it (POCO X3 Pro, vayu, SM8150 "DPU 5.0"), bsd/dev/vayu_mdp.c: a
 * look, reading only, for the zero-copy present (the panel scanning out iOS's surface instead of the
 * framebuffer driver copying every frame: 5.5-7.9 ms of copy a frame, run 194827, too slow for 120 Hz).
 *
 * Where everything is (mainline drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_5_0_sm8150.h, offsets from the
 * MDP at 0x0ae01000, sm8150.dtsi:3807 "display-controller@ae01000"; INTF1 at +0x6a800 = 0x0ae6b800 is what
 * vayu_vsync.c reads, and agrees with the vendor sm8150-sde.dtsi):
 *   CTL 0-5      +0x1000 + 0x200 n          SSPP VIG0-3  +0x4000 + 0x2000 n   SSPP DMA0-3 +0x24000 + 0x2000 n
 *   LM 0-5       +0x44000 + 0x1000 n        PINGPONG 0-5 +0x70000 + 0x800 n   DSC 0-2     +0x80000 + 0x400 n
 * Registers (mainline dpu_hw_ctl.c:15-48, dpu_hw_sspp.c:21-64, dpu_hw_lm.c:15-42):
 *   CTL   LAYER(lm) 0x00 + 4 lm (0x24 for lm 5), TOP 0x14, FLUSH 0x18, INTF_ACTIVE 0xf4, FETCH_PIPE_ACTIVE 0xfc,
 *         PIPE_ACTIVE 0x12c, LAYER_ACTIVE 0x130, INTF_MASTER 0x134, DSC_ACTIVE 0xe8
 *   SSPP  SRC_SIZE 0x00, SRC_XY 0x08, OUT_SIZE 0x0c, OUT_XY 0x10, SRC0..3_ADDR 0x14-0x20, YSTRIDE0/1 0x24/0x28,
 *         FORMAT 0x30, UNPACK 0x34, OP_MODE 0x38, FETCH_CONFIG 0x48, ADDR_SW_STATUS 0x70
 *   LM    OP_MODE 0x00, OUT_SIZE 0x04 (and the blend stages after it, dumped raw)
 * The boot framebuffer: 1080 x 2400 at 0x9c000000 (loader, "display 1080 x 2400 at 9c000000"; platform fb_base).
 *
 * Boot-arg vayu_mdp=1: one look 20 s after the start (the user interface up by then), another 60 s later.
 * Nothing is written. Every line also goes into the loader's log (tee_printf).
 *
 * vayu_mdp=2: the first write toward the zero-copy present, a page flip. What run 033326 showed (look 1):
 * VIG0 alone scans out 1080x2400 from 0x9c000000 -- the boot framebuffer's physical address, so the MDP
 * fetches physical memory, no SMMU in the way -- ystride 4320, format 0x237ff; CTL0 has it on LM0
 * (LAYER(lm0) 0x1000005), INTF1 and DSC0 active. 30 s after the start, if all of that still reads so: a
 * contiguous copy of the frame below 4 GB (SRC0_ADDR is 32 bits), VIG0's SRC0_ADDR <- its address (read
 * back), CTL0's FLUSH <- BIT(0) (VIG0: dpu_hw_ctl.c dpu_hw_ctl_update_pending_flush_sspp), the flush bit
 * watched until the hardware takes it (at the next frame); 10 s on the copy (iOS's drawing into 0x9c000000
 * is not seen meanwhile), then SRC0_ADDR <- 0x9c000000 and FLUSH again. Only those two registers are ever
 * written; DSC, INTF, LM and CTL routing stay as UEFI left them. vayu_mdp_mark=1 draws a 160x160 square in
 * the copy, so that the flip can be seen. vayu_mdp_quiesce() puts 0x9c000000 back on the way to the loader.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_mdp.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern kern_return_t thread_terminate(thread_t thread);
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_putc(char c);
extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void flush_dcache(vm_offset_t addr, unsigned count, int phys);

static int tee_printf(const char *format, ...) __printflike(1, 2);

static int
tee_printf(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		(printf)("%s", line);
	}
	return length;
}
#define printf(...) tee_printf(__VA_ARGS__)

#define MDP_PHYS                0x0ae01000UL    /* sm8150.dtsi:3807 */
#define MDP_SIZE                0x90000UL       /* through DSC 2 (+0x80800 + 0x140) */
#define FB_PHYS                 0x9c000000UL    /* the boot framebuffer (loader's GOP line) */

#define CTL(n)                  (0x1000UL + 0x200UL * (n))
#define SSPP_VIG(n)             (0x4000UL + 0x2000UL * (n))
#define SSPP_DMA(n)             (0x24000UL + 0x2000UL * (n))
#define LM(n)                   (0x44000UL + 0x1000UL * (n))
#define PINGPONG(n)             (0x70000UL + 0x800UL * (n))
#define DSC(n)                  (0x80000UL + 0x400UL * (n))

static vm_offset_t mdp;

static inline uint32_t
rd(vm_offset_t offset)
{
	return *(volatile uint32_t *)(mdp + offset);
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
	    addr, addr == FB_PHYS ? " (= the boot framebuffer's physical address)" : "", rd(base + 0x24) & 0xffff, rd(base + 0x30),
	    rd(base + 0x34), rd(base + 0x38), rd(base + 0x48), rd(base + 0x70));
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

/* ---- vayu_mdp=2: the flip ------------------------------------------------------------------------------ */

#define VIG0_SRC0_ADDR          (SSPP_VIG(0) + 0x14)
#define CTL0_FLUSH              (CTL(0) + 0x18)
#define FLUSH_VIG0              (1u << 0)
#define FRAME_BYTES             (1080UL * 2400UL * 4UL)
#define INTF1_FRAME_COUNT       (0x6a800UL + 0xac)  /* vayu_vsync.c: INTF1 at +0x6a800, FRAME_COUNT 0xac */

static volatile bool flipped;
static bool spare_used;                         /* the frame is the loader's fb_spare (not ours to free) */

static inline void
wr(vm_offset_t offset, uint32_t value)
{
	*(volatile uint32_t *)(mdp + offset) = value;
}

/* SRC0_ADDR set and read back, the flush asked for and watched (at most 100 ms: the next frame is 8.3 ms away) */
static bool
point(uint32_t address, const char *what)
{
	uint32_t back, frame0 = rd(INTF1_FRAME_COUNT), flush = 0;
	unsigned ms;

	wr(VIG0_SRC0_ADDR, address);
	back = rd(VIG0_SRC0_ADDR);
	if (back != address) {
		printf("vayu-mdp: 2: VIG0 SRC0_ADDR <- %#x reads %#x: NOT FLUSHED (%s)\n", address, back, what);
		return false;
	}
	wr(CTL0_FLUSH, FLUSH_VIG0);
	for (ms = 0; ms < 100; ms++) {
		if (((flush = rd(CTL0_FLUSH)) & FLUSH_VIG0) == 0) {
			break;
		}
		IOSleep(1);
	}
	printf("vayu-mdp: 2: %s: VIG0 SRC0_ADDR %#x, CTL0 FLUSH %s after %u ms (flush %#x), frames %u -> %u\n", what, address,
	    (flush & FLUSH_VIG0) == 0 ? "TAKEN" : "STILL PENDING", ms, flush, frame0, rd(INTF1_FRAME_COUNT));
	return (flush & FLUSH_VIG0) == 0;
}

static void
flip_test(void)
{
	vm_offset_t copy = 0, fb;
	vm_size_t size = round_page(FRAME_BYTES);
	uint64_t phys;
	uint32_t mark = 0, flip = 1, layer = rd(CTL(0) + 0x00), intf = rd(CTL(0) + 0xf4);

	if (rd(VIG0_SRC0_ADDR) != FB_PHYS || rd(SSPP_VIG(0) + 0x00) != (2400u << 16 | 1080u) ||
	    (rd(SSPP_VIG(0) + 0x24) & 0xffff) != 4320 || (layer & 0x7) == 0 || intf != 0x2) {
		printf("vayu-mdp: 2: VIG0/CTL0 are not as run 033326 had them (addr %#x, size %#x, stride %u, layer %#x, intf %#x): "
		    "nothing written\n", rd(VIG0_SRC0_ADDR), rd(SSPP_VIG(0) + 0x00), rd(SSPP_VIG(0) + 0x24) & 0xffff, layer, intf);
		return;
	}
	/*
	 * The frame: the loader's spare frames if it was asked for them (fb_spare=N: below 4 GB, outside XNU's
	 * RAM, mapped write-combined, so nothing to clean from the caches), else a contiguous allocation of ours
	 * below 4 GB (run 034941: there was none of 10 MB).
	 */
	uint64_t spare = 0;

	if (PE_parse_boot_argn("fb_spare_pa", &spare, sizeof(spare)) && spare != 0 && spare + size <= (4ULL << 30)) {
		phys = spare;
		copy = ml_io_map_wcomb((vm_offset_t)spare, size);
		spare_used = true;
	} else if (kmem_alloc_contig(kernel_map, &copy, size, PAGE_MASK, (ppnum_t)((4ULL << 30) >> PAGE_SHIFT) - 1, 0,
	    KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("vayu-mdp: 2: no contiguous %lu bytes below 4 GB and no fb_spare_pa from the loader (fb_spare=1): nothing "
		    "written\n", (unsigned long)size);
		return;
	} else {
		phys = kvtophys(copy);
		if (phys == 0 || phys + size > (4ULL << 30)) {
			printf("vayu-mdp: 2: the copy is at %#llx, not below 4 GB: nothing written\n", phys);
			kmem_free(kernel_map, copy, size);
			return;
		}
	}
	/*
	 * The boot framebuffer is RAM: read through a write-combined (normal, uncached) mapping, not a device one
	 * (memcpy's paired and possibly unaligned loads are not for device memory). The steps go to the loader log
	 * first, so that a hang (run MDP3, 2026-10-25: no log) shows where it stopped.
	 */
	printf("vayu-mdp: 2: step copy: frame at %#llx mapped at %#lx; mapping the boot framebuffer\n", phys, (unsigned long)copy);
	fb = ml_io_map_wcomb(FB_PHYS, size);
	if (copy == 0 || fb == 0) {
		printf("vayu-mdp: 2: cannot map (copy %#lx, boot framebuffer %#lx): nothing written\n", (unsigned long)copy,
		    (unsigned long)fb);
		return;
	}
	/*
	 * MDP3 (flip) and MDP4 (copy only, vayu_mdp_flip=0) both hung the whole phone with no log: the one thing the
	 * two runs share is touching the loader's fb_spare memory. Each step is said first, so that a run whose log
	 * streams live (no usb_handover) shows the last one that began: a word written to the first and the last page of
	 * the frame and read back, a read of the boot framebuffer, then the frame cleared, then the copy.
	 */
	{
		volatile uint32_t *first = (volatile uint32_t *)copy, *last = (volatile uint32_t *)(copy + size - 4);
		volatile const uint32_t *src = (volatile const uint32_t *)fb;
		uint32_t sum = 0;

		printf("vayu-mdp: 2: step touch: the frame's first word at %#llx: writing\n", phys);
		*first = 0x5a5a1234u;
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("vayu-mdp: 2: step touch: first word reads %#x; the last word at %#llx: writing\n", *first,
		    phys + size - 4);
		*last = 0xa5a54321u;
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("vayu-mdp: 2: step touch: last word reads %#x; reading the boot framebuffer's first 4 KB\n", *last);
		for (unsigned i = 0; i < 1024; i++) {
			sum += src[i];
		}
		printf("vayu-mdp: 2: step touch: boot framebuffer sum %#x; clearing the frame (%lu bytes)\n", sum,
		    (unsigned long)size);
		bzero((void *)copy, size);
		printf("vayu-mdp: 2: step touch: cleared; copying\n");
	}
	memcpy((void *)copy, (const void *)fb, FRAME_BYTES);
	printf("vayu-mdp: 2: step copy: done\n");
	PE_parse_boot_argn("vayu_mdp_mark", &mark, sizeof(mark));
	if (mark) {
		for (unsigned y = 40; y < 200; y++) {
			for (unsigned x = 40; x < 200; x++) {
				((uint32_t *)copy)[y * 1080 + x] = 0xffff00ff;
			}
		}
	}
	if (spare_used) {
		__asm__ volatile ("dsb sy" ::: "memory");      /* (write-combined: drained, nothing cached) */
	} else {
		flush_dcache(copy, (unsigned)size, 0);
	}
	printf("vayu-mdp: 2: a copy of the frame at %#llx (%lu bytes, contiguous, %s)%s; flipping VIG0 to it for 10 s\n", phys,
	    (unsigned long)size, spare_used ? "the loader's fb_spare" : "allocated here", mark ? ", with a square at (40,40)" : "");
	PE_parse_boot_argn("vayu_mdp_flip", &flip, sizeof(flip));
	if (flip == 0) {
		printf("vayu-mdp: 2: vayu_mdp_flip=0: the copy only, VIG0 not touched\n");
		return;
	}
	printf("vayu-mdp: 2: step flip: VIG0 SRC0_ADDR <- %#llx\n", phys);
	flipped = true;
	if (!point((uint32_t)phys, "to the copy")) {
		point((uint32_t)FB_PHYS, "back (the flip was not taken)");
		flipped = false;
		return;                 /* (the copy is kept: the hardware may still be about to read it) */
	}
	IOSleep(10000);
	if (point((uint32_t)FB_PHYS, "back to the boot framebuffer")) {
		flipped = false;
		IOSleep(100);
		if (!spare_used) {
			kmem_free(kernel_map, copy, size);
		}
	}
	printf("vayu-mdp: 2: done%s\n", flipped ? ": THE FLIP BACK WAS NOT TAKEN (put back again at the run's end)" : "");
}

/*
 * Zero-copy scanout (MiniIOMobileFramebuffer, fb_zero=1): VIG0 fetches a surface of the scanout pool directly. Only the
 * source address, the source stride and the source offset change (the crop of a 1125x2436 surface to the panel's 1080x2400
 * centre); SRC_SIZE and OUT_SIZE stay 1080x2400 as UEFI left them. Registers (mainline dpu_hw_sspp.c:21-29,
 * dpu_hw_sspp.h:357-380): SRC_XY +0x08 = y << 16 | x, SRC0_ADDR +0x14, SRC_YSTRIDE0 +0x24 (plane 0 in bits 15:0), then
 * CTL0 FLUSH bit 0 as the flip above. The flush is waited for, bounded (it is taken at the next frame start: 8.3 ms at
 * 120 Hz): true once taken. Callers serialise (the framebuffer's one display thread).
 */
#define VIG0_SRC_XY             (SSPP_VIG(0) + 0x08)
#define VIG0_SRC_YSTRIDE0       (SSPP_VIG(0) + 0x24)
static uint32_t boot_ystride0, boot_src_xy;
static bool boot_saved, scanning_out;

static bool
scanout_flush(void)
{
	wr(CTL0_FLUSH, FLUSH_VIG0);
	for (unsigned us = 0; us < 50000; us += 100) {
		if ((rd(CTL0_FLUSH) & FLUSH_VIG0) == 0) {
			return true;
		}
		delay(100);
	}
	return false;
}

bool
vayu_mdp_scanout(uint64_t phys, uint32_t stride, uint32_t x, uint32_t y)
{
	if (mdp == 0 || phys == 0 || phys + (uint64_t)stride * 2436 > (4ULL << 30) || stride < 4320 || stride > 0xffff) {
		return false;
	}
	if (!boot_saved) {
		boot_ystride0 = rd(VIG0_SRC_YSTRIDE0);
		boot_src_xy = rd(VIG0_SRC_XY);
		boot_saved = true;
	}
	wr(VIG0_SRC0_ADDR, (uint32_t)phys);
	wr(VIG0_SRC_YSTRIDE0, (boot_ystride0 & 0xffff0000u) | stride);
	wr(VIG0_SRC_XY, y << 16 | x);
	scanning_out = true;
	flipped = true;                         /* (vayu_mdp_quiesce puts the boot framebuffer back) */
	return scanout_flush();
}

void
vayu_mdp_unscanout(void)
{
	if (mdp == 0 || !scanning_out) {
		return;
	}
	wr(VIG0_SRC0_ADDR, (uint32_t)FB_PHYS);
	wr(VIG0_SRC_YSTRIDE0, boot_ystride0);
	wr(VIG0_SRC_XY, boot_src_xy);
	scanning_out = false;
	flipped = false;
	scanout_flush();
}

/* On the way to the loader (loader_return.c): the boot framebuffer on the panel again, whatever happened. */
void
vayu_mdp_quiesce(void)
{
	if (flipped && mdp != 0) {
		*(volatile uint32_t *)(mdp + VIG0_SRC0_ADDR) = (uint32_t)FB_PHYS;
		if (boot_saved) {               /* (a scanout changed the stride and the offset as well) */
			*(volatile uint32_t *)(mdp + VIG0_SRC_YSTRIDE0) = boot_ystride0;
			*(volatile uint32_t *)(mdp + VIG0_SRC_XY) = boot_src_xy;
		}
		*(volatile uint32_t *)(mdp + CTL0_FLUSH) = FLUSH_VIG0;
		flipped = false;
	}
}

static void
vayu_mdp_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t level = (uint32_t)(uintptr_t)parameter;

	mdp = ml_io_map(MDP_PHYS, MDP_SIZE);
	if (mdp == 0) {
		printf("vayu-mdp: the MDP cannot be mapped\n");
		thread_terminate(current_thread());
	}
	IOSleep(20000);
	look(1);
	if (level >= 2) {
		IOSleep(10000);
		flip_test();
	}
	IOSleep(60000);
	look(2);
	thread_terminate(current_thread());
}

void
vayu_mdp_init(void)
{
	thread_t thread;
	uint32_t level = 0;

	if (!PE_parse_boot_argn("vayu_mdp", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(vayu_mdp_thread, (void *)(uintptr_t)level, &thread) != KERN_SUCCESS) {
		printf("vayu-mdp: the thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}
