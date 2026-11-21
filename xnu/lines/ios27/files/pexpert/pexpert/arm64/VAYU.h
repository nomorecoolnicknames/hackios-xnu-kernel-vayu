/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Board config for the POCO X3 Pro ("vayu", Qualcomm SM8150 / Snapdragon 860): Kryo 485 = Cortex-A76 + Cortex-A55,
 * ARMv8.2, no PAC, GICv3, PSCI.
 *
 * The generic ARMv8 layer of Scion's tree, as SWEET.h describes it for the Redmi Note 10 Pro (SM7150) and FRANKEL for the
 * Pixel 10: SWEET is defined, so the machine layer (nos_arm_asm, nos_arm_pmap, sweet_secondary_entry, pe_fiq.c's GICv3
 * Group 1 path, SweetCPU's PSCI CPU_ON, sweet_a64e.c) is the same code. Taken from SWEET.h: the mechanisms only (no
 * monitor, 16K pages, GICv3 with two security states, the architected timer). NOT taken: anything that names one of the
 * Redmi's addresses. Those paths are compiled for SWEET && !A5XELTE && !FRANKEL && !VAYU (osfmk/arm64/start.s,
 * arm_vm_init.c, sweet_early.c); a vayu kernel writes nothing before the boot arguments are parsed.
 *
 * Where a vayu number comes from:
 *  - virtual timer INTID 19: the SM8150 numbers the architected timer PPIs 1/2/3/0 (secure phys, phys, virt, hyp) as
 *    the SM7150 does, so CNTV is 16 + 3 = 19. Run on the phone with the 8796 kernel as vtimer_intid=19
 *    (hakios repository devices/poco-x3-pro/README.md:135). The device tree's /arm-io/gic "virtual-timer-intid"
 *    still overrides it (pe_fiq.c), which is what a QEMU virt run (INTID 27) uses.
 *  - redistributor stride 0x20000 (RD + SGI frames, no VLPI): GIC-600 of the SM8150, 8 cores in one 1 MB region.
 *  - PSCI by SMC from EL1 (board_config.h default "smc #0"): Qualcomm's TZ answers PSCI at EL3; the 8796 kernel's
 *    osfmk/arm64/psci_smp.c uses SMC on the phone.
 *  - cores in MPIDR Aff1 (SWEET_AFF0_CORES 0, the board_config.h default): Kryo 485 reports MT=1 with the core number
 *    in Aff1. QEMU virt numbers cores in Aff0: run QEMU with one CPU until SweetCPU takes either.
 *
 * Page size: 16K (__ARM_16K_PG__), Apple's own on arm64 iOS, so iOS 27's dyld shared cache, its 16K slide info and
 * CodeDirectory page size need no 16K-on-4K handling (decision K0, plans/ios27-macos: rep-A-R1.md "4K/16K").
 */

#ifndef _PEXPERT_ARM64_VAYU_H
#define _PEXPERT_ARM64_VAYU_H

#define NO_MONITOR                1
#define NO_ECORE                  1

/* The generic layer (see above) and this board */
#define SWEET                     1
#define VAYU                      1

/*
 * All eight cores (four A55, three A76, the A76 "prime" core 7) as one homogeneous SMP cluster: cpus=N is clamped here.
 * One CPU unless cpus=N says otherwise (machine_routines.c), as on every SWEET board.
 */
#define SWEET_SMP_MAX_CPUS        8

#define ARM_ARCH_TIMER
#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7
#define __ARM_ENABLE_SWAP__       1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1
#define __ARM_PAN_AVAILABLE__     1
#define __ARM_16K_PG__            1

#define HAS_GIC_V3                1

#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif /* ! ASSEMBLER */

#define SWEET_VTIMER_INTID        19      // CNTV: PPI 3 on Qualcomm's numbering (see above)
#define SWEET_IPI_SGI             1       // SGI used for cross-CPU signals
#define GIC_SPURIOUS_IRQ          1023    // IRQ no. for GIC spurious interrupt
#define GICR_PE_SIZE              0x20000 // Size of each redistributor region (RD + SGI)

/*
 * The console: the phone has no UART the kernel may use (its log leaves through the loader's ring, osfmk/arm64/
 * loader_return.c on the 8796 line); a QEMU virt run has a PL011, found from the device tree's "serial-device" only.
 */
#define PL011_UART                1

/* GICv3 register definitions; see GICv3 spec (Arm IHI 0069G). The same as SWEET.h's. */
#define GICD_CTLR                 0x0

/* Non-secure view of GICD_CTLR (DS=0) */
#define GICD_CTLR_ENABLEGRP1      0x1
#define GICD_CTLR_ENABLEGRP1A     0x2
#define GICD_CTLR_ARE_NS          0x10
#define GICD_CTLR_RWP             0x80000000

#define GICR_TYPER                              0x08
#define GICR_WAKER                              0x14
#define GICR_IGROUPR0                           0x10080
#define GICR_ISENABLER0                         0x10100
#define GICR_IPRIORITYR0                        0x10400

#define GICR_TYPER_AFFINITY_VALUE_SHIFT         32
#define GICR_TYPER_LAST                         0x10

#define GICR_WAKER_PROCESSORSLEEP               0x2
#define GICR_WAKER_CHILDRENASLEEP               0x4

#define ICC_CTLR_EOIMODE                        0x2

#define ICC_SRE_SRE                             0x1
/* End of GICv3 register definitions */

#endif /* ! _PEXPERT_ARM64_VAYU_H */
