/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * The kernel names osfmk/arm64/gic_v3_irq.c uses, for its host test (gic_host_test.c): a fake distributor with the
 * set/clear-enable semantics of the real one, pthread spin locks for the simple lock, a thread-local "interrupt
 * context" and interrupt mask. Only what that file needs.
 */
#ifndef GIC_HOST_SHIM_H
#define GIC_HOST_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

typedef int kern_return_t;
typedef uintptr_t vm_offset_t;
typedef int boolean_t;
#define TRUE  1
#define FALSE 0
#define KERN_SUCCESS              0
#define KERN_INVALID_ARGUMENT     4
#define KERN_FAILURE              5
#define KERN_RESOURCE_SHORTAGE    6
#define KERN_NOT_SUPPORTED        46
#define KERN_NAME_EXISTS          13
#define KERN_OPERATION_TIMED_OUT  49
#define MAX_CPUS 8

#define os_atomic_load(p, m)      __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define os_atomic_store(p, v, m)  __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define os_atomic_inc(p, m)       __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define os_atomic_dec(p, m)       __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)

extern pthread_spinlock_t host_spi_lock;
extern __thread int host_in_irq, host_irq_off;
#define SPI_LOCK(s)     do { (s) = !host_irq_off; host_irq_off = 1; pthread_spin_lock(&host_spi_lock); } while (0)
#define SPI_UNLOCK(s)   do { pthread_spin_unlock(&host_spi_lock); host_irq_off = !(s); } while (0)
#define simple_lock(l, g)       pthread_spin_lock(&host_spi_lock)
#define simple_unlock(l)        pthread_spin_unlock(&host_spi_lock)
static inline boolean_t ml_at_interrupt_context(void) { return host_in_irq; }

/* the fake distributor: 64 KB of registers, ISENABLER/ICENABLER folded onto one enable bitmap */
extern uint32_t host_gicd[0x10000 / 4];
extern uint32_t host_enabled[32];
uint32_t host_r32(vm_offset_t a);
void host_w32(vm_offset_t a, uint32_t v);
#define GIC_R32(a)      host_r32((vm_offset_t)(a))
#define GIC_W32(a, v)   host_w32((vm_offset_t)(a), (v))
#define GIC_W8(a, v)    (*(volatile uint8_t *)(a) = (v))
#define GIC_W64(a, v)   (*(volatile uint64_t *)(a) = (v))
#define GIC_DSB()       __atomic_thread_fence(__ATOMIC_SEQ_CST)

static inline vm_offset_t vayu_gic_gicd_base(void) { return (vm_offset_t)host_gicd; }
static inline bool vayu27_enabled(void) { return true; }
static inline bool vayu_arg(const char *n) { (void)n; return true; }

typedef struct { uint32_t phys_id; } ml_topology_cpu_t;
typedef struct { const ml_topology_cpu_t *boot_cpu; } ml_topology_info_t;
const ml_topology_info_t *ml_get_topology_info(void);

static inline uint64_t mach_absolute_time(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static inline void nanoseconds_to_deadline(uint64_t ns, uint64_t *d) { *d = mach_absolute_time() + ns; }
#define panic(...)      do { fprintf(stderr, "panic: " __VA_ARGS__); fputc('\n', stderr); abort(); } while (0)

/* the prototypes of osfmk/arm64/vayu_gic.h */
#define VAYU_GIC_IAR_VALID      (1ULL << 63)
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);
bool vayu_gic_spi_enabled(void);
bool vayu_gic_spi_handle(uint32_t intid);
uint64_t vayu_gic_sgi1r(uint32_t phys_id, uint32_t sgi);
extern uint64_t vayu_gic_unhandled_spi;

#endif
