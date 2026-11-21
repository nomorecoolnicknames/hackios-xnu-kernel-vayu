/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * Host test of osfmk/arm/vayu_mem_rules.h, io_map's part (K3 commit 1): the header the kernel compiles, included as
 * it is. Run: tools/vayu/tests/k3/run.sh (cc on any host, 64-bit).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "arm/vayu_mem_rules.h"

#define PAGE    0x4000ull
#define MASK    (PAGE - 1)

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
span_case(uint64_t phys, uint64_t size, enum vayu_io_map_result want, uint64_t pa, uint64_t offset, uint64_t bytes)
{
	struct vayu_io_span s = { 0xdead, 0xdead, 0xdead };
	enum vayu_io_map_result r = vayu_io_map_span(phys, size, MASK, &s);

	CHECK(r == want, "span(%#" PRIx64 ", %#" PRIx64 ") = %d, want %d", phys, size, r, want);
	if (want == VAYU_IO_MAP_OK && r == VAYU_IO_MAP_OK) {
		CHECK(s.pa == pa && s.offset == offset && s.bytes == bytes,
		    "span(%#" PRIx64 ", %#" PRIx64 ") = pa %#" PRIx64 " off %#" PRIx64 " bytes %#" PRIx64 ", want %#" PRIx64
		    " %#" PRIx64 " %#" PRIx64, phys, size, s.pa, s.offset, s.bytes, pa, offset, bytes);
		/* the caller's bytes are inside what is mapped */
		CHECK(s.pa + s.offset == phys && s.offset + size <= s.bytes, "caller's range not inside the mapping");
	} else {
		CHECK(s.pa == 0xdead && s.bytes == 0xdead, "span written on refusal");
	}
}

/* the early path: map the cases one after the other from one cursor; every pair must be disjoint */
static void
cursor_sequence(void)
{
	static const struct { uint64_t phys, size; } maps[] = {
		{ 0x0c3f3fff, 2 }, { 0x1000, 0x3001 }, { 0x9c000000, 1080ull * 2400 * 4 }, { 0x88e0000, 0x1000 },
		{ 0xf3f00000, 0x80000 }, { 0x17c00000 + 0x3ffc, 8 },
	};
	uint64_t cursor = 0xfffffff007654321ull, limit = 0xfffffffbffffffffull;
	uint64_t lo[8], hi[8];
	unsigned n = 0;

	for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
		struct vayu_io_span s;
		uint64_t start, next;

		CHECK(vayu_io_map_span(maps[i].phys, maps[i].size, MASK, &s) == VAYU_IO_MAP_OK, "span %u", i);
		CHECK(vayu_io_map_take_va(cursor, s.bytes, MASK, limit, &start, &next) == VAYU_IO_MAP_OK, "va %u", i);
		CHECK((start & MASK) == 0 && start >= cursor && next == start + s.bytes, "va %u layout", i);
		lo[n] = start;
		hi[n] = next;
		n++;
		cursor = next;
	}
	for (unsigned i = 0; i < n; i++) {
		for (unsigned j = i + 1; j < n; j++) {
			CHECK(hi[i] <= lo[j] || hi[j] <= lo[i], "early mappings %u and %u overlap", i, j);
		}
	}

	/* xnu's old cursor step, round_page(size), on the same first case: shows the overlap the fix removes */
	struct vayu_io_span s;
	(void)vayu_io_map_span(0x0c3f3fff, 2, MASK, &s);
	uint64_t old_step = (2 + MASK) & ~MASK;
	CHECK(s.bytes == 2 * PAGE && old_step == PAGE, "the legacy step (one page) is shorter than the mapping (two)");
}

static void
refusals(void)
{
	uint64_t start = 1, next = 2;

	/* a refusal leaves the cursor as it was (nothing written) */
	CHECK(vayu_io_map_take_va(UINT64_MAX - 10, PAGE, MASK, 0, &start, &next) == VAYU_IO_MAP_VA_WRAPS &&
	    start == 1 && next == 2, "cursor near 2^64");
	CHECK(vayu_io_map_take_va(0xffffffffffff0000ull, 0x20000, MASK, 0, &start, &next) == VAYU_IO_MAP_VA_WRAPS,
	    "cursor + bytes wraps");
	CHECK(vayu_io_map_take_va(0x1000, 0x8000, MASK, 0x8000, &start, &next) == VAYU_IO_MAP_VA_WRAPS,
	    "past the limit");
	CHECK(vayu_io_map_take_va(0x1000, 0x4000, MASK, 0x8000, &start, &next) == VAYU_IO_MAP_OK && start == 0x4000 &&
	    next == 0x8000, "up to the limit exactly");
	CHECK(vayu_io_map_take_va(0, 0x4000, MASK, 0, &start, &next) == VAYU_IO_MAP_OK && start == 0 && next == 0x4000,
	    "cursor 0");
}

int
main(void)
{
	span_case(0, 0, VAYU_IO_MAP_EMPTY, 0, 0, 0);
	span_case(0x3fff, 0, VAYU_IO_MAP_EMPTY, 0, 0, 0);
	span_case(0, 0x4000, VAYU_IO_MAP_OK, 0, 0, 0x4000);
	span_case(0x3fff, 2, VAYU_IO_MAP_OK, 0, 0x3fff, 0x8000);
	span_case(0x1000, 0x3000, VAYU_IO_MAP_OK, 0, 0x1000, 0x4000);
	span_case(0x1000, 0x3001, VAYU_IO_MAP_OK, 0, 0x1000, 0x8000);
	span_case(0x100000000ull - 0x4000, 0x4000, VAYU_IO_MAP_OK, 0xffffc000ull, 0, 0x4000);     /* 4G - 16K */
	span_case(0x100000000ull - 1, 2, VAYU_IO_MAP_OK, 0xffffc000ull, 0x3fff, 0x8000);           /* across 4G */
	span_case(0x100000000ull, 1, VAYU_IO_MAP_OK, 0x100000000ull, 0, 0x4000);                  /* 4G */
	span_case(0x100004000ull, 0x4000, VAYU_IO_MAP_OK, 0x100004000ull, 0, 0x4000);             /* 4G + 16K */
	span_case(0, UINT64_MAX, VAYU_IO_MAP_SIZE_WRAPS, 0, 0, 0);
	span_case(0x3fff, UINT64_MAX - 0x3ffe, VAYU_IO_MAP_SIZE_WRAPS, 0, 0, 0);
	span_case(0, UINT64_MAX - 0x3ffe, VAYU_IO_MAP_SIZE_WRAPS, 0, 0, 0);       /* rounding up passes 2^64 */
	span_case(UINT64_MAX, 1, VAYU_IO_MAP_OK, UINT64_MAX & ~MASK, MASK, 0x4000); /* the very last page: fits */
	span_case(UINT64_MAX, 2, VAYU_IO_MAP_PA_WRAPS, 0, 0, 0);
	span_case(UINT64_MAX - 0x7fff, 0x8001, VAYU_IO_MAP_PA_WRAPS, 0, 0, 0);
	cursor_sequence();
	refusals();
	printf("test_io_map: %d checks, %d failures: %s\n", checks, failures, failures ? "FAIL" : "PASS");
	return failures != 0;
}
