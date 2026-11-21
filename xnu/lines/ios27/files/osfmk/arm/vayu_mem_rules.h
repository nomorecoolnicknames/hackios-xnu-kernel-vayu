/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (K3): the arithmetic of the kernel's memory rules, without the kernel. Each function here is pure (integers
 * in, integers out, no global state, no kernel header) so that the very code osfmk/arm/io_map.c and
 * osfmk/arm/pmap/pmap.c run is what tools/vayu/tests/k3 runs on a host: the tests do not copy it.
 *
 * Every size is checked before it is added: an address near the top of the 64-bit space is refused, not wrapped.
 * pmask is PAGE_MASK (0x3fff on vayu, 16 KiB pages).
 */
#ifndef _ARM_VAYU_MEM_RULES_H_
#define _ARM_VAYU_MEM_RULES_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * io_map(): what one mapping of [phys, phys + size) takes.
 *
 * The mapping starts at the page phys is in and covers every page the bytes touch; the caller gets the page's address
 * plus the offset. The early path (before kmem) takes the kernel address space from a cursor, virtual_space_start, and
 * must move it by the whole mapped size: xnu moved it by round_page(size), one page short whenever the offset carries
 * the end into one more page (offset 0x3fff, 2 bytes: two pages mapped, one taken), so the next early mapping lay over
 * the last page of this one (SPEC-K3, "io_map, ptoa и 16K").
 */
enum vayu_io_map_result {
	VAYU_IO_MAP_OK = 0,
	VAYU_IO_MAP_EMPTY,              /* size 0: nothing to map */
	VAYU_IO_MAP_SIZE_WRAPS,         /* size + offset, or its rounding up to a page, passes 2^64 */
	VAYU_IO_MAP_PA_WRAPS,           /* the mapping's last physical byte passes 2^64 */
	VAYU_IO_MAP_VA_WRAPS,           /* the cursor + the mapping passes 2^64 or the given limit */
};

struct vayu_io_span {
	uint64_t pa;            /* the first page's physical address */
	uint64_t offset;        /* phys - pa: what the caller's address is past the mapping's start */
	uint64_t bytes;         /* what is mapped (whole pages) */
};

static inline enum vayu_io_map_result
vayu_io_map_span(uint64_t phys, uint64_t size, uint64_t pmask, struct vayu_io_span *span)
{
	uint64_t offset = phys & pmask, need, bytes;

	if (size == 0) {
		return VAYU_IO_MAP_EMPTY;
	}
	if (__builtin_add_overflow(size, offset, &need) || __builtin_add_overflow(need, pmask, &bytes)) {
		return VAYU_IO_MAP_SIZE_WRAPS;
	}
	bytes &= ~pmask;
	/* the last byte mapped is pa + bytes - 1 */
	if ((phys - offset) + (bytes - 1) < (phys - offset)) {
		return VAYU_IO_MAP_PA_WRAPS;
	}
	span->pa = phys - offset;
	span->offset = offset;
	span->bytes = bytes;
	return VAYU_IO_MAP_OK;
}

/*
 * The early cursor: the mapping starts at the cursor rounded up to a page and the cursor moves past all of it. limit is
 * the first address the cursor may not reach past (0: only 2^64). On a refusal *start and *next are not written, so a
 * caller that stops there has moved nothing.
 */
static inline enum vayu_io_map_result
vayu_io_map_take_va(uint64_t cursor, uint64_t bytes, uint64_t pmask, uint64_t limit, uint64_t *start,
    uint64_t *next)
{
	uint64_t s, e;

	if (__builtin_add_overflow(cursor, pmask, &s)) {
		return VAYU_IO_MAP_VA_WRAPS;
	}
	s &= ~pmask;
	if (__builtin_add_overflow(s, bytes, &e) || (limit != 0 && e > limit)) {
		return VAYU_IO_MAP_VA_WRAPS;
	}
	*start = s;
	*next = e;
	return VAYU_IO_MAP_OK;
}

/*
 * Which physical pages may be Normal Non-Cacheable (WCOMB, RT) rather than Device (K3 commit 2, vayu_wcomb_safe=1).
 *
 * Scion's SWEET layer makes every WCOMB/RT mapping Normal NC, whatever the address (pmap.c pmap_wcomb_normal()): the
 * Exynos boards' framebuffer and panic region are RAM outside the kernel's window, and as Device memory bzero's DC ZVA
 * takes an alignment fault there. On vayu that also makes a register block mapped WCOMB by mistake Normal memory, open
 * to speculative reads. With the rule on, a page is Normal NC only when it lies wholly in one of:
 *
 *   - the window the loader gave the kernel (gDramBase, gDramSize: the claimed UEFI block, not all of the DRAM);
 *   - an extra RAM range the kernel took (hakios_extra_ram[], extra_ram=1; none unless the loader names them);
 *   - a loader hand-off range: RAM outside the window that the loader itself names to the kernel for a purpose --
 *     the boot framebuffer (boot_args.Video, /sweet-fbcon) and the panic log (/pram) -- rounded out to whole pages,
 *     each at most VAYU_HANDOFF_MAX_BYTES.
 *
 * Anything else (MMIO such as 0x0c3f0000, the splash region at 0x9c000000 when no loader record names it, the UFS
 * mailbox) is Device, as xnu-12377 maps it on every other board.
 */
#define VAYU_HANDOFF_MAX        4
#define VAYU_HANDOFF_MAX_BYTES  (64ull << 20)

struct vayu_pa_range {
	uint64_t pa;            /* [pa, end): whole pages, end > pa */
	uint64_t end;
};

struct vayu_mem_map {
	uint64_t window_base, window_size;
	const struct vayu_pa_range *extra;
	unsigned extra_count;
	const struct vayu_pa_range *handoff;
	unsigned handoff_count;
};

enum vayu_mem_class {
	VAYU_MEM_DEVICE = 0,    /* none of the below: Device */
	VAYU_MEM_WINDOW,
	VAYU_MEM_EXTRA,
	VAYU_MEM_HANDOFF,
};

/* is all of [pa, pa + page) inside [base, base + size)? (no sum is formed that could wrap) */
static inline bool
vayu_page_inside(uint64_t pa, uint64_t page, uint64_t base, uint64_t size)
{
	uint64_t off;

	if (pa < base) {
		return false;
	}
	off = pa - base;
	return off < size && size - off >= page;
}

static inline bool
vayu_page_in_ranges(uint64_t pa, uint64_t page, const struct vayu_pa_range *r, unsigned n)
{
	for (unsigned i = 0; r != 0 && i < n; i++) {
		if (r[i].end > r[i].pa && vayu_page_inside(pa, page, r[i].pa, r[i].end - r[i].pa)) {
			return true;
		}
	}
	return false;
}

/* the class of the page at pa (pa is rounded down to its page first) */
static inline enum vayu_mem_class
vayu_mem_page_class(uint64_t pa, uint64_t pmask, const struct vayu_mem_map *m)
{
	uint64_t page = pmask + 1;

	pa &= ~pmask;
	if (vayu_page_inside(pa, page, m->window_base, m->window_size)) {
		return VAYU_MEM_WINDOW;
	}
	if (vayu_page_in_ranges(pa, page, m->extra, m->extra_count)) {
		return VAYU_MEM_EXTRA;
	}
	if (vayu_page_in_ranges(pa, page, m->handoff, m->handoff_count)) {
		return VAYU_MEM_HANDOFF;
	}
	return VAYU_MEM_DEVICE;
}

/*
 * A mapping of [start, end) (whole pages): Normal NC only when every page is in a class above; one page that is not
 * makes the whole mapping Device. An empty, unaligned or wrapping range is refused. *first_bad: the first page refused.
 */
static inline bool
vayu_wcomb_normal_range(uint64_t start, uint64_t end, uint64_t pmask, const struct vayu_mem_map *m,
    uint64_t *first_bad)
{
	uint64_t page = pmask + 1;

	if (end <= start || (start & pmask) != 0 || (end & pmask) != 0) {
		*first_bad = start;
		return false;
	}
	for (uint64_t pa = start; pa < end; pa += page) {
		if (vayu_mem_page_class(pa, pmask, m) == VAYU_MEM_DEVICE) {
			*first_bad = pa;
			return false;
		}
		if (pa + page < pa) {
			break;
		}
	}
	return true;
}

/*
 * A loader hand-off record [base, base + len) as a range of whole pages (rounded out: what io_map maps of it). False:
 * empty, wrapping, or longer than max -- then it is not taken.
 */
static inline bool
vayu_handoff_range(uint64_t base, uint64_t len, uint64_t pmask, uint64_t max, struct vayu_pa_range *out)
{
	uint64_t last, end;

	if (len == 0 || len > max || __builtin_add_overflow(base, len - 1, &last) ||
	    __builtin_add_overflow(last | pmask, 1, &end) || end == 0) {
		return false;
	}
	out->pa = base & ~pmask;
	out->end = end;
	return true;
}

#endif /* _ARM_VAYU_MEM_RULES_H_ */
