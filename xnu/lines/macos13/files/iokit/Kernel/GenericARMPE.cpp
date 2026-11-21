/*
 * GenericARMPE: a minimal platform expert for non-Apple ARMv8 boards.
 *
 * On Apple hardware the platform expert comes from a closed kext
 * (AppleARMPlatform). Without one, IOKit falls through to IOPanicPlatform
 * ("Unable to find driver for this platform"). This class is the least that
 * lets IOKit start on a board described by a loader-built device tree:
 * it publishes the device-tree nodes as nubs and names the machine.
 */
extern "C" {
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <kern/cpu_number.h>
}
#include <machine/machine_routines.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCPU.h>
#include <kern/clock.h>
#include <kern/thread_call.h>

#include "MiniDrivers.h"

extern "C" {
kern_return_t processor_start(processor_t processor);
kern_return_t processor_exit(processor_t processor);
extern uint32_t processor_avail_count;

/* osfmk/arm64/psci_smp.c and gic_v3_irq.c */
void psci_smp_init(void);
kern_return_t psci_smp_start_cpu(int cpu);
void psci_smp_cpu_running(void);
void psci_smp_cpu_off(void) __dead2;
void gic_v3_irq_signal(uint32_t phys_id);
}

/*
 * GenericARMCPU: registers a device-tree CPU node with the kernel. The closed
 * AppleARMCPU driver does this on Apple hardware; without it the kernel waits
 * forever for the CPU count (ml_wait_max_cpus) and never calls
 * cpu_machine_init() on the boot processor.
 *
 * The other processors are started through the firmware (PSCI CPU_ON, see
 * osfmk/arm64/psci_smp.c) and signalled with interrupts of the GIC
 * (osfmk/arm64/gic_v3_irq.c). How many there are is the device tree's and the
 * boot-arg cpus=N's to say (ml_parse_cpu_topology()); with one, nothing of
 * that is used.
 */
class GenericARMCPU : public IOCPU
{
	OSDeclareDefaultStructors(GenericARMCPU);

	uint32_t physID;        /* the device tree's "reg": the low affinity fields of the core's MPIDR */

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual void initCPU(bool boot) APPLE_KEXT_OVERRIDE;
	virtual void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
	virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
	virtual void haltCPU(void) APPLE_KEXT_OVERRIDE;
	virtual void signalCPU(IOCPU * target) APPLE_KEXT_OVERRIDE;
	virtual const OSSymbol * getCPUName(void) APPLE_KEXT_OVERRIDE;

	static int cpuNumberOfNub(IOService * nub);
	void run(void);
};

OSDefineMetaClassAndStructors(GenericARMCPU, IOCPU);

/* The kernel's number for the processor of a /cpus node, -1 if it is not to be used (boot-arg cpus=N). */
int
GenericARMCPU::cpuNumberOfNub(IOService * nub)
{
	OSData * reg = OSDynamicCast(OSData, nub->getProperty("reg"));

	if (nub->getProperty("state") == NULL || reg == NULL || reg->getLength() < sizeof(uint32_t)) {
		return -1;
	}
	return ml_get_cpu_number(*(const uint32_t *)reg->getBytesNoCopy());
}

/* Registers the processor; run() starts it. */
bool
GenericARMCPU::start(IOService * provider)
{
	ml_processor_info_t info;
	perfmon_interrupt_handler_func pmi_handler = NULL;
	int cpu = cpuNumberOfNub(provider);

	if (cpu < 0 || !IOCPU::start(provider)) {
		return false;
	}
	const ml_topology_cpu_t * topology = &ml_get_topology_info()->cpus[cpu];

	physID = topology->phys_id;
	setCPUNumber((UInt32)cpu);

	bzero(&info, sizeof(info));
	info.cpu_id = (cpu_id_t)this;
	info.log_id = getCPUNumber();
	info.phys_id = physID;
	info.cluster_id = topology->cluster_id;
	info.cluster_type = topology->cluster_type;
	if (ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi_handler) != KERN_SUCCESS) {
		IOLog("GenericARMCPU: ml_processor_register failed\n");
		return false;
	}

	setCPUState(kIOCPUStateUninitalized);
	registerService();
	IOLog("GenericARMCPU: cpu %u (phys id 0x%x) registered\n", (unsigned)getCPUNumber(), physID);
	return true;
}

/*
 * The boot processor: runs cpu_machine_init() -> initCPU(true) on it, right here.
 * Any other: cpu_start() -> startCPU(), and the new core comes to initCPU() by itself.
 */
void
GenericARMCPU::run(void)
{
	processor_start(machProcessor);
}

/* On the processor itself, with its interrupts off, when it joins (or rejoins) the scheduler. */
void
GenericARMCPU::initCPU(bool boot)
{
	/* The interrupt controller and timer were set up in arm_init / arm_init_cpu (pe_init_fiq). */
	setCPUState(kIOCPUStateRunning);
	if (ml_get_cpu_count() > 1) {
		psci_smp_cpu_running();
		/*
		 * A processor takes no signals until it has handled an interrupt for one
		 * (cpu_signal_handler() lifts SIGPdisabled), and until then it does not go to
		 * sleep when idle either: send it its first.
		 */
		gic_v3_irq_signal(physID);
	}
}

/* On the processor itself, at the end of processor_exit(): it is to stop for good. */
void
GenericARMCPU::quiesceCPU(void)
{
	if (getCPUNumber() != (UInt32)master_cpu) {
		setCPUState(kIOCPUStateStopped);
		psci_smp_cpu_off();
	}
}

kern_return_t
GenericARMCPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
	return psci_smp_start_cpu((int)getCPUNumber());
}

void
GenericARMCPU::haltCPU(void)
{
	if (getCPUNumber() != (UInt32)master_cpu) {
		processor_exit(machProcessor);
	}
}

void
GenericARMCPU::signalCPU(IOCPU * target)
{
	gic_v3_irq_signal(((GenericARMCPU *)target)->physID);
}

const OSSymbol *
GenericARMCPU::getCPUName(void)
{
	char name[32];
	snprintf(name, sizeof(name), "Primary%u", (unsigned)getCPUNumber());
	return OSSymbol::withCString(name);
}

class GenericARMPE : public IODTPlatformExpert
{
	OSDeclareDefaultStructors(GenericARMPE);

public:
	virtual IOService * probe(IOService * provider, SInt32 * score) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual const char * deleteList(void) APPLE_KEXT_OVERRIDE;
	virtual const char * excludeList(void) APPLE_KEXT_OVERRIDE;
	virtual bool getMachineName(char * name, int maxLength) APPLE_KEXT_OVERRIDE;
	virtual long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
	virtual void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
};

#define super IODTPlatformExpert

OSDefineMetaClassAndStructors(GenericARMPE, IODTPlatformExpert);

IOService *
GenericARMPE::probe(IOService * provider, SInt32 * score)
{
	/*
	 * IODTPlatformExpert::probe() wants an IONameMatch against the root
	 * node; this expert is the only one built in, so it takes any board.
	 */
	if (!IOPlatformExpert::probe(provider, score)) {
		return NULL;
	}
	return this;
}

bool
GenericARMPE::start(IOService * provider)
{
	if (!super::start(provider)) {
		return false;
	}
	IOLog("GenericARMPE: platform \"%s\"\n", provider->getName());

	/*
	 * super::start() published the /cpus children as nubs. Attach a CPU driver to each
	 * one the kernel has a number for, register them all -- that settles the CPU count --
	 * and then start them, the boot processor (number master_cpu) first.
	 */
	static GenericARMCPU * cpus[MAX_CPUS];        /* (static: the slower cores may be started later, see below) */
	unsigned int count = 0;
	OSIterator * iter = getChildIterator(gIOServicePlane);
	if (iter != NULL) {
		OSObject * obj;
		while ((obj = iter->getNextObject()) != NULL) {
			IOService * nub = OSDynamicCast(IOService, obj);
			int number = nub ? GenericARMCPU::cpuNumberOfNub(nub) : -1;

			if (number < 0 || number >= MAX_CPUS || cpus[number] != NULL) {
				continue;
			}
			GenericARMCPU * cpu = new GenericARMCPU;
			if (cpu != NULL && cpu->init() && cpu->attach(nub)) {
				if (cpu->start(nub)) {
					cpus[number] = cpu;     /* kept: the kernel has its address as the cpu's id */
					count++;
					continue;
				}
				cpu->detach(nub);
			}
			if (cpu != NULL) {
				cpu->release();
			}
		}
		iter->release();
	}
	if (cpus[master_cpu] != NULL) {
		/* Unblocks everything that waits for the CPU count to be known. */
		ml_set_max_cpus(count);
		if (count > 1) {
			psci_smp_init();
		}
		/*
		 * The slower cores (an L2 access penalty, the loader's "small-core") are started 28 seconds
		 * later (boot-arg small_cores_after=S; 0: at once). With all seven from the start the lock
		 * screen came 1.5 s later than with the four big ones alone (26.5-27.0 s after the kernel
		 * began instead of 25.3-25.5: start-up's other work shares memory and cache with
		 * SpringBoard's), while that work as a whole was done earlier (34 s instead of 38). With
		 * the slower cores joining once the lock screen is there: 25.3 s and 36 s.
		 */
		uint32_t later = 28;
		static thread_call_t later_call;

		PE_parse_boot_argn("small_cores_after", &later, sizeof(later));
		cpus[master_cpu]->run();
		for (unsigned int number = 0; number < MAX_CPUS; number++) {
			if (cpus[number] != NULL && number != (unsigned int)master_cpu) {
				if (later != 0 && ml_get_topology_info()->cpus[number].l2_access_penalty != 0) {
					continue;
				}
				cpus[number]->run();
			}
		}
		if (later != 0) {
			uint64_t deadline;

			later_call = thread_call_allocate([](thread_call_param_t, thread_call_param_t) {
				for (unsigned int number = 0; number < MAX_CPUS; number++) {
					if (cpus[number] != NULL && ml_get_topology_info()->cpus[number].l2_access_penalty != 0) {
						cpus[number]->run();
					}
				}
				IOLog("GenericARMPE: the slower cores started too: %u processors running\n", processor_avail_count);
			}, NULL);
			clock_interval_to_deadline(later, NSEC_PER_SEC, &deadline);
			thread_call_enter_delayed(later_call, deadline);
		}
		if (count > 1) {
			IOLog("GenericARMPE: %u of %u processors are running\n", processor_avail_count, count);
		}
	}

	/* BSD looks the platform expert up by class (IOSecureBSDRoot). */
	registerService();

	/* Stand-ins for the closed graphics drivers that the iOS window server talks to. */
	MiniIOSurfaceStart(this);
	MiniIOMobileFramebufferStart(this);
	HakiosMacAcceleratorStart(this);        /* boot-arg macos_gpu=1 only */
	MiniAppleKeyStoreStart();
	MiniIONetworkStackStart();
	MiniAppleStockholmStart();      /* boot-arg vayu_nfc_ios=1 only */
	if (hakios_macos_abi != 0) {
		MiniIOHIDSystemStart(this);     /* what macOS's WindowServer opens first (below) */
		MiniEndpointSecurityStart(this);        /* boot-arg macos_es=1 only (below) */
		apfs_media_iokit_ready();               /* the disks and APFS containers as macOS's nodes (HakiosAPFSMedia.cpp) */
	}
	VayuTouchPointerStart();        /* boot-arg macos_hid=1 only */
	return true;
}

/*
 * The time of day. There is no driver for a real-time clock here; whoever started the kernel
 * says what time it was then -- the boot-arg epoch= (seconds since 1970, put there by the
 * host scripts) or, failing that, the property boot-epoch of /chosen (the loader, from the
 * firmware's clock) -- and the kernel's own timer counts from there. Without either the
 * calendar starts in 1970, as before.
 */
static long gBootEpoch;         /* the calendar time at which the system timer read zero */
static bool gBootEpochKnown;

static void
findBootEpoch(void)
{
	uint64_t epoch = 0;

	if (gBootEpochKnown) {
		return;
	}
	gBootEpochKnown = true;
	if (!PE_parse_boot_argn("epoch", &epoch, sizeof(epoch)) || epoch == 0) {
		IORegistryEntry * chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
		OSData * data = chosen ? OSDynamicCast(OSData, chosen->getProperty("boot-epoch")) : NULL;

		if (data != NULL && data->getLength() == sizeof(epoch)) {
			memcpy(&epoch, data->getBytesNoCopy(), sizeof(epoch));
		}
		OSSafeReleaseNULL(chosen);
	}
	if (epoch != 0) {
		clock_sec_t secs;
		clock_usec_t usecs;

		clock_get_system_microtime(&secs, &usecs);
		gBootEpoch = (long) epoch - (long) secs;
	}
}

long
GenericARMPE::getGMTTimeOfDay(void)
{
	clock_sec_t secs;
	clock_usec_t usecs;

	findBootEpoch();
	if (gBootEpoch == 0) {
		return 0;
	}
	clock_get_system_microtime(&secs, &usecs);
	return gBootEpoch + (long) secs;
}

void
GenericARMPE::setGMTTimeOfDay(long secs)
{
	clock_sec_t uptime;
	clock_usec_t usecs;

	clock_get_system_microtime(&uptime, &usecs);
	gBootEpoch = secs - (long) uptime;
	gBootEpochKnown = true;
}

/* Device-tree nodes to drop entirely. */
const char *
GenericARMPE::deleteList(void)
{
	return "('packages', 'psuedo-usb', 'psuedo-hid', 'multiboot', 'rtas')";
}

/* Top-level nodes that describe boot state rather than devices: no nubs for these. */
const char *
GenericARMPE::excludeList(void)
{
	return "('chosen', 'memory', 'options', 'aliases', 'defaults', 'pram', 'cpus')";
}

/*
 * hw.machine. On Apple's devices it is the product type ("iPhone10,3"), from the device tree's
 * "model", and iOS decides from it what kind of device it is on: with anything it does not know
 * MobileGestalt settles for an iPod touch.
 */
bool
GenericARMPE::getMachineName(char * name, int maxLength)
{
	uint32_t real = 1;

	/*
	 * hw.machine is the model the loader put into the device tree, "iPhone10,3". It has to
	 * begin with "iPhone": CoreFoundation picks the device-specific variants of resources
	 * (Something~iphone.plist, Key~iphone in an Info.plist) by this string, and for a name it
	 * does not know it picks none at all -- Control Center, for one, then lays its modules out
	 * without their sizes. (boot-arg machine_name=0: the plain "arm64" this used to be.)
	 */
	PE_parse_boot_argn("machine_name", &real, sizeof(real));
	if (!real || !IODTPlatformExpert::getMachineName(name, maxLength) || name[0] == 0) {
		strlcpy(name, "arm64", maxLength);
	}
	return true;
}

/*
 * MiniIOHIDSystem: the registry entry called "IOHIDSystem", which the macOS 13 WindowServer
 * (SkyLight's open_event_driver) asks for before it does anything else:
 *
 *   IOServiceMatching("IOHIDSystem") -> IOServiceGetMatchingServices -> IOServiceOpen
 *
 * The real class lives in Apple's IOHIDFamily, which this tree does not have. This stand-in
 * publishes the name, a user client that IOServiceOpen() succeeds on, the answer to SkyLight's
 * IOHIDCreateSharedMemory and a zeroed piece of shared memory: what took the lab's WindowServer to
 * "Successfully opened the IOHIDSession" and a software cursor. No events are delivered through
 * it yet (no IOHIDEventQueue header in that memory): touch and keys for macOS are another piece.
 *
 * It lives here rather than in a file of its own because adding one to iokit/conf/files makes
 * the build regenerate the IOKit symbol set, and check_all_exports then fails on a mismatch
 * that predates this change (the lab, measured 2026-11-02). From the lab's macOS 13.4 kernel
 * (work/macos13-vayu, a963bfba); started with boot-arg macos_abi=1 only (bsd/kern/kern_exec.c),
 * iOS has its own IOHIDEventSystem and never asks for this.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

/* matching is by class name, so the class has to be called IOHIDSystem itself */
class MiniIOHIDUserClient;

class IOHIDSystem : public IOService {
	OSDeclareDefaultStructors(IOHIDSystem)

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties, IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

/*
 * The smallest client that makes IOServiceOpen() succeed; every method call through it is
 * refused by IOUserClient's default externalMethod(), which the server will report. The event
 * queue (IOConnectMapMemory of kIOHIDEventQueue) is not implemented yet.
 */
class MiniIOHIDUserClient : public IOUserClient {
	OSDeclareDefaultStructors(MiniIOHIDUserClient)

public:
	virtual bool initWithTask(task_t owningTask, void * securityToken, UInt32 type,
	    OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOService * getService(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options,
	    IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOHIDSystem, IOService)

/*
 * Boot-arg macos_hid_params=1 (default 0: none, as before): the service where macOS has it, under IOResources,
 * its "HIDParameters", and its user clients' IOConnectGetService. 22F66's SkyLight CGSMouseStateCreate does NXClickTime(NXOpenEventStatus()) on
 * WindowServer's main thread; NXOpenEventStatus is IORegistryEntryFromPath("IOService:/IOResources/IOHIDSystem")
 * and IOServiceOpen(type 1) -- under the platform expert it found nothing and gave 0. IOHIDCopyCFTypeParameter
 * (IOKit of 22F66, read in its cache) then asks IOConnectGetService(handle), looks the key up in the service's
 * "HIDParameters", then in its own properties, and only when that fails asks the HID event system,
 * com.apple.iohideventsystem -- which 22F66's WindowServer.plist gives to WindowServer itself: it waited on
 * itself for ever (QEMU, g3-wslab-metal and SESS's g2-wslab-*: NXClickTime ->
 * IOHIDCopyHIDParameterFromEventSystem -> io_hideventsystem_open -> mach_msg).
 *   macos_hid_params=1   HIDClickTime (500 ms in ns, a 64-bit number) and HIDClickSpace (two zeros): what
 *                        NXClickTime/NXClickSpace read
 *   macos_hid_params=2   and the rest of a real Mac's IOHIDSystem defaults (macOS 27.2 ioreg)
 * From the Scion kernel's SweetHIDSystem.cpp (iokit/Kernel/arm/hid, Scion-Project xnu c2f63b27, APSL 2.0 as
 * the rest of XNU), which found and fixed the same self-wait on macOS 26.5; its key list and types as there.
 */
static void
mini_hid_parameters(IOService * service, uint32_t level)
{
	static const struct { const char *key; uint64_t value; } more[] = {
		{ "HIDKeyRepeat", 83333333ULL }, { "HIDInitialKeyRepeat", 500000000ULL },
		{ "HIDPointerAcceleration", 45056 }, { "HIDScrollAcceleration", 20480 }, { "HIDTrackpadAcceleration", 45056 },
		{ "HIDMouseAcceleration", 45056 }, { "HIDPointerButtonMode", 2 }, { "HIDF12EjectDelay", 250 },
		{ "HIDSlowKeysDelay", 0 }, { "HIDStickyKeysDisabled", 0 }, { "HIDStickyKeysOn", 0 },
		{ "HIDStickyKeysShiftToggles", 0 }, { "HIDMouseKeysOptionToggles", 0 }, { "HIDFKeyMode", 0 },
		{ "HIDScrollZoomModifierMask", 0 }, { "HIDUseLinearScalingMouseAcceleration", 0 },
		{ "MouseButtonDivision", 55 }, { "Clicking", 0 }, { "Dragging", 0 }, { "DragLock", 0 },
		{ "TrackpadRightClick", 1 }, { "TrackpadCornerSecondaryClick", 0 }, { "TrackpadHorizScroll", 1 },
		{ "TrackpadPinch", 1 }, { "TrackpadRotate", 1 }, { "TrackpadTwoFingerDoubleTapGesture", 1 },
		{ "TrackpadThreeFingerTapGesture", 0 }, { "TrackpadThreeFingerHorizSwipeGesture", 2 },
		{ "TrackpadThreeFingerVertSwipeGesture", 2 }, { "TrackpadFourFingerHorizSwipeGesture", 2 },
		{ "TrackpadFourFingerVertSwipeGesture", 2 }, { "TrackpadFourFingerPinchGesture", 2 },
		{ "TrackpadFiveFingerPinchGesture", 2 }, { "TrackpadTwoFingerFromRightEdgeSwipeGesture", 3 },
		{ "MouseTwoFingerDoubleTapGesture", 3 }, { "MouseTwoFingerHorizSwipeGesture", 2 },
		{ "MouseOneFingerDoubleTapGesture", 0 }, { "USBMouseStopsTrackpad", 0 }, { "FirstClickThreshold", 0 },
		{ "SecondClickThreshold", 0 }, { "EjectDelay", 0 }, { "version", 1 },
	};
	static const char *yes[] = { "TrackpadScroll", "TrackpadMomentumScroll", "TrackpadHandResting", "MouseMomentumScroll",
		                     "MouseVerticalScroll", "MouseHorizontalScroll", "UserPreferences", "HIDDefaultParameters" };
	OSDictionary *params = OSDictionary::withCapacity(64);
	OSNumber *n, *zero;
	OSArray *space;

	if (params == NULL) {
		return;
	}
	n = OSNumber::withNumber(500000000ULL, 64);
	if (n != NULL) {
		params->setObject("HIDClickTime", n);
		n->release();
	}
	zero = OSNumber::withNumber(0ULL, 32);
	space = OSArray::withCapacity(2);
	if (zero != NULL && space != NULL) {
		space->setObject(zero);
		space->setObject(zero);
		params->setObject("HIDClickSpace", space);
	}
	OSSafeReleaseNULL(space);
	OSSafeReleaseNULL(zero);
	if (level >= 2) {
		for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) {
			n = OSNumber::withNumber(more[i].value, 64);
			if (n != NULL) {
				params->setObject(more[i].key, n);
				n->release();
			}
		}
		for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
			params->setObject(yes[i], kOSBooleanTrue);
		}
		params->setObject("TrackpadThreeFingerDrag", kOSBooleanFalse);
		params->setObject("ForceSuppressed", kOSBooleanFalse);
		OSString *mode = OSString::withCString("OneButton");
		if (mode != NULL) {
			params->setObject("MouseButtonMode", mode);
			mode->release();
		}
		OSArray *pairs = OSArray::withCapacity(0);
		if (pairs != NULL) {
			params->setObject("HIDKeyboardModifierMappingPairs", pairs);
			pairs->release();
		}
	}
	service->setProperty("HIDParameters", params);
	IOLog("MiniIOHIDSystem: HIDParameters, %u keys (macos_hid_params=%u)\n", params->getCount(), level);
	params->release();
}

static uint32_t gMiniHIDParams;        /* boot-arg macos_hid_params */

bool
IOHIDSystem::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		IOLog("MiniIOHIDSystem: IOService::start failed\n");
		return false;
	}
	if (gMiniHIDParams != 0) {
		mini_hid_parameters(this, gMiniHIDParams);      /* before registerService: a client finds them at once */
	}
	registerService();
	IOLog("MiniIOHIDSystem: registered as IOHIDSystem (macos_abi=1)%s\n",
	    provider == IOService::getResourceService() ? ", IOService:/IOResources/IOHIDSystem" : "");
	return true;
}

OSDefineMetaClassAndStructors(MiniIOHIDUserClient, IOUserClient)

bool
MiniIOHIDUserClient::initWithTask(task_t owningTask, void * securityToken, UInt32 type,
    OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties)) {
		return false;
	}
	IOLog("MiniIOHIDSystem: user client opened (type %u)\n", (unsigned)type);
	return true;
}

/*
 * The event queue the server maps with IOConnectMapMemory64(kIOHIDGlobalMemory = 0): a piece of
 * wired memory, zeroed. NOTE: no IOHIDEventQueue header yet -- the server will most likely
 * reject it right after mapping, and that is the next thing to fill in.
 */
IOReturn
MiniIOHIDUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options,
    IOMemoryDescriptor ** memory)
{
	IOBufferMemoryDescriptor * buf;

	IOLog("MiniIOHIDSystem: clientMemoryForType(%u)\n", (unsigned)type);
	buf = IOBufferMemoryDescriptor::withCapacity(0x100000, kIOMemoryKernelUserShared);
	if (buf == NULL) {
		return kIOReturnNoMemory;
	}
	*options = 0;
	*memory = buf;
	return kIOReturnSuccess;
}

IOReturn
MiniIOHIDUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	IOLog("MiniIOHIDSystem: externalMethod %u (scalarIn %u structIn %u scalarOut %u)\n", selector,
	    (unsigned)args->scalarInputCount, (unsigned)args->structureInputSize,
	    (unsigned)args->scalarOutputCount);
	/*
	 * Selector 0 with one scalar input is what SkyLight calls first: IOHIDCreateSharedMemory,
	 * the size of the event queue. Answer it (the same number back) and let the mapping
	 * (clientMemoryForType) speak next; everything else is still refused.
	 */
	if (selector == 0 && args->scalarInputCount >= 1) {
		IOLog("MiniIOHIDSystem: createSharedMemory size %llu\n",
		    (unsigned long long)args->scalarInput[0]);
		if (args->scalarOutputCount >= 1) {
			args->scalarOutput[0] = args->scalarInput[0];
		}
		return kIOReturnSuccess;
	}
	return kIOReturnUnsupported;
}

/*
 * IOConnectGetService(connect): IOUserClient's own answer is none, and IOHIDCopyCFTypeParameter then asks the
 * event system instead of reading "HIDParameters" (above). IOHIDFamily's clients answer with their IOHIDSystem;
 * this one does with macos_hid_params=1, and with nothing as before without it.
 */
IOService *
MiniIOHIDUserClient::getService(void)
{
	return gMiniHIDParams != 0 ? getProvider() : NULL;
}

IOReturn
MiniIOHIDUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOHIDSystem::newUserClient(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties, IOUserClient ** handler)
{
	MiniIOHIDUserClient * client = new MiniIOHIDUserClient;

	if (client == NULL) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties)) {
		client->release();
		return kIOReturnBadArgument;
	}
	if (!client->attach(this)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

void
MiniIOHIDSystemStart(IOService * provider)
{
	IOHIDSystem * hid;

	if (PE_parse_boot_argn("macos_hid_params", &gMiniHIDParams, sizeof(gMiniHIDParams)) && gMiniHIDParams != 0 &&
	    IOService::getResourceService() != NULL) {
		provider = IOService::getResourceService();     /* where NXOpenEventStatus looks (above) */
	}
	hid = new IOHIDSystem;

	if (hid == NULL) {
		return;
	}
	if (!hid->init()) {
		hid->release();
		return;
	}
	if (!hid->attach(provider)) {
		hid->release();
		return;
	}
	if (!hid->start(provider)) {
		hid->detach(provider);
		hid->release();
	}
}

/*
 * MiniEndpointSecurity (boot-arg macos_es=1 with macos_abi=1; default 0: no such service): the
 * registry entry of class EndpointSecurityDriver that macOS 13.4's launchd boot task
 * "EndpointSecurity" (endpointsecurityd --init) waits 30 s for. Without it endpointsecurityd exits
 * -14, the boot task fails and launchd panics the kernel (U1's QEMU gate 3). What --init does
 * with it (endpointsecurityd of 22F66, arm64e, 0x100003340..0x100003a64): IOServiceOpen type 0
 * ("Failed to open service" -> exit -2), reads /Library/SystemExtensions/EndpointSecurity/
 * .early_boot.plist (absent on a Mac without ES system extensions: "Loaded 0 early boot
 * clients"), hands the clients to the kernel with IOConnectCallStructMethod selector 1 and exits 0
 * when that answers kIOReturnSuccess. Apple's kext would then hold those clients until they
 * connect; with none registered there is nothing to hold, so selector 1 is taken and said in the
 * log, and every other selector (the daemon's own, not --init's) is refused and said, once each.
 * No EndpointSecurity event is ever produced: no ES client on this port gets any.
 */
#include <sys/proc.h>
#include <os/atomic_private.h>

class EndpointSecurityDriver : public IOService {
	OSDeclareDefaultStructors(EndpointSecurityDriver)

public:
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties, IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

class MiniEndpointSecurityUserClient : public IOUserClient {
	OSDeclareDefaultStructors(MiniEndpointSecurityUserClient)

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(EndpointSecurityDriver, IOService)
OSDefineMetaClassAndStructors(MiniEndpointSecurityUserClient, IOUserClient)

IOReturn
MiniEndpointSecurityUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	static uint32_t told;
	char name[32];

	proc_selfname(name, sizeof(name));
	if (selector == 1 && args->structureInputSize >= 4 && args->scalarOutputCount == 0 && args->structureOutputSize == 0) {
		IOLog("MiniEndpointSecurity: %s[%d] registers its early boot clients (%u bytes): taken, none will be held\n", name,
		    proc_selfpid(), (unsigned) args->structureInputSize);
		return kIOReturnSuccess;
	}
	if (selector >= 32 || !(os_atomic_or_orig(&told, 1u << selector, relaxed) & (1u << selector))) {
		IOLog("MiniEndpointSecurity: %s[%d] selector %u (scalars %u in, struct %u in): not served\n", name, proc_selfpid(),
		    selector, (unsigned) args->scalarInputCount, (unsigned) args->structureInputSize);
	}
	return kIOReturnUnsupported;
}

IOReturn
MiniEndpointSecurityUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
EndpointSecurityDriver::newUserClient(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties, IOUserClient ** handler)
{
	MiniEndpointSecurityUserClient * client;

	if (type != 0) {
		IOLog("MiniEndpointSecurity: open of type %u refused (0 only)\n", (unsigned) type);
		return kIOReturnUnsupported;
	}
	client = new MiniEndpointSecurityUserClient;
	if (client == NULL) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(this)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

void
MiniEndpointSecurityStart(IOService * provider)
{
	EndpointSecurityDriver * es;
	uint32_t on = 0;

	if (!PE_parse_boot_argn("macos_es", &on, sizeof(on)) || on == 0) {
		return;
	}
	es = new EndpointSecurityDriver;
	if (es == NULL || !es->init() || !es->attach(provider)) {
		OSSafeReleaseNULL(es);
		return;
	}
	if (!es->start(provider)) {
		es->detach(provider);
		es->release();
		return;
	}
	es->registerService();
	IOLog("MiniEndpointSecurity: macos_es=1: EndpointSecurityDriver published (no events: early boot registration only)\n");
	es->release();
}
