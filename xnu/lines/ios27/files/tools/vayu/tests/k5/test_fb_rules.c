/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * Host test of iokit/Kernel/arm/vayu_fb_rules.h (K5): the very header SweetFramebuffer.cpp includes. The phone's
 * numbers (1080x2400, rowbytes 4320 at 0x9c000000), QEMU's (800x600 of the GOP), and the edges: no framebuffer, a depth
 * other than 32, a row shorter than the width or not whole pixels, sides over 8192, rowbytes * height overflowing 64
 * bits or passing 256 MiB, base + size wrapping; the layout of the iPhone X size on the panel and the refusal of a
 * smaller one; every SwapEnd length that matters (0x3ff, 0x400, 0x468, 0x46c, 0x514, 0x6e0, 0x800, 0x801, 0x4000,
 * 0x4001, descriptors, both at once); the CRC against zlib's (known vectors computed with Python's zlib.crc32).
 */
#include <stdio.h>
#include <string.h>
#include "vayu_fb_rules.h"

static int failed, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
test_check(void)
{
	uint64_t bytes = 0;

	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 4320, 32, &bytes) == VAYU_FB_OK && bytes == 10368000, "phone");
	CHECK(vayu_fb_check(0x1bc7a0000ull, 800, 600, 3200, 32, &bytes) == VAYU_FB_OK && bytes == 1920000, "qemu");
	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 4320, 0, &bytes) == VAYU_FB_OK, "depth 0 taken as 32");
	CHECK(vayu_fb_check(0, 1080, 2400, 4320, 32, &bytes) == VAYU_FB_NONE, "no base");
	CHECK(vayu_fb_check(0x9c000000, 0, 2400, 4320, 32, &bytes) == VAYU_FB_NONE, "no width");
	CHECK(vayu_fb_check(0x9c000000, 1080, 0, 4320, 32, &bytes) == VAYU_FB_NONE, "no height");
	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 4320, 16, &bytes) == VAYU_FB_DEPTH, "16 bpp");
	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 4320, 24, &bytes) == VAYU_FB_DEPTH, "24 bpp");
	CHECK(vayu_fb_check(0x9c000000, 8193, 2400, 8193 * 4, 32, &bytes) == VAYU_FB_DIM, "width 8193");
	CHECK(vayu_fb_check(0x9c000000, 1080, 8193, 4320, 32, &bytes) == VAYU_FB_DIM, "height 8193");
	CHECK(vayu_fb_check(0x9c000000, 8192, 8192, 8192 * 4, 32, &bytes) == VAYU_FB_OK && bytes == 256ull << 20,
	    "8192^2 = 256 MiB exactly");
	CHECK(vayu_fb_check(0x9c000000, 8192, 8192, 8192 * 4 + 4, 32, &bytes) == VAYU_FB_SIZE, "past 256 MiB");
	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 4319, 32, &bytes) == VAYU_FB_ROWBYTES, "row 1 byte short");
	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 4322, 32, &bytes) == VAYU_FB_ROWBYTES, "row not whole pixels");
	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 4352, 32, &bytes) == VAYU_FB_OK && bytes == 4352ull * 2400, "padded row");
	CHECK(vayu_fb_check(0x9c000000, 1080, 2400, 1ull << 62, 32, &bytes) == VAYU_FB_SIZE, "rowbytes * height overflows");
	CHECK(vayu_fb_check(0xffffffffffff0000ull, 1080, 2400, 4320, 32, &bytes) == VAYU_FB_WRAPS, "base + size wraps");
	CHECK(vayu_fb_check(0xffffffffffffffffull - 10368000, 1080, 2400, 4320, 32, &bytes) == VAYU_FB_OK,
	    "ends at 2^64 - 1");
	CHECK(vayu_fb_check(0x9c000000, 1ull << 33, 2400, 4320, 32, &bytes) == VAYU_FB_DIM, "a 64-bit width");
	CHECK(strcmp(vayu_fb_check_name(VAYU_FB_SIZE), "?") != 0, "names");
}

static void
test_layout(void)
{
	struct vayu_fb_layout l;

	vayu_fb_layout(1080, 2400, 1125, 2436, &l);
	CHECK(l.lw == 1125 && l.lh == 2436 && l.offx == 22 && l.offy == 18 && !l.native, "phone as iPhone X: %u %u %d %d",
	    l.lw, l.lh, l.offx, l.offy);
	vayu_fb_layout(1080, 2400, 1080, 2400, &l);
	CHECK(l.lw == 1080 && l.lh == 2400 && l.offx == 0 && l.offy == 0 && l.native, "native");
	vayu_fb_layout(800, 600, 1125, 2436, &l);
	CHECK(l.lw == 1125 && l.lh == 2436 && l.offx == 162 && l.offy == 918, "qemu 800x600");
	vayu_fb_layout(1080, 2400, 828, 1792, &l);
	CHECK(l.lw == 1080 && l.lh == 2400 && l.native, "smaller than the panel (iPhone 11's 828x1792): native");
	vayu_fb_layout(1080, 2400, 1125, 2000, &l);
	CHECK(l.native, "one side smaller: native");
	vayu_fb_layout(1080, 2400, 9000, 9000, &l);
	CHECK(l.native, "larger than 8192: native");
	vayu_fb_layout(1080, 2400, 1081, 2401, &l);
	CHECK(l.offx == 0 && l.offy == 0 && !l.native, "odd difference rounds down");
}

static void
test_swap(void)
{
	static const struct {
		uint64_t inband, desc;
		bool has_desc;
		enum vayu_fb_swap_result want;
		uint32_t take;
	} t[] = {
		{ 0x3ff, 0, false, VAYU_FB_SWAP_SHORT, 0 },
		{ 0, 0, false, VAYU_FB_SWAP_SHORT, 0 },
		{ 0x400, 0, false, VAYU_FB_SWAP_OK, 0x400 },
		{ 0x468, 0, false, VAYU_FB_SWAP_OK, 0x468 },    /* macOS 13 */
		{ 0x46c, 0, false, VAYU_FB_SWAP_OK, 0x46c },    /* iOS 16 */
		{ 0x514, 0, false, VAYU_FB_SWAP_OK, 0x514 },    /* iPadOS 18 */
		{ 0x6e0, 0, false, VAYU_FB_SWAP_OK, 0x6e0 },    /* iOS 27: whole */
		{ 0x800, 0, false, VAYU_FB_SWAP_OK, 0x800 },
		{ 0x801, 0, false, VAYU_FB_SWAP_OK, 0x800 },
		{ 0x1000, 0, false, VAYU_FB_SWAP_OK, 0x800 },
		{ 0x4000, 0, false, VAYU_FB_SWAP_OK, 0x800 },
		{ 0x4001, 0, false, VAYU_FB_SWAP_LONG, 0 },
		{ 0xffffffff, 0, false, VAYU_FB_SWAP_LONG, 0 },
		{ 0, 0x6e0, true, VAYU_FB_SWAP_OK, 0x6e0 },
		{ 0, 0x3ff, true, VAYU_FB_SWAP_SHORT, 0 },
		{ 0, 0x2000, true, VAYU_FB_SWAP_OK, 0x800 },
		{ 0, 1ull << 40, true, VAYU_FB_SWAP_LONG, 0 },
		{ 0x6e0, 0x6e0, true, VAYU_FB_SWAP_BOTH, 0 },
		{ 0x1, 0, true, VAYU_FB_SWAP_BOTH, 0 },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		uint32_t take = 0xdead;
		enum vayu_fb_swap_result r = vayu_fb_swap_len(t[i].inband, t[i].has_desc, t[i].desc, &take);
		CHECK(r == t[i].want && (r != VAYU_FB_SWAP_OK || take == t[i].take) && (r == VAYU_FB_SWAP_OK || take == 0xdead),
		    "swap %u: inband %#llx desc %d %#llx -> %d take %#x", i, (unsigned long long)t[i].inband, t[i].has_desc,
		    (unsigned long long)t[i].desc, r, take);
	}
}

static void
test_crc(void)
{
	uint32_t tab[256], lit = 0, white = 0, c;
	static const uint32_t px[4] = { 0x00112233, 0xffffffff, 0x00000000, 0x80abcdef };

	vayu_fb_crc_table(tab);
	c = VAYU_FB_CRC_INIT;
	for (const char *p = "123456789"; *p; p++) {
		c = VAYU_FB_CRC_BYTE(tab, c, *p);
	}
	CHECK(VAYU_FB_CRC_END(c) == 0xcbf43926u, "zlib check value %08x", VAYU_FB_CRC_END(c));
	c = vayu_fb_crc_rgb_row(tab, VAYU_FB_CRC_INIT, px, 4, &lit, &white);
	CHECK(VAYU_FB_CRC_END(c) == 0xd7a2cc27u && lit == 3 && white == 1, "rgb row %08x lit %u white %u",
	    VAYU_FB_CRC_END(c), lit, white);
	/* two rows of two = one row of four (row-major) */
	lit = white = 0;
	c = vayu_fb_crc_rgb_row(tab, VAYU_FB_CRC_INIT, px, 2, &lit, &white);
	c = vayu_fb_crc_rgb_row(tab, c, px + 2, 2, &lit, &white);
	CHECK(VAYU_FB_CRC_END(c) == 0xd7a2cc27u && lit == 3, "two rows");
}

static void
test_brightness(void)
{
	static const struct {
		uint64_t bits;
		uint32_t want;
	} t[] = {
		{ 0x0, 1 },                             /* 0.0: the lowest level, not off */
		{ 0x8000000000000000ull, 1 },           /* -0.0 */
		{ 0x3fe0000000000000ull, 500 },         /* 0.5 */
		{ 0x3ff0000000000000ull, 1000 },        /* 1.0 */
		{ 0x3f40624dd2f1a9fcull, 1 },           /* 0.0005 */
		{ 0x406f400000000000ull, 500 },         /* 250 nits of 500 */
		{ 0x407f400000000000ull, 1000 },        /* 500 nits */
		{ 0x408f400000000000ull, 1000 },        /* 1000 nits: clamped */
		{ 0x4000000000000000ull, 4 },           /* 2.0: 2 nits */
		{ 0x3e112e0be826d695ull, 1 },           /* 1e-9 */
		{ 0x46293e5939a08ceaull, 1000 },        /* 1e30 */
		{ 0xbff0000000000000ull, 0 },           /* -1.0 */
		{ 0x7ff8000000000000ull, 0 },           /* NaN */
		{ 0x7ff0000000000000ull, 0 },           /* inf */
		{ 0x0000000000000001ull, 1 },           /* subnormal */
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		uint32_t got = vayu_fb_brightness_permille(t[i].bits, 500);
		CHECK(got == t[i].want, "brightness %#llx: %u, want %u", (unsigned long long)t[i].bits, got, t[i].want);
	}
	CHECK(vayu_fb_brightness_permille(0x406f400000000000ull, 0) == 0, "nits without a scale");
}

int
main(void)
{
	test_check();
	test_layout();
	test_swap();
	test_crc();
	test_brightness();
	printf("test_fb_rules: %d/%d checks passed%s\n", checks - failed, checks, failed ? ": FAILED" : "");
	return failed != 0;
}
