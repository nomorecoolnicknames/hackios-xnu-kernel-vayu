/*
 * VayuBaseband: the two IOKit services iOS's CommCenter looks for on the cellular
 * path, as far as it takes to keep the process alive -- no PCIe, no firmware, no
 * DMA, no QMI.  Behind boot-arg vayu_bb=1 (services are there; every method says
 * kIOReturnNotReady), vayu_bb=2 (the same, but methods answer success with
 * zeros, for an A/B against a caller that treats a failure as fatal), or
 * vayu_bb=3 (plausible answers read out of the 16.5 client: no reset, manager
 * started/stopped echoed back, valid self-identify, baseband booted; see the
 * table above VayuBBUserClient::externalMethod and REPORT.md section 1).
 *
 * What CommCenter asks for, and where that was read from (docs/research/
 * VAYU_APPLE_BASEBAND_ABI.md has the addresses and the full tables):
 *
 *   AppleIPAppender  -- ApplePDPHelper.framework, ApplePDPHelperInterface::init():
 *                       IOServiceMatching("AppleIPAppender"), then
 *                       IOServiceOpen(service, mach_task_self(), 0, &connect),
 *                       then an IOGeneralInterest notification.  The user client
 *                       methods are IOConnectCallMethod selectors 0..0x13 (PDP
 *                       contexts, traffic-class flows, dedicated bearers).
 *
 *   AppleBaseband    -- libTelephonyBasebandDynamic.dylib registers
 *                       IOServiceAddMatchingNotification("AppleBaseband",
 *                       "IOServiceMatched", ...), then opens type 0.  Its methods
 *                       are scalar selectors 0..42 (reset, power, mux, baseband
 *                       state/property, coredump, timesync, SPMI).
 *
 * The kext these belong to is com.apple.driver.AppleBasebandPCI; the service
 * CommCenter matches is the AppleBaseband nub under it, not the PCI driver.
 * Both are here, as is AppleBasebandPCIControl, the name libPCITransport probes
 * before it decides which transport variant to use.
 *
 * Everything is a stub: the methods log and return an error (or zeros), and
 * nothing here talks to the modem.  The first milestone is only that CommCenter
 * stays alive; data needs the QMI client in the vayu-cell work.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/thread.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/lock.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

/* 0: off; 1: services present, methods refuse; 2: services present, methods answer zeros;
 * 3: plausible answers (see the table in externalMethod). */
static uint32_t gVayuBBMode;

/*
 * vayu_bb_extra=1: also publish AppleConvergedIPCBBControl, the second name
 * libPCITransport::probeVariant() asks about (docs/research/VAYU_APPLE_BASEBAND_ABI.md
 * section 3).  Off by default: a service that is there and cannot answer makes the
 * probe choose a variant that is not implemented, where an absent service lets it
 * fall to the other one.  One variable per run.
 */
static uint32_t gVayuBBExtra;

/*
 * Mode 3 state.  The AppleBaseband user client is a set of setters and getters over one
 * driver-side state word, and the 16.5 caller reads several of them back:
 *
 *   sel 22 (manager started/stopped) stores 1 for Started and 0 for Stopped, and
 *   sel 23 (get manager state) must return that same word -- libTelephonyBasebandDynamic
 *   only writes its out-parameter when the call succeeds (0x1e9a4a1b8..0x1e9a4a1c8),
 *   so an error there is "unknown manager state", not "stopped".
 *
 *   sel 26 (get baseband state) returns one scalar that ABM maps to its own
 *   abm::BasebandBootState*.  The numeric enum lives in the Apple kext and was not
 *   recovered.  The strings in libBasebandManager name the states in this order:
 *   Reset, Not Booted, Resetting, Booting, Booted, IsReady, Boot Error, Dead,
 *   Shutting Down, Shutdown (REPORT.md section 2, HYPOTHESIS).  The default is 4
 *   ("Booted" in that order) and vayu_bb_bbstate=N overrides it, so a run can sweep
 *   the value without a new kernel.
 *
 *   sel 27 (set baseband state) is expected to be the driver telling itself the state;
 *   it is remembered and reported back by sel 26.
 */
static uint32_t gVayuBBBasebandState = 4;       /* mode 3: value sel 26 reports */
static uint32_t gVayuBBManagerState;            /* mode 3: last value set by sel 22 */
static uint32_t gVayuBBSelfIdentify;            /* mode 3: number of 64-byte self-identifies seen */
static char     gVayuBBSelfIdentifyText[64];    /* mode 3: the last one, printable only */

/* Per-selector call counters, printed as a one-line summary every VAYU_BB_SUMMARY_EVERY
 * calls.  The point is the tail of a run: which selectors were new, and how often the
 * reset/recovery loop came back.  Two tables because the PDP interface has its own set. */
#define VAYU_BB_SEL_MAX 64
#define VAYU_BB_PDP_MAX 32
#define VAYU_BB_SUMMARY_EVERY 24
static uint32_t gVayuBBSelCount[VAYU_BB_SEL_MAX];
static uint32_t gVayuBBPdpCount[VAYU_BB_PDP_MAX];
static uint32_t gVayuBBSelCalls;
static uint32_t gVayuBBPdpCalls;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The msgbuf is a ring: with msgbuf=1048576 the boot half of a twenty-minute run is
 * overwritten before anyone reads it, and the first lines lost are exactly the ones this
 * stub exists to produce ("stubs up", which user client was opened, the first selector).
 * The run on 2026-11-04 15:23 lost all of them that way; only a single selector line
 * survived at kernel t=160.
 *
 * So every line this file prints is also appended here, and the tail is readable as
 * kern.vayu_bb_trace at any time:
 *
 *     sysctl kern.vayu_bb_trace
 *
 * Text only, and only what the stub itself says: class names, selector numbers, counts and
 * the scalar arguments the caller passed. No payload of any kind, and this file never sees
 * a QMI message in the first place.
 */
#define VAYU_BB_TRACE_SIZE 32768

static char vayu_bb_trace_buf[VAYU_BB_TRACE_SIZE];
static LCK_GRP_DECLARE(vayu_bb_trace_lck_grp, "vayu-bb-trace");
static LCK_MTX_DECLARE(vayu_bb_trace_lock, &vayu_bb_trace_lck_grp);

SYSCTL_STRING(_kern, OID_AUTO, vayu_bb_trace, CTLFLAG_RD, vayu_bb_trace_buf,
    sizeof(vayu_bb_trace_buf), "VayuBaseband stub trace (IOLog lines, tail kept)");

static void
vayu_bb_say(const char *format, ...) __printflike(1, 2);

/* One formatted line: IOLog (as before) plus the ring.  The lock is held only for the
 * copy; IOLog is called outside it so a slow console cannot stall another user client. */
static void
vayu_bb_say(const char *format, ...)
{
	char line[256];
	va_list list;
	size_t used, length;
	int written;

	va_start(list, format);
	written = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (written < 0) {
		return;
	}
	IOLog("%s", line);

	length = strlen(line);
	if (length == 0) {
		return;
	}
	if (length >= VAYU_BB_TRACE_SIZE) {
		length = VAYU_BB_TRACE_SIZE - 1;
	}

	lck_mtx_lock(&vayu_bb_trace_lock);
	used = strlen(vayu_bb_trace_buf);
	if (used + length + 1 > VAYU_BB_TRACE_SIZE) {
		/* Keep the tail: drop the older half before appending. */
		size_t keep = VAYU_BB_TRACE_SIZE / 2;
		if (used > keep) {
			memmove(vayu_bb_trace_buf, vayu_bb_trace_buf + (used - keep), keep);
			used = keep;
			vayu_bb_trace_buf[used] = 0;
		}
		if (used + length + 1 > VAYU_BB_TRACE_SIZE) {
			length = VAYU_BB_TRACE_SIZE - 1 - used;
		}
	}
	memcpy(vayu_bb_trace_buf + used, line, length);
	vayu_bb_trace_buf[used + length] = 0;
	lck_mtx_unlock(&vayu_bb_trace_lock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * The selector names are the strings the callers pass to IOConnectCallMethod as
 * their second argument ("get baseband state", "Enable QoS header prepend", ...).
 * They are not part of the ABI -- the selector is -- but a log line that says
 * "selector 26" without a name is not much use when the next run is being read.
 */
static const char *
vayu_bb_selector_name(uint32_t selector)
{
	switch (selector) {
	case 0:  return "reset modem";
	case 1:  return "power on/off";
	case 2:  return "mux on/off";
	case 3:  return "get reset";
	case 6:  return "powercycle modem";
	case 8:  return "usb on/off";
	case 10: return "assert/deassert device wake";
	case 11: return "get pmuext on";
	case 15: return "self-identify";
	case 16: return "get diag ready";
	case 17: return "get efs-erase done";
	case 18: return "get reset request";
	case 19: return "coredump on/off";
	case 20: return "timesync";
	case 22: return "manager started/stopped";
	case 23: return "get manager state";
	case 25: return "charger desense on/off";
	case 26: return "get baseband state";
	case 27: return "set baseband state";
	case 28: return "get baseband property";
	case 29: return "set baseband property";
	case 32: return "clear frequency list";
	case 35: return "reset SPMI";
	case 42: return "get coredump ready status";
	default: return NULL;
	}
}

static const char *
vayu_pdp_selector_name(uint32_t selector)
{
	switch (selector) {
	case 0:  return "add flow (traffic class -> flow id)";
	case 1:  return "remove flow";
	case 2:  return "get flow for traffic class";
	case 3:  return "enable QoS header prepend";
	case 4:  return "set dormancy";
	case 5:  return "get dormancy time";
	case 6:  return "get sleep time adjustment";
	case 7:  return "get status";
	case 8:  return "set link quality metric";
	case 9:  return "restart dormancy check";
	case 10: return "copy wake buffer";
	case 11: return "report link throughput";
	case 12: return "get open ports (extension)";
	case 13: return "enable low-power PDP throttle";
	case 14: return "activate low-power PDP throttle";
	case 15: return "get low-power PDP throttle stats";
	case 16: return "block PDP traffic";
	case 17: return "enable metadata trailer";
	case 18: return "get keepalive offload frames";
	case 19: return "notify dedicated bearer";
	default: return NULL;
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class VayuBBService;

class VayuBBUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(VayuBBUserClient);

protected:
	uint32_t fKind;                 /* 0: AppleBaseband, 1: AppleIPAppender, 2: PCI control */
	UInt32   fType;
	task_t   fTask;

public:
	void setKind(uint32_t kind) { fKind = kind; }

	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(VayuBBUserClient, IOUserClient);

bool
VayuBBUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	fType = type;
	return true;
}

bool
VayuBBUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	vayu_bb_say("VayuBaseband: %s opened (type 0x%x, mode %u: %s)\n",
	    getMetaClass()->getClassName(), (unsigned) fType, gVayuBBMode,
	    gVayuBBMode == 3 ? "answers plausible success (vayu_bb=3)" :
	    (gVayuBBMode == 2 ? "answers success with zeros" : "answers kIOReturnNotReady"));
	return true;
}

IOReturn
VayuBBUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
VayuBBUserClient::free(void)
{
	IOUserClient::free();
}

/*
 * Helpers for the mode-3 answers.  The counts in IOExternalMethodArguments are capacities on
 * the way in and must be rewritten to the number of items actually produced on the way out.
 */
static void
vayu_bb_scalar_out(IOExternalMethodArguments * args, uint64_t value)
{
	if (args->scalarOutput != NULL && args->scalarOutputCount > 0) {
		args->scalarOutput[0] = value;
		args->scalarOutputCount = 1;
	} else {
		args->scalarOutputCount = 0;
	}
}

static void
vayu_bb_struct_out_zero(IOExternalMethodArguments * args, uint32_t bytes)
{
	if (args->structureOutput != NULL && args->structureOutputSize >= bytes) {
		bzero(args->structureOutput, bytes);
		args->structureOutputSize = bytes;
	} else {
		args->structureOutputSize = 0;
	}
}

/* Which property ids ABM asks for is not reversed; every one answers 0 ("not set"), which is
 * what a driver with no NVRAM-backed baseband properties would say.  Logged so the next run
 * shows the ids. */
static uint64_t
vayu_bb_baseband_property(uint64_t property_id)
{
	(void) property_id;
	return 0;
}

/* Called for every method while mode 3 is on.  Keeps the per-selector counters and prints a
 * one-line summary every VAYU_BB_SUMMARY_EVERY calls: the tail of a long run is easier to read
 * than a thousand identical selector lines. */
static void
vayu_bb_note_call(uint32_t kind, uint32_t selector)
{
	uint32_t * table;
	uint32_t * calls;
	uint32_t limit;
	uint32_t total;

	if (kind == 1) {
		table = gVayuBBPdpCount;
		calls = &gVayuBBPdpCalls;
		limit = VAYU_BB_PDP_MAX;
	} else {
		table = gVayuBBSelCount;
		calls = &gVayuBBSelCalls;
		limit = VAYU_BB_SEL_MAX;
	}
	if (selector < limit) {
		table[selector]++;
	}
	total = ++(*calls);
	if (total % VAYU_BB_SUMMARY_EVERY != 0) {
		return;
	}

	char line[256];
	size_t used = 0;
	int written;

	written = snprintf(line, sizeof(line), "VayuBaseband: %s summary after %u:",
	    kind == 1 ? "PDP" : "BB", (unsigned) total);
	if (written > 0) {
		used = (size_t) written;
	}
	for (uint32_t i = 0; i < limit && used + 12 < sizeof(line); i++) {
		if (table[i] == 0) {
			continue;
		}
		written = snprintf(line + used, sizeof(line) - used, " %u=%u", (unsigned) i,
		    (unsigned) table[i]);
		if (written <= 0) {
			break;
		}
		used += (size_t) written;
	}
	vayu_bb_say("%s\n", line);
}

IOReturn
VayuBBUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const char * name;
	const char * who;
	uint32_t mode = gVayuBBMode;

	if (fKind == 1) {
		name = vayu_pdp_selector_name(selector);
		who = "ApplePDPHelperInterface";
	} else if (fKind == 2) {
		/* The PCI control interface's selectors were not reversed. */
		name = NULL;
		who = "AppleBasebandPCIControlClient";
	} else {
		name = vayu_bb_selector_name(selector);
		who = "AppleBasebandUserClient";
	}

	/*
	 * The scalar arguments are part of the question: "get baseband property" carries a
	 * property id in scalarInput[0], and a bare count does not say which one was asked.
	 * PDP selectors 8 and 9 are mapped now (set link quality metric, restart dormancy
	 * check) -- they were read out of ApplePDPHelper: 0x1f230eea0 and 0x1f230ee80 in
	 * REPORT.md section 1.3; the 16.5 caller in the 1054b trace used exactly those two.
	 */
	const char * label = name != NULL ? name : "unknown";

	vayu_bb_say("VayuBaseband: %s selector %u (%s): scalars in %u, struct in %u, "
	    "scalars out %u, struct out %u%s (in %llu %llu %llu %llu)\n",
	    who, (unsigned) selector, label,
	    (unsigned) args->scalarInputCount, (unsigned) args->structureInputSize,
	    (unsigned) args->scalarOutputCount, (unsigned) args->structureOutputSize,
	    args->structureOutputDescriptor != NULL ? " (out-of-line output)" : "",
	    (unsigned long long) (args->scalarInput != NULL && args->scalarInputCount > 0 ?
	    args->scalarInput[0] : 0),
	    (unsigned long long) (args->scalarInput != NULL && args->scalarInputCount > 1 ?
	    args->scalarInput[1] : 0),
	    (unsigned long long) (args->scalarInput != NULL && args->scalarInputCount > 2 ?
	    args->scalarInput[2] : 0),
	    (unsigned long long) (args->scalarInput != NULL && args->scalarInputCount > 3 ?
	    args->scalarInput[3] : 0));

	if (name == NULL && fKind != 2) {
		/* Not one of the selectors the 16.5 caller was seen to use. */
		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		return kIOReturnUnsupported;
	}
	if (mode == 3) {
		vayu_bb_note_call(fKind, selector);
	}
	if (mode != 2 && mode != 3) {
		/* The stub knows the request (or, for the PCI control, does not) and has no hardware. */
		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		return name != NULL ? kIOReturnNotReady : kIOReturnUnsupported;
	}

	/*
	 * Modes 2 and 3: a well-formed answer.  Out-of-line output is not touched -- no caller
	 * seen in 16.5 asks for one of these structures out of line, and writing a user's
	 * memory descriptor from a stub is not worth the code until one does.
	 */
	if (args->structureOutputDescriptor != NULL) {
		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		return kIOReturnUnsupported;
	}

	uint64_t in0 = (args->scalarInput != NULL && args->scalarInputCount > 0) ?
	    args->scalarInput[0] : 0;
	uint64_t in1 = (args->scalarInput != NULL && args->scalarInputCount > 1) ?
	    args->scalarInput[1] : 0;

	if (mode == 2) {
		if (args->scalarOutput != NULL && args->scalarOutputCount != 0) {
			bzero(args->scalarOutput,
			    (size_t) args->scalarOutputCount * sizeof(uint64_t));
		}
		if (args->structureOutput != NULL && args->structureOutputSize != 0) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;
	}

	/*
	 * Mode 3: the plausible answers.  Everything here is read out of the 16.5 callers; the
	 * addresses are in REPORT.md section 1 and in VAYU_APPLE_BASEBAND_ABI.md.
	 *
	 *  - sel 3 (get reset): libTelephonyBasebandDynamic 0x1e9a49250..0x1e9a49264 makes
	 *    "a reset happened" = (!call_ok) || (value != 0).  Success + 0 is the only answer
	 *    that says "no reset"; kIOReturnNotReady itself was being read as a reset in the
	 *    1054b trace and is what produced the powercycle/recovery loop.
	 *  - sel 15 (self-identify): 64-byte struct input, a C string "<process>:
	 *    TelephonyBaseband" (0x1e9a482bc..0x1e9a48308).  A failure is only logged by the
	 *    client, so this is about the trace, not about staying alive.  The string is
	 *    copied here and printed (printable bytes only) -- it names the process that
	 *    opened the controller.
	 *  - sel 22/23: 1 = Started, 0 = Stopped; sel 23 returns the last value set.
	 *  - sel 26/27: the baseband boot state scalar; 27 remembers, 26 reports.
	 *  - sel 16/17: "diag ready" and "efs-erase done" answer 1: a booted baseband would
	 *    have both.  HYPOTHESIS for 16 -- ABM may use it only as a hint.
	 *  - sel 28: property id in scalarInput[0], value 0 for every id (see above).
	 *  - sel 20 (timesync): echoes the caller's value; the shape is scalar in/out.
	 *  - PDP sel 6 (get sleep time adjustment): 8-byte mach_timespec, zero is a valid
	 *    "no adjustment"; sel 17 (enable metadata trailer) answers 1 = enabled.
	 *  - PCI control: selectors 0..12 are the libIOABP control/data calls (open, close,
	 *    write, read, sendImage, readRegister, abortChannel, startChannel, timeSync,
	 *    mapMemory, unmapMemory, registerTimeEvent, deregisterTimeEvent; libIOABP
	 *    0x1e92e07c8..0x1e92e2c44).  Success with zeros lets the caller show its next
	 *    selector instead of failing on the first one; nothing is mapped and nothing is
	 *    read from the modem.
	 */
	if (fKind == 2) {
		if (selector > 12) {
			args->scalarOutputCount = 0;
			args->structureOutputSize = 0;
			return kIOReturnUnsupported;
		}
		if (args->scalarOutput != NULL && args->scalarOutputCount != 0) {
			bzero(args->scalarOutput,
			    (size_t) args->scalarOutputCount * sizeof(uint64_t));
		}
		if (args->structureOutput != NULL && args->structureOutputSize != 0) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;
	}

	if (fKind == 1) {
		switch (selector) {
		case 3:         /* enable QoS header prepend: status = on */
			vayu_bb_scalar_out(args, 1);
			break;
		case 6:         /* get sleep time adjustment: mach_timespec {0, 0} */
			vayu_bb_struct_out_zero(args, 8);
			break;
		case 17:        /* enable metadata trailer: enabled */
			vayu_bb_scalar_out(args, 1);
			break;
		default:
			args->scalarOutputCount = 0;
			args->structureOutputSize = 0;
			break;
		}
		return kIOReturnSuccess;
	}

	switch (selector) {
	case 3:                 /* get reset: no reset */
		vayu_bb_scalar_out(args, 0);
		vayu_bb_say("VayuBaseband: get reset -> success, reset 0\n");
		break;
	case 6:                 /* powercycle modem: accepted, no hardware behind it */
		vayu_bb_say("VayuBaseband: powercycle modem accepted (mode 3); baseband state %u\n",
		    (unsigned) gVayuBBBasebandState);
		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		break;
	case 11:                /* get pmuext on: off */
		vayu_bb_scalar_out(args, 0);
		break;
	case 15: {              /* self-identify: 64-byte string */
		const char * text = (const char *) args->structureInput;
		size_t length = 0;

		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		if (text != NULL && args->structureInputSize > 0) {
			while (length < args->structureInputSize && length < 63 && text[length] != 0) {
				length++;
			}
			lck_mtx_lock(&vayu_bb_trace_lock);
			for (size_t i = 0; i < length; i++) {
				char c = text[i];
				gVayuBBSelfIdentifyText[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
			}
			gVayuBBSelfIdentifyText[length] = 0;
			gVayuBBSelfIdentify++;
			lck_mtx_unlock(&vayu_bb_trace_lock);
			vayu_bb_say("VayuBaseband: self-identify #%u: \"%s\" (%u bytes of %u)\n",
			    (unsigned) gVayuBBSelfIdentify, gVayuBBSelfIdentifyText,
			    (unsigned) length, (unsigned) args->structureInputSize);
		}
		break;
	}
	case 16:                /* get diag ready: ready */
		vayu_bb_scalar_out(args, 1);
		break;
	case 17:                /* get efs-erase done: done */
		vayu_bb_scalar_out(args, 1);
		break;
	case 18:                /* get reset request: none */
		vayu_bb_scalar_out(args, 0);
		break;
	case 20:                /* timesync: echo the caller's value */
		vayu_bb_scalar_out(args, in0);
		break;
	case 22:                /* manager started (1) / stopped (0) */
		gVayuBBManagerState = in0 != 0 ? 1 : 0;
		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		vayu_bb_say("VayuBaseband: manager state set to %u\n",
		    (unsigned) gVayuBBManagerState);
		break;
	case 23:                /* get manager state */
		vayu_bb_scalar_out(args, gVayuBBManagerState);
		vayu_bb_say("VayuBaseband: get manager state -> %u\n",
		    (unsigned) gVayuBBManagerState);
		break;
	case 26:                /* get baseband state */
		vayu_bb_scalar_out(args, gVayuBBBasebandState);
		vayu_bb_say("VayuBaseband: get baseband state -> %u\n",
		    (unsigned) gVayuBBBasebandState);
		break;
	case 27:                /* set baseband state */
		gVayuBBBasebandState = (uint32_t) in0;
		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		vayu_bb_say("VayuBaseband: baseband state set to %u\n",
		    (unsigned) gVayuBBBasebandState);
		break;
	case 28:                /* get baseband property */
		vayu_bb_scalar_out(args, vayu_bb_baseband_property(in0));
		vayu_bb_say("VayuBaseband: get baseband property %llu -> 0\n",
		    (unsigned long long) in0);
		break;
	default:
		args->scalarOutputCount = 0;
		args->structureOutputSize = 0;
		break;
	}
	(void) in1;
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class AppleBasebandUserClient : public VayuBBUserClient
{
	OSDeclareDefaultStructors(AppleBasebandUserClient);
};

OSDefineMetaClassAndStructors(AppleBasebandUserClient, VayuBBUserClient);

class AppleBasebandPCIControlClient : public VayuBBUserClient
{
	OSDeclareDefaultStructors(AppleBasebandPCIControlClient);
};

OSDefineMetaClassAndStructors(AppleBasebandPCIControlClient, VayuBBUserClient);

class ApplePDPHelperInterface : public VayuBBUserClient
{
	OSDeclareDefaultStructors(ApplePDPHelperInterface);
};

OSDefineMetaClassAndStructors(ApplePDPHelperInterface, VayuBBUserClient);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class VayuBBService : public IOService
{
	OSDeclareDefaultStructors(VayuBBService);

protected:
	/* 0: baseband controller, 1: IP appender, 2: PCI control */
	virtual uint32_t vayuKind(void) const { return 0; }

public:
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties, IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(VayuBBService, IOService);

IOReturn
VayuBBService::newUserClient(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties, IOUserClient ** handler)
{
	VayuBBUserClient * client;
	uint32_t kind = vayuKind();

	switch (kind) {
	case 1:
		client = OSTypeAlloc(ApplePDPHelperInterface);
		break;
	case 2:
		client = OSTypeAlloc(AppleBasebandPCIControlClient);
		break;
	default:
		client = OSTypeAlloc(AppleBasebandUserClient);
		break;
	}
	if (client == NULL) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties)) {
		client->release();
		return kIOReturnBadArgument;
	}
	client->setKind(kind);
	if (!client->start(this)) {
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

class AppleBasebandPCI : public VayuBBService
{
	OSDeclareDefaultStructors(AppleBasebandPCI);
};

OSDefineMetaClassAndStructors(AppleBasebandPCI, VayuBBService);

class AppleBaseband : public VayuBBService
{
	OSDeclareDefaultStructors(AppleBaseband);
};

OSDefineMetaClassAndStructors(AppleBaseband, VayuBBService);

class AppleBasebandPCIControl : public VayuBBService
{
	OSDeclareDefaultStructors(AppleBasebandPCIControl);

protected:
	virtual uint32_t vayuKind(void) const APPLE_KEXT_OVERRIDE { return 2; }
};

OSDefineMetaClassAndStructors(AppleBasebandPCIControl, VayuBBService);

class AppleIPAppender : public VayuBBService
{
	OSDeclareDefaultStructors(AppleIPAppender);

protected:
	virtual uint32_t vayuKind(void) const APPLE_KEXT_OVERRIDE { return 1; }
};

OSDefineMetaClassAndStructors(AppleIPAppender, VayuBBService);

/*
 * libIOABP (the userland half of AppleBasebandPCI) matches two names, not one:
 *
 *   IOABPControlClass  -> IOServiceMatching("AppleBasebandPCIControl")     (0x1e92e0134)
 *   IOABPClass         -> IOServiceMatching("AppleBasebandPCIInterface")   (0x1e92e14c4)
 *
 * The first one is the control channel (open/close/logs/registers); the second is the data
 * channel (read/write/sendImage/mapMemory, selectors 0..12 of the same user client).  The
 * 16.5 kext publishes both; the stub published only the control name, so a transport that
 * got as far as IOABPControlClass::start would stop at "could not get matching service" for
 * the data one.  Only in mode 3: it is part of "let the caller show its next step", not of
 * keeping CommCenter alive.
 */
class AppleBasebandPCIInterface : public VayuBBService
{
	OSDeclareDefaultStructors(AppleBasebandPCIInterface);

protected:
	virtual uint32_t vayuKind(void) const APPLE_KEXT_OVERRIDE { return 2; }
};

OSDefineMetaClassAndStructors(AppleBasebandPCIInterface, VayuBBService);

/*
 * The other variant libPCITransport::probeVariant() asks about (the newer Converged IPC
 * control, whose Apple user client is IOAppleConvergedIPCBBControlUserClient).  Only with
 * vayu_bb_extra=1; the selectors were not reversed, so it answers like the PCI control.
 */
class AppleConvergedIPCBBControl : public VayuBBService
{
	OSDeclareDefaultStructors(AppleConvergedIPCBBControl);

protected:
	virtual uint32_t vayuKind(void) const APPLE_KEXT_OVERRIDE { return 2; }
};

OSDefineMetaClassAndStructors(AppleConvergedIPCBBControl, VayuBBService);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static AppleBasebandPCI *        gPCI;
static AppleBaseband *           gBaseband;
static AppleBasebandPCIControl * gControl;
static AppleConvergedIPCBBControl * gConvergedControl;
static AppleBasebandPCIInterface * gInterface;
static AppleIPAppender *         gAppender;

/* Put a service under `provider` and into the service plane.  It owns itself from
 * registerService() on; the globals above are the parent the next one attaches to. */
static IOService *
vayu_bb_publish(IOService * service, IOService * provider, const char * name,
    const char * bundle, const char * userClientClass)
{
	if (service == NULL || provider == NULL || !service->init()) {
		OSSafeReleaseNULL(service);
		return NULL;
	}
	service->setName(name);
	service->setProperty(kIOUserClientClassKey, userClientClass);
	service->setProperty("CFBundleIdentifier", bundle);
	service->setProperty("VayuBasebandStub", kOSBooleanTrue);
	service->setProperty("VayuBasebandMode", (unsigned long long) gVayuBBMode, 32);
	if (!service->attach(provider)) {
		vayu_bb_say("VayuBaseband: %s: attach failed\n", name);
		service->release();
		return NULL;
	}
	if (!service->start(provider)) {
		vayu_bb_say("VayuBaseband: %s: start failed\n", name);
		service->detach(provider);
		service->release();
		return NULL;
	}
	service->registerService();
	if (!service->inPlane(gIOServicePlane)) {
		vayu_bb_say("VayuBaseband: %s: registerService did not put it in the service plane\n", name);
	}
	return service;
}

static void
vayu_bb_register(IOService * platform)
{
	if (platform == NULL || gPCI != NULL) {
		return;
	}

	gPCI = (AppleBasebandPCI *) vayu_bb_publish(OSTypeAlloc(AppleBasebandPCI), platform,
	    "AppleBasebandPCI", "com.apple.driver.AppleBasebandPCI", "AppleBasebandPCIControlClient");
	if (gPCI == NULL) {
		vayu_bb_say("VayuBaseband: no AppleBasebandPCI; nothing else can be attached\n");
		return;
	}

	/* The nub CommCenter matches: IOServiceMatching("AppleBaseband"). */
	gBaseband = (AppleBaseband *) vayu_bb_publish(OSTypeAlloc(AppleBaseband), gPCI,
	    "AppleBaseband", "com.apple.driver.AppleBasebandPCI", "AppleBasebandUserClient");
	if (gBaseband == NULL) {
		vayu_bb_say("VayuBaseband: no AppleBaseband nub\n");
	}

	/* libPCITransport's isServicePresent() probes this one before it picks a transport. */
	gControl = (AppleBasebandPCIControl *) vayu_bb_publish(OSTypeAlloc(AppleBasebandPCIControl), gPCI,
	    "AppleBasebandPCIControl", "com.apple.driver.AppleBasebandPCI", "AppleBasebandPCIControlClient");
	if (gControl == NULL) {
		vayu_bb_say("VayuBaseband: no AppleBasebandPCIControl\n");
	}

	/* The second variant libPCITransport may probe; only with vayu_bb_extra=1. */
	if (gVayuBBExtra != 0) {
		gConvergedControl = (AppleConvergedIPCBBControl *) vayu_bb_publish(
		    OSTypeAlloc(AppleConvergedIPCBBControl), gPCI, "AppleConvergedIPCBBControl",
		    "com.apple.driver.AppleConvergedIPCBB", "AppleBasebandPCIControlClient");
		if (gConvergedControl == NULL) {
			vayu_bb_say("VayuBaseband: no AppleConvergedIPCBBControl\n");
		}
	}

	/* The data half of libIOABP; mode 3 only (see the class comment above). */
	if (gVayuBBMode == 3) {
		gInterface = (AppleBasebandPCIInterface *) vayu_bb_publish(
		    OSTypeAlloc(AppleBasebandPCIInterface), gPCI, "AppleBasebandPCIInterface",
		    "com.apple.driver.AppleBasebandPCI", "AppleBasebandPCIControlClient");
		if (gInterface == NULL) {
			vayu_bb_say("VayuBaseband: no AppleBasebandPCIInterface\n");
		} else {
			vayu_bb_say("VayuBaseband: AppleBasebandPCIInterface up (mode 3: libIOABP data class)\n");
		}
	}

	/* The PDP helper is a sibling of the PCI driver, not a child: CommCenter matches it by class. */
	gAppender = (AppleIPAppender *) vayu_bb_publish(OSTypeAlloc(AppleIPAppender), platform,
	    "AppleIPAppender", "com.apple.driver.AppleIPAppender", "ApplePDPHelperInterface");
	if (gAppender == NULL) {
		vayu_bb_say("VayuBaseband: no AppleIPAppender\n");
	}

	vayu_bb_say("VayuBaseband: stubs up (%s): AppleBasebandPCI/AppleBaseband/AppleBasebandPCIControl/"
	    "AppleIPAppender%s, mode %u\n",
	    gAppender != NULL && gBaseband != NULL ? "all four" : "incomplete",
	    gInterface != NULL ? "/AppleBasebandPCIInterface" : "", gVayuBBMode);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static void
vayu_bb_thread(void * arg, wait_result_t result)
{
	IOService * platform = NULL;

	(void) arg;
	(void) result;

	/* bsd_init() can run before the platform expert is attached; wait for it. */
	for (unsigned i = 0; i < 600; i++) {
		platform = IOService::getPlatform();
		if (platform != NULL && IOService::getRegistryRoot() != NULL) {
			break;
		}
		IOSleep(100);
	}
	if (platform == NULL) {
		vayu_bb_say("VayuBaseband: no platform expert after 60 s; not registering\n");
		return;
	}
	vayu_bb_register(platform);
}

extern "C" void
vayu_bb_init(void)
{
	uint32_t mode = 0;

	if (!PE_parse_boot_argn("vayu_bb", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	if (mode > 3) {
		vayu_bb_say("VayuBaseband: vayu_bb=%u is not 1 (refuse), 2 (zeros) or 3 (plausible); using 1\n", mode);
		mode = 1;
	}
	gVayuBBMode = mode;

	uint32_t extra = 0;
	if (PE_parse_boot_argn("vayu_bb_extra", &extra, sizeof(extra))) {
		gVayuBBExtra = extra != 0;
	}

	/* Mode 3 only: the scalar sel 26 reports.  4 is "Booted" in the abm::BasebandBootState
	 * string order found in libBasebandManager (REPORT.md section 2, HYPOTHESIS); a run can
	 * sweep it without a new kernel. */
	uint32_t bbstate = 0;
	if (PE_parse_boot_argn("vayu_bb_bbstate", &bbstate, sizeof(bbstate))) {
		gVayuBBBasebandState = bbstate;
	}

	vayu_bb_say("VayuBaseband: vayu_bb=%u%s: registering the CommCenter stubs%s\n", mode,
	    gVayuBBExtra ? " extra=1 (also AppleConvergedIPCBBControl)" : "",
	    mode == 3 ? "; mode 3 answers: reset 0, manager echo, self-identify ok, baseband state" : "");

	/* Usually the platform expert is already there by bsd_init(); if not, wait in a thread. */
	if (IOService::getPlatform() != NULL && IOService::getRegistryRoot() != NULL) {
		vayu_bb_register(IOService::getPlatform());
		return;
	}

	thread_t thread = NULL;
	if (kernel_thread_start(vayu_bb_thread, NULL, &thread) != KERN_SUCCESS) {
		vayu_bb_say("VayuBaseband: cannot start the wait thread\n");
		return;
	}
	thread_deallocate(thread);
}
