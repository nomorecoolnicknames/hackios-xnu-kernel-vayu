/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * Host test of osfmk/arm/vayu_mem_rules.h, the WCOMB/RT part (K3 commit 2, vayu_wcomb_safe=1): which pages may be
 * Normal NC. The header the kernel compiles, included as it is. The addresses are vayu's (SPEC-K3): window examples,
 * the MMIO at 0x0c3f0000, the splash region at 0x9c000000, the loader's panic log at 0xf3f00000.
 */
#include <inttypes.h>
#include <stdio.h>
#include "arm/vayu_mem_rules.h"

#define PAGE    0x4000ull
#define MASK    (PAGE - 1)
#define G4      0x100000000ull

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *
name(enum vayu_mem_class c)
{
	return c == VAYU_MEM_WINDOW ? "window" : c == VAYU_MEM_EXTRA ? "extra" : c == VAYU_MEM_HANDOFF ? "handoff" :
	       "Device";
}

static void
page(const struct vayu_mem_map *m, uint64_t pa, enum vayu_mem_class want)
{
	enum vayu_mem_class c = vayu_mem_page_class(pa, MASK, m);

	CHECK(c == want, "page %#" PRIx64 ": %s, want %s", pa, name(c), name(want));
}

static void
range(const struct vayu_mem_map *m, uint64_t start, uint64_t end, int want, uint64_t want_bad)
{
	uint64_t bad = 0x1234;
	bool ok = vayu_wcomb_normal_range(start, end, MASK, m, &bad);

	CHECK(ok == (want != 0), "range [%#" PRIx64 ", %#" PRIx64 "): %s, want %s", start, end, ok ? "Normal NC" :
	    "Device", want ? "Normal NC" : "Device");
	if (!want && !ok) {
		CHECK(bad == want_bad, "range [%#" PRIx64 ", %#" PRIx64 "): first refused %#" PRIx64 ", want %#" PRIx64,
		    start, end, bad, want_bad);
	}
}

int
main(void)
{
	/* the boot framebuffer, 1080x2400 XRGB8888 at the splash region (boot_args.Video): rounded out to 16K */
	struct vayu_pa_range fb, pram, r;
	const uint64_t fb_bytes = 1080ull * 2400 * 4;   /* 0x9e3400 */

	CHECK(vayu_handoff_range(0x9c000000, fb_bytes, MASK, VAYU_HANDOFF_MAX_BYTES, &fb) && fb.pa == 0x9c000000 &&
	    fb.end == 0x9c9e4000, "fb hand-off [%#" PRIx64 ", %#" PRIx64 ")", fb.pa, fb.end);
	CHECK(vayu_handoff_range(0xf3f00000, 0x80000, MASK, VAYU_HANDOFF_MAX_BYTES, &pram) && pram.pa == 0xf3f00000 &&
	    pram.end == 0xf3f80000, "pram hand-off");
	CHECK(vayu_handoff_range(0x9c001000, 0x100, MASK, VAYU_HANDOFF_MAX_BYTES, &r) && r.pa == 0x9c000000 &&
	    r.end == 0x9c004000, "unaligned record rounds out");
	CHECK(vayu_handoff_range(0x9c003fff, 2, MASK, VAYU_HANDOFF_MAX_BYTES, &r) && r.pa == 0x9c000000 &&
	    r.end == 0x9c008000, "a record across a page boundary takes both pages");
	CHECK(!vayu_handoff_range(0x9c000000, 0, MASK, VAYU_HANDOFF_MAX_BYTES, &r), "empty record refused");
	CHECK(!vayu_handoff_range(0x9c000000, VAYU_HANDOFF_MAX_BYTES + 1, MASK, VAYU_HANDOFF_MAX_BYTES, &r),
	    "record over the cap refused");
	CHECK(!vayu_handoff_range(UINT64_MAX - 0x1000, 0x2000, MASK, VAYU_HANDOFF_MAX_BYTES, &r), "wrapping record");
	CHECK(!vayu_handoff_range(UINT64_MAX - 0x3fff, 0x4000, MASK, VAYU_HANDOFF_MAX_BYTES, &r),
	    "the last page of 2^64: its end does not fit, refused");

	/*
	 * A: a 6 GiB window across 4 GiB (mkadt_vayu.py's example, 0xc0000000 + 0x180000000), one extra range below it,
	 * the framebuffer and the panic log handed off (the panic log then lies inside the window: class window).
	 */
	struct vayu_pa_range extra_a[] = { { 0x80000000, 0x85700000 } };
	struct vayu_pa_range handoff_a[] = { fb, pram };
	struct vayu_mem_map a = { 0xc0000000, 0x180000000, extra_a, 1, handoff_a, 2 };

	page(&a, 0xc0000000 - PAGE, VAYU_MEM_DEVICE);
	page(&a, 0xc0000000, VAYU_MEM_WINDOW);
	page(&a, 0xc0000000 + 0x1234, VAYU_MEM_WINDOW);
	page(&a, G4 - PAGE, VAYU_MEM_WINDOW);
	page(&a, G4, VAYU_MEM_WINDOW);
	page(&a, G4 + PAGE, VAYU_MEM_WINDOW);
	page(&a, 0x240000000 - PAGE, VAYU_MEM_WINDOW);
	page(&a, 0x240000000, VAYU_MEM_DEVICE);
	page(&a, 0x280000000 - PAGE, VAYU_MEM_DEVICE);         /* DRAM, but not given to the kernel */
	page(&a, 0x80000000 - PAGE, VAYU_MEM_DEVICE);
	page(&a, 0x80000000, VAYU_MEM_EXTRA);
	page(&a, 0x85700000 - PAGE, VAYU_MEM_EXTRA);
	page(&a, 0x85700000, VAYU_MEM_DEVICE);                 /* hyp, reserved */
	page(&a, 0x0c3f0000, VAYU_MEM_DEVICE);                 /* MMIO: Device */
	page(&a, 0x0c3f0000 + 0x3ffc, VAYU_MEM_DEVICE);
	page(&a, 0x17c00000, VAYU_MEM_DEVICE);                 /* GIC */
	page(&a, 0x9c000000, VAYU_MEM_HANDOFF);
	page(&a, 0x9c9e0000, VAYU_MEM_HANDOFF);                /* the framebuffer's last (partial) page */
	page(&a, 0x9c9e4000, VAYU_MEM_DEVICE);                 /* the rest of splash: not named */
	page(&a, 0x9e400000, VAYU_MEM_DEVICE);                 /* qseecom */
	page(&a, 0xf3f00000, VAYU_MEM_WINDOW);
	page(&a, 0, VAYU_MEM_DEVICE);
	page(&a, UINT64_MAX, VAYU_MEM_DEVICE);

	range(&a, 0x9c000000, 0x9c9e4000, 1, 0);               /* the framebuffer as io_map maps it */
	range(&a, 0x9c000000, 0x9c9e8000, 0, 0x9c9e4000);      /* one page more: Device, all of it */
	range(&a, 0x9bffc000, 0x9c004000, 0, 0x9bffc000);
	range(&a, G4 - PAGE, G4 + PAGE, 1, 0);                 /* across 4 GiB inside the window */
	range(&a, 0x240000000 - PAGE, 0x240000000 + PAGE, 0, 0x240000000);
	range(&a, 0xc0000000 - PAGE, 0xc0000000 + PAGE, 0, 0xc0000000 - PAGE);
	range(&a, 0x0c3f0000, 0x0c3f4000, 0, 0x0c3f0000);
	range(&a, 0x9c000000, 0x9c000000, 0, 0x9c000000);      /* empty */
	range(&a, 0x9c000000, 0x9bffc000, 0, 0x9c000000);      /* end before start */
	range(&a, 0x9c001000, 0x9c004000, 0, 0x9c001000);      /* unaligned */
	range(&a, 0x9c000000, 0x9c002000, 0, 0x9c000000);
	range(&a, 0xc0000000, 0x240000000, 1, 0);              /* the whole window */

	/*
	 * B: QEMU's window (K1 qemu-run3: 0xf4000000 + 0xc8000000), no extra RAM, no framebuffer; the panic log below
	 * the window is RAM the loader names: hand-off.
	 */
	struct vayu_pa_range handoff_b[] = { pram };
	struct vayu_mem_map b = { 0xf4000000, 0xc8000000, NULL, 0, handoff_b, 1 };

	page(&b, 0xf3f00000, VAYU_MEM_HANDOFF);
	page(&b, 0xf3f7c000, VAYU_MEM_HANDOFF);
	page(&b, 0xf3f80000, VAYU_MEM_DEVICE);                 /* the rest of the loader's payload */
	page(&b, 0xf4000000, VAYU_MEM_WINDOW);
	page(&b, 0x1bc000000 - PAGE, VAYU_MEM_WINDOW);
	page(&b, 0x1bc000000, VAYU_MEM_DEVICE);
	page(&b, 0x9c000000, VAYU_MEM_DEVICE);                 /* splash with no record naming it */
	page(&b, 0x80000000, VAYU_MEM_DEVICE);                 /* no extra RAM: nothing below the window */
	range(&b, 0xf3f00000, 0xf3f80000, 1, 0);
	range(&b, 0xf3f7c000, 0xf4004000, 0, 0xf3f80000);      /* hand-off, a gap, the window: Device */

	/* C: degenerate maps never wrap: a window at the top of 2^64, empty entries ignored */
	struct vayu_pa_range junk[] = { { 0x5000, 0x5000 }, { 0x8000, 0x4000 } };
	struct vayu_mem_map c = { UINT64_MAX - 0x7fff, 0x8000, junk, 2, NULL, 0 };

	page(&c, UINT64_MAX - 0x7fff, VAYU_MEM_WINDOW);
	page(&c, UINT64_MAX - 0x3fff, VAYU_MEM_WINDOW);
	page(&c, 0x4000, VAYU_MEM_DEVICE);
	page(&c, 0x0, VAYU_MEM_DEVICE);
	struct vayu_mem_map d = { 0xc0000000, UINT64_MAX, NULL, 0, NULL, 0 };     /* a size past 2^64 */
	page(&d, 0xbfffc000, VAYU_MEM_DEVICE);
	page(&d, UINT64_MAX - 0x3fff, VAYU_MEM_WINDOW);
	struct vayu_mem_map e = { 0xc0000000, 0x2000, NULL, 0, NULL, 0 };         /* window smaller than a page */
	page(&e, 0xc0000000, VAYU_MEM_DEVICE);

	printf("test_wcomb: %d checks, %d failures: %s\n", checks, failures, failures ? "FAIL" : "PASS");
	return failures != 0;
}
