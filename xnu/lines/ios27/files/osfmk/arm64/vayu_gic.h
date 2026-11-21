/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (POCO X3 Pro, GIC-600): what sleh.c, SweetCPU, psci_smp.c and the drivers use of the interrupt controller
 * beyond pexpert/arm/pe_fiq.c's set-up (osfmk/arm64/gic_v3_irq.c says how and why).
 *
 * One owner of ICC_IAR1_EL1 / ICC_EOIR1_EL1: sleh_irq() (osfmk/arm64/sleh.c). It reads the IAR, publishes it in this
 * CPU's slot (vayu_gic_iar_taken), EOIs it and clears the slot (vayu_gic_iar_done); a way out (the loader's return, a
 * CPU that is switched off) ends a slot that is still set, on its own CPU only (vayu_gic_cpu_quiesce).
 *
 * SGIs: VAYU_SGI_SIGNAL (1, SWEET_IPI_SGI) is cpu_signal(); VAYU_SGI_STOP (2) is the order to stop for good, EOI'd
 * before its handler, which does not come back. With vayu27=1 vayu_spi=1 any other SGI is ended and counted, and an
 * SPI nobody attached is masked and counted (no storm); without them sleh_irq() is Scion's.
 */
#ifndef _ARM64_VAYU_GIC_H_
#define _ARM64_VAYU_GIC_H_

#include <stdint.h>
#include <stdbool.h>
#include <mach/kern_return.h>

#define VAYU_SGI_SIGNAL         1
#define VAYU_SGI_STOP           2
#define VAYU_GIC_IAR_VALID      (1ULL << 63)    /* INTID 0 is a valid IAR: the slot's mark */

/* the SPI ABI of the 8796 line (the iOS 16.5 line's osfmk/arm64/gic_v3_irq.c), for the vayu drivers */
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);

/* vayu27=1 vayu_spi=1 (read once on the boot CPU) */
bool vayu_gic_spi_enabled(void);

/* sleh_irq(), interrupts off */
void vayu_gic_iar_taken(unsigned int cpu, uint64_t iar);
void vayu_gic_iar_done(unsigned int cpu);
bool vayu_gic_spi_handle(uint32_t intid);       /* true: a driver's, or masked and counted (vayu_spi=1) */
bool vayu_gic_sgi_unknown(uint32_t intid);      /* true: counted, the caller only EOIs it (vayu_spi=1) */
void vayu_gic_sgi_stop(void);                   /* SGI 2, after its EOI: the registered stop, else counted */

/* psci_smp.c: who answers SGI 2 (it does not return) */
void vayu_gic_set_stop_handler(void (*fn)(void));

/* ICC_SGI1R_EL1 for the CPU with that MPIDR (Aff1:Aff0 of the ADT's reg, Aff2 too); ~0 when Aff0 >= 16 */
uint64_t vayu_gic_sgi1r(uint32_t phys_id, uint32_t sgi);
/* send it (dsb ishst before, isb after); false when the MPIDR cannot be addressed */
bool vayu_gic_send_sgi(uint32_t phys_id, uint32_t sgi);

/*
 * This CPU leaves the kernel (the loader's return, PSCI CPU_OFF): its active IAR ended, every SPI attached disabled
 * when all is set, its redistributor's SGIs/PPIs disabled and cleared when local_off, Group 1 off then. No lock, no
 * printf: a panic's way out uses it.
 */
void vayu_gic_cpu_quiesce(bool all_spis, bool local_off);

/* counters for the logs */
extern uint64_t vayu_gic_unhandled_spi, vayu_gic_unknown_sgi, vayu_gic_stray_stop;
extern uint32_t vayu_gic_last_unhandled;

#endif /* _ARM64_VAYU_GIC_H_ */
