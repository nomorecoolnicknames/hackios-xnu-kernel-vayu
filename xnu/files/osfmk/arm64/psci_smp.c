/*
 * More than one processor on generic (non-Apple) ARMv8 boards.
 *
 * On Apple hardware the other cores are powered up by closed drivers. Here
 * the firmware does it, through the standard power interface of ARM systems
 * (PSCI): CPU_ON starts a core at an address of the caller's choice, CPU_OFF
 * stops the calling core for good. The address we give is the kernel's own
 * reset vector (LowResetVectorBase in start.s), so that a new core takes the
 * path Apple's cores take: it finds its cpu data by its MPIDR, turns the MMU
 * on and comes out in arm_init_cpu(). The calls are made with SMC or, under
 * a hypervisor that implements them itself, with HVC; the boot loader says
 * which (/chosen/psci-method).
 *
 * The platform expert (iokit/Kernel/GenericARMPE.cpp) registers the cores of
 * the device tree and starts them here, one after the other. Signals between
 * processors are interrupts of the GIC (gic_v3_irq.c).
 *
 * A new core begins with its MMU and caches off, and what it reads then it
 * reads from memory. On real hardware that is not the same as what the
 * running processors see: under Qualcomm's hypervisor (the phone this was
 * written for) a line that is dirty in another core's cache is simply not
 * there yet. QEMU has no cache and shows none of this. So before CPU_ON
 * everything that core will read before its MMU is on is written back to
 * memory (psci_smp_start_cpu()), and what it writes in that state is read
 * only after throwing away the reader's cached copy.
 *
 * The other half is the way out. The kernel ends by returning to the boot
 * loader (loader_return.c), and the loader gets back only what it started:
 * one core, the boot processor's. psci_smp_stop_others() makes every other
 * core switch itself off first, wherever it is.
 */
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <kern/sched_prim.h>
#include <kern/timer_call.h>
#include <kern/percpu.h>
#include <kern/thread.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>

#if defined(GENERIC_PSCI_SMP)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1
/*
 * SYSTEM_OFF and SYSTEM_RESET (PSCI 1.0 sections 5.6 and 5.7) are 32-bit calls even in the 64-bit
 * convention. They end the whole system, not just the calling core, and are made from the boot
 * processor only, after the others have gone (loader_return() has done that by the time it asks).
 */
#define PSCI_SYSTEM_OFF         0x84000008u
#define PSCI_SYSTEM_RESET       0x84000009u

#define PSCI_SMP_START_SECONDS  5       /* how long a started core is waited for */
#define PSCI_SMP_STOP_SECONDS   3       /* ... and the cores that were told to stop */

extern void loader_return(void);
extern void loader_log_putc(char c);
extern unsigned long gVirtBase, gPhysBase;

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t psci_smp_hvc;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Lsecondary_early_exception in start.s). That core writes it with its cache off; it has a
 * cache line to itself so that looking at memory does not disturb anything else.
 */
struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 64, "one cache line");

static uint32_t psci_smp_cpu_stage[MAX_CPUS];   /* PSCI_SMP_CPU_*, by cpu number */
static uint32_t psci_smp_started;               /* a bit per core that CPU_ON was granted for */

static int      psci_smp_stop_owner = -1;       /* the processor that began the return to the boot loader */
static bool     psci_smp_boot_wanted;           /* it was not the boot processor: that one is to finish */
static bool     psci_smp_boot_busy;             /* ... and has begun to */
static bool     psci_smp_stopping;              /* every other core switches itself off */
static bool     psci_smp_quiesced;              /* no other core will execute kernel work */

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (psci_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

static void psci_idle_init(void);
static void psci_idle_core_started(void);
static void psci_idle_final_counts(void);

void
psci_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		psci_smp_hvc = 1;
	}
	psci_idle_init();
}

/* A processor other than the boot processor says how far it has come. */
void
psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

/* ... all the way: from GenericARMCPU::initCPU(), where the scheduler takes it. */
void
psci_smp_cpu_running(void)
{
	psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * joined the scheduler, and says what became of it if it has not: on a phone the log is all
 * there is to go by.
 */
kern_return_t
psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline;
	int64_t status;

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, psci_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	psci_smp_start_fault.valid = 0;
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	status = psci_call(PSCI_CPU_ON, phys_id, entry, 0);
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);

	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (stage >= PSCI_SMP_CPU_RUNNING) {
			psci_idle_core_started();
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);

	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* nothing of it is left to stop */
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
		    cpu, phys_id, PSCI_SMP_START_SECONDS,
		    stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no sign of it since the firmware took the request");
	}
	return KERN_FAILURE;
}

/*
 * The calling processor switches itself off. Its interrupted work, if any, is abandoned where
 * it was: only for the end of the system.
 */
void
psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	/* leave nothing behind that would interrupt a core that is no longer there */
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	gic_v3_irq_cpu_shutdown();
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);

	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

static void
psci_smp_note(const char *what, int cpu)
{
	static const char prefix[] = "\npsci: cpu ";

	for (const char *c = prefix; *c; c++) {
		loader_log_putc(*c);
	}
	loader_log_putc((char)('0' + cpu));
	for (const char *c = what; *c; c++) {
		loader_log_putc(*c);
	}
}

/* Tell every core but this one to switch itself off (psci_smp_stop_here() is where they hear it). */
static void
psci_smp_stop_rest(void)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	os_atomic_store(&psci_smp_stopping, true, release);
	__builtin_arm_dsb(DSB_SY);
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
			gic_v3_irq_signal_stop(cdp->cpu_phys_id);
		}
	}
	/* for those that wait with their interrupts off (wait_while_mp_kdp_trap(), after a panic) */
	__builtin_arm_sev();
}

boolean_t
psci_smp_is_quiesced(void)
{
	return psci_smp_quiesced && cpu_number() == master_cpu;
}

/*
 * The system is going back to the boot loader (loader_return()). Returns on the boot processor
 * only, once the other cores are off -- or have been given up on. On any other processor it
 * does not return: that one hands the job to the boot processor, which the loader's side of
 * the return belongs to, and switches itself off like the rest.
 */
void
psci_smp_stop_others(void)
{
	int me, max_cpu;
	uint64_t now, deadline;

	if (psci_smp_started == 0) {
		psci_smp_quiesced = true;
		return;                 /* there is only this one */
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	psci_idle_final_counts();
	me = cpu_number();
	max_cpu = ml_get_max_cpu_number();
	if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
		psci_smp_cpu_off();     /* another processor is at it already */
	}

	if (me != master_cpu) {
		/*
		 * Nobody is stopped before the boot processor has answered: a core that is switched
		 * off while it holds a lock the boot processor is waiting for would take the return
		 * with it.
		 */
		psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
		os_atomic_store(&psci_smp_boot_wanted, true, release);
		__builtin_arm_dsb(DSB_SY);
		gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
		__builtin_arm_sev();
		now = mach_absolute_time();
		nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
		deadline += now;
		while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
			now = mach_absolute_time();
		}
		if (!os_atomic_load(&psci_smp_stopping, acquire)) {
			/*
			 * It does not hear (stuck with its interrupts off). There is no return without
			 * it; a loader that watches from its own core will find the kernel silent and
			 * take the log as it is, and so this goes into the log.
			 */
			psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
			psci_smp_stop_rest();
		}
		psci_smp_cpu_off();
	}

	psci_smp_stop_rest();
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
	deadline += now;
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
			continue;
		}
		while (now < deadline) {
			if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
				/* it has said it is going; the firmware knows when it has gone (if it can tell at all) */
				int64_t state = psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0);

				if (state == PSCI_AFFINITY_OFF || state < 0) {
					break;
				}
			}
			now = mach_absolute_time();
		}
		if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
		    psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0) != PSCI_AFFINITY_OFF) {
			psci_smp_note(" did not stop: it is left running\n", cpu);
		}
	}
	/* OFF is published only after a CPU abandons kernel work, even if firmware refuses CPU_OFF. */
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		if (cpu != me && (psci_smp_started & (1u << cpu)) &&
		    os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF) {
			return;
		}
	}
	psci_smp_quiesced = true;
}

/*
 * Where a processor learns that the system is ending: the interrupt psci_smp_stop_rest() sends,
 * and the loop in which the processors wait while another one panics (they have their
 * interrupts off there). Comes back if there is nothing of the kind going on.
 */
void
psci_smp_stop_here(void)
{
	if (psci_smp_started == 0) {
		return;
	}
	if (cpu_number() != master_cpu) {
		if (os_atomic_load(&psci_smp_stopping, acquire)) {
			psci_smp_cpu_off();
		}
		return;
	}
	if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
		psci_smp_boot_busy = true;
		loader_return();        /* stops the rest (psci_smp_stop_others()); back only if no loader listens */
		for (;;) {
			__builtin_arm_wfi();
		}
	}
}

/*
 * The system itself goes off or resets (PSCI SYSTEM_OFF / SYSTEM_RESET), not merely the kernel:
 * what "slide to power off" and a reboot from the system are supposed to do. Called from
 * loader_return() with the other processors already off, so the calling one -- the boot
 * processor -- is the only one left, which is what the specification asks for. The call does not
 * return when the firmware takes it; a negative status is its refusal (PSCI_ERROR_NOT_SUPPORTED
 * when the hypervisor implements only the CPU_* calls, as Qualcomm's may), and then the loader is
 * still there to take the run and its log.
 */
static int64_t
psci_system_end(uint64_t function)
{
	int64_t status;

	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);       /* nothing between the last quiesce and this */
	status = psci_call(function, 0, 0, 0);
	__builtin_arm_wsr("DAIFClr", DAIFSC_ALL);       /* only reached when the firmware refused */
	return status;
}

int64_t
psci_system_off(void)
{
	return psci_system_end(PSCI_SYSTEM_OFF);
}

int64_t
psci_system_reset(void)
{
	return psci_system_end(PSCI_SYSTEM_RESET);
}


/*
 * Idle by power collapse (boot-arg cpu_suspend=N, default 0: off, idle is WFI as before).
 *
 * WFI only stops a core's clock: it stays powered, at the voltage of its OSM row, and leaks like a
 * running one. PSCI CPU_SUSPEND with a power-down state lets the firmware switch the core off
 * until an interrupt for it arrives; it then starts again at the entry point we give, with its MMU
 * and caches off, like a core that CPU_ON started. That entry is the kernel's reset vector, and
 * the cpu data's reset handler is already resume_idle_cpu (cpu_machine_idle_init()): XNU's own way
 * back from a non-retention idle, start.s -> arm_init_idle_cpu() -> cpu_idle_exit(TRUE) ->
 * Idle_load_context(), which returns into machine_idle() as if WFI had ended. What the core lost
 * and the kernel does not set up again on that path is put back by psci_idle_resumed(): the GIC's
 * CPU interface (its ICC_* registers live in the core) and the timer's compare value.
 *
 * The power state is the SM8150's per-core "rail power collapse": arm,psci-suspend-param
 * 0x40000004 of the cpu idle states in the mainline sm8150 device tree (bit 30 power down, state
 * 4; entry/exit 355/909 us for the little cores, 241/1461 us for the big ones; read from
 * sm8150-hdk.dtb). Only the core, never its cluster:
 * the cluster state (0x4100c244) is for OS-initiated mode, which the firmware has not been put in.
 *
 * Those states are "local-timer-stop": a core that is off does not see its own timer. So:
 *   cpu_suspend=1  a core is switched off only when it has no deadline at all (no timer of its
 *                  own is due: psci_idle_deadline()): nothing can be missed, it wakes for an interrupt or a signal from
 *                  another processor (the GIC wakes it).
 *   cpu_suspend=2  also with a deadline at least cpu_suspend_min_us (5000) away. A thread of the
 *                  boot processor's, which never collapses, then signals the core
 *                  cpu_suspend_lead_us (2000, the exit latency and some) before it: a broadcast
 *                  timer in software. If that thread would not look in time, the core takes WFI.
 * cpu_suspend_cpus=MASK (default every core CPU_ON started; bit n = XNU cpu n) and
 * cpu_suspend_after_ms=N (20000: not during the boot) narrow it, cpu_suspend_state=X overrides the
 * power state, cpu_suspend_log_s=N (60; 0: none) is the period of the count line:
 *   "psci: idle: cpu N: S suspends, R back by reset, F returned, E refused (last E), L late, T WFI for a deadline"
 * A firmware that refuses the state 8 times in a row turns it off for good ("psci: idle: off").
 * The core that refuses goes on with WFI; so does any core under QEMU, whose PSCI rejects a
 * power-down state as invalid parameters.
 */
#define PSCI_CPU_SUSPEND        0xC4000001u     /* SMC64: the entry point is a 64-bit address */
#define PSCI_IDLE_STATE_DEFAULT 0x40000004u     /* sm8150 cpu rail power collapse (see above) */
#define PSCI_IDLE_REFUSALS      8

struct psci_idle_count {
	uint64_t suspends, resets, returned, refused, late, timed;
	int64_t  last_error;
	uint32_t refused_in_a_row;
} __attribute__((aligned(64)));

static uint32_t psci_idle_mode;                 /* cpu_suspend */
static uint32_t psci_idle_state = PSCI_IDLE_STATE_DEFAULT;
static uint32_t psci_idle_cpus = ~0u;
static uint32_t psci_idle_log_s = 60;
static uint64_t psci_idle_entry;                /* physical address of LowResetVectorBase */
static uint64_t psci_idle_from;                 /* mach_absolute_time() before which nothing collapses */
static uint64_t psci_idle_min_abs, psci_idle_lead_abs;
static bool     psci_idle_off;                  /* the firmware refuses it */
static bool     psci_idle_thread_started;
static struct psci_idle_count psci_idle_count[MAX_CPUS];
/* cpu_suspend=2: when each core that is off wants its signal (0: none), and when the boot processor's
 * thread looks next (UINT64_MAX: it does not run, nobody may count on it) */
static uint64_t psci_idle_wake_at[MAX_CPUS];
static uint64_t psci_idle_watch_next = UINT64_MAX;

extern void gic_v3_irq_cpu_resume(void);

static void
psci_idle_init(void)
{
	uint32_t after_ms = 20000, min_us = 5000, lead_us = 2000;

	if (!PE_parse_boot_argn("cpu_suspend", &psci_idle_mode, sizeof(psci_idle_mode)) || psci_idle_mode == 0) {
		psci_idle_mode = 0;
		return;
	}
	PE_parse_boot_argn("cpu_suspend_state", &psci_idle_state, sizeof(psci_idle_state));
	PE_parse_boot_argn("cpu_suspend_cpus", &psci_idle_cpus, sizeof(psci_idle_cpus));
	PE_parse_boot_argn("cpu_suspend_after_ms", &after_ms, sizeof(after_ms));
	PE_parse_boot_argn("cpu_suspend_min_us", &min_us, sizeof(min_us));
	PE_parse_boot_argn("cpu_suspend_lead_us", &lead_us, sizeof(lead_us));
	PE_parse_boot_argn("cpu_suspend_log_s", &psci_idle_log_s, sizeof(psci_idle_log_s));
	if (lead_us < 1000) {
		lead_us = 1000;
	}
	if (min_us < lead_us + 1000) {
		min_us = lead_us + 1000;
	}
	nanoseconds_to_absolutetime((uint64_t)min_us * NSEC_PER_USEC, &psci_idle_min_abs);
	nanoseconds_to_absolutetime((uint64_t)lead_us * NSEC_PER_USEC, &psci_idle_lead_abs);
	nanoseconds_to_absolutetime((uint64_t)after_ms * NSEC_PER_MSEC, &psci_idle_from);
	psci_idle_from += mach_absolute_time();
	psci_idle_entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	printf("psci: idle: CPU_SUSPEND state 0x%x (cpu_suspend=%u: %s), cpus 0x%x, from %u ms on\n", psci_idle_state,
	    psci_idle_mode, psci_idle_mode >= 2 ? "also with a deadline, the boot processor signals" : "only with no deadline",
	    psci_idle_cpus, after_ms);
}

static void
psci_idle_say(const char *line)
{
	for (const char *c = line; *c; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

/* The count line (also for other drivers' power lines): one core's, into `line'. */
void
psci_idle_counts(int cpu, char *line, size_t size)
{
	const struct psci_idle_count *n = &psci_idle_count[cpu];

	snprintf(line, size, "psci: idle: cpu %d: %llu suspends, %llu back by reset, %llu returned, %llu refused "
	    "(last %lld), %llu late, %llu WFI for a deadline%s\n", cpu, n->suspends, n->resets, n->returned, n->refused,
	    n->last_error, n->late, n->timed,
	    psci_idle_off ? " (psci: idle: off, the firmware refuses the state)" : "");
}

/* The power collapses every core came back from, for the battery's power line (battery_log_s). */
uint64_t
psci_idle_resets_total(void)
{
	uint64_t total = 0;

	for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
		total += psci_idle_count[cpu].resets;
	}
	return total;
}

/*
 * The boot processor's thread: the counts every cpu_suspend_log_s, and with cpu_suspend=2 the signal
 * to every core that is off and whose deadline is near. Before it looks at the deadlines it says
 * when it will look next (psci_idle_watch_next); a core that goes off with a deadline first writes
 * its own and then reads that, so one of the two always sees the other.
 */
static void
psci_idle_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t log_every = 0, log_next, idle_look;

	thread_bind(master_processor);
	thread_block(THREAD_CONTINUE_NULL);     /* (now on the boot processor) */
	nanoseconds_to_absolutetime((uint64_t)psci_idle_log_s * NSEC_PER_SEC, &log_every);
	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &idle_look);
	log_next = mach_absolute_time() + log_every;
	for (;;) {
		uint64_t now = mach_absolute_time(), next = log_every != 0 ? log_next : now + 10 * idle_look;
		int max_cpu = ml_get_max_cpu_number();

		if (psci_idle_mode >= 2 && !psci_idle_off) {
			if (now + idle_look < next) {
				next = now + idle_look;
			}
			os_atomic_store(&psci_idle_watch_next, next, seq_cst);
			for (int cpu = 0; cpu <= max_cpu && cpu < MAX_CPUS; cpu++) {
				uint64_t wake = os_atomic_load(&psci_idle_wake_at[cpu], seq_cst);

				if (wake == 0) {
					continue;
				}
				if (wake <= now) {
					if (os_atomic_cmpxchg(&psci_idle_wake_at[cpu], wake, 0, relaxed) && cpu_datap(cpu) != NULL) {
						gic_v3_irq_signal(cpu_datap(cpu)->cpu_phys_id);
					}
				} else if (wake < next) {
					next = wake;
				}
			}
			os_atomic_store(&psci_idle_watch_next, next, seq_cst);
		}
		if (log_every != 0 && now >= log_next) {
			log_next = now + log_every;
			for (int cpu = 0; cpu <= max_cpu && cpu < MAX_CPUS; cpu++) {
				char line[200];

				if ((psci_smp_started & psci_idle_cpus & (1u << cpu)) != 0) {
					psci_idle_counts(cpu, line, sizeof(line));
					psci_idle_say(line);
				}
			}
			if (log_next < next) {
				next = log_next;
			}
		}
		if (next <= mach_absolute_time()) {
			continue;
		}
		assert_wait_deadline_with_leeway((event_t)&psci_idle_watch_next, THREAD_UNINT, TIMEOUT_URGENCY_SYS_CRITICAL,
		    next, 0);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* The counts once more at the end of the run (psci_smp_stop_others(), into the loader's log only). */
static void
psci_idle_final_counts(void)
{
	static bool said;

	if (psci_idle_mode == 0 || said) {
		return;
	}
	said = true;
	for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
		char line[200];

		if ((psci_smp_started & psci_idle_cpus & (1u << cpu)) != 0) {
			psci_idle_counts(cpu, line, sizeof(line));
			psci_idle_say(line);
		}
	}
}

/* From psci_smp_start_cpu(), on the boot processor, once a core has joined. */
static void
psci_idle_core_started(void)
{
	thread_t thread;

	if (psci_idle_mode == 0 || psci_idle_thread_started) {
		return;
	}
	psci_idle_thread_started = true;
	if (kernel_thread_start_priority(psci_idle_thread, NULL, MAXPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("psci: idle: cannot start the boot processor's thread: no deadline is signalled\n");
	}
}

/*
 * The next thing this core's timer is for, as timer_resync_deadlines() works it out (arm_timer.c):
 * rtcPop is not it -- with nothing due it is the furthest the decrementer reaches, not the end of
 * all time.
 */
static uint64_t
psci_idle_deadline(cpu_data_t *cdp)
{
	const rtclock_timer_t *timer = &cdp->rtclock_timer;
	uint64_t deadline = EndOfAllTime, run;

	if (!timer->has_expired && timer->deadline > 0 && timer->deadline < deadline) {
		deadline = timer->deadline;
	}
	if (cdp->idle_timer_deadline > 0 && cdp->idle_timer_deadline < deadline) {
		deadline = cdp->idle_timer_deadline;
	}
	run = running_timers_deadline(PERCPU_GET_RELATIVE(processor, cpu_data, cdp));
	return run < deadline ? run : deadline;
}

/*
 * cpu_idle(), interrupts off, instead of WFI: true if the core went into the firmware and came
 * straight back (an interrupt was already pending, or the firmware gave up the power down; WFI would
 * end at once too), false for WFI as usual. When the core is switched off this does not return: the
 * core comes back through the reset vector to psci_idle_resumed().
 */
bool
psci_idle_enter(cpu_data_t *cdp)
{
	int cpu = cdp->cpu_number;
	struct psci_idle_count *n;
	uint64_t now, pop, wake = 0;
	int64_t status;

	if (psci_idle_mode == 0 || psci_idle_off || cpu < 0 || cpu >= MAX_CPUS || cpu == master_cpu ||
	    (psci_smp_started & psci_idle_cpus & (1u << cpu)) == 0 ||
	    os_atomic_load(&psci_smp_cpu_stage[cpu], relaxed) != PSCI_SMP_CPU_RUNNING ||
	    os_atomic_load(&psci_smp_stopping, relaxed)) {
		return false;
	}
	now = mach_absolute_time();
	if (now < psci_idle_from) {
		return false;
	}
	n = &psci_idle_count[cpu];
	pop = psci_idle_deadline(cdp);
	if (pop != EndOfAllTime) {
		if (psci_idle_mode < 2 || pop < now + psci_idle_min_abs) {
			psci_idle_count[cpu].timed++;   /* (a deadline too near, or cpu_suspend=1: WFI) */
			return false;
		}
		wake = pop - psci_idle_lead_abs;
		os_atomic_store(&psci_idle_wake_at[cpu], wake, seq_cst);
		if (os_atomic_load(&psci_idle_watch_next, seq_cst) > wake) {
			os_atomic_store(&psci_idle_wake_at[cpu], 0, relaxed);
			n->late++;              /* (the boot processor's thread would look too late) */
			return false;
		}
	}
	if (cpu_interrupt_is_pending()) {
		os_atomic_store(&psci_idle_wake_at[cpu], 0, relaxed);
		return false;
	}
	n->suspends++;
	/* what the reset vector reads of this core with its MMU off (the rest: psci_smp_start_cpu()) */
	CleanPoC_DcacheRegion((vm_offset_t)cdp, sizeof(cpu_data_t));
	CleanPoC_DcacheRegion((vm_offset_t)n, sizeof(*n));
	__builtin_arm_dsb(DSB_SY);
	status = psci_call(PSCI_CPU_SUSPEND, psci_idle_state, psci_idle_entry, 0);

	os_atomic_store(&psci_idle_wake_at[cpu], 0, relaxed);
	if (status == 0) {
		n->returned++;
		n->refused_in_a_row = 0;
		return true;
	}
	n->refused++;
	n->last_error = status;
	if (++n->refused_in_a_row >= PSCI_IDLE_REFUSALS) {
		psci_idle_off = true;
	}
	return false;
}

/* arm_init_idle_cpu(), interrupts off, on the way back from a power collapse. */
void
psci_idle_resumed(void)
{
	int cpu = cpu_number();

	if (cpu >= 0 && cpu < MAX_CPUS) {
		os_atomic_store(&psci_idle_wake_at[cpu], 0, relaxed);
		psci_idle_count[cpu].resets++;
		psci_idle_count[cpu].refused_in_a_row = 0;
	}
	gic_v3_irq_cpu_resume();
	ClearIdlePop(FALSE);                    /* the timer's compare value went with the core */
}

#endif /* defined(GENERIC_PSCI_SMP) */
