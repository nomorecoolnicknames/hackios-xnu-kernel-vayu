/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include <os/atomic_private.h>
extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void hakios_tl(const char * format, ...) __printflike(1, 2);
static void fbLogProperty(const char * who, const OSSymbol * key, OSObject * value);

extern "C" void loader_log_putc(char c);
/*
 * Every line of this driver also goes to the loader's log, which is the one a run's log keeps: IOLog
 * alone does not reach it. (The TEST5Q run -- 73 boots, 2823889 lines, artifacts/vayu/test5-20261027 --
 * has not one line of this driver in it, while the same boot's printf lines from bsd/dev/ are all
 * there: "vayu: ...", "vayu-vsync: ...". A frame that comes out blank cannot be seen in a log that
 * the driver of the panel does not write to.) The same way MiniAppleStockholm.cpp and vayu_vsync.c do it.
 */
static void fbLog(const char * format, ...) __printflike(1, 2);

static void
fbLog(const char * format, ...)
{
	char line[1024];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char * c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	(IOLog)("%s", line);                    /* (in parentheses: IOLog is this, below) */
}
#define IOLog(...)      fbLog(__VA_ARGS__)

/*
 * The panel's backlight, if a driver has one (vayu: vayu_wled.c sets it with vayu_wled=2): called with false when
 * iOS asks for the display to be powered down (RequestPowerChange, in[0] == 0) and true when it asks for it back.
 */
extern "C" { bool (*hakios_display_power)(bool on) = NULL; }
/*
 * The panel's own frame timing, if a driver can read it (vayu: vayu_vsync.c with vayu_vsync=2, from
 * the display controller's line counter): the absolute time of the next frame start, and a frame's
 * length. Without them refreshes are a timer at fb_hz, which beats against the panel's real rate.
 *
 * Boot-arg fb_120=1: the display is the panel's 120 Hz one, and iOS is told so. The mode's SyncRate
 * (TimingElements, publishModes()) and the refresh timer are 120, a refresh is told to iOS every
 * panel frame (no adaptive divisor of 2), and a frame is put on the panel as rows copied (the middle
 * 1080x2400 of the picture, as fb_fit=1) rather than scaled: a scaled frame costs 5.5-7.9 ms of the
 * 8.33 ms a 120 Hz frame has.
 *
 * That is this driver's half, and it was already done when a tester said 120 Hz was not visible:
 * a test run had fb_hz=120 vayu_vsync=2 fb_vsync_div=1 fb_fit=1 fb_threads=6, and 48-59 frames a
 * second with 292 of 300 intervals between 10 and 18 ms -- a steady 60, while the copy cost 1.0 ms
 * and the GPU 0.7-7.7 ms. A test run had the panel's timing armed as well, and the same
 * 301-302 frames in 5005-5013 ms: every one of ~300 swaps a window came more than a 120 Hz period
 * after the one before. Nothing in this driver was the ceiling: the window server asked for 60
 * frames a second. CoreAnimation keeps a 16.7 ms floor (its "minimum frame duration") on every
 * display that is not a ProMotion one, and an iPhone X's identity is not; the port lifts it from
 * user space, from this same boot-arg: src/inject.c (backboardd) and src/inject_sb.c (SpringBoard)
 * answer CoreAnimation's CADisableMinimumFrameDuration[OnPhone] Info.plist key with true (the key
 * is read through CFBundleGetValueForInfoDictionaryKey, not getenv; see inject.c). What this driver
 * can do is tell the refreshes at the panel's rate and say in the log how many it told, so the next
 * run shows whether the compositor took them.
 */
extern "C" { uint64_t (*hakios_vsync_next)(uint64_t now) = NULL; uint64_t (*hakios_vsync_frame)(void) = NULL; }
/* the panel's brightness, 1..1000 of its full scale, if a driver has one (vayu: vayu_wled.c, vayu_wled=2) */
extern "C" { bool (*hakios_display_brightness)(uint32_t permille) = NULL; }

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz (or fb_120) says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define FRAME_LATE_US           12000                   /* a frame this late did not come on a 120 or an 80 Hz beat */
#define FENCE_LONG_US           4000                    /* a wait for the GPU that a 120 Hz frame cannot spare */
#define COPY_LONG_US            3000                    /* ... and a copy that cannot */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_FENCE_MS           20                      /* the swap's own fence (a submission of anybody's before it) */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
/*
 * Boot-arg fb_fit=1: the picture is not scaled to the panel but put on it pixel for pixel, its middle
 * 1080 x 2400 of 1125 x 2436 (22 columns cut on the left, 23 on the right, 18 rows top and bottom). A
 * frame is then rows copied, not every pixel worked out from four (5.5-7.9 ms a frame scaled, run 194827:
 * too slow for 120 a second). Touches are mapped to match (hakios_display_crop, hakios.input.c).
 */
static TUNABLE(uint32_t, gFit, "fb_fit", 0);
/*
 * Boot-arg fb_crop=1 (default 0: unchanged): a picture larger than the panel by no more than 64
 * pixels each way is put on the panel pixel for pixel from its middle -- iOS's 1125x2436 surface on
 * the 1080x2400 panel, 22 columns and 18 rows cropped -- instead of being scaled. drawRows() then
 * takes its "unscaled" path: rows memcpy'd, not a blend and a column lookup for every pixel. This
 * is fb_fit's crop without changing the size the display reports (fb_fit needs fbwidth/fbheight for
 * that); touch stays 1:1 with the panel, as the shown picture is.
 */
static TUNABLE(uint32_t, gCrop, "fb_crop", 0);
/*
 * fb_zero=1 (vayu, with vayu_mdp=1 and the loader's fb_spare=N): a frame that is one 1125x2436 BGRA surface of
 * MiniIOSurface's scanout pool is not copied: the display controller's VIG0 is pointed at it (its centre 1080x2400,
 * vayu_mdp_scanout) and the flip is taken at the panel's next frame start. Anything else is copied as before, after
 * VIG0 is put back on the boot framebuffer. The surface shown is held until the next flip has been taken.
 */
static TUNABLE(uint32_t, gFbZero, "fb_zero", 0);
extern "C" bool vayu_mdp_scanout(uint64_t phys, uint32_t stride, uint32_t x, uint32_t y);
extern "C" void vayu_mdp_unscanout(void);
/* for the touch screen with fb_fit=1: { left, top, picture width, picture height, panel width, panel height } */
extern "C" { uint32_t hakios_display_crop[6] = { 0, 0, 0, 0, 0, 0 }; }
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * With the panel's timing (vayu_vsync=2): refreshes told to iOS every N panel frames. 0 = by the drawing:
 * the framebuffer's own copy takes 5.5-7.9 ms a frame here (run 194827: 49-56 frames a second, gaps of 10
 * to 35 ms), so at 120 Hz frames land on every first or second refresh at random -- the jitter the owner
 * sees. A steady 60 is smoother than a ragged 55: every second frame while the copy takes more than 60%
 * of a frame, back to every frame below 45%. fb_120=1 forces 1 (a refresh every panel frame).
 */
static TUNABLE(uint32_t, gVsyncDivisor, "fb_vsync_div", 0);
/*
 * Boot-arg fb_120=1: iOS is told the display is the panel's 120 Hz one -- the mode's SyncRate, the
 * refresh timer and the refreshes themselves (see the top of this file). Off by default: the display
 * has been an iPhone X's 60 to iOS since the port began, and a window server with the software
 * renderer cannot fill 120 frames anyway.
 */
static TUNABLE(uint32_t, gFb120, "fb_120", 0);
/*
 * Boot-arg fb_swap_fence_ms=N (default 20): how long a swap waits for the GPU's last submission as of
 * the SwapEnd that handed it over. The surfaces of a swap have fences of their own (the last submission
 * that named them, MiniIOSurfaceDrawn) -- but the submission that ended the frame need not name the
 * surface the frame is in (the GPU's blit into a mirror texture, freedreno's merged batches:
 * userland/src/eagl.c), and the surface's fence then belongs to the frame before: the copy would read
 * pixels the core is drawing into. Short, because a frame given to the GPU after the swap is not this
 * frame's to wait for; it is a floor under the per-surface wait (SWAP_GPU_MS), not a wait for everything.
 */
static TUNABLE(uint32_t, gSwapFenceMs, "fb_swap_fence_ms", SWAP_FENCE_MS);
/*
 * Boot-arg fb_budget=0: no per-frame accounting of the frame's own three times (see frameBudget()).
 * It is a few adds a frame, and on by default: "the frames come late -- whose?" is the question this
 * driver kept being asked by its own log and could not answer. The five-second line it feeds says,
 * of the frames that came later than FRAME_LATE_US after the one before, how many waited over
 * FENCE_LONG_US for the GPU's fence and how many spent over COPY_LONG_US in the copy: a late frame
 * with neither is one nothing asked for (the window server's own cadence), and no change here can
 * make it come sooner.
 */
static TUNABLE(uint32_t, gBudget, "fb_budget", 1);
/*
 * Boot-arg fb_gpu_read=1 (default 0): before a surface is copied to the panel, its pixels are taken into
 * this processor's view (MiniIOSurfaceToCpu: clean and invalidate, 11 MB of a 1125x2436 frame, some 1 to
 * 2 ms). The GPU's submission path does that before the core is given a surface (hakios.gpu.cpp submit),
 * which is enough when one submission a frame names it; this is for when that one was skipped (the 4 ms
 * GPU_PUSH_AGAIN_MS of the same file) and the copy reads the frame before out of the cache.
 */
static TUNABLE(uint32_t, gGpuRead, "fb_gpu_read", 0);
/*
 * Boot-arg fb_120_idle=1 (with fb_120=1): 120 Hz only while the screen is touched. The refreshes told to iOS come
 * every panel frame (120 a second) while a finger is down or was within fb_120_idle_ms (default 1000) ms, and every
 * second panel frame (60 a second) after that: CoreAnimation's frames follow the refreshes, so the window server
 * draws 60 where nothing is being touched (video, clock, an animation that runs on by itself) and the heat of the
 * doubled frame work is spent only where the eye can see it. The mode iOS is told stays 120 (no mode switch).
 */
static TUNABLE(uint32_t, gFb120Idle, "fb_120_idle", 0);
static TUNABLE(uint32_t, gFb120IdleMs, "fb_120_idle_ms", 1000);
static bool gFb120Slow;                         /* the refreshes are now every 2nd panel frame (fb_120_idle) */
static uint32_t gFb120Changes;                  /* ... and how often that changed, for the log */
extern "C" uint64_t hakios_touch_idle_abs(void);       /* bsd/dev/hakios.input.c */
static uint32_t gModeHz;                        /* the refresh the display is said to run at (fb_120: 120) */
static bool gPictureFit;                        /* rows copied, not scaled: fb_fit, or a 120 Hz frame's cost */
static uint32_t gVSyncsTold;                    /* refreshes told to user space since the last 5 s line (atomic) */
static uint64_t gFBDrawAbs;                     /* the copy's time per frame, a running average */
static uint32_t gFBDivisor = 1;
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/hakios.input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	uint32_t hz = gModeHz != 0 ? gModeHz : gRefreshHz;

	return hz >= 1 && hz <= 240 ? hz : kFBRefreshHz;
}
/*
 * Is a frame being put on the panel just now? bsd/dev/vayu_adreno.c asks this (hakios_display_frames)
 * before it votes the GPU down: the vote is a round trip to the GMU, the first frames after it are
 * drawn at the lower speed, and its own messages go to the serial console from the middle of a
 * submission (hfi_send). A frame shown in the last 100 ms says that something is moving on the screen.
 */
extern "C" uint32_t (*hakios_display_frames)(void);
static uint64_t gFBLastFrameShown;              /* mach_absolute_time of the last frame copied to the panel */

static uint32_t
fbFramesNow(void)
{
	uint64_t last = os_atomic_load(&gFBLastFrameShown, relaxed), limit, now = mach_absolute_time();

	if (last == 0) {
		return 0;
	}
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &limit);
	return now - last < limit ? 1 : 0;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	MiniIOSurface * fScanoutShown;          /* fb_zero: the pool surface VIG0 fetches now (held), or NULL */
	uint32_t        fScanoutFlips, fScanoutFails, fScanoutCopies;   /* fb_zero: frames flipped, flips not taken, frames copied */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	uint32_t        fSwapFence[SWAP_QUEUE]; /* ... and the GPU's last submission when each came in (MiniIOSurfaceFenceNow) */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* the swap's own fence, and the surfaces it copied while the GPU was given them again */
	uint32_t        fStatSwapFence;         /* swaps that waited for the fence of the last submission before them */
	uint32_t        fStatSwapFenceLate;     /* ... that one was not done in fb_swap_fence_ms */
	uint64_t        fStatSwapWait;          /* how long those waits took together */
	uint64_t        fStatSurfaceWait;       /* how long the surfaces' own fences took together (MiniIOSurfaceDrawn) */
	uint32_t        fStatRedrawn;           /* surfaces given to the GPU again while they were copied */
	uint32_t        fStatSeedChanged;       /* surfaces locked for drawing while they were copied (no GPU needed) */
	uint32_t        fStatToCpu;             /* surfaces taken into this processor's view before the copy */
	/* boot-arg fb_budget (frameBudget()): the frames that came late, and whether the driver was the reason */
	uint32_t        fBudgetFrames;          /* frames counted (those with one before them) */
	uint32_t        fBudgetLate;            /* ... that came over FRAME_LATE_US after the one before */
	uint32_t        fBudgetLateFence;       /* ... and waited over FENCE_LONG_US for the GPU's fence */
	uint32_t        fBudgetLateCopy;        /* ... and spent over COPY_LONG_US in the copy */
	uint32_t        fBudgetLateNeither;     /* ... and neither: nothing asked for this frame on time */
	uint32_t        fBudgetFence[6];        /* every frame's fence wait: under 0.5, 1, 2, 4, 8 ms, and longer */
	uint32_t        fBudgetCopy[6];         /* ... and its copy the same */
	uint32_t        fBudgetFenceLongest, fBudgetCopyLongest;
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces, uint32_t swapFence);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void reportFrameBudget(void);
	void frameBudget(uint32_t intervalUs, uint32_t fenceUs, uint32_t copyUs);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N; fb_120=1: at 120), with one colour mode, 8-bit RGB. The
 * identifiers are what "the current mode" (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	/*
	 * A 120 Hz display: the mode (and the refresh timer) at 120, and a frame put on the panel as
	 * rows copied rather than scaled (a scaled frame does not fit in 8.33 ms). fb_fit=1 asks for
	 * the same picture; with fb_zero=1 the flip has no copy at all and the crop is its own.
	 */
	gModeHz = gFb120 != 0 ? 120 : gRefreshHz;
	gPictureFit = gFit != 0 || gFb120 != 0;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);
	if (gFb120 != 0) {
		IOLog("MiniIOMobileFramebuffer: fb_120=1: the display is said to be %u Hz; a refresh every panel frame; "
		    "a frame copied as rows, not scaled%s\n", refreshHz(),
		    gFbZero != 0 ? " (fb_zero=1: a whole surface is flipped, no copy at all)" : "");
		if (!(fWidth >= fPanelWidth && fHeight >= fPanelHeight)) {
			IOLog("MiniIOMobileFramebuffer: fb_120=1: the picture %ux%u is smaller than the panel: it is scaled, and "
			    "120 frames a second are not to be expected\n", fWidth, fHeight);
		}
	}
	if (gPictureFit && fWidth >= fPanelWidth && fHeight >= fPanelHeight) {
		hakios_display_crop[0] = (fWidth - fPanelWidth) / 2;
		hakios_display_crop[1] = (fHeight - fPanelHeight) / 2;
		hakios_display_crop[2] = fWidth;
		hakios_display_crop[3] = fHeight;
		hakios_display_crop[4] = fPanelWidth;
		os_atomic_store(&hakios_display_crop[5], fPanelHeight, release);       /* (last: the touch screen reads it first) */
		IOLog("MiniIOMobileFramebuffer: %s: the picture's middle %ux%u, pixel for pixel (from %u, %u); no scaling\n",
		    gFit != 0 ? "fb_fit=1" : "fb_120=1", fPanelWidth, fPanelHeight, hakios_display_crop[0], hakios_display_crop[1]);
	}

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("HakiosWaitsForGPU", kOSBooleanTrue);
	/* ... and for who votes the GPU down (bsd/dev/vayu_adreno.c): not while frames are going to the panel */
	hakios_display_frames = fbFramesNow;
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all: the columns on the panel, cx0 to cx1 */
			memcpy(to + cx0, pixels + (size_t) (sy + y - py0) * stride + (size_t) (sx + cx0 - px0) * 4, (size_t) (cx1 - cx0) * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->py1 - job->py0;
		y0 = job->py0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->py0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	if (gCrop && sx == 0 && sy == 0 && sw == (int32_t) s->info.width && sh == (int32_t) s->info.height &&
	    dx <= 0 && dy <= 0 && dx + dw >= fWidth && dy + dh >= fHeight &&
	    sw >= (int32_t) fPanelWidth && sh >= (int32_t) fPanelHeight &&
	    sw - (int32_t) fPanelWidth <= 64 && sh - (int32_t) fPanelHeight <= 64) {
		static bool logged;
		int32_t left = (sw - (int32_t) fPanelWidth) / 2, top = (sh - (int32_t) fPanelHeight) / 2;

		px0 = sx - left;
		py0 = sy - top;
		px1 = px0 + sw;
		py1 = py0 + sh;
		if (!logged) {
			logged = true;
			IOLog("MiniIOMobileFramebuffer: fb_crop=1: the middle %ux%u of a %dx%d picture, pixel for pixel "
			    "(%d columns and %d rows cropped)\n", fPanelWidth, fPanelHeight, sw, sh, left, top);
		}
	} else if (gPictureFit && fWidth >= fPanelWidth && fHeight >= fPanelHeight) {
		int32_t left = (int32_t) (fWidth - fPanelWidth) / 2, top = (int32_t) (fHeight - fPanelHeight) / 2;

		px0 = dx - left;
		py0 = dy - top;
		px1 = dx + dw - left;
		py1 = dy + dh - top;
	} else {
		px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
		py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
		px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
		py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	}
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && cx1 > cx0;   /* (clipped to the panel in drawRows) */
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(&job, py0, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* The bucket of a time in microseconds: under 0.5, 1, 2, 4, 8 ms, and longer (6). */
static unsigned
timeBucket(uint32_t us)
{
	static const uint32_t limits[] = { 500, 1000, 2000, 4000, 8000 };

	for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
		if (us < limits[i]) {
			return i;
		}
	}
	return sizeof(limits) / sizeof(limits[0]);
}

/*
 * One frame's three times, for the five-second line (fb_budget=0: not called). `intervalUs` is 0 for
 * the first frame after a pause or the driver's start: it has no frame before it to be late after.
 * Called with fLock held.
 */
void
IOMobileFramebuffer::frameBudget(uint32_t intervalUs, uint32_t fenceUs, uint32_t copyUs)
{
	bool fenceLong = fenceUs > FENCE_LONG_US;
	bool copyLong = copyUs > COPY_LONG_US;

	fBudgetFence[timeBucket(fenceUs)]++;
	fBudgetCopy[timeBucket(copyUs)]++;
	fBudgetFenceLongest = fenceUs > fBudgetFenceLongest ? fenceUs : fBudgetFenceLongest;
	fBudgetCopyLongest = copyUs > fBudgetCopyLongest ? copyUs : fBudgetCopyLongest;
	if (intervalUs == 0) {
		return;
	}
	fBudgetFrames++;
	if (intervalUs > FRAME_LATE_US) {
		fBudgetLate++;
		fBudgetLateFence += fenceLong;
		fBudgetLateCopy += copyLong;
		fBudgetLateNeither += !fenceLong && !copyLong;
	}
}

/*
 * What the last five seconds' late frames were waiting for. The line to read first when a display
 * that should run at 120 runs at 60: "of those %u neither" counts the frames that nothing in this
 * driver and nothing in the GPU held up -- the window server simply did not ask for them on time.
 */
void
IOMobileFramebuffer::reportFrameBudget(void)
{
	const uint32_t * f = fBudgetFence;
	const uint32_t * c = fBudgetCopy;

	if (gBudget == 0 || fBudgetFrames == 0) {
		return;
	}
	IOLog("MiniIOMobileFramebuffer: the frames that came late: %u of %u over %u ms; of those, %u waited over %u ms for "
	    "the GPU's fence, %u spent over %u ms in the copy, %u neither\n", fBudgetLate, fBudgetFrames, FRAME_LATE_US / 1000,
	    fBudgetLateFence, FENCE_LONG_US / 1000, fBudgetLateCopy, COPY_LONG_US / 1000, fBudgetLateNeither);
	IOLog("MiniIOMobileFramebuffer: the fence wait: %u %u %u %u %u %u frames under 0.5/1/2/4/8 ms and longer (the longest "
	    "%u us); the copy: %u %u %u %u %u %u (the longest %u us); the surfaces' own fences: %llu us a frame\n", f[0], f[1],
	    f[2], f[3], f[4], f[5], fBudgetFenceLongest, c[0], c[1], c[2], c[3], c[4], c[5], fBudgetCopyLongest,
	    fStatFrames != 0 ? fStatSurfaceWait / fStatFrames / NSEC_PER_USEC : 0);
	bzero(fBudgetFence, sizeof(fBudgetFence));
	bzero(fBudgetCopy, sizeof(fBudgetCopy));
	fBudgetFrames = fBudgetLate = fBudgetLateFence = fBudgetLateCopy = fBudgetLateNeither = 0;
	fBudgetFenceLongest = fBudgetCopyLongest = 0;
	fStatSurfaceWait = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		hakios_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	/*
	 * The GPU's last submission as of this swap: the frame is presented no sooner than that, whatever
	 * the submission named. Taken here, in SwapEnd's own order with the submissions of the program that
	 * made the frame, and not later on the presenter's thread, where a submission of the next frame
	 * would be in it too.
	 */
	fSwapFence[slot] = MiniIOSurfaceFenceNow();
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		uint32_t swapFence;

		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		swapFence = fSwapFence[fSwapFirst];
		IOLockUnlock(fSwapLock);
		show(swap, surfaces, swapFence);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces, uint32_t swapFence)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	uint32_t fenceUs = 0, copyUs = 0, intervalUs = 0;
	bool report;

	/*
	 * The swap's own fence first: the GPU's last submission when the swap came in. A frame is not to be
	 * copied before the core has finished what the program that made it had given the core by then --
	 * whether the submission named the surface or not (MiniIOSurfaceFenceNow). Then, surface by surface,
	 * the fence of the last submission that named that surface, which is the longer wait of the two when
	 * a frame was drawn in more than one submission.
	 */
	if (swapFence != 0) {
		uint64_t fenceBegan = mach_absolute_time(), fenceDone;
		bool done = MiniIOSurfaceFenceDone(swapFence, gSwapFenceMs);

		fenceDone = mach_absolute_time();
		fStatSwapWait += fenceDone - fenceBegan;
		fenceUs = (uint32_t) micros(fenceDone - fenceBegan);
		if (!done) {
			fStatSwapFenceLate++;
			if (fStatSwapFenceLate <= 4) {
				IOLog("MiniIOMobileFramebuffer: swap %u: submission %u, the GPU's last when it came, is not done "
				    "after %u ms; the frame is copied as it is\n", swapWord(swap, SWAP_ID), swapFence, gSwapFenceMs);
			}
		}
	}
	fStatSwapFence += swapFence != 0;       /* (how many swaps had a GPU fence at all: 0 = the GPU draws none of this) */
	{
		uint64_t surfacesBegan = mach_absolute_time();

		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
				IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
				    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
			}
		}
		fStatSurfaceWait += mach_absolute_time() - surfacesBegan;
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	/* fb_zero: one surface of the scanout pool, the whole of it, in layer 0 alone */
	uint64_t zero_phys = 0;
	uint32_t zero_stride = 0;
	bool zero = gFbZero && surfaces[0] != NULL && swapWord(swap, SWAP_SURFACE(1)) == 0 && swapWord(swap, SWAP_SURFACE(2)) == 0 &&
	    swapWord(swap, SWAP_SURFACE(3)) == 0 && surfaces[0]->info.width == 1125 && surfaces[0]->info.height == 2436 &&
	    MiniIOSurfaceScanout(surfaces[0], &zero_phys, &zero_stride);
	uint64_t (*panel_next)(uint64_t) = os_atomic_load(&hakios_vsync_next, acquire);

	uint64_t (*panel_frame)(void) = os_atomic_load(&hakios_vsync_frame, acquire);
	uint64_t panel_period = panel_frame != NULL ? panel_frame() : 0, panel_beat = 0;

	if (zero) {
		/* (no beat to wait for: the flush is taken at the panel's next frame start by itself) */
	} else if (gOnTheBeat && panel_next != NULL && panel_period != 0 && (panel_beat = panel_next(began)) != 0) {
		/*
		 * The copy starts as the panel starts a frame when that is less than half a frame away; later
		 * than that it starts at once (a late frame is not held back a whole refresh, which would halve
		 * the rate when drawing takes most of a frame).
		 */
		if (panel_beat - began < panel_period / 2) {
			clock_delay_until(panel_beat);
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
		fBeat = panel_beat;
	} else if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		uint64_t gap;

		absolutetime_to_nanoseconds(began - fLastFrame, &gap);
		intervalUs = (uint32_t) (gap / NSEC_PER_USEC);
		nanos = gap / NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				if (fScanoutShown != NULL) {
					vayu_mdp_unscanout();   /* (the black goes into the boot framebuffer: VIG0 must show it) */
					fScanoutShown->release();
					fScanoutShown = NULL;
				}
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (zero && layer == 0) {
			if (!fGraphics) {
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			if (vayu_mdp_scanout(zero_phys, zero_stride, (1125 - 1080) / 2, (2436 - 2400) / 2)) {
				MiniIOSurface * was = fScanoutShown;

				s->retain();
				fScanoutShown = s;      /* held while VIG0 fetches it */
				if (was != NULL) {
					was->release(); /* (the flush is taken: VIG0 no longer reads it) */
				}
				if (fScanoutFlips++ == 0) {
					IOLog("MiniIOMobileFramebuffer: fb_zero: surface %u flipped onto the panel, no copy (phys 0x%llx, %u a row, "
					    "the centre 1080x2400)\n", sid, zero_phys, zero_stride);
				}
				continue;
			}
			if (fScanoutFails++ < 4) {
				IOLog("MiniIOMobileFramebuffer: fb_zero: the flip to surface %u was not taken; copied instead\n", sid);
			}
		}
		if (fScanoutShown != NULL) {
			vayu_mdp_unscanout();   /* VIG0 back on the boot framebuffer before it is drawn into */
			fScanoutShown->release();
			fScanoutShown = NULL;
		}
		if (gFbZero) {
			fScanoutCopies++;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			uint32_t drawnBefore = MiniIOSurfaceFence(s);
			uint32_t seedBefore = s->shared()->seed;

			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			if (gGpuRead != 0) {
				/*
				 * The core wrote these pixels; this processor's cache may hold the frame before them.
				 * Asked for with fb_gpu_read=1: it costs 1 to 2 ms of a frame (MiniIOSurfaceToCpu).
				 */
				MiniIOSurfaceToCpu(s);
				fStatToCpu++;
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
			/*
			 * Was the surface being drawn into while it was copied? The seed changes when somebody locks it
			 * to write (MiniIOSurface.cpp), and the window server does that for every frame it draws: a seed
			 * taken before the copy and another value after it says the frame that was copied is part of the
			 * next one -- work of the processor's, which no fence of the GPU's covers (TEST5Q drew without
			 * the GPU: R6).
			 */
			if (s->shared()->seed != seedBefore) {
				fStatSeedChanged++;
				if (fStatSeedChanged <= 4) {
					IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u was locked for drawing while it "
					    "was copied (seed %u -> %u): the frame is part of the next\n", swapWord(swap, SWAP_ID), layer,
					    sid, seedBefore, s->shared()->seed);
				}
			}
			/*
			 * Was the surface given to the GPU again while it was copied? Then this frame is part of the
			 * next one: the swap came before the GPU had been given all of the frame, and the copy read
			 * pixels that were being drawn over. Says how a frame with parts of it missing happens --
			 * with the frame's surface, which is the one that matters (tools/latency.py, R6).
			 */
			if (MiniIOSurfaceRedrawn(s, drawnBefore)) {
				fStatRedrawn++;
				if (fStatRedrawn <= 4) {
					IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u was given to the GPU again "
					    "while it was copied (submission %u came after %u): the frame is part of the next\n",
					    swapWord(swap, SWAP_ID), layer, sid, MiniIOSurfaceFence(s), drawnBefore);
				}
			}
		}
	}
	drawn = mach_absolute_time();
	os_atomic_store(&gFBLastFrameShown, drawn, relaxed);
	copyUs = (uint32_t) micros(drawn - began);
	if (gBudget != 0) {
		frameBudget(intervalUs, fenceUs, copyUs);
	}
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		hakios_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	gFBDrawAbs = (gFBDrawAbs * 7 + (mach_absolute_time() - began)) / 8;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		/*
		 * The fences of the frames: how many swaps came with a GPU submission to wait for at all (0: the
		 * GPU is not in the drawing of what is on the screen, and neither the idle vote nor a fence can
		 * be the reason a frame came out blank), and how many were copied while the core was given the
		 * same surface again (the frame was then copied before it was finished: R6).
		 */
		{
			uint64_t swapWait;

			absolutetime_to_nanoseconds(fStatSwapWait, &swapWait);
			IOLog("MiniIOMobileFramebuffer: fences: %u swaps came with a submission of the GPU's (the last was waited for: "
			    "%llu us of %u, %u of them %u ms or more); %u surfaces were given to the GPU again while they were copied; "
			    "%u were locked for drawing while they were copied; %u were taken into the processor's view before the copy "
			    "(fb_gpu_read)\n", fStatSwapFence, swapWait / NSEC_PER_USEC, fStatSwapFence, fStatSwapFenceLate, gSwapFenceMs,
			    fStatRedrawn, fStatSeedChanged, fStatToCpu);
			fStatSwapWait = 0;
		}
		fStatSwapFence = fStatSwapFenceLate = fStatRedrawn = fStatSeedChanged = fStatToCpu = 0;
		if (gFb120 != 0) {
			/*
			 * Whether the compositor is on this driver's clock at all: refreshes told to user space.
			 * A window server that takes them asks every panel frame (120 a second); one on its own
			 * timer never does, and this reads 0.
			 */
			uint32_t told = os_atomic_xchg(&gVSyncsTold, 0, relaxed);

			IOLog("MiniIOMobileFramebuffer: fb_120: %u refreshes told to user space in %llu ms (%u a second)%s\n",
			    told, nanos / NSEC_PER_MSEC, (uint32_t) ((uint64_t) told * NSEC_PER_SEC / nanos),
			    told == 0 ? ": nobody is listening, the compositor is on its own clock" : "");
		}
		if (gFbZero) {
			uint32_t slots = 0, used = 0;

			MiniIOSurfacePoolInfo(&slots, &used);
			IOLog("MiniIOMobileFramebuffer: fb_zero: %u frames flipped, %u copied, %u flips not taken (so far); pool %u of %u slots "
			    "in use\n", fScanoutFlips, fScanoutCopies, fScanoutFails, used, slots);
		}
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/*
			 * How many display surfaces the bottom layer came in: two says double buffering, and then a
			 * frame the driver (or the GPU) is still holding is one the window server cannot draw into --
			 * the pipeline's latency, not its throughput, is what the frame rate is then made of.
			 */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in %u%s different surfaces%s; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", different, different == 8 ? " (or more)" : "",
			    different == 8 ? " -- none twice" : "", made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		reportFrameBudget();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

/*
 * What the brightness system (backboardd's CoreBrightness) sets on the framebuffer. IOService's own
 * setProperties refuses everything, so until now each of these was turned away without a word. On an
 * iPhone "DisplayBrightnessFactor" (CoreBrightness: ...FactorZero, ...FactorWithFade) goes to 0 when the
 * display is to be dark and back up when it is lit: with a backlight driver (hakios_display_power) that
 * switches it. Every key is kept as a property and logged (the first 200 sets; brightness-factor changes
 * always), so the run shows what the slider sends.
 */
static unsigned gFBPropsLogged;
static int64_t gFBFactor = -1;

static void
fbLogProperty(const char * who, const OSSymbol * key, OSObject * value)
{
	OSNumber * number = OSDynamicCast(OSNumber, value);
	OSData * data = OSDynamicCast(OSData, value);

	if (number != NULL) {
		IOLog("MiniIOMobileFramebuffer: %s sets %s = %llu (%#llx)\n", who, key->getCStringNoCopy(),
		    number->unsigned64BitValue(), number->unsigned64BitValue());
	} else if (value == kOSBooleanTrue || value == kOSBooleanFalse) {
		IOLog("MiniIOMobileFramebuffer: %s sets %s = %s\n", who, key->getCStringNoCopy(), value == kOSBooleanTrue ? "true" : "false");
	} else if (data != NULL) {
		IOLog("MiniIOMobileFramebuffer: %s sets %s = data of %u bytes\n", who, key->getCStringNoCopy(), data->getLength());
	} else {
		IOLog("MiniIOMobileFramebuffer: %s sets %s = a %s\n", who, key->getCStringNoCopy(),
		    value != NULL ? value->getMetaClass()->getClassName() : "NULL");
	}
}

IOReturn
IOMobileFramebuffer::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSObject * value = key != NULL ? dict->getObject(key) : NULL;
		OSNumber * number = OSDynamicCast(OSNumber, value);
		bool factor = key != NULL && key->isEqualTo("DisplayBrightnessFactor");

		if (key == NULL || value == NULL) {
			continue;
		}
		if (factor && number != NULL) {
			int64_t now = (int64_t)number->unsigned64BitValue();

			if ((now == 0) != (gFBFactor == 0) || gFBFactor < 0) {
				bool (*display_power)(bool) = hakios_display_power;
				bool on = now != 0, done = display_power != NULL && display_power(on);

				IOLog("MiniIOMobileFramebuffer: %s: DisplayBrightnessFactor %lld -> %lld: display %s%s\n", who, gFBFactor, now,
				    on ? "on" : "off", display_power == NULL ? " (no backlight driver)" : done ? "" : " (the backlight said no)");
			}
			gFBFactor = now;
		} else if (gFBPropsLogged < 200) {
			gFBPropsLogged++;
			fbLogProperty(who, key, value);
		}
		setProperty(key, value);
	}
	keys->release();
	return kIOReturnSuccess;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	uint64_t (*panel_next)(uint64_t) = os_atomic_load(&hakios_vsync_next, acquire);
	uint64_t (*panel_frame)(void) = os_atomic_load(&hakios_vsync_frame, acquire);

	if (panel_next != NULL && panel_frame != NULL && (period = panel_frame()) != 0) {
		/* the next frame start the panel itself will make; a quarter frame at least after the last one told */
		uint64_t next = panel_next(now);
		uint32_t div = gFb120 != 0 ? 1 : gVsyncDivisor;

		if (gFb120 != 0 && gFb120Idle != 0) {
			uint64_t limit, idle = hakios_touch_idle_abs();
			bool slow;

			nanoseconds_to_absolutetime((uint64_t)(gFb120IdleMs > 60000 ? 60000 : gFb120IdleMs) * NSEC_PER_MSEC, &limit);
			slow = idle > limit;
			if (slow != gFb120Slow) {
				gFb120Slow = slow;
				if (gFb120Changes++ < 40) {
					IOLog("MiniIOMobileFramebuffer: fb_120_idle: %s (%u changes)\n", slow ?
					    "nothing touched for a while: refreshes every 2nd panel frame (60 a second)" :
					    "touched: refreshes every panel frame (120 a second)", gFb120Changes);
				}
			}
			div = slow ? 2 : 1;
		}

		if (div == 0) {
			if (gFBDivisor == 1 && gFBDrawAbs * 10 > period * 6) {
				gFBDivisor = 2;
				IOLog("MiniIOMobileFramebuffer: the copy takes over 60%% of a panel frame: refreshes every 2nd frame\n");
			} else if (gFBDivisor == 2 && gFBDrawAbs * 100 < period * 45) {
				gFBDivisor = 1;
				IOLog("MiniIOMobileFramebuffer: the copy takes under 45%% of a panel frame: refreshes every frame\n");
			}
			div = gFBDivisor;
		}
		div = div < 1 ? 1 : div > 4 ? 4 : div;
		if (next != 0) {
			while (next < fVSyncTime + period * div - period / 4) {
				next += period;
			}
			fVSyncTime = next;
			thread_call_enter_delayed(fVSyncCall, fVSyncTime);
			return;
		}
	}
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		os_atomic_inc(&gVSyncsTold, relaxed);
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			hakios_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodRequestPowerChange: {        /* in[0]: 0 = display off (backboardd's screen-off), else on */
		bool on = args->scalarInputCount < 1 || in[0] != 0;
		bool done = hakios_display_power != NULL && hakios_display_power(on);

		IOLog("MiniIOMobileFramebuffer: power change asked (scalars %u in [0x%llx 0x%llx]): display %s%s\n",
		    args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    on ? "on" : "off", hakios_display_power == NULL ? " (no backlight driver)" : done ? "" : " (the backlight said no)");
		return kIOReturnSuccess;
	}
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);

	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

/* ---- AppleARMBacklight: what iOS's brightness system drives ------------------------------------------ */

/*
 * backboardd's BrightnessSystem (CoreBrightness) finds the built-in display's backlight by matching an
 * IOService of the class AppleARMBacklight (strings of the iOS 16.5 dyld cache, 20F66: CBDisplayContaineriOS
 * "AppleARMBacklight" next to "IOHIDDevice"; PLDisplayAgent IOServiceMatched "AppleARMBacklight",
 * "IODisplayParameters", "backlight-control"). With none, it had no display to set: the mgr8 run logged
 * "Set BrightnessSystem property:DisplayBrightnessFactor to:0" on the lock button and nothing reached the
 * kernel. This service gives it one: the parameters an iPhone's has (brightness 0..65536, rawBrightness
 * 0..4095, BrightnessMilliNits), every property set on it logged (the first 300) and kept, and what can be
 * read as a level goes to the panel (hakios_display_brightness), 0 (or DisplayBrightnessFactor 0) turning
 * the backlight off and anything else on (hakios_display_power). HYPOTHESIS until the log shows which
 * keys CoreBrightness really sends.
 */
class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);
public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	void publishParameters(uint32_t brightness);
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

static unsigned gBacklightLogged;
static int gBacklightOn = -1;

static OSDictionary *
backlightParameter(uint32_t min, uint32_t max, uint32_t value)
{
	OSDictionary * d = OSDictionary::withCapacity(3);
	OSNumber * n;

	if (d == NULL) {
		return NULL;
	}
	if ((n = OSNumber::withNumber(min, 32)) != NULL) {
		d->setObject("min", n);
		n->release();
	}
	if ((n = OSNumber::withNumber(max, 32)) != NULL) {
		d->setObject("max", n);
		n->release();
	}
	if ((n = OSNumber::withNumber(value, 32)) != NULL) {
		d->setObject("value", n);
		n->release();
	}
	return d;
}

void
AppleARMBacklight::publishParameters(uint32_t brightness)
{
	OSDictionary * params = OSDictionary::withCapacity(3), * d;

	if (params == NULL) {
		return;
	}
	if ((d = backlightParameter(0, 65536, brightness)) != NULL) {
		params->setObject("brightness", d);
		d->release();
	}
	if ((d = backlightParameter(0, 4095, (uint32_t)((uint64_t)brightness * 4095 / 65536))) != NULL) {
		params->setObject("rawBrightness", d);
		d->release();
	}
	if ((d = backlightParameter(0, 500000, (uint32_t)((uint64_t)brightness * 500000 / 65536))) != NULL) {
		params->setObject("BrightnessMilliNits", d);
		d->release();
	}
	setProperty("IODisplayParameters", params);
	params->release();
}

/* a number, or a parameter dictionary's "value"; false if neither */
static bool
backlightNumber(OSObject * o, uint64_t * value, uint64_t * max)
{
	OSNumber * n = OSDynamicCast(OSNumber, o);
	OSDictionary * d = OSDynamicCast(OSDictionary, o);

	if (d != NULL) {
		OSNumber * m = OSDynamicCast(OSNumber, d->getObject("max"));

		n = OSDynamicCast(OSNumber, d->getObject("value"));
		if (m != NULL && max != NULL) {
			*max = m->unsigned64BitValue();
		}
	}
	if (n == NULL) {
		return false;
	}
	*value = n->unsigned64BitValue();
	return true;
}

static void
backlightApply(const char * who, const char * key, uint64_t value, uint64_t max)
{
	bool (*power)(bool) = hakios_display_power;
	bool (*level)(uint32_t) = hakios_display_brightness;
	uint32_t permille = max != 0 ? (uint32_t)(value * 1000 / max) : 0;
	bool on = value != 0;

	if (on != (gBacklightOn == 1)) {
		bool done = power != NULL && power(on);

		IOLog("AppleARMBacklight: %s: %s %llu: backlight %s%s\n", who, key, value, on ? "on" : "off",
		    power == NULL ? " (no backlight driver)" : done ? "" : " (the backlight said no)");
		gBacklightOn = on;
	}
	if (on && max != 0 && level != NULL) {
		level(permille < 1 ? 1 : permille > 1000 ? 1000 : permille);
	}
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSObject * value = key != NULL ? dict->getObject(key) : NULL;
		uint64_t v = 0, max = 0;

		if (key == NULL || value == NULL) {
			continue;
		}
		if (gBacklightLogged < 300) {
			gBacklightLogged++;
			fbLogProperty(who, key, value);
			if (OSDictionary * inner = OSDynamicCast(OSDictionary, value)) {
				OSCollectionIterator * ik = OSCollectionIterator::withCollection(inner);
				OSObject * in;

				while (ik != NULL && (in = ik->getNextObject()) != NULL) {
					const OSSymbol * k2 = OSDynamicCast(OSSymbol, in);

					if (k2 != NULL) {
						IOLog("AppleARMBacklight:   %s.", key->getCStringNoCopy());
						fbLogProperty("", k2, inner->getObject(k2));
					}
				}
				OSSafeReleaseNULL(ik);
			}
		}
		if (key->isEqualTo("DisplayBrightnessFactor") || key->isEqualTo("DisplayBrightnessFactorWithFade")) {
			if (backlightNumber(value, &v, NULL)) {
				backlightApply(who, key->getCStringNoCopy(), v, 0);
			}
		} else if (key->isEqualTo("brightness") && backlightNumber(value, &v, &max)) {
			backlightApply(who, "brightness", v, max ? max : 65536);
		} else if (key->isEqualTo("rawBrightness") && backlightNumber(value, &v, &max)) {
			backlightApply(who, "rawBrightness", v, max ? max : 4095);
		} else if (key->isEqualTo("IODisplayParameters")) {
			OSDictionary * p2 = OSDynamicCast(OSDictionary, value);

			if (p2 != NULL && backlightNumber(p2->getObject("brightness"), &v, &max)) {
				backlightApply(who, "IODisplayParameters.brightness", v, max ? max : 65536);
			} else if (p2 != NULL && backlightNumber(p2->getObject("rawBrightness"), &v, &max)) {
				backlightApply(who, "IODisplayParameters.rawBrightness", v, max ? max : 4095);
			}
			continue;               /* (our published parameters stay as they are) */
		}
		setProperty(key, value);
	}
	keys->release();
	return kIOReturnSuccess;
}

static TUNABLE(uint32_t, gPublishBacklight, "fb_backlight", 1);      /* fb_backlight=0: no AppleARMBacklight */

static void
publishBacklight(IOService * provider)
{
	AppleARMBacklight * bl;

	if (!gPublishBacklight) {
		return;
	}
	bl = OSTypeAlloc(AppleARMBacklight);

	if (bl != NULL && bl->init() && bl->attach(provider)) {
		bl->setName("backlight");
		bl->setProperty("backlight-control", kOSBooleanTrue);
		bl->publishParameters(32768);
		bl->registerService();
		IOLog("AppleARMBacklight: published for the brightness system (%s)\n",
		    hakios_display_power != NULL ? "a backlight driver is there" : "no backlight driver yet");
	}
	OSSafeReleaseNULL(bl);
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
		publishBacklight(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
