/*
 * VayuTouchPointer and VayuHIDButtons: the touch screen and the buttons of the POCO X3 Pro (vayu) as
 * HID services of macOS's own HID event system, for a macOS 13.4 user space on this kernel. Only with
 * boot-arg macos_hid=1; without it neither service exists and nothing below runs (vayu_hid_post()
 * returns at once), so iOS boots are as before.
 *
 * macOS has no touch interface; a Mac takes a pointer. On a Mac the path is IOHIDDevice (report
 * descriptor) -> IOHIDEventDriver -> IOHIDEventService -> IOHIDEventServiceUserClient -> the event
 * system in hidd -> WindowServer. This kernel loads no kexts and has no IOHIDFamily, so the two
 * services here are IOHIDEventServices (iokit/Kernel/hid/, IOHIDFamily-1915.120.2's user client,
 * queue and IOHIDEvent) that build the events IOHIDEventDriver would have built:
 *
 *   VayuTouchPointer   Generic Desktop / Mouse. Its samples are the ones every touch driver posts to
 *                      bsd/dev/hakios.input.c (vayu_nvt.c on the phone, virtio_input in QEMU, usbctl's
 *                      "oem touch"). The gesture state machine is iokit/Kernel/VayuTouchGesture.h:
 *                      the pointer goes where a finger lands, a tap clicks, a drag drags, a finger
 *                      held still is a secondary click, two fingers scroll, a two-finger tap is a
 *                      secondary click. In the default absolute mode each report is what
 *                      IOHIDEventService::dispatchAbsolutePointerEvent makes of an absolute tablet
 *                      (a QEMU usb-tablet, macOS-uke's descriptor) in 1915.120.2: an absolute pointer
 *                      event, 16.16 fractions of the display, the button mask, no old-button state.
 *   VayuHIDButtons     Consumer / Consumer Control: power (the side button) is Consumer Power, the
 *                      key macOS takes as its power key; volume up/down are Consumer Volume
 *                      Increment/Decrement keyboard events. Home (no such button on vayu) is ignored.
 *
 * Boot-args (all read once, at start):
 *   macos_hid=1          the two services; 0 / absent: none
 *   macos_hid_rot=N      how the desktop is turned on the upright 1080x2400 panel, clockwise: 0, 90
 *                        (default: the desktop's top along the panel's right edge, 2400x1080), 180, 270.
 *                        Must match the display's rotation.
 *   macos_hid_rel=1      trackpad mode: relative movement, tap-to-click at the pointer, tap and touch
 *                        again to drag (the fallback the Scion kernel needed on macOS 26.5, where its
 *                        absolute events reached WindowServer but did not move the pointer)
 *   macos_hid_slop=PX    travel below which a finger is held still (40 panel pixels, ~2.5 mm)
 *   macos_hid_long_ms=MS a finger held still this long is a secondary click (500)
 *   macos_hid_scroll=PX  panel pixels per scroll line (24); negative reverses the direction
 *   macos_hid_log=1      a line for every report, not only for clicks, drags and scrolls
 *   macos_hid_test=1     at start, a scripted tap, drag, long press, two-finger scroll, two-finger tap
 *                        and volume-up press through the same path, on script time rather than the
 *                        clock (QEMU's smoke run lasts under a second after start); ends with
 *                        "VayuTouchPointer: self-test PASS/FAIL". Nothing has the services open then,
 *                        so it clicks nothing.
 *
 * Origin. The idea and the device's shape -- the panel as an absolute one-button pointer over the
 * whole screen, transport "Virtual", a non-Apple vendor id -- are those of macOS-uke's
 * touch/src/package/UkeTouchPointer.cpp (github.com/iOSNJBrepo/macOS-uke, commit 3d27d4f,
 * SPDX-License-Identifier: BSD-2-Clause-Patent; the repository has no LICENSE file and the file names
 * no copyright holder). No code of it is copied: it is an IOHIDDevice with a report descriptor on top
 * of the real IOHIDFamily, which does not exist here, and it has neither gestures nor buttons. The
 * Mac-mouse registry properties are from the Scion kernel's SweetHID.cpp (APSL 2.0), whose comment
 * says WindowServer did not move the pointer without HIDPointerResolution.
 *
 * Not proven: that hidd/WindowServer of 13.4 open these services (look for "VayuTouchPointer: open
 * by IOHIDEventServiceUserClient" and "... N clients" in the gesture lines), that the entitlement
 * check of the user client passes for them ("not entitled" otherwise), and the sign of the scroll.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSArray.h>
#include <pexpert/pexpert.h>

#include "hid/IOHIDEventService.h"
#include "hid/IOHIDEvent.h"
#include "hid/IOHIDKeys.h"
#include "hid/IOHIDUsageTables.h"
#include "VayuTouchGesture.h"
#include "MiniDrivers.h"

extern "C" uint64_t mach_absolute_time(void);
extern "C" void hakios_touch_panel_size(uint32_t *width, uint32_t *height);

/* the record types of sys/hakios.input.h (not seen from iokit/) */
#define HAKIOS_TOUCH_DOWN       1
#define HAKIOS_TOUCH_MOVE       2
#define HAKIOS_TOUCH_UP         3
#define HAKIOS_BUTTON_DOWN      4
#define HAKIOS_BUTTON_UP        5
#define HAKIOS_BUTTON_LOCK      1
#define HAKIOS_BUTTON_VOLUME_UP 2
#define HAKIOS_BUTTON_VOLUME_DOWN 3

static bool gVayuHIDLog;
static bool gVayuHIDDig;        /* macos_hid_dig=1: the absolute reports as a pen digitizer's events (AQUA Q-M4) */
static uint32_t gVayuReports[4], gVayuKeys;     /* reports by kind, key events: for macos_hid_test */

static uint64_t
abs_to_us(uint64_t abs)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000;
}

class VayuTouchPointer : public IOHIDEventService {
	OSDeclareDefaultStructors(VayuTouchPointer);
public:
	bool configure(const struct vtg_config *config);
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t stamp);
	void tickAt(uint64_t stamp);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return gVayuHIDDig ? kHIDPage_Digitizer : kHIDPage_GenericDesktop; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return gVayuHIDDig ? kHIDUsage_Dig_Pen : kHIDUsage_GD_Mouse; }
	virtual OSString *getProduct() APPLE_KEXT_OVERRIDE { return OSString::withCString("vayu Touch Pointer"); }
	virtual UInt32 getVendorID() APPLE_KEXT_OVERRIDE { return 0x5555; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 0x7a33; }
	virtual UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return 4166; }        /* the panel's 240 Hz */
private:
	static void timerFired(thread_call_param_t self, thread_call_param_t);
	void deliver(const struct vtg_out *out, int count, uint64_t stamp);
	void rearm(void);

	IOLock *fLock;
	thread_call_t fTimer;
	struct vtg_state fGesture;
	uint64_t fTimerUs;              /* the deadline the timer is set for, 0 none */
	uint8_t fRelButtons;            /* trackpad mode: the buttons of the last relative report */
	uint8_t fAbsButtons;            /* the buttons of the last absolute report */
};
OSDefineMetaClassAndStructors(VayuTouchPointer, IOHIDEventService);

class VayuHIDButtons : public IOHIDEventService {
	OSDeclareDefaultStructors(VayuHIDButtons);
public:
	void key(uint32_t button, bool down, uint64_t stamp);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Consumer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Csmr_ConsumerControl; }
	virtual OSString *getProduct() APPLE_KEXT_OVERRIDE { return OSString::withCString("vayu Buttons"); }
	virtual UInt32 getVendorID() APPLE_KEXT_OVERRIDE { return 0x5555; }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 0x7a34; }
};
OSDefineMetaClassAndStructors(VayuHIDButtons, IOHIDEventService);

static VayuTouchPointer *gVayuPointer;
static VayuHIDButtons *gVayuButtons;

bool
VayuTouchPointer::configure(const struct vtg_config *config)
{
	fLock = IOLockAlloc();
	fTimer = thread_call_allocate(&VayuTouchPointer::timerFired, this);
	if (fLock == NULL || fTimer == NULL) {
		return false;
	}
	vtg_init(&fGesture, config);
	return true;
}

bool
VayuTouchPointer::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	/*
	 * A Mac mouse's usage pairs (mouse and pointer), and what a Mac's USB mouse service publishes for
	 * the pointer and scroll filters: the acceleration step scales by the resolution, and with none the
	 * Scion kernel's pointer never moved (SweetHID.cpp, boot 154332).
	 */
	OSArray *pairs = OSArray::withCapacity(2);
	static const UInt32 usages[] = { kHIDUsage_GD_Mouse, kHIDUsage_GD_Pointer };
	for (unsigned i = 0; pairs != NULL && i < 2; i++) {
		OSDictionary *pair = OSDictionary::withCapacity(2);
		OSNumber *page = OSNumber::withNumber(kHIDPage_GenericDesktop, 32);
		OSNumber *usage = OSNumber::withNumber(usages[i], 32);
		if (pair != NULL && page != NULL && usage != NULL) {
			pair->setObject(kIOHIDDeviceUsagePageKey, page);
			pair->setObject(kIOHIDDeviceUsageKey, usage);
			pairs->setObject(pair);
		}
		OSSafeReleaseNULL(pair);
		OSSafeReleaseNULL(page);
		OSSafeReleaseNULL(usage);
	}
	if (pairs != NULL) {
		setProperty(kIOHIDDeviceUsagePairsKey, pairs);
		pairs->release();
	}
	setProperty("HIDPointerResolution", (unsigned long long)(400 << 16), 32);
	setProperty("HIDScrollResolution", (unsigned long long)(400 << 16), 32);
	setProperty("HIDPointerAccelerationType", "HIDMouseAcceleration");
	setProperty("HIDScrollAccelerationType", "HIDMouseScrollAcceleration");
	setProperty("HIDPointerButtonCount", 2ULL, 32);
	setProperty("HIDPointerReportRate", 240ULL, 32);
	setProperty("VayuRotation", (unsigned long long)fGesture.cfg.rotation, 32);
	setProperty("VayuMode", fGesture.cfg.relative ? "trackpad" : "absolute");
	registerService();
	return true;
}

/* Reports -> IOHIDEvents, with fLock held (so that a timer's and a sample's reports keep their order). */
void
VayuTouchPointer::deliver(const struct vtg_out *out, int count, uint64_t stamp)
{
	for (int i = 0; i < count; i++) {
		IOHIDEvent *event = NULL;
		const struct vtg_out *o = &out[i];

		switch (o->kind) {
		case VTG_ABS:
			/*
			 * the buttons of the report before too: the event's button children are the bits that changed
			 * (IOHIDEvent.cpp), a down once and an up once, as the Scion kernel's SweetHID.cpp passes them
			 */
			if (gVayuHIDDig) {
				/*
				 * (AQUA Q-M4, macos_hid_dig=1) a pen in range over the panel, its tip down while the button is:
				 * QEMU r49/r52 showed 13.4's window server taking an absolute pointer event's 16.16 fractions as
				 * a relative move (the pointer crept to 2,6), where a tablet's digitizer events are placed
				 */
				event = IOHIDEvent::digitizerEvent(stamp, 0, kIOHIDDigitizerTransducerTypeStylus, true, o->buttons, o->a, o->b, 0,
				    (o->buttons & 1) ? 0x10000 : 0);
				if (event != NULL) {
					event->setIntegerValue(kIOHIDEventFieldDigitizerRange, 1);
					event->setIntegerValue(kIOHIDEventFieldDigitizerTouch, (o->buttons & 1) ? 1 : 0);
				}
			} else {
				event = IOHIDEvent::absolutePointerEvent(stamp, o->a, o->b, 0, o->buttons, fAbsButtons);
			}
			if (event != NULL) {
				fAbsButtons = o->buttons;
			}
			break;
		case VTG_REL:
			event = IOHIDEvent::relativePointerEvent(stamp, o->a, o->b, 0, o->buttons, fRelButtons);
			if (event != NULL) {
				fRelButtons = o->buttons;
			}
			break;
		case VTG_SCROLL:
			/* x horizontal, y vertical, as dispatchScrollWheelEventWithFixed hands them on */
			event = IOHIDEvent::scrollEvent(stamp, o->a, o->b, 0);
			break;
		}
		if (event == NULL) {
			continue;
		}
		gVayuReports[o->kind & 3]++;
		if (gVayuHIDLog) {
			IOLog("VayuTouchPointer: report %d buttons %u %d %d\n", o->kind, o->buttons, o->a, o->b);
		}
		dispatchEvent(event);
		event->release();
	}
}

/* With fLock held: the timer set for the next long press, or cancelled. */
void
VayuTouchPointer::rearm(void)
{
	uint64_t deadline_us = vtg_deadline(&fGesture), deadline;

	if (deadline_us == fTimerUs) {
		return;                 /* as it is: not touched again for every sample of a touch */
	}
	fTimerUs = deadline_us;
	if (deadline_us == 0) {
		thread_call_cancel(fTimer);
		return;
	}
	nanoseconds_to_absolutetime(deadline_us * 1000, &deadline);
	thread_call_enter_delayed(fTimer, deadline);
}

void
VayuTouchPointer::post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t stamp)
{
	static const char *const names[] = { "idle", "touch", "drag", "move", "scroll", "spent" };
	struct vtg_out out[VTG_OUT_MAX];
	int before, count;
	bool scrolled;

	IOLockLock(fLock);
	before = fGesture.state;
	scrolled = fGesture.scrolled;
	count = vtg_touch(&fGesture, type, finger, x, y, abs_to_us(stamp), out);
	deliver(out, count, stamp);
	rearm();
	/* a line for each step of a gesture, not for each sample */
	if (before == VTG_PENDING && fGesture.state == VTG_IDLE) {
		IOLog("VayuTouchPointer: tap = click (%u clients)\n", clientCount());
	} else if (before == VTG_SCROLLING && fGesture.state == VTG_IDLE && count != 0) {
		IOLog("VayuTouchPointer: two-finger tap = secondary click (%u clients)\n", clientCount());
	} else if (fGesture.state != before) {
		IOLog("VayuTouchPointer: %s -> %s, finger %u at %u,%u (%u clients)\n", names[before % 6],
		    names[fGesture.state % 6], finger, x, y, clientCount());
	} else if (fGesture.scrolled && !scrolled) {
		IOLog("VayuTouchPointer: scrolling (%u clients)\n", clientCount());
	}
	IOLockUnlock(fLock);
}

/* the passing of time up to stamp (mach_absolute_time): a long press that is due */
void
VayuTouchPointer::tickAt(uint64_t stamp)
{
	struct vtg_out out[VTG_OUT_MAX];
	int count;

	IOLockLock(fLock);
	fTimerUs = 0;                   /* fired: a deadline still due is set again below */
	count = vtg_tick(&fGesture, abs_to_us(stamp), out);
	deliver(out, count, stamp);
	if (count != 0) {
		IOLog("VayuTouchPointer: long press = secondary click (%u clients)\n", clientCount());
	}
	rearm();
	IOLockUnlock(fLock);
}

void
VayuTouchPointer::timerFired(thread_call_param_t param, thread_call_param_t)
{
	((VayuTouchPointer *)param)->tickAt(mach_absolute_time());
}

void
VayuHIDButtons::key(uint32_t button, bool down, uint64_t stamp)
{
	UInt32 usage;

	switch (button) {
	case HAKIOS_BUTTON_LOCK:        usage = kHIDUsage_Csmr_Power; break;
	case HAKIOS_BUTTON_VOLUME_UP:   usage = kHIDUsage_Csmr_VolumeIncrement; break;
	case HAKIOS_BUTTON_VOLUME_DOWN: usage = kHIDUsage_Csmr_VolumeDecrement; break;
	default:                        return;
	}
	IOHIDEvent *event = IOHIDEvent::keyboardEvent(stamp, kHIDPage_Consumer, usage, down);
	if (event != NULL) {
		gVayuKeys++;
		IOLog("VayuHIDButtons: consumer 0x%x %s (%u clients)\n", (unsigned)usage, down ? "down" : "up", clientCount());
		dispatchEvent(event);
		event->release();
	}
}

/* bsd/dev/hakios.input.c: every sample a touch or button driver posts (sys/hakios.input.h). */
extern "C" void
vayu_hid_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t stamp)
{
	if (type == HAKIOS_BUTTON_DOWN || type == HAKIOS_BUTTON_UP) {
		if (gVayuButtons != NULL) {
			gVayuButtons->key(finger, type == HAKIOS_BUTTON_DOWN, stamp);
		}
	} else if (gVayuPointer != NULL && type >= HAKIOS_TOUCH_DOWN && type <= HAKIOS_TOUCH_UP) {
		gVayuPointer->post(type, finger, x, y, stamp);
	}
}

/*
 * macos_hid_test=1: one of each gesture, as a driver would post them (finger, type, x, y in 0..65535
 * of the upright panel, then the pause before the next step). Expected with the defaults: a click,
 * a drag, a long-press secondary click, two scroll reports, a two-finger-tap secondary click and a
 * volume-up press and release.
 */
static const struct {
	uint16_t delay_ms;
	uint8_t type, finger;
	uint16_t x, y;
} gTestScript[] = {
	{ 0, HAKIOS_TOUCH_DOWN, 0, 32768, 32768 }, { 80, HAKIOS_TOUCH_UP, 0, 32768, 32768 },           /* tap */
	{ 400, HAKIOS_TOUCH_DOWN, 0, 20000, 20000 }, { 30, HAKIOS_TOUCH_MOVE, 0, 24000, 24000 },
	{ 30, HAKIOS_TOUCH_MOVE, 0, 28000, 28000 }, { 30, HAKIOS_TOUCH_UP, 0, 28000, 28000 },          /* drag */
	{ 400, HAKIOS_TOUCH_DOWN, 0, 40000, 40000 }, { 800, HAKIOS_TOUCH_UP, 0, 40000, 40000 },        /* long press */
	{ 400, HAKIOS_TOUCH_DOWN, 0, 30000, 30000 }, { 20, HAKIOS_TOUCH_DOWN, 1, 36000, 30000 },
	{ 30, HAKIOS_TOUCH_MOVE, 0, 34000, 30000 }, { 30, HAKIOS_TOUCH_MOVE, 0, 38000, 30000 },
	{ 30, HAKIOS_TOUCH_UP, 0, 38000, 30000 }, { 10, HAKIOS_TOUCH_UP, 1, 36000, 30000 },            /* scroll */
	{ 400, HAKIOS_TOUCH_DOWN, 0, 50000, 50000 }, { 20, HAKIOS_TOUCH_DOWN, 1, 52000, 50000 },
	{ 60, HAKIOS_TOUCH_UP, 0, 50000, 50000 }, { 10, HAKIOS_TOUCH_UP, 1, 52000, 50000 },            /* two-finger tap */
	{ 400, HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_VOLUME_UP, 0, 0 }, { 100, HAKIOS_BUTTON_UP, HAKIOS_BUTTON_VOLUME_UP, 0, 0 },
};
/*
 * Expected with the default settings (absolute mode): 15 absolute reports (tap 3, drag 5, long press 3,
 * scroll 1, two-finger tap 3), 2 scroll reports, 2 key events.
 */
static void
selfTest(bool absolute)
{
	uint64_t stamp = mach_absolute_time(), step;

	for (unsigned i = 0; i < sizeof(gTestScript) / sizeof(gTestScript[0]); i++) {
		nanoseconds_to_absolutetime((uint64_t)gTestScript[i].delay_ms * NSEC_PER_MSEC, &step);
		stamp += step;
		gVayuPointer->tickAt(stamp);    /* a long press that fell due during the pause */
		vayu_hid_post(gTestScript[i].type, gTestScript[i].finger, gTestScript[i].x, gTestScript[i].y, stamp);
	}
	bool pass = !absolute || (gVayuReports[VTG_ABS] == 15 && gVayuReports[VTG_SCROLL] == 2 && gVayuKeys == 2);
	IOLog("VayuTouchPointer: self-test %s: %u absolute, %u relative, %u scroll reports, %u keys%s\n",
	    pass ? "PASS" : "FAIL", gVayuReports[VTG_ABS], gVayuReports[VTG_REL], gVayuReports[VTG_SCROLL], gVayuKeys,
	    absolute ? " (want 15, 0, 2, 2)" : "");
}

/* under IOResources, as the real IOHIDFamily's resource-matched services are; false if it did not start */
static bool
attachAndStart(IOService *service, IOService *resources)
{
	if (!service->attach(resources)) {
		return false;
	}
	if (!service->start(resources)) {
		service->detach(resources);
		return false;
	}
	return true;            /* the reference is kept: the service lives as long as the kernel */
}

void
VayuTouchPointerStart(void)
{
	uint32_t on = 0, rot = 90, rel = 0, slop = 40, long_ms = 500, log = 0, test = 0;
	int32_t scroll = 24;
	IOService *resources = IOService::getResourceService();
	struct vtg_config config = { 0 };
	VayuTouchPointer *pointer;
	VayuHIDButtons *buttons;

	if (!PE_parse_boot_argn("macos_hid", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("macos_hid_rot", &rot, sizeof(rot));
	PE_parse_boot_argn("macos_hid_rel", &rel, sizeof(rel));
	PE_parse_boot_argn("macos_hid_slop", &slop, sizeof(slop));
	PE_parse_boot_argn("macos_hid_long_ms", &long_ms, sizeof(long_ms));
	PE_parse_boot_argn("macos_hid_scroll", &scroll, sizeof(scroll));
	PE_parse_boot_argn("macos_hid_log", &log, sizeof(log));
	PE_parse_boot_argn("macos_hid_test", &test, sizeof(test));
	{
		uint32_t dig = 0;

		PE_parse_boot_argn("macos_hid_dig", &dig, sizeof(dig));
		gVayuHIDDig = dig != 0;
	}
	if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
		IOLog("VayuTouchPointer: macos_hid_rot=%u is not 0/90/180/270, using 90\n", rot);
		rot = 90;
	}
	/* (scroll 0 would be a division by zero in the gesture machine; INT32_MIN has no positive) */
	if (slop == 0 || slop > 1000) {
		IOLog("VayuTouchPointer: macos_hid_slop=%u is not 1..1000, using 40\n", slop);
		slop = 40;
	}
	if (long_ms < 50 || long_ms > 10000) {
		IOLog("VayuTouchPointer: macos_hid_long_ms=%u is not 50..10000, using 500\n", long_ms);
		long_ms = 500;
	}
	if (scroll == 0 || scroll < -2400 || scroll > 2400) {
		IOLog("VayuTouchPointer: macos_hid_scroll=%d is not +-1..2400, using 24\n", scroll);
		scroll = 24;
	}
	gVayuHIDLog = log != 0 || test != 0;
	hakios_touch_panel_size(&config.panel_w, &config.panel_h);
	config.rotation = rot;
	config.relative = rel != 0;
	config.slop_px = (int32_t)slop;
	config.long_us = (uint64_t)long_ms * 1000;
	config.double_us = 300000;
	config.scroll_px = scroll;

	if (resources == NULL) {
		return;
	}
	pointer = OSTypeAlloc(VayuTouchPointer);
	if (pointer != NULL && pointer->init() && pointer->configure(&config) && attachAndStart(pointer, resources)) {
		gVayuPointer = pointer;
	} else {
		OSSafeReleaseNULL(pointer);
	}
	buttons = OSTypeAlloc(VayuHIDButtons);
	if (buttons != NULL && buttons->init() && attachAndStart(buttons, resources)) {
		gVayuButtons = buttons;
	} else {
		OSSafeReleaseNULL(buttons);
	}
	IOLog("VayuTouchPointer: %s, %s mode, %ux%u panel turned %u, slop %d px, long press %u ms, scroll %d px/line; "
	    "buttons %s\n", gVayuPointer != NULL ? "started" : "FAILED", config.relative ? "trackpad" : "absolute",
	    config.panel_w, config.panel_h, rot, config.slop_px, long_ms, scroll, gVayuButtons != NULL ? "started" : "FAILED");

	if (test != 0 && gVayuPointer != NULL && gVayuButtons != NULL) {
		selfTest(!config.relative);
	}
}
