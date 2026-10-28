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
 *
 * N9 (TEST21): the PN553 rejects the NXP proprietary CORE_SET_CONFIG parameter 0xA0BF ("NFCC CRC
 * check configuration", the only work of _NFDriverEnableCRC) with status 05, and after a local
 * success the re-init that follows waits for a CORE_RESET_NTF the chip never sends. The bridge now
 * answers A0BF locally and injects that one NTF; both are in filterSetConfig() and bridgeRx().
 *
 * N11 (TEST22C): the local A0BF answer and the synthetic CORE_RESET_NTF were not enough because
 * the success of A0BF makes Comet do phTmlNfc_IoCtl(70) (phLibNfc_IoctlSetPropRfCfgComplete
 * 0x1f456bd94) -> setsockopt IO_STOCKHOLM_CRC=1 -> phNciNfc_UpdateCrcChkFlag 0x1f45f15fc, and from
 * then on nfcd checks a CRC16 on every frame it receives (phNciNfc_CoreUtilsVerifyCrc 0x1f455612c)
 * and appends one to every frame it sends (phNciNfc_CoreUtilsAppendCrc 0x1f4550054). The bridge
 * sent plain NCI, so the very first frame after the flag - the CORE_RESET_RSP 40 00 03 00 10 00 -
 * was rejected: "Incorrect CRC / Received CRC: =0x6000 / Calculated CRC: =0xFCAF" (the 0x6000
 * were the first two bytes of the injected NTF, eaten as the CRC of the RSP). The re-init then
 * returned 137 = 0x89 and _NFDriverEnableCRC turned it into 13 = 0xd: "Step 2 error : 0xd".
 * The bridge now emulates the SPMI CRC in deliverToNfcd() (append) and ctlReassemblyAppend()
 * (verify/strip); nciCrc16() is CRC-16/CCITT-FALSE, poly 0x1021, init 0xFFFF, big-endian on the
 * wire. CRC(40 00 03 00 10 00) = 0xFCAF, exactly the number in the TEST22C log.
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
static uint32_t gNfceeMode;             /* vayu_nfc_nfcee=2: answer 0 NFCEEs locally; otherwise forward 22 00/22 01 */
static uint32_t gNfceeNtfFilter = 1;    /* vayu_nfc_nfcee_ntf=0: pass every 62 00, do not cut after the RSP count */
static int32_t  gNfceeAnnounced = -1;   /* NFCEE count from the forwarded 42 00 RSP; -1 = no RSP seen yet */
static uint32_t gNfceeNtfSeen;          /* 62 00 notifications passed since that RSP */

/*
 * N8 (TEST17): after 40 04 CORE_CONN_CREATE_RSP (status 0, conn id 3) nfcd waits 2.02 s and then
 * tears the session down. phLibNfc_OpenLogConnProcess 0x1f4593588 immediately calls
 * phHciNfc_CoreInit 0x1f45433a4; the first HCI command goes through phHciNfc_CoreSend
 * 0x1f45465fc -> phNciNfc_SeSendData 0x1f45670b8 -> the mux ->
 * phNciMuxNfc_BuildHciNciPkt 0x1f4541cb4. That function asks phNciNfc_GetConnCredits
 * 0x1f457a0c0 for credits; with none it calls phNciNfc_RegForConnCredits 0x1f457a1e8 and returns
 * "pending" (0x1f4541f44: strb w20,[x25,#841]; mov w22,#13), so no data packet is ever written.
 * The HCI command-response timer armed by phHciNfc_CoreSend is 2000 ms (0x1f45466e4), which is
 * exactly the observed 2.02 s. A synthetic CORE_CONN_CREDITS_NTF (60 06) for that conn id lets
 * the HCI CORE_RESET travel. The PN553 never sends 60 06 by itself for this connection.
 */
static uint32_t gNfcCredits;            /* vayu_nfc_credits=N: after a successful CORE_CONN_CREATE_RSP send 60 06 with N credits; 0 = off */
static uint32_t gNfcA0bf;               /* vayu_nfc_a0bf: 0 = answer 0xA0BF locally with success (default), 1 = forward as is, 3 = 2-byte probe */
static uint32_t gNfcData;               /* vayu_nfc_data=1: forward data packets on the logical connections learnt from 40 04 */
/*
 * N9: the PN553 never sends CORE_RESET_NTF (no "60 00" in any NCIDump of TEST17/20B/21), but the
 * re-init that phLibNfc_IoctlSetPropRfCfgComplete 0x1f456be04 starts after a successful 0xA0BF
 * waits for it: the RSP arrives, the stack still holds the NCI response timer, and 2.03 s later
 * phNciNfc_RspTimeOutCb 0x1f45a6f08 logs "NCI RSP : Timer expired before response is received!"
 * (TEST20B 16:53:00.016). The re-init then returns 137 = 0x89 ("NFCC Re-Init failed",
 * 0x1f456be3c), _NFDriverEnableCRC fails, "Step 2 error : 0xd", seven retries, "Max retries
 * reached" and "Failed to start stack : 16". A synthetic NTF after the RSP of a *re*-init closes
 * that loop; the first CORE_RESET of a session is passed without one, exactly as in TEST21,
 * because there the stack goes straight from the RSP to CORE_INIT.
 */
static uint32_t gNfcResetNtf = 1;       /* vayu_nfc_resetntf=0: never inject; default 1 */
static uint32_t gCoreInits;             /* CORE_INIT_RSP (40 01) seen: the next CORE_RESET is a re-init */
static uint32_t gResetNtfInjected;
/*
 * N11: the SPMI CRC16. gSpmiCrc is what nfcd asked for through IO_STOCKHOLM_CRC (0x80047301,
 * ctlSetopt); gNfcCrc is the bridge's own switch (vayu_nfc_crc=0 for an A/B run). When both are
 * non-zero every frame delivered to nfcd gets two CRC bytes appended and every frame received
 * from nfcd is expected to carry them (see nciCrc16(), deliverToNfcd(), ctlReassemblyAppend()).
 */
static uint32_t gNfcCrc = 1;            /* vayu_nfc_crc=0: no CRC emulation (diagnostic) */
static uint32_t gCrcAppended, gCrcStripped, gCrcPlainFromNfcd;
static int32_t  gLogConns[4] = { -1, -1, -1, -1 };      /* logical conn ids created by a successful CORE_CONN_CREATE_RSP */
static uint32_t gCreditsInjected, gCreditsErrors;
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

/*
 * N11: CRC-16/CCITT-FALSE, the checksum of the SPMI transport iOS turns on with IO_STOCKHOLM_CRC.
 * phNciNfc_CalcCrc16 (libNFC_Comet 0x1f45f17ac) is the same table-driven CRC: poly 0x1021,
 * init 0xFFFF, no reflection, no final xor; phNciNfc_CoreUtilsAppendCrc 0x1f4550054 writes it
 * with rev16, i.e. big-endian. Reference: CRC16(40 00 03 00 10 00) = 0xFCAF, the "Calculated
 * CRC" of TEST22C.
 */
static uint16_t
nciCrc16(const uint8_t * bytes, unsigned length)
{
	uint16_t crc = 0xffff;

	for (unsigned i = 0; i < length; i++) {
		crc ^= (uint16_t)((uint16_t)bytes[i] << 8);
		for (unsigned bit = 0; bit < 8; bit++) {
			if ((crc & 0x8000) != 0) {
				crc = (uint16_t)((crc << 1) ^ 0x1021);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}
	return crc;
}

static bool
spmiCrcEnabled(void)
{
	return gNfcCrc != 0 && gSpmiCrc != 0;
}

/*
 * One place that hands a complete NCI frame to nfcd. With the SPMI CRC on, the two checksum
 * bytes go after the frame; the chip itself never sees them (it is on the other side of the
 * bridge and the PN553 I2C link has no CRC). Every producer - the filter's answers, the
 * forwarded chip packets, the synthetic 60 06 credits and the synthetic 60 00 reset NTF -
 * must come through here, or nfcd rejects the frame with status 34 (0x22) and flushes TML.
 */
static errno_t
deliverToNfcd(kern_ctl_ref ref, uint32_t unit, const uint8_t * packet, unsigned length,
    const char * what)
{
	uint8_t framed[262];

	if (spmiCrcEnabled() && length + 2 <= sizeof(framed)) {
		uint16_t crc = nciCrc16(packet, length);

		memcpy(framed, packet, length);
		framed[length] = (uint8_t)(crc >> 8);
		framed[length + 1] = (uint8_t)(crc & 0xff);
		if (++gCrcAppended <= 8 || (gCrcAppended % 100) == 0) {
			stockholmLog("MiniAppleStockholm: crc: %s %u bytes + crc %02x %02x (#%u)\n", what,
			    length, framed[length], framed[length + 1], gCrcAppended);
		}
		return ctl_enqueuedata(ref, unit, framed, length + 2, 0);
	}
	return ctl_enqueuedata(ref, unit, (void *)(uintptr_t)packet, length, 0);
}

static void
answerNfcd(const uint8_t * bytes, unsigned length)
{
	kern_ctl_ref ref = gKctlRef;
	uint32_t unit = gKctlUnit;
	errno_t error;

	if (ref == NULL) return;
	error = deliverToNfcd(ref, unit, bytes, length, "answer");
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
 * The conn id in a successful 40 04 CORE_CONN_CREATE_RSP (payload: status, max data packet
 * payload size, number of connections, conn id...) is what phNciNfc_LogConnResp stores at
 * ctx+3392 (0x1f45432fc..0x1f4543320) and what HCI data packets then carry in byte 1.
 */
static void
rememberLogConn(uint8_t id)
{
	for (unsigned i = 0; i < sizeof(gLogConns) / sizeof(gLogConns[0]); i++) {
		if (gLogConns[i] == (int32_t)id) return;
	}
	for (unsigned i = 0; i < sizeof(gLogConns) / sizeof(gLogConns[0]); i++) {
		if (gLogConns[i] < 0) {
			gLogConns[i] = id;
			stockholmLog("MiniAppleStockholm: data: conn %u known (CORE_CONN_CREATE_RSP)\n", id);
			return;
		}
	}
	stockholmLog("MiniAppleStockholm: data: conn %u not remembered (table full)\n", id);
}

static bool
isLogConn(uint8_t id)
{
	for (unsigned i = 0; i < sizeof(gLogConns) / sizeof(gLogConns[0]); i++) {
		if (gLogConns[i] == (int32_t)id) return true;
	}
	return false;
}

/*
 * vayu_nfc_credits=N. A CORE_CONN_CREDITS_NTF is [60 06][03][count=1][conn id][credits]
 * (phNciNfc_LogConnNotifCB: payload[0] count, payload[1] conn id, payload[2] credits; the
 * credits are added to the per-connection entry at ctx+3393 and clamp at 254). The delay lets
 * nfcd process the 40 04 first: the registration for credits happens in
 * phNciNfc_RegForConnCredits, called from the HCI send that runs *after* the connect callback.
 */
static void
injectConnCredits(kern_ctl_ref ref, uint32_t unit, uint8_t conn)
{
	uint8_t ntf[6] = { 0x60, 0x06, 0x03, 0x01, conn, (uint8_t)gNfcCredits };
	errno_t error;

	IOSleep(20);
	if (gKctlRef != ref) return;
	error = deliverToNfcd(ref, unit, ntf, sizeof(ntf), "credits");
	if (error == 0) {
		gCreditsInjected++;
		if (gCreditsInjected <= 8 || (gCreditsInjected % 20) == 0) {
			stockholmLog("MiniAppleStockholm: credits: synthetic 60 06 03 01 %02x %02x "
			    "(conn %u, %u credits, #%u)\n", conn, (uint8_t)gNfcCredits, conn,
			    gNfcCredits, gCreditsInjected);
		}
	} else {
		gCreditsErrors++;
		if (gCreditsErrors <= 8 || (gCreditsErrors % 100) == 0) {
			stockholmLog("MiniAppleStockholm: credits: 60 06 for conn %u not delivered: %d%s\n",
			    conn, error, error == ENOBUFS ? " (ENOBUFS)" : "");
		}
	}
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
	unsigned input = 4, written = 0, kept = 0, stripped = 0, kept_extended = 0;
	unsigned count;
	bool saw_a0bf = false;
	char decision[48];

	/*
	 * N9: 20 02 05 01 A0 BF 01 01 is the whole work of _NFDriverEnableCRC (libPN548_API
	 * 0x1f13e65f0, step 2 of _NFDriverLoadStackSteps 0x1f13e4eb8). It reaches Comet as ioctl
	 * 226 (0xE2): phLibNfc_Ioctl_PropSetCrcChkConfig 0x1f455a194 ->
	 * phLibNfc_SM_IoctlCrcChkConfigTrans 0x1f4555808, which writes the parameter id itself -
	 * 0x1f4555888 mov w8,#49056 (0xBFA0); 0x1f455588c strh w8,[x1,#216]! - then length 1
	 * (0x1f4555894) and the value from the ioctl input (0x1f45558a0). So 0xA0BF is the NXP
	 * "NFCC CRC check configuration" parameter, value 1 = enable; it is not an RF parameter and
	 * not an OID. The PN553 answers 40 02 02 05 00 (status 0x05 SYNTAX_ERROR: the id is in no
	 * PN553 NXP configuration at all), and Comet resends five times ("Resend Count Reached")
	 * before "IoctlSetPropRfCfgComplete: Failed" / "Step 2 error : 0x1" (TEST21).
	 *
	 * The value is not what the stack needs: every SET_CONFIG status other than 0 (and 147 in
	 * one internal case) is failure (0x1f456bbbc cbz w20 / cmp w20,#147), so a local status 0 is
	 * all that is required; phLibNfc_IoctlSetPropRfCfgComplete then advances the sequence and
	 * re-inits the NFCC. Answer success locally (default). The completion then asks the host for
	 * CRC via phTmlNfc_IoCtl 70 (0x1f456bd94) -> setsockopt IO_STOCKHOLM_CRC, which ctlSetopt
	 * only logs: the SPMI link stays plain NCI, no CRC bytes are added or expected.
	 */
	if (length == 8 && packet[2] == 5 && packet[3] == 1 && packet[4] == 0xa0 && packet[5] == 0xbf &&
	    packet[6] == 1) {
		if (gNfcA0bf == 1) {
			/* TEST21 behaviour, kept for A/B: the PN553 answers 40 02 02 05 00. */
			stockholmLog("MiniAppleStockholm: a0bf: forwarding 20 02 05 01 a0 bf 01 %02x to the chip "
			    "(vayu_nfc_a0bf=1; the PN553 is expected to answer status 05)\n", packet[7]);
			forwardPacket(packet, length, packet, length, "forward (keep A0BF, strip 0)");
			return;
		}
		if (gNfcA0bf == 3) {
			/*
			 * H9-a probe: phNciNfc_BuildPropConfigParams 0x1f45eb1d4 builds the same id with a
			 * TWO-byte value (0x1f45eb40c mov w2,#41151; 0x1f45eb410 mov w3,#2). Ask the chip
			 * that form with value 0x0000 - even if it accepts, CRC stays off on the wire.
			 */
			static const uint8_t probe[] = {
				0x20, 0x02, 0x06, 0x01, 0xa0, 0xbf, 0x02, 0x00, 0x00
			};

			stockholmLog("MiniAppleStockholm: a0bf: probing the 2-byte form 20 02 06 01 a0 bf 02 00 00 "
			    "(vayu_nfc_a0bf=3; a 40 02 03 00 00 .. means the PN553 knows the parameter)\n");
			forwardPacket(packet, length, probe, sizeof(probe), "forward (A0BF 2-byte probe)");
			return;
		}
		{
			static const uint8_t ok[] = { 0x40, 0x02, 0x02, 0x00, 0x00 };

			stockholmLog("MiniAppleStockholm: a0bf: local success 40 02 02 00 00 for "
			    "20 02 05 01 a0 bf 01 %02x (NFCC CRC check config; nothing sent to the PN553)\n",
			    packet[7]);
			filterLog(packet, length, "answer 40 02 02 00 00 (A0BF local)");
			answerNfcd(ok, sizeof(ok));
			return;
		}
	}
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
		bool extended, keep_extended = false;

		if (input >= end) {
			answerRejected(packet, length, 0x40, 0x02);
			return;
		}
		/*
		 * N11: NXP's extended ids are two bytes and the first one is 0xA0 or 0xA1
		 * (phNciNfc_ProcessRfParams 0x1f45eb608 takes 0xA0/0xA1 that way). The PN553 knows
		 * only some 0xA0xx; nothing with 0xA1. Step 5 of _NFDriverLoadStackSteps
		 * (_NFDriverEnableWriteCounters 0x1f13e6cb0) asks for 0xA154 with
		 * "A1 54 01 01" through ioctl 0x90A1 and step 10 (_NFDriverSetupRFClockTimeout
		 * 0x1f13e73e4) computes its id at run time; both must be stripped, not forwarded.
		 */
		extended = packet[input] >= 0xa0;
		if (extended) {
			if (input + 1 >= end) {
				answerRejected(packet, length, 0x40, 0x02);
				return;
			}
			/*
			 * N9: 0xA0BF is the NFCC CRC check parameter (see the block above). Only
			 * vayu_nfc_a0bf=1 forwards it; in the default mode it is stripped like every
			 * other 0xA0xx and, when it was the only parameter, the whole command is
			 * answered locally with status 0.
			 */
			saw_a0bf = packet[input + 1] == 0xbf;
			keep_extended = gNfcA0bf == 1 && saw_a0bf;
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
		if (extended && !keep_extended) {
			stripped++;
		} else {
			if (4 + written + parameter_bytes > sizeof(rebuilt)) {
				answerRejected(packet, length, 0x40, 0x02);
				return;
			}
			memcpy(rebuilt + 4 + written, packet + start, parameter_bytes);
			written += parameter_bytes;
			kept++;
			if (keep_extended) kept_extended++;
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

		if (saw_a0bf) {
			stockholmLog("MiniAppleStockholm: a0bf: local success 40 02 02 00 00 (mixed SET_CONFIG, "
			    "%u parameter(s) stripped, nothing sent to the PN553)\n", stripped);
		}
		filterLog(packet, length, "answer 40 02 02 00 00");
		answerNfcd(ok, sizeof(ok));
		return;
	}
	if (kept_extended != 0) {
		snprintf(decision, sizeof(decision), "forward (keep A0BF, strip %u)", stripped);
	} else if (stripped != 0) {
		snprintf(decision, sizeof(decision), "strip %u", stripped);
	} else {
		snprintf(decision, sizeof(decision), "forward");
	}
	forwardPacket(packet, length, rebuilt, 4 + written, decision);
}

/*
 * CORE_GET_CONFIG_CMD. The first config nfcd reads after the init sequence is 0xA00F, the
 * controller's EEPROM user data. iOS 16.5 (libNFC_Comet 20F66) expects a 64-byte value
 * there and nothing else: phNciNfc_ProcessRfParams 0x1f45eb608 takes a 0xA0/0xA1 parameter
 * as a two-byte id and the length from the third byte (0x1f45ebc78 ldrb w8,[x10,#2]!,
 * 0x1f45ebc80 cbz), and its 0xA00F branch is 0x1f45ebe7c (ldrb w8,[x23,#1]; cmp w8,#15),
 * 0x1f45ebe8c (cmp w8,#64; b.ne 0x1f45ecb00 -> w21=1). Any other length, including the
 * 0x20 bytes the PN553 sends, fails the parse; phLibNfc_GetEepromParamsProc 0x1f458f7c0
 * then runs with status 1 and returns 255 (0x1f458f7fc cbnz w20 -> 0x1f458f844), so
 * phLibNfc_Mgt_Initialize completes with 1 and _NFDriverLoadMW 0x1f13e5570 prints
 * "status=0x0001" (0x1f13e5bac) and "Stack load failed with 0x10" (via NFDriverLoadStack).
 *
 * Answer it locally with the 64-byte block phLibNfc_SetEepromParams 0x1f458fe84 writes
 * after a successful configuration: wCurrMwVer 0x0134 (0x1f458ff00), wCurrFwVer (read only
 * by the "EEPROM User param - wCurrFwVer" log line, 0x1f458f910), bConfig 0xC3C3A5A5 which
 * makes phLibNfc_GetEepromParamsProc 0x1f458f9bc say "Skipping set config" and advance the
 * sequence by five steps, then 56 zero bytes - the same 8 + 56 layout SetEepromParams
 * zeroes and fills at ctx+4053 (= the descriptor's A00F value, ctx+3440+613). The chip is
 * still asked nothing: every CORE_SET_CONFIG carrying an 0xA0xx parameter is stripped in
 * filterSetConfig(), so the PN553's own 32-byte 0xA00F is never a source of truth here.
 */
static void
filterGetConfig(const uint8_t * packet, unsigned length)
{
	/* 20 03 03 01 a0 0f: one parameter, 0xA00F, no value. */
	if (length == 6 && packet[2] == 3 && packet[3] == 1 && packet[4] == 0xa0 && packet[5] == 0x0f) {
		static const uint8_t eepromUserData[64] = {
			0x34, 0x01,             /* wCurrMwVer = 0x0134, as phLibNfc_SetEepromParams writes it */
			0x00, 0x00,             /* wCurrFwVer: read only by phLibNfc_LogEepromUserData */
			0xa5, 0xa5, 0xc3, 0xc3, /* bConfig = 0xC3C3A5A5: "Skipping set config" */
		};
		uint8_t answer[3 + 1 + 1 + 3 + sizeof(eepromUserData)];

		answer[0] = 0x40;                       /* CORE_GET_CONFIG_RSP */
		answer[1] = 0x03;
		answer[2] = (uint8_t)(sizeof(answer) - 3);
		answer[3] = 0x00;                       /* status OK */
		answer[4] = 0x01;                       /* one parameter */
		answer[5] = 0xa0;                       /* 0xA00F */
		answer[6] = 0x0f;
		answer[7] = (uint8_t)sizeof(eepromUserData);
		memcpy(answer + 8, eepromUserData, sizeof(eepromUserData));
		filterLog(packet, length, "answer 40 03 45 .. a0 0f 40 (64-byte EEPROM user data)");
		answerNfcd(answer, sizeof(answer));
		return;
	}
	forwardPacket(packet, length, packet, length, "forward");
}

/*
 * NFCEE management (GID 2). iOS 16.5's init asks NFCEE_DISCOVER (22 00) right after the
 * CORE_SET_CONFIG/GET_CONFIG block. Two facts decide what the bridge must do with it.
 *
 * 1. The response shape is [status][number of NFCEEs], payload length 2;
 *    phNciNfc_ProcessNfceeDiscoverRsp 0x1f45a3aa0 reads it at ctx+3360/3368:
 *      0x1f45a3ae0  ldr  w9, [x19, #3368]    ; payload length; >2 -> 255
 *      0x1f45a3aec  ldrb w8, [x8]            ; status
 *      0x1f45a3af0  cmp  w8, #0xf1           ; already started -> 81
 *      0x1f45a3af8  cbnz w8, 0x1f45a3b58     ; any other status -> 255
 *      0x1f45a3b24  ldrb w5, [x8, #1]        ; number of NFCEEs
 *      0x1f45a3b28  strb w5, [x19, #3545]
 *    TEST14's 42 00 01 01 (status 1) takes the 255 branch: "LibNfc Init/Re-init Failed"
 *    in 16 ms. TEST15's local 42 00 02 00 00 (status 0, count 0) passes this parser.
 *
 * 2. Count 0 nevertheless does not finish the init. phNciNfc_CompleteNfceeDiscoverSequence
 *    0x1f4555290 does register the 62 00 handler (0x1f4555310, handler
 *    phNciNfc_NfceeDiscNtfHandler 0x1f45a41a0) and calls phNciNfc_Notify 0x1f45d552c at
 *    0x1f45553cc at once - the branch at 0x1f4555380 is only the "DeRegistering for Nfcee
 *    Discover Notification" / stop-discovery path (ctx+4088 = 1 is set in
 *    phNciNfc_Nfcee_StartDiscovery 0x1f45ed4ec), it is not a timer. What then runs out is
 *    phLibNfc_InitCompleteWdTimer 0x1f4559874, armed by phLibNfc_InitializeComplete
 *    0x1f45533dc with 1000 ms and ctx+2752 = 8 retries (0x1f4559a2c); its callback
 *    phLibNfc_InitCompleteWdTimerCb 0x1f45d4674 restarts it at 0x1f45d477c (1000 ms) until
 *    the count is 0, then 0x1f45d48c0-0x1f45d48c8 calls phLibNfc_ReleaseLibNfcContext
 *    with 255 -> "LibNfc Init/Re-init Failed" 0x1f4593e20. 1 + 8 s = the 9.118 s silence of
 *    TEST15. The only thing that stops that timer is phLibNfc_staticFeatureCfgComplete
 *    0x1f4558880, which is reached from phLibNfc_NfceeInitializeComplete 0x1f459474c after
 *    the per-SE (HCI child device) init - i.e. iOS expects the discovery result to agree
 *    with its own SE map (A0EC=1/A0ED=0 -> ctx+325 = 3 at phLibNfc_GetNfceeIntfCfgProc
 *    0x1f45903fc, so the empty "no SE" path at 0x1f45533e0..0x1f455340c is not taken).
 *
 * So an empty list is only consistent when the controller itself says it has no NFCEE.
 * The PN553 keeps A0EC=1 because filterSetConfig() strips iOS's "A0EC=0/A0ED=1" write, and
 * its firmware is the only authority on what it really reports. Forward 22 00 (discover is
 * read only) and the 22 01 that follows when the chip reports an NFCEE; a local "0 NFCEE"
 * answer stays available for comparison, where 22 01 is refused as before.
 *
 * TEST15B (forward, job 0766) proved the point: the chip answered 42 00 02 00 01 and sent
 * 62 00 05 01 00 01 01 00 (id 0x01, status DISABLED, one protocol, no TLV); nfcd then sent
 * 20 04 06 03 01 01 02 01 01 CORE_CONN_CREATE. But the chip also sent two more 62 00 (ids
 * 0x81 and 0x02) for one announced NFCEE, iOS keeps one SE entry per NTF, and the init ended
 * in "NCI RSP: Timer expired". bridgeRx() therefore passes only the first "count" 62 00 of
 * the current discovery (vayu_nfc_nfcee_ntf=0 disables that cut).
 *
 * vayu_nfc_nfcee=0 (default) or 1: forward (1 keeps the old spelling used by TEST15)
 * vayu_nfc_nfcee=2: answer 42 00 02 00 00 locally, refuse 22 01 (N6/TEST15 behaviour)
 */
static void
filterNfcee(const uint8_t * packet, unsigned length)
{
	uint8_t oid = packet[1] & 0x3f;

	if (oid == 0x00) {                      /* NFCEE_DISCOVER */
		static const uint8_t no_nfcee[] = { 0x42, 0x00, 0x02, 0x00, 0x00 };

		if (gNfceeMode == 2) {
			filterLog(packet, length, "answer 42 00 02 00 00 (0 NFCEEs, local)");
			answerNfcd(no_nfcee, sizeof(no_nfcee));
			return;
		}
		/*
		 * A new discovery: forget the previous list. The 62 00 count filter in bridgeRx() is
		 * re-armed by the 42 00 that the chip answers this command with.
		 */
		gNfceeAnnounced = -1;
		gNfceeNtfSeen = 0;
		forwardPacket(packet, length, packet, length, "forward (NFCEE_DISCOVER)");
		return;
	}
	if (oid == 0x01) {                      /* NFCEE_MODE_SET */
		if (gNfceeMode == 2) {
			/* No NFCEE was reported, so there is nothing to switch on. */
			answerRejected(packet, length, 0x42, oid);
			return;
		}
		forwardPacket(packet, length, packet, length, "forward (NFCEE_MODE_SET)");
		return;
	}
	/* The rest of GID 2 (22 02..22 3f) has no meaning for this board. */
	answerRejected(packet, length, 0x42, oid);
}

static void
filter(const uint8_t * packet, unsigned length)
{
	uint8_t mt = packet[0] >> 5;
	uint8_t gid = packet[0] & 0x0f;
	uint8_t oid = packet[1] & 0x3f;
	char decision[64];

	if (mt == 0) {                          /* data packet: straight to the chip */
		/*
		 * N8: the HCI CORE_RESET/INIT traffic for the NFCEE runs on the logical connection
		 * created by 20 04 (conn id 3 in TEST15B/17), so it is a data packet 00 03 ... .
		 * Until vayu_nfc_data=1 these were dropped and HCI could never start. The whitelist
		 * is the conn id from a successful 40 04; conn 0 (static RF) is always passed.
		 */
		if (packet[1] == 0) {
			forwardPacket(packet, length, packet, length, "forward");
			return;
		}
		if (gNfcData != 0 && isLogConn(packet[1])) {
			snprintf(decision, sizeof(decision), "forward (data, conn %u)", packet[1]);
			forwardPacket(packet, length, packet, length, decision);
			return;
		}
		if (gNfcData == 0) {
			snprintf(decision, sizeof(decision), "drop (data on conn %u; vayu_nfc_data=0)", packet[1]);
		} else {
			snprintf(decision, sizeof(decision), "drop (data on unknown conn %u)", packet[1]);
		}
		filterLog(packet, length, decision);
		return;
	}
	if (mt != 1) {
		filterLog(packet, length, "drop");
		return;
	}
	if (gid == 0) {                         /* CORE */
		switch (oid) {
		case 0x00:                      /* CORE_RESET */
			/*
			 * N8: only NCI's two defined payloads go through: 0x00 keep configuration,
			 * 0x01 reset configuration. Anything else (download/PN553 test modes) is
			 * refused with CORE_RESET_RSP status 1.
			 */
			if (length >= 4 && packet[3] > 0x01) {
				filterLog(packet, length, "refuse 40 00 01 01 (bad CORE_RESET payload)");
				answerRejected(packet, length, 0x40, 0x00);
				return;
			}
			forwardPacket(packet, length, packet, length, "forward");
			return;
		case 0x01:                      /* CORE_INIT */
		case 0x04:                      /* CORE_CONN_CREATE */
		case 0x05:                      /* CORE_CONN_CLOSE */
			forwardPacket(packet, length, packet, length, "forward");
			return;
		case 0x03:                      /* CORE_GET_CONFIG: 0xA00F is answered locally */
			filterGetConfig(packet, length);
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
		/*
		 * Forward. RF_DISCOVER_MAP (21 00) only fills the protocol-to-interface table and
		 * RF_DISCOVER (21 03) starts polling; the same two commands bsd/dev/vayu_nfc.c sends
		 * to this PN553 in its own discovery step (build_discover, vayu_nfc.c:1354 and :1398),
		 * so the chip is known to answer them. RF_DISCOVER_SELECT (21 04) and RF_DEACTIVATE
		 * (21 06) are the rest of that conversation. None of them is a firmware/download
		 * operation. CORE_SET_CONFIG (20 02) is the one that is filtered, above: 0xA0xx
		 * parameters are stripped, the standard ones still go to the chip.
		 */
		forwardPacket(packet, length, packet, length, "forward");
		return;
	}
	if (gid == 2) {                         /* NFCEE: no eSE and no UICC on this phone */
		filterNfcee(packet, length);
		return;
	}
	if (gid == 0x0f) {                      /* NXP proprietary */
		/*
		 * N8: 2F 2C, phNciNfc_SetConfigSigned 0x1f45e64d8 ("phLibNfc_Ioctl_PropSetConfigSigned"
		 * 0x1f4575c18), is the signed firmware/RF blob write. It is never needed for bring-up,
		 * and it is the one 2F command that can touch persistent controller data. Refuse it
		 * explicitly (0x4F 0x2C 01 01) before the generic 2F handling; every other 0x2F OID
		 * outside the whitelist below also ends in answerRejected(), never in a forward.
		 */
		if (oid == 0x2c) {
			filterLog(packet, length, "refuse 4f 2c 01 01 (SET_CONFIG_SIGNED/download)");
			answerRejected(packet, length, 0x4f, oid);
			return;
		}
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
			/*
			 * N8: 2F 02 is the zero-length "enable NXP extensions" command
			 * (phNciNfc_EnableExtnCmd 0x1f45e5300, payload length 0). A 2F 02 with a
			 * payload is not that command and has no known safe meaning here; refuse it.
			 */
			if (length != 3) {
				filterLog(packet, length, "refuse 4f 02 01 01 (2F 02 with payload)");
				answerRejected(packet, length, 0x4f, oid);
				return;
			}
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
ctlReassemblyAppend(const uint8_t * bytes, size_t count, bool final)
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
		size_t consumed = packet_length;
		uint8_t mt = gCtlReassembly[0] >> 5;

		if (mt != 0 && mt != 1) {
			stockholmLog("MiniAppleStockholm: reassembly: desync (first byte 0x%02x, MT %u): "
			    "dropping %zu bytes\n", gCtlReassembly[0], mt, gCtlReassemblyLength);
			gCtlReassemblyLength = 0;
			break;
		}
		/*
		 * N11: with the SPMI CRC on, nfcd appends the two checksum bytes itself
		 * (phNciNfc_CorePktSendToTml 0x1f454d8e0 -> phNciNfc_CoreUtilsAppendCrc 0x1f4550054)
		 * once phNciNfc_UpdateCrcChkFlag has armed the flag. The flag becomes active only in
		 * phNciNfc_ProcessResetRsp (0x1f458087c copies ctx+5825 to ctx+5824), so the first
		 * CORE_RESET after the A0BF ioctl can still be plain. Decide per frame by checking:
		 * a frame whose last two bytes are its CRC is stripped, anything else is passed on as
		 * plain NCI (and counted, so the log says which case happened).
		 */
		if (spmiCrcEnabled()) {
			if (gCtlReassemblyLength >= packet_length + 2) {
				uint16_t received = (uint16_t)(((uint16_t)gCtlReassembly[packet_length] << 8) |
				    gCtlReassembly[packet_length + 1]);
				uint16_t calculated = nciCrc16(gCtlReassembly, (unsigned)packet_length);

				if (received == calculated) {
					consumed = packet_length + 2;
					if (++gCrcStripped <= 8 || (gCrcStripped % 100) == 0) {
						stockholmLog("MiniAppleStockholm: crc: nfcd frame %zu bytes "
						    "crc %04x verified and stripped (#%u)\n", packet_length,
						    calculated, gCrcStripped);
					}
				} else {
					gCrcPlainFromNfcd++;
					if (gCrcPlainFromNfcd <= 8) {
						stockholmLog("MiniAppleStockholm: crc: nfcd frame %zu bytes "
						    "has no CRC (read %04x, expected %04x): passing plain "
						    "(#%u)\n", packet_length, received, calculated,
						    gCrcPlainFromNfcd);
					}
				}
			} else if (!final) {
				/* the rest of this frame (and maybe its CRC) may still arrive */
				break;
			} else if (gCtlReassemblyLength >= packet_length) {
				gCrcPlainFromNfcd++;
				if (gCrcPlainFromNfcd <= 8) {
					stockholmLog("MiniAppleStockholm: crc: nfcd frame %zu bytes is "
					    "too short to carry a CRC: passing plain (#%u)\n",
					    packet_length, gCrcPlainFromNfcd);
				}
			}
		}
		if (gCtlReassemblyLength < packet_length) break;
		filter(gCtlReassembly, (unsigned)packet_length);
		gCtlReassemblyLength -= consumed;
		if (gCtlReassemblyLength != 0) {
			memmove(gCtlReassembly, gCtlReassembly + consumed, gCtlReassemblyLength);
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
		if (!ctlReassemblyAppend(chunk, count, offset + count >= total)) return;
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
	/*
	 * NFCEE discovery: TEST15B showed the PN553 answering 42 00 02 00 01 (one NFCEE) and then
	 * sending three 62 00 notifications (ids 01, 81, 02). iOS keeps one SE entry per NTF and
	 * then tries to connect to each of them; the extras describe NFCEEs its own configuration
	 * does not have (A0ED=0 -> no UICC), and the init ends in "NCI RSP: Timer expired". Keep
	 * the list to the count the RSP announced: the first N notifications pass, the rest are
	 * dropped here. vayu_nfc_nfcee_ntf=0 passes every NTF instead.
	 */
	if (length >= 5 && packet[0] == 0x42 && packet[1] == 0x00 && packet[2] == 0x02) {
		gNfceeAnnounced = packet[4];
		gNfceeNtfSeen = 0;
		stockholmLog("MiniAppleStockholm: nfcee rsp: status %02x, %u NFCEE(s)\n",
		    packet[3], packet[4]);
	} else if (length >= 4 && packet[0] == 0x62 && packet[1] == 0x00) {
		/* payload: [0] id, [1] status, [2] number of supported protocols (filterNfcee()) */
		uint8_t nfcee_id = packet[3];
		uint8_t nfcee_status = length > 4 ? packet[4] : 0;
		uint8_t nfcee_protocols = length > 5 ? packet[5] : 0;

		if (gNfceeNtfFilter != 0 && gNfceeAnnounced >= 0 &&
		    gNfceeNtfSeen >= (uint32_t)gNfceeAnnounced) {
			stockholmLog("MiniAppleStockholm: nfcee ntf: id %02x status %02x dropped "
			    "(RSP announced %d NFCEE(s), %u already passed)\n",
			    nfcee_id, nfcee_status, gNfceeAnnounced, gNfceeNtfSeen);
			return;
		}
		gNfceeNtfSeen++;
		stockholmLog("MiniAppleStockholm: nfcee ntf: %u bytes: id %02x status %02x protocols %u\n",
		    length, nfcee_id, nfcee_status, nfcee_protocols);
	}
	error = deliverToNfcd(ref, unit, packet, length, "rx");
	if (error != 0) {
		gBridgeRxErrors++;
		if (gBridgeRxErrors <= 8 || (gBridgeRxErrors % 100) == 0) {
			stockholmLog("MiniAppleStockholm: bridge rx: %u bytes not delivered: %d%s (error %u)\n",
			    length, error, error == ENOBUFS ? " (ENOBUFS)" : "", gBridgeRxErrors);
		}
		return;
	}
	/*
	 * N8: 40 04 CORE_CONN_CREATE_RSP, payload [status][max pld][count][conn id...]. Remember the
	 * conn id for data forwarding and, with vayu_nfc_credits=N, answer the credit wait that
	 * phNciNfc_RegForConnCredits started. The NTF goes after the RSP on the same socket, 20 ms
	 * later (injectConnCredits), so nfcd has already run phLibNfc_OpenLogConnProcess.
	 */
	if (length >= 7 && packet[0] == 0x40 && packet[1] == 0x04 && packet[2] >= 4 && packet[3] == 0x00) {
		uint8_t conn = packet[6];

		rememberLogConn(conn);
		if (gNfcCredits != 0) {
			injectConnCredits(ref, unit, conn);
		}
	}
	/*
	 * N9: the CORE_RESET_NTF the PN553 never sends. The first CORE_RESET of a session is fine
	 * without it (TEST21: RSP, then CORE_INIT 0.7 ms later), but every CORE_RESET that follows a
	 * CORE_INIT belongs to a re-init - the one phLibNfc_IoctlSetPropRfCfgComplete starts after
	 * the 0xA0BF CRC config - and that one waits for the NTF until the 2 s NCI response timer
	 * fires (TEST20B NCIDump: RSP at ...614.989708, Rx-Abort at ...617.020559). NCI 1.0 payload:
	 * [reset trigger 00][configuration status 00][NCI version 10][manufacturer info len 00].
	 * phNciNfc_ResetNtfCb 0x1f4582070 takes the payload as opaque bytes, so only the shape
	 * matters here.
	 */
	if (gNfcResetNtf != 0 && gCoreInits != 0 && length >= 4 && packet[0] == 0x40 &&
	    packet[1] == 0x00 && packet[3] == 0x00) {
		static const uint8_t reset_ntf[] = {
			0x60, 0x00, 0x05, 0x00, 0x00, 0x10, 0x00, 0x00
		};

		error = deliverToNfcd(ref, unit, reset_ntf, sizeof(reset_ntf), "resetntf");
		if (error == 0) {
			gResetNtfInjected++;
			stockholmLog("MiniAppleStockholm: resetntf: synthetic 60 00 05 00 00 10 00 00 after the "
			    "CORE_RESET_RSP of a re-init (#%u)%s\n", gResetNtfInjected,
			    spmiCrcEnabled() ? " + CRC" : "");
		} else {
			stockholmLog("MiniAppleStockholm: resetntf: 60 00 not delivered: %d%s\n", error,
			    error == ENOBUFS ? " (ENOBUFS)" : "");
		}
	}
	if (length >= 3 && packet[0] == 0x40 && packet[1] == 0x01) {
		if (gCoreInits == 0) {
			stockholmLog("MiniAppleStockholm: resetntf: CORE_INIT_RSP seen; a later CORE_RESET is a "
			    "re-init and will get the synthetic NTF\n");
		}
		gCoreInits++;
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
	gCoreInits = 0;         /* N9: a new session starts with its own first CORE_RESET (no synthetic NTF) */
	gSpmiCrc = 0;           /* N11: nfcd turns the SPMI CRC on again after its A0BF, per session */
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
	gNfceeAnnounced = -1;
	gNfceeNtfSeen = 0;
	/*
	 * N9: a new nfcd session starts with its own first CORE_RESET, which must not get the
	 * synthetic NTF (the stack goes straight to CORE_INIT there).
	 */
	gCoreInits = 0;
	gSpmiCrc = 0;
	for (unsigned i = 0; i < sizeof(gLogConns) / sizeof(gLogConns[0]); i++) {
		gLogConns[i] = -1;
	}
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
	case 0x80047301:
		/*
		 * N9/N11: IO_STOCKHOLM_CRC. nfcd sets this right after the 0xA0BF answer, from
		 * phLibNfc_IoctlSetPropRfCfgComplete 0x1f456bd94 (phTmlNfc_IoCtl 70 when the CRC value
		 * is 1, 71 when 0). Comet then checks the CRC16 on every received frame and appends
		 * one to every sent frame (phNciNfc_UpdateCrcChkFlag 0x1f45f15fc, VerifyCrc 0x1f455612c,
		 * AppendCrc 0x1f4550054). The bridge emulates that on its own side of the SPMI link:
		 * deliverToNfcd() adds the two bytes, ctlReassemblyAppend() verifies and strips them.
		 * The PN553 itself stays on plain NCI.
		 */
		if (gSpmiCrc != value) {
			stockholmLog("MiniAppleStockholm: crc: nfcd asks for NFCC CRC (IO_STOCKHOLM_CRC=%d); "
			    "the bridge %s CRC16 on its side of the SPMI link (the PN553 stays plain NCI)\n",
			    value, (value != 0 && gNfcCrc != 0) ? "emulates" : "does not emulate");
		}
		gSpmiCrc = value;
		break;
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
	/*
	 * NFCEE (see filterNfcee()). Default is forward: the PN553's own discovery answer and its
	 * 62 00 notifications are the only pair that agrees with the A0EC=1 its EEPROM keeps, so
	 * forward both 22 00 and 22 01. vayu_nfc_nfcee=2 answers 42 00 02 00 00 locally and
	 * refuses 22 01 (the N6/TEST15 behaviour, kept for A/B runs). =1 is the old spelling of
	 * "forward" and still means that.
	 */
	(void)PE_parse_boot_argn("vayu_nfc_nfcee", &gNfceeMode, sizeof(gNfceeMode));
	if (gNfceeMode == 2) {
		stockholmLog("MiniAppleStockholm: nfcee: answering NFCEE_DISCOVER (22 00) locally with "
		    "0 NFCEEs; 22 01 refused\n");
	} else {
		stockholmLog("MiniAppleStockholm: nfcee: forwarding NFCEE_DISCOVER (22 00) and "
		    "NFCEE_MODE_SET (22 01) to the chip\n");
	}
	/*
	 * TEST15B: the chip announced one NFCEE but sent three 62 00. The count filter in
	 * bridgeRx() keeps the notifications equal to the announced number. vayu_nfc_nfcee_ntf=0
	 * turns it off (pure forward, the TEST15B behaviour).
	 */
	(void)PE_parse_boot_argn("vayu_nfc_nfcee_ntf", &gNfceeNtfFilter, sizeof(gNfceeNtfFilter));
	if (gNfceeNtfFilter == 0) {
		stockholmLog("MiniAppleStockholm: nfcee: 62 00 count filter off (every NTF passed)\n");
	}
	/*
	 * N8 (TEST17). Three independent switches, all off by default so an A/B run changes one
	 * variable at a time:
	 *   vayu_nfc_credits=N  after 40 04 status 0 send 60 06 03 01 <conn> N; this answers the
	 *                       credit wait that otherwise holds the first HCI packet for 2000 ms.
	 *   vayu_nfc_a0bf=1     keep 0xA0BF in a CORE_SET_CONFIG and forward it to the PN553.
	 *                       N9 changed the default: 0 answers it locally with success (the
	 *                       NFCC CRC check config of _NFDriverEnableCRC), 1 is the old TEST21
	 *                       forward, 3 is the 2-byte probe; see filterSetConfig().
	 *   vayu_nfc_data=1     forward data packets (00 <conn> ...) on the conn id from 40 04.
	 */
	(void)PE_parse_boot_argn("vayu_nfc_credits", &gNfcCredits, sizeof(gNfcCredits));
	if (gNfcCredits > 254) gNfcCredits = 254;
	if (gNfcCredits != 0) {
		stockholmLog("MiniAppleStockholm: credits: on (synthetic 60 06 with %u credits after a "
		    "successful CORE_CONN_CREATE_RSP)\n", gNfcCredits);
	}
	(void)PE_parse_boot_argn("vayu_nfc_a0bf", &gNfcA0bf, sizeof(gNfcA0bf));
	if (gNfcA0bf == 1) {
		stockholmLog("MiniAppleStockholm: a0bf: forward (0xA0BF kept in CORE_SET_CONFIG and sent to the "
		    "chip; TEST21 behaviour, the PN553 answers status 05)\n");
	} else if (gNfcA0bf == 3) {
		stockholmLog("MiniAppleStockholm: a0bf: 2-byte probe (20 02 06 01 a0 bf 02 00 00; diagnostic "
		    "for H9-a, not for a normal run)\n");
	} else {
		stockholmLog("MiniAppleStockholm: a0bf: local success (default: 20 02 05 01 a0 bf 01 .. gets "
		    "40 02 02 00 00 and never reaches the chip; this is the NFCC CRC check config of "
		    "_NFDriverEnableCRC)\n");
	}
	/*
	 * N9: see the comment at gNfcResetNtf. On by default: it is the only thing between a local
	 * A0BF success and the 2 s "NCI RSP : Timer expired" of the re-init (TEST20B).
	 */
	(void)PE_parse_boot_argn("vayu_nfc_resetntf", &gNfcResetNtf, sizeof(gNfcResetNtf));
	if (gNfcResetNtf != 0) {
		stockholmLog("MiniAppleStockholm: resetntf: on (a synthetic 60 00 05 00 00 10 00 00 follows "
		    "the CORE_RESET_RSP of every re-init; the PN553 never sends one)\n");
	} else {
		stockholmLog("MiniAppleStockholm: resetntf: off (a re-init after A0BF will time out after 2 s)\n");
	}
	(void)PE_parse_boot_argn("vayu_nfc_data", &gNfcData, sizeof(gNfcData));
	if (gNfcData != 0) {
		stockholmLog("MiniAppleStockholm: data: on (data packets on conn ids from "
		    "CORE_CONN_CREATE_RSP are forwarded; 2F 2C stays refused)\n");
	}
	/*
	 * N11: emulate the SPMI CRC16 on the bridge's side of the link. On by default; the nfcd
	 * side only turns it on when it asks for it (IO_STOCKHOLM_CRC=1 after the A0BF answer).
	 * vayu_nfc_crc=0 keeps the old TEST22C behaviour for an A/B run (Step 2 error 0xd).
	 */
	(void)PE_parse_boot_argn("vayu_nfc_crc", &gNfcCrc, sizeof(gNfcCrc));
	if (gNfcCrc != 0) {
		stockholmLog("MiniAppleStockholm: crc: on (CRC-16/CCITT-FALSE, poly 0x1021 init 0xFFFF, "
		    "big-endian; appended to every frame for nfcd and stripped from nfcd's frames once "
		    "IO_STOCKHOLM_CRC=1)\n");
	} else {
		stockholmLog("MiniAppleStockholm: crc: off (vayu_nfc_crc=0; plain NCI in both directions, "
		    "the re-init after A0BF will fail with 0xd)\n");
	}
	if (gNfcCredits != 0 && gNfcData == 0) {
		stockholmLog("MiniAppleStockholm: credits: on but vayu_nfc_data=0: the HCI packet the "
		    "credits unblock would still be dropped\n");
	}
	resources = IOService::getResourceService();
	if (resources == NULL) return;
	/* IOService:/IOResources/AppleStockholmControl and .../AppleStockholmSPMI */
	publishNode(OSTypeAlloc(AppleStockholmControl), resources, "AppleStockholmControl");
	publishNode(OSTypeAlloc(AppleStockholmSPMI), resources, "AppleStockholmSPMI");
	registerSpmiControl();
}
