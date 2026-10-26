/*
 * libhakios.motion.dylib, inserted into backboardd beside libhakios.dylib (etc/inject.conf): the phone's
 * accelerometer to iOS, for auto-rotate. It reads the kernel's /dev/vayu-motion and hands the samples to the
 * HID event server as a virtual sensor service.
 *
 * A library of its own and not more of inject.c, so that it can be left out for a phone without the
 * sensor. backboardd is the HID event server; like libhakios's touch screen, the accelerometer is a
 * virtual service made by a client of that server inside the same process.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const char *getprogname(void);

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* ---- the HID event system, as much of it as is used (no header for it in the iOS SDK; as in inject.c) ---- */

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateAccelerometerEvent(CFAllocatorRef allocator, uint64_t time, double x, double y, double z,
    uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3
#define FIELD_IS_BUILT_IN               0x4

static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

static volatile double motion_interval_us;      /* the accelerometer's ReportInterval, from the event server; 0 = not said */

struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	return value ? CFRetain(value) : NULL;
}

/* What the event server tells the accelerometer: opened; how often it wants samples (us, a double); shake thresholds. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static unsigned told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	if (key == NULL || value == NULL) {
		return true;
	}
	if (CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) && CFGetTypeID(value) == CFBooleanGetTypeID() &&
	    CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (CFEqual(key, CFSTR("ReportInterval")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		double interval = 0;

		CFNumberGetValue(value, kCFNumberDoubleType, &interval);
		if (interval > 0) {
			motion_interval_us = interval;
		}
	}
	if (told++ < 40) {
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = CFCopyDescription(value);
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: motion: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/* A built-in HID service with one usage, made again until the event server has opened it (as inject.c's make_service). */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("hakios"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("hakios: motion: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* ---- the accelerometer (the sensor work's spec, the bring-up notes) --------------------------- */

/*
 * The kernel's latest accelerometer sample (vayu-sens, sys/vayu_ssc.h): /dev/vayu-motion exists only with
 * vayu_slpi=6 once the sensor core streams; a read never blocks. Values are the sensor core's: Android's convention,
 * m/s^2 x 1000, flat face up about (0, 0, +9807).
 */
#define VAYU_MOTION_DEVICE      "/dev/vayu-motion"
#define VAYU_MOTION_VERSION     1
struct vayu_motion {
	uint32_t version, seq;          /* seq 0: none yet */
	int32_t accel_mm_s2[3];
	uint32_t age_ms;
};
#define STANDARD_GRAVITY_MM     9806.65
#define HID_PAGE_APPLE_VENDOR   0xff00  /* BKOrientationManager's accelerometer: 0xFF00/3, built in (backboardd 0x100079fe4) */
#define HID_PAGE_APPLE_MOTION   0xff0c  /* one of CoreMotion's (CLIoHidInterface) matches */
#define HID_USAGE_ACCELEROMETER 3

/* UIDeviceOrientation */
enum { ORIENT_UNKNOWN, ORIENT_PORTRAIT, ORIENT_UPSIDE_DOWN, ORIENT_LANDSCAPE_LEFT, ORIENT_LANDSCAPE_RIGHT };

/*
 * Fallback B of the spec: the orientation worked out here and handed to backboardd's orientation manager, for when
 * CoreMotion does not read our services. Its instance is asked for by the class methods such a manager has; none
 * answering is said once and B stays off.
 */
static void
motion_set_orientation(int orientation)
{
	static id manager;
	static bool looked;
	SEL update = sel_registerName("updateDeviceOrientation:changeSource:");

	if (!looked) {
		Class cls = objc_getClass("BKOrientationManager");
		static const char *const getters[] = { "sharedInstance", "sharedManager", "defaultManager", "sharedOrientationManager" };

		looked = true;
		for (unsigned i = 0; cls != Nil && manager == nil && i < sizeof(getters) / sizeof(getters[0]); i++) {
			SEL getter = sel_registerName(getters[i]);

			if (class_getClassMethod(cls, getter) != NULL) {
				manager = ((id (*)(id, SEL))objc_msgSend)((id)cls, getter);
				say("hakios: motion: BKOrientationManager +%s: %p\n", getters[i], (void *)manager);
			}
		}
		if (manager == nil || !class_respondsToSelector(object_getClass(manager), update)) {
			say("hakios: motion: no BKOrientationManager instance to tell (class %p): fallback B is off\n", (void *)cls);
			manager = nil;
		}
	}
	if (manager != nil) {
		((void (*)(id, SEL, long, long))objc_msgSend)(manager, update, orientation, 0);
		say("hakios: motion: orientation %d handed to BKOrientationManager\n", orientation);
	}
}

/*
 * The orientation of the samples (g, Apple's sign: portrait upright reads y = -1): flat (|z| > 0.8 g) decides
 * nothing; an axis must hold more than sin(25 deg) beyond the other to win (hysteresis), and the winner must
 * stay 300 ms before it is said.
 */
static void
motion_orientation(double x, double y, double z, uint64_t now_us)
{
	static int current = ORIENT_UNKNOWN, candidate = ORIENT_UNKNOWN;
	static uint64_t since;
	int seen = ORIENT_UNKNOWN;

	if (z > 0.8 || z < -0.8) {
		return;
	}
	if (y < 0 && -y > (x < 0 ? -x : x) + 0.42) {
		seen = ORIENT_PORTRAIT;
	} else if (y > 0 && y > (x < 0 ? -x : x) + 0.42) {
		seen = ORIENT_UPSIDE_DOWN;
	} else if (x < 0 && -x > (y < 0 ? -y : y) + 0.42) {
		seen = ORIENT_LANDSCAPE_LEFT;       /* HYPOTHESIS: right edge up; flip on the bench if mirrored */
	} else if (x > 0 && x > (y < 0 ? -y : y) + 0.42) {
		seen = ORIENT_LANDSCAPE_RIGHT;
	}
	if (seen == ORIENT_UNKNOWN || seen == current) {
		candidate = ORIENT_UNKNOWN;
		return;
	}
	if (seen != candidate) {
		candidate = seen;
		since = now_us;
		return;
	}
	if (now_us - since >= 300000) {
		say("hakios: motion: orientation %d -> %d (x %.2f y %.2f z %.2f g)\n", current, seen, x, y, z);
		current = seen;
		candidate = ORIENT_UNKNOWN;
		motion_set_orientation(seen);
	}
}

/*
 * Boot-arg motion_orient=0 (off), 1 (A: the samples as accelerometer events on 0xFF00/3 and 0xFF0C/3; the default),
 * 2 (A, and B: the orientation from them told to backboardd directly). Nothing at all happens without the device.
 */
static void
motion_run(IOHIDEventSystemClientRef client, int mode)
{
	IOHIDServiceClientRef services[2] = { NULL, NULL };
	struct vayu_motion m;
	uint32_t last_seq = 0;
	unsigned shown = 0;
	uint64_t last_said = 0;
	int device = -1;

	for (unsigned waited = 0; (device = open(VAYU_MOTION_DEVICE, O_RDONLY)) < 0; waited++) {
		if (waited == 0) {
			say("hakios: motion: %s: %s; trying every 2 s\n", VAYU_MOTION_DEVICE, strerror(errno));
		}
		if (waited >= 150) {            /* 5 minutes: the sensor core is not coming in this boot */
			say("hakios: motion: %s never came\n", VAYU_MOTION_DEVICE);
			return;
		}
		sleep(2);
	}
	services[0] = make_service(client, "Hakios accelerometer", HID_PAGE_APPLE_VENDOR, HID_USAGE_ACCELEROMETER);
	services[1] = make_service(client, "Hakios motion accel", HID_PAGE_APPLE_MOTION, HID_USAGE_ACCELEROMETER);
	say("hakios: motion: %s open, services %p (0xff00/3) %p (0xff0c/3), mode %d\n", VAYU_MOTION_DEVICE,
	    (void *)services[0], (void *)services[1], mode);
	for (;;) {
		double interval = motion_interval_us > 0 ? motion_interval_us : 20000;
		uint64_t now = mach_absolute_time(), now_us = micros(now);

		if (interval < 10000) interval = 10000;
		if (interval > 200000) interval = 200000;
		if (read(device, &m, sizeof(m)) == (ssize_t)sizeof(m) && m.version == VAYU_MOTION_VERSION && m.seq != 0 &&
		    m.seq != last_seq) {
			/* g, Apple's sign (HYPOTHESIS: Apple flat face up z = -1 g, Android +9.8) */
			double x = -m.accel_mm_s2[0] / STANDARD_GRAVITY_MM, y = -m.accel_mm_s2[1] / STANDARD_GRAVITY_MM,
			    z = -m.accel_mm_s2[2] / STANDARD_GRAVITY_MM;

			last_seq = m.seq;
			for (unsigned i = 0; i < 2; i++) {
				IOHIDEventRef event;

				if (services[i] == NULL) continue;
				event = IOHIDEventCreateAccelerometerEvent(NULL, now, x, y, z, 0);
				if (event == NULL) continue;
				IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
				IOHIDVirtualServiceClientDispatchEvent(services[i], event);
				CFRelease(event);
			}
			if (shown < 5 || now_us - last_said >= 10000000) {
				shown++;
				last_said = now_us;
				say("hakios: motion: sample %u: raw %d %d %d mm/s2 (age %u ms) -> %.3f %.3f %.3f g, every %.0f us\n",
				    m.seq, m.accel_mm_s2[0], m.accel_mm_s2[1], m.accel_mm_s2[2], m.age_ms, x, y, z, interval);
			}
			if (mode >= 2) {
				motion_orientation(x, y, z, now_us);
			}
		}
		usleep((useconds_t)interval);
	}
}


/*
 * The thread: a client of the event server (backboardd, this very process) once the server is up -- after
 * libhakios's touch screen has had its turn -- then the motion loop.
 */
static void *
motion_thread(void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	int mode = (int)(intptr_t)arg;
	IOHIDEventSystemClientRef client = NULL;

	pthread_setname_np("hakios motion");
	mach_timebase_info(&timebase);
	if (access(VAYU_MOTION_DEVICE, F_OK) != 0) {
		/* a boot without the sensor core streaming: wait for the device first, quietly, before any client */
		for (unsigned waited = 0; access(VAYU_MOTION_DEVICE, F_OK) != 0; waited++) {
			if (waited == 0) {
				say("hakios: motion: %s: %s; trying every 2 s\n", VAYU_MOTION_DEVICE, strerror(errno));
			}
			if (waited >= 150) {
				say("hakios: motion: %s never came\n", VAYU_MOTION_DEVICE);
				return NULL;
			}
			sleep(2);
		}
	}
	sleep(10);                      /* libhakios's input thread makes its services 3 s in; ours after it */
	for (unsigned i = 0; client == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
	}
	if (client == NULL) {
		say("hakios: motion: no HID client\n");
		return NULL;
	}
	IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("hakios.motion", NULL));
	motion_run(client, mode);
	return NULL;
}

__attribute__((constructor))
static void
motion_start(void)
{
	char arguments[4096] = "";
	size_t size = sizeof(arguments) - 1;
	const char *option;
	int mode = 1;
	pthread_t thread;

	if (strcmp(getprogname(), "backboardd") != 0) {
		return;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && (option = strstr(arguments, "motion_orient=")) != NULL) {
		mode = atoi(option + strlen("motion_orient="));
	}
	say("hakios: motion: libhakios.motion in backboardd, motion_orient=%d\n", mode);
	if (mode > 0 && pthread_create(&thread, NULL, motion_thread, (void *)(intptr_t)mode) == 0) {
		pthread_detach(thread);
	}
}
