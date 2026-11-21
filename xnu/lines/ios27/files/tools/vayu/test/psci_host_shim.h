/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * The kernel names osfmk/arm64/psci_smp.c's stop path uses, for its host test (psci_host_test.c): CPUs as threads
 * (a thread-local logical number), a fake PSCI firmware (CPU_ON, CPU_OFF, AFFINITY_INFO with faults to order), SGI 2
 * as a per-CPU flag, the ARM barriers as fences, WFE in a CPU that has gone as the end of its thread.
 */
#ifndef PSCI_HOST_SHIM_H
#define PSCI_HOST_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "../../../osfmk/arm64/psci_smp.h"

typedef uintptr_t vm_offset_t;
#define MAX_CPUS 8
#define SECURITY_READ_ONLY_LATE(x) x

#define os_atomic_load(p, m)            __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define os_atomic_store(p, v, m)        __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define os_atomic_inc(p, m)             __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define os_atomic_cmpxchg(p, e, v, m)   ({ __typeof__(*(p)) _e = (e); \
	__atomic_compare_exchange_n((p), &_e, (v), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })

#define DAIFSC_ALL 0xf
#define DSB_SY 0xf
#define __builtin_arm_wsr(r, v)         ((void)(v))
#define __builtin_arm_dsb(x)            __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define __builtin_arm_sev()             __atomic_thread_fence(__ATOMIC_SEQ_CST)
void host_wfe(void);
#define __builtin_arm_wfe()             host_wfe()

typedef struct { uint32_t cpu_id, phys_id; } ml_topology_cpu_t;
typedef struct { unsigned int num_cpus; ml_topology_cpu_t cpus[MAX_CPUS]; ml_topology_cpu_t *boot_cpu; } ml_topology_info_t;
extern ml_topology_info_t host_topology;
static inline const ml_topology_info_t *ml_get_topology_info(void) { return &host_topology; }
extern __thread int host_cpu;
static inline unsigned int ml_get_cpu_number_local(void) { return (unsigned int)host_cpu; }

static inline uint64_t mach_absolute_time(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static inline void nanoseconds_to_deadline(uint64_t ns, uint64_t *d) { *d = mach_absolute_time() + ns; }
#define kdb_printf_unbuffered printf

int64_t host_psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3);
#define VAYU_SGI_STOP 2
bool vayu_gic_send_sgi(uint32_t phys_id, uint32_t sgi);
void vayu_gic_cpu_quiesce(bool all_spis, bool local_off);
static inline vm_offset_t vayu_gic_gicr_base(unsigned int cpu) { (void)cpu; return 0; }
extern uint64_t vayu_gic_stray_stop;
void loader_log_line(const char *line);
void loader_return(void);

#endif
