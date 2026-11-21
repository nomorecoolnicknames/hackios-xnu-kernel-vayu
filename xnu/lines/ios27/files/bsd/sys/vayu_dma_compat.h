/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * What the vayu drivers of the 8796 line used for DMA memory and device mappings, where xnu-12377 declares it
 * (sys/vayu_12377_abi.h, rule 3). A driver that included <vm/vm_kern.h> for kmem_alloc_contig() and declared
 * ml_io_map()/ml_io_map_wcomb()/kvtophys()/flush_dcache() itself includes this instead.
 *
 * Scion's sys/hakios.xnu.h has the same declarations for the shared drivers (and ml_io_unmap(), FlushPoC_DcacheRegion(),
 * loader_log_line()); either may be included, the prototypes agree.
 *
 * Owner: K3 (VM / pmap / DMA). The contract below is K3's (osfmk/arm64/vayu_mem.c, osfmk/arm/pmap/pmap.c).
 *
 * THE DMA CONTRACT ON VAYU (xnu-12377, 16K pages, no monitor)
 *
 * 1. DMA memory is write-back RAM, kept coherent by hand. Get it from vayu_dma_alloc() (or _contig_below4g() for a
 *    32-bit master): physically contiguous, zeroed, page aligned, every page checked with kvtophys(), wholly below
 *    max_pa. The device is given *pa; the CPU uses *va. Before the device reads: vayu_dma_sync_for_device(va, len).
 *    After it has written (its completion seen): vayu_dma_sync_for_cpu(va, len), on whole cache lines only (else
 *    false and nothing done). Between the two the CPU does not write those lines.
 *
 * 2. Do NOT make DMA RAM uncached with pmap_set_cache_attributes(..., VM_WIMG_WCOMB / _RT / _IO) -- the 8796 line's
 *    USB rings did (b4-tree vayu_usbnet.c dma_area()). On this kernel __ARM_PTE_PHYSMAP__ is 0 (no XNU_MONITOR,
 *    proc_reg.h) and the physical aperture is built of WB L2 blocks (arm_vm_init.c arm_vm_physmap_init()):
 *    pmap_set_cache_attributes() changes the page's other mappings but leaves its WB alias in the aperture. A
 *    speculative fill through that alias can put a stale or dirty line over what the device wrote. vayu_mem_diag=1
 *    counts such pages ("made non-WB by pmap_set_cache_attributes()").
 *
 * 3. Structures that the CPU and the device both write within one cache line (xHCI/DWC3 TRB rings, UFS UTRDs with
 *    the controller's status next to the CPU's fields) cannot be kept coherent by 1. Until there is uncached memory
 *    without a WB alias, such a driver keeps the halves apart (each cache line written by one side only, the
 *    producer's index in a line of its own) or stays off. CLOSED BLOCKER, vayu_dma_pte=1 (not implemented): a
 *    page-granular aperture for the DMA RAM (no contiguous hint, L3 entries), break-before-make of the alias with
 *    TLBI/DSB on every CPU, cache clean before the switch, extra page tables (~3 MiB of L3 for 6 GiB). Another way out
 *    that needs no pmap change: RAM outside the window that the loader names to the kernel (as it does /pram and the
 *    framebuffer), mapped only by ml_io_map_wcomb() -- with vayu_wcomb_safe=1 that is Normal NC only when the loader
 *    record is accepted (osfmk/arm/vayu_mem_rules.h); it needs a loader record (`/chosen/hakios-dma-pool`), not here.
 *
 * 4. ml_io_map_wcomb() of anything but RAM is a mistake: with vayu27=1 vayu_wcomb_safe=1 it gives Device memory
 *    (WCOMB is Normal NC only for the window, the extra RAM and the loader's hand-off records). Register blocks are
 *    ml_io_map(). A 16K page maps the whole granule around a 4K block: do not touch the neighbours.
 *
 * 5. Physical addresses are 64-bit everywhere (ptoa((uint64_t)pn), not pn << PAGE_SHIFT); an IOVA behind an SMMU is
 *    not the PA. CPU pages are 16K; a device's own page tables keep the device's granule (4K for the SMMU/GSI).
 */
#ifndef _SYS_VAYU_DMA_COMPAT_H_
#define _SYS_VAYU_DMA_COMPAT_H_

#ifdef XNU_KERNEL_PRIVATE

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/vm_kern_xnu.h>     /* kmem_alloc_contig(), kmem_free(): vm/vm_kern.h on xnu-8796 */

__BEGIN_DECLS
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void flush_dcache(vm_offset_t addr, unsigned count, int phys);

/*
 * K3 (osfmk/arm64/vayu_mem.c). vayu_dma_alloc: size bytes (rounded up to pages, at most 1 GiB), the last byte below
 * max_pa (0: anywhere), the first page aligned to align (0, or a power of two >= PAGE_SIZE). KERN_SUCCESS: *va, *pa
 * set. Any other answer: nothing allocated, *va and *pa 0. May block (it is not for interrupt context).
 */
extern kern_return_t vayu_dma_alloc(vm_size_t size, uint64_t max_pa, uint64_t align, vm_offset_t *va, uint64_t *pa);
extern kern_return_t vayu_dma_alloc_contig_below4g(vm_size_t size, vm_offset_t *va, uint64_t *pa);
extern void vayu_dma_free(vm_offset_t va, vm_size_t size);
/* clean+invalidate to the point of coherency, with the barriers; false: empty or wrapping range (for_cpu: or not
 * whole cache lines), nothing done */
extern bool vayu_dma_sync_for_device(vm_offset_t va, vm_size_t len);
extern bool vayu_dma_sync_for_cpu(vm_offset_t va, vm_size_t len);
__END_DECLS

#endif /* XNU_KERNEL_PRIVATE */
#endif /* _SYS_VAYU_DMA_COMPAT_H_ */
