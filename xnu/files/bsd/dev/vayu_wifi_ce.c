/*
 * W15-A: the WCN3990 host copy engines, up to one real HTC_MSG_READY.
 *
 * Scope (deliberately small): after WLAN_CFG + WLAN_MODE MISSION have been
 * acknowledged, wait 100 ms, allocate one contiguous DMA pool, map it in the
 * WLAN stage-1 SMMU context, program the 12 host copy engines from
 * host_ce_config_wlan, post 32 receive buffers on every host RX CE, and poll
 * (bounded, no interrupts) for the firmware's HTC READY on CE2.  Nothing else:
 * no HTC CONNECT, no WMI, no HTT, no scan, no ifnet.
 *
 * Memory coherency (see W15-report.md for the full proof):
 *   - The kernel VA from kmem_alloc_contig() is Normal write-back cacheable
 *     (CACHE_ATTRINDX_DEFAULT = CACHE_ATTRINDX_WRITEBACK, osfmk/arm64/proc_reg.h:
 *     1114, 1122).  The SMMU leaf uses WLAN_PAGE_NC, i.e. AttrIndx 0 with
 *     MAIR0 byte 0 = 0x44 (Normal Non-cacheable), outer shareable: that
 *     attribute governs *device* accesses only and says nothing about the CPU
 *     cache.  It is not sufficient.
 *   - Therefore the zeroing done by KMA_ZERO (through the cacheable VA) is
 *     written back and invalidated once with FlushPoC_DcacheRegion(); then every
 *     CPU access to the pool goes through a second mapping made with
 *     ml_io_map_unmappable(pa, size, VM_WIMG_WCOMB).  VM_WIMG_WCOMB is
 *     CACHE_ATTRINDX_WRITECOMB (osfmk/arm/pmap/pmap.c wimg_to_pte), which
 *     start.s:864 sets to MAIR_WRITECOMB = 0x44 = Normal Memory, Inner and
 *     Outer Non-Cacheable (osfmk/arm64/proc_reg.h:1094).  The cacheable VA is
 *     never touched again while the device is alive.
 *   - Ordering: a dsb sy separates pool writes from the CE doorbell write and
 *     follows every CE index read before a descriptor read.  CE registers are
 *     Device-nGnRnE (VM_WIMG_IO on non-DRAM, pmap.c wimg_to_pte).
 *   - FlushPoC_DcacheRegion() is a real clean-and-invalidate in this kernel:
 *     caches_asm.s assembles the non-Apple branch (the .s does not include the
 *     header that sets APPLE_ARM64_ARCH_FAMILY), and the built object contains
 *     "dc civac" + "dsb sy" (W15-work/receipts/caches_asm.dis).  A C-level
 *     CleanPoC_DcacheRegion() would be compiled to just dsb sy on this port,
 *     which is exactly the trap this comment exists to avoid.
 *
 * Lifecycle: once the first CE register is written, the pool is firmware-owned
 * until the rings are stopped; it is never freed while the modem can still DMA
 * (vayu_wifi_ce_teardown() quiesces and keeps the memory mapped).  Before that
 * point every failure frees everything it allocated.
 *
 * Copyright (c) 2026 The Redwood Project.  APSL 2.0.
 */

#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_wifi_ce.h>
#include <sys/vayu_wifi_ce_wire.h>
#include <sys/vayu_wifi_htc.h>
#include <sys/vayu_wifi_htc_connect.h>
#include <sys/vayu_wifi_wmi.h>
#include <sys/vayu_wifi_smmu.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void loader_log_putc(char c);

/* WLAN subsystem register blocks (flame radio.c:151-183, ath10k hw.c wcn3990_regs). */
#define VAYU_CE_BASE            0x18a40000UL    /* 0x18800000 + 0x240000 */
#define VAYU_CE_STRIDE          0x1000U
#define VAYU_CE_MAP_SIZE        0xd000U         /* 12 engines + wrapper 0xc000..0xc008 */
#define VAYU_CE_WRAP_RRI_LO     0xc004U
#define VAYU_CE_WRAP_RRI_HI     0xc008U
#define VAYU_CE_SHADOW_BASE     0x18832000UL
#define VAYU_CE_SHADOW_SIZE     0x1000U
#define VAYU_CE_SHADOW_SR       0x000U          /* + 4*ce: source write index shadow */
#define VAYU_CE_SHADOW_DR       0x030U          /* + 4*ce: destination write index shadow */

/* Per-engine registers, ath10k hw.c wcn3990_ce_regs (offsets inside one 4 KiB block). */
#define CE_SR_BASE_LO           0x00U
#define CE_SR_BASE_HI           0x04U
#define CE_SR_SIZE              0x08U
#define CE_DR_BASE_LO           0x0cU
#define CE_DR_BASE_HI           0x10U
#define CE_DR_SIZE              0x14U
#define CE_CTRL1                0x18U
#define CE_HOST_IE              0x2cU
#define CE_MISC_IE              0x34U
#define CE_MISC                 0x38U
#define CE_SR_WR_IDX            0x3cU
#define CE_DR_WR_IDX            0x40U
#define CE_CUR_SRRI             0x44U
#define CE_CUR_DRRI             0x48U
#define CE_SRC_WM               0x4cU
#define CE_DST_WM               0x50U
#define CE_CTRL1_DMAX           0x0000ffffU
#define CE_CTRL1_SRC_SWAP       (1U << 17)
#define CE_CTRL1_DST_SWAP       (1U << 18)
#define CE_CTRL1_RRI_UPD        (1U << 19)

#define VAYU_CE_MISSION_DELAY_MS 100U           /* flame wland.c:673, a hard minimum */
#define VAYU_CE_POLL_MS          1U
#define VAYU_CE_TIMEOUT_MS       5000U          /* hard cap for the READY wait */
#define VAYU_CE_DRAIN_MAX        16U            /* descriptors per CE per pass */
#define VAYU_CE_LOG_FRAMES       8U             /* bounded frame lines */
#define VAYU_CE_LOG_REJECTS      8U
#define VAYU_CE_QUIESCE_MS       20U
#define VAYU_CE_LOW32_MASK       0xffffffffULL

struct vayu_ce_regs {
	uint32_t sr_wr0, dr_wr0, cur_srri0, cur_drri0;
};

static struct {
	enum vayu_ce_result result;
	bool programmed;                /* at least one CE register was written */
	bool pool_allocated;
	bool pool_mapped;               /* our SMMU leaf entries exist */
	bool quarantined;               /* never free: the firmware may still DMA */
	bool torn_down;
	vm_offset_t ce_va, shadow_va;
	vm_offset_t pool_kva;           /* cacheable, used only for the one-time flush */
	vm_offset_t pool_nc;            /* all CPU access goes through this NC alias */
	uint64_t pool_pa;
	vm_size_t pool_size;
	struct vayu_ce_pool layout;
	struct vayu_ce_regs regs[VAYU_CE_COUNT];
	struct vayu_ce_rx_ring rx[VAYU_CE_COUNT];  /* producer/consumer state, host-tested */
	uint32_t frames, rejects, bytes;
	uint32_t log_frames, log_rejects;
	bool block_down;                /* PFR/early-crash: the CE block may be collapsed */
	uint32_t ce_frames[VAYU_CE_COUNT];
	uint32_t ce_rejects[VAYU_CE_COUNT];
	struct vayu_htc_ready ready;
	uint64_t tx_off;                /* W21: one HTC TX buffer after the layout, 0 = none */
	vayu_ce_abort_fn abort;
} ce;

static int say(const char *format, ...) __printflike(1, 2);
static int
say(const char *format, ...)
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
	printf("%s", line);
	return length;
}

static void
ce_barrier(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t
ce_rd(uint32_t engine, uint32_t offset)
{
	return *(volatile uint32_t *)(ce.ce_va + (vm_offset_t)engine * VAYU_CE_STRIDE + offset);
}

static void
ce_wr(uint32_t engine, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(ce.ce_va + (vm_offset_t)engine * VAYU_CE_STRIDE + offset) = value;
	ce_barrier();
}

static uint32_t
shadow_rd(uint32_t offset)
{
	return *(volatile uint32_t *)(ce.shadow_va + offset);
}

static void *
pool_at(uint64_t offset)
{
	return (void *)(ce.pool_nc + (vm_offset_t)offset);
}

static enum vayu_ce_abort
ce_abort(vayu_ce_abort_fn abort)
{
	return abort != NULL ? abort() : VAYU_CE_ABORT_NONE;
}

static const char *
ce_abort_name(enum vayu_ce_abort why)
{
	switch (why) {
	case VAYU_CE_ABORT_NONE:
		return "none";
	case VAYU_CE_ABORT_STOP:
		return "driver stop";
	case VAYU_CE_ABORT_PFR:
		return "pd-mapper PFR";
	case VAYU_CE_ABORT_CRASH:
		return "SMP2P early-crash-ind";
	default:
		return "unknown";
	}
}

/*
 * R3: a fresh abort check immediately before any failure-path CE MMIO.  A PFR or
 * early-crash may have collapsed the WLAN block between the last poll and now; in
 * that case set block_down and let ce_quiesce() skip every register access.
 * Returns true when the block must be treated as down.
 */
static bool
ce_mark_down_if_fault(vayu_ce_abort_fn abort)
{
	enum vayu_ce_abort why = ce_abort(abort);

	if (why == VAYU_CE_ABORT_PFR || why == VAYU_CE_ABORT_CRASH) {
		ce.block_down = true;
		return true;
	}
	return false;
}

/* ===================================================================================================
 * DMA pool: one contiguous allocation, one layout from the host CE table.
 * =================================================================================================== */

static void
ce_pool_free(void)
{
	if (ce.pool_mapped) {
		/* The caller must have rolled the SMMU leaves back first. */
		say("vayu-wifi: ce: internal: refusing to free a pool the SMMU still maps\n");
		return;
	}
	if (ce.pool_nc != 0) {
		ml_io_unmap(ce.pool_nc, ce.pool_size);
		ce.pool_nc = 0;
	}
	if (ce.pool_kva != 0) {
		kmem_free(kernel_map, ce.pool_kva, ce.pool_size);
		ce.pool_kva = 0;
	}
	ce.pool_allocated = false;
	ce.pool_pa = 0;
}

static bool
ce_pool_alloc(void)
{
	vm_offset_t kva = 0;
	kern_return_t kr;
	ppnum_t max_pnum;
	uint64_t limit_hi;
	uint32_t want[VAYU_CE_COUNT];

	vayu_wmi_rx_post_want(want, VAYU_CE_COUNT, VAYU_CE_RX_POST_WANT);
	if (!vayu_ce_pool_layout_want(vayu_wmi_host_cfg(vayu_ce_host_cfg), want, &ce.layout) ||
	    ce.layout.size == 0) {
		say("vayu-wifi: ce: bad pool layout for host_ce_config_wlan\n");
		return false;
	}
	for (uint32_t i = 0; i < VAYU_CE_COUNT; i++) {
		say("vayu-wifi: ce: layout ce%u src %u dst %u rx_post %u rx_bufsz %u\n", i,
		    ce.layout.src_n[i], ce.layout.dst_n[i], ce.layout.rx_post[i], ce.layout.rx_bufsz[i]);
	}
	ce.pool_size = round_page((vm_size_t)ce.layout.size);
	if (vayu_htcc_mode != 0) {
		ce.tx_off = ce.pool_size;
		ce.pool_size += round_page((vm_size_t)(2 * VAYU_HTCC_TX_MAX));
	}
	if (ce.pool_size == 0 || ce.pool_size > (vm_size_t)VAYU_CE_POOL_MAX ||
	    (uint64_t)ce.pool_size < ce.layout.size) {
		say("vayu-wifi: ce: pool size %llu rounds to an unusable %u\n",
		    (unsigned long long)ce.layout.size, (unsigned int)ce.pool_size);
		return false;
	}
	/* The CE descriptor address field is 35 bits (ce.h:42); stay below 32 GiB. */
	limit_hi = (uint64_t)32 << 30;
	max_pnum = (ppnum_t)((limit_hi >> PAGE_SHIFT) - 1);
	kr = kmem_alloc_contig(kernel_map, &kva, ce.pool_size, PAGE_MASK, max_pnum, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS || kva == 0) {
		say("vayu-wifi: ce: no %u bytes of contiguous DMA memory below 32 GiB (%d)\n",
		    (unsigned int)ce.pool_size, kr);
		return false;
	}
	ce.pool_kva = kva;
	ce.pool_allocated = true;
	/*
	 * KMA_ZERO wrote through the cacheable kernel VA.  Push those lines to
	 * the PoC and invalidate them before the device (or the NC alias) can
	 * look at the memory.  Nothing touches this VA afterwards.
	 */
	FlushPoC_DcacheRegion(ce.pool_kva, ce.pool_size);
	ce_barrier();
	ce.pool_pa = kvtophys(ce.pool_kva) & ~((uint64_t)PAGE_MASK);
	if (ce.pool_pa == 0 || (ce.pool_pa & (uint64_t)PAGE_MASK) != 0 ||
	    ce.pool_pa + (uint64_t)ce.pool_size > limit_hi) {
		say("vayu-wifi: ce: the pool's physical address %#llx is unusable (limit %#llx)\n",
		    (unsigned long long)ce.pool_pa, (unsigned long long)limit_hi);
		ce_pool_free();
		return false;
	}
	ce.pool_nc = ml_io_map_unmappable((vm_offset_t)ce.pool_pa, ce.pool_size, VM_WIMG_WCOMB);
	if (ce.pool_nc == 0) {
		say("vayu-wifi: ce: cannot map the pool at %#llx non-cacheable\n",
		    (unsigned long long)ce.pool_pa);
		ce_pool_free();
		return false;
	}
	say("vayu-wifi: ce: pool %u bytes (layout %llu), cacheable VA %#llx, PA %#llx+%#x, "
	    "NC alias %#llx; the cacheable VA was flushed once and is never used again\n",
	    (unsigned int)ce.pool_size, (unsigned long long)ce.layout.size,
	    (unsigned long long)ce.pool_kva, (unsigned long long)ce.pool_pa,
	    (unsigned int)ce.pool_size, (unsigned long long)ce.pool_nc);
	return true;
}

/* ===================================================================================================
 * Copy-engine programming.
 * =================================================================================================== */

static bool
ce_program(void)
{
	uint32_t ce_id, value, ffs_lo = 0, ffs_hi = 0, rri_lo, rri_hi;
	uint64_t rri_pa = ce.pool_pa + ce.layout.rri_off;
	bool ok = true;

	/* From this line on, rollback is not a free: the device owns the pool. */
	ce.programmed = true;
	rri_lo = (uint32_t)(rri_pa & VAYU_CE_LOW32_MASK);
	rri_hi = (uint32_t)((rri_pa >> 32) & VAYU_CE_ADDR_HI_MASK);
	ce_wr(0, VAYU_CE_WRAP_RRI_LO, rri_lo);
	ce_wr(0, VAYU_CE_WRAP_RRI_HI, rri_hi);
	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		value = ce_rd(ce_id, CE_CTRL1);
		ce_wr(ce_id, CE_CTRL1, value | CE_CTRL1_RRI_UPD);
	}
	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		uint32_t src_n = ce.layout.src_n[ce_id];
		uint32_t dst_n = ce.layout.dst_n[ce_id];
		uint64_t src_pa = ce.pool_pa + ce.layout.src_off[ce_id];
		uint64_t dst_pa = ce.pool_pa + ce.layout.dst_off[ce_id];

		ce.regs[ce_id].sr_wr0 = 0;
		ce.regs[ce_id].dr_wr0 = 0;
		ce.regs[ce_id].cur_srri0 = 0;
		ce.regs[ce_id].cur_drri0 = 0;
		if (src_n != 0) {
			ce.regs[ce_id].sr_wr0 = ce_rd(ce_id, CE_SR_WR_IDX) & (src_n - 1);
			ce.regs[ce_id].cur_srri0 = ce_rd(ce_id, CE_CUR_SRRI) & (src_n - 1);
			ce_wr(ce_id, CE_SR_BASE_LO, (uint32_t)(src_pa & VAYU_CE_LOW32_MASK));
			ce_wr(ce_id, CE_SR_BASE_HI, (uint32_t)((src_pa >> 32) & VAYU_CE_ADDR_HI_MASK));
			ce_wr(ce_id, CE_SR_SIZE, src_n);
			value = ce_rd(ce_id, CE_CTRL1);
			ce_wr(ce_id, CE_CTRL1, (value & ~(CE_CTRL1_DMAX | CE_CTRL1_SRC_SWAP)) |
			    (uint32_t)vayu_ce_host_cfg[ce_id].src_sz_max);
			ce_wr(ce_id, CE_SRC_WM, src_n);         /* low 0, high src_n */
		}
		if (dst_n != 0) {
			ce.regs[ce_id].dr_wr0 = ce_rd(ce_id, CE_DR_WR_IDX) & (dst_n - 1);
			ce.regs[ce_id].cur_drri0 = ce_rd(ce_id, CE_CUR_DRRI) & (dst_n - 1);
			ce_wr(ce_id, CE_DR_BASE_LO, (uint32_t)(dst_pa & VAYU_CE_LOW32_MASK));
			ce_wr(ce_id, CE_DR_BASE_HI, (uint32_t)((dst_pa >> 32) & VAYU_CE_ADDR_HI_MASK));
			ce_wr(ce_id, CE_DR_SIZE, dst_n);
			value = ce_rd(ce_id, CE_CTRL1);
			ce_wr(ce_id, CE_CTRL1, value & ~CE_CTRL1_DST_SWAP);
			ce_wr(ce_id, CE_DST_WM, dst_n);
		}
		say("vayu-wifi: ce: ce%u src %u/%u dst %u/%u sr_wr %u dr_wr %u srri %u drri %u ctrl1 %#x\n",
		    ce_id, src_n, (unsigned int)vayu_ce_host_cfg[ce_id].src_sz_max,
		    dst_n, (unsigned int)vayu_ce_host_cfg[ce_id].src_sz_max,
		    ce.regs[ce_id].sr_wr0, ce.regs[ce_id].dr_wr0,
		    ce.regs[ce_id].cur_srri0, ce.regs[ce_id].cur_drri0,
		    ce_rd(ce_id, CE_CTRL1));
	}
	/*
	 * Readback: every ring base/size and the wrapper RRI pointer must be
	 * exactly what was written.  A mismatch is a hard stop; the caller
	 * quiesces and keeps the memory.
	 */
	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		uint32_t src_n = ce.layout.src_n[ce_id];
		uint32_t dst_n = ce.layout.dst_n[ce_id];

		ffs_lo = 0;
		ffs_hi = 0;
		if (src_n != 0) {
			uint64_t src_pa = ce.pool_pa + ce.layout.src_off[ce_id];

			ffs_lo = ce_rd(ce_id, CE_SR_BASE_LO);
			ffs_hi = ce_rd(ce_id, CE_SR_BASE_HI);
			if (ffs_lo != (uint32_t)(src_pa & VAYU_CE_LOW32_MASK) ||
			    ffs_hi != (uint32_t)((src_pa >> 32) & VAYU_CE_ADDR_HI_MASK) ||
			    ce_rd(ce_id, CE_SR_SIZE) != src_n ||
			    (ce_rd(ce_id, CE_CTRL1) & CE_CTRL1_DMAX) !=
			    (uint32_t)vayu_ce_host_cfg[ce_id].src_sz_max) {
				say("vayu-wifi: ce: ce%u src readback failed: base %#x_%08x size %u ctrl1 %#x\n",
				    ce_id, ffs_hi, ffs_lo, ce_rd(ce_id, CE_SR_SIZE),
				    ce_rd(ce_id, CE_CTRL1));
				ok = false;
			}
		}
		ffs_lo = 0;
		ffs_hi = 0;
		if (dst_n != 0) {
			uint64_t dst_pa = ce.pool_pa + ce.layout.dst_off[ce_id];

			ffs_lo = ce_rd(ce_id, CE_DR_BASE_LO);
			ffs_hi = ce_rd(ce_id, CE_DR_BASE_HI);
			if (ffs_lo != (uint32_t)(dst_pa & VAYU_CE_LOW32_MASK) ||
			    ffs_hi != (uint32_t)((dst_pa >> 32) & VAYU_CE_ADDR_HI_MASK) ||
			    ce_rd(ce_id, CE_DR_SIZE) != dst_n) {
				say("vayu-wifi: ce: ce%u dst readback failed: base %#x_%08x size %u\n",
				    ce_id, ffs_hi, ffs_lo, ce_rd(ce_id, CE_DR_SIZE));
				ok = false;
			}
		}
		if ((ce_rd(ce_id, CE_CTRL1) & CE_CTRL1_RRI_UPD) == 0) {
			say("vayu-wifi: ce: ce%u CTRL1_RRI_UPD did not stick (ctrl1 %#x)\n",
			    ce_id, ce_rd(ce_id, CE_CTRL1));
			ok = false;
		}
	}
	if (ce_rd(0, VAYU_CE_WRAP_RRI_LO) != rri_lo || ce_rd(0, VAYU_CE_WRAP_RRI_HI) != rri_hi) {
		say("vayu-wifi: ce: wrapper RRI readback failed: %#x/%#x, wanted %#x/%#x\n",
		    ce_rd(0, VAYU_CE_WRAP_RRI_HI), ce_rd(0, VAYU_CE_WRAP_RRI_LO), rri_hi, rri_lo);
		ok = false;
	}
	if (ok) {
		say("vayu-wifi: ce: 12 engines programmed and read back (RRI array %#llx, %u bytes)\n",
		    (unsigned long long)rri_pa, (unsigned int)(VAYU_CE_COUNT * sizeof(uint32_t)));
	}
	return ok;
}

/* ===================================================================================================
 * RX posting, polling and the HTC READY proof.
 *
 * The producer/consumer index machine lives in vayu_wifi_ce_wire.c so the host tests
 * drive the very same code: every slot in [sw, wr) must carry a descriptor the host
 * wrote for one of the `post` buffers.  C1 was re-posting at the consumed slot while
 * advancing wr, which advertises a never-written descriptor (addr 0).
 * =================================================================================================== */

static bool
ce_post_all(void)
{
	uint32_t ce_id;

	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		uint32_t post = ce.layout.rx_post[ce_id];
		uint32_t dst_n = ce.layout.dst_n[ce_id];
		struct vayu_ce_rx_ring *ring = &ce.rx[ce_id];
		uint32_t i;

		if (post == 0) {
			continue;
		}
		/*
		 * R3: the reset/initial indices must describe the same point.  If the CE's
		 * write index and the host's write index differ, the window is unknown
		 * and advertising anything could expose never-written slots.  Refuse
		 * cleanly instead of trusting a surprising reset value.
		 */
		if (ce.regs[ce_id].cur_drri0 != ce.regs[ce_id].dr_wr0) {
			say("vayu-wifi: ce: ce%u initial indices disagree: CUR_DRRI %u != DR_WR %u; "
			    "refusing to advertise buffers\n", ce_id, ce.regs[ce_id].cur_drri0,
			    ce.regs[ce_id].dr_wr0);
			return false;
		}
		if (!vayu_ce_rx_ring_init(ring, (uint8_t *)pool_at(ce.layout.dst_off[ce_id]),
		    ce.pool_pa + ce.layout.rx_off[ce_id], dst_n, post,
		    ce.layout.rx_bufsz[ce_id], ce.regs[ce_id].cur_drri0,
		    ce.regs[ce_id].dr_wr0)) {
			say("vayu-wifi: ce: ce%u ring init failed (nentries %u post %u sw %u wr %u)\n",
			    ce_id, dst_n, post, ce.regs[ce_id].cur_drri0, ce.regs[ce_id].dr_wr0);
			return false;
		}
		for (i = 0; i < post; i++) {
			if (!vayu_ce_rx_ring_post(ring)) {
				say("vayu-wifi: ce: ce%u post %u/%u failed\n", ce_id, i, post);
				return false;
			}
		}
		/* R3: prove every advertised slot was written before the first doorbell. */
		{
			uint32_t bad = 0;

			if (!vayu_ce_rx_ring_window_ok(ring, &bad)) {
				say("vayu-wifi: ce: ce%u advertised window [%u,%u) is not "
				    "host-written (first bad slot %u); not writing DR_WR_IDX\n",
				    ce_id, ring->sw, ring->wr, bad);
				return false;
			}
		}
		ce_barrier();
		ce_wr(ce_id, CE_DR_WR_IDX, ring->wr);
		say("vayu-wifi: ce: posted %u buffers on ce%u (%u-byte buffers, sw %u, dr_wr %u, drri %u)\n",
		    post, ce_id, ce.layout.rx_bufsz[ce_id], ring->sw, ring->wr,
		    ce_rd(ce_id, CE_CUR_DRRI));
		if (ce_id == 5 && vayu_wmi_mode == 8) {
			say("vayu-wifi: w21h: ce5 posted %u x %u bytes (ring %u, %u pool buffers)\n",
			    post, ce.layout.rx_bufsz[ce_id], dst_n, ring->nbuf);
		}
	}
	return true;
}

static void
ce_note_frame(uint32_t ce_id, uint32_t len, const struct vayu_htc_frame *frame,
    enum vayu_htc_reject why)
{
	if (ce.frames + ce.rejects < VAYU_CE_LOG_FRAMES) {
		say("vayu-wifi: ce: ce%u frame %u bytes eid %u flags %#x htc_len %u trailer %u msg %u: %s\n",
		    ce_id, len, (unsigned int)frame->eid, (unsigned int)frame->flags,
		    (unsigned int)frame->payload_len, (unsigned int)frame->trailer_len,
		    (unsigned int)frame->msg_id, vayu_htc_reject_name(why));
	}
}

/* Called by vayu_ce_rx_ring_drain() after the slot was consumed and before it is re-posted. */
static bool
ce_frame(void *context, uint32_t slot, const struct vayu_ce_desc *desc,
    enum vayu_ce_rx_verdict verdict)
{
	uint32_t ce_id = *(const uint32_t *)context;
	struct vayu_htc_frame frame;
	enum vayu_htc_reject why;
	uint64_t buffer_off;
	const uint8_t *payload;

	if (verdict != VAYU_CE_RX_OK) {
		ce.rejects++;
		ce.ce_rejects[ce_id]++;
		if (ce.log_rejects < VAYU_CE_LOG_REJECTS) {
			ce.log_rejects++;
			say("vayu-wifi: ce: ce%u slot %u rejected: %s nbytes %u addr %#llx\n",
			    ce_id, slot,
			    verdict == VAYU_CE_RX_TOO_BIG ? "frame longer than the buffer" :
			    "descriptor address outside the posted buffers",
			    (unsigned int)desc->nbytes, (unsigned long long)desc->addr);
		}
		return false;
	}
	ce.frames++;
	ce.ce_frames[ce_id]++;
	ce.bytes += desc->nbytes;
	buffer_off = desc->addr - (ce.pool_pa + ce.layout.rx_off[ce_id]);
	payload = (const uint8_t *)pool_at(ce.layout.rx_off[ce_id] + buffer_off);
	if (vayu_htcc_active) {
		vayu_htcc_rx(ce_id, payload, desc->nbytes);
		return false;
	}
	why = vayu_htc_parse_frame(payload, desc->nbytes, &frame);
	if (why == VAYU_HTC_OK && vayu_htc_relaxed &&
	    (frame.flags & (uint8_t)~VAYU_HTC_FLAG_KNOWN) != 0) {
		say("vayu-wifi: htc: ce%u eid %u flags %#x: undefined-htc-flag-bits (logged, accepted)\n",
		    ce_id, (unsigned int)frame.eid, (unsigned int)frame.flags);
	}
	if (why == VAYU_HTC_OK && vayu_htc_relaxed && ce_id != VAYU_HTC_READY_CE &&
	    frame.msg_id == VAYU_HTC_MSG_READY) {
		say("vayu-wifi: htc: READY candidate on ce%u: not-ce2 (logged, accepted)\n", ce_id);
	}
	if (why == VAYU_HTC_OK) {
		struct vayu_htc_ready candidate;

		if (vayu_htc_lenient && frame.msg_id == VAYU_HTC_MSG_READY) {
			char hex[3 * VAYU_HTC_CTRL_MAX + 1];

			for (uint32_t i = 0; i < frame.ctrl_len; i++) {
				snprintf(hex + 3 * i, 4, " %02x", (unsigned int)frame.ctrl[i]);
			}
			hex[3 * frame.ctrl_len] = 0;
			say("vayu-wifi: htc: READY raw ctrl (%u of %u body bytes):%s\n",
			    (unsigned int)frame.ctrl_len, (unsigned int)frame.body_len, hex);
		}
		why = vayu_htc_frame_is_ready(ce_id, &frame, &candidate);
		if (why == VAYU_HTC_OK && candidate.raw_endpoints != candidate.max_endpoints) {
			say("vayu-wifi: htc: lenient: firmware max_endpoints %u, clamped to %u\n",
			    (unsigned int)candidate.raw_endpoints, (unsigned int)candidate.max_endpoints);
		}
		if (why == VAYU_HTC_OK) {
			ce.ready = candidate;
			ce.result = VAYU_CE_READY;
			say("vayu-wifi: htc: READY on ce2 credits %u x %u endpoints %u "
			    "htc_version %#x frame %u bytes (real descriptor %u, "
			    "htc eid %u flags %#x len %u trailer %u)\n",
			    candidate.credits, candidate.credit_size,
			    candidate.max_endpoints, candidate.htc_version,
			    candidate.frame_len, desc->nbytes,
			    (unsigned int)frame.eid, (unsigned int)frame.flags,
			    (unsigned int)frame.payload_len,
			    (unsigned int)frame.trailer_len);
			ce_note_frame(ce_id, desc->nbytes, &frame, why);
			return true;
		}
	}
	ce.rejects++;
	ce.ce_rejects[ce_id]++;
	ce_note_frame(ce_id, desc->nbytes, &frame, why);
	return false;
}

static bool
ce_drain(uint32_t ce_id)
{
	struct vayu_ce_rx_ring *ring = &ce.rx[ce_id];
	uint32_t drri;

	ce_barrier();
	drri = ce_rd(ce_id, CE_CUR_DRRI);
	{
		bool outside = false;

		(void)vayu_ce_rx_ring_drain(ring, drri, VAYU_CE_DRAIN_MAX, ce_frame, &ce_id,
		    &outside);
		if (outside) {
			say("vayu-wifi: ce: ce%u CUR_DRRI %u is outside the advertised window "
			    "[sw %u, wr %u); refusing to read or re-post those slots\n",
			    ce_id, drri & (ce.layout.dst_n[ce_id] - 1), ring->sw, ring->wr);
			ce.result = VAYU_CE_BLOCKED;
			return false;
		}
	}
	ce_barrier();
	ce_wr(ce_id, CE_DR_WR_IDX, ring->wr);
	return ce.result == VAYU_CE_READY;
}
static void
ce_diag(uint32_t timeout_ms)
{
	uint32_t ce_id;

	say("vayu-wifi: ce: no HTC READY in %u ms: %u valid frames, %u rejected, %u data bytes; "
	    "%u polls x %u descriptors max per CE\n", timeout_ms, ce.frames, ce.rejects, ce.bytes,
	    timeout_ms / VAYU_CE_POLL_MS, VAYU_CE_DRAIN_MAX);
	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		uint32_t dst_n = ce.layout.dst_n[ce_id];
		uint32_t src_n = ce.layout.src_n[ce_id];
		uint32_t mask, sw, drri, wr, sr_wr, srri, rri_word, next_raw, last_raw;
		struct vayu_ce_desc next, last;
		uint8_t *next_p, *last_p;

		if (dst_n == 0 && src_n == 0) {
			continue;
		}
		mask = dst_n != 0 ? dst_n - 1 : 0;
		sw = dst_n != 0 ? ce.rx[ce_id].sw : 0;
		drri = dst_n != 0 ? ce_rd(ce_id, CE_CUR_DRRI) & mask : 0;
		wr = dst_n != 0 ? ce_rd(ce_id, CE_DR_WR_IDX) & mask : 0;
		sr_wr = src_n != 0 ? ce_rd(ce_id, CE_SR_WR_IDX) : 0;
		srri = src_n != 0 ? ce_rd(ce_id, CE_CUR_SRRI) : 0;
		rri_word = *(volatile uint32_t *)pool_at(ce.layout.rri_off +
		    (uint64_t)ce_id * sizeof(uint32_t));
		if (dst_n != 0) {
			next_p = (uint8_t *)pool_at(ce.layout.dst_off[ce_id] +
			    (uint64_t)sw * VAYU_CE_DESC_SIZE);
			last_p = (uint8_t *)pool_at(ce.layout.dst_off[ce_id] +
			    (uint64_t)((drri + dst_n - 1) & mask) * VAYU_CE_DESC_SIZE);
			vayu_ce_desc_decode(next_p, &next);
			vayu_ce_desc_decode(last_p, &last);
			next_raw = next.nbytes;
			last_raw = last.nbytes;
		} else {
			next.nbytes = 0;
			next.flags = 0;
			next.addr = 0;
			last.nbytes = 0;
			last.flags = 0;
			last.addr = 0;
			next_raw = 0;
			last_raw = 0;
		}
		say("vayu-wifi: ce: ce%u src %u [sr_wr %u srri %u] dst %u [sw %u drri %u dr_wr %u] "
		    "rri %#x shadow_sr %#x shadow_dr %#x next %u bytes addr %#llx flags %#x "
		    "last %u bytes addr %#llx flags %#x\n",
		    ce_id, src_n, sr_wr, srri, dst_n, sw, drri, wr, rri_word,
		    shadow_rd(VAYU_CE_SHADOW_SR + 4 * ce_id),
		    shadow_rd(VAYU_CE_SHADOW_DR + 4 * ce_id),
		    next_raw, (unsigned long long)next.addr, (unsigned int)next.flags,
		    last_raw, (unsigned long long)last.addr, (unsigned int)last.flags);
		if (dst_n != 0) {
			say("vayu-wifi: ce: ce%u misc_ie %#x misc %#x host_ie %#x\n", ce_id,
			    ce_rd(ce_id, CE_MISC_IE), ce_rd(ce_id, CE_MISC),
			    ce_rd(ce_id, CE_HOST_IE));
		}
		if (ce.layout.rx_post[ce_id] != 0) {
			uint32_t bad_addr = 0, bad_idle = 0;
			bool addr_ok = vayu_ce_rx_ring_window_addr_ok(&ce.rx[ce_id], &bad_addr);
			bool idle_ok = vayu_ce_rx_ring_window_ok(&ce.rx[ce_id], &bad_idle);

			/*
			 * The address check is the host invariant.  idle_ok additionally
			 * requires nbytes == 0; at a timeout a non-zero nbytes is expected
			 * for descriptors the CE produced and we did not drain, so it is
			 * reported but is not a failure.
			 */
			say("vayu-wifi: ce: ce%u window [%u,%u) %s\n", ce_id,
			    ce.rx[ce_id].sw, ce.rx[ce_id].wr,
			    addr_ok ? "all advertised slots carry a posted buffer" :
			    "BROKEN: an advertised slot has no posted buffer");
			if (!addr_ok) {
				say("vayu-wifi: ce: ce%u first advertised slot without a posted "
				    "buffer: %u\n", ce_id, bad_addr);
			}
			if (!idle_ok && addr_ok) {
				say("vayu-wifi: ce: ce%u %s (produced, not drained yet)\n", ce_id,
				    "some advertised slots carry CE-produced descriptors");
			}
		}
	}
	vayu_wifi_smmu_fault("ce timeout");
}

static enum vayu_ce_result
ce_poll(vayu_ce_abort_fn abort)
{
	uint32_t waited;

	for (waited = 0; waited < VAYU_CE_TIMEOUT_MS; waited += VAYU_CE_POLL_MS) {
		uint32_t ce_id;
		enum vayu_ce_abort why;

		why = ce_abort(abort);
		if (why != VAYU_CE_ABORT_NONE) {
			say("vayu-wifi: ce: stopping the poll after %u ms: %s\n", waited,
			    ce_abort_name(why));
			/* A PFR/crash may have collapsed the WLAN block: no more MMIO later. */
			if (why != VAYU_CE_ABORT_STOP) {
				ce.block_down = true;
			}
			ce.result = VAYU_CE_BLOCKED;
			return ce.result;
		}
		for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
			if (ce.layout.rx_post[ce_id] == 0) {
				continue;
			}
			if (ce_drain(ce_id)) {
				return VAYU_CE_READY;
			}
			if (ce.result == VAYU_CE_BLOCKED) {
				return ce.result;       /* DRRI outside the advertised window */
			}
		}
		delay_for_interval(VAYU_CE_POLL_MS, NSEC_PER_MSEC);
	}
	/*
	 * R3: the last loop check was before the final 1 ms sleep.  A PFR/crash can
	 * arrive in that gap; a fresh check decides between the normal CE-register
	 * diagnostics and a no-MMIO failure.
	 */
	if (ce_mark_down_if_fault(abort)) {
		ce.result = VAYU_CE_BLOCKED;
		say("vayu-wifi: ce: timeout diagnostics suppressed: the CE block may be down after "
		    "PFR/crash; the SMMU state is read instead\n");
		vayu_wifi_smmu_fault("ce timeout after PFR/crash");
		return ce.result;
	}
	ce.result = VAYU_CE_TIMEOUT;
	ce_diag(VAYU_CE_TIMEOUT_MS);
	return ce.result;
}

/* ===================================================================================================
 * W21: accessors for the MIT connect stage (bsd/dev/vayu_wifi_htc_connect.c).
 * =================================================================================================== */

static void
htcc_shadow_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(ce.shadow_va + offset) = value;
	ce_barrier();
}

static bool
htcc_src_ring(uint32_t ce_id, uint8_t **desc, uint32_t *nentries, uint32_t *wr0)
{
	if (ce_id >= VAYU_CE_COUNT || ce.layout.src_n[ce_id] == 0) {
		return false;
	}
	*desc = (uint8_t *)pool_at(ce.layout.src_off[ce_id]);
	*nentries = ce.layout.src_n[ce_id];
	*wr0 = ce.regs[ce_id].sr_wr0;
	return true;
}

static bool
htcc_poll(void)
{
	uint32_t ce_id;

	enum vayu_ce_abort why = ce_abort(ce.abort);

	if (why != VAYU_CE_ABORT_NONE) {
		if (why != VAYU_CE_ABORT_STOP) {
			ce.block_down = true;
		}
		say("vayu-wifi: htc: connect stage stopped: %s\n", ce_abort_name(why));
		return false;
	}
	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		if (ce.layout.rx_post[ce_id] == 0) {
			continue;
		}
		(void)ce_drain(ce_id);
		if (ce.result == VAYU_CE_BLOCKED) {
			return false;
		}
	}
	return true;
}

static void
htcc_delay_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* W21b: WMI host memory chunks.  Below 4 GiB (the chunk address word is 32-bit), never freed. */
static uint64_t
htcc_dma_alloc(uint32_t size)
{
	vm_offset_t kva = 0;
	vm_size_t bytes = round_page((vm_size_t)size);
	kern_return_t kr;
	uint64_t pa;

	if (size == 0 || size > VAYU_WMI_MEM_TOTAL_MAX) {
		return 0;
	}
	kr = kmem_alloc_contig(kernel_map, &kva, bytes, PAGE_MASK,
	    (ppnum_t)((((uint64_t)1 << 32) >> PAGE_SHIFT) - 1), 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS || kva == 0) {
		say("vayu-wifi: wmi: no %u contiguous bytes below 4 GiB (%d)\n", (unsigned int)bytes, kr);
		return 0;
	}
	FlushPoC_DcacheRegion(kva, bytes);
	ce_barrier();
	pa = kvtophys(kva) & ~((uint64_t)PAGE_MASK);
	if (pa == 0 || pa + (uint64_t)bytes > ((uint64_t)1 << 32)) {
		say("vayu-wifi: wmi: host memory PA %#llx is not below 4 GiB; kept, not used\n",
		    (unsigned long long)pa);
		return 0;
	}
	if (!vayu_wifi_smmu_map_dma(pa, bytes, "wmi host memory")) {
		say("vayu-wifi: wmi: SMMU refused the host memory map; kept, not used\n");
		return 0;
	}
	say("vayu-wifi: wmi: host memory %u bytes at PA %#llx, SMMU-mapped, never freed\n",
	    (unsigned int)bytes, (unsigned long long)pa);
	return pa;
}

static void
ce_htc_connect(void)
{
	struct vayu_htcc_ops ops = {
		.rd = ce_rd,
		.wr = ce_wr,
		.shadow_wr = htcc_shadow_wr,
		.src_ring = htcc_src_ring,
		.tx_buf = (uint8_t *)pool_at(ce.tx_off),
		.tx_pa = ce.pool_pa + ce.tx_off,
		.poll = htcc_poll,
		.delay_ms = htcc_delay_ms,
		.dma_alloc = htcc_dma_alloc,
		.log = say,
	};

	if (ce.tx_off == 0) {
		say("vayu-wifi: htc: connect stage skipped: no TX buffer in the pool\n");
		return;
	}
	(void)vayu_htcc_run(&ops);
	vayu_wifi_smmu_fault("htc connect");
}

/* ===================================================================================================
 * Stop / teardown.
 * =================================================================================================== */

static void
ce_quiesce(const char *why)
{
	uint32_t ce_id;
	bool ok = true;

	/*
	 * A pd-mapper PFR or an SMP2P early-crash may have collapsed the WLAN
	 * power/clock domain.  Touching the CE registers then is the same class
	 * of access flame warns about before MISSION ("a CE access hangs the
	 * bus"), so do not issue one.  The pool stays mapped and allocated.
	 * ce.block_down is set by the live abort checks, including the fresh
	 * check immediately before every failure-path call.
	 */
	if (ce.block_down) {
		say("vayu-wifi: ce: %s: the CE block may be down after PFR/crash; no register "
		    "access, rings left as they are, pool retained\n", why);
		return;
	}
	if (ce.ce_va == 0) {
		return;
	}
	/* Take every receive buffer back before the rings lose their base. */
	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		uint32_t dst_n = ce.layout.dst_n[ce_id];

		if (dst_n != 0) {
			ce_wr(ce_id, CE_DR_WR_IDX, ce_rd(ce_id, CE_CUR_DRRI) & (dst_n - 1));
		}
	}
	ce_barrier();
	delay_for_interval(VAYU_CE_QUIESCE_MS, NSEC_PER_MSEC);
	for (ce_id = 0; ce_id < VAYU_CE_COUNT; ce_id++) {
		uint32_t src_n = ce.layout.src_n[ce_id];
		uint32_t dst_n = ce.layout.dst_n[ce_id];

		if (src_n != 0) {
			ce_wr(ce_id, CE_SR_WR_IDX, 0);
			ce_wr(ce_id, CE_SR_BASE_LO, 0);
			ce_wr(ce_id, CE_SR_BASE_HI, 0);
			ce_wr(ce_id, CE_SR_SIZE, 0);
			ce_wr(ce_id, CE_CTRL1, ce_rd(ce_id, CE_CTRL1) & ~CE_CTRL1_DMAX);
			ce_wr(ce_id, CE_SRC_WM, 0);
			if (ce_rd(ce_id, CE_SR_SIZE) != 0 || ce_rd(ce_id, CE_SR_BASE_LO) != 0) {
				ok = false;
			}
		}
		if (dst_n != 0) {
			ce_wr(ce_id, CE_DR_BASE_LO, 0);
			ce_wr(ce_id, CE_DR_BASE_HI, 0);
			ce_wr(ce_id, CE_DR_SIZE, 0);
			ce_wr(ce_id, CE_DST_WM, 0);
			if (ce_rd(ce_id, CE_DR_SIZE) != 0 || ce_rd(ce_id, CE_DR_BASE_LO) != 0) {
				ok = false;
			}
		}
	}
	say("vayu-wifi: ce: rings stopped (%s): base/size readback %s; frames %u rejects %u\n",
	    why, ok ? "all zero" : "MISMATCH", ce.frames, ce.rejects);
}

bool
vayu_wifi_ce_programmed(void)
{
	return ce.programmed;
}

const struct vayu_htc_ready *
vayu_wifi_ce_ready(void)
{
	return ce.result == VAYU_CE_READY ? &ce.ready : NULL;
}

enum vayu_ce_result
vayu_wifi_ce_result(void)
{
	return ce.result;
}

const char *
vayu_wifi_ce_result_name(enum vayu_ce_result result)
{
	switch (result) {
	case VAYU_CE_OFF:
		return "off";
	case VAYU_CE_BLOCKED:
		return "blocked";
	case VAYU_CE_TIMEOUT:
		return "no-ready";
	case VAYU_CE_READY:
		return "ready";
	default:
		return "unknown";
	}
}

void
vayu_wifi_ce_mark_block_down(void)
{
	if (!ce.block_down) {
		ce.block_down = true;
		say("vayu-wifi: ce: a PFR/early-crash was seen after the CE stage; teardown will not "
		    "touch a CE register\n");
	}
}

void
vayu_wifi_ce_teardown(void)
{
	if (ce.torn_down) {
		return;
	}
	ce.torn_down = true;
	if (!ce.programmed) {
		if (ce.pool_allocated && !ce.pool_mapped) {
			ce_pool_free();
			say("vayu-wifi: ce: teardown before any CE write: pool freed\n");
		} else if (ce.pool_allocated) {
			say("vayu-wifi: ce: teardown before any CE write, but the SMMU maps the pool: "
			    "kept %#llx+%#x\n", (unsigned long long)ce.pool_pa,
			    (unsigned int)ce.pool_size);
		}
		return;
	}
	ce_quiesce("teardown");
	/*
	 * The modem may still be alive: its firmware could have an in-flight
	 * transfer or a stale ring pointer.  Freeing the pool or removing the
	 * SMMU leaves would turn that into a fault on live firmware memory, so
	 * both are kept for the rest of this boot.  The rollback is the kernel
	 * (or vayu_wifi_ce=0), not a free here.
	 */
	ce.quarantined = true;
	say("vayu-wifi: ce: pool %#llx+%#x stays allocated and SMMU-mapped (firmware peer may still DMA)\n",
	    (unsigned long long)ce.pool_pa, (unsigned int)ce.pool_size);
}

/* ===================================================================================================
 * Entry point: one bounded W15-A attempt.
 * =================================================================================================== */

enum vayu_ce_result
vayu_wifi_ce_stage(bool qmi_ok, vayu_ce_abort_fn abort)
{
	enum vayu_ce_abort why;

	if (ce.result != VAYU_CE_OFF) {
		return ce.result;
	}
	ce.abort = abort;
	{
		uint32_t connect = 0;

		PE_parse_boot_argn("vayu_wifi_htc_connect", &connect, sizeof(connect));
		vayu_htcc_mode = connect <= 2 ? connect : 0;
		vayu_htc_relaxed = vayu_htcc_mode != 0;
		{
			uint32_t wmi_arg = 0;

			PE_parse_boot_argn("vayu_wifi_wmi", &wmi_arg, sizeof(wmi_arg));
			vayu_wmi_mode = (vayu_htcc_mode != 0 && (wmi_arg <= 6 || wmi_arg == 8)) ? wmi_arg : 0;
			if (wmi_arg != 0 && vayu_htcc_mode == 0) {
				say("vayu-wifi: wmi: vayu_wifi_wmi=%u ignored: needs vayu_wifi_htc_connect\n",
				    wmi_arg);
			} else if (vayu_wmi_mode != 0) {
				say("vayu-wifi: wmi: vayu_wifi_wmi=%u: SERVICE_READY parse, INIT on ce%u, "
				    "READY wait; TX doorbell %s\n", vayu_wmi_mode, VAYU_HTCC_WMI_TX_CE,
				    vayu_wmi_mode >= 2 ? "shadow SR (ath10k shadow_reg_support)" :
				    "CE SR_WR_IDX");
			}
		}
		if (vayu_htcc_mode != 0) {
			say("vayu-wifi: htc: vayu_wifi_htc_connect=%u: CONNECT after READY; not-ce2 and "
			    "undefined-htc-flag-bits are logged, not rejected\n", vayu_htcc_mode);
		}
	}
	if (!qmi_ok) {
		ce.result = VAYU_CE_BLOCKED;
		say("vayu-wifi: ce: not started: WLAN_CFG/WLAN_MODE were not both 0/0\n");
		return ce.result;
	}
	if (!vayu_wifi_smmu_enabled()) {
		ce.result = VAYU_CE_BLOCKED;
		say("vayu-wifi: ce: not started: no WLAN stage-1 context of ours "
		    "(vayu_wifi_smmu=1 must have installed one)\n");
		return ce.result;
	}
	why = ce_abort(abort);
	if (why != VAYU_CE_ABORT_NONE) {
		ce.result = VAYU_CE_BLOCKED;
		say("vayu-wifi: ce: not started: %s is already visible\n", ce_abort_name(why));
		return ce.result;
	}
	say("vayu-wifi: ce: waiting %u ms after WLAN_MODE MISSION before the first CE access\n",
	    VAYU_CE_MISSION_DELAY_MS);
	delay_for_interval(VAYU_CE_MISSION_DELAY_MS, NSEC_PER_MSEC);
	why = ce_abort(abort);
	if (why != VAYU_CE_ABORT_NONE) {
		ce.result = VAYU_CE_BLOCKED;
		say("vayu-wifi: ce: not started: %s arrived during the 100 ms wait\n",
		    ce_abort_name(why));
		if (why != VAYU_CE_ABORT_STOP) {
			ce.block_down = true;
		}
		return ce.result;
	}
	if (!ce_pool_alloc()) {
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	if (!vayu_wifi_smmu_map_dma(ce.pool_pa, ce.pool_size, "ce pool")) {
		say("vayu-wifi: ce: the SMMU refused the pool map; no CE register is touched\n");
		ce_pool_free();
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	ce.pool_mapped = true;
	if (vayu_wmi_mode == 8 && !vayu_wifi_smmu_check_range(ce.pool_pa, ce.pool_size, "w21h ce pool")) {
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	ce.ce_va = ml_io_map_unmappable((vm_offset_t)VAYU_CE_BASE, VAYU_CE_MAP_SIZE, VM_WIMG_IO);
	ce.shadow_va = ml_io_map_unmappable((vm_offset_t)VAYU_CE_SHADOW_BASE, VAYU_CE_SHADOW_SIZE,
	    VM_WIMG_IO);
	if (ce.ce_va == 0 || ce.shadow_va == 0) {
		say("vayu-wifi: ce: cannot map the CE/shadow registers; no CE register is touched\n");
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	say("vayu-wifi: ce: registers at %#x (12 x %#x) and shadow %#x\n",
	    (unsigned int)VAYU_CE_BASE, (unsigned int)VAYU_CE_STRIDE,
	    (unsigned int)VAYU_CE_SHADOW_BASE);
	/* H2: a PFR during allocation/SMMU/register mapping must not reach a CE write. */
	why = ce_abort(abort);
	if (why != VAYU_CE_ABORT_NONE) {
		say("vayu-wifi: ce: %s before the first CE register; pool %s\n", ce_abort_name(why),
		    ce.pool_mapped ? "kept (SMMU-mapped, no CE write)" : "freed");
		if (why != VAYU_CE_ABORT_STOP) {
			ce.block_down = true;
		}
		if (!ce.pool_mapped) {
			ce_pool_free();
		}
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	if (!ce_program()) {
		say("vayu-wifi: ce: programming readback failed; stopping the rings\n");
		(void)ce_mark_down_if_fault(abort);
		ce_quiesce("programming failure");
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	/* H2: and not between programming the rings and advertising RX buffers. */
	why = ce_abort(abort);
	if (why != VAYU_CE_ABORT_NONE) {
		say("vayu-wifi: ce: %s after programming and before any RX buffer; no DMA to the "
		    "pool is possible, rings and pool kept\n", ce_abort_name(why));
		if (why != VAYU_CE_ABORT_STOP) {
			ce.block_down = true;
		}
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	if (!ce_post_all()) {
		say("vayu-wifi: ce: RX posting failed; stopping the rings\n");
		(void)ce_mark_down_if_fault(abort);
		ce_quiesce("post failure");
		ce.result = VAYU_CE_BLOCKED;
		return ce.result;
	}
	{
		uint32_t addrmask = 0;

		PE_parse_boot_argn("vayu_wifi_ce_addrmask", &addrmask, sizeof(addrmask));
		vayu_ce_rx_addr_mask = addrmask != 0;
	}
	{
		uint32_t lenient = 0;

		PE_parse_boot_argn("vayu_wifi_htc_lenient", &lenient, sizeof(lenient));
		vayu_htc_lenient = lenient != 0;
		if (vayu_htc_lenient) {
			say("vayu-wifi: htc: lenient 1: READY endpoints above %u are clamped, raw ctrl printed\n",
			    VAYU_HTC_EP_COUNT);
		}
	}
	say("vayu-wifi: ce: polling for HTC READY (cap %u ms, %u ms period, %u descriptors/CE/pass; "
	    "addrmask %u)\n", VAYU_CE_TIMEOUT_MS, VAYU_CE_POLL_MS, VAYU_CE_DRAIN_MAX,
	    vayu_ce_rx_addr_mask ? 1 : 0);
	if (!vayu_ce_rx_addr_mask) {
		enum vayu_ce_result r = ce_poll(abort);

		if (r == VAYU_CE_READY && vayu_htcc_mode != 0) {
			ce_htc_connect();
		}
		return r;
	}
	{
		enum vayu_ce_result r = ce_poll(abort);

		if (r == VAYU_CE_READY && vayu_htcc_mode != 0) {
			ce_htc_connect();
		}

		say("vayu-wifi: ce: addrmask: last raw descriptor address with flag bits %#llx "
		    "(masked to %#llx)\n", (unsigned long long)vayu_ce_rx_addr_hi_seen,
		    (unsigned long long)(vayu_ce_rx_addr_hi_seen & VAYU_CE_ADDR_MASK));
		vayu_wifi_smmu_fault(r == VAYU_CE_READY ? "htc ready" : "htc poll");
		return r;
	}
}
