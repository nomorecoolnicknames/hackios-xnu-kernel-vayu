/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (POCO X3 Pro, SM8150): more than one CPU on Scion's SWEET layer, and the way back to the loader with every one
 * of them off. K24 (K2 of plans/ios27-macos, SPEC-K2.md 1-4 and "Возврат загрузчику").
 *
 * Origin: the algorithm of the iOS 16.5 line (tree 6a36d992) osfmk/arm64/psci_smp.c (the 8796 line, HakiOS; no licence header of
 * its own: this tree's APSL-2.0), not its code: SweetCPU (iokit/Kernel/arm/SweetCPU.cpp) starts the CPUs here, through
 * sweet_secondary_entry, as on Scion's other SWEET boards; B's own start path, its idle power-down (cpu_suspend) and
 * its SYSTEM_OFF/RESET are not taken (sweet_c2 stays off on VAYU).
 *
 * The gate (vayu_smp_parse, from ml_parse_cpu_topology on the boot CPU): cpus=N above 1 is kept only with vayu27=1
 * vayu_smp=1 and a /chosen "psci-method" of "smc" or "hvc" (xnuloader: smc on the phone, hvc under QEMU); anything else
 * cuts it to 1 and says why. The conduit is the device tree's, read once on the boot CPU: no guessing by trying calls.
 * The ADT has one CPU per core the loader gives the kernel (bootcpu=7: core 7 first and the loader's core 0 left out:
 * at most 7 on the phone, cpus=8 is a ceiling, not a reason to take core 0); the logical number is the position in it.
 *
 * Every CPU is accounted for from the moment it may run: VAYU_CPU_ASKED is stored before CPU_ON is called (a CPU
 * that returned 0 runs whether or not it ever reaches C), RUNNING when it is in SweetCPU::initCPU, then STOPPING,
 * OFF_PENDING ("no more kernel work": the last store before CPU_OFF), OFF once PSCI AFFINITY_INFO says so, or
 * OFF_FAILED when CPU_OFF came back (the CPU parks in WFE: not off). CPU_ON answering ALREADY_ON / ON_PENDING leaves
 * the CPU UNKNOWN: running, and not ours to stop.
 *
 * The way back (loader_return.c calls vayu_smp_stop_others() on every way, a panic's too, and gives the machine back
 * only on true). The CPU that runs it:
 *  - is not the boot CPU: it asks the boot CPU to do the return (SGI 2, and SEV for one that waits with interrupts
 *    off), then switches itself off. The loader gets back the core it gave (QEMU: core 0, where UEFI runs).
 *  - is the boot CPU (or the one that won the race): every other CPU is told to stop (SGI 2; SEV for those polling),
 *    and each one that may run is waited for, 1 s in all: OFF_PENDING or OFF_FAILED in its state, then AFFINITY_INFO
 *    OFF. Any CPU not confirmed (one that never reached C after CPU_ON 0, one spinning with interrupts off, CPU_OFF
 *    refused, AFFINITY_INFO not supported or never OFF): false, and the loader does not get the machine back.
 * A CPU takes the stop in sleh_irq() (SGI 2, after its EOI) or, interrupts off, where the kernel waits for the
 * debugger's cross call or spins after a panic (vayu_smp_poll()). Taking it: interrupts off, this CPU's active IAR
 * ended, its timer masked, its SGIs/PPIs disabled and cleared, Group 1 off (vayu_gic_cpu_quiesce), OFF_PENDING, DSB,
 * CPU_OFF. Nothing on this path takes a lock, allocates or calls printf(): a panic runs it. Lines go to the loader's
 * log (loader_log_line: a bounded try-lock), and the refusal also to the console (kdb_printf_unbuffered).
 *
 * vayu_smp_test=1 (with vayu_smp=1): after every CPU is up, on each CPU in turn (bound there) its timer count must
 * grow over 20 ms of sleep, a cross call from it must reach the boot CPU and one from the boot CPU must reach it.
 * vayu_smp_test_hang=1 (a negative test): the last CPU parks instead of taking the stop, so the way back is refused.
 */
#if defined(VAYU_PSCI_HOST_TEST)
#include "psci_host_shim.h"    /* tools/vayu/test: the stop on a fake firmware, CPUs as threads */
#else
#include <pexpert/arm64/board_config.h>
#endif

#if defined(ARM64_BOARD_CONFIG_VAYU) || defined(VAYU_PSCI_HOST_TEST)

#if !defined(VAYU_PSCI_HOST_TEST)
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <machine/atomic.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <arm64/vayu_gic.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/section_keywords.h>
#include <libkern/kernel_mach_header.h>
#include <arm/caches_internal.h>
#include <string.h>
#endif

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define VAYU_SMP_STOP_NS        (1000ULL * 1000 * 1000)    /* every CPU's stop, in all */
#define VAYU_SMP_TEST_NS        (2000ULL * 1000 * 1000)

void loader_log_line(const char *line);                     /* osfmk/arm64/loader_return.c */
void loader_log_sync(void);                                 /* ... the log back to RAM (K2b) */
void loader_return(void);
void loader_set_stop_others(bool (*fn)(void));
#if !defined(VAYU_PSCI_HOST_TEST)
extern bool vayu27_enabled(void);                           /* bsd/dev/vayu_board.c */
extern bool vayu_arg(const char *name);
extern vm_offset_t vayu_gic_gicr_base(unsigned int cpu);    /* pexpert/arm/pe_fiq.c */
extern void IOSleep(unsigned int milliseconds);
#endif

SECURITY_READ_ONLY_LATE(static bool) vayu_smp_on;           /* the gate passed */
SECURITY_READ_ONLY_LATE(static bool) vayu_psci_hvc;         /* the conduit: HVC, else SMC */
static uint32_t vayu_smp_requested, vayu_smp_available;
static const char *vayu_smp_refused;                        /* why cpus= was cut to 1, NULL: not cut */

static uint32_t vayu_cpu_state[MAX_CPUS];
static int32_t vayu_cpu_on_rc[MAX_CPUS], vayu_cpu_off_rc[MAX_CPUS];
static uint32_t vayu_cpus_asked;                            /* how many CPU_ONs may have started a CPU */
static int vayu_stopper = -1;                               /* the logical CPU that gives the machine back */
static uint32_t vayu_stop_asked;                            /* every CPU but vayu_stopper: stop */
static int vayu_return_asker = -1;                          /* a secondary that asked the boot CPU to return */
static uint32_t vayu_smp_hang_test;
uint64_t vayu_timer_ticks[MAX_CPUS];

/*
 * K2b: each secondary's slot (start.s writes it with its MMU off, at physical addresses: the layout is fixed there).
 *   early  the CPU, MMU off, straight to memory: [0] checkpoint VAYU_CRUMB(step), [1] MPIDR_EL1, [2] CurrentEL,
 *          [3] SCTLR_EL1 as the firmware left it, [4] ESR, [5] ELR, [6] FAR, [7] LR of an early exception (or the
 *          answer of a CPU_OFF that came back)
 *   late   the CPU, MMU on (cached, coherent): [0] checkpoint 7..9
 *   gate   the boot CPU, before CPU_ON (cleaned to memory): [0] closed (the CPU was given up: switch off at once),
 *          [1] MPIDR[23:0] | 1 << 32 (how the CPU finds its slot), [2] HVC (the conduit of its CPU_OFF), [3] a
 *          negative test for this CPU: 1 vayu_smp_test_noarrive=N (no checkpoint: it waits for its gate to close, a
 *          CPU that comes too late), 2 vayu_smp_test_fault=N (an undefined instruction after checkpoint 2)
 * The boot CPU never writes early or late after CPU_ON, so its cache never holds a dirty copy of them; it throws its
 * copy away before each look (DC CIVAC). The early line stays written only with the MMU off, the late one only with
 * it on: no line mixes the two kinds of store.
 */
struct vayu_smp_slot {
	uint64_t early[8];
	uint64_t late[8];
	uint64_t gate[8];
} __attribute__((aligned(64)));
_Static_assert(sizeof(struct vayu_smp_slot) == 192, "start.s: VAYU_SMP_SLOT_SIZE");
struct vayu_smp_slot vayu_smp_slot[MAX_CPUS];
static bool vayu_smp_slots_ready;
static uint64_t vayu_cpu_on_at[MAX_CPUS];                   /* mach_absolute_time() of the CPU_ON that said 0 */
static uint64_t vayu_crumb_seen[MAX_CPUS];                  /* the last checkpoint said */
static uint64_t vayu_smp_arrive_ns = 1000ULL * 1000 * 1000; /* vayu_smp_arrive_ms= (default 1000) */
static bool vayu_smp_clean_image_on = true;                 /* vayu_smp_clean_image=0: only cpu.c's cleans */
static uint32_t vayu_smp_test_noarrive, vayu_smp_test_fault; /* logical CPU of the negative test (0: none) */

#if defined(VAYU_PSCI_HOST_TEST)
#define vayu_line_clean(p, n)   __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define vayu_line_flush(p, n)   __atomic_thread_fence(__ATOMIC_SEQ_CST)
static uint64_t vayu_ns_since(uint64_t t0) { return mach_absolute_time() - t0; }
#else
/* to memory (DC CVAC), and to memory then away (DC CIVAC): both end with DSB SY */
#define vayu_line_clean(p, n)   CleanPoC_DcacheRegion_Force((vm_offset_t)(p), (n))
#define vayu_line_flush(p, n)   FlushPoC_DcacheRegion((vm_offset_t)(p), (unsigned)(n))
static uint64_t
vayu_ns_since(uint64_t t0)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns;
}
#endif

static uint64_t
vayu_smp_slot_pa(unsigned int logical)
{
#if defined(VAYU_PSCI_HOST_TEST)
	return (uint64_t)(uintptr_t)&vayu_smp_slot[logical];
#else
	return (uint64_t)ml_static_vtop((vm_offset_t)&vayu_smp_slot[logical]);
#endif
}

static uint64_t
vayu_slot_read(volatile uint64_t *line, unsigned int word)
{
	vayu_line_flush((uintptr_t)line, 64);
	return line[word];
}

/* ---- PSCI ---- */

#if defined(VAYU_PSCI_HOST_TEST)
#define vayu_psci_call host_psci_call
#else
static int64_t
vayu_psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	/* SMCCC 1.0: x0 the result, x1-x17 not kept */
	if (vayu_psci_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	}
	return (int64_t)(int32_t)x0;
}
#endif

static const char *
vayu_cpu_state_name(uint32_t s)
{
	static const char *const names[] = {
		"none", "asked", "running", "stopping", "off-pending", "off", "OFF FAILED", "UNKNOWN", "never arrived",
	};
	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

/* ---- the gate ---- */

bool
vayu_smp_enabled(void)
{
	return vayu_smp_on;
}

#if !defined(VAYU_PSCI_HOST_TEST)
static bool
vayu_boot_arg_set(const char *name)
{
	uint32_t v = 0;
	return PE_parse_boot_argn(name, &v, sizeof(v)) && v != 0;
}

void
vayu_smp_parse(uint32_t *cpus)
{
	DTEntry entry, child;
	OpaqueDTEntryIterator iter;

	vayu_smp_requested = *cpus;
	if (SecureDTLookupEntry(NULL, "/cpus", &entry) == kSuccess && SecureDTInitEntryIterator(entry, &iter) == kSuccess) {
		while (SecureDTIterateEntries(&iter, &child) == kSuccess) {
			vayu_smp_available++;
		}
	}
	if (*cpus <= 1) {
		return;
	}
	/* (vayu27_enabled() is the same test; this runs before anything may cache it differently) */
	if (!vayu_boot_arg_set("vayu27") || !vayu_boot_arg_set("vayu_smp")) {
		vayu_smp_refused = "cpus>1 needs vayu27=1 vayu_smp=1";
		*cpus = 1;
		return;
	}
	void const *prop;
	unsigned int size;
	if (SecureDTLookupEntry(NULL, "/chosen", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "psci-method", &prop, &size) != kSuccess || size < 4) {
		vayu_smp_refused = "no /chosen psci-method";
		*cpus = 1;
		return;
	}
	if (strncmp((const char *)prop, "hvc", size) == 0) {
		vayu_psci_hvc = true;
	} else if (strncmp((const char *)prop, "smc", size) == 0) {
		vayu_psci_hvc = false;
	} else {
		vayu_smp_refused = "/chosen psci-method is neither smc nor hvc";
		*cpus = 1;
		return;
	}
	vayu_smp_hang_test = vayu_boot_arg_set("vayu_smp_test_hang");
	uint32_t v;
	if (PE_parse_boot_argn("vayu_smp_arrive_ms", &v, sizeof(v)) && v != 0) {
		vayu_smp_arrive_ns = (uint64_t)v * 1000 * 1000;
	}
	if (PE_parse_boot_argn("vayu_smp_clean_image", &v, sizeof(v))) {
		vayu_smp_clean_image_on = v != 0;
	}
	(void)PE_parse_boot_argn("vayu_smp_test_noarrive", &vayu_smp_test_noarrive, sizeof(vayu_smp_test_noarrive));
	(void)PE_parse_boot_argn("vayu_smp_test_fault", &vayu_smp_test_fault, sizeof(vayu_smp_test_fault));
	vayu_smp_on = true;
}

void
vayu_smp_report(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();

	printf("VAYU SMP: requested=%u available=%u selected=%u, conduit %s%s%s\n", vayu_smp_requested,
	    vayu_smp_available, topology->num_cpus, vayu_smp_on ? (vayu_psci_hvc ? "hvc" : "smc") : "unused",
	    vayu_smp_refused ? ": " : "", vayu_smp_refused ? vayu_smp_refused : "");
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		printf("VAYU SMP: logical=%u -> mpidr=0x%x%s\n", i, topology->cpus[i].phys_id,
		    &topology->cpus[i] == topology->boot_cpu ? " (boot)" : "");
	}
	if (vayu_smp_on) {
		printf("VAYU SMP: a secondary with no checkpoint %llu ms after CPU_ON is given up (vayu_smp_arrive_ms); "
		    "the whole kernel image cleaned to memory before each CPU_ON: %s (vayu_smp_clean_image)\n",
		    (unsigned long long)(vayu_smp_arrive_ns / 1000000), vayu_smp_clean_image_on ? "yes" : "no");
	}
}

/*
 * What a secondary reads with its MMU off is read from memory, and a line dirty in the boot CPU's cache is not there
 * yet. cpu.c cleans the structures the reset path reads (CpuDataEntries, the target's cpu data, ResetHandlerData,
 * BootArgs, cpu_ttep, the bootstrap tables); the 8796 kernel that runs SMP on this phone (the iOS 16.5 line (tree 6a36d992)
 * psci_smp.c psci_smp_start_cpu) cleans the kernel's whole image and every CPU's data before each CPU_ON. The same
 * here: the segments that stay mapped (__BOOTDATA and __KLD are given back after boot).
 */
static void
vayu_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion_Force(start, size);
	}
}

static void
vayu_smp_clean_image(void)
{
	static const char *const segments[] = { "__TEXT", "__TEXT_EXEC", "__DATA_CONST", "__LASTDATA_CONST", "__DATA" };

	for (unsigned int i = 0; i < sizeof(segments) / sizeof(segments[0]); i++) {
		vayu_smp_clean_segment(segments[i]);
	}
	for (unsigned int i = 0; i < MAX_CPUS; i++) {
		if (CpuDataEntries[i].cpu_data_vaddr != NULL) {
			CleanPoC_DcacheRegion_Force((vm_offset_t)CpuDataEntries[i].cpu_data_vaddr, sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);
}
#endif /* !VAYU_PSCI_HOST_TEST */

/* ---- starting ---- */

/* the boot CPU, before CPU_ON: the target's slot (and, the first time, every slot) as start.s will read it */
static void
vayu_smp_slot_prepare(unsigned int logical, uint64_t target)
{
	if (!vayu_smp_slots_ready) {
		/* none matches an MPIDR until its gate says so (whatever RAM held before) */
		memset((void *)vayu_smp_slot, 0, sizeof(vayu_smp_slot));
		vayu_line_clean(vayu_smp_slot, sizeof(vayu_smp_slot));
		vayu_smp_slots_ready = true;
	}
	volatile struct vayu_smp_slot *s = &vayu_smp_slot[logical];
	for (unsigned int i = 0; i < 8; i++) {
		s->early[i] = 0;
		s->late[i] = 0;
		s->gate[i] = 0;
	}
	s->gate[1] = (target & 0xffffff) | (1ULL << 32);
	s->gate[2] = vayu_psci_hvc;
	s->gate[3] = logical == 0 ? 0 : logical == vayu_smp_test_noarrive ? 1 : logical == vayu_smp_test_fault ? 2 : 0;
	if (s->gate[3] != 0) {
		printf("VAYU SMP: cpu=%u is a negative test: %s\n", logical, s->gate[3] == 1 ?
		    "vayu_smp_test_noarrive (no checkpoint; it waits for its gate)" : "vayu_smp_test_fault (UDF after checkpoint 2)");
	}
	vayu_line_clean((uintptr_t)s, sizeof(*s));
	vayu_crumb_seen[logical] = 0;
}

int32_t
vayu_psci_cpu_on(unsigned int logical, uint64_t target, uint64_t entry, uint64_t context)
{
	if (!vayu_smp_on || logical >= MAX_CPUS) {
		return VAYU_PSCI_DENIED;
	}
	vayu_smp_slot_prepare(logical, target);
#if !defined(VAYU_PSCI_HOST_TEST)
	if (vayu_smp_clean_image_on) {
		vayu_smp_clean_image();
	}
#endif
	printf("VAYU SMP: cpu=%u CPU_ON next: target=0x%llx entry=0x%llx context=%llu (%s), slot 0x%llx\n", logical,
	    (unsigned long long)target, (unsigned long long)entry, (unsigned long long)context, vayu_psci_hvc ? "hvc" : "smc",
	    (unsigned long long)vayu_smp_slot_pa(logical));
	/* from here it may run: the way back must know before the call returns */
	os_atomic_store(&vayu_cpu_state[logical], VAYU_CPU_ASKED, release);
	os_atomic_inc(&vayu_cpus_asked, relaxed);
	__builtin_arm_dsb(DSB_SY);
	vayu_cpu_on_at[logical] = mach_absolute_time();
	int32_t rc = (int32_t)vayu_psci_call(PSCI_CPU_ON, target, entry, context);
	vayu_cpu_on_rc[logical] = rc;
	if (rc == VAYU_PSCI_ALREADY_ON || rc == VAYU_PSCI_ON_PENDING) {
		os_atomic_store(&vayu_cpu_state[logical], VAYU_CPU_UNKNOWN, release);
	} else if (rc != VAYU_PSCI_SUCCESS) {
		/* refused: it was off and stays off (counted all the same: the way back checks it) */
		os_atomic_store(&vayu_cpu_state[logical], VAYU_CPU_NONE, release);
	}
	printf("VAYU SMP: cpu=%u CPU_ON target=0x%llx entry=0x%llx rc=%d (%s)\n", logical, target, entry, rc,
	    vayu_psci_hvc ? "hvc" : "smc");
	return rc;
}

void
vayu_smp_crumb(unsigned int logical, uint32_t step)
{
	if (logical < MAX_CPUS && vayu_smp_on) {
		os_atomic_store(&vayu_smp_slot[logical].late[0], VAYU_CRUMB(step), release);
	}
}

static const char *
vayu_crumb_name(uint64_t crumb)
{
	static const char *const names[] = {
		"none: not one instruction of the kernel yet",
		"1 sweet_secondary_entry: first instructions, slot found",
		"2 at EL1: early vectors and SCTLR_EL1 set",
		"3 reset_vector: past the OS lock write",
		"4 reset_vector: cpu data found",
		"5 start_cpu",
		"6 common_start: MMU about to go on",
		"7 common_start: MMU on",
		"8 arm_init_cpu",
		"9 SweetCPU::initCPU",
	};
	uint32_t step = (uint32_t)(crumb & 0xffff);

	if (crumb == 0) {
		return names[0];
	}
	if ((crumb & 0xffff0000u) != VAYU_CRUMB_TAG) {
		return "? (not a checkpoint)";
	}
	if (step < sizeof(names) / sizeof(names[0])) {
		return names[step];
	}
	switch (step & 0xf000) {
	case VAYU_CRUMB_ABANDONED:  return "the gate was closed: it switched itself off";
	case VAYU_CRUMB_OFF_REFUSED: return "CPU_OFF refused: parked in WFI, NOT off";
	case VAYU_CRUMB_NO_CPUDATA: return "reset_vector found no cpu data: it switched itself off";
	case VAYU_CRUMB_FAULT:      return "an exception before the kernel's vectors: it switched itself off";
	default:                    return "?";
	}
}

/* the furthest checkpoint of `logical`: the late line once it has one, else the early one */
static uint64_t
vayu_smp_crumb_now(unsigned int logical, uint64_t *early_out)
{
	volatile struct vayu_smp_slot *s = &vayu_smp_slot[logical];
	uint64_t early = vayu_slot_read(s->early, 0);
	uint64_t late = vayu_slot_read(s->late, 0);

	if (early_out != NULL) {
		*early_out = early;
	}
	/* an exception or a switch-off said on the early line wins over an older late step */
	if (late != 0 && (early & 0xf000) == 0) {
		return late;
	}
	return early;
}

const char *
vayu_smp_checkpoint_name(unsigned int logical)
{
	return logical < MAX_CPUS ? vayu_crumb_name(vayu_smp_crumb_now(logical, NULL)) : "?";
}

int
vayu_smp_arrival(unsigned int logical)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	volatile struct vayu_smp_slot *s;
	uint64_t early, crumb, ns;

	if (!vayu_smp_on || logical >= MAX_CPUS || logical >= topology->num_cpus ||
	    os_atomic_load(&vayu_cpu_state[logical], acquire) == VAYU_CPU_NONE) {
		return VAYU_ARRIVAL_WAIT;
	}
	s = &vayu_smp_slot[logical];
	crumb = vayu_smp_crumb_now(logical, &early);
	ns = vayu_ns_since(vayu_cpu_on_at[logical]);
	if (crumb != vayu_crumb_seen[logical]) {
		if (vayu_crumb_seen[logical] == 0) {
			printf("VAYU SMP: cpu=%u mpidr=0x%x arrived: MPIDR_EL1 0x%llx, EL%llu, SCTLR_EL1 0x%llx as the firmware left it\n",
			    logical, topology->cpus[logical].phys_id, (unsigned long long)s->early[1],
			    (unsigned long long)((s->early[2] >> 2) & 3), (unsigned long long)s->early[3]);
		}
		vayu_crumb_seen[logical] = crumb;
		printf("VAYU SMP: cpu=%u checkpoint %s, %llu us after CPU_ON\n", logical, vayu_crumb_name(crumb),
		    (unsigned long long)(ns / 1000));
		if ((early & 0xf000) == VAYU_CRUMB_FAULT) {
			printf("VAYU SMP: cpu=%u took an exception at checkpoint %llu: ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx "
			    "(physical; the MMU was off)\n", logical, (unsigned long long)(early & 0xfff),
			    (unsigned long long)s->early[4], (unsigned long long)s->early[5], (unsigned long long)s->early[6],
			    (unsigned long long)s->early[7]);
		}
		if ((early & 0xf000) == VAYU_CRUMB_OFF_REFUSED) {
			printf("VAYU SMP: cpu=%u: its CPU_OFF answered %lld\n", logical, (long long)(int64_t)(int32_t)s->early[7]);
		}
	}
	switch (early & 0xf000) {
	case VAYU_CRUMB_FAULT:
	case VAYU_CRUMB_NO_CPUDATA:
	case VAYU_CRUMB_ABANDONED:
		if ((early & 0xffff0000u) == VAYU_CRUMB_TAG) {
			return VAYU_ARRIVAL_GONE;       /* the way back still asks the firmware (AFFINITY_INFO OFF) */
		}
		break;
	case VAYU_CRUMB_OFF_REFUSED:
		if ((early & 0xffff0000u) == VAYU_CRUMB_TAG) {
			return VAYU_ARRIVAL_PARKED;
		}
		break;
	}
	if (crumb != 0 || ns < vayu_smp_arrive_ns) {
		return VAYU_ARRIVAL_WAIT;
	}
	/*
	 * Not one checkpoint: it has not run the first instructions of the kernel. Close its gate, then look once more
	 * (Dekker: it stores its first checkpoint, DSB, then reads the gate; this CPU stores the gate, DSB, then reads the
	 * checkpoint -- one of the two sees the other). Still nothing: it never ran kernel code, and if it starts now it
	 * reads the gate closed and switches itself off before touching anything but its slot.
	 */
	s->gate[0] = 1;
	vayu_line_flush((uintptr_t)s->gate, 64);
	early = vayu_slot_read(s->early, 0);
	if (early != 0) {
		return VAYU_ARRIVAL_WAIT;               /* it came just now: watch it (it may yet see the gate) */
	}
	os_atomic_store(&vayu_cpu_state[logical], VAYU_CPU_ABANDONED, release);
	int64_t aff = vayu_psci_call(PSCI_AFFINITY_INFO, topology->cpus[logical].phys_id, 0, 0);
	printf("VAYU SMP: secondary cpu=%u (mpidr 0x%x) never arrived: no checkpoint %llu ms after CPU_ON said 0; "
	    "AFFINITY_INFO %lld; abandoned (it never ran kernel code; its gate is closed)\n", logical,
	    topology->cpus[logical].phys_id, (unsigned long long)(ns / 1000000), (long long)aff);
	return VAYU_ARRIVAL_NEVER;
}

void
vayu_smp_cpu_running(unsigned int logical)
{
	if (logical >= MAX_CPUS) {
		return;
	}
	vayu_smp_crumb(logical, VAYU_CRUMB_INITCPU);
	os_atomic_store(&vayu_cpu_state[logical], VAYU_CPU_RUNNING, release);
	if (vayu_smp_on) {
		/* SGI 2, the stop: on this CPU's redistributor (pe_fiq.c set priority and group of all 32) */
		vm_offset_t gicr = vayu_gic_gicr_base(logical);
		if (gicr != 0) {
			*(volatile uint32_t *)(gicr + 0x10100) = 1u << VAYU_SGI_STOP;      /* GICR_ISENABLER0 */
			__builtin_arm_dsb(DSB_SY);
		}
	}
}

/* ---- stopping ---- */

static void __attribute__((noreturn))
vayu_smp_stop_self(void)
{
	unsigned int me = ml_get_cpu_number_local();

	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	if (me < MAX_CPUS) {
		os_atomic_store(&vayu_cpu_state[me], VAYU_CPU_STOPPING, release);
	}
	if (vayu_smp_hang_test && me + 1 == ml_get_topology_info()->num_cpus && me != 0) {
		for (;;) {
			__builtin_arm_wfe();    /* the negative test: never confirmed */
		}
	}
	vayu_gic_cpu_quiesce(false, true);
	if (me < MAX_CPUS) {
		os_atomic_store(&vayu_cpu_state[me], VAYU_CPU_OFF_PENDING, release);
	}
	__builtin_arm_dsb(DSB_SY);
	int32_t rc = (int32_t)vayu_psci_call(PSCI_CPU_OFF, 0, 0, 0);
	/* came back: the firmware refused; this CPU is parked, not off */
	if (me < MAX_CPUS) {
		vayu_cpu_off_rc[me] = rc;
		os_atomic_store(&vayu_cpu_state[me], VAYU_CPU_OFF_FAILED, release);
	}
	__builtin_arm_dsb(DSB_SY);
	for (;;) {
		__builtin_arm_wfe();
	}
}

static bool
vayu_smp_any_asked(void)
{
	return os_atomic_load(&vayu_cpus_asked, acquire) != 0;
}

static void vayu_smp_say(const char *line);

/* sleh_irq(), SGI 2, already EOI'd; and vayu_smp_poll(). Comes back only when nothing was asked of this CPU. */
static void
vayu_smp_take(void)
{
	int me = (int)ml_get_cpu_number_local();
	const ml_topology_info_t *topology = ml_get_topology_info();
	int stopper = os_atomic_load(&vayu_stopper, acquire);

	if (os_atomic_load(&vayu_stop_asked, acquire) != 0 && stopper >= 0 && stopper != me) {
		vayu_smp_stop_self();
	}
	if (stopper < 0 && os_atomic_load(&vayu_return_asker, acquire) >= 0 && me == (int)topology->boot_cpu->cpu_id) {
		/* a secondary asked: the boot CPU gives the machine back (returns only when no loader listens) */
		static uint32_t said;
		if (os_atomic_cmpxchg(&said, 0, 1, relaxed)) {
			char line[128];
			snprintf(line, sizeof(line), "VAYU SMP: cpu=%d (boot) takes over the way back from cpu=%d\n", me,
			    os_atomic_load(&vayu_return_asker, acquire));
			vayu_smp_say(line);
		}
		loader_return();
	}
}

static void
vayu_smp_sgi_stop(void)
{
	vayu_smp_take();
	os_atomic_inc(&vayu_gic_stray_stop, relaxed);
}

void
vayu_smp_poll(void)
{
	if (vayu_smp_on && vayu_smp_any_asked()) {
		vayu_smp_take();
	}
}

/* to the loader's log, and the log back to RAM at once: if the way back hangs after it, the line is in DRAM */
static void
vayu_smp_say(const char *line)
{
	loader_log_line(line);
	loader_log_sync();
}

bool
vayu_smp_stop_others(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	int boot = (int)topology->boot_cpu->cpu_id;
	char line[256];

	if (!vayu_smp_any_asked()) {
		return true;            /* one CPU: nothing else ever ran */
	}
	/* from here on this CPU stays this CPU: no interrupt, no preemption, no migration (the way back masks it anyway) */
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	int me = (int)ml_get_cpu_number_local();
	snprintf(line, sizeof(line), "VAYU SMP: the way back on cpu=%d (boot cpu=%d, stopper %d, asker %d)\n", me, boot,
	    os_atomic_load(&vayu_stopper, acquire), os_atomic_load(&vayu_return_asker, acquire));
	vayu_smp_say(line);
	if (me != boot && os_atomic_load(&vayu_stopper, acquire) < 0) {
		int none = -1;
		if (os_atomic_cmpxchg(&vayu_return_asker, none, me, acq_rel)) {
			snprintf(line, sizeof(line), "VAYU SMP: cpu=%d is not the boot CPU: cpu=%d gives the machine back\n", me, boot);
			vayu_smp_say(line);
			__builtin_arm_dsb(DSB_SY);
			(void)vayu_gic_send_sgi(topology->cpus[boot].phys_id, VAYU_SGI_STOP);
			__builtin_arm_sev();
		}
		vayu_smp_stop_self();
	}
	int none = -1;
	if (!os_atomic_cmpxchg(&vayu_stopper, none, me, acq_rel) && os_atomic_load(&vayu_stopper, acquire) != me) {
		vayu_smp_stop_self();   /* another CPU gives it back: this one is one of those it stops */
	}
	os_atomic_store(&vayu_stop_asked, 1, release);
	__builtin_arm_dsb(DSB_SY);
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		uint32_t s = os_atomic_load(&vayu_cpu_state[i], acquire);
		if ((int)i != me && (s == VAYU_CPU_RUNNING || s == VAYU_CPU_ASKED)) {
			(void)vayu_gic_send_sgi(topology->cpus[i].phys_id, VAYU_SGI_STOP);
		}
	}
	__builtin_arm_sev();

	uint64_t deadline;
	nanoseconds_to_deadline(VAYU_SMP_STOP_NS, &deadline);
	bool all = true;
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		uint32_t s = os_atomic_load(&vayu_cpu_state[i], acquire);
		int64_t aff = VAYU_PSCI_NOT_SUPPORTED;

		if ((int)i == me || s == VAYU_CPU_NONE) {
			continue;               /* this one; one that never ran */
		}
		if (s == VAYU_CPU_ABANDONED) {
			/* it never ran kernel code (vayu_smp_arrival): said, not waited for -- fail-safe, not fail-closed */
			aff = vayu_psci_call(PSCI_AFFINITY_INFO, topology->cpus[i].phys_id, 0, 0);
			snprintf(line, sizeof(line), "VAYU SMP: cpu=%u mpidr=0x%x never arrived (abandoned, never ran kernel code): "
			    "not waited for; AFFINITY_INFO %lld%s\n", i, topology->cpus[i].phys_id, (long long)aff,
			    aff == VAYU_PSCI_AFFINITY_OFF ? "" : " (the firmware does not say OFF: if it ever starts, its gate "
			    "switches it off)");
			vayu_smp_say(line);
			continue;
		}
		/* its own word first (it publishes OFF_PENDING last), then the firmware's */
		while (s != VAYU_CPU_OFF_PENDING && s != VAYU_CPU_OFF_FAILED && s != VAYU_CPU_OFF &&
		    mach_absolute_time() < deadline) {
			if (s == VAYU_CPU_ASKED && vayu_psci_call(PSCI_AFFINITY_INFO, topology->cpus[i].phys_id, 0, 0) ==
			    VAYU_PSCI_AFFINITY_OFF) {
				break;          /* asked, never came (or went by itself): the firmware says off */
			}
			s = os_atomic_load(&vayu_cpu_state[i], acquire);
		}
		if (s == VAYU_CPU_OFF_PENDING || s == VAYU_CPU_ASKED || s == VAYU_CPU_OFF) {
			do {
				aff = vayu_psci_call(PSCI_AFFINITY_INFO, topology->cpus[i].phys_id, 0, 0);
			} while (aff != VAYU_PSCI_AFFINITY_OFF && aff >= 0 && mach_absolute_time() < deadline);
		}
		bool off = aff == VAYU_PSCI_AFFINITY_OFF && s != VAYU_CPU_OFF_FAILED && s != VAYU_CPU_UNKNOWN;
		if (off) {
			os_atomic_store(&vayu_cpu_state[i], VAYU_CPU_OFF, release);
		}
		all = all && off;
		snprintf(line, sizeof(line), "VAYU SMP: cpu=%u mpidr=0x%x %s: state %s, AFFINITY_INFO %lld%s\n", i,
		    topology->cpus[i].phys_id, off ? "OFF" : "NOT CONFIRMED OFF", vayu_cpu_state_name(s), (long long)aff,
		    s == VAYU_CPU_OFF_FAILED ? ", CPU_OFF refused" : "");
		vayu_smp_say(line);
	}
	if (!all) {
		snprintf(line, sizeof(line), "VAYU SMP: a CPU of the kernel may still run: the machine is NOT given back to the "
		    "loader (fail-closed)\n");
		vayu_smp_say(line);
		kdb_printf_unbuffered("%s", line);
	}
	return all;
}

#if !defined(VAYU_PSCI_HOST_TEST)
/* ---- the self test ---- */

static volatile uint32_t vayu_xcall_seen[MAX_CPUS];

static void
vayu_xcall_mark(void *arg)
{
	(void)arg;
	os_atomic_store(&vayu_xcall_seen[cpu_number()], 1, release);
}

static bool
vayu_xcall_wait(int cpu)
{
	uint64_t deadline;

	nanoseconds_to_deadline(VAYU_SMP_TEST_NS, &deadline);
	while (os_atomic_load(&vayu_xcall_seen[cpu], acquire) == 0) {
		if (mach_absolute_time() > deadline) {
			return false;
		}
	}
	return true;
}

void
vayu_smp_selftest(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	int boot = (int)topology->boot_cpu->cpu_id;
	unsigned int pass = 0;

	uint32_t panic_on = 0;
	if (vayu_smp_on && PE_parse_boot_argn("vayu_smp_test_panic", &panic_on, sizeof(panic_on)) && panic_on != 0 &&
	    panic_on < topology->num_cpus && cpu_to_processor((int)panic_on) != PROCESSOR_NULL) {
		/* K2b, a negative test: the panic (and so the way back) on a secondary, as a user thread that migrated */
		thread_bind(cpu_to_processor((int)panic_on));
		thread_block(THREAD_CONTINUE_NULL);
		panic("VAYU SMP TEST: a panic on cpu %d (vayu_smp_test_panic=%u)", cpu_number(), panic_on);
	}
	if (!vayu_smp_on || !vayu_arg("vayu_smp_test")) {
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		processor_t p = cpu_to_processor((int)i);
		if (p == PROCESSOR_NULL) {
			printf("VAYU SMP TEST: cpu=%u FAIL: no processor\n", i);
			continue;
		}
		thread_bind(p);
		thread_block(THREAD_CONTINUE_NULL);
		int on = cpu_number();
		uint64_t t0 = os_atomic_load(&vayu_timer_ticks[i], relaxed);
		IOSleep(20);
		uint64_t t1 = os_atomic_load(&vayu_timer_ticks[i], relaxed);
		bool to_boot = true, from_boot = true;
		if ((int)i != boot) {
			/* from this CPU to the boot CPU */
			os_atomic_store(&vayu_xcall_seen[boot], 0, release);
			to_boot = cpu_xcall(boot, vayu_xcall_mark, (void *)1) == KERN_SUCCESS && vayu_xcall_wait(boot);
			/* and back, from the boot CPU */
			thread_bind(cpu_to_processor(boot));
			thread_block(THREAD_CONTINUE_NULL);
			os_atomic_store(&vayu_xcall_seen[i], 0, release);
			from_boot = cpu_xcall((int)i, vayu_xcall_mark, (void *)1) == KERN_SUCCESS && vayu_xcall_wait((int)i);
		}
		bool ok = on == (int)i && t1 > t0 && to_boot && from_boot;
		pass += ok;
		printf("VAYU SMP TEST: cpu=%u mpidr=0x%x ran on %d, timer %llu -> %llu, xcall to boot %s, from boot %s: %s\n",
		    i, topology->cpus[i].phys_id, on, t0, t1, to_boot ? "ok" : "LOST", from_boot ? "ok" : "LOST",
		    ok ? "PASS" : "FAIL");
	}
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
	printf("VAYU SMP TEST: %u/%u CPUs PASS\n", pass, topology->num_cpus);
}

static void
vayu_smp_init(void)
{
	/* every way back asks this, a single CPU's too (then at once true) */
	loader_set_stop_others(vayu_smp_stop_others);
	vayu_gic_set_stop_handler(vayu_smp_sgi_stop);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, vayu_smp_init);
#endif /* !VAYU_PSCI_HOST_TEST */

#endif /* ARM64_BOARD_CONFIG_VAYU || VAYU_PSCI_HOST_TEST */
