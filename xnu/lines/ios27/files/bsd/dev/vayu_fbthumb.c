/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU diagnostics: a thumbnail of the screen in the log, for when nobody is looking at the phone (U3G1).
 *
 * Every N seconds the buffer on scanout -- what hakios_fb_scanned() names: the panel's memory, which the CPU copy and
 * the GPU copy both present into (iokit/Kernel/arm/SweetFramebuffer.cpp) -- is shrunk by D in each direction (each
 * thumbnail pixel the average of four samples of its D x D block) into RGB565 or RGB888, and printed as base64:
 *
 *   FBTHUMB <seq> <w>x<h> begin fmt=<rgb565|rgb888> div=<D> src=<W>x<H> lines=<L> bytes=<B> t=<s>
 *   FBTHUMB <seq> <w>x<h> <line#>/<L>: <base64 of up to 144 bytes>
 *   FBTHUMB <seq> <w>x<h> end crc32=<hex> bytes=<B>
 *   FBTHUMB <seq> <w>x<h> same as <seq'> crc32=<hex> t=<s>      (the picture has not changed: no lines)
 *
 * The thumbnail is taken at once (a few ms of uncached reads) and printed afterwards a line at a time with a pause
 * between lines, so the log keeps flowing: 1080x2400 / 8 = 135x300 in RGB565 is 81000 bytes, 563 lines of 192
 * characters, about 11 s at the default 20 ms a line. tools/fbthumb2png.py (in the repository) turns a log back into PNGs.
 *
 * Boot-args (with vayu27=1, which runs the hook):
 *   vayu_fbthumb=N        every N seconds (at least 5); 0 or absent: off, nothing is started
 *   vayu_fbthumb_div=D    the shrink factor, 2..32 (default 8)
 *   vayu_fbthumb_fmt=888  RGB888 instead of RGB565 (half again as many bytes)
 *   vayu_fbthumb_ms=M     the pause between lines, 0..1000 ms (default 20)
 *   vayu_fbthumb_delay=S  the first thumbnail S seconds after the hook (default N)
 *   vayu_fbthumb_count=K  K thumbnails, then the thread ends (default 0: no end)
 * Only reads the panel's memory; nothing is written anywhere but the log.
 */
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>

#pragma clang diagnostic error "-Wuninitialized"
#pragma clang diagnostic error "-Wsometimes-uninitialized"

extern void IOSleep(unsigned int milliseconds);
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
/* iokit/Kernel/arm/SweetFramebuffer.cpp: the buffer on scanout; 0 before the framebuffer has started */
extern int hakios_fb_scanned(uintptr_t *base, uint32_t *width, uint32_t *height, uint32_t *rowbytes, unsigned *index);

#define FT_LINE_BYTES   144                     /* 192 base64 characters a line */
#define FT_MAX_BYTES    (512 * 1024)            /* a thumbnail bigger than this is refused (div too small) */

static uint32_t ft_every, ft_div = 8, ft_bpp = 2, ft_ms = 20, ft_delay, ft_count;

static const char ft_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
ft_base64(const uint8_t *in, size_t n, char *out)
{
	size_t o = 0;

	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < n) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (i + 2 < n) {
			v |= in[i + 2];
		}
		out[o++] = ft_b64[(v >> 18) & 63];
		out[o++] = ft_b64[(v >> 12) & 63];
		out[o++] = i + 1 < n ? ft_b64[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < n ? ft_b64[v & 63] : '=';
	}
	out[o] = 0;
}

static uint32_t
ft_seconds(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_SEC);
}

/* the panel's BGRA8888 word: memory order B, G, R, A */
static inline void
ft_add(volatile const uint8_t *row, uint32_t x, uint32_t *r, uint32_t *g, uint32_t *b)
{
	uint32_t p = *(volatile const uint32_t *)(row + (uint64_t)x * 4);

	*b += p & 0xff;
	*g += (p >> 8) & 0xff;
	*r += (p >> 16) & 0xff;
}

/* the thumbnail of the buffer into out (w * h * ft_bpp bytes) */
static void
ft_take(uintptr_t base, uint32_t rowbytes, uint32_t w, uint32_t h, uint8_t *out)
{
	uint32_t d = ft_div, q = d / 4, q3 = d - 1 - d / 4;

	for (uint32_t ty = 0; ty < h; ty++) {
		volatile const uint8_t *r0 = (volatile const uint8_t *)(base + (uint64_t)(ty * d + q) * rowbytes);
		volatile const uint8_t *r1 = (volatile const uint8_t *)(base + (uint64_t)(ty * d + q3) * rowbytes);

		for (uint32_t tx = 0; tx < w; tx++) {
			uint32_t r = 0, g = 0, b = 0, x0 = tx * d + q, x1 = tx * d + q3;
			uint8_t *o = out + ((uint64_t)ty * w + tx) * ft_bpp;

			ft_add(r0, x0, &r, &g, &b);
			ft_add(r0, x1, &r, &g, &b);
			ft_add(r1, x0, &r, &g, &b);
			ft_add(r1, x1, &r, &g, &b);
			r /= 4; g /= 4; b /= 4;
			if (ft_bpp == 3) {
				o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b;
			} else {
				uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
				o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8);       /* little-endian */
			}
		}
	}
}

static void
ft_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint8_t *buf = NULL;
	size_t size = 0;
	uint32_t seq = 0, last_crc = 0, last_seq = 0, last_w = 0, last_h = 0;
	char line[FT_LINE_BYTES / 3 * 4 + 1];

	IOSleep(ft_delay * 1000);
	for (uint32_t n = 0; ft_count == 0 || n < ft_count; n++) {
		uintptr_t base = 0;
		uint32_t sw = 0, sh = 0, rowbytes = 0;
		unsigned index = 0;

		if (hakios_fb_scanned(&base, &sw, &sh, &rowbytes, &index) == 0 || base == 0 || sw < ft_div || sh < ft_div) {
			printf("FBTHUMB %u 0x0 none: no buffer on scanout yet\n", ++seq);
			IOSleep(ft_every * 1000);
			continue;
		}
		uint32_t w = sw / ft_div, h = sh / ft_div;
		size_t bytes = (size_t)w * h * ft_bpp;

		if (bytes > FT_MAX_BYTES) {
			printf("FBTHUMB: %ux%u / %u is %lu bytes, more than %u: off\n", sw, sh, ft_div, (unsigned long)bytes,
			    FT_MAX_BYTES);
			break;
		}
		if (bytes > size) {
			if (buf != NULL) {
				kfree_data(buf, size);
			}
			buf = kalloc_data(bytes, Z_WAITOK);
			size = buf != NULL ? bytes : 0;
			if (buf == NULL) {
				printf("FBTHUMB: no memory for %lu bytes: off\n", (unsigned long)bytes);
				break;
			}
		}
		seq++;
		ft_take(base, rowbytes, w, h, buf);
		uint32_t crc = crc32(0, buf, bytes);
		if (seq > 1 && crc == last_crc && w == last_w && h == last_h) {
			printf("FBTHUMB %u %ux%u same as %u crc32=%08x t=%u\n", seq, w, h, last_seq, crc, ft_seconds());
		} else {
			uint32_t lines = (uint32_t)((bytes + FT_LINE_BYTES - 1) / FT_LINE_BYTES);

			printf("FBTHUMB %u %ux%u begin fmt=%s div=%u src=%ux%u lines=%u bytes=%lu t=%u\n", seq, w, h,
			    ft_bpp == 3 ? "rgb888" : "rgb565", ft_div, sw, sh, lines, (unsigned long)bytes, ft_seconds());
			for (uint32_t l = 0; l < lines; l++) {
				size_t off = (size_t)l * FT_LINE_BYTES, n_ = MIN((size_t)FT_LINE_BYTES, bytes - off);

				ft_base64(buf + off, n_, line);
				printf("FBTHUMB %u %ux%u %u/%u: %s\n", seq, w, h, l + 1, lines, line);
				if (ft_ms != 0) {
					IOSleep(ft_ms);
				}
			}
			printf("FBTHUMB %u %ux%u end crc32=%08x bytes=%lu\n", seq, w, h, crc, (unsigned long)bytes);
			last_seq = seq;
		}
		last_crc = crc;
		last_w = w;
		last_h = h;
		IOSleep(ft_every * 1000);
	}
	if (buf != NULL) {
		kfree_data(buf, size);
	}
	printf("FBTHUMB: done\n");
}

static void
vayu_fbthumb_start(void)
{
	uint32_t fmt = 565;
	thread_t thread;

	if (!PE_parse_boot_argn("vayu_fbthumb", &ft_every, sizeof(ft_every)) || ft_every == 0) {
		return;
	}
	ft_every = ft_every < 5 ? 5 : ft_every > 3600 ? 3600 : ft_every;
	ft_delay = ft_every;
	PE_parse_boot_argn("vayu_fbthumb_div", &ft_div, sizeof(ft_div));
	PE_parse_boot_argn("vayu_fbthumb_fmt", &fmt, sizeof(fmt));
	PE_parse_boot_argn("vayu_fbthumb_ms", &ft_ms, sizeof(ft_ms));
	PE_parse_boot_argn("vayu_fbthumb_delay", &ft_delay, sizeof(ft_delay));
	PE_parse_boot_argn("vayu_fbthumb_count", &ft_count, sizeof(ft_count));
	ft_div = ft_div < 2 ? 2 : ft_div > 32 ? 32 : ft_div;
	ft_bpp = fmt == 888 ? 3 : 2;
	ft_ms = ft_ms > 1000 ? 1000 : ft_ms;
	ft_delay = ft_delay > 3600 ? 3600 : ft_delay;
	printf("FBTHUMB: every %u s after %u s, 1/%u, %s, %u ms a line, %u thumbnail(s) (0: no end)\n", ft_every,
	    ft_delay, ft_div, ft_bpp == 3 ? "rgb888" : "rgb565", ft_ms, ft_count);
	if (kernel_thread_start(ft_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, fbthumb, vayu_fbthumb_start);
