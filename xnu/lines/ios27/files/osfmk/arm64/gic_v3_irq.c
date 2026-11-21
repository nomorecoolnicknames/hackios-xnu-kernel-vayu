/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (POCO X3 Pro, SM8150, GIC-600): device interrupts (SPIs) for the vayu drivers, the stop SGI, and what a CPU
 * leaving the kernel owes the interrupt controller. K24 (K2 of plans/ios27-macos, SPEC-K2.md 5-8).
 *
 * Origin: the C ABI and the GIC sequences of the iOS 16.5 line (tree 6a36d992) osfmk/arm64/gic_v3_irq.c (the 8796 line, HakiOS,
 * no licence header of its own: this tree's APSL-2.0). Not taken from there: its interrupt handler (Scion's sleh_irq()
 * is the one owner of ICC_IAR1_EL1/ICC_EOIR1_EL1 here), its own GIC set-up and mapping (pexpert/arm/pe_fiq.c's is the
 * one), and its SPI table's lifetime (between its handler's load of the INTID and busy++ a detach and a new attach
 * could reuse the slot: the handler ran with another driver's context, or after its driver freed it).
 *
 * The SPI table. A slot is FREE, ATTACHING (configured, not yet seen by interrupts), ACTIVE, or DETACHING (no new
 * handler may start, old ones finishing). Every change of state and every start of a handler happens under one spin
 * lock, taken with interrupts off; a handler starts only on an ACTIVE slot of its INTID and is counted in the slot's
 * busy, under the lock; it runs without the lock. A detach makes the slot DETACHING under the lock, so from then on no
 * handler can start on it, waits (in thread context, without the lock) for busy to drain, and only then frees it. A slot
 * is reused only once FREE: no handler of an old driver can run with a new driver's context, and none runs after
 * gic_v3_spi_detach() returned (the driver may free its context then). A detach from the interrupt's own handler would
 * wait for itself: it panics. The distributor's shared words (IGROUPR, IGRPMODR, ICFGR: 32 or 16 interrupts each) are
 * written under the same lock; the wait for GICD_CTLR.RWP is bounded.
 *
 * Off unless vayu27=1 vayu_spi=1: gic_v3_spi_attach() refuses, and sleh_irq() is Scion's (an unknown SGI is
 * cpu_signal(), an SPI without a handler is EOI'd). With them an SPI nobody attached is masked and counted (no storm
 * from a level line left on by the firmware), an SGI other than 1 and 2 is EOI'd and counted.
 *
 * VAYU_GIC_HOST_TEST: built on the host by tools/vayu/test/gic_host_test.c with a fake distributor (the SPI lifetime
 * and the SGI1R encoding under threads).
 */
#if defined(VAYU_GIC_HOST_TEST)
#include "gic_host_shim.h"
#else
#include <pexpert/arm64/board_config.h>
#endif

#if defined(ARM64_BOARD_CONFIG_VAYU) || defined(VAYU_GIC_HOST_TEST)

#if !defined(VAYU_GIC_HOST_TEST)
#include <mach/mach_types.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/locks.h>
#include <kern/simple_lock.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <os/atomic_private.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <arm64/vayu_gic.h>
#include <arm64/psci_smp.h>

extern vm_offset_t vayu_gic_gicd_base(void);              /* pexpert/arm/pe_fiq.c */
extern vm_offset_t vayu_gic_gicr_base(unsigned int cpu);
extern bool vayu27_enabled(void);                           /* bsd/dev/vayu_board.c */
extern bool vayu_arg(const char *name);

static SIMPLE_LOCK_DECLARE(vayu_spi_lock, 0);
#define SPI_LOCK(s)     do { (s) = ml_set_interrupts_enabled(FALSE); simple_lock(&vayu_spi_lock, LCK_GRP_NULL); } while (0)
#define SPI_UNLOCK(s)   do { simple_unlock(&vayu_spi_lock); ml_set_interrupts_enabled(s); } while (0)
#define GIC_R32(a)      (*(volatile uint32_t *)(a))
#define GIC_W32(a, v)   (*(volatile uint32_t *)(a) = (v))
#define GIC_W8(a, v)    (*(volatile uint8_t *)(a) = (v))
#define GIC_W64(a, v)   (*(volatile uint64_t *)(a) = (v))
#define GIC_DSB()       __builtin_arm_dsb(DSB_SY)
#endif

#define GICD_TYPER              0x0004
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_ICACTIVER(n)       (0x0380 + 4 * (n))
#define GICD_IPRIORITYR(n)      (0x0400 + (n))
#define GICD_ICFGR(n)           (0x0c00 + 4 * (n))
#define GICD_IGRPMODR(n)        (0x0d00 + 4 * (n))
#define GICD_IROUTER(n)         (0x6000 + 8 * (n))
#define GICD_CTLR_RWP_BIT       (1u << 31)
#define GICR_SGI_ICENABLER0     0x10180
#define GICR_SGI_ICPENDR0       0x10280
#define GICR_SGI_ICACTIVER0     0x10380

#define VAYU_SPI_MIN            32
#define VAYU_SPI_END            1020    /* 1020-1023: special INTIDs */
#define VAYU_SPI_SLOTS          48
#define VAYU_SPI_PRIORITY       0xa0    /* the timer's and the SGIs' (pe_fiq.c) */
#define VAYU_RWP_TIMEOUT_NS     (10 * 1000 * 1000ULL)

enum { SLOT_FREE = 0, SLOT_ATTACHING, SLOT_ACTIVE, SLOT_DETACHING };

struct vayu_spi {
	uint32_t intid;                 /* 0 when FREE */
	uint32_t state;                 /* changed under vayu_spi_lock only */
	uint32_t busy;                  /* handlers running now: ++ under the lock, -- after the handler */
	void   (*handler)(void *context, uint32_t intid);
	void    *context;
	uint64_t count;
};
static struct vayu_spi vayu_spis[VAYU_SPI_SLOTS];

uint64_t vayu_gic_unhandled_spi, vayu_gic_unknown_sgi, vayu_gic_stray_stop;
uint32_t vayu_gic_last_unhandled;

/* sleh_irq()'s IAR of each CPU, VAYU_GIC_IAR_VALID set while it is acknowledged and not ended */
static uint64_t vayu_gic_active_iar[MAX_CPUS];
static void (*volatile vayu_gic_stop_fn)(void);
static int vayu_spi_on;         /* 0: not read yet; 1 on; -1 off */

bool
vayu_gic_spi_enabled(void)
{
	int v = os_atomic_load(&vayu_spi_on, relaxed);

	if (v == 0) {
		v = vayu27_enabled() && vayu_arg("vayu_spi") ? 1 : -1;
		os_atomic_store(&vayu_spi_on, v, relaxed);
	}
	return v > 0;
}

/* ---- SGIs ---- */

uint64_t
vayu_gic_sgi1r(uint32_t phys_id, uint32_t sgi)
{
	/* ICC_SGI1R_EL1: Aff3 [55:48] (0: S's reg is 32 bits), Aff2 [39:32], INTID [27:24], Aff1 [23:16], target list
	 * [15:0] a bit per Aff0, IRM 0. Aff0 >= 16 needs the range selector: not on these boards. */
	uint32_t aff0 = phys_id & 0xff, aff1 = (phys_id >> 8) & 0xff, aff2 = (phys_id >> 16) & 0xff;

	if (aff0 >= 16 || sgi >= 16) {
		return ~0ULL;
	}
	return ((uint64_t)aff2 << 32) | ((uint64_t)sgi << 24) | ((uint64_t)aff1 << 16) | (1ULL << aff0);
}

#if !defined(VAYU_GIC_HOST_TEST)
bool
vayu_gic_send_sgi(uint32_t phys_id, uint32_t sgi)
{
	uint64_t v = vayu_gic_sgi1r(phys_id, sgi);

	if (v == ~0ULL) {
		return false;
	}
	__builtin_arm_dsb(DSB_ISHST);
	__builtin_arm_wsr64("ICC_SGI1R_EL1", v);
	__builtin_arm_isb(ISB_SY);
	return true;
}

void
vayu_gic_iar_taken(unsigned int cpu, uint64_t iar)
{
	if (cpu < MAX_CPUS) {
		vayu_gic_active_iar[cpu] = iar | VAYU_GIC_IAR_VALID;
	}
}

void
vayu_gic_iar_done(unsigned int cpu)
{
	if (cpu < MAX_CPUS) {
		vayu_gic_active_iar[cpu] = 0;
	}
}

void
vayu_gic_set_stop_handler(void (*fn)(void))
{
	vayu_gic_stop_fn = fn;
}

void
vayu_gic_sgi_stop(void)
{
	void (*fn)(void) = vayu_gic_stop_fn;

	if (fn != NULL) {
		fn();
	} else {
		os_atomic_inc(&vayu_gic_stray_stop, relaxed);
	}
}

bool
vayu_gic_sgi_unknown(uint32_t intid)
{
	if (!vayu_gic_spi_enabled()) {
		return false;
	}
	os_atomic_inc(&vayu_gic_unknown_sgi, relaxed);
	vayu_gic_last_unhandled = intid;
	return true;
}
#endif /* !VAYU_GIC_HOST_TEST */

/* ---- the distributor ---- */

static uint32_t
vayu_gic_spi_limit(vm_offset_t gicd)
{
	/* GICD_TYPER.ITLinesNumber: 32 * (N + 1) INTIDs, the last ones special */
	uint32_t lines = 32 * ((GIC_R32(gicd + GICD_TYPER) & 0x1f) + 1);

	return lines < VAYU_SPI_END ? lines : VAYU_SPI_END;
}

static bool
vayu_gic_settle(vm_offset_t gicd)
{
	uint64_t deadline;

	nanoseconds_to_deadline(VAYU_RWP_TIMEOUT_NS, &deadline);
	while (GIC_R32(gicd) & GICD_CTLR_RWP_BIT) {
		if (mach_absolute_time() > deadline) {
			return false;
		}
	}
	return true;
}

/* Under the lock: the slot of an INTID in a given state set, or NULL */
static struct vayu_spi *
vayu_spi_find(uint32_t intid)
{
	for (unsigned i = 0; i < VAYU_SPI_SLOTS; i++) {
		if (vayu_spis[i].state != SLOT_FREE && vayu_spis[i].intid == intid) {
			return &vayu_spis[i];
		}
	}
	return NULL;
}

/*
 * A driver takes an SPI: edge- or level-triggered, Group 1 non-secure, the timer's priority, routed to the boot CPU
 * (it never goes off), enabled. The handler runs in interrupt context, interrupts off, before the EOI; a level
 * source has to be quieted there (or the line masked: gic_v3_spi_mask()). Thread context.
 *   KERN_NOT_SUPPORTED       without vayu27=1 vayu_spi=1, or before the GIC is set up
 *   KERN_INVALID_ARGUMENT    not an SPI this distributor has, no handler, or in interrupt context
 *   KERN_NAME_EXISTS         someone has it; KERN_RESOURCE_SHORTAGE: no free slot
 *   KERN_OPERATION_TIMED_OUT the distributor did not settle (the line stays masked)
 *   KERN_FAILURE             the enable bit did not stick (a Group 0 line: the secure world's)
 */
kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	vm_offset_t gicd = vayu_gic_gicd_base();
	struct vayu_spi *slot = NULL;
	boolean_t s;

	if (!vayu_gic_spi_enabled() || gicd == 0) {
		return KERN_NOT_SUPPORTED;
	}
	if (intid < VAYU_SPI_MIN || intid >= vayu_gic_spi_limit(gicd) || handler == NULL || ml_at_interrupt_context()) {
		return KERN_INVALID_ARGUMENT;
	}
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t route = topology->boot_cpu->phys_id & 0xffffff;      /* Aff2.Aff1.Aff0, IRM 0, Aff3 0 */
	uint32_t bit = 1u << (intid % 32), shift = (intid % 16) * 2;

	SPI_LOCK(s);
	if (vayu_spi_find(intid) != NULL) {
		SPI_UNLOCK(s);
		return KERN_NAME_EXISTS;
	}
	for (unsigned i = 0; i < VAYU_SPI_SLOTS && slot == NULL; i++) {
		if (vayu_spis[i].state == SLOT_FREE && os_atomic_load(&vayu_spis[i].busy, acquire) == 0) {
			slot = &vayu_spis[i];
		}
	}
	if (slot == NULL) {
		SPI_UNLOCK(s);
		return KERN_RESOURCE_SHORTAGE;
	}
	slot->intid = intid;
	slot->state = SLOT_ATTACHING;           /* reserved: sleh_irq() starts no handler on it */
	slot->handler = handler;
	slot->context = context;
	os_atomic_store(&slot->count, 0, relaxed);

	GIC_W32(gicd + GICD_ICENABLER(intid / 32), bit);
	bool settled = vayu_gic_settle(gicd);
	if (settled) {
		GIC_W32(gicd + GICD_ICPENDR(intid / 32), bit);
		GIC_W32(gicd + GICD_ICACTIVER(intid / 32), bit);
		GIC_W32(gicd + GICD_IGROUPR(intid / 32), GIC_R32(gicd + GICD_IGROUPR(intid / 32)) | bit);
		GIC_W32(gicd + GICD_IGRPMODR(intid / 32), GIC_R32(gicd + GICD_IGRPMODR(intid / 32)) & ~bit);
		GIC_W8(gicd + GICD_IPRIORITYR(intid), VAYU_SPI_PRIORITY);
		GIC_W32(gicd + GICD_ICFGR(intid / 16),
		    (GIC_R32(gicd + GICD_ICFGR(intid / 16)) & ~(3u << shift)) | ((edge ? 2u : 0u) << shift));
		GIC_W64(gicd + GICD_IROUTER(intid), (uint64_t)route);
		GIC_DSB();
		settled = vayu_gic_settle(gicd);
	}
	if (!settled) {
		slot->state = SLOT_FREE;
		slot->intid = 0;
		SPI_UNLOCK(s);
		return KERN_OPERATION_TIMED_OUT;
	}
	slot->state = SLOT_ACTIVE;              /* published before the line can fire */
	GIC_DSB();
	GIC_W32(gicd + GICD_ISENABLER(intid / 32), bit);
	GIC_DSB();
	bool stuck = (GIC_R32(gicd + GICD_ISENABLER(intid / 32)) & bit) != 0;
	SPI_UNLOCK(s);
	if (!stuck) {
		gic_v3_spi_detach(intid);
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}

/* An attached SPI only (another is not the caller's to touch); the mask has taken effect when this returns. */
void
gic_v3_spi_mask(uint32_t intid)
{
	vm_offset_t gicd = vayu_gic_gicd_base();
	boolean_t s;

	if (gicd == 0 || intid < VAYU_SPI_MIN || intid >= VAYU_SPI_END) {
		return;
	}
	SPI_LOCK(s);
	struct vayu_spi *slot = vayu_spi_find(intid);
	if (slot != NULL) {
		GIC_W32(gicd + GICD_ICENABLER(intid / 32), 1u << (intid % 32));     /* write-1-to-clear: no RMW */
	}
	SPI_UNLOCK(s);
	if (slot != NULL) {
		(void)vayu_gic_settle(gicd);
	}
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	vm_offset_t gicd = vayu_gic_gicd_base();
	boolean_t s;

	if (gicd == 0 || intid < VAYU_SPI_MIN || intid >= VAYU_SPI_END) {
		return;
	}
	SPI_LOCK(s);
	struct vayu_spi *slot = vayu_spi_find(intid);
	if (slot != NULL && slot->state == SLOT_ACTIVE) {
		GIC_W32(gicd + GICD_ISENABLER(intid / 32), 1u << (intid % 32));
	}
	SPI_UNLOCK(s);
}

/*
 * ... and gives it back: masked, nothing pending, no new handler of it can start, and none still runs when this
 * returns (the context may be freed then). Thread context, never the interrupt's own handler.
 */
void
gic_v3_spi_detach(uint32_t intid)
{
	vm_offset_t gicd = vayu_gic_gicd_base();
	boolean_t s;

	if (gicd == 0 || intid < VAYU_SPI_MIN || intid >= VAYU_SPI_END) {
		return;
	}
	if (ml_at_interrupt_context()) {
		panic("VAYU GIC: SPI %u detached in interrupt context (its own handler would wait for itself)", intid);
	}
	SPI_LOCK(s);
	struct vayu_spi *slot = vayu_spi_find(intid);
	if (slot == NULL || slot->state == SLOT_DETACHING) {
		SPI_UNLOCK(s);
		return;
	}
	GIC_W32(gicd + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	GIC_W32(gicd + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	slot->state = SLOT_DETACHING;           /* from here no handler starts on it */
	SPI_UNLOCK(s);
	(void)vayu_gic_settle(gicd);
	while (os_atomic_load(&slot->busy, acquire) != 0) {
		/* a handler of it on another CPU is finishing */
	}
	SPI_LOCK(s);
	slot->handler = NULL;
	slot->context = NULL;
	slot->intid = 0;
	slot->state = SLOT_FREE;
	SPI_UNLOCK(s);
}

/* How many have arrived since it was attached (0: not attached). */
uint64_t
gic_v3_spi_count(uint32_t intid)
{
	uint64_t n = 0;
	boolean_t s;

	SPI_LOCK(s);
	struct vayu_spi *slot = vayu_spi_find(intid);
	if (slot != NULL) {
		n = os_atomic_load(&slot->count, relaxed);
	}
	SPI_UNLOCK(s);
	return n;
}

/* sleh_irq(), interrupts off, between the IAR and the EOI. */
bool
vayu_gic_spi_handle(uint32_t intid)
{
	void (*handler)(void *, uint32_t) = NULL;
	void *context = NULL;
	struct vayu_spi *slot;

	if (!vayu_gic_spi_enabled() || intid < VAYU_SPI_MIN || intid >= VAYU_SPI_END) {
		return false;
	}
	simple_lock(&vayu_spi_lock, LCK_GRP_NULL);
	slot = vayu_spi_find(intid);
	if (slot != NULL && slot->state == SLOT_ACTIVE) {
		os_atomic_inc(&slot->busy, relaxed);
		handler = slot->handler;
		context = slot->context;
	}
	simple_unlock(&vayu_spi_lock);
	if (handler != NULL) {
		os_atomic_inc(&slot->count, relaxed);
		handler(context, intid);
		os_atomic_dec(&slot->busy, release);
		return true;
	}
	/* nobody's (or being attached/detached): keep it from storming; the driver unmasks it when it is ready */
	vm_offset_t gicd = vayu_gic_gicd_base();
	if (gicd != 0) {
		GIC_W32(gicd + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	}
	if (slot == NULL) {
		os_atomic_inc(&vayu_gic_unhandled_spi, relaxed);
		vayu_gic_last_unhandled = intid;
	}
	return true;
}

#if !defined(VAYU_GIC_HOST_TEST)
/* ---- a CPU leaving the kernel ---- */

void
vayu_gic_cpu_quiesce(bool all_spis, bool local_off)
{
	vm_offset_t gicd = vayu_gic_gicd_base();
	unsigned int cpu;

	if (gicd == 0) {
		return;                 /* as early as this nothing was acknowledged, nor is there a CPU number */
	}
	cpu = ml_get_cpu_number_local();
	if (all_spis) {
		/* the drivers' interrupts go with the kernel: none may fire under the loader (no lock: a panic's way) */
		for (unsigned i = 0; i < VAYU_SPI_SLOTS; i++) {
			uint32_t intid = vayu_spis[i].intid;
			if (vayu_spis[i].state != SLOT_FREE && intid >= VAYU_SPI_MIN && intid < VAYU_SPI_END) {
				GIC_W32(gicd + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
			}
		}
	}
	if (cpu < MAX_CPUS && (vayu_gic_active_iar[cpu] & VAYU_GIC_IAR_VALID)) {
		/* the interrupt this CPU was in (a panic in a handler): its priority dropped and deactivated (EOImode 0) */
		__builtin_arm_wsr64("ICC_EOIR1_EL1", vayu_gic_active_iar[cpu] & ~VAYU_GIC_IAR_VALID);
		__builtin_arm_isb(ISB_SY);
		vayu_gic_active_iar[cpu] = 0;
	}
	if (local_off) {
		vm_offset_t gicr = vayu_gic_gicr_base(cpu);
		__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
		if (gicr != 0) {
			GIC_W32(gicr + GICR_SGI_ICENABLER0, 0xffffffffu);
			GIC_W32(gicr + GICR_SGI_ICPENDR0, 0xffffffffu);
			GIC_W32(gicr + GICR_SGI_ICACTIVER0, 0xffffffffu);
		}
		__builtin_arm_dsb(DSB_SY);
		__builtin_arm_wsr64("S3_0_C12_C12_7", 0);       /* ICC_IGRPEN1_EL1 */
		__builtin_arm_isb(ISB_SY);
	}
}

static void
vayu_gic_loader_quiesce(void)
{
	vayu_gic_cpu_quiesce(true, false);
}

int loader_quiesce_register(const char *name, void (*fn)(void));       /* osfmk/arm64/loader_return.c */

static void
vayu_gic_init(void)
{
	if (!vayu_gic_spi_enabled() && !vayu_smp_enabled()) {
		return;
	}
	/* every way back to the loader ends this CPU's interrupt and silences the drivers' lines (SMP: from a handler too) */
	(void)loader_quiesce_register("vayu_gic", vayu_gic_loader_quiesce);
	if (!vayu_gic_spi_enabled()) {
		return;
	}
	vm_offset_t gicd = vayu_gic_gicd_base();
	printf("VAYU GIC: SPIs on (vayu27=1 vayu_spi=1): INTIDs 32..%u, %u slots, routed to mpidr 0x%x\n",
	    gicd ? vayu_gic_spi_limit(gicd) - 1 : 0, VAYU_SPI_SLOTS, ml_get_topology_info()->boot_cpu->phys_id);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, vayu_gic_init);
#endif /* !VAYU_GIC_HOST_TEST */

#endif /* ARM64_BOARD_CONFIG_VAYU || VAYU_GIC_HOST_TEST */
