/*
 * fbshot: the screen as it is now, as a PNG.
 *
 * iOS's window server (inside backboardd/SpringBoard) draws every frame into an IOSurface and hands
 * it to the display driver with SwapEnd; the driver copies the surface into the panel's memory
 * (iokit/Kernel/MiniIOMobileFramebuffer.cpp, show()/drawLayer()). There is no framebuffer device
 * node to read (no devfs_make_node in that file, and /dev in day1-live.log has none), so the frame
 * is taken where it lives: the same surface the driver copies.
 *
 * The surfaces the window server uses are 1125x2436 'BGRA' and alternate (day1-live.log:6190
 * "swap 1 layer 0: surface 3 (1125x2436 'BGRA')", :8922 "surface 4"). IOSurfaceLookup() by number
 * reaches any surface that exists, from any process (MiniIOSurface.cpp:1518-1542, methodLookup:
 * the global table, no check of who asks), and IOSurfaceGetSeed() says how often the surface has
 * been locked for drawing (MiniIOSurface.cpp:2018-2031). The picture is therefore the full-screen
 * surface with the highest seed: the one the window server is drawing into now. A stale surface
 * (an application's old layer, a snapshot) has a seed that stopped growing long ago.
 *
 * What is written is the part the panel shows: fb_fit=1 shows the picture's middle 1080x2400 from
 * (22, 18) (day1-live.log:393-396), so the default PNG is 1080x2400. --full writes the whole
 * 1125x2436 picture instead, and --crop X,Y,W,H an exact rectangle of it (picture pixels).
 *
 * usage: fbshot [-v] [-l] [-i ID] [-m MAXID] [-f] [-c X,Y,W,H] [-p WxH] FILE.png
 *   -v        say which surfaces were found and which one was taken
 *   -l        list every candidate surface (size, format, seed, bytes a row)
 *   -i ID     take this surface number (from a previous -l) instead of choosing one
 *   -m MAXID  how far to scan for surfaces (default 4096; numbers only grow)
 *   -f        the whole picture, not the panel's crop
 *   -p WxH    the panel size the crop is made for (default 1080x2400)
 *
 * It runs as root from an ssh session; no entitlement check is in the way (see above).
 * Exit status: 0 written, 1 failed (no surface, no pixels, PNG not written), 2 bad usage.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>

#define FOURCC(a, b, c, d) ((uint32_t) (a) << 24 | (uint32_t) (b) << 16 | (uint32_t) (c) << 8 | (uint32_t) (d))

#define MAX_SURFACES        4096

static int verbose, list;
static IOSurfaceID wanted;
static uint32_t panel_width = 1080, panel_height = 2400;
static uint32_t display_width = 1125, display_height = 2436;
static int full, have_crop;
static uint32_t crop_x, crop_y, crop_width, crop_height;

/* The display as iOS sees it (selector 8 of IOMobileFramebufferUserClient). */
static int
ask_display_size(uint32_t *width, uint32_t *height)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("IOMobileFramebuffer"));
	io_connect_t connection = 0;
	uint64_t out[2] = { 0, 0 };
	uint32_t count = 2;
	kern_return_t kr;

	if (service == 0) {
		return 0;
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		return 0;
	}
	kr = IOConnectCallScalarMethod(connection, 8, NULL, 0, out, &count);
	IOServiceClose(connection);
	if (kr != KERN_SUCCESS || count < 2 || out[0] < 100 || out[1] < 100) {
		return 0;
	}
	*width = (uint32_t) out[0];
	*height = (uint32_t) out[1];
	return 1;
}

struct candidate {
	IOSurfaceID id;
	uint32_t width, height, bytes_per_row, bytes_per_element, format, seed;
	size_t alloc;
};

static uint64_t
score(const struct candidate *candidate)
{
	uint64_t exact = candidate->width == display_width && candidate->height == display_height ? 1 : 0;

	/* an exact full-screen surface first; then the one being drawn into most (its seed); then size */
	return (exact << 48) | ((uint64_t) candidate->seed << 16) | ((uint64_t) (candidate->alloc >> 20) & 0xffff);
}

static int
pixel_format_ok(uint32_t format)
{
	return format == FOURCC('B', 'G', 'R', 'A') || format == FOURCC('A', 'R', 'G', 'B') ||
	    format == FOURCC('R', 'G', 'B', 'A');
}

static const char *
format_name(uint32_t format, char *text)
{
	text[0] = (char) (format >> 24);
	text[1] = (char) (format >> 16);
	text[2] = (char) (format >> 8);
	text[3] = (char) format;
	text[4] = 0;
	return text;
}

/* One surface, described in one line. 1 if it can be a screen. */
static int
describe(IOSurfaceRef surface, struct candidate *candidate)
{
	char name[8];

	memset(candidate, 0, sizeof(*candidate));
	candidate->id = IOSurfaceGetID(surface);
	candidate->width = (uint32_t) IOSurfaceGetWidth(surface);
	candidate->height = (uint32_t) IOSurfaceGetHeight(surface);
	candidate->bytes_per_row = (uint32_t) IOSurfaceGetBytesPerRow(surface);
	candidate->bytes_per_element = (uint32_t) IOSurfaceGetBytesPerElement(surface);
	candidate->format = IOSurfaceGetPixelFormat(surface);
	candidate->seed = IOSurfaceGetSeed(surface);
	candidate->alloc = IOSurfaceGetAllocSize(surface);
	if (list) {
		fprintf(stderr, "fbshot: surface %u: %ux%u '%s', %u bytes a row, %u bytes an element, "
		    "%zu bytes, seed %u, %zu planes\n", candidate->id, candidate->width, candidate->height,
		    format_name(candidate->format, name), candidate->bytes_per_row, candidate->bytes_per_element,
		    candidate->alloc, candidate->seed, IOSurfaceGetPlaneCount(surface));
	}
	/* a single-plane surface (IOSurface.framework answers 1 for the implicit plane) of 32-bit pixels */
	return candidate->width >= 320 && candidate->height >= 320 && candidate->bytes_per_element == 4 &&
	    IOSurfaceGetPlaneCount(surface) <= 1 && pixel_format_ok(candidate->format);
}

/*
 * The pixels of the chosen rectangle, 4 bytes a pixel in BGRA order whatever the surface held.
 * NULL when the surface cannot be mapped or read.
 */
static uint8_t *
copy_pixels(IOSurfaceRef surface, const struct candidate *candidate, uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
	const uint8_t *base;
	uint8_t *pixels;
	uint32_t seed = 0;
	size_t row_bytes = (size_t) width * 4;
	size_t size = row_bytes * height;

	if (x + width > candidate->width || y + height > candidate->height ||
	    candidate->bytes_per_row < candidate->width * 4) {
		fprintf(stderr, "fbshot: the rectangle does not fit the surface\n");
		return NULL;
	}
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, &seed);       /* what a reader does (MiniIOSurface: seed only) */
	base = IOSurfaceGetBaseAddress(surface);
	if (base == NULL) {
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
		fprintf(stderr, "fbshot: surface %u has no mapping in this process\n", candidate->id);
		return NULL;
	}
	pixels = malloc(size);
	if (pixels == NULL) {
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
		fprintf(stderr, "fbshot: no memory for %zu bytes\n", size);
		return NULL;
	}
	for (uint32_t row = 0; row < height; row++) {
		const uint8_t *from = base + (size_t) (y + row) * candidate->bytes_per_row + (size_t) x * 4;
		uint8_t *to = pixels + (size_t) row * row_bytes;

		if (candidate->format == FOURCC('B', 'G', 'R', 'A')) {
			memcpy(to, from, row_bytes);
		} else if (candidate->format == FOURCC('A', 'R', 'G', 'B')) {
			for (uint32_t column = 0; column < width; column++) {
				to[column * 4 + 0] = from[column * 4 + 3];       /* B */
				to[column * 4 + 1] = from[column * 4 + 2];       /* G */
				to[column * 4 + 2] = from[column * 4 + 1];       /* R */
				to[column * 4 + 3] = from[column * 4 + 0];       /* A */
			}
		} else {                                                /* 'RGBA' */
			for (uint32_t column = 0; column < width; column++) {
				to[column * 4 + 0] = from[column * 4 + 2];
				to[column * 4 + 1] = from[column * 4 + 1];
				to[column * 4 + 2] = from[column * 4 + 0];
				to[column * 4 + 3] = from[column * 4 + 3];
			}
		}
	}
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	return pixels;
}

static char *
absolute(const char *path)
{
	char *result;

	if (path[0] == '/') {
		return strdup(path);
	}
	{
		char directory[1024];

		if (getcwd(directory, sizeof(directory)) == NULL) {
			return NULL;
		}
		result = malloc(strlen(directory) + strlen(path) + 2);
		if (result != NULL) {
			sprintf(result, "%s/%s", directory, path);
		}
	}
	return result;
}

static int
write_png(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height)
{
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGDataProviderRef provider = NULL;
	CGImageRef image = NULL;
	CGImageDestinationRef destination = NULL;
	CFURLRef url = NULL;
	int written = 0;

	if (space != NULL) {
		provider = CGDataProviderCreateWithData(NULL, pixels, (size_t) width * height * 4, NULL);
	}
	if (provider != NULL) {
		image = CGImageCreate(width, height, 8, 32, (size_t) width * 4, space,
		    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL, false,
		    kCGRenderingIntentDefault);
	}
	if (image != NULL) {
		url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *) path, (CFIndex) strlen(path), false);
	}
	if (url != NULL) {
		destination = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
	}
	if (destination != NULL) {
		CGImageDestinationAddImage(destination, image, NULL);
		written = CGImageDestinationFinalize(destination) ? 1 : 0;
	}
	if (destination != NULL) {
		CFRelease(destination);
	}
	if (url != NULL) {
		CFRelease(url);
	}
	if (image != NULL) {
		CGImageRelease(image);
	}
	if (provider != NULL) {
		CGDataProviderRelease(provider);
	}
	if (space != NULL) {
		CGColorSpaceRelease(space);
	}
	return written;
}

static int
usage(void)
{
	fprintf(stderr, "usage: fbshot [-v] [-l] [-i ID] [-m MAXID] [-f] [-c X,Y,W,H] [-p WxH] FILE.png\n");
	return 2;
}

int
main(int argc, char **argv)
{
	const char *path = NULL;
	uint32_t max_sid = MAX_SURFACES;
	IOSurfaceRef best = NULL;
	struct candidate best_candidate;
	int i = 1;

	memset(&best_candidate, 0, sizeof(best_candidate));
	while (i < argc) {
		const char *option = argv[i];

		if (option[0] != '-' || option[1] == 0) {
			break;
		}
		if (strcmp(option, "-v") == 0) {
			verbose = 1;
			i++;
		} else if (strcmp(option, "-l") == 0) {
			list = 1;
			i++;
		} else if (strcmp(option, "-f") == 0) {
			full = 1;
			i++;
		} else if (strcmp(option, "-i") == 0 && i + 1 < argc) {
			wanted = (IOSurfaceID) strtoul(argv[i + 1], NULL, 0);
			i += 2;
		} else if (strcmp(option, "-m") == 0 && i + 1 < argc) {
			max_sid = (uint32_t) strtoul(argv[i + 1], NULL, 0);
			i += 2;
		} else if (strcmp(option, "-p") == 0 && i + 1 < argc) {
			if (sscanf(argv[i + 1], "%ux%u", &panel_width, &panel_height) != 2) {
				return usage();
			}
			i += 2;
		} else if (strcmp(option, "-c") == 0 && i + 1 < argc) {
			if (sscanf(argv[i + 1], "%u,%u,%u,%u", &crop_x, &crop_y, &crop_width, &crop_height) != 4) {
				return usage();
			}
			have_crop = 1;
			i += 2;
		} else {
			return usage();
		}
	}
	if (i != argc - 1) {
		return usage();
	}
	path = argv[i];

	if (!ask_display_size(&display_width, &display_height) && verbose) {
		fprintf(stderr, "fbshot: no answer from IOMobileFramebuffer; the boot log's %ux%u is assumed\n",
		    display_width, display_height);
	}

	if (wanted != 0) {
		best = IOSurfaceLookup(wanted);
		if (best == NULL) {
			fprintf(stderr, "fbshot: there is no surface %u\n", wanted);
			return 1;
		}
		if (!describe(best, &best_candidate)) {
			fprintf(stderr, "fbshot: surface %u is not a picture this program can read\n", wanted);
			CFRelease(best);
			return 1;
		}
	} else {
		for (uint32_t sid = 1; sid <= max_sid; sid++) {
			IOSurfaceRef surface = IOSurfaceLookup(sid);
			struct candidate candidate;

			if (surface == NULL) {
				continue;
			}
			if (!describe(surface, &candidate)) {
				CFRelease(surface);
				continue;
			}
			if (best == NULL || score(&candidate) > score(&best_candidate)) {
				if (best != NULL) {
					CFRelease(best);
				}
				best = surface;
				best_candidate = candidate;
			} else {
				CFRelease(surface);
			}
		}
	}
	if (best == NULL) {
		fprintf(stderr, "fbshot: no full-screen IOSurface found in 1..%u\n", max_sid);
		return 1;
	}

	if (!have_crop) {
		if (!full && best_candidate.width > panel_width && best_candidate.height > panel_height) {
			crop_x = (best_candidate.width - panel_width) / 2;
			crop_y = (best_candidate.height - panel_height) / 2;
			crop_width = panel_width;
			crop_height = panel_height;
		} else {
			crop_x = crop_y = 0;
			crop_width = best_candidate.width;
			crop_height = best_candidate.height;
		}
	}
	{
		uint8_t *pixels = copy_pixels(best, &best_candidate, crop_x, crop_y, crop_width, crop_height);
		char *where = absolute(path);

		if (pixels == NULL) {
			CFRelease(best);
			free(where);
			return 1;
		}
		if (where == NULL || !write_png(where, pixels, crop_width, crop_height)) {
			fprintf(stderr, "fbshot: %s: %s\n", path, where == NULL ? strerror(errno) : "PNG not written");
			free(where);
			free(pixels);
			CFRelease(best);
			return 1;
		}
		if (verbose) {
			fprintf(stderr, "fbshot: surface %u %ux%u seed %u -> %s (%ux%u from %u,%u)\n", best_candidate.id,
			    best_candidate.width, best_candidate.height, best_candidate.seed, where, crop_width,
			    crop_height, crop_x, crop_y);
		} else {
			printf("fbshot: %s (%ux%u, surface %u)\n", where, crop_width, crop_height, best_candidate.id);
		}
		free(where);
		free(pixels);
		CFRelease(best);
	}
	return 0;
}
