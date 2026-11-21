/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (K5): the arithmetic of the display, without the kernel. New work of this tree. Every function here is pure
 * (integers and caller-given memory in, integers out, no global state, no kernel header), so the very code
 * SweetFramebuffer.cpp runs is what tools/vayu/tests/k5 runs on a host.
 *
 *   vayu_fb_check()        is the framebuffer the loader handed over (boot_args.Video) one the kernel may draw into?
 *   vayu_fb_layout()       where the presented ("logical") display lies on the panel
 *   vayu_fb_swap_len()     how much of a SwapEnd record is taken, from the inband bytes or a descriptor
 *   vayu_fb_crc_*()        the CRC-32 (IEEE, zlib's) of a picture's R, G, B bytes, row-major: what the kernel logs
 *                          for each frame and what a host computes from a saved picture
 */
#ifndef _VAYU_FB_RULES_H_
#define _VAYU_FB_RULES_H_

#include <stdbool.h>
#include <stdint.h>

/* ---- the boot framebuffer ------------------------------------------------------------------------------------ */

#define VAYU_FB_MAX_DIM         8192u                   /* a side longer than this is not a panel */
#define VAYU_FB_MAX_BYTES       (256ull << 20)          /* nor a framebuffer larger than 256 MiB */

enum vayu_fb_check_result {
	VAYU_FB_OK = 0,
	VAYU_FB_NONE,                   /* base, width or height 0: the loader found no display */
	VAYU_FB_DEPTH,                  /* not 32 bits a pixel (the only format the panel and the GOP give) */
	VAYU_FB_DIM,                    /* a side over VAYU_FB_MAX_DIM */
	VAYU_FB_ROWBYTES,               /* a row shorter than width * 4, or not a whole number of pixels */
	VAYU_FB_SIZE,                   /* rowbytes * height overflows or passes VAYU_FB_MAX_BYTES */
	VAYU_FB_WRAPS,                  /* base + size passes 2^64 */
};

static inline const char *
vayu_fb_check_name(enum vayu_fb_check_result r)
{
	switch (r) {
	case VAYU_FB_OK: return "ok";
	case VAYU_FB_NONE: return "no framebuffer (base, width or height 0)";
	case VAYU_FB_DEPTH: return "not 32 bits a pixel";
	case VAYU_FB_DIM: return "a side longer than 8192";
	case VAYU_FB_ROWBYTES: return "a row shorter than width * 4 or not whole pixels";
	case VAYU_FB_SIZE: return "rowbytes * height overflows or passes 256 MiB";
	case VAYU_FB_WRAPS: return "base + size passes 2^64";
	}
	return "?";
}

/* depth 0 is taken as 32: the loaders before boot_args v2 left it 0. *bytes: rowbytes * height when OK. */
static inline enum vayu_fb_check_result
vayu_fb_check(uint64_t base, uint64_t width, uint64_t height, uint64_t rowbytes, uint64_t depth, uint64_t *bytes)
{
	uint64_t size, end;

	if (base == 0 || width == 0 || height == 0) {
		return VAYU_FB_NONE;
	}
	if (depth != 32 && depth != 0) {
		return VAYU_FB_DEPTH;
	}
	if (width > VAYU_FB_MAX_DIM || height > VAYU_FB_MAX_DIM) {
		return VAYU_FB_DIM;
	}
	if (rowbytes < width * 4 || (rowbytes & 3) != 0) {
		return VAYU_FB_ROWBYTES;
	}
	if (__builtin_mul_overflow(rowbytes, height, &size) || size > VAYU_FB_MAX_BYTES) {
		return VAYU_FB_SIZE;
	}
	if (__builtin_add_overflow(base, size, &end)) {
		return VAYU_FB_WRAPS;
	}
	*bytes = size;
	return VAYU_FB_OK;
}

/* ---- the presented display on the panel ---------------------------------------------------------------------- */

/*
 * The panel is pw x ph. Presented as lw x lh (userland's size): when that is no smaller than the panel on both sides,
 * the panel shows its centre, (lw - pw) / 2, (lh - ph) / 2 in; a presented size smaller than the panel would leave
 * a border nothing draws, so it is refused and the panel's own size is presented (native). A logical point (x, y) is
 * the panel's (x - offx, y - offy).
 */
struct vayu_fb_layout {
	uint32_t pw, ph;                /* the panel */
	uint32_t lw, lh;                /* presented */
	int32_t  offx, offy;            /* where the panel's (0,0) is in presented coordinates */
	bool     native;                /* presented as the panel itself */
};

static inline void
vayu_fb_layout(uint32_t pw, uint32_t ph, uint32_t want_w, uint32_t want_h, struct vayu_fb_layout *l)
{
	l->pw = pw;
	l->ph = ph;
	if (want_w >= pw && want_h >= ph && want_w <= VAYU_FB_MAX_DIM && want_h <= VAYU_FB_MAX_DIM) {
		l->lw = want_w;
		l->lh = want_h;
	} else {
		l->lw = pw;
		l->lh = ph;
	}
	l->offx = (int32_t)((l->lw - pw) / 2);
	l->offy = (int32_t)((l->lh - ph) / 2);
	l->native = l->lw == pw && l->lh == ph;
}

/* ---- the SwapEnd record -------------------------------------------------------------------------------------- */

/*
 * IOMobileFramebuffer's SwapEnd (selector 5) sends a record: macOS 13 0x468 bytes, iOS 16 0x46c, iPadOS 18 0x514, iOS
 * 27 0x6e0 (kern_SwapEnd). It comes inband up to 4096 bytes, past that as a memory descriptor. The kernel keeps
 * VAYU_FB_REC_KEEP bytes of it (every field read lies in the first 0x200; 0x6e0 whole); a record shorter than
 * VAYU_FB_REC_MIN is refused, and so is one longer than VAYU_FB_REC_LIMIT (no IOMFB sends such, and a descriptor of any
 * length is not read for it). Inband and descriptor together: refused (which one would be the record?).
 */
#define VAYU_FB_REC_MIN         0x400u
#define VAYU_FB_REC_KEEP        0x800u
#define VAYU_FB_REC_LIMIT       0x4000u

enum vayu_fb_swap_result {
	VAYU_FB_SWAP_OK = 0,
	VAYU_FB_SWAP_SHORT,             /* shorter than VAYU_FB_REC_MIN */
	VAYU_FB_SWAP_LONG,              /* longer than VAYU_FB_REC_LIMIT */
	VAYU_FB_SWAP_BOTH,              /* inband bytes and a descriptor */
};

/* *take: the bytes to copy into the kept record (the rest of it is zero) */
static inline enum vayu_fb_swap_result
vayu_fb_swap_len(uint64_t inband, bool has_descriptor, uint64_t descriptor_len, uint32_t *take)
{
	uint64_t len;

	if (has_descriptor && inband != 0) {
		return VAYU_FB_SWAP_BOTH;
	}
	len = has_descriptor ? descriptor_len : inband;
	if (len < VAYU_FB_REC_MIN) {
		return VAYU_FB_SWAP_SHORT;
	}
	if (len > VAYU_FB_REC_LIMIT) {
		return VAYU_FB_SWAP_LONG;
	}
	*take = (uint32_t)(len < VAYU_FB_REC_KEEP ? len : VAYU_FB_REC_KEEP);
	return VAYU_FB_SWAP_OK;
}

/* ---- the brightness iOS 27 puts in a swap record -------------------------------------------------------------- */

/*
 * kern_SwapSetBrightness stores a double in the record (SWEET_FB_REC27_BRIGHT, the byte before it says "set"). What
 * it measures is not known for sure (HYPOTHESIS: a fraction 0..1, or nits): a value in [0, 1] is taken as the
 * fraction of full scale, a larger one as nits of a panel whose full scale is nits_max. Answer: 1..1000 permille, or
 * 0 when the bits are no usable number (negative, NaN, infinite, nits_max 0). Integers only (the kernel has no FP).
 */
static inline uint32_t
vayu_fb_brightness_permille(uint64_t bits, uint32_t nits_max)
{
	uint64_t frac = bits & ((1ull << 52) - 1), m, x;
	int exp = (int)((bits >> 52) & 0x7ff), e;

	if ((bits >> 63) != 0 || exp == 0x7ff) {
		return bits == (1ull << 63) ? 1 : 0;    /* -0.0 is 0: the lowest; anything else negative, NaN, inf: none */
	}
	if (exp == 0) {
		return 1;                               /* 0.0 and subnormals: the lowest level, not off */
	}
	m = frac | (1ull << 52);
	e = exp - 1023;
	if (e > 40) {
		x = UINT64_MAX;
	} else if (e > -64 + 52) {
		x = (m * 1000) >> (52 - e);             /* floor(v * 1000) */
	} else {
		x = 0;
	}
	if (x <= 1000) {
		return x < 1 ? 1 : (uint32_t)x;
	}
	if (nits_max == 0) {
		return 0;
	}
	x /= nits_max;
	return x < 1 ? 1 : x > 1000 ? 1000 : (uint32_t)x;
}

/* ---- CRC-32 of a picture ------------------------------------------------------------------------------------- */

/* zlib's CRC-32 (reflected 0xedb88320), by a table of 256 the caller keeps (vayu_fb_crc_table fills it once) */
static inline void
vayu_fb_crc_table(uint32_t table[256])
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
		}
		table[n] = c;
	}
}

/* start with crc = VAYU_FB_CRC_INIT, end with VAYU_FB_CRC_END (as zlib.crc32 does inside) */
#define VAYU_FB_CRC_INIT        0xffffffffu
#define VAYU_FB_CRC_END(c)      (~(c))
#define VAYU_FB_CRC_BYTE(t, c, b) (((c) >> 8) ^ (t)[((c) ^ (uint8_t)(b)) & 0xffu])

/*
 * One row of 32-bit pixels as the panel takes them (0xXXRRGGBB, in memory B, G, R, X): its R, G, B bytes into the CRC,
 * and counts of the pixels that are not black and of the pure white ones (the framebuffer console's text).
 */
static inline uint32_t
vayu_fb_crc_rgb_row(const uint32_t table[256], uint32_t crc, const volatile uint32_t *row, uint32_t width, uint32_t *lit,
    uint32_t *white)
{
	for (uint32_t x = 0; x < width; x++) {
		uint32_t v = row[x] & 0xffffffu;
		crc = VAYU_FB_CRC_BYTE(table, crc, v >> 16);
		crc = VAYU_FB_CRC_BYTE(table, crc, v >> 8);
		crc = VAYU_FB_CRC_BYTE(table, crc, v);
		*lit += v != 0;
		*white += v == 0xffffffu;
	}
	return crc;
}

#endif /* _VAYU_FB_RULES_H_ */
