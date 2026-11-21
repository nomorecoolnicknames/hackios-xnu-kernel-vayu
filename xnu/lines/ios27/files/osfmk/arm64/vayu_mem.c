/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (K3): DMA memory for the vayu drivers, and what vayu_mem_diag=1 says about this kernel's memory. New work of
 * this tree; the contract is in bsd/sys/vayu_dma_compat.h.
 *
 * DMA memory here is ordinary write-back RAM: physically contiguous pages from kmem_alloc_contig(), mapped WB in the
 * kernel map, with the same WB alias in the physical aperture. Data reaches the device and comes back by cache
 * maintenance to the point of coherency (vayu_dma_sync_for_device / _for_cpu), never by switching the pages to WCOMB:
 * this kernel has no monitor (VAYU.h), so __ARM_PTE_PHYSMAP__ is 0, the aperture is built of WB L2 blocks
 * (arm_vm_init.c) and pmap_set_cache_attributes() leaves it as it is -- a WCOMB page would keep a WB alias there.
 */
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <machine/atomic.h>
#include <vm/pmap.h>
#include <vm/vm_kern_xnu.h>
#include <arm/caches_internal.h>
#include <arm/pmap.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>

/*
 * What this file and the DMA contract are written for; a build that changes any of it has to look at them again.
 */
#if defined(ARM_LARGE_MEMORY)
#error "VAYU: ARM_LARGE_MEMORY changes the kernel's VA layout and link address (K3: keep it off)"
#endif
#if !defined(__ARM_16K_PG__) || !__ARM_16K_PG__
#error "VAYU: 16K pages (VAYU.h)"
#endif
#if XNU_MONITOR
#error "VAYU: no PPL on this board"
#endif
#if __ARM_PTE_PHYSMAP__
#error "VAYU: the DMA contract assumes the physical aperture is not updated by pmap_set_cache_attributes()"
#endif

/* ---- the DMA calls ---- */

/* the smallest data cache line (CTR_EL0.DminLine) */
static vm_size_t
vayu_dma_line(void)
{
	uint64_t ctr = __builtin_arm_rsr64("CTR_EL0");

	return (vm_size_t)4 << ((ctr >> 16) & 0xf);
}

kern_return_t vayu_dma_alloc(vm_size_t size, uint64_t max_pa, uint64_t align, vm_offset_t *va, uint64_t *pa);
kern_return_t vayu_dma_alloc_contig_below4g(vm_size_t size, vm_offset_t *va, uint64_t *pa);
void vayu_dma_free(vm_offset_t va, vm_size_t size);
bool vayu_dma_sync_for_device(vm_offset_t va, vm_size_t len);
bool vayu_dma_sync_for_cpu(vm_offset_t va, vm_size_t len);

kern_return_t
vayu_dma_alloc(vm_size_t size, uint64_t max_pa, uint64_t align, vm_offset_t *va, uint64_t *pa)
{
	vm_offset_t addr = 0;
	vm_size_t bytes;
	ppnum_t max_pnum = 0, pnum_mask = 0;
	uint64_t first, last;
	kern_return_t kr;

	*va = 0;
	*pa = 0;
	if (size == 0 || size > (1ULL << 30)) {
		return KERN_INVALID_ARGUMENT;
	}
	bytes = round_page(size);
	if (align != 0) {
		if ((align & (align - 1)) != 0 || align < PAGE_SIZE || (align >> PAGE_SHIFT) > UINT32_MAX) {
			return KERN_INVALID_ARGUMENT;
		}
		pnum_mask = (ppnum_t)((align >> PAGE_SHIFT) - 1);
	}
	if (max_pa != 0) {
		if (max_pa < bytes || (max_pa >> PAGE_SHIFT) == 0 || (max_pa >> PAGE_SHIFT) - 1 > UINT32_MAX) {
			return KERN_INVALID_ARGUMENT;
		}
		max_pnum = (ppnum_t)((max_pa >> PAGE_SHIFT) - 1);    /* the last page number allowed */
	}
	kr = kmem_alloc_contig(kernel_map, &addr, bytes, PAGE_MASK, max_pnum, pnum_mask,
	    KMA_KOBJECT | KMA_DATA | KMA_ZERO | KMA_NOPAGEWAIT, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS) {
		return kr;
	}
	/* what the device is given is checked, not assumed: every page where it should be, below the limit */
	first = kvtophys(addr);
	if (first == 0 || (first & PAGE_MASK) != 0 || __builtin_add_overflow(first, (uint64_t)bytes - 1, &last) ||
	    (max_pa != 0 && last >= max_pa) || (align != 0 && (first & (align - 1)) != 0)) {
		kmem_free(kernel_map, addr, bytes);
		return KERN_FAILURE;
	}
	for (vm_size_t off = PAGE_SIZE; off < bytes; off += PAGE_SIZE) {
		if (kvtophys(addr + off) != first + off) {
			kmem_free(kernel_map, addr, bytes);
			return KERN_FAILURE;
		}
	}
	/* the zeroing's lines out of the cache before the device sees the memory */
	FlushPoC_DcacheRegion(addr, bytes);
	__asm__ volatile ("dsb sy" ::: "memory");
	*va = addr;
	*pa = first;
	return KERN_SUCCESS;
}

kern_return_t
vayu_dma_alloc_contig_below4g(vm_size_t size, vm_offset_t *va, uint64_t *pa)
{
	return vayu_dma_alloc(size, 1ULL << 32, 0, va, pa);
}

void
vayu_dma_free(vm_offset_t va, vm_size_t size)
{
	if (va != 0 && size != 0) {
		kmem_free(kernel_map, va, round_page(size));
	}
}

/* the CPU's writes in [va, va + len) to the point of coherency, and the lines out of the cache: the device may read */
bool
vayu_dma_sync_for_device(vm_offset_t va, vm_size_t len)
{
	vm_size_t line = vayu_dma_line();
	vm_offset_t start = va & ~(line - 1), end;

	if (len == 0 || __builtin_add_overflow(va, len, &end)) {
		return false;
	}
	__asm__ volatile ("dmb sy" ::: "memory");       /* the CPU's stores before the maintenance */
	FlushPoC_DcacheRegion(start, end - start);
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

/*
 * After the device has written [va, va + len): no stale line is left for the CPU to read. Clean+invalidate: a line the
 * CPU did not write since vayu_dma_sync_for_device() is clean, so it is only dropped. The range must be whole cache
 * lines: a partial line would be written back over what the device put in the rest of it (false, nothing done).
 */
bool
vayu_dma_sync_for_cpu(vm_offset_t va, vm_size_t len)
{
	vm_size_t line = vayu_dma_line();
	vm_offset_t end;

	if (len == 0 || ((va | len) & (line - 1)) != 0 || __builtin_add_overflow(va, len, &end)) {
		return false;
	}
	__asm__ volatile ("dsb sy" ::: "memory");       /* the device's completion seen (by the caller) before this */
	FlushPoC_DcacheRegion(va, len);
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

/* ---- vayu_mem_diag=1 ---- */

extern vm_map_address_t physmap_base, physmap_end;     /* arm_vm_init.c */
extern unsigned int vayu_pmap_alias_pages;              /* pmap.c: managed pages made non-WB, their WB alias kept */
void vayu_wcomb_report(void);
void vayu_mem_diag_run(void);

static const uint64_t vayu_diag_magic = 0x5641595544494147ULL;   /* "VAYUDIAG" */

/*
 * One buffer, both CPU aliases (the kernel map's and the physical aperture's, both WB): every page where kvtophys says
 * and WB in the pmap, a pattern written through one alias and read through the other, around the sync calls.
 */
static bool
vayu_diag_buffer(const char *what, vm_offset_t va, uint64_t pa, vm_size_t bytes)
{
	vm_offset_t alias = (vm_offset_t)phystokv(pa);
	bool ok = true, pages = true;
	unsigned int bad_attr = 0, bad_word = 0;

	for (vm_size_t off = 0; off < bytes; off += PAGE_SIZE) {
		ppnum_t pn = (ppnum_t)atop(pa + off);

		/* pp -> pa -> pp in 64 bits, and both aliases on the same page */
		if (ptoa((uint64_t)pn) != pa + off || kvtophys(va + off) != pa + off ||
		    phystokv(pa + off) != alias + off) {
			pages = false;
		}
		if ((pmap_cache_attributes(pn) & VM_WIMG_MASK) != VM_WIMG_DEFAULT) {
			bad_attr++;
		}
	}
	for (vm_size_t off = 0; off < bytes; off += sizeof(uint64_t)) {
		((volatile uint64_t *)va)[off / sizeof(uint64_t)] = (pa + off) ^ vayu_diag_magic;
	}
	(void)vayu_dma_sync_for_device(va, bytes);
	for (vm_size_t off = 0; off < bytes; off += sizeof(uint64_t)) {
		if (((volatile uint64_t *)alias)[off / sizeof(uint64_t)] != ((pa + off) ^ vayu_diag_magic)) {
			bad_word++;
		}
		((volatile uint64_t *)alias)[off / sizeof(uint64_t)] = ~((pa + off) ^ vayu_diag_magic);
	}
	(void)vayu_dma_sync_for_device(alias, bytes);
	if (!vayu_dma_sync_for_cpu(va, bytes)) {
		ok = false;
	}
	for (vm_size_t off = 0; off < bytes; off += sizeof(uint64_t)) {
		if (((volatile uint64_t *)va)[off / sizeof(uint64_t)] != ~((pa + off) ^ vayu_diag_magic)) {
			bad_word++;
		}
	}
	ok = ok && pages && bad_attr == 0 && bad_word == 0;
	printf("VAYU MEM: %s: %lu KiB at pa %#llx..%#llx, va %#lx, aperture %#lx: pages %s, %u non-WB page(s), %u bad "
	    "word(s): %s\n", what, (unsigned long)(bytes >> 10), pa, pa + bytes - 1, (unsigned long)va, (unsigned long)alias,
	    pages ? "where kvtophys says" : "NOT where kvtophys says", bad_attr, bad_word, ok ? "PASS" : "FAIL");
	return ok;
}

void
vayu_mem_diag_run(void)
{
	const uint64_t g4 = 1ULL << 32, win_end = gDramBase + gDramSize;
	const vm_size_t bytes = 64 * 1024;
	bool pass = true;
	vm_offset_t va = 0;
	uint64_t pa = 0;
	kern_return_t kr;

	printf("VAYU MEM: build: PAGE_SIZE %lu, __ARM_16K_PG__ 1, ARM_LARGE_MEMORY undefined, XNU_MONITOR 0, "
	    "__ARM_PTE_PHYSMAP__ 0, kernel VA [%#lx, %#lx], D-cache line %lu\n", (unsigned long)PAGE_SIZE,
	    (unsigned long)VM_MIN_KERNEL_ADDRESS, (unsigned long)VM_MAX_KERNEL_ADDRESS, (unsigned long)vayu_dma_line());
	printf("VAYU MEM: window (gDram) [%#llx, %#llx) %llu MiB; managed (gPhys) [%#lx, %#lx); mem_size %lu MiB, "
	    "max_mem %llu MiB\n", gDramBase, win_end, gDramSize >> 20, gPhysBase, gPhysBase + gPhysSize,
	    (unsigned long)(mem_size >> 20), max_mem >> 20);
	printf("VAYU MEM: physical aperture [%#lx, %#lx), gVirtBase %#lx; extra RAM: %u range(s), %u page(s)\n",
	    (unsigned long)physmap_base, (unsigned long)physmap_end, gVirtBase, hakios_extra_ram_count,
	    hakios_extra_ram_pages);
	for (unsigned int i = 0; i < hakios_extra_ram_count; i++) {
		printf("VAYU MEM: extra RAM [%#llx, %#llx)\n", hakios_extra_ram[i].pa, hakios_extra_ram[i].end);
	}
	if (gDramSize == 0 || PAGE_SIZE != 16384 || (gDramBase & PAGE_MASK) != 0) {
		pass = false;
	}
	vayu_wcomb_report();
	printf("VAYU MEM: %u managed page(s) made non-WB by pmap_set_cache_attributes() (their aperture alias stays WB)\n",
	    os_atomic_load(&vayu_pmap_alias_pages, relaxed));

	/* below 4 GiB, for 32-bit DMA masters (MDP, ...) */
	kr = vayu_dma_alloc_contig_below4g(bytes, &va, &pa);
	if (gDramBase < g4) {
		if (kr != KERN_SUCCESS) {
			printf("VAYU MEM: below 4 GiB: no %lu KiB (%d) although the window starts at %#llx: FAIL\n",
			    (unsigned long)(bytes >> 10), kr, gDramBase);
			pass = false;
		} else {
			pass = vayu_diag_buffer("below 4 GiB", va, pa, bytes) && pa + bytes <= g4 && pass;
			vayu_dma_free(va, bytes);
		}
	} else if (kr == KERN_SUCCESS) {
		printf("VAYU MEM: below 4 GiB: got pa %#llx from a window above 4 GiB: FAIL\n", pa);
		vayu_dma_free(va, bytes);
		pass = false;
	} else {
		printf("VAYU MEM: below 4 GiB: none (the window starts at %#llx): 32-bit DMA needs memory the loader names\n",
		    gDramBase);
	}

	/* at or above 4 GiB: aligned to 4 GiB, then 1 GiB, then 256 MiB, until a buffer lies above it */
	if (win_end > g4) {
		static const uint64_t aligns[] = { 1ULL << 32, 1ULL << 30, 1ULL << 28, 1ULL << 24 };
		vm_offset_t held[4] = {};
		bool found = false;

		for (unsigned int i = 0; i < sizeof(aligns) / sizeof(aligns[0]) && !found; i++) {
			kr = vayu_dma_alloc(bytes, 0, aligns[i], &va, &pa);
			if (kr != KERN_SUCCESS) {
				continue;
			}
			if (pa >= g4) {
				found = true;
				pass = vayu_diag_buffer("above 4 GiB", va, pa, bytes) && pass;
				vayu_dma_free(va, bytes);
			} else {
				held[i] = va;   /* kept until the end, so the next try lands elsewhere */
			}
		}
		for (unsigned int i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
			vayu_dma_free(held[i], bytes);
		}
		if (!found) {
			printf("VAYU MEM: above 4 GiB: no contiguous %lu KiB found there (the window ends at %#llx): FAIL\n",
			    (unsigned long)(bytes >> 10), win_end);
			pass = false;
		}
	} else {
		printf("VAYU MEM: above 4 GiB: the window ends at %#llx, nothing to test\n", win_end);
	}
	/* the contract's refusals */
	if (vayu_dma_sync_for_cpu(0x1000 + 8, 64) || vayu_dma_alloc(0, 0, 0, &va, &pa) == KERN_SUCCESS ||
	    vayu_dma_alloc(bytes, 0, 3 * PAGE_SIZE, &va, &pa) == KERN_SUCCESS) {
		printf("VAYU MEM: an unaligned sync or a bad allocation was not refused: FAIL\n");
		pass = false;
	}
	printf("VAYU MEM: diag %s\n", pass ? "PASS" : "FAIL");
}
