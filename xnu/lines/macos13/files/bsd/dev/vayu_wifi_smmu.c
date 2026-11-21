/*
 * WCN399x SMMU on vayu (SM8150), before the modem starts the WLAN firmware.
 *
 * Stock icnss attaches 0x640/0x641 to one stage-1 translation context. Its host
 * DMA aperture is 0xa0000000+0x10000000; it does not identity-map all of DDR.
 * This first firmware experiment maps only the exact 1.5 MiB MSA reservation,
 * identity and 4 KiB at a time, because the CAL_REPORT PFR is the hypothesis
 * being tested. No host ring is handed out here and no existing WLAN stream is
 * overwritten. Register and page-table readback are mandatory.
 *
 * The translation only exists if the ANOC2 TBU, whose stream-id range is
 * 0x400..0x7ff and which covers 0x640/0x641, is powered: DT's anoc_2_tbu has
 * vdd-supply = hlos1_vote_aggre_noc_mmu_tbu2_gdsc (qcom,gdsc@0x17d048).  Stock
 * Linux enables that GDSC through the TBU's regulator when it attaches the
 * device.  This file enables it the same way (clear SW_COLLAPSE, poll PWR_ON)
 * instead of giving up when it is off, which is what the first W6 build did.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_wifi_smmu.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>

#define APPS_SMMU               0x15000000
#define SMMU_SIZE               0x100000
#define GCC_TBU2_GDSCR          0x17d048        /* DT qcom,gdsc@0x17d048: hlos1_vote_aggre_noc_mmu_tbu2_gdsc */
#define GCC_TBU2_GDSCR_OFF      (GCC_TBU2_GDSCR & (PAGE_SIZE - 1))
#define GDSC_SW_COLLAPSE        (1u << 0)       /* gdsc.c: SW_COLLAPSE_MASK */
#define GDSC_PWR_ON             (1u << 31)      /* gdsc.c: PWR_ON_MASK */
#define GDSC_TIMEOUT_MS         100             /* gdsc.c: TIMEOUT_US 100 */
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R_64_BIT       1
#define PTE_ADDRESS             0x0000fffffffff000ULL
/* AttrIndx 0 (Normal non-cacheable), AP[1], AF, outer-shareable, non-global. */
#define WLAN_PAGE_NC            (3ULL | 1ULL << 6 | 2ULL << 8 | 1ULL << 10 | 1ULL << 11)
#define WLAN_STREAM             0x640
#define WLAN_STREAM_MASK        1
#define WLAN_MSA_PA             0x8bc00000ULL
#define WLAN_MSA_SIZE           0x180000ULL

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_log_putc(char c);
extern lck_mtx_t hakios_smmu_lock;

static int say(const char *format, ...) __printflike(1, 2);
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) loader_log_putc(*c);
	printf("%s", line);
	return length;
}

static struct {
	bool enabled, on, changed;
	vm_offset_t base, second, context, tables;
	uint32_t match, context_id, old_smr, old_s2cr, old_sctlr;
} wlan;

/*
 * vayu_wifi_smmu_ttbrfix=1: take the context's TTBR0 from the table pages after
 * wlan_tables() has allocated them.  Default 0 keeps the W15-A behaviour, where the
 * root was computed while wlan.tables was still 0, so TTBR0 held only the ASID
 * (runs 172514..151133: "root 0x4000000000000").
 */
static uint32_t wlan_ttbrfix;

static uint32_t
rd(vm_offset_t at)
{
	return *(volatile uint32_t *)at;
}

static void
wr(vm_offset_t at, uint32_t value)
{
	*(volatile uint32_t *)at = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* 0 = no new context may be installed, 1 = allocate one, 2 = a usable
 * translation context already exists and is left completely unchanged. */
static int
wlan_find(uint32_t idr0, uint32_t idr1)
{
	bool used[256] = { false };
	uint32_t matches = idr0 & 0xff, contexts = idr1 & 0xff;
	uint32_t pages = 2U << ((idr1 >> 28) & 7);
	uint32_t page = idr1 >> 31 ? 0x10000 : 0x1000;
	uint32_t seen640 = 0, seen641 = 0, existing_context = 0;
	bool existing = false, existing_trans = true, multiple = false;

	if (contexts > 256 || 2 * pages * page != SMMU_SIZE) {
		say("vayu-wifi: smmu: unexpected geometry: %u contexts, %u pages of %#x\n",
		    contexts, pages, page);
		return 0;
	}
	wlan.match = ~0U;
	wlan.context = 0;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(wlan.base + SMMU_SMR(n));
		uint32_t s2cr = rd(wlan.base + SMMU_S2CR(n));
		uint32_t stream, mask;

		if (!(smr >> 31)) {
			if (wlan.match == ~0U) {
				wlan.match = n;
				wlan.old_smr = smr;
				wlan.old_s2cr = s2cr;
			}
			continue;
		}
		used[s2cr & 0xff] = true;
		stream = smr & 0x7fff;
		mask = (smr >> 16) & 0x7fff;
		say("vayu-wifi: smmu: match %u in use: sid %#x mask %#x s2cr %#x\n",
		    n, stream, mask, s2cr);
		if (((stream ^ 0x640) & ~mask & 0x7fff) == 0) seen640++;
		if (((stream ^ 0x641) & ~mask & 0x7fff) == 0) seen641++;
		if ((((stream ^ 0x640) & ~mask & 0x7fff) == 0) ||
		    (((stream ^ 0x641) & ~mask & 0x7fff) == 0)) {
			if (existing && existing_context != (s2cr & 0xff)) multiple = true;
			existing = true;
			existing_context = s2cr & 0xff;
			existing_trans &= ((s2cr >> 16) & 3) == 0;
			wlan.old_smr = smr;
			wlan.old_s2cr = s2cr;
		}
	}
	if (existing) {
		wlan.context_id = existing_context;
		wlan.second = wlan.base + page;
		wlan.context = wlan.base + (pages + wlan.context_id) * page;
		wlan.old_sctlr = rd(wlan.context + SMMU_CB_SCTLR);
		if (seen640 != 0 && seen641 != 0 && !multiple && existing_trans && (wlan.old_sctlr & 1) &&
		    (rd(wlan.second + SMMU_CBAR(wlan.context_id)) & 0x30000) ==
		    (SMMU_CBAR_STAGE_1 & 0x30000)) {
			wlan.on = true;
			say("vayu-wifi: smmu: existing stage-1 context %u covers both WLAN SIDs; unchanged (cbar %#x)\n",
			    wlan.context_id, rd(wlan.second + SMMU_CBAR(wlan.context_id)));
			return 2;
		}
		say("vayu-wifi: smmu: existing WLAN routing is not a usable shared translation "
		    "(640=%u 641=%u trans=%u multiple=%u sctlr %#x); not overwriting it\n",
		    seen640, seen641, existing_trans, multiple, wlan.old_sctlr);
		return 0;
	}
	for (wlan.context_id = 0; wlan.context_id < contexts; wlan.context_id++) {
		if (used[wlan.context_id]) continue;
		wlan.second = wlan.base + page;
		wlan.context = wlan.base + (pages + wlan.context_id) * page;
		wlan.old_sctlr = rd(wlan.context + SMMU_CB_SCTLR);
		if (!(wlan.old_sctlr & 1)) break;
		say("vayu-wifi: smmu: unreferenced context %u is already on; trying the next\n",
		    wlan.context_id);
	}
	if (wlan.match == ~0U || wlan.context_id >= contexts) {
		say("vayu-wifi: smmu: no free match/context for SID %#x mask %#x\n",
		    WLAN_STREAM, WLAN_STREAM_MASK);
		return 0;
	}
	return 1;
}

static bool
wlan_tables(void)
{
	uint64_t *root, *level2, *level3;
	uint32_t index = (WLAN_MSA_PA >> 21) & 0x1ff;

	if (kmem_alloc(kernel_map, &wlan.tables, 3 * PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || wlan.tables == 0) {
		say("vayu-wifi: smmu: no three pages for MSA tables\n");
		return false;
	}
	root = (uint64_t *)wlan.tables;
	level2 = (uint64_t *)(wlan.tables + PAGE_SIZE);
	level3 = (uint64_t *)(wlan.tables + 2 * PAGE_SIZE);
	root[WLAN_MSA_PA >> 30] = kvtophys((vm_offset_t)level2) | 3;
	level2[index] = kvtophys((vm_offset_t)level3) | 3;
	for (uint64_t offset = 0; offset < WLAN_MSA_SIZE; offset += 4096) {
		uint64_t at = WLAN_MSA_PA + offset;

		level3[(at >> 12) & 0x1ff] = at | WLAN_PAGE_NC;
	}
	for (vm_offset_t line = wlan.tables; line < wlan.tables + 3 * PAGE_SIZE; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line));
	}
	__asm__ volatile("dsb sy" ::: "memory");
	if ((root[WLAN_MSA_PA >> 30] & PTE_ADDRESS) != kvtophys((vm_offset_t)level2) ||
	    (level2[index] & PTE_ADDRESS) != kvtophys((vm_offset_t)level3) ||
	    level3[0] != (WLAN_MSA_PA | WLAN_PAGE_NC) ||
	    level3[(WLAN_MSA_SIZE / 4096) - 1] !=
	    ((WLAN_MSA_PA + WLAN_MSA_SIZE - 4096) | WLAN_PAGE_NC)) {
		say("vayu-wifi: smmu: MSA page-table readback failed\n");
		return false;
	}
	return true;
}

static void
wlan_restore(void)
{
	if (!wlan.changed || wlan.base == 0 || wlan.match == ~0U || wlan.context == 0) return;
	wr(wlan.base + SMMU_SMR(wlan.match), wlan.old_smr);
	wr(wlan.base + SMMU_S2CR(wlan.match), wlan.old_s2cr);
	wr(wlan.context + SMMU_CB_SCTLR, wlan.old_sctlr);
	wlan.on = false;
	wlan.changed = false;
	/* enabled stays set: a second start cannot reuse stale match/context state. */
	say("vayu-wifi: smmu: match %u/context %u restored after failure\n",
	    wlan.match, wlan.context_id);
}

static bool
wlan_on(void)
{
	uint32_t smr = SMMU_SMR(wlan.match), s2cr = SMMU_S2CR(wlan.match);
	uint32_t sees = 1U << 31 | WLAN_STREAM_MASK << 16 | WLAN_STREAM;
	uint64_t root = kvtophys(wlan.tables) | (uint64_t)wlan.context_id << 48;

	if (!wlan_tables()) return false;
	if (wlan_ttbrfix != 0) {
		root = kvtophys(wlan.tables) | (uint64_t)wlan.context_id << 48;
		say("vayu-wifi: smmu: ttbrfix: TTBR0 %#llx (tables pa %#llx)\n",
		    (unsigned long long)root, (unsigned long long)kvtophys(wlan.tables));
		if ((root & PTE_ADDRESS) == 0) {
			say("vayu-wifi: smmu: ttbrfix: tables PA is 0; context not programmed\n");
			return false;
		}
	}
	wlan.changed = true;               /* from here rollback is meaningful */
	wr(wlan.context + SMMU_CB_SCTLR, 0);
	wr(wlan.second + SMMU_CBA2R(wlan.context_id), SMMU_CBA2R_64_BIT);
	wr(wlan.second + SMMU_CBAR(wlan.context_id), SMMU_CBAR_STAGE_1);
	wr(wlan.context + SMMU_CB_TCR2, SMMU_TCR2);
	wr(wlan.context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr(wlan.context + SMMU_CB_TTBR0, (uint32_t)root);
	wr(wlan.context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	wr(wlan.context + SMMU_CB_TTBR1, 0);
	wr(wlan.context + SMMU_CB_TTBR1 + 4, 0);
	wr(wlan.context + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(wlan.context + SMMU_CB_MAIR1, 0);
	wr(wlan.context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(wlan.context + SMMU_CB_TLBIASID, wlan.context_id);
	wr(wlan.context + SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(wlan.context + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("vayu-wifi: smmu: TLB invalidation timed out\n");
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	wr(wlan.context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	wlan.on = true;
	wr(wlan.base + s2cr, wlan.context_id);
	wr(wlan.base + smr, sees);
	if (rd(wlan.base + smr) != sees || (rd(wlan.base + s2cr) & 0x300ff) != wlan.context_id ||
	    (rd(wlan.second + SMMU_CBAR(wlan.context_id)) & 0x30000) !=
	    (SMMU_CBAR_STAGE_1 & 0x30000) || !(rd(wlan.context + SMMU_CB_SCTLR) & 1) ||
	    rd(wlan.context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    rd(wlan.context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		say("vayu-wifi: smmu: readback failed: smr %#x s2cr %#x sctlr %#x ttbr %#x_%08x\n",
		    rd(wlan.base + smr), rd(wlan.base + s2cr), rd(wlan.context + SMMU_CB_SCTLR),
		    rd(wlan.context + SMMU_CB_TTBR0 + 4), rd(wlan.context + SMMU_CB_TTBR0));
		return false;
	}
	say("vayu-wifi: smmu: SID %#x/%#x -> match %u context %u root %#llx; "
	    "MSA %#llx+%#llx identity 4K map read back OK\n", WLAN_STREAM, WLAN_STREAM_MASK,
	    wlan.match, wlan.context_id, (unsigned long long)root, WLAN_MSA_PA, WLAN_MSA_SIZE);
	return true;
}

/*
 * Power the ANOC2 TBU, the interconnect translation buffer that carries stream
 * IDs 0x400..0x7ff (DT anoc_2_tbu@0x15189000).  Its GDSC is one 32-bit GCC
 * register; stock Linux clears SW_COLLAPSE and polls PWR_ON.  A translation
 * context for 0x640 means nothing while the TBU is down, so this is a hard
 * prerequisite, not a best-effort step.  Bounded: 100 x 1 ms.
 */
static bool
wlan_tbu_on(vm_offset_t gcc)
{
	uint32_t before = rd(gcc + GCC_TBU2_GDSCR_OFF), after;
	unsigned waited;

	if (before & GDSC_PWR_ON) {
		say("vayu-wifi: smmu: ANOC2 TBU already on (GCC %#x)\n", before);
		return true;
	}
	say("vayu-wifi: smmu: ANOC2 TBU is off (GCC %#x); enabling: clear SW_COLLAPSE, poll PWR_ON\n", before);
	wr(gcc + GCC_TBU2_GDSCR_OFF, before & ~GDSC_SW_COLLAPSE);
	for (waited = 0; waited < GDSC_TIMEOUT_MS; waited++) {
		after = rd(gcc + GCC_TBU2_GDSCR_OFF);
		if (after & GDSC_PWR_ON) {
			say("vayu-wifi: smmu: ANOC2 TBU on after %u ms (GCC %#x -> %#x)\n", waited + 1, before, after);
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	after = rd(gcc + GCC_TBU2_GDSCR_OFF);
	say("vayu-wifi: smmu: ANOC2 TBU did not power on in %u ms (GCC %#x -> %#x, PWR_ON %u)\n",
	    GDSC_TIMEOUT_MS, before, after, (after & GDSC_PWR_ON) != 0);
	return false;
}

bool
vayu_wifi_smmu_enable(void)
{
	vm_offset_t gcc;
	uint32_t tbu, idr0, idr1;
	int found;
	bool ok = false;

	if (wlan.enabled && wlan.on) return true;
	if (wlan.enabled) return false;
	wlan.enabled = true;                 /* one bounded attempt per boot */
	PE_parse_boot_argn("vayu_wifi_smmu_ttbrfix", &wlan_ttbrfix, sizeof(wlan_ttbrfix));
	wlan.base = ml_io_map_unmappable(APPS_SMMU, SMMU_SIZE, VM_WIMG_IO);
	gcc = ml_io_map_unmappable(GCC_TBU2_GDSCR & ~(PAGE_SIZE - 1), PAGE_SIZE, VM_WIMG_IO);
	if (wlan.base == 0 || gcc == 0) {
		say("vayu-wifi: smmu: cannot map SMMU/GCC registers\n");
		return false;
	}
	if (!wlan_tbu_on(gcc)) {
		return false;
	}
	tbu = rd(gcc + GCC_TBU2_GDSCR_OFF);
	if (rd(wlan.base + SMMU_SCR0) & (1U << 3)) {
		say("vayu-wifi: smmu: 16-bit stream IDs are not supported by this path\n");
		return false;
	}
	idr0 = rd(wlan.base + SMMU_IDR0);
	idr1 = rd(wlan.base + SMMU_IDR1);
	say("vayu-wifi: smmu: TBU on (%#x), scr0 %#x idr0 %#x idr1 %#x\n",
	    tbu, rd(wlan.base + SMMU_SCR0), idr0, idr1);
	lck_mtx_lock(&hakios_smmu_lock);
	found = wlan_find(idr0, idr1);
	if (found == 2 || (found == 1 && wlan_on())) ok = true;
	if (!ok) {
		vayu_wifi_smmu_fault("enable");
		wlan_restore();
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return ok;
}

void
vayu_wifi_smmu_fault(const char *where)
{
	uint64_t far;
	uint32_t fsr;

	if (wlan.base == 0 || wlan.context == 0) return;
	fsr = rd(wlan.context + SMMU_CB_FSR);
	far = (uint64_t)rd(wlan.context + SMMU_CB_FAR + 4) << 32 |
	    rd(wlan.context + SMMU_CB_FAR);
	say("vayu-wifi: smmu after %s: context %u fsr %#x far %#llx fsynr %#x; "
	    "global fsr %#x fsynr %#x %#x\n", where, wlan.context_id, fsr,
	    (unsigned long long)far, rd(wlan.context + SMMU_CB_FSYNR0),
	    rd(wlan.base + SMMU_SGFSR), rd(wlan.base + SMMU_SGFSYNR0),
	    rd(wlan.base + SMMU_SGFSYNR1));
}

/* ===================================================================================================
 * W15-A: extend the live stage-1 tables with one more identity-mapped region.
 *
 * The MSA map, its table pages and its readback are left exactly as wlan_on()
 * built them; the new region gets its own L2/L3 pages only where the existing
 * tree has no table for that 1 GiB / 2 MiB slot.  Every leaf that already exists
 * is refused rather than overwritten, so a second call can never replace the
 * MSA or a previous pool.  A failed map clears only the leaves it installed.
 *
 * The tables are walked and modified through their kernel VAs; the SMMU reads
 * them as Device memory, so every changed table page is cleaned+invalidated to
 * the PoC and the context's TLB is invalidated before the function reports
 * success.  The readback walks the same tables again and also re-checks the
 * first and last MSA leaf.
 * =================================================================================================== */

#define PT_EXTRA_MAX 8

static struct {
	uint64_t pa;
	uint64_t *va;
} pt_extra[PT_EXTRA_MAX];
static uint32_t pt_extra_count;

bool
vayu_wifi_smmu_enabled(void)
{
	return wlan.enabled && wlan.on && wlan.changed && wlan.tables != 0;
}

static uint64_t *
pt_page_va(uint64_t pa)
{
	uint32_t i;

	if (wlan.tables == 0) {
		return NULL;
	}
	if ((kvtophys(wlan.tables) & PTE_ADDRESS) == pa) {
		return (uint64_t *)wlan.tables;
	}
	if ((kvtophys(wlan.tables + PAGE_SIZE) & PTE_ADDRESS) == pa) {
		return (uint64_t *)(wlan.tables + PAGE_SIZE);
	}
	if ((kvtophys(wlan.tables + 2 * PAGE_SIZE) & PTE_ADDRESS) == pa) {
		return (uint64_t *)(wlan.tables + 2 * PAGE_SIZE);
	}
	for (i = 0; i < pt_extra_count; i++) {
		if (pt_extra[i].pa == pa) {
			return pt_extra[i].va;
		}
	}
	return NULL;
}

static uint64_t *
pt_new_page(void)
{
	vm_offset_t page = 0;

	if (pt_extra_count >= PT_EXTRA_MAX) {
		return NULL;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || page == 0) {
		return NULL;
	}
	pt_extra[pt_extra_count].va = (uint64_t *)page;
	pt_extra[pt_extra_count].pa = kvtophys(page) & PTE_ADDRESS;
	pt_extra_count++;
	return (uint64_t *)page;
}

/* Existing leaf slot for pa, or NULL when its table path is not there (never allocates). */
static uint64_t *
pt_lookup(uint64_t pa)
{
	uint64_t *root, *l2, *l3, entry;

	if (wlan.tables == 0) {
		return NULL;
	}
	root = (uint64_t *)wlan.tables;
	entry = root[(pa >> 30) & 0x1ff];
	if ((entry & 3) != 3) {
		return NULL;
	}
	l2 = pt_page_va(entry & PTE_ADDRESS);
	if (l2 == NULL) {
		return NULL;
	}
	entry = l2[(pa >> 21) & 0x1ff];
	if ((entry & 3) != 3) {
		return NULL;
	}
	l3 = pt_page_va(entry & PTE_ADDRESS);
	if (l3 == NULL) {
		return NULL;
	}
	return &l3[(pa >> 12) & 0x1ff];
}

static bool
pt_install(uint64_t pa)
{
	uint64_t *root = (uint64_t *)wlan.tables;
	uint32_t l1 = (uint32_t)((pa >> 30) & 0x1ff);
	uint32_t l2i = (uint32_t)((pa >> 21) & 0x1ff);
	uint32_t l3i = (uint32_t)((pa >> 12) & 0x1ff);
	uint64_t entry = root[l1];
	uint64_t *l2, *l3;

	if (entry == 0) {
		l2 = pt_new_page();
		if (l2 == NULL) {
			return false;
		}
		root[l1] = (kvtophys((vm_offset_t)l2) & PTE_ADDRESS) | 3;
	} else if ((entry & 3) == 3) {
		l2 = pt_page_va(entry & PTE_ADDRESS);
		if (l2 == NULL) {
			return false;
		}
	} else {
		return false;                   /* a block mapping: never split or overwrite */
	}
	entry = l2[l2i];
	if (entry == 0) {
		l3 = pt_new_page();
		if (l3 == NULL) {
			return false;
		}
		l2[l2i] = (kvtophys((vm_offset_t)l3) & PTE_ADDRESS) | 3;
	} else if ((entry & 3) == 3) {
		l3 = pt_page_va(entry & PTE_ADDRESS);
		if (l3 == NULL) {
			return false;
		}
	} else {
		return false;
	}
	if ((l3[l3i] & 3) == 3) {
		return false;                   /* already mapped: refuse, do not replace */
	}
	l3[l3i] = pa | WLAN_PAGE_NC;
	return true;
}

static void
pt_flush_tables(void)
{
	vm_offset_t line;
	uint32_t i;

	/* The three MSA pages are contiguous; the extra pages are PAGE_SIZE each. */
	for (line = wlan.tables; line < wlan.tables + 3 * PAGE_SIZE; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line));
	}
	for (i = 0; i < pt_extra_count; i++) {
		for (line = (vm_offset_t)pt_extra[i].va;
		    line < (vm_offset_t)pt_extra[i].va + PAGE_SIZE; line += 64) {
			__asm__ volatile("dc civac, %0" : : "r"(line));
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static bool
pt_tlbi(void)
{
	uint32_t waited;

	wr(wlan.context + SMMU_CB_TLBIASID, wlan.context_id);
	wr(wlan.context + SMMU_CB_TLBSYNC, 1);
	for (waited = 0; waited < 100; waited++) {
		if ((rd(wlan.context + SMMU_CB_TLBSTATUS) & 1) == 0) {
			return true;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	say("vayu-wifi: smmu: TLB invalidation timed out after the DMA map\n");
	return false;
}

static bool
pt_verify(uint64_t pa)
{
	uint64_t *pte = pt_lookup(pa);

	if (pte == NULL) {
		return false;
	}
	return (*pte & PTE_ADDRESS) == (pa & PTE_ADDRESS) &&
	    (*pte & 0xfff) == (WLAN_PAGE_NC & 0xfff);
}

static bool
pt_clear_range(uint64_t pa, uint64_t size)
{
	bool cleared = false;
	uint64_t at;

	for (at = pa; at - pa < size; at += 4096) {
		uint64_t *pte = pt_lookup(at);

		if (pte != NULL && (*pte & PTE_ADDRESS) == (at & PTE_ADDRESS)) {
			*pte = 0;
			cleared = true;
		}
	}
	return cleared;
}

bool
vayu_wifi_smmu_map_dma(uint64_t pa, uint64_t size, const char *what)
{
	uint32_t tables_before = pt_extra_count;
	uint64_t at;
	bool ok = true;

	if (what == NULL) {
		what = "dma";
	}
	if (!vayu_wifi_smmu_enabled()) {
		say("vayu-wifi: smmu: cannot map %s %#llx+%#llx: no stage-1 context installed by "
		    "this driver (an externally created context is never extended)\n", what,
		    (unsigned long long)pa, (unsigned long long)size);
		return false;
	}
	if (size == 0 || (pa & 0xfff) != 0 || (size & 0xfff) != 0 ||
	    pa > ((uint64_t)1 << 39) || size > ((uint64_t)1 << 39) - pa) {
		say("vayu-wifi: smmu: bad %s range %#llx+%#llx\n", what,
		    (unsigned long long)pa, (unsigned long long)size);
		return false;
	}
	/* Refuse the whole call before touching anything if any leaf already exists. */
	for (at = 0; at < size; at += 4096) {
		uint64_t *pte = pt_lookup(pa + at);

		if (pte != NULL && (*pte & 3) == 3) {
			say("vayu-wifi: smmu: %s %#llx: page %#llx is already mapped; refusing\n",
			    what, (unsigned long long)pa, (unsigned long long)(pa + at));
			return false;
		}
	}
	lck_mtx_lock(&hakios_smmu_lock);
	for (at = 0; at < size; at += 4096) {
		if (!pt_install(pa + at)) {
			say("vayu-wifi: smmu: %s: installing %#llx failed (table budget %u pages)\n",
			    what, (unsigned long long)(pa + at),
			    (unsigned int)(pt_extra_count - tables_before));
			ok = false;
			break;
		}
	}
	if (ok) {
		pt_flush_tables();
		ok = pt_tlbi();
	}
	if (ok) {
		ok = pt_verify(pa) && pt_verify(pa + size - 4096) &&
		    pt_verify(WLAN_MSA_PA) &&
		    pt_verify(WLAN_MSA_PA + WLAN_MSA_SIZE - 4096);
		if (!ok) {
			say("vayu-wifi: smmu: %s: page-table readback failed (first %u last %u "
			    "msa %u/%u)\n", what, pt_verify(pa) ? 1 : 0,
			    pt_verify(pa + size - 4096) ? 1 : 0, pt_verify(WLAN_MSA_PA) ? 1 : 0,
			    pt_verify(WLAN_MSA_PA + WLAN_MSA_SIZE - 4096) ? 1 : 0);
		}
	}
	if (!ok) {
		(void)pt_clear_range(pa, size);
		pt_flush_tables();
		(void)pt_tlbi();
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!ok) {
		return false;
	}
	say("vayu-wifi: smmu: %s %#llx+%#llx identity 4K mapped in context %u "
	    "(%u new table pages): first/last read back, MSA read back OK\n", what,
	    (unsigned long long)pa, (unsigned long long)size, wlan.context_id,
	    (unsigned int)(pt_extra_count - tables_before));
	return true;
}

/* W21h: every 4K page of [pa, pa+size) must be an identity NC leaf in this context. */
bool
vayu_wifi_smmu_check_range(uint64_t pa, uint64_t size, const char *what)
{
	uint64_t at, bad = 0;
	uint32_t pages = 0, missing = 0;

	if (!vayu_wifi_smmu_enabled() || size == 0 || (pa & 0xfff) != 0 || (size & 0xfff) != 0) {
		say("vayu-wifi: smmu: %s: range check refused %#llx+%#llx\n", what,
		    (unsigned long long)pa, (unsigned long long)size);
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	for (at = 0; at < size; at += 4096) {
		pages++;
		if (!pt_verify(pa + at)) {
			if (missing++ == 0) {
				bad = pa + at;
			}
		}
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	say("vayu-wifi: smmu: %s %#llx+%#llx: %u/%u pages identity-mapped in context %u (SID %#x)%s%#llx\n",
	    what, (unsigned long long)pa, (unsigned long long)size, pages - missing, pages,
	    wlan.context_id, WLAN_STREAM, missing ? "; first missing " : "; ok ",
	    (unsigned long long)(missing ? bad : pa + size));
	return missing == 0;
}
