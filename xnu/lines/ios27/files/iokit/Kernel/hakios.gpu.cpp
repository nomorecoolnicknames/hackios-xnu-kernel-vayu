/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Origin: the iOS 16.5 line (tree 6a36d992) iokit/Kernel/hakios.gpu.cpp (the 8796 daily line, 1b2dae9: the service the daily
 * 16.5's HakiosMetal opens). Ported to xnu-12377 (K8): the msm interface as it was (selectors, structures, error
 * codes); started by bsd/dev/vayu_adreno.c only when the Adreno 640 is kept (gpu=1 vayu_gpu=1, vayu27=1), so QEMU and
 * a run without them never see the service; the calls it makes are sys/vayu_gpu.h's.
 *
 * K8b, the GPU contract with SweetSurface and SweetFramebuffer (both on this branch):
 *  - 0x7e: a SweetSurface's pixels as a buffer (its own memory descriptor: a kernel buffer, or one contiguous run
 *    below 4 GB with vayu_surface_contig=1 -- what fb_zero's MDP scans out). The surface is held while the buffer is.
 *    A client's own memory with a surface around it (prepared, or a snapshot) is refused: kIOReturnUnsupported;
 *  - every submission that names a surface cleans it to memory first (CPU -> GPU, no seed shortcut) and stores its
 *    fence in the surface (gpuFence, release); the display waits for that fence before it flips or copies the surface
 *    and, for a copy, invalidates it for the CPU (fence-to-present, SweetFramebuffer.cpp);
 *  - 0x7f: the caller's pixels copied to the panel (sweet_fb_vayu_show);
 *  - every request's structure length is checked against its own (the oldest libdrm form .. the current one), not
 *    against the union; a buffer made, or given again, is cleaned and invalidated before the client sees it;
 *  - HakiosGPUFillSurface() for vayu_gpufb_test=1: the GPU writes a gradient into a surface (CP_MEM_WRITE rows).
 *
 * HakiosGPU: the phone's Adreno for programs to draw with.
 *
 * bsd/dev/hakios.adreno.c wakes the GPU and, with boot-arg adreno_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's msm DRM driver,
 * the one Mesa's freedreno knows: every request of that interface is a call of the user client
 * with the request's number as the selector and the request's structure passed in and back
 * (userland: tools/mesa/hakios-drm, which stands in for libdrm inside Mesa). What Linux does
 * with mmap() on the device is the client memory of the type that is the buffer's handle.
 *
 * What there is of the interface is what freedreno uses to draw: parameters, buffers (made,
 * told their address in the GPU's address space, mapped, given back), submission of command
 * buffers, and waiting for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context; it is zero when made, and write-combined: what the program writes goes to
 * memory, where the GPU reads it, and not into this processor's cache (as Linux maps them for
 * freedreno; boot-arg gpu_cached=1 makes them ordinary memory, pushed out of the cache whenever
 * the GPU is given something to do: see submit()).
 *
 * One request is not Linux's: a buffer that is the memory of an IOSurface (surfaceBuffer()), for
 * drawing into what the display shows and for reading pictures where other programs have drawn
 * them. Linux hands such memory around as file descriptors; here it has a name already. That
 * memory is ordinary, cached memory, which everybody else reads and writes as such.
 *
 * Not here yet: interrupts (waiting is asking again five times a millisecond), more than one
 * program's worth of care (every client's buffers are in the one address space; a client that
 * goes takes its own away), recovery of a GPU that has stopped answering (it is reported, see
 * hakios_gpu_report(), and stays stopped), fences and buffers as file descriptors.
 */

#include <IOKit/IOLib.h>
#include <stddef.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/systm.h>
#include <os/atomic_private.h>

extern "C" {
/* bsd/dev/vayu_gpu_abi.c and vayu_adreno.c (bsd/sys/vayu_gpu.h, bsd/sys/vayu_locks.h: not seen from iokit) */
bool hakios_gpu_ready(void);
bool hakios_gpu_parameter(uint32_t which, uint64_t * value);
uint64_t hakios_gpu_address(uint64_t size);
void hakios_gpu_address_free(uint64_t address, uint64_t size);
bool hakios_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void hakios_gpu_mapped(void);
void hakios_gpu_unmap(uint64_t address, uint64_t size);
void hakios_gpu_to_memory(void * start, uint64_t size);
uint32_t hakios_gpu_submit(const uint64_t * addresses, const uint32_t * words, uint32_t count);
extern uint32_t gpu_log;
bool hakios_gpu_done(uint32_t fence);
uint32_t hakios_gpu_last(void);
void hakios_gpu_worked(uint64_t * microseconds, uint32_t * submissions, uint32_t by[7]);
void hakios_gpu_report(const char * why);
void hakios_gpu_stop(void);
void hakios_tl(const char * format, ...) __printflike(1, 2);

void hakios_gpu_for_device(void * start, uint64_t size);
void hakios_gpu_for_cpu(void * start, uint64_t size);

void HakiosGPUStart(void);
void HakiosGPUStop(void);
void HakiosAcceleratorStart(void);
bool HakiosGPUFillSurface(uint32_t surface, uint32_t * fence);
void sweet_fb_vayu_show(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

#include "arm/SweetSurface.h"

/*
 * Every line of this file also goes to the loader's log, which is the one a run's log keeps: IOLog
 * alone does not reach it (MiniIOMobileFramebuffer.cpp says the same at its top, and the TEST5Q run
 * with 2.8 million lines has not one "HakiosGPU:" line in it while it has every printf of bsd/dev/).
 * The five-second summary below is the only place the GPU's own work per submission is written down
 * ("the GPU worked %llu us on each of %u", with the 0.1/0.25/0.5/1/2/4 ms buckets, the cache pushes
 * and the waits), and a display whose frames come late cannot be told from a window server that asks
 * for them late without it: T120b.
 */
static void gpuLog(const char * format, ...) __printflike(1, 2);
static void
gpuLog(const char * format, ...)
{
	char line[512];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	(IOLog)("%s", line);                    /* (in parentheses: IOLog is this, below; the console reaches the loader's log) */
}
#define IOLog(...)      gpuLog(__VA_ARGS__)

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kHakiosSurface         = 0x7e,         /* ours: an IOSurface's memory as a buffer */
	kHakiosShow            = 0x7f,         /* ours: pixels of the caller's onto the panel, to see a test's result */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHakiosDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct hakios_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

#define kHakiosDescribe        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             8192            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          8192
#define GPU_PATIENCE_MS         5000            /* for a buffer or a submission that somebody waits for without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting for the core: how long between two looks at how far it is */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again for a submission this long after the last */

static IOLock * gGPULock;                       /* around everything that is the GPU's (hakios_gpu_...) */
static bool gGPUReported;
static bool gGPUCached;                         /* boot-arg gpu_cached=1 */

/* What the last seconds' submissions were and cost here, for the log. */
static struct {
	uint64_t since, flushing, waiting, taking;      /* times of the processor's clock */
	uint64_t flushed;                       /* bytes sent out of the cache */
	uint32_t submissions, commands, buffers, surfaces, waits, over;
	uint32_t made, given_again;             /* buffers of their own made for programs, and of those kept ones given again */
	uint64_t making;                        /* ... and the time it took to make the others */
} gGPUStat;

/* Wait for the 3D core to be done with submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hakios_gpu_done(fence)) {
			if (waited) {
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	if (milliseconds >= 1000 && !gGPUReported) {
		gGPUReported = true;
		hakios_gpu_report("no answer");
	}
	return false;
}

/*
 * Wait for the 3D core to be done with submission `fence`, with no lock held: what the display's
 * driver does with a frame (MiniIOSurfaceDrawn). A client's submission holds the clients' lock
 * while it waits for room in the ring (up to two seconds, hakios_gpu_submit) and while it pushes
 * its memory out of the cache; a frame that waited behind that would be shown half drawn, or late
 * enough that the next one is. The fence is a word in memory the core writes: asking it again is
 * what a wait is here (there are no interrupts).
 */
static bool
gpuWaitFree(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (bool waited = false;; waited = true) {
		if (hakios_gpu_done(fence)) {
			if (waited && gGPULock != NULL) {
				IOLockLock(gGPULock);           /* (only to count it) */
				gGPUStat.waits++;
				gGPUStat.waiting += mach_absolute_time() - began;
				IOLockUnlock(gGPULock);
			}
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUReported) {
			break;                  /* (a GPU that has stopped answering is not waited for again) */
		}
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
	}
	return false;
}

/*
 * Boot-arg touch_lat=1: a line for every wait of a program's for the GPU that took a millisecond or
 * more ("tl: f KIND US at US"), for telling apart what holds a frame up (tools/frames.py).
 */
static TUNABLE(uint32_t, gWaitLines, "touch_lat", 0);

static void
slowWait(const char * kind, uint64_t began)
{
	uint64_t now = mach_absolute_time(), ns, at;

	if (!gWaitLines) {
		return;
	}
	absolutetime_to_nanoseconds(now - began, &ns);
	if (ns >= NSEC_PER_MSEC) {
		absolutetime_to_nanoseconds(now, &at);
		hakios_tl("tl: f %s %llu at %llu\n", kind, ns / 1000, at / 1000);
	}
}

/* How long from now a time of msm's is, in milliseconds; 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec * when)
{
	uint64_t now;
	int64_t left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when->seconds * (int64_t) NSEC_PER_SEC + when->nanoseconds - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : (uint64_t) left / NSEC_PER_MSEC;
}

/*
 * A wait that was to end at a time that had passed when it was asked for, and the GPU was not
 * done: whoever asked goes on as if it were (a picture is shown half drawn). That is what comes
 * of a caller whose clock is not this one, so the first few are told.
 */
static void
waitOver(const struct msm_timespec * when, uint32_t fence)
{
	static uint32_t told;
	uint64_t now;

	gGPUStat.over++;
	if (told < 4) {
		told++;
		absolutetime_to_nanoseconds(mach_continuous_time(), &now);
		IOLog("HakiosGPU: a wait for submission %u, which is not done, until %lld.%09lld s, asked for at %llu.%09llu s: "
		    "over before it began\n", fence, when->seconds, when->nanoseconds, now / NSEC_PER_SEC, now % NSEC_PER_SEC);
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class HakiosGPU : public IOService
{
	OSDeclareDefaultStructors(HakiosGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPU, IOService);

bool
HakiosGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	setName("HakiosGPU");
	registerService();
	return true;
}

/*
 * Metal finds a GPU (MTLRegisterDevices, iOS 16.5 as the daily line's HakiosMetal knows it, and iOS 27 the same way: the
 * iOS side matches IOServiceMatching("IOAcceleratorES"), StarbiteAccelerator.cpp says the same of Scion's 26/27 Metal):
 * the services of class IOAcceleratorES, their MetalPluginName -- /System/Library/Extensions/<name>.bundle -- and
 * MetalPluginClassName, the _MTLDevice subclass sent -initWithAcceleratorPort: with the service. The daily line published
 * it from MiniIOMobileFramebuffer.cpp (publishAccelerator, metal=N); here on the platform beside HakiosGPU, with metal=N
 * (N != 0: HakiosMetal reads N for which processes get a device), from vayu27's AFTER_DEVFS hook (bsd/dev/vayu_gpu_abi.c),
 * GPU or not: HakiosMetalDevice opens HakiosGPU itself, and without one (QEMU) holds its buffers in plain memory.
 * (StarbiteAccelerator.cpp, which defines the same class for the S9, is not built for VAYU.)
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

void
HakiosAcceleratorStart(void)
{
	IOService * provider = IOService::getPlatform();
	IOAcceleratorES * accelerator;
	uint32_t metal = 0;
	static bool once;

	(void) PE_parse_boot_argn("metal", &metal, sizeof(metal));
	if (metal == 0 || once) {
		return;
	}
	once = true;
	if (provider == NULL) {
		IOLog("HakiosGPU: metal=%u: no platform to publish the IOAcceleratorES on\n", metal);
		return;
	}
	accelerator = OSTypeAlloc(IOAcceleratorES);
	if (accelerator == NULL || !accelerator->init() || !accelerator->attach(provider)) {
		OSSafeReleaseNULL(accelerator);
		IOLog("HakiosGPU: cannot publish the IOAcceleratorES\n");
		return;
	}
	accelerator->setName("HakiosAccelerator");
	accelerator->setProperty("MetalPluginName", "HakiosMetal");
	accelerator->setProperty("MetalPluginClassName", "HakiosMetalDevice");
	accelerator->setProperty("IOGLBundleName", "HakiosMetal");          /* (as Scion's StarbiteAccelerator, iOS 26/27) */
	accelerator->setProperty("model", "HakiOS Adreno 640");
	accelerator->registerService();
	IOLog("HakiosGPU: metal=%u: IOAcceleratorES HakiosAccelerator published (MetalPluginName HakiosMetal, "
	    "MetalPluginClassName HakiosMetalDevice, IOGLBundleName HakiosMetal, model HakiOS Adreno 640)\n", metal);
	accelerator->release();
}

void
HakiosGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	HakiosGPU * gpu;
	uint32_t cached = 0;

	gGPUCached = PE_parse_boot_argn("gpu_cached", &cached, sizeof(cached)) && cached != 0;
	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(HakiosGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosGPU: the service did not start\n");
	}
	gpu->release();
}

/* ---- vayu_gpufb_test=1: the GPU fills a surface (iokit/Kernel/arm/SweetFramebuffer.cpp sweet_fb_vayu_gpu_test) ---- */

#define FILL_CP_MEM_WRITE       0x3d
#define FILL_IB_WORDS_MAX       (512u * 1024)           /* one indirect buffer: well under the CP's 20-bit size */
#define FILL_IBS_MAX            16

static uint32_t
fillParity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

extern "C" void vayu_crumb(const char *format, ...) __printflike(1, 2);  /* vayu_crumb=1: bsd/sys/vayu_mdp.h */

/* memory -> GPU addresses, 4 KB at a time (as newBuffer); 0: not had. With gGPULock held. */
static uint64_t
fillMap(IOMemoryDescriptor * memory, uint64_t size)
{
	uint64_t address = hakios_gpu_address(size);
	bool mapped = address != 0;

	for (uint64_t offset = 0; mapped && offset < size; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && length >= 4096 && hakios_gpu_map(address + offset, phys, false);
	}
	hakios_gpu_mapped();
	if (!mapped && address != 0) {
		hakios_gpu_unmap(address, size);
		hakios_gpu_address_free(address, size);
		address = 0;
	}
	return address;
}

/*
 * One submission of CP_MEM_WRITE packets, a row each, that write the gradient of sweet_fb_vayu_gpu_pixel() into every
 * pixel of a 32-bit surface; the surface's fence set as SUBMIT sets it; waited for (2 s at most) before the mappings go.
 */
extern "C" bool
HakiosGPUFillSurface(uint32_t id, uint32_t * fenceOut)
{
	SweetSurface * s = sweet_surface_copy(id);
	IOBufferMemoryDescriptor * ib = NULL;
	uint64_t size, ibBytes, surfaceAt = 0, ibAt = 0, addresses[FILL_IBS_MAX];
	uint32_t words[FILL_IBS_MAX], count = 0, fence = 0, * w;
	uintptr_t px;
	bool ok = false;

	*fenceOut = 0;
	if (s == NULL || !hakios_gpu_ready() || gGPULock == NULL) {
		OSSafeReleaseNULL(s);
		return false;
	}
	px = s->kernelAddress();
	size = round_page_64(s->allocSize);
	if (px == 0 || s->memory == NULL || s->bytesPerElement != 4 || s->planeCount != 0 || s->width == 0 ||
	    s->width + 2 > 0x3fff || size > s->memory->getLength()) {
		IOLog("HakiosGPU: fill: surface %u is not one 32-bit plane\n", id);
		OSSafeReleaseNULL(s);
		return false;
	}
	ibBytes = round_page_64((uint64_t)s->height * (s->width + 3) * 4);
	ib = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, (vm_size_t) ibBytes, PAGE_SIZE);
	if (ib == NULL) {
		OSSafeReleaseNULL(s);
		return false;
	}
	IOLockLock(gGPULock);
	{
		IOByteCount first = 0;
		uint64_t at = s->memory->getPhysicalSegment(0, &first, kIOMemoryMapperNone);

		vayu_crumb("gpu fill: surface %u, %#llx bytes from %#llx (%#llx in one piece): mapped for the GPU\n", id, size,
		    at, (uint64_t)first);
	}
	surfaceAt = fillMap(s->memory, size);
	ibAt = surfaceAt != 0 ? fillMap(ib, ibBytes) : 0;
	vayu_crumb("gpu fill: surface at GPU %#llx, commands at %#llx\n", surfaceAt, ibAt);
	if (ibAt != 0) {
		uint32_t at = 0, start = 0;

		w = (uint32_t *) ib->getBytesNoCopy();
		for (uint32_t y = 0; y < s->height && count < FILL_IBS_MAX; y++) {
			uint64_t row = surfaceAt + (uint64_t)y * s->bytesPerRow;
			uint32_t n = s->width + 2;
			uint32_t r = s->height > 1 ? y * 255 / (s->height - 1) : 0;

			if (at - start + n + 1 > FILL_IB_WORDS_MAX) {   /* this indirect buffer is full: the next one */
				addresses[count] = ibAt + 4ull * start;
				words[count++] = at - start;
				start = at;
			}
			w[at++] = 7u << 28 | n | fillParity(n) << 15 | FILL_CP_MEM_WRITE << 16 | fillParity(FILL_CP_MEM_WRITE) << 23;
			w[at++] = (uint32_t) row;
			w[at++] = (uint32_t)(row >> 32);
			for (uint32_t x = 0; x < s->width; x++) {
				uint32_t g = s->width > 1 ? x * 255 / (s->width - 1) : 0;

				w[at++] = 0xff000000u | r << 16 | g << 8 | 0x80u;
			}
		}
		if (count < FILL_IBS_MAX && at > start) {
			addresses[count] = ibAt + 4ull * start;
			words[count++] = at - start;
		}
		hakios_gpu_for_device(w, ibBytes);                     /* the commands out to memory */
		hakios_gpu_for_device((void *) px, size);              /* (and nothing of the CPU's left in the surface) */
		vayu_crumb("gpu fill: %u indirect buffers submitted\n", count);
		fence = count != 0 ? hakios_gpu_submit(addresses, words, count) : 0;
		if (fence != 0) {
			__atomic_store_n(&s->gpuFence, fence, __ATOMIC_RELEASE);
		}
	}
	IOLockUnlock(gGPULock);
	vayu_crumb("gpu fill: fence %u: waited for\n", fence);
	ok = fence != 0 && gpuWaitFree(fence, 2000);
	vayu_crumb("gpu fill: fence %u %s; unmapped next\n", fence, ok ? "done" : "NOT done");
	IOLog("HakiosGPU: fill: surface %u %ux%u (%llu bytes at GPU %#llx) from %u indirect buffer(s) at %#llx: fence %u %s\n",
	    id, s->width, s->height, size, surfaceAt, count, ibAt, fence, ok ? "done" : "NOT done");
	IOLockLock(gGPULock);
	if (ok || fence == 0) {                 /* (a GPU not done may still read or write there: left to it) */
		if (ibAt != 0) {
			hakios_gpu_unmap(ibAt, ibBytes);
			hakios_gpu_address_free(ibAt, ibBytes);
		}
		if (surfaceAt != 0) {
			hakios_gpu_unmap(surfaceAt, size);
			hakios_gpu_address_free(surfaceAt, size);
		}
		ib->release();
	}
	IOLockUnlock(gGPULock);
	vayu_crumb("gpu fill: done\n");
	*fenceOut = fence;
	OSSafeReleaseNULL(s);
	return ok;
}

/* The run is ending (hakios.adreno.c): nobody is in the middle of anything when the GPU goes. */
void
HakiosGPUStop(void)
{
	IOLockLock(gGPULock);
	hakios_gpu_stop();
	IOLockUnlock(gGPULock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *            memory;         /* NULL: this handle is free */
	uint8_t *                       kernel;         /* where it is in the kernel */
	SweetSurface *                  surface;        /* whose memory it is, when not its own (held) */
	uint64_t                        address;        /* the GPU's for it */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when that was */
	bool                            seen;           /* by the program: mapped, and from then on written by it */
	uint32_t                        fence;          /* the last submission that names it (0: none has) */
};

/*
 * A buffer given back while the GPU is still at it: its memory and its place in the GPU's
 * address space are kept until the submission is done, and nobody waits for that. (A window
 * server that makes a surface for every frame gives the last frame's back while the GPU draws
 * this one: waiting there, for all the GPU had been given, cost it some five milliseconds a
 * frame.)
 */
struct gpu_left {
	IOMemoryDescriptor *            memory;
	SweetSurface *                  surface;
	uint64_t                        address, size;
	uint32_t                        fence, flags;
};
#define GPU_LEFT                64

/*
 * A buffer of the program's own given back, and done with: kept, memory and place in the GPU's
 * address space, for the next one of the same size and kind. CoreAnimation makes a new texture to
 * draw a blur into for every frame of an animation with one (Search: 1082 x 1226, 6 MB with Mesa's
 * rounding), and gives them back only later, a score at a time; Mesa keeps what is given back for
 * a second and then lets it go. Each made afresh here took 2 to 3.5 ms of the window server's frame
 * (memory got, cleared, made write-combined, and entered page by page into the GPU's address
 * space). A buffer kept is given again as it is, not cleared: it was the same program's (Mesa's own
 * cache gives buffers back the same way). At most GPU_KEPT_BYTES are kept (the one kept longest
 * makes way), none for longer than GPU_KEPT_SECONDS. rc.jobs.fps, Search: 8 such textures made
 * slowly instead of 77, the window server's frames drawn in 6.8 ms instead of 9.3-10 (median).
 */
struct gpu_kept {
	IOMemoryDescriptor *            memory;         /* NULL: this place is free */
	uint8_t *                       kernel;
	uint64_t                        address, size, since;
	uint32_t                        flags;
};
#define GPU_KEPT                48
#define GPU_KEPT_BYTES          (160ull << 20)
#define GPU_KEPT_SECONDS        60              /* a buffer not asked for again in so long is let go */
#define GPU_KEEP_FROM           (256u << 10)    /* bytes: smaller ones are not worth it */
static TUNABLE(uint32_t, gGPUKeep, "gpu_keep", 1);      /* boot-arg gpu_keep=0 (a test): none kept; gpu_keep=2: kept ones cleared when given again */

class HakiosGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosGPUUserClient);

	task_t                  fTask;
	struct gpu_buffer *     fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextQueue;
	uint32_t                fNextBuffer;            /* where the search for a free handle starts: one given back is not the next given */
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	uint32_t                fStrangers;             /* requests that named a buffer the client does not have */
	struct gpu_left         fLeft[GPU_LEFT];
	uint32_t                fLeftCount;
	struct gpu_kept         fKept[GPU_KEPT];
	uint64_t                fKeptBytes;

	struct gpu_buffer * buffer(uint32_t handle);
	struct gpu_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle);
	void freeBuffer(struct gpu_buffer * b);
	bool keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags);
	void dropKept(struct gpu_kept * k);
	void reap(void);
	IOReturn gemNew(struct msm_gem_new * request);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn gemInfo(struct msm_gem_info * request);
	IOReturn submit(struct msm_gem_submit * request);
	IOReturn show(const struct hakios_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosGPUUserClient, IOUserClient);

bool
HakiosGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
HakiosGPUUserClient::start(IOService * provider)
{
	if (!hakios_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

/*
 * With the lock held, and the GPU done with everything it has been given (`idle`). If it is
 * not -- it has stopped answering -- the buffer's memory stays the GPU's for good: it may yet be
 * written, and must not be somebody else's by then.
 */
void
HakiosGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
	bool busy = hakios_gpu_ready() && b->fence != 0 && !hakios_gpu_done(b->fence);

	if (busy && fLeftCount == GPU_LEFT) {
		busy = !gpuWait(b->fence, 1000);        /* (nowhere to keep it: this one is waited for) */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
	} else {
		if (busy) {
			static uint32_t said;

			b->memory->retain();
			if (said++ < 8) {
				IOLog("HakiosGPU: the GPU is not done with what it was given: a buffer of %llu bytes stays its own\n", b->size);
			}
		} else if (b->surface == NULL && hakios_gpu_ready() && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;               /* (now the kept one's) */
		} else if (hakios_gpu_ready()) {
			hakios_gpu_unmap(b->address, b->size);
			hakios_gpu_address_free(b->address, b->size);
		}
		if (b->memory != NULL) {
			b->memory->release();
		}
		OSSafeReleaseNULL(b->surface);
	}
	b->memory = NULL;
	b->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
HakiosGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (hakios_gpu_ready() && !hakios_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && hakios_gpu_ready() &&
		    keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (hakios_gpu_ready()) {
			hakios_gpu_unmap(left->address, left->size);
			hakios_gpu_address_free(left->address, left->size);
		}
		if (left->memory != NULL) {
			left->memory->release();
		}
		OSSafeReleaseNULL(left->surface);
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
HakiosGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (hakios_gpu_ready()) {
			gpuWait(hakios_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("HakiosGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
HakiosGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
HakiosGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HakiosGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: there is no room.
 */
struct gpu_buffer *
HakiosGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = hakios_gpu_address(size)) == 0) {
		IOLog("HakiosGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	hakios_gpu_mapped();
	if (!mapped) {
		hakios_gpu_unmap(b->address, size);
		hakios_gpu_address_free(b->address, size);
		IOLog("HakiosGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
void
HakiosGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return;
	}
	if (hakios_gpu_ready()) {
		hakios_gpu_unmap(k->address, k->size);
		hakios_gpu_address_free(k->address, k->size);
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
HakiosGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		dropKept(oldest);
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
HakiosGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	IOLockLock(gGPULock);
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old) {
			dropKept(k);
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			IOLockUnlock(gGPULock);
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
				hakios_gpu_to_memory(b->kernel, b->size);
			}
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(gGPULock);
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("HakiosGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	/*
	 * (SPEC-K8, after macOS-uke 0008: a new write-combined buffer is not trusted to be out of the cache -- its pages
	 * have a write-back alias in the physical aperture on this kernel): cleaned and invalidated, before the client
	 * has it, whatever its caching.
	 */
	hakios_gpu_to_memory(memory->getBytesNoCopy(), size);

	IOLockLock(gGPULock);
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h) -- or, for a surface whose pixels are a scanout frame (boot-arg
 * fb_zero=1), from the frame itself. The surface is held for as long as the buffer is there. A
 * surface that is a buffer of this client's already is that buffer again, as with Linux's handles:
 * so the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kHakiosDescribe), no buffer is made.
 */
IOReturn
HakiosGPUUserClient::surfaceBuffer(struct hakios_surface * request)
{
	SweetSurface * s = sweet_surface_copy(request->surface);
	struct gpu_buffer * b = NULL;
	uint64_t size, began = mach_absolute_time();
	uintptr_t kernel;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->memory == NULL || s->prepared || s->snapTask != NULL) {
		/* (a program's own memory with a surface made around it: its lifetime is the program's, not the GPU's) */
		OSSafeReleaseNULL(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->allocSize);
	kernel = s->kernelAddress();
	if (size == 0 || size > GPU_BUFFER_MAX || size > s->memory->getLength() || kernel == 0) {
		OSSafeReleaseNULL(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->offset;
	request->rowBytes = s->bytesPerRow;
	request->width = s->width;
	request->height = s->height;
	request->pixelFormat = s->pixelFormat;
	request->planes = s->planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		OSSafeReleaseNULL(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		OSSafeReleaseNULL(s);           /* (the buffer holds it already) */
	} else if ((b = newBuffer(s->memory, 0, size, 0, &request->handle)) != NULL) {
		s->memory->retain();
		b->memory = s->memory;
		b->kernel = (uint8_t *) kernel;
		b->surface = s;                 /* (sweet_surface_copy's reference) */
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		OSSafeReleaseNULL(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
HakiosGPUUserClient::gemInfo(struct msm_gem_info * request)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t) request->handle << 32;      /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		request->value = b->address;
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {            /* a name for somebody's debugger: not kept */
		ret = kIOReturnUnsupported;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
HakiosGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t began, nanos, again;
	IOReturn ret = kIOReturnBadArgument;

	if (request->nr_cmds == 0 || request->nr_cmds > GPU_SUBMIT_CMDS || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("HakiosGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL) {
			continue;
		}
		if (b->surface == NULL && (!b->seen || !gGPUCached)) {
			continue;
		}
		/* (a surface: every submission -- who else writes it does not always say so; the CPU's lines to memory) */
		hakios_gpu_for_device(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	hakios_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("HakiosGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = hakios_gpu_submit(addresses, words, count);
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE at US: N buffers, B of 4 MB or more, W words of commands": what was given (tl: q: what it took) */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		hakios_tl("tl: Q %u at %llu: %u buffers, %u big, %u words\n", request->fence, ns / 1000, request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * (freedreno names every buffer for reading and writing alike, so which it is cannot be
	 * told; the last submission of a frame names the surface the frame is drawn into.)
	 * The fence is stored with release: the display's driver reads it, waits for that fence, and
	 * then reads the pixels the core wrote under it -- which is where frames came out half drawn
	 * when the last submission of a frame named another surface than the frame's
	 * (MiniIOSurfaceFenceNow() and MiniIOSurfaceRedrawn() are what tell of that).
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				__atomic_store_n(&b->surface->gpuFence, request->fence, __ATOMIC_RELEASE);
			}
		}
	}
	reap();
	if (request->fence == 0 && !gGPUReported) {
		gGPUReported = true;
		hakios_gpu_report("the ring is full");
	}
	if (fSubmits++ < 4) {
		IOLog("HakiosGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		hakios_gpu_worked(&worked, &timed, by);
		uint64_t making;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		IOLog("HakiosGPU: buffers: %u made in %llu us each, %u kept ones given again (%llu MB kept)\n", gGPUStat.made,
		    gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.given_again, fKeptBytes >> 20);
		IOLog("HakiosGPU: %u submissions in %llu ms: %u command buffers and %u buffers named, %llu KB out of the cache "
		    "in %llu us a submission; the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all); %u waits of %llu us, %u over before they "
		    "began; %u IOSurfaces taken as buffers in %llu us each; %u buffers, %llu MB\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions,
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000,
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}

/* Pixels of the caller's (32 bits each, blue first) copied to the panel: for a test to show what it has drawn. */
IOReturn
HakiosGPUUserClient::show(const struct hakios_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		sweet_fb_vayu_show(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

IOReturn
HakiosGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		b->seen = true;
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

/* the length a request's structure may have: its oldest form .. its current one (msm_drm.h, drm.h, ours) */
static bool
requestLengths(uint32_t selector, size_t * least, size_t * most)
{
	switch (selector) {
	case kMsmGetParam:
	case kMsmSetParam:
		*least = offsetof(struct msm_param, len);       /* (len and pad came with 5.x's SET_PARAM strings) */
		*most = sizeof(struct msm_param);
		return true;
	case kMsmGemNew:
		*least = *most = sizeof(struct msm_gem_new);
		return true;
	case kMsmGemInfo:
		*least = offsetof(struct msm_gem_info, len);
		*most = sizeof(struct msm_gem_info);
		return true;
	case kMsmGemCpuPrep:
		*least = *most = sizeof(struct msm_gem_cpu_prep);
		return true;
	case kMsmGemCpuFini:                            /* struct msm_gem_cpu_fini { u32 handle; } */
	case kMsmSubmitQueueClose:                      /* a u32 id */
		*least = 4;
		*most = 8;
		return true;
	case kMsmGemSubmit:
		*least = offsetof(struct msm_gem_submit, in_syncobjs);  /* (the syncobjs came with 5.8) */
		*most = sizeof(struct msm_gem_submit);
		return true;
	case kMsmWaitFence:
		*least = offsetof(struct msm_wait_fence, queueid);
		*most = sizeof(struct msm_wait_fence);
		return true;
	case kMsmGemMadvise:
		*least = *most = sizeof(struct msm_gem_madvise);
		return true;
	case kMsmSubmitQueueNew:
		*least = *most = sizeof(struct msm_submitqueue);
		return true;
	case kMsmSubmitQueueQuery:
		*least = *most = sizeof(struct msm_submitqueue_query);
		return true;
	case kDrmGemClose:
		*least = *most = sizeof(struct drm_gem_close);
		return true;
	case kHakiosSurface:
		*least = *most = sizeof(struct hakios_surface);
		return true;
	case kHakiosShow:
		*least = *most = sizeof(struct hakios_show);
		return true;
	default:
		return false;
	}
}

IOReturn
HakiosGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct hakios_surface surface;
		struct hakios_show show;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience, waitBegan;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	/*
	 * Each request's own length: from its oldest form libdrm sends (msm_drm.h grew some at the end; the rest is zero, as
	 * Linux's drm_ioctl() fills it) to its current one. A length outside that is refused, and so is a selector that is
	 * not one of these. (The 8796 line took any length up to the union's and zero-filled the rest.)
	 */
	{
		size_t least = 0, most = 0;

		if (!requestLengths(selector, &least, &most)) {
			IOLog("HakiosGPU: request %#x (%zu bytes) is not known here\n", selector, size);
			return kIOReturnUnsupported;
		}
		if (size < least || size > most) {
			static uint32_t said;

			if (said++ < 16) {
				IOLog("HakiosGPU: request %#x of %zu bytes refused (%zu..%zu)\n", selector, size, least, most);
			}
			return kIOReturnBadArgument;
		}
	}
	if (!hakios_gpu_ready() && selector != kHakiosShow) {
		return kIOReturnNotReady;                       /* given back: the run is ending */
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		ret = hakios_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kMsmSetParam:                              /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                            /* is the GPU done with a buffer: with all it has been given, here */
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (!gpuWait(hakios_gpu_last(), patience)) {
			ret = kIOReturnBusy;
			if (patience == 0 && !(r.cpuPrep.op & MSM_PREP_NOSYNC)) {
				waitOver(&r.cpuPrep.timeout, hakios_gpu_last());
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("cpu-prep", waitBegan);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		waitBegan = mach_absolute_time();
		IOLockLock(gGPULock);
		patience = millisecondsUntil(&r.waitFence.timeout);
		if (!gpuWait(r.waitFence.fence, patience)) {
			ret = kIOReturnTimeout;
			if (patience == 0) {
				waitOver(&r.waitFence.timeout, r.waitFence.fence);
			}
		}
		IOLockUnlock(gGPULock);
		slowWait("fence", waitBegan);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:                      /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			copyout(&zero, (user_addr_t) r.query.data, sizeof(zero));
		}
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap();
		}
		IOLockUnlock(gGPULock);
		break;
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("HakiosGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
