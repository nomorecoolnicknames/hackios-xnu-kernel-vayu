/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU: more than one CPU, and every one of them off before the loader gets the machine back (osfmk/arm64/psci_smp.c
 * says how). Used by machine_routines.c (the gate), SweetCPU (CPU_ON, a CPU's arrival, the self test), sleh.c (the
 * timer count), model_dep.c and debug.c (the poll of a CPU waiting with interrupts off), loader_return.c (the stop).
 */
#ifndef _ARM64_PSCI_SMP_H_
#define _ARM64_PSCI_SMP_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* PSCI 0.2+ results */
#define VAYU_PSCI_SUCCESS               0
#define VAYU_PSCI_NOT_SUPPORTED         (-1)
#define VAYU_PSCI_INVALID_PARAMETERS    (-2)
#define VAYU_PSCI_DENIED                (-3)
#define VAYU_PSCI_ALREADY_ON            (-4)
#define VAYU_PSCI_ON_PENDING            (-5)
#define VAYU_PSCI_AFFINITY_ON           0
#define VAYU_PSCI_AFFINITY_OFF          1
#define VAYU_PSCI_AFFINITY_ON_PENDING   2

/* each logical CPU, as the stop needs to know it */
enum vayu_cpu_state {
	VAYU_CPU_NONE = 0,      /* never asked to start (or the firmware refused: it never ran) */
	VAYU_CPU_ASKED,         /* CPU_ON returned 0: it may run from now on, whether or not it reached C */
	VAYU_CPU_RUNNING,       /* in the kernel (SweetCPU::initCPU) */
	VAYU_CPU_STOPPING,      /* taking the stop */
	VAYU_CPU_OFF_PENDING,   /* published: no more kernel work; PSCI CPU_OFF next */
	VAYU_CPU_OFF,           /* AFFINITY_INFO said OFF after that */
	VAYU_CPU_OFF_FAILED,    /* CPU_OFF came back: parked in WFE, NOT off */
	VAYU_CPU_UNKNOWN,       /* CPU_ON said ALREADY_ON / ON_PENDING: running something, not ours to stop */
	VAYU_CPU_ABANDONED,     /* CPU_ON 0, but no checkpoint at all in vayu_smp_arrive_ms: it never ran kernel code, and
	                         * if it comes now, its first instructions see the gate and switch it off (K2b) */
};

/*
 * K2b: the checkpoints a secondary leaves on its way in, from its first instruction (start.s sweet_secondary_entry,
 * MMU off: straight to memory) to SweetCPU::initCPU, in its slot of vayu_smp_slot[] (psci_smp.c). The boot CPU
 * watches them while it waits (vayu_smp_arrival) and says each one it sees, so the log has how far a CPU came even
 * when it never comes back. VAYU_CRUMB(step) is what is stored; start.s writes the same numbers.
 */
#define VAYU_CRUMB_TAG                  0x5ec00000u
#define VAYU_CRUMB(step)                ((uint64_t)(VAYU_CRUMB_TAG | (step)))
#define VAYU_CRUMB_ENTRY                1       /* sweet_secondary_entry: its first instructions, slot found */
#define VAYU_CRUMB_EL1                  2       /* at EL1, the early vectors and SCTLR_EL1 set (before the OS lock) */
#define VAYU_CRUMB_RESET                3       /* reset_vector: past the OS lock write */
#define VAYU_CRUMB_CPUDATA              4       /* reset_vector: its cpu data found by MPIDR */
#define VAYU_CRUMB_START                5       /* start_cpu */
#define VAYU_CRUMB_PREMMU               6       /* common_start: about to turn the MMU on */
#define VAYU_CRUMB_MMU                  7       /* common_start: the MMU is on (from here the late line, cached) */
#define VAYU_CRUMB_ARM_INIT             8       /* arm_init_cpu(), in C */
#define VAYU_CRUMB_INITCPU              9       /* SweetCPU::initCPU: in the kernel (state RUNNING) */
#define VAYU_CRUMB_ABANDONED            0xA000  /* it saw the gate closed: switches itself off (PSCI CPU_OFF) */
#define VAYU_CRUMB_OFF_REFUSED          0xB000  /* ... and CPU_OFF came back: parked in WFI, NOT off */
#define VAYU_CRUMB_NO_CPUDATA           0xD000  /* reset_vector found no cpu data / handler: switches itself off */
#define VAYU_CRUMB_FAULT                0xE000  /* | the step it was at: an exception before the kernel's vectors */

/* SweetCPU::startAll while it waits for a secondary's initCPU: what vayu_smp_arrival() says */
enum vayu_arrival {
	VAYU_ARRIVAL_WAIT = 0,  /* coming (or not yet due): keep waiting */
	VAYU_ARRIVAL_NEVER,     /* no checkpoint in vayu_smp_arrive_ms: abandoned (VAYU_CPU_ABANDONED), the return is safe */
	VAYU_ARRIVAL_GONE,      /* it switched itself off on its way in (an early exception, no cpu data, the gate) */
	VAYU_ARRIVAL_PARKED,    /* ... but CPU_OFF was refused: it is parked, NOT off */
};

/* machine_routines.c ml_parse_cpu_topology(), boot CPU, before printf: cpus= kept, or cut to 1 (and why) */
void vayu_smp_parse(uint32_t *cpus);
/* vayu27=1 vayu_smp=1 and a valid /chosen psci-method */
bool vayu_smp_enabled(void);

/* SweetCPU::startCPU: CPU_ON through the device tree's conduit, the CPU accounted for before the call */
int32_t vayu_psci_cpu_on(unsigned int logical, uint64_t target, uint64_t entry, uint64_t context);
/* SweetCPU::initCPU, on the CPU itself: it is in the kernel; SGI 2 is enabled on it */
void vayu_smp_cpu_running(unsigned int logical);
/* a secondary's checkpoint once its MMU is on (arm_init.c, SweetCPU): its slot's late line */
void vayu_smp_crumb(unsigned int logical, uint32_t step);
/* SweetCPU::startAll, each pass of its wait for `logical` (it says the checkpoints it sees; enum vayu_arrival) */
int vayu_smp_arrival(unsigned int logical);
/* the last checkpoint seen of `logical`, in words (for the timeout's panic) */
const char *vayu_smp_checkpoint_name(unsigned int logical);
/* SweetCPU::startAll: requested / available / selected, logical -> MPIDR, the conduit; then the self test */
void vayu_smp_report(void);
void vayu_smp_selftest(void);

/* a CPU waiting with interrupts off (the debugger's cross call, panic_spin_forever): take a stop or a return asked */
void vayu_smp_poll(void);

/* loader_return.c: every other CPU confirmed off (true), or not (false: do not give the machine back) */
bool vayu_smp_stop_others(void);

/* sleh_irq(): virtual timer interrupts taken, per logical CPU */
extern uint64_t vayu_timer_ticks[];

__END_DECLS

#endif /* _ARM64_PSCI_SMP_H_ */
