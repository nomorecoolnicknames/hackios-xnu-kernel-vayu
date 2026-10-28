/*
 * MiniAppleStockholm: AppleStockholmControl (GPIO) and AppleStockholmSPMI (SPMI socket), as iOS
 * 16.5's nfcd reaches them through libNFC_HAL / libPN548_API (iPhone10,3 20F66 dyld cache).
 *   GPIO: _NFHardwareGPIOOpen (0x1f51eac20) matches AppleStockholmControl by name, opens type 0;
 *   absent -> "Could not open service". Selectors (0x1f51e8e08): 0 SetPower, 1 SetDownloadRequest,
 *   2 SetPMUStandbyPower, 3 SetDeviceWake, 5 SetVBAT (one scalar in), 6 ValidateSPMIConfig,
 *   7 ConfigureHammerfestSPMI; 4 unused.
 *   SPMI: _NFHardwareSerialGetDefaultInterfaceType (0x1f51ed934) matches AppleStockholmControl on
 *   "nfc,primary,gpio" and AppleStockholmSPMI on "nfc,primary,spmi"; type 3 only if the SPMI node
 *   has "stockholm-spmi-data-socket" as a true boolean, else type 2 (UART), whose relative
 *   open("com.apple.uart.stockholm") fails -- SPMI is the only route. Type 3 -> "com.apple.spmi.nfc"
 *   (libPN548_API 0x1f14004b0). _NFHardwareSerialOpen (0x1f51ec0f4): socket(PF_SYSTEM, SOCK_STREAM,
 *   SYSPROTO_CONTROL), ioctl(CTLIOCGINFO) with ctl_name verbatim, connect() unit 0; getsockopt
 *   0x40487413 / setsockopt 0x80487414 (72 bytes, termios), setsockopt 0x80047301 (IO_STOCKHOLM_CRC),
 *   0x80047302 (IO_STOCKHOLM_ON_DEMAND_CLIENT), getsockopt 0x40407303 (64 bytes, SPMIERRORS); data
 *   are read()/write(). Framing is raw NCI (reassembled in ctlSend); with vayu_nfc_ios >= 2 the
 *   packets are bridged to the PN553 by bsd/dev/vayu_nfc.c.
 * On this phone (POCO X3 Pro "vayu") the controller is an NXP PN553 driven by bsd/dev/vayu_nfc.c;
 * this file touches no hardware. Selector 1 with a non-zero value is still refused
 * (kIOReturnNotPermitted): the firmware-download line is never raised.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/systm.h>
/*
 * bsd/dev/vayu_nfc.c's bridge API (declared in bsd/sys/vayu_nfc.h, which IOKit's build cannot include: that
 * header is not exported). Keep these three in step with it.
 */
extern "C" {
typedef void (*vayu_nfc_rx_fn)(const uint8_t *packet, unsigned length);
int vayu_nfc_bridge_start(vayu_nfc_rx_fn rx);
int vayu_nfc_bridge_send(const uint8_t *packet, unsigned length);
void vayu_nfc_bridge_stop(void);
}

#include "MiniDrivers.h"

#define kSpmiCtlName "com.apple.spmi.nfc"

extern "C" void loader_log_putc(char c);

/*
 * IOLog alone did not reach the run log (a test run: no MiniAppleStockholm line at all), so
 * every line also goes to the loader's log, as bsd/dev/vayu_nfc.c's say() does.
 */
static void stockholmLog(const char * format, ...) __printflike(1, 2);

static void
stockholmLog(const char * format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char * c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	IOLog("%s", line);
}

enum {
	kStockholmSetPower             = 0,     /* one scalar in: enable */
	kStockholmSetDownloadRequest   = 1,     /* one scalar in; non-zero is refused */
	kStockholmSetPMUStandbyPower   = 2,
	kStockholmSetDeviceWake        = 3,
	kStockholmSelector4            = 4,     /* no caller found in the HAL */
	kStockholmSetVBAT              = 5,
	kStockholmValidateSPMIConfig   = 6,     /* no scalars */
	kStockholmConfigureHammerfestSPMI = 7,  /* no scalars */
	kStockholmSelectorCount        = 8,
};

static const char * const kStockholmNames[kStockholmSelectorCount] = {
	"SetPower", "SetDownloadRequest", "SetPMUStandbyPowerEnabled", "SetDeviceWake",
	"selector 4", "SetVBAT", "ValidateSPMIConfig", "ConfigureHammerfestSPMI",
};

/* calls per selector, the last value of each setter, and the selectors already reported */
static uint32_t gStockholmCalls[kStockholmSelectorCount];
static uint64_t gStockholmLast[kStockholmSelectorCount];
static bool     gStockholmHaveLast[kStockholmSelectorCount];
static uint64_t gStockholmUnknown[8];   /* selectors 8..511, one bit each */

class AppleStockholmControl : public IOService
{
	OSDeclareDefaultStructors(AppleStockholmControl);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleStockholmControl, IOService);

bool
AppleStockholmControl::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleStockholmControlUserClient");
	/*
	 * _NFHardwareGetMatchingService (libNFC_HAL 0x1f51f1650) matches IOServiceMatching(class) with
	 * IOPropertyMatch {IONameMatched: "nfc,primary,gpio"}: on an iPhone the driver matched a device-tree
	 * node by that name. A test run had a boolean key instead: "Failed to get iterator for service",
	 * then "Failed to open device: Unknown device".
	 */
	setProperty("IONameMatched", "nfc,primary,gpio");
	setName("AppleStockholmControl");
	registerService();
	stockholmLog("MiniAppleStockholm: AppleStockholmControl published (no hardware access, step 1)\n");
	return true;
}

class AppleStockholmControlUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleStockholmControlUserClient);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleStockholmControlUserClient, IOUserClient);

bool
AppleStockholmControlUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties)
{
	char procname[32] = "?";

	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	proc_selfname(procname, sizeof(procname));
	stockholmLog("MiniAppleStockholm: %s[%d] opened the connection (type %u)\n", procname, proc_selfpid(), type);
	return true;
}

IOReturn
AppleStockholmControlUserClient::clientClose(void)
{
	char procname[32] = "?";

	proc_selfname(procname, sizeof(procname));
	stockholmLog("MiniAppleStockholm: %s[%d] closed the connection\n", procname, proc_selfpid());
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/*
 * The setters: one scalar in, nothing out. A value is logged for the first eight calls of its
 * selector and when it changes; selector 1 with a non-zero value is always refused and logged.
 */
static IOReturn
setScalar(uint32_t selector, IOExternalMethodArguments * args)
{
	uint64_t value;
	char procname[32] = "?";

	if (args->scalarInputCount < 1) {
		return kIOReturnBadArgument;
	}
	value = args->scalarInput[0];
	proc_selfname(procname, sizeof(procname));
	if (selector == kStockholmSetDownloadRequest && value != 0) {
		stockholmLog("MiniAppleStockholm: %s[%d] SetDownloadRequest(%llu) REFUSED: the PN553 is never put in download mode\n",
		    procname, proc_selfpid(), value);
		gStockholmCalls[selector]++;
		args->scalarOutputCount = 0;
		return kIOReturnNotPermitted;
	}
	if (gStockholmCalls[selector] < 8 || !gStockholmHaveLast[selector] || gStockholmLast[selector] != value) {
		stockholmLog("MiniAppleStockholm: %s[%d] %s(%llu)\n", procname, proc_selfpid(),
		    kStockholmNames[selector], value);
	}
	gStockholmCalls[selector]++;
	gStockholmLast[selector] = value;
	gStockholmHaveLast[selector] = true;
	args->scalarOutputCount = 0;
	return kIOReturnSuccess;
}

IOReturn
AppleStockholmControlUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	switch (selector) {
	case kStockholmSetPower:
	case kStockholmSetDownloadRequest:
	case kStockholmSetPMUStandbyPower:
	case kStockholmSetDeviceWake:
	case kStockholmSetVBAT:
		return setScalar(selector, args);

	case kStockholmSelector4:
	case kStockholmValidateSPMIConfig:
	case kStockholmConfigureHammerfestSPMI:
		if (gStockholmCalls[selector]++ < 4) {
			uint64_t in[4] = { 0, 0, 0, 0 };
			char procname[32] = "?";
			for (uint32_t i = 0; i < 4 && i < args->scalarInputCount; i++) {
				in[i] = args->scalarInput[i];
			}
			proc_selfname(procname, sizeof(procname));
			stockholmLog("MiniAppleStockholm: %s[%d] %s (%u scalars in: %llu %llu %llu %llu; "
			    "structure %u in, %u out)\n", procname, proc_selfpid(), kStockholmNames[selector],
			    args->scalarInputCount, in[0], in[1], in[2], in[3], args->structureInputSize,
			    args->structureOutputSize);
		}
		args->scalarOutputCount = 0;
		return kIOReturnSuccess;
	}

	if (selector < 512 && !(gStockholmUnknown[selector / 64] & (1ULL << (selector % 64)))) {
		char procname[32] = "?";

		gStockholmUnknown[selector / 64] |= 1ULL << (selector % 64);
		proc_selfname(procname, sizeof(procname));
		stockholmLog("MiniAppleStockholm: %s[%d] selector 0x%x not implemented (scalars %u in, %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarOutputCount, args->structureInputSize, args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* The registry node that makes _NFHardwareSerialGetDefaultInterfaceType answer 3. No user client. */
class AppleStockholmSPMI : public IOService
{
	OSDeclareDefaultStructors(AppleStockholmSPMI);
public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleStockholmSPMI, IOService);

bool
AppleStockholmSPMI::start(IOService * provider)
{
	if (!IOService::start(provider)) return false;
	setProperty("IONameMatched", "nfc,primary,spmi");    /* as above (0x1f51f1650) */
	setProperty("stockholm-spmi-data-socket", true);
	setName("AppleStockholmSPMI");
	registerService();
	stockholmLog("MiniAppleStockholm: AppleStockholmSPMI published (nfc,primary,spmi)\n");
	return true;
}

/* one client (nfcd, user mobile: no CTL_FLAG_PRIVILEGED); writes are logged and dropped */
static kern_ctl_ref gKctlRef;           /* registration ref, then the client's ref */
static uint32_t gKctlUnit, gKctlWrites, gKctlGetopts;
static int      gSpmiCrc, gSpmiOnDemand;
static uint8_t  gSpmiTermios[72];       /* TIOCSETA-shaped, zeros until nfcd sets it */

/*
 * The bridge (vayu_nfc_ios >= 2). nfcd's stream is raw NCI packets, but one write may carry
 * several packets or half of one and one read may do the same, so ctlSend appends to a 1 KB
 * buffer and takes whole packets out of it before the filter decides what the PN553 may see.
 */
static uint32_t gIosLevel;
static uint32_t gHwSpoof;               /* vayu_nfc_hwspoof=1: CORE_INIT_RSP hardware byte 0x41 shown to nfcd as 0x42 */
static uint32_t gPropInit;              /* vayu_nfc_propinit=1: also answer the 2F 06 and 2F 00 init steps (filter()) */
static uint8_t  gCtlReassembly[1024];
static size_t   gCtlReassemblyLength;
static uint32_t gFilterDecisions, gBridgeRxErrors, gForwardErrors, gAnswerErrors;

/* The filter's and the bridge's "one line per packet, then every 20th after 400" log. */
static void
filterLog(const uint8_t * packet, unsigned length, const char * decision)
{
	static const char hexdig[] = "0123456789abcdef";
	char hex[3 * 8 + 1];
	unsigned shown = length < 8 ? length : 8;
	size_t used = 0;

	for (unsigned i = 0; i < shown; i++) {
		hex[used++] = hexdig[packet[i] >> 4];
		hex[used++] = hexdig[packet[i] & 0xf];
		hex[used++] = i + 1 < shown ? ' ' : '\0';
	}
	if (shown == 0) hex[0] = '\0';
	if (++gFilterDecisions > 400 && (gFilterDecisions % 20) != 0) return;
	stockholmLog("MiniAppleStockholm: filter: %s -> %s\n", hex, decision);
}

static void
answerNfcd(const uint8_t * bytes, unsigned length)
{
	kern_ctl_ref ref = gKctlRef;
	uint32_t unit = gKctlUnit;
	errno_t error;

	if (ref == NULL) return;
	error = ctl_enqueuedata(ref, unit, (void *)(uintptr_t)bytes, length, 0);
	if (error != 0) {
		gAnswerErrors++;
		if (gAnswerErrors <= 8 || (gAnswerErrors % 100) == 0) {
			stockholmLog("MiniAppleStockholm: filter: answer of %u bytes not delivered: %d%s\n",
			    length, error, error == ENOBUFS ? " (ENOBUFS)" : "");
		}
	}
}

static void
forwardPacket(const uint8_t * log_packet, unsigned log_length,
    const uint8_t * packet, unsigned length, const char * decision)
{
	errno_t error;

	filterLog(log_packet, log_length, decision);
	error = vayu_nfc_bridge_send(packet, length);
	if (error != 0) {
		gForwardErrors++;
		if (gForwardErrors <= 8 || (gForwardErrors % 100) == 0) {
			stockholmLog("MiniAppleStockholm: filter: forward of %u bytes failed: %d%s\n",
			    length, error, error == EIO ? " (EIO)" : "");
		}
	}
}

static void
answerRejected(const uint8_t * packet, unsigned length, uint8_t first, uint8_t oid)
{
	uint8_t answer[4] = { first, oid, 0x01, 0x01 };
	char decision[32];

	snprintf(decision, sizeof(decision), "answer %02x %02x 01 01", first, oid);
	filterLog(packet, length, decision);
	answerNfcd(answer, sizeof(answer));
}

/*
 * CORE_SET_CONFIG_CMD: payload = [count][params], a param = [id][len][value]. NXP's extended
 * ids take two id bytes, the first 0xA0; the PN553 answers those, the firmware here does not
 * know them, so they are stripped and the count and the length are rebuilt.
 */
static void
filterSetConfig(const uint8_t * packet, unsigned length)
{
	uint8_t rebuilt[260];
	unsigned payload = packet[2];
	unsigned end = 3 + payload;
	unsigned input = 4, written = 0, kept = 0, stripped = 0;
	unsigned count;
	char decision[32];

	if (payload < 1 || length < 4 || length != end) {
		answerRejected(packet, length, 0x40, 0x02);
		return;
	}
	count = packet[3];
	rebuilt[0] = packet[0];
	rebuilt[1] = packet[1];
	for (unsigned i = 0; i < count; i++) {
		unsigned start = input;
		unsigned parameter_length, parameter_bytes;
		bool extended;

		if (input >= end) {
			answerRejected(packet, length, 0x40, 0x02);
			return;
		}
		extended = packet[input] == 0xa0;
		if (extended) {
			if (input + 1 >= end) {
				answerRejected(packet, length, 0x40, 0x02);
				return;
			}
			input += 2;
		} else {
			input++;
		}
		if (input >= end) {
			answerRejected(packet, length, 0x40, 0x02);
			return;
		}
		parameter_length = packet[input++];
		if (input + parameter_length > end) {
			answerRejected(packet, length, 0x40, 0x02);
			return;
		}
		parameter_bytes = input - start + parameter_length;
		if (extended) {
			stripped++;
		} else {
			if (4 + written + parameter_bytes > sizeof(rebuilt)) {
				answerRejected(packet, length, 0x40, 0x02);
				return;
			}
			memcpy(rebuilt + 4 + written, packet + start, parameter_bytes);
			written += parameter_bytes;
			kept++;
		}
		input += parameter_length;
	}
	if (input != end) {
		answerRejected(packet, length, 0x40, 0x02);
		return;
	}
	rebuilt[2] = (uint8_t)(1 + written);
	rebuilt[3] = (uint8_t)kept;
	if (kept == 0) {
		static const uint8_t ok[] = { 0x40, 0x02, 0x02, 0x00, 0x00 };

		filterLog(packet, length, "answer 40 02 02 00 00");
		answerNfcd(ok, sizeof(ok));
		return;
	}
	if (stripped != 0) {
		snprintf(decision, sizeof(decision), "strip %u", stripped);
	} else {
		snprintf(decision, sizeof(decision), "forward");
	}
	forwardPacket(packet, length, rebuilt, 4 + written, decision);
}

static void
filter(const uint8_t * packet, unsigned length)
{
	uint8_t mt = packet[0] >> 5;
	uint8_t gid = packet[0] & 0x0f;
	uint8_t oid = packet[1] & 0x3f;

	if (mt == 0) {                          /* data packet: straight to the chip */
		if (packet[1] != 0) {
			filterLog(packet, length, "drop");
			return;
		}
		forwardPacket(packet, length, packet, length, "forward");
		return;
	}
	if (mt != 1) {
		filterLog(packet, length, "drop");
		return;
	}
	if (gid == 0) {                         /* CORE */
		switch (oid) {
		case 0x00:                      /* CORE_RESET */
		case 0x01:                      /* CORE_INIT */
		case 0x03:                      /* CORE_GET_CONFIG */
		case 0x04:                      /* CORE_CONN_CREATE */
		case 0x05:                      /* CORE_CONN_CLOSE */
			forwardPacket(packet, length, packet, length, "forward");
			return;
		case 0x02:                      /* CORE_SET_CONFIG */
			filterSetConfig(packet, length);
			return;
		default:
			answerRejected(packet, length, 0x40, oid);
			return;
		}
	}
	if (gid == 1) {                         /* RF management */
		forwardPacket(packet, length, packet, length, "forward");
		return;
	}
	if (gid == 2) {                         /* NFCEE: no eSE and no UICC on this phone */
		answerRejected(packet, length, 0x42, oid);
		return;
	}
	if (gid == 0x0f) {                      /* NXP proprietary */
		/*
		 * 2F 03 = "configure the HSU baud rate" (builder phNciNfc_PropConfigHsuBr 0x1f4546f9c,
		 * label "phLibNfc_ConfigHsuBrCmd" 0x1f4548a20). On an iPhone the controller sits behind
		 * HSU; here nfcd's bytes go to the PN553 over I2C (bsd/dev/vayu_nfc.c), so there is no
		 * baud rate to change and the chip must not see the command. It cannot be refused either:
		 * phNciNfc_ConfigHsuBrCmdRsp 0x1f4581154 accepts only payload status 0 (44/0x2C means
		 * "retry" one level up, phLibNfc_ConfigHsuBrResp 0x1f454c548), the failure reaches
		 * phLibNfc as 0x00FF, and the init dies with "Stack load failed with 0x10" ->
		 * "_openDriver: Failed to start stack : 16" (TEST10: the third command of the session,
		 * 2F 03 08 10 00 00 5A 05 DC 02 04 -> the old blanket 4F 03 01 01). Status 0 with no
		 * payload is what phTmlNfc_ConfigHsuBaudRate() then needs; the kctl accepts the ioctl.
		 */
		if (oid == 0x03) {
			static const uint8_t hsu_ok[] = { 0x4f, 0x03, 0x01, 0x00 };

			filterLog(packet, length, "answer 4f 03 01 00 (no HSU here)");
			answerNfcd(hsu_ok, sizeof(hsu_ok));
			return;
		}
		/*
		 * 2F 19 = "get FW interface version" (builder phNciNfc_GetFwInterfaceVersionCmd
		 * 0x1f454ed38: GID 0x0F, OID 0x19, len 0). It is step 5 of the init sequence
		 * (phLibNfc_MwFwCompatibilityChk 0x1f455049c -> phNciNfc_GetFwInterfaceVersion
		 * 0x1f45ed110), before 2F 06 and 2F 00, so it must be answered locally whatever
		 * vayu_nfc_propinit says. Refusing it is fatal: phNciNfc_GetFwInterfaceVersionRsp
		 * 0x1f4580f34 takes payload[0] == 1 (REJECTED) or 5 (SYNTAX_ERROR) as "Hence this is
		 * OLD FW" and returns 0x80 (0x1f4580fa8); _NFDriverLoadMW 0x1f13e5570 turns 0x80/0x81
		 * into its status 2 ("Incompatible firmware !", 0x1f13e5778), and NFDriverLoadStack:441
		 * prints "Stack load failed with 0x2" -> "_openDriver: Failed to start stack : 2".
		 * The answer needs three payload bytes [status 0][version high][version low]; the
		 * response handler requires length 3 and phLibNfc_MwFwCompatibilityChkProcess
		 * 0x1f458f1ec compares rev16(the two bytes) with 21 for hw 0x42 (phLibNfc_GetNfccFeatures
		 * 0x1f4592b14 sets ctx[2811]=1 for hw 66; 0x1f458f274 expects 21). Hence 0x00 0x15.
		 */
		if (oid == 0x19) {
			static const uint8_t fw_interface_ok[] = { 0x4f, 0x19, 0x03, 0x00, 0x00, 0x15 };

			filterLog(packet, length, "answer 4f 19 03 00 00 15");
			answerNfcd(fw_interface_ok, sizeof(fw_interface_ok));
			return;
		}
		/*
		 * The rest of the 2F family stays refused; vayu_nfc_propinit=1 additionally answers the
		 * two proprietary init steps that come after the HSU one, locally and never to the chip:
		 * 2F 06 GET_FW_TYPE - phLibNfc_GetFwTypeProc 0x1f45e37a0 (pInfo at 0x1f45e3860) passes the
		 * status through, and with status 0 an empty payload only makes it clamp the fw type to 3
		 * (0x1f45e38a4);
		 * 2F 00 standby - phLibNfc_SetStandbyModeProcess 0x1f458f76c returns 255 on any non-zero
		 * status. Off by default so the next run still proves one variable at a time.
		 */
		if (gPropInit != 0 && oid == 0x06) {
			static const uint8_t fwtype_ok[] = { 0x4f, 0x06, 0x01, 0x00 };

			filterLog(packet, length, "answer 4f 06 01 00 (fw type 3)");
			answerNfcd(fwtype_ok, sizeof(fwtype_ok));
			return;
		}
		if (gPropInit != 0 && oid == 0x00) {
			static const uint8_t standby_ok[] = { 0x4f, 0x00, 0x01, 0x00 };

			filterLog(packet, length, "answer 4f 00 01 00 (standby)");
			answerNfcd(standby_ok, sizeof(standby_ok));
			return;
		}
		if (oid == 0x02) {
			forwardPacket(packet, length, packet, length, "forward");
		} else {
			answerRejected(packet, length, 0x4f, oid);
		}
		return;
	}
	/* A group this phone does not implement: reject instead of forwarding. */
	answerRejected(packet, length, (uint8_t)(0x40 | gid), oid);
}

static bool
ctlReassemblyAppend(const uint8_t * bytes, size_t count)
{
	if (count > sizeof(gCtlReassembly) - gCtlReassemblyLength) {
		stockholmLog("MiniAppleStockholm: reassembly: %zu bytes do not fit with %zu held: dropped\n",
		    count, gCtlReassemblyLength);
		gCtlReassemblyLength = 0;
		return false;
	}
	memcpy(gCtlReassembly + gCtlReassemblyLength, bytes, count);
	gCtlReassemblyLength += count;
	while (gCtlReassemblyLength >= 3) {
		size_t packet_length = 3u + gCtlReassembly[2];
		uint8_t mt;

		if (gCtlReassemblyLength < packet_length) break;
		mt = gCtlReassembly[0] >> 5;
		if (mt != 0 && mt != 1) {
			stockholmLog("MiniAppleStockholm: reassembly: desync (first byte 0x%02x, MT %u): "
			    "dropping %zu bytes\n", gCtlReassembly[0], mt, gCtlReassemblyLength);
			gCtlReassemblyLength = 0;
			break;
		}
		filter(gCtlReassembly, (unsigned)packet_length);
		gCtlReassemblyLength -= packet_length;
		if (gCtlReassemblyLength != 0) {
			memmove(gCtlReassembly, gCtlReassembly + packet_length, gCtlReassemblyLength);
		}
	}
	return true;
}

static void
ctlReassemblyMbuf(mbuf_t m, size_t total)
{
	uint8_t chunk[128];
	size_t offset = 0;

	while (offset < total) {
		size_t count = total - offset;

		if (count > sizeof(chunk)) count = sizeof(chunk);
		if (mbuf_copydata(m, offset, count, chunk) != 0) {
			stockholmLog("MiniAppleStockholm: reassembly: cannot copy %zu bytes at %zu of %zu: "
			    "dropped\n", count, offset, total);
			gCtlReassemblyLength = 0;
			return;
		}
		if (!ctlReassemblyAppend(chunk, count)) return;
		offset += count;
	}
}

/* Called on the bridge thread, with packets the PN553 has already put in the I2C buffer. */
static void
bridgeRx(const uint8_t * packet, unsigned length)
{
	kern_ctl_ref ref = gKctlRef;
	uint32_t unit = gKctlUnit;
	errno_t error;
	uint8_t spoofed[260];

	if (ref == NULL) return;
	/*
	 * vayu_nfc_hwspoof=1. iOS's NXP stack checks the CORE_INIT_RSP hardware byte right after INIT
	 * (libNFC_Comet phLibNfc_SysConfigInitRspCb 0x1f45e2a2c -> phLibNfc_GetPlatformInfo 0x1f4570764): the
	 * PN553's 0x41 is not in its table ("Unknown platform", a test run), 0x42 is (index 4). Only that one
	 * byte of that one answer is changed (NCI 1.x layout: payload[13 + n], n = the RF-interface count at
	 * payload[5]); the chip itself is never told anything else. Download mode and the 2Fxx commands stay
	 * refused by the filter whatever the stack decides from it.
	 */
	if (gHwSpoof != 0 && length >= 4 && length <= sizeof(spoofed) && packet[0] == 0x40 && packet[1] == 0x01 &&
	    length == 3u + packet[2] && packet[2] >= 6) {
		unsigned n = packet[3 + 5];
		unsigned hw = 3 + 13 + n;

		if (hw < length && packet[hw] == 0x41) {
			memcpy(spoofed, packet, length);
			spoofed[hw] = 0x42;
			stockholmLog("MiniAppleStockholm: hwspoof: CORE_INIT_RSP hardware byte 0x41 -> 0x42 for nfcd "
			    "(offset %u)\n", hw);
			packet = spoofed;
		}
	}
	error = ctl_enqueuedata(ref, unit, (void *)(uintptr_t)packet, length, 0);
	if (error != 0) {
		gBridgeRxErrors++;
		if (gBridgeRxErrors <= 8 || (gBridgeRxErrors % 100) == 0) {
			stockholmLog("MiniAppleStockholm: bridge rx: %u bytes not delivered: %d%s (error %u)\n",
			    length, error, error == ENOBUFS ? " (ENOBUFS)" : "", gBridgeRxErrors);
		}
	}
}

static errno_t
ctlConnect(kern_ctl_ref kctlref, struct sockaddr_ctl * sac, void ** unitinfo)
{
	char procname[32] = "?";

	proc_selfname(procname, sizeof(procname));
	gKctlRef = kctlref;
	gKctlUnit = sac != NULL ? sac->sc_unit : 0;
	*unitinfo = NULL;
	gCtlReassemblyLength = 0;
	stockholmLog("MiniAppleStockholm: %s[%d] connected unit %u\n", procname, proc_selfpid(), gKctlUnit);
	if (gIosLevel >= 2) {
		int error = vayu_nfc_bridge_start(bridgeRx);

		stockholmLog("MiniAppleStockholm: vayu_nfc_bridge_start -> %d%s\n", error,
		    error == 0 ? " (started)" : error == EPERM ? " (EPERM)" :
		    error == ENODEV ? " (ENODEV)" : error == EBUSY ? " (EBUSY)" : "");
	}
	return 0;
}

static errno_t
ctlDisconnect(kern_ctl_ref kctlref, u_int32_t unit, void * unitinfo)
{
	char procname[32] = "?";

	proc_selfname(procname, sizeof(procname));
	stockholmLog("MiniAppleStockholm: %s[%d] disconnected unit %u\n", procname, proc_selfpid(), unit);
	if (gIosLevel >= 2) {
		vayu_nfc_bridge_stop();
	}
	gCtlReassemblyLength = 0;
	gKctlRef = NULL;
	gKctlUnit = 0;
	return 0;
}

static errno_t
ctlSend(kern_ctl_ref kctlref, u_int32_t unit, void * unitinfo, mbuf_t m, int flags)
{
	uint8_t data[300];
	char line[3 * 64 + 4] = "", procname[32] = "?";
	size_t total = 0, copied, shown;
	mbuf_t cur;

	if (m != NULL && (mbuf_flags(m) & MBUF_PKTHDR)) {
		total = mbuf_pkthdr_len(m);
	} else {
		for (cur = m; cur != NULL; cur = mbuf_next(cur)) total += mbuf_len(cur);
	}
	copied = total > sizeof(data) ? sizeof(data) : total;
	if (copied > 0 && mbuf_copydata(m, 0, copied, data) != 0) copied = 0;
	proc_selfname(procname, sizeof(procname));
	/* after the first 200 writes only every 50th is reported */
	if (++gKctlWrites <= 200 || (gKctlWrites % 50) == 0) {
		static const char hexdig[] = "0123456789abcdef";
		shown = copied < 64 ? copied : 64;
		for (size_t i = 0; i < shown; i++) {
			line[3 * i] = hexdig[data[i] >> 4];
			line[3 * i + 1] = hexdig[data[i] & 0xf];
			line[3 * i + 2] = i + 1 < shown ? ' ' : '\0';
		}
		stockholmLog("MiniAppleStockholm: %s[%d] write %u bytes: %s%s\n", procname, proc_selfpid(),
		    (uint32_t) total, line, total > shown ? " ..." : "");
	}
	if (gIosLevel >= 2 && total != 0) {
		ctlReassemblyMbuf(m, total);
	}
	mbuf_freem(m);
	return 0;
}

static errno_t
ctlSetopt(kern_ctl_ref kctlref, u_int32_t unit, void * unitinfo, int opt, void * data, size_t len)
{
	int value = 0;

	if (data != NULL && len >= sizeof(int)) memcpy(&value, data, sizeof(value));
	stockholmLog("MiniAppleStockholm: setsockopt 0x%x len %zu value %d\n", (uint32_t) opt, len, value);
	switch ((uint32_t) opt) {
	case 0x80047301: gSpmiCrc = value; break;               /* IO_STOCKHOLM_CRC */
	case 0x80047302: gSpmiOnDemand = value; break;          /* IO_STOCKHOLM_ON_DEMAND_CLIENT */
	case 0x80487414:                                        /* TIOCSETA-shaped termios */
		memset(gSpmiTermios, 0, sizeof(gSpmiTermios));
		if (data != NULL && len > 0) {
			memcpy(gSpmiTermios, data, len < sizeof(gSpmiTermios) ? len : sizeof(gSpmiTermios));
		}
		break;
	default:
		stockholmLog("MiniAppleStockholm: setsockopt 0x%x unknown (len %zu)\n", (uint32_t) opt, len);
		break;
	}
	return 0;
}

static errno_t
ctlGetopt(kern_ctl_ref kctlref, u_int32_t unit, void * unitinfo, int opt, void * data, size_t * len)
{
	size_t n;

	if (len == NULL) return EINVAL;
	n = *len;
	if (++gKctlGetopts <= 20) {
		stockholmLog("MiniAppleStockholm: getsockopt 0x%x len %zu\n", (uint32_t) opt, n);
	}
	switch ((uint32_t) opt) {
	case 0x40487413:        /* TIOCGETA-shaped: what nfcd set, zeros at first */
		n = n < sizeof(gSpmiTermios) ? n : sizeof(gSpmiTermios);
		if (data != NULL) memcpy(data, gSpmiTermios, n);
		break;
	case 0x40407303:        /* IO_STOCKHOLM_SPMIERRORS: 64 zero bytes */
		n = n < 64 ? n : 64;
		if (data != NULL) memset(data, 0, n);
		break;
	default:
		stockholmLog("MiniAppleStockholm: getsockopt 0x%x unknown\n", (uint32_t) opt);
		if (data != NULL) memset(data, 0, n);
		break;
	}
	*len = n;
	return 0;
}

static void
registerSpmiControl(void)
{
	struct kern_ctl_reg reg = {};

	strlcpy(reg.ctl_name, kSpmiCtlName, sizeof(reg.ctl_name));
	/*
	 * A stream control: _NFHardwareSerialOpen does socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL) (libNFC_HAL
	 * 0x1f51ec204-0x1f51ec210: w1 = 1), and kern_control.c refuses a datagram control to it with EPROTOTYPE (test run
	 * 231946: "Protocol wrong type for socket" errno=41 connect). No CTL_FLAG_PRIVILEGED: nfcd runs as user mobile.
	 */
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = 4096;
	reg.ctl_recvsize = 8192;
	reg.ctl_connect = ctlConnect;
	reg.ctl_disconnect = ctlDisconnect;
	reg.ctl_send = ctlSend;
	reg.ctl_setopt = ctlSetopt;
	reg.ctl_getopt = ctlGetopt;
	errno_t err = ctl_register(&reg, &gKctlRef);
	if (err != 0) {
		gKctlRef = NULL;
	}
	stockholmLog("MiniAppleStockholm: kctl %s %s (%d)\n", kSpmiCtlName,
	    err == 0 ? "registered" : "registration failed", err);
}

static void
publishNode(IOService * node, IOService * provider, const char * name)
{
	if (node == NULL || !node->init() || !node->attach(provider)) {
		OSSafeReleaseNULL(node);
		stockholmLog("MiniAppleStockholm: cannot create %s\n", name);
		return;
	}
	if (!node->start(provider)) node->detach(provider);
	node->release();
}

void
MiniAppleStockholmStart(void)
{
	IOService * resources;

	/* the services exist only when asked for; every other boot stays without them */
	if (!PE_parse_boot_argn("vayu_nfc_ios", &gIosLevel, sizeof(gIosLevel)) || gIosLevel == 0) return;
	(void)PE_parse_boot_argn("vayu_nfc_hwspoof", &gHwSpoof, sizeof(gHwSpoof));
	/*
	 * Say it here and not only when a packet is rewritten: a test run had vayu_nfc_hwspoof=1 in the
	 * command line and not a single line about it, because nfcd never started and no CORE_INIT_RSP
	 * ever crossed the bridge (the kernel's own probe at vayu_nfc.c is a different path). This line
	 * makes the next log answer "was the option seen?" before the answer "was a byte rewritten?".
	 */
	if (gHwSpoof != 0) {
		stockholmLog("MiniAppleStockholm: hwspoof: on (CORE_INIT_RSP hardware byte 0x41 -> 0x42 for nfcd)\n");
	}
	(void)PE_parse_boot_argn("vayu_nfc_propinit", &gPropInit, sizeof(gPropInit));
	if (gPropInit != 0) {
		stockholmLog("MiniAppleStockholm: propinit: on (2F 06 GET_FW_TYPE and 2F 00 standby get a local "
		    "status 0; the rest of the 2F family stays refused)\n");
	}
	resources = IOService::getResourceService();
	if (resources == NULL) return;
	/* IOService:/IOResources/AppleStockholmControl and .../AppleStockholmSPMI */
	publishNode(OSTypeAlloc(AppleStockholmControl), resources, "AppleStockholmControl");
	publishNode(OSTypeAlloc(AppleStockholmSPMI), resources, "AppleStockholmSPMI");
	registerSpmiControl();
}
