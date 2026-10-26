/*
 * IPA SMMU contexts: what the AP and its uC see of memory, before the modem's IPA driver runs.
 *
 * The application SMMU is at 0x15000000 (Arm MMU-500). vayu DT gives IPA_AP SID 0x520 and
 * IPA_UC SID 0x522. Each gets its own context and a small set of page tables: the first 2 GB
 * of DDR, SMEM and IMEM are identity-mapped, so the IPA can reach the SMEM filter table and
 * the reserved IPA firmware memory. The global SMMU configuration is not changed; a stream
 * match register and a context are used only if they are free. Every write is read back.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <sys/vayu_pas.h>

#define APPS_SMMU               0x15000000
#define SMMU_SIZE               0x100000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SCR0               0x000
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
#define PTE_PAGE                (3ULL | 1ULL << 6 | 2ULL << 8 | 1ULL << 10 | 1ULL << 11)
#define PTE_BLOCK               ((PTE_PAGE & ~3ULL) | 1ULL)  /* 1 GiB/2 MiB block, not table */
#define PTE_DEVICE              (PTE_BLOCK | 2ULL << 2 | 1ULL << 53 | 1ULL << 54)

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_log_putc(char c);
extern lck_mtx_t hakios_smmu_lock;     /* QUP/Wi-Fi/audio/camera allocate the same SMRs/contexts */

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

struct ipa_smmu_stream {
	uint32_t sid, match, context;
	vm_offset_t base, second, context_base;
	vm_offset_t tables;
	bool found, on;
};
static struct ipa_smmu_stream ipa_smmu[2];

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

static bool
ipa_smmu_find(struct ipa_smmu_stream *s, vm_offset_t base, uint32_t idr0, uint32_t idr1)
{
	bool used[256] = { false };
	uint32_t matches = idr0 & 0xff, contexts = idr1 & 0xff, pages = 2U << ((idr1 >> 28) & 7);
	uint32_t page = idr1 >> 31 ? 0x10000 : 0x1000;

	s->match = ~0U;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(base + SMMU_SMR(n)), s2cr = rd(base + SMMU_S2CR(n));
		if (smr >> 31) {
			uint32_t stream = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

			/* SMR matches SID when the bits outside the mask are equal. The old "~(mask | sid)"
			 * also ignored the SID's own set bits, so 0x520 "covered" 0x522. */
			if (((stream ^ s->sid) & ~mask & 0x7fff) == 0) {
				say("vayu-ipa: SMMU match %u already covers SID %#x (smr %#x s2cr %#x); not overriding it\n",
				    n, s->sid, smr, s2cr);
				return false;
			}
			used[s2cr & 0xff] = true;
		} else if (s->match == ~0U) {
			s->match = n;
		}
	}
	for (s->context = 0; s->context < contexts && used[s->context]; s->context++) {
	}
	if (s->match == ~0U || s->context >= contexts) {
		say("vayu-ipa: SMMU: no free match register or context for SID %#x\n", s->sid);
		return false;
	}
	s->base = base;
	s->second = base + page;
	s->context_base = base + (pages + s->context) * page;
	if (rd(s->context_base + SMMU_CB_SCTLR) & 1) {
		say("vayu-ipa: SMMU context %u is already on; leaving it alone\n", s->context);
		return false;
	}
	s->found = true;
	return true;
}

static bool
ipa_smmu_map(struct ipa_smmu_stream *s)
{
	uint64_t *root, *l1;
	uint32_t pa;

	if (kmem_alloc(kernel_map, &s->tables, 2 * PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || s->tables == 0) {
		say("vayu-ipa: no page tables for SID %#x\n", s->sid);
		return false;
	}
	root = (uint64_t *)s->tables;
	l1 = (uint64_t *)(s->tables + PAGE_SIZE);
	/* TCR 39-bit VA: root index = VA >> 30; a level-1 table has 2 MiB blocks.
	 * root[2] points to that table and maps DDR at 0x80000000. The two DDR
	 * windows below are 0x80000000..0xffffffff, not a 2 GiB low-memory alias. */
	root[2] = kvtophys((vm_offset_t)l1) | 3;
	for (unsigned i = 0; i < 1024; i++) {
		pa = 0x80000000U + i * 0x200000U;
		l1[i] = (uint64_t)pa | PTE_BLOCK;
	}
	/* Device mappings in the low VA aliases: SMEM and IMEM are at those PAs. */
	root[0] = 0x86000000ULL | PTE_DEVICE;       /* SMEM, 1 GiB block */
	root[1] = 0x14600000ULL | PTE_DEVICE;       /* IMEM, 1 GiB block */
	for (vm_offset_t line = s->tables; line < s->tables + 2 * PAGE_SIZE; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r" (line));
	}
	return true;
}

static bool
ipa_smmu_on(struct ipa_smmu_stream *s)
{
	uint32_t smr = SMMU_SMR(s->match), s2cr = SMMU_S2CR(s->match), sees = 1U << 31 | s->sid;
	uint64_t root = kvtophys(s->tables) | (uint64_t)s->context << 48;
	vm_offset_t second = s->second, context = s->context_base;   /* kernel VAs: 32-bit locals truncated them */

	if (!ipa_smmu_map(s)) return false;
	wr(context + SMMU_CB_SCTLR, 0);
	wr(second + SMMU_CBA2R(s->context), SMMU_CBA2R_64_BIT);
	wr(second + SMMU_CBAR(s->context), SMMU_CBAR_STAGE_1);
	wr(context + SMMU_CB_TCR2, SMMU_TCR2);
	wr(context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr(context + SMMU_CB_TTBR0, (uint32_t)root);
	wr(context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	wr(context + SMMU_CB_TTBR1, 0);
	wr(context + SMMU_CB_TTBR1 + 4, 0);
	wr(context + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(context + SMMU_CB_MAIR1, 0);
	wr(context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(context + SMMU_CB_TLBIASID, s->context);
	wr(context + SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(context + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("vayu-ipa: SMMU context %u TLB invalidation timed out\n", s->context);
			return false;
		}
	}
	wr(context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	s->on = true;
	wr(s->base + s2cr, s->context);
	wr(s->base + smr, sees);
	if (rd(s->base + smr) != sees || (rd(s->base + s2cr) & 0x300ff) != s->context ||
	    !(rd(context + SMMU_CB_SCTLR) & 1) || rd(context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    rd(context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		say("vayu-ipa: SMMU did not accept SID %#x into context %u (read-back failed): "
		    "smr %#x s2cr %#x sctlr %#x ttbr %#x_%08x; wanted smr %#x s2cr %#x ttbr %#llx\n",
		    s->sid, s->context, rd(s->base + smr), rd(s->base + s2cr), rd(context + SMMU_CB_SCTLR),
		    rd(context + SMMU_CB_TTBR0 + 4), rd(context + SMMU_CB_TTBR0), sees, s->context,
		    (unsigned long long)root);
		return false;
	}
	say("vayu-ipa: SMMU SID %#x -> context %u, match %u, root %#llx; DDR/SMEM/IMEM identity map read back OK\n",
	    s->sid, s->context, s->match, (unsigned long long)root);
	return true;
}

bool
vayu_ipa_smmu_enable(void)
{
	vm_offset_t base = ml_io_map_unmappable(APPS_SMMU, SMMU_SIZE, VM_WIMG_IO);
	uint32_t idr0, idr1;

	if (base == 0) {
		say("vayu-ipa: application SMMU did not map\n");
		return false;
	}
	idr0 = rd(base + SMMU_IDR0);
	idr1 = rd(base + SMMU_IDR1);
	if (rd(base + SMMU_SCR0) & (1U << 3)) {
		say("vayu-ipa: SMMU uses 16-bit stream IDs; this first context path is not for it\n");
		return false;
	}
	/* Finding and programming is one transaction with the other SMMU users: a test run had the QUP's GSI
	 * take match 6/7 and context 4/5 while this driver was taking context 4 too. */
	lck_mtx_lock(&hakios_smmu_lock);
	for (unsigned i = 0; i < 2; i++) {
		ipa_smmu[i].sid = i == 0 ? 0x520 : 0x522;
		if (!ipa_smmu_find(&ipa_smmu[i], base, idr0, idr1) || !ipa_smmu_on(&ipa_smmu[i])) {
			say("vayu-ipa: SMMU enable failed for SID %#x\n", ipa_smmu[i].sid);
			lck_mtx_unlock(&hakios_smmu_lock);
			return false;
		}
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return true;
}
