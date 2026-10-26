/*
 * libhakios.sensors.dylib, inserted into backboardd beside libhakios.dylib and libhakios.motion.dylib
 * (etc/inject.conf): the phone's ambient-light and proximity sensors to iOS, as two virtual HID
 * services of the event server that backboardd is.
 *
 * Why these exact services (iOS 16.5, iPhone10,3, 20F66; the evidence is in the task report):
 *
 *   ambient light  CoreBrightness (loaded by backboardd for the display) matches its HID services by
 *                  "DeviceUsagePage"/"DeviceUsage" pairs, one of which is (0x20, 0x41): the HID
 *                  Sensor page's Light: Ambient Light usage. It then sets the service's
 *                  "IOHIDALSContinuousMode" to 1 (to be fed a stream rather than changes) and builds
 *                  CBALSEvents out of the IOHIDEventTypeAmbientLightSensor events (type 12). The
 *                  brightness side already exists in the kernel: MiniIOMobileFramebuffer.cpp's
 *                  AppleARMBacklight.
 *
 *   proximity      backboardd's UI-sensor table has one configuration with the matching dictionary
 *                  {"DeviceUsagePage": 0xFF00, "DeviceUsage": 0x08}; its wrapper block reads
 *                  "Placement" (must be 1, the top placement, or the service is ignored) and
 *                  "NeedsHostState" (0 -> BKHIDUIDiscreteProximityServiceWrapper, 1 ->
 *                  BKHIDUIHostStateProximityServiceWrapper). The state itself travels as events of
 *                  type 14: BKHIDProximityEventProcessor reads field 0xE0000 (the detection mask,
 *                  non-zero = object within proximity) and posts
 *                  com.apple.backboard.hid.proximity; SpringBoard's SBProximitySensorManager and
 *                  SBProximityBacklightPolicy* turn the screen off from there. The mask bit that
 *                  reaches clients is the Receiver bit, 0x40 (the names of the bits are in
 *                  backboardd's _BKSNSStringFromIOHIDProximityDetectionMask).
 *
 * The kernel's side is /dev/vayu-als and /dev/vayu-prox (sys/vayu_ssc.h), one small struct per
 * sensor, with /dev/vayu-motion v2 as the fallback for a kernel without them. A missing device
 * (the sensor core did not come up: vayu_slpi=6 without vayu_ssc_als=1) is not an error: the
 * library says so once and creates nothing, so a phone without the streams boots as before.
 *
 * Boot args: sensors_hid=0 (both off), als_hid=0, prox_hid=0 (one off), prox_mask_near=N (the
 * mask for "near", default 0x40 = Receiver). Default: both sensors on.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
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

/* ---- the HID event system, as much of it as is used (no header for it in the iOS SDK; as inject.c) ---- */

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
extern IOHIDEventRef IOHIDEventCreateAmbientLightSensorEvent(CFAllocatorRef allocator, uint64_t time, uint32_t lux, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateProximtyEvent(CFAllocatorRef allocator, uint64_t time, uint32_t mask, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3
#define FIELD_IS_BUILT_IN               0x4
#define EVENT_TYPE_ALS                  12      /* kIOHIDEventTypeAmbientLightSensor */
#define EVENT_TYPE_PROXIMITY            14      /* kIOHIDEventTypeProximity */

/* What iOS matches on. Apple's own constants: HID Sensors page 0x20, Light: Ambient Light 0x41
 * (IOHIDUsageTables.h: kHIDPage_Sensor, kHIDUsage_Snsr_Light_AmbientLight); the vendor page's
 * usage 8 is what backboardd's proximity UI-sensor configuration asks for. */
#define HID_PAGE_SENSOR                 0x20
#define HID_USAGE_AMBIENT_LIGHT         0x41
#define HID_PAGE_APPLE_VENDOR           0xff00
#define HID_USAGE_APPLE_PROXIMITY       0x08

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

/* ---- the kernel's samples (sys/vayu_ssc.h) ------------------------------------------------ */

#define VAYU_ALS_DEVICE         "/dev/vayu-als"
#define VAYU_PROX_DEVICE        "/dev/vayu-prox"
#define VAYU_MOTION_DEVICE      "/dev/vayu-motion"

#define VAYU_ALS_VERSION        1
struct vayu_als {
	uint32_t version, seq;          /* seq 0: none yet */
	int32_t lux_milli;
	uint32_t age_ms;
};

#define VAYU_PROX_VERSION       1
struct vayu_prox {
	uint32_t version, seq;
	int32_t near;                   /* 1 near, 0 far, -1 unknown */
	int32_t distance;
	uint32_t age_ms;
};

/* /dev/vayu-motion v2 (the fallback: a kernel before the two nodes above), the same fields. */
#define VAYU_MOTION_VERSION     2
struct vayu_motion {
	uint32_t version, seq;
	int32_t accel_mm_s2[3];
	uint32_t age_ms;
	uint32_t light_seq;
	int32_t lux_milli;
	uint32_t light_age_ms;
	uint32_t prox_seq;
	int32_t prox_near;
	int32_t prox_distance;
	uint32_t prox_age_ms;
};

/* One sensor's device: its own node, or /dev/vayu-motion v2 when the node is not there. */
struct source {
	int fd;
	bool combined;
};

static bool
source_open(struct source *s, const char *device)
{
	struct vayu_motion m;

	s->fd = -1;
	s->combined = false;
	if ((s->fd = open(device, O_RDONLY)) >= 0) {
		return true;
	}
	if (strcmp(device, VAYU_MOTION_DEVICE) == 0) {
		return false;
	}
	if ((s->fd = open(VAYU_MOTION_DEVICE, O_RDONLY)) < 0) {
		return false;
	}
	if (read(s->fd, &m, sizeof(m)) != (ssize_t)sizeof(m) || m.version < VAYU_MOTION_VERSION) {
		/* v1 (or nothing yet): no light or proximity fields at all */
		close(s->fd);
		s->fd = -1;
		return false;
	}
	s->combined = true;
	say("hakios: sensors: %s is not there; %s v%u is the source\n", device, VAYU_MOTION_DEVICE, m.version);
	return true;
}

/* Read the sensor's latest sample; false when the device has nothing new or is not open. */
static bool
source_als(struct source *s, struct vayu_als *out)
{
	struct vayu_motion m;

	if (s->fd < 0) {
		return false;
	}
	if (s->combined) {
		if (read(s->fd, &m, sizeof(m)) != (ssize_t)sizeof(m) || m.version < VAYU_MOTION_VERSION) {
			return false;
		}
		out->version = VAYU_ALS_VERSION;
		out->seq = m.light_seq;
		out->lux_milli = m.lux_milli;
		out->age_ms = m.light_age_ms;
		return true;
	}
	return read(s->fd, out, sizeof(*out)) == (ssize_t)sizeof(*out) && out->version == VAYU_ALS_VERSION;
}

static bool
source_prox(struct source *s, struct vayu_prox *out)
{
	struct vayu_motion m;

	if (s->fd < 0) {
		return false;
	}
	if (s->combined) {
		if (read(s->fd, &m, sizeof(m)) != (ssize_t)sizeof(m) || m.version < VAYU_MOTION_VERSION) {
			return false;
		}
		out->version = VAYU_PROX_VERSION;
		out->seq = m.prox_seq;
		out->near = m.prox_near;
		out->distance = m.prox_distance;
		out->age_ms = m.prox_age_ms;
		return true;
	}
	return read(s->fd, out, sizeof(*out)) == (ssize_t)sizeof(*out) && out->version == VAYU_PROX_VERSION;
}

/* ---- the virtual services ------------------------------------------------------------------ */

/* The proximity detection mask sent when the object is near: Apple's "Receiver" bit (0x40); see
 * send_prox(). prox_mask_near=N in the boot args overrides it for the bench. */
static uint32_t prox_near_mask = 0x40;

struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;           /* the event server set DeviceOpenedByEventSystem */
	volatile bool continuous;       /* iOS set IOHIDALSContinuousMode (ambient light only) */
	volatile double interval_us;    /* ReportInterval, from the event server; 0 = not said */
	IOHIDEventRef last_event;       /* the latest event sent, retained; copyEvent hands it back */
	pthread_mutex_t lock;           /* last_event */
};

static bool
cf_bool(CFTypeRef value)
{
	if (value == NULL) {
		return false;
	}
	if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		return CFBooleanGetValue(value);
	}
	if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		int n = 0;

		CFNumberGetValue(value, kCFNumberIntType, &n);
		return n != 0;
	}
	return false;
}

static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	return value ? CFRetain(value) : NULL;
}

/* What the event server tells a service: opened; how often it wants samples (us, a double); and
 * the proximity/ALS knobs iOS sets (all logged, the ones that change what we send acted on). */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	struct service *made = refcon;
	static unsigned told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	if (key == NULL || value == NULL) {
		return true;
	}
	if (CFEqual(key, CFSTR("DeviceOpenedByEventSystem"))) {
		if (cf_bool(value)) {
			made->opened = true;
		}
	} else if (CFEqual(key, CFSTR("ReportInterval")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		double interval = 0;

		CFNumberGetValue(value, kCFNumberDoubleType, &interval);
		if (interval > 0) {
			made->interval_us = interval;
		}
	} else if (CFEqual(key, CFSTR("IOHIDALSContinuousMode"))) {
		made->continuous = cf_bool(value);
	} else if (CFEqual(key, CFSTR("DetectionMode")) || CFEqual(key, CFSTR("EnableDebugMode")) ||
	    CFEqual(key, CFSTR("ReceiverProximityReset")) || CFEqual(key, CFSTR("IOHIDALSTestMode"))) {
		/* the proximity wrapper's mode, the prox debug switch, its calibration reset, ALS test mode:
		 * nothing to do but say it (the first ones only; the event server repeats them) */
	} else if (told >= 40) {
		return true;
	}
	if (told++ < 60) {
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = CFCopyDescription(value);
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: sensors: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

/*
 * IOHIDServiceClientCopyEvent: CoreBrightness asks a service for its current event of a type
 * (the ALS one, type 12) instead of waiting for the next one. Handing back the last event we sent
 * makes the first brightness decision immediate.
 */
static IOHIDEventRef
service_copy_event(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, uint32_t type,
    __unused IOHIDEventRef matching, __unused uint32_t options)
{
	struct service *made = refcon;
	IOHIDEventRef event = NULL;

	if (type != EVENT_TYPE_ALS && type != EVENT_TYPE_PROXIMITY) {
		return NULL;
	}
	pthread_mutex_lock(&made->lock);
	if (made->last_event != NULL) {
		event = (IOHIDEventRef)CFRetain(made->last_event);
	}
	pthread_mutex_unlock(&made->lock);
	return event;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property, .copyEvent = service_copy_event,
};

static void
service_sent(struct service *made, IOHIDEventRef event)
{
	pthread_mutex_lock(&made->lock);
	if (made->last_event != NULL) {
		CFRelease(made->last_event);
	}
	made->last_event = (IOHIDEventRef)CFRetain(event);
	pthread_mutex_unlock(&made->lock);
}

/* A built-in HID service with one usage (and that usage as its device pair), made again until the
 * event server has opened it (as inject.c's make_service and motion.c's). */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, struct service *made, const char *product, int page, int usage,
    CFDictionaryRef extra)
{
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	made->properties = properties;
	pthread_mutex_init(&made->lock, NULL);
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
	if (extra != NULL) {
		CFIndex i, n = CFDictionaryGetCount(extra);
		const void **keys = calloc((size_t)n, sizeof(*keys)), **values = calloc((size_t)n, sizeof(*values));

		if (keys != NULL && values != NULL) {
			CFDictionaryGetKeysAndValues(extra, keys, values);
			for (i = 0; i < n; i++) {
				CFDictionarySetValue(properties, keys[i], values[i]);
			}
		}
		free(keys);
		free(values);
	}
	CFRelease(pairs);
	CFRelease(pair);
	CFRelease(name);
	CFRelease(page_number);
	CFRelease(usage_number);
	CFRelease(zero);
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
		say("hakios: sensors: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* The properties only the proximity service carries: iOS looks at both before it will use it. */
static CFMutableDictionaryRef
proximity_extras(void)
{
	CFMutableDictionaryRef extra = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFNumberRef one = number(1);

	/* backboardd's UI-sensor wrapper block: "Placement" intValue must be 1 (top placement) or the
	 * service is logged as "not top placement" and no wrapper is made; "NeedsHostState" false picks
	 * BKHIDUIDiscreteProximityServiceWrapper (the sensor reports by itself, by events). */
	CFDictionarySetValue(extra, CFSTR("Placement"), one);
	CFDictionarySetValue(extra, CFSTR("NeedsHostState"), kCFBooleanFalse);
	CFRelease(one);
	return extra;
}

static void
send_als(struct service *made, IOHIDServiceClientRef service, int32_t lux_milli)
{
	uint64_t now = mach_absolute_time();
	uint32_t lux = lux_milli <= 0 ? 0 : (uint32_t)((lux_milli + 500) / 1000);
	IOHIDEventRef event = IOHIDEventCreateAmbientLightSensorEvent(NULL, now, lux, 0);

	if (event == NULL) {
		return;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(service, event);
	service_sent(made, event);
	CFRelease(event);
}

static void
send_prox(struct service *made, IOHIDServiceClientRef service, bool near, int32_t distance)
{
	uint64_t now = mach_absolute_time();
	/* The detection mask is Apple's proximity mask: backboardd's own
	 * _BKSNSStringFromIOHIDProximityDetectionMask names the bits (1 LargeBodyContact,
	 * 2 LargeBodyFarField, 4 IrregularObjects, 8 EdgeStraddling, 16 FlatFingerClasp, 32
	 * FingerTouch, 64 Receiver, 128 SmallObjectsHovering, 256 ReceiverCrude, 512
	 * ReceiverMonitoring, 1024 ReceiverCrudeMonitoring), and BKHIDProximityEventProcessor posts
	 * the Receiver bit (0x40) on to clients: "near" is 0x40, "far" is 0. It treats any non-zero
	 * mask as object-within-proximity, so the bit choice only matters for what clients see.
	 * prox_mask_near=N overrides 0x40 on the bench. */
	uint32_t mask = near ? prox_near_mask : 0x0;
	IOHIDEventRef event = IOHIDEventCreateProximtyEvent(NULL, now, mask, 0);

	if (event == NULL) {
		return;
	}
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(service, event);
	service_sent(made, event);
	CFRelease(event);
	say("hakios: sensors: proximity %s (mask %#x, distance %d)\n", near ? "NEAR" : "far", mask, distance);
}

/* ---- the run ------------------------------------------------------------------------------- */

struct run {
	struct service als, prox;
	IOHIDServiceClientRef als_service, prox_service;
	struct source als_source, prox_source;
};

static struct run run;          /* the thread's, for the life of the process */

static void
sensors_run(struct run *r, IOHIDEventSystemClientRef client, bool want_als, bool want_prox)
{
	uint32_t last_als_seq = 0, last_prox_seq = 0;
	int32_t last_near = -1;
	unsigned als_shown = 0;
	uint64_t last_als_at = 0, last_said = 0;
	CFMutableDictionaryRef extra = proximity_extras();

	if (want_als) {
		r->als_service = make_service(client, &r->als, "Hakios ambient light", HID_PAGE_SENSOR,
		    HID_USAGE_AMBIENT_LIGHT, NULL);
	}
	if (want_prox) {
		r->prox_service = make_service(client, &r->prox, "Hakios proximity", HID_PAGE_APPLE_VENDOR,
		    HID_USAGE_APPLE_PROXIMITY, extra);
	}
	CFRelease(extra);
	say("hakios: sensors: services %p (ALS 0x%x/0x%x) %p (proximity 0x%x/0x%x), sources %d/%d\n",
	    (void *)r->als_service, HID_PAGE_SENSOR, HID_USAGE_AMBIENT_LIGHT, (void *)r->prox_service,
	    HID_PAGE_APPLE_VENDOR, HID_USAGE_APPLE_PROXIMITY, r->als_source.fd, r->prox_source.fd);
	for (;;) {
		uint64_t now = mach_absolute_time(), now_us = micros(now);
		struct vayu_als a;
		struct vayu_prox p;

		if (r->als_service != NULL && source_als(&r->als_source, &a) && a.seq != 0 && a.seq != last_als_seq) {
			/* the event server's ReportInterval if it asked for one, else the sensor's 5 Hz;
			 * never faster than 10 ms or slower than 1 s */
			double interval = r->als.interval_us > 0 ? r->als.interval_us : 200000;

			last_als_seq = a.seq;
			if (interval < 10000) interval = 10000;
			if (interval > 1000000) interval = 1000000;
			if (now_us - last_als_at >= (uint64_t)interval) {
				last_als_at = now_us;
				send_als(&r->als, r->als_service, a.lux_milli);
				if (als_shown < 5 || now_us - last_said >= 30000000) {
					als_shown++;
					last_said = now_us;
					say("hakios: sensors: ambient light #%u: %d milli-lux (age %u ms), every %.0f us, continuous %d\n",
					    a.seq, a.lux_milli, a.age_ms, interval, r->als.continuous);
				}
			}
		}
		if (r->prox_service != NULL && source_prox(&r->prox_source, &p) && p.seq != 0 && p.seq != last_prox_seq) {
			last_prox_seq = p.seq;
			if (p.near >= 0 && p.near != last_near) {
				last_near = p.near;
				send_prox(&r->prox, r->prox_service, p.near == 1, p.distance);
			}
		}
		usleep(20000);
	}
}

/* The thread: after the event server is up (libhakios's input and motion threads have had their
 * turn), open the sensors and make the services; a sensor that is not there is skipped. */
static void *
sensors_thread(void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	bool want_als = ((int)(intptr_t)arg & 1) != 0, want_prox = ((int)(intptr_t)arg & 2) != 0;
	struct run *r = &run;
	IOHIDEventSystemClientRef client = NULL;

	pthread_setname_np("hakios sensors");
	mach_timebase_info(&timebase);
	sleep(12);                      /* libhakios's input at 3 s, libhakios.motion at 10 s */
	if (want_als) {
		want_als = source_open(&r->als_source, VAYU_ALS_DEVICE);
		if (!want_als) {
			say("hakios: sensors: no ambient-light source (%s: %s)\n", VAYU_ALS_DEVICE, strerror(errno));
		}
	}
	if (want_prox) {
		want_prox = source_open(&r->prox_source, VAYU_PROX_DEVICE);
		if (!want_prox) {
			say("hakios: sensors: no proximity source (%s: %s)\n", VAYU_PROX_DEVICE, strerror(errno));
		}
	}
	if (!want_als && !want_prox) {
		return NULL;
	}
	for (unsigned i = 0; client == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
	}
	if (client == NULL) {
		say("hakios: sensors: no HID client\n");
		return NULL;
	}
	IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("hakios.sensors", NULL));
	sensors_run(r, client, want_als, want_prox);
	return NULL;
}

__attribute__((constructor))
static void
sensors_start(void)
{
	char arguments[4096] = "";
	size_t size = sizeof(arguments) - 1;
	const char *option;
	int mask = 3;
	pthread_t thread;

	if (strcmp(getprogname(), "backboardd") != 0) {
		return;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		if ((option = strstr(arguments, "sensors_hid=")) != NULL && atoi(option + strlen("sensors_hid=")) == 0) {
			mask = 0;
		}
		if ((option = strstr(arguments, "als_hid=")) != NULL && atoi(option + strlen("als_hid=")) == 0) {
			mask &= ~1;
		}
		if ((option = strstr(arguments, "prox_hid=")) != NULL && atoi(option + strlen("prox_hid=")) == 0) {
			mask &= ~2;
		}
		if ((option = strstr(arguments, "prox_mask_near=")) != NULL) {
			unsigned long value = strtoul(option + strlen("prox_mask_near="), NULL, 0);

			if (value != 0 && value <= 0xffff) {
				prox_near_mask = (uint32_t)value;
			}
		}
	}
	say("hakios: sensors: libhakios.sensors in backboardd, sensors=%d (ALS %d, proximity %d), prox mask %#x\n",
	    mask, (mask & 1) != 0, (mask & 2) != 0, prox_near_mask);
	if (mask != 0 && pthread_create(&thread, NULL, sensors_thread, (void *)(intptr_t)mask) == 0) {
		pthread_detach(thread);
	}
}
