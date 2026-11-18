/*
 * touch: taps, swipes and the two hardware buttons, from a shell on the phone.
 *
 * The samples go where rwtouch's go: /dev/hakios-touch (bsd/dev/hakios.input.c), which the input
 * thread of libhakios.dylib inside backboardd turns into IOHID events for the window server. The
 * record is 24 bytes (bsd/sys/hakios.input.h):
 *
 *     uint64_t time; uint32_t type, finger, x, y;
 *
 * x and y are fractions of the *picture* (0..65535), not of the panel. iOS is told the display is
 * 1125x2436 while the panel is 1080x2400 and fb_fit=1 shows the picture's middle 1080x2400 from
 * (22, 18) (day1-live.log:393-396). The kernel applies that crop only to samples its own touch
 * driver posts (hakios_touch_post_at(), hakios.input.c:237-260); a write() to the device goes
 * straight into the queue (touch_write(), :308-323). So this program does the crop itself:
 *
 *     panel (X, Y)  ->  picture (X + crop_x, Y + crop_y)  ->  (x, y) in 0..65535
 *
 * The display size is asked of IOMobileFramebuffer (selector 8, kFBMethodGetDisplaySize,
 * iokit/Kernel/MiniIOMobileFramebuffer.cpp:2078-2086); if the driver cannot be opened the values
 * from the boot log are used. --display WxH and --panel WxH override either.
 *
 * usage: touch tap X Y
 *        touch swipe X1 Y1 X2 Y2 [MS]        (default 300 ms, a sample every 8 ms)
 *        touch long X Y MS                   (press, hold, release)
 *        touch home                          (the home gesture: a swipe up from the bottom edge)
 *        touch power                         (the side/power button, HID consumer 0x30)
 *        touch button home|power|volup|voldown [MS]
 *
 * "touch home" is the gesture because this phone has no home button and backboardd says so at boot
 * (day1-live.log:8583); "touch button home" presses the HID consumer usage anyway.
 *
 * Coordinates are panel pixels: 0 0 is the top left corner of the 1080x2400 panel, 1079 2399 the
 * bottom right. Exit status: 0 done, 1 the device could not be written, 2 bad usage.
 */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#define TOUCH_DEVICE            "/dev/hakios-touch"

#define TOUCH_DOWN              1
#define TOUCH_MOVE              2
#define TOUCH_UP                3
#define BUTTON_DOWN             4
#define BUTTON_UP               5

#define BUTTON_LOCK             1       /* the side button: power/wake/sleep */
#define BUTTON_VOLUME_UP        2
#define BUTTON_VOLUME_DOWN      3
#define BUTTON_HOME             4

#define SAMPLE_MS               8       /* what the touch screen gives: rwtouch.c, SAMPLE_MS */

/* bsd/sys/hakios.input.h */
struct hakios_touch {
	uint64_t time;                  /* ignored on write: the kernel stamps it */
	uint32_t type;
	uint32_t finger;
	uint32_t x, y;                  /* 0 = left/top edge, 65535 = right/bottom edge of the picture */
};

_Static_assert(sizeof(struct hakios_touch) == 24, "the record /dev/hakios-touch takes");

static int device = -1;
static uint32_t panel_width = 1080, panel_height = 2400;
static uint32_t display_width = 1125, display_height = 2436;
static uint32_t crop_x = 22, crop_y = 18;       /* (display - panel) / 2, fb_fit=1 */
static int verbose;

static mach_timebase_info_data_t timebase;

static void
die(const char *what)
{
	fprintf(stderr, "touch: %s: %s\n", what, strerror(errno));
	exit(1);
}

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

static void
set_geometry(void)
{
	uint32_t width = 0, height = 0;

	if (!ask_display_size(&width, &height)) {
		if (verbose) {
			fprintf(stderr, "touch: no IOMobileFramebuffer answer; the boot log's %ux%u is assumed\n",
			    display_width, display_height);
		}
		return;
	}
	display_width = width;
	display_height = height;
	/* fb_fit=1: the picture's middle panel_width x panel_height, pixel for pixel */
	crop_x = display_width > panel_width ? (display_width - panel_width) / 2 : 0;
	crop_y = display_height > panel_height ? (display_height - panel_height) / 2 : 0;
	if (verbose) {
		fprintf(stderr, "touch: display %ux%u, panel %ux%u, crop %u,%u\n", display_width, display_height,
		    panel_width, panel_height, crop_x, crop_y);
	}
}

/* A panel pixel to the picture's 0..65535, as the window server reads it. */
static void
panel_to_screen(double px, double py, uint32_t *x, uint32_t *y)
{
	double sx = px + crop_x, sy = py + crop_y;

	if (sx < 0) sx = 0;
	if (sy < 0) sy = 0;
	if (sx > display_width - 1) sx = display_width - 1;
	if (sy > display_height - 1) sy = display_height - 1;
	/* the kernel's own driver path divides by the picture's size, not size-1 (hakios.input.c:249-256) */
	*x = (uint32_t) (sx * 65535.0 / display_width + 0.5);
	*y = (uint32_t) (sy * 65535.0 / display_height + 0.5);
}

static void
post(uint32_t type, uint32_t finger, unsigned x, unsigned y)
{
	struct hakios_touch t = { .time = 0, .type = type, .finger = finger, .x = x, .y = y };

	if (write(device, &t, sizeof(t)) != (ssize_t) sizeof(t)) {
		die("write " TOUCH_DEVICE);
	}
}

static void
post_at(uint32_t type, double px, double py)
{
	uint32_t x, y;

	panel_to_screen(px, py, &x, &y);
	if (verbose) {
		fprintf(stderr, "touch: %s panel %.1f %.1f -> %u %u\n", type == TOUCH_DOWN ? "down" :
		    type == TOUCH_MOVE ? "move" : "up", px, py, x, y);
	}
	post(type, 0, x, y);
}

static void
button(uint32_t which, int ms)
{
	if (verbose) {
		fprintf(stderr, "touch: button %u for %d ms\n", which, ms);
	}
	post(BUTTON_DOWN, which, 0, 0);
	usleep((useconds_t) ms * 1000);
	post(BUTTON_UP, which, 0, 0);
}

static uint64_t
micros(void)
{
	return (mach_absolute_time() * timebase.numer / timebase.denom) / 1000;
}

/* Sleep until `at` microseconds from `start`; the clock, not one sleep after another (rwtouch.c). */
static void
sleep_until(uint64_t start, uint64_t at)
{
	for (;;) {
		uint64_t now = micros() - start;

		if (now >= at) {
			return;
		}
		usleep((useconds_t) (at - now));
	}
}

static void
swipe(double x0, double y0, double x1, double y1, int ms)
{
	uint64_t start, total_us = (uint64_t) (ms > 0 ? ms : 1) * 1000;

	if (verbose) {
		fprintf(stderr, "touch: swipe %.1f %.1f -> %.1f %.1f in %d ms\n", x0, y0, x1, y1, ms);
	}
	post_at(TOUCH_DOWN, x0, y0);
	start = micros();
	for (uint64_t at = SAMPLE_MS * 1000;; at += SAMPLE_MS * 1000) {
		double part;

		if (at > total_us) {
			at = total_us;
		}
		part = (double) at / (double) total_us;
		sleep_until(start, at);
		post_at(TOUCH_MOVE, x0 + (x1 - x0) * part, y0 + (y1 - y0) * part);
		if (at == total_us) {
			break;
		}
	}
	usleep(16000);                  /* a finger that lifts is still on the panel for a frame */
	post_at(TOUCH_UP, x1, y1);
}

static void
tap(double x, double y)
{
	if (verbose) {
		fprintf(stderr, "touch: tap %.1f %.1f\n", x, y);
	}
	post_at(TOUCH_DOWN, x, y);
	usleep(60000);
	post_at(TOUCH_UP, x, y);
}

static int
usage(void)
{
	fprintf(stderr,
	    "usage: touch tap X Y\n"
	    "       touch swipe X1 Y1 X2 Y2 [MS]\n"
	    "       touch long X Y MS\n"
	    "       touch home                       (swipe up from the bottom edge)\n"
	    "       touch power | button home|power|volup|voldown [MS]\n"
	    "Coordinates are panel pixels (1080x2400); environment: TOUCH_VERBOSE=1.\n");
	return 2;
}

static int
number(const char *text, const char *what)
{
	char *end = NULL;
	long value = strtol(text, &end, 10);

	if (end == text || *end != 0) {
		fprintf(stderr, "touch: %s: \"%s\" is not a number\n", what, text);
		exit(2);
	}
	return (int) value;
}

int
main(int argc, char **argv)
{
	int i = 1;

	mach_timebase_info(&timebase);
	verbose = getenv("TOUCH_VERBOSE") != NULL;

	/* options first, so that every command line can carry them */
	while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
		if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
			if (sscanf(argv[i + 1], "%ux%u", &display_width, &display_height) != 2) {
				return usage();
			}
			crop_x = display_width > panel_width ? (display_width - panel_width) / 2 : 0;
			crop_y = display_height > panel_height ? (display_height - panel_height) / 2 : 0;
			i += 2;
		} else if (strcmp(argv[i], "--panel") == 0 && i + 1 < argc) {
			if (sscanf(argv[i + 1], "%ux%u", &panel_width, &panel_height) != 2) {
				return usage();
			}
			i += 2;
		} else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			verbose = 1;
			i++;
		} else {
			return usage();
		}
	}
	if (i >= argc) {
		return usage();
	}
	if (strcmp(argv[i], "tap") != 0 && strcmp(argv[i], "swipe") != 0 && strcmp(argv[i], "long") != 0 &&
	    strcmp(argv[i], "home") != 0 && strcmp(argv[i], "power") != 0 && strcmp(argv[i], "button") != 0) {
		return usage();
	}

	device = open(TOUCH_DEVICE, O_WRONLY);
	if (device < 0) {
		die(TOUCH_DEVICE);
	}
	if (strcmp(argv[i], "tap") == 0 || strcmp(argv[i], "swipe") == 0 || strcmp(argv[i], "long") == 0 ||
	    strcmp(argv[i], "home") == 0 || strcmp(argv[i], "button") == 0) {
		set_geometry();
	}

	if (strcmp(argv[i], "tap") == 0) {
		if (argc - i != 3) {
			return usage();
		}
		tap(number(argv[i + 1], "X"), number(argv[i + 2], "Y"));
	} else if (strcmp(argv[i], "swipe") == 0) {
		int ms = 300;

		if (argc - i != 5 && argc - i != 6) {
			return usage();
		}
		if (argc - i == 6) {
			ms = number(argv[i + 5], "MS");
		}
		swipe(number(argv[i + 1], "X1"), number(argv[i + 2], "Y1"), number(argv[i + 3], "X2"),
		    number(argv[i + 4], "Y2"), ms);
	} else if (strcmp(argv[i], "long") == 0) {
		double x, y;
		int ms;

		if (argc - i != 4) {
			return usage();
		}
		x = number(argv[i + 1], "X");
		y = number(argv[i + 2], "Y");
		ms = number(argv[i + 3], "MS");
		if (ms < 1) {
			ms = 1;
		}
		post_at(TOUCH_DOWN, x, y);
		usleep((useconds_t) ms * 1000);
		post_at(TOUCH_UP, x, y);
	} else if (strcmp(argv[i], "home") == 0) {
		if (argc - i != 1) {
			return usage();
		}
		/*
		 * The gesture, not the button: an iPhone of this generation has no home button, and
		 * backboardd says so at boot ("not configuring SSHB because we don't have a real home
		 * button", day1-live.log:8583). The swipe starts in the bottom edge strip that inject.c
		 * marks DIGITIZER_FROM_EDGE_TIP (y > 1 - 34/812), which is what SpringBoard's home
		 * recognizer takes; "button home" presses the HID menu usage instead.
		 */
		swipe(panel_width / 2.0, panel_height - 1, panel_width / 2.0, panel_height * 0.55, 250);
	} else if (strcmp(argv[i], "power") == 0) {
		if (argc - i != 1) {
			return usage();
		}
		button(BUTTON_LOCK, 80);
	} else if (strcmp(argv[i], "button") == 0) {
		uint32_t which;
		int ms = argc - i == 3 ? number(argv[i + 2], "MS") : 80;

		if (argc - i != 2 && argc - i != 3) {
			return usage();
		}
		if (strcmp(argv[i + 1], "home") == 0) {
			which = BUTTON_HOME;
		} else if (strcmp(argv[i + 1], "power") == 0 || strcmp(argv[i + 1], "lock") == 0) {
			which = BUTTON_LOCK;
		} else if (strcmp(argv[i + 1], "volup") == 0) {
			which = BUTTON_VOLUME_UP;
		} else if (strcmp(argv[i + 1], "voldown") == 0) {
			which = BUTTON_VOLUME_DOWN;
		} else {
			fprintf(stderr, "touch: buttons are home, power, volup and voldown\n");
			return 2;
		}
		button(which, ms);
	}
	close(device);
	return 0;
}
