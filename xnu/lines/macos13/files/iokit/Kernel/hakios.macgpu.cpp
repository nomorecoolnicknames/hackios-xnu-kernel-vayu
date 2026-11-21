/*
 * AGXAccelerator: the Adreno as macOS's Metal looks for a GPU (boot-arg macos_gpu=1; default 0:
 * there is no such service, and nothing here runs).
 *
 * macOS 13.4's Metal.framework (MTLRegisterDevices) asks the registry for the services of class
 * IOAccelerator -- that name and no other (the lab's reading of 22F66's Metal, the string next to
 * "MetalPluginName" and "IOServiceGetMatchingServices failed"; IOAcceleratorES, which the iOS side
 * publishes as "sgx" in MiniIOMobileFramebuffer.cpp, is not in macOS's Metal at all). Of each it
 * reads MetalPluginName and MetalPluginClassName, opens
 * /System/Library/Extensions/<MetalPluginName>.bundle and sends -initWithAcceleratorPort: this
 * service to the class (userland/src/HakiosMetalCA.m, HakiosMetalDevice). CoreDisplay then takes
 * the device's -acceleratorPort to tell which accelerator draws for the display; a device that
 * gives none leaves WindowServer on its CPU compositor.
 *
 * The real IOAccelerator is IOGraphicsFamily's, which this kernel has not got (it has no kexts):
 * the class is an empty one of that name (MiniIOMobileFramebuffer.cpp's, declared in MiniDrivers.h:
 * one class of that name in the kernel), so that matching by class finds the service. It
 * would collide with the real one; nothing here loads IOGraphicsFamily. The service keeps Apple's
 * GPU's name, AGXAccelerator, as macOS-uke's does, because macOS's sandbox profiles name the
 * accelerator and its user clients by class (AGXAccelerator, AGXDeviceUserClient).
 *
 * Opened with type 0 it is HakiosGPU (hakios.gpu.cpp): the same user client, the same requests,
 * so that a plug-in given this port can draw through it without looking HakiosGPU up by name.
 * Other types are what Apple's own Metal driver would open (IOGPU's device, its queues); they are
 * said in the log, once each, and refused -- but for type 5 with macos_accel_device=1, below. Without the GPU (QEMU, or no adreno_keep) the service
 * is there all the same and every open fails: Metal then loads the plug-in, which finds no GPU
 * and gives no device, which is what macOS on a machine without one is to see.
 *
 * Boot-arg macos_accel_device=1 (default 0: type 5 refused as before): type 5 is the accelerator's device
 * client, AGXDeviceUserClient below -- what IOAccelerator.framework's IOAccelDeviceCreate opens (22F66:
 * IOServiceOpen(service, task, 5)), and IOGPU.framework's IOGPUDeviceCreate (type 5, its API number in the
 * upper 16 bits). WindowServer's CoreDisplay opens it for the accelerator of the device's -acceleratorPort
 * (CGXMPRegisterScreen: IOAccelDeviceCreate, IOAccelDeviceGetConfig64, IOSurfaceBindAccel of the display's
 * surfaces with the configuration's accelerator number, IOAccelDeviceRelease) and goes on without it when it
 * is refused. What the two frameworks read of it, and so what is answered (22F66's code; no header of Apple's
 * says it):
 *   0  the configuration, 64 bytes out: five numbers IOAccelDeviceGetConfig64 hands back, the fifth (at 0x18)
 *      the accelerator's number IOSurfaceBindAccel is given; both creates fail without it
 *   1  the name, 64 bytes out (IOAccelDeviceGetName)
 *   2  the device's shared memory, 0x258 bytes out (IOAccelerator) or 0x218 (IOGPU): its address, the number
 *      of stamp segments and their offsets, and (IOAccelerator only, at 0x18) the name of the function
 *      IOAccelDeviceTestEvent calls, which IOAccelDeviceCreate looks up with dlsym and fails without. Here: no
 *      shared memory and no stamps (nothing is submitted through this client: HakiosGPU's type 0 is the GPU),
 *      and IOAccelerator's own IOAccelDeviceTestEventFast, the stamp test over that memory
 *   7  IOAccelerator: more of the configuration, 24 bytes out (optional: refused); IOGPU: its API's name in
 *   9  IOAccelerator: the API's name in (Metal, CoreDisplay, ...): taken, said once
 * Anything else is said once and refused.
 *
 * Boot-arg macos_gpu_plugin=NAME: the bundle's name instead of HakiosMetal (Metal puts it between
 * "/System/Library/Extensions/" and ".bundle": a name with "../" in it reaches a bundle outside
 * the system volume, as the lab's ../../../redwood/lib/RedwoodMetal did).
 *
 * After macOS-uke's gpu/src/macos/UkeGpuAccelerator.cpp (github.com/iOSNJBrepo/macOS-uke, commit
 * eaa695ace7e58308512126e25f4b256292a78c51, BSD-2-Clause-Patent): the class layout, the two
 * properties and the reason for the name are that file's; the code is written anew for this kernel.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <sys/proc.h>
#include <os/atomic_private.h>

#include "MiniDrivers.h"

extern "C" void loader_log_putc(char c);

/* As hakios.gpu.cpp: the loader's log too, which is the one a run's log keeps. */
static void macLog(const char * format, ...) __printflike(1, 2);
static void
macLog(const char * format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char * c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	IOLog("%s", line);
}

/*
 * Type 5 (macos_accel_device=1): the device client, the name macOS's sandbox profiles give Apple's
 * (AGXDeviceUserClient). See the top of the file for what each request is.
 */
class AGXDeviceUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AGXDeviceUserClient);

	char fName[MAXCOMLEN + 1];
	char fAPI[17];

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient);

enum {
	kAGXDeviceGetConfig     = 0,
	kAGXDeviceGetName       = 1,
	kAGXDeviceGetShmem      = 2,
	kAGXDeviceSetAPIIOGPU   = 7,            /* (IOAccelerator's 7 is its configuration's second part) */
	kAGXDeviceSetAPI        = 9,
};

/* IOSurface's number of this accelerator (IOSurfaceBindAccel): MiniIOSurface keeps what it is told, nothing reads it */
#define AGX_ACCEL_ID            1
#define AGX_SHMEM_IOACCEL       0x258   /* IOAccelDeviceCreateWithAPIProperty's request 2 */
#define AGX_SHMEM_TEST_EVENT    0x18    /* ... the name of the event test in it */

bool
AGXDeviceUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	proc_selfname(fName, sizeof(fName));
	return true;
}

IOReturn
AGXDeviceUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AGXDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	static uint32_t told;                   /* the requests refused so far, a bit each (0 to 31) */
	uint8_t * out = (uint8_t *) args->structureOutput;
	uint32_t size = args->structureOutputSize;

	(void) dispatch;
	(void) target;
	(void) reference;
	switch (selector) {
	case kAGXDeviceGetConfig:
		if (out == NULL || size < 0x40) {
			return kIOReturnBadArgument;
		}
		bzero(out, size);
		*(uint32_t *) (out + 0x18) = AGX_ACCEL_ID;
		return kIOReturnSuccess;

	case kAGXDeviceGetName:
		if (out == NULL || size == 0) {
			return kIOReturnBadArgument;
		}
		bzero(out, size);
		strlcpy((char *) out, "HakiOS Adreno 640", size);
		return kIOReturnSuccess;

	case kAGXDeviceGetShmem:                /* no shared memory, no stamp segments */
		if (out == NULL || size < 0x18) {
			return kIOReturnBadArgument;
		}
		bzero(out, size);
		if (size >= AGX_SHMEM_IOACCEL) {
			strlcpy((char *) out + AGX_SHMEM_TEST_EVENT, "IOAccelDeviceTestEventFast", 0x40);
		}
		return kIOReturnSuccess;

	case kAGXDeviceSetAPIIOGPU:
	case kAGXDeviceSetAPI:
		if (args->structureInput != NULL && args->structureInputSize > 0 && args->structureInputSize <= 16) {
			bool first = fAPI[0] == 0;

			bzero(fAPI, sizeof(fAPI));
			memcpy(fAPI, args->structureInput, args->structureInputSize);
			if (first) {
				macLog("AGXAccelerator: %s's device client is for %s\n", fName, fAPI);
			}
			return kIOReturnSuccess;
		}
		break;
	}
	if (selector < 32 && !(os_atomic_or_orig(&told, 1u << selector, relaxed) & (1u << selector))) {
		macLog("AGXAccelerator: %s's device client asks %u (scalars %u in, %u out; structure %u in, %u out): refused\n",
		    fName, selector, args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, size);
	}
	return kIOReturnUnsupported;
}

class AGXAccelerator : public IOAccelerator
{
	OSDeclareDefaultStructors(AGXAccelerator);

public:
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAccelerator);

static bool sDeviceClients;             /* boot-arg macos_accel_device=1 */

IOReturn
AGXAccelerator::newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
    IOUserClient ** handler)
{
	static uint32_t told;                   /* the types refused so far, a bit each (types 1 to 31), and */
	static uint32_t toldOther;              /* ... how many others were said */
	static uint32_t opened;
	char name[32];
	IOReturn ret;

	proc_selfname(name, sizeof(name));
	if ((type & 0xffff) == 5 && sDeviceClients) {
		AGXDeviceUserClient * client = OSTypeAlloc(AGXDeviceUserClient);

		if (client == NULL || !client->initWithTask(owningTask, securityID, type, properties) || !client->attach(this)) {
			OSSafeReleaseNULL(client);
			return kIOReturnNoMemory;
		}
		if (!client->start(this)) {
			client->detach(this);
			client->release();
			return kIOReturnError;
		}
		*handler = client;
		if (os_atomic_inc_orig(&opened, relaxed) < 16) {
			macLog("AGXAccelerator: %s[%d] opens type %#x: the device client\n", name, proc_selfpid(), (unsigned) type);
		}
		return kIOReturnSuccess;
	}
	if (type != 0) {
		bool say = type < 32 ? !(os_atomic_or_orig(&told, 1u << type, relaxed) & (1u << type)) :
		    os_atomic_inc_orig(&toldOther, relaxed) < 8;

		if (say) {
			macLog("AGXAccelerator: %s[%d] opens type %u: not served (only type 0, HakiosGPU's)\n", name, proc_selfpid(),
			    (unsigned) type);
		}
		return kIOReturnUnsupported;
	}
	/* (IOService's own: the class of kIOUserClientClassKey, made, attached here and started) */
	ret = IOService::newUserClient(owningTask, securityID, type, properties, handler);
	if (os_atomic_inc_orig(&opened, relaxed) < 16 || ret != kIOReturnSuccess) {
		macLog("AGXAccelerator: %s[%d] opens type 0: %s (%#x)\n", name, proc_selfpid(),
		    ret == kIOReturnSuccess ? "HakiosGPU's user client" : "no GPU to draw with", ret);
	}
	return ret;
}

void
HakiosMacAcceleratorStart(IOService * provider)
{
	AGXAccelerator * accelerator;
	uint32_t macos = 0, device = 0;
	char plugin[128] = "HakiosMetal";

	if (!PE_parse_boot_argn("macos_gpu", &macos, sizeof(macos)) || macos == 0) {
		return;
	}
	(void) PE_parse_boot_argn("macos_gpu_plugin", plugin, sizeof(plugin));
	if (PE_parse_boot_argn("macos_accel_device", &device, sizeof(device)) && device != 0) {
		sDeviceClients = true;
	}
	accelerator = OSTypeAlloc(AGXAccelerator);
	if (accelerator == NULL || !accelerator->init() || !accelerator->attach(provider)) {
		OSSafeReleaseNULL(accelerator);
		macLog("AGXAccelerator: macos_gpu=%u: cannot create the service\n", macos);
		return;
	}
	accelerator->setName("AGXAccelerator");
	accelerator->setProperty("MetalPluginName", plugin);
	accelerator->setProperty("MetalPluginClassName", "HakiosMetalDevice");
	accelerator->setProperty("model", "HakiOS Adreno 640");
	accelerator->setProperty(kIOUserClientClassKey, "HakiosGPUUserClient");
	accelerator->registerService();
	macLog("AGXAccelerator: macos_gpu=%u: published (IOAccelerator, MetalPluginName %s, MetalPluginClassName "
	    "HakiosMetalDevice, registry ID %#llx)%s\n", macos, plugin, accelerator->getRegistryEntryID(),
	    sDeviceClients ? "; type 5, the device client, served (macos_accel_device=1)" : "");
	accelerator->release();
}
