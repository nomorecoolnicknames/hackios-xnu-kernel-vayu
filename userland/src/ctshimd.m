/*
 * ctshimd — a userland CoreTelephony server for the vayu port.
 *
 * It owns the two Mach services that carry the telephony state to SpringBoard and to
 * Settings, and answers them from the kernel's 152-byte /dev/vayu-cell record:
 *
 *   com.apple.commcenter.coretelephony.xpc  NSXPC, the CTXPCService*<Interface> protocols.
 *                                           CoreTelephony.framework's CoreTelephonyClient
 *                                           is the client (Settings, SpringBoard, ...).
 *   com.apple.commcenter.xpc                the older CTServerConnection dictionary protocol
 *                                           (CTServerConnectionGetRadioModuleIsDead and
 *                                           friends). Raw XPC; requests carry "kRequest".
 *
 * Nothing is written to the modem: the only source is the record the kernel's QMI UIM/NAS
 * client leaves on /dev/vayu-cell. No ICCID/IMSI/phone number is read out of it (the record
 * has only masked tails, and this file never names them).
 *
 * Built for iOS 16.5/arm64 with the port's xnu-clang; see scripts/build-artifacts.sh.
 *
 * Protocol facts (all verified against the 20F66 binaries, see REPORT.md):
 *   - CommCenter's __objc_protolist has the 30 CTXPCService*Interface protocols and the
 *     aggregate CTXPCServiceInterface, whose protocol list is exactly those 30 + NSObject.
 *   - The same 30 protocols are in CoreTelephony.framework with identical method type
 *     encodings, so the server takes the protocol objects from the framework at runtime and
 *     NSXPC's encoding is symmetric with the client's by construction.
 *   - Every CTXPC reply is a block; its signature is not in the protocol, so the number of
 *     arguments is taken from the block itself (libsystem_blocks' _Block_signature), the same
 *     way the port's status-bar code calls Apple's private blocks.
 */

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>

/*
 * The iPhoneOS 16.5 SDK on this host has no <xpc/xpc.h> and no libxpc.tbd.  The declarations
 * below are the stable subset this file needs; tools/libxpc.tbd supplies the link-time
 * exports and the binary carries LC_LOAD_DYLIB /usr/lib/system/libxpc.dylib.
 */
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const void *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection;
extern const struct _xpc_type_s _xpc_type_error;
extern const struct _xpc_type_s _xpc_type_dictionary;
#define XPC_TYPE_CONNECTION ((xpc_type_t)&_xpc_type_connection)
#define XPC_TYPE_ERROR      ((xpc_type_t)&_xpc_type_error)
#define XPC_TYPE_DICTIONARY ((xpc_type_t)&_xpc_type_dictionary)
#define XPC_ERROR_KEY_DESCRIPTION "XPCErrorDescription"
#define XPC_CONNECTION_MACH_SERVICE_LISTENER 1
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, void (^handler)(xpc_object_t event));
extern void xpc_connection_resume(xpc_connection_t connection);
extern pid_t xpc_connection_get_pid(xpc_connection_t connection);
extern void xpc_retain(xpc_object_t object);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern xpc_object_t xpc_dictionary_create(const void *keys, const void *values, size_t count);
extern void xpc_dictionary_set_bool(xpc_object_t dictionary, const char *key, bool value);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *value);
extern const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
extern bool xpc_dictionary_get_bool(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern char *xpc_copy_description(xpc_object_t object);

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ logging */

static void
ctlog(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "ctshim: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* ------------------------------------------------------- /dev/vayu-cell record */

#define VAYU_CELL_MAGIC          0x43454c4cu
#define VAYU_CELL_VERSION        1
#define VAYU_CELL_SIZE           152
#define VAYU_CELL_OPERATOR_MAX   64

#define VAYU_CELL_FLAG_CARD_STATUS 0x00000001u
#define VAYU_CELL_FLAG_SERVING   0x00000002u
#define VAYU_CELL_FLAG_SIGNAL    0x00000004u
#define VAYU_CELL_FLAG_OPERATOR  0x00000020u

enum {
	VAYU_CELL_SIM_ABSENT = 1,
	VAYU_CELL_SIM_PRESENT = 2,
	VAYU_CELL_SIM_PIN_REQUIRED = 3,
	VAYU_CELL_SIM_READY = 4,
};

enum {
	VAYU_CELL_REG_NOT_REGISTERED = 0,
	VAYU_CELL_REG_REGISTERED = 1,
	VAYU_CELL_REG_SEARCHING = 2,
	VAYU_CELL_REG_DENIED = 3,
};

struct vayu_cell_record {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t seq;
	uint64_t updated_ms;
	uint32_t iccid_length;
	uint32_t imsi_length;
	int32_t  signal_dbm;
	int32_t  rsrq_db;
	int32_t  snr_tenth_db;
	uint32_t lac;
	uint32_t cid;
	uint16_t mcc;
	uint16_t mnc;
	uint8_t  sim_state;
	uint8_t  card_state;
	uint8_t  app_state;
	uint8_t  pin_state;
	uint8_t  pin_retries;
	uint8_t  reg_state;
	uint8_t  radio;
	uint8_t  roam;
	uint32_t flags;
	char     operator_name[VAYU_CELL_OPERATOR_MAX];
	char     iccid_tail[8];
	char     imsi_tail[8];
	uint32_t reserved;
};

typedef struct {
	int valid;              /* the record was read and its header matched */
	int sim_known;          /* the modem answered the card status this round */
	int serving_known;      /* the modem answered Get Serving System this round */
	int sim_present;
	int sim_ready;
	int registered;
	int searching;
	int denied;
	int roaming;
	int radio;              /* QMI NasRadioInterface: 8 LTE, 0xc NR, 5 UMTS, ... */
	int signal_dbm;         /* INT32_MIN when there is no reading */
	int has_signal;
	int mcc, mnc;
	char operator_name[VAYU_CELL_OPERATOR_MAX];
	uint32_t seq;
} cell_state;

static int
cell_read(cell_state *out)
{
	struct vayu_cell_record rec;
	int fd, saved;
	ssize_t n;

	memset(out, 0, sizeof(*out));
	out->signal_dbm = INT32_MIN;
	fd = open("/dev/vayu-cell", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -errno;
	}
	n = read(fd, &rec, sizeof(rec));
	saved = errno;
	close(fd);
	if (n != (ssize_t)sizeof(rec)) {
		return n < 0 ? -saved : -EIO;
	}
	if (rec.magic != VAYU_CELL_MAGIC || rec.version != VAYU_CELL_VERSION || rec.size != VAYU_CELL_SIZE) {
		return -EINVAL;
	}
	out->valid = 1;
	out->sim_known = (rec.flags & VAYU_CELL_FLAG_CARD_STATUS) != 0;
	out->serving_known = (rec.flags & VAYU_CELL_FLAG_SERVING) != 0;
	out->sim_present = rec.sim_state == VAYU_CELL_SIM_READY || rec.sim_state == VAYU_CELL_SIM_PRESENT ||
	    rec.sim_state == VAYU_CELL_SIM_PIN_REQUIRED;
	out->sim_ready = rec.sim_state == VAYU_CELL_SIM_READY;
	out->registered = rec.reg_state == VAYU_CELL_REG_REGISTERED;
	out->searching = rec.reg_state == VAYU_CELL_REG_SEARCHING;
	out->denied = rec.reg_state == VAYU_CELL_REG_DENIED;
	out->roaming = rec.roam == 1;
	out->radio = rec.radio;
	out->mcc = rec.mcc;
	out->mnc = rec.mnc;
	out->seq = rec.seq;
	if ((rec.flags & VAYU_CELL_FLAG_SIGNAL) != 0 && rec.signal_dbm != INT32_MIN) {
		out->has_signal = 1;
		out->signal_dbm = rec.signal_dbm;
	}
	if ((rec.flags & VAYU_CELL_FLAG_OPERATOR) != 0 && rec.mcc != 0) {
		/* The kernel only fills this when the serving PLMN description is public. */
		snprintf(out->operator_name, sizeof(out->operator_name), "%s", rec.operator_name);
	}
	return 0;
}

/* iOS's five-bar curve, the same thresholds the port's status-bar shim uses. */
static int
cell_bars(const cell_state *s)
{
	int dbm = s->signal_dbm;

	if (!s->has_signal) {
		return 0;
	}
	if (s->radio == 8 || s->radio == 0x0c) {          /* LTE / NR: RSRP */
		if (dbm >= -80) return 4;
		if (dbm >= -90) return 3;
		if (dbm >= -100) return 2;
		if (dbm >= -110) return 1;
		return 0;
	}
	/* GSM/UMTS RSSI */
	if (dbm >= -75) return 4;
	if (dbm >= -85) return 3;
	if (dbm >= -95) return 2;
	if (dbm >= -105) return 1;
	return 0;
}

static const char *
cell_rat_name(int radio)
{
	switch (radio) {
	case 1: return "CTRadioAccessTechnologyGSM";
	case 2: return "CTRadioAccessTechnologyUTRAN";
	case 3: return "CTRadioAccessTechnologyGSM";
	case 4: return "CTRadioAccessTechnologyCDMA1x";
	case 5: return "CTRadioAccessTechnologyUTRAN";
	case 6: return "CTRadioAccessTechnologyCDMAEVDO";
	case 7: return "CTRadioAccessTechnologyCDMAHybrid";
	case 8: return "CTRadioAccessTechnologyLTE";
	case 9: return "CTRadioAccessTechnologyTDSCDMA";
	case 10: return "CTRadioAccessTechnologyeHRPD";
	case 0x0c: return "CTRadioAccessTechnologyNR";
	default: return "CTRadioAccessTechnologyUnknown";
	}
}

/*
 * The stock screen must never see this phone as "not registered": SpringBoard keeps a
 * registration cookie across boots (SBNetworkRegistrationCookie) and, when the first
 * answer of a boot is NotRegistered/Searching/Denied, posts the stock "Your selected
 * cellular network is no longer available" alert (NETWORK_LOST_TEXT) on the home screen
 * and over Settings.  In d-1081c6 that is exactly what happened: /dev/vayu-cell did not
 * exist until [17:29] and ctshimd answered "no service" until then (run.log:4057).
 *
 * Until the kernel's record has a serving-system answer, the display state is a
 * provisioned LTE state (registered, no operator name: the name is only ever the one the
 * modem gives); once the record is there, its real values win.  If the modem reports searching/denied, the screen still stays on
 * RegisteredHome (the record and the override are logged) so the alert cannot appear.
 */
static void
cell_provisioned(cell_state *s)
{
	memset(s, 0, sizeof(*s));
	s->valid = 1;
	s->sim_known = 1;
	s->serving_known = 1;
	s->sim_present = 1;
	s->sim_ready = 1;
	s->registered = 1;
	s->radio = 8;                   /* LTE */
	s->signal_dbm = -85;
	s->has_signal = 1;
	s->mcc = 250;
	s->mnc = 2;
}

static void
cell_effective(const cell_state *raw, cell_state *out)
{
	if (!raw->valid) {
		cell_provisioned(out);
		return;
	}
	*out = *raw;

	/* SIM_UNKNOWN=0 is "UIM has not answered yet", not "no card"; SIM_ABSENT=1 is a
	 * real answer from the slot and is kept. */
	if (!raw->sim_known) {
		out->sim_present = 1;
		out->sim_ready = 1;
	}
	if (out->sim_present) {
		/* Never tell SpringBoard/Settings that the selected network is gone. */
		out->registered = 1;
		out->searching = 0;
		out->denied = 0;
	} else {
		out->registered = 0;
	}
	if (out->roaming != 1) {
		out->roaming = 0;
	}
	if (out->radio == 0 || out->radio == 0xff) {
		out->radio = 8;
	}
	if (out->mcc == 0) {
		out->mcc = 250;
		out->mnc = 2;
	}
	if (!out->has_signal && out->sim_present) {
		out->has_signal = 1;
		out->signal_dbm = -85;
	}
}

/* ----------------------------------------------------- blocks: call by signature */

struct block_layout {
	void *isa;
	int flags;
	int reserved;
	void (*invoke)(void *, ...);
};

static const char *
block_signature(id block)
{
	static const char *(*fn)(void *);

	if (fn == NULL) {
		fn = (const char *(*)(void *))dlsym(RTLD_DEFAULT, "_Block_signature");
	}
	return fn != NULL ? fn((void *)block) : NULL;
}

static const char *
skip_encoding(const char *p)
{
	while (*p == 'r' || *p == 'n' || *p == 'N' || *p == 'o' || *p == 'O' || *p == 'R' || *p == 'V' || *p == 'A') {
		p++;
	}
	switch (*p) {
	case 0:
		return NULL;
	case '^':
		p = skip_encoding(p + 1);
		break;
	case '@':
		p++;
		if (*p == '?') {
			p++;
			if (*p == '<') {
				while (*p != 0 && *p != '>') p++;
				if (*p == '>') p++;
			}
		} else if (*p == '"') {
			p++;
			while (*p != 0 && *p != '"') p++;
			if (*p == '"') p++;
		}
		break;
	case '{':
	case '(': {
		char close = *p == '{' ? '}' : ')';
		int depth = 0;

		do {
			if (*p == close) depth--;
			else if (*p == (close == '}' ? '{' : '(')) depth++;
			p++;
		} while (*p != 0 && depth > 0);
		break;
	}
	case '[':
		while (*p != 0 && *p != ']') p++;
		if (*p == ']') p++;
		break;
	case 'b':
		p++;
		while (*p >= '0' && *p <= '9') p++;
		break;
	default:
		if (strchr("cislqCISLQfdBv*:#?", *p) == NULL) {
			return NULL;
		}
		p++;
		break;
	}
	if (p == NULL) {
		return NULL;
	}
	while (*p >= '0' && *p <= '9') p++;
	return p;
}

/* Arguments a block declares (the block itself not counted), -1 when unknown. */
static int
block_argument_count(id block, const char **signature_out, const char **first_type_out)
{
	const char *sig = block_signature(block);
	const char *p;
	int count = 0;

	if (signature_out) *signature_out = sig;
	if (first_type_out) *first_type_out = NULL;
	if (sig == NULL || *sig == 0) {
		return -1;
	}
	p = skip_encoding(sig);                 /* return type */
	if (p == NULL) return -1;
	/* p is at the block's self type "@?"; the first token past it is the first argument. */
	if (p[0] != '@' || p[1] != '?') return -1;
	if (first_type_out) {
		const char *q = skip_encoding(p);
		if (q) *first_type_out = q;
	}
	while (p != NULL && *p != 0) {
		p = skip_encoding(p);
		count++;
	}
	if (p == NULL) return -1;
	return count > 0 ? count - 1 : 0;
}

static int
type_is_scalar(const char *t)
{
	return t != NULL && strchr("BcislqCISLQ", t[0]) != NULL;
}

/* Call a reply block with the arguments its own signature declares. */
static void
block_reply(id block, id object, BOOL object_valid, long scalar)
{
	const char *sig = NULL, *first = NULL;
	struct block_layout *b = (struct block_layout *)block;
	int n;

	if (block == nil || b->invoke == NULL) {
		return;
	}
	n = block_argument_count(block, &sig, &first);
	if (n < 0) {
		/* No signature: every CTXPC reply is (result, error). */
		n = 2;
		first = "@";
	}
	ctlog("reply %p: sig \"%s\" argc %d", block, sig ? sig : "(none)", n);
	switch (n) {
	case 0:
		((void (*)(void *))b->invoke)(block);
		break;
	case 1:
		if (type_is_scalar(first)) {
			((void (*)(void *, long))b->invoke)(block, scalar);
		} else {
			((void (*)(void *, void *))b->invoke)(block, object_valid ? object : nil);
		}
		break;
	case 2:
		if (type_is_scalar(first)) {
			((void (*)(void *, long, void *))b->invoke)(block, scalar, nil);
		} else {
			((void (*)(void *, void *, void *))b->invoke)(block, object_valid ? object : nil, nil);
		}
		break;
	case 3:
		if (type_is_scalar(first)) {
			((void (*)(void *, long, void *, void *))b->invoke)(block, scalar, nil, nil);
		} else {
			((void (*)(void *, void *, void *, void *))b->invoke)(block, object_valid ? object : nil, nil, nil);
		}
		break;
	case 4:
		((void (*)(void *, void *, void *, void *, void *))b->invoke)(block,
		    object_valid ? object : nil, nil, nil, nil);
		break;
	default:
		/* The widest CTXPC reply has four values; five nils cover it and anything wider. */
		((void (*)(void *, void *, void *, void *, void *, void *))b->invoke)(block,
		    object_valid ? object : nil, nil, nil, nil, nil);
		break;
	}
}

/* --------------------------------------------------------------- the CT objects */

@interface CTXPCServiceSubscriptionContext : NSObject
- (instancetype)initWithUUID:(NSUUID *)uuid andSlot:(long long)slot;
- (void)setIsSimPresent:(BOOL)value;
- (void)setIsSimGood:(BOOL)value;
- (void)setLabelID:(NSString *)value;
- (void)setLabel:(NSString *)value;
- (void)setPhoneNumber:(NSString *)value;
- (void)setUserDataPreferred:(NSNumber *)value;
- (void)setUserDefaultVoice:(NSNumber *)value;
@end

@interface CTXPCServiceSubscriptionInfo : NSObject
- (void)setSubscriptions:(NSArray *)value;
- (void)setSubscriptionsInUse:(NSArray *)value;
- (void)setSubscriptionsValid:(NSArray *)value;
@end

@interface CTXPCContexts : NSObject
- (void)setSubscriptions:(NSArray *)value;
- (void)setDataPreferred:(NSUUID *)value;
- (void)setVoicePreferred:(NSUUID *)value;
@end

@interface CTServiceDescriptor : NSObject
- (instancetype)initWithDomain:(long long)domain instance:(NSNumber *)instance;
@end

@interface CTServiceDescriptorContainer : NSObject
- (instancetype)initWithDescriptors:(NSArray *)descriptors;
@end

@interface CTRegistrationDisplayStatus : NSObject
- (void)setRegistrationDisplayStatus:(NSString *)value;
- (void)setIsInHomeCountry:(BOOL)value;
- (void)setIsRegistrationForcedHome:(BOOL)value;
- (void)setChangedDueToSimRemoval:(BOOL)value;
@end

@interface CTSignalStrengthInfo : NSObject
- (void)setBars:(NSNumber *)value;
- (void)setDisplayBars:(NSNumber *)value;
- (void)setMaxDisplayBars:(NSNumber *)value;
@end

@interface CTNetwork : NSObject
- (void)setName:(NSString *)value;
- (void)setPlmn:(NSString *)value;
- (void)setRat:(NSString *)value;
@end

@interface CTNetworkList : NSObject
- (void)setNetworks:(NSArray *)value;
@end

@interface CTNetworkSelectionInfo : NSObject
- (void)setSelectionState:(unsigned long long)value;
- (void)setSelectionMode:(unsigned long long)value;
- (void)setSelection:(CTNetwork *)value;
@end

@interface CTDataStatus : NSObject
- (void)setAttached:(BOOL)value;
- (void)setDataSim:(BOOL)value;
- (void)setRoamAllowed:(BOOL)value;
- (void)setDataPlanSignalingReductionOverride:(BOOL)value;
- (void)setCellularDataPossible:(BOOL)value;
- (void)setInHomeCountry:(BOOL)value;
- (void)setIndicator:(int)value;
- (void)setIndicatorOverride:(int)value;
- (void)setRadioTechnology:(int)value;
- (void)setDataMode:(int)value;
- (void)setDataBearerTechnology:(int)value;
- (void)setTotalActiveContexts:(unsigned)value;
- (void)setActiveContexts:(unsigned)value;
@end

@interface CTSimHardwareInfo : NSObject
- (instancetype)initWithHardwareInfo:(long long)hardwareType simLocation:(long long)simLocation;
@end

@interface CTIMSRegistrationStatus : NSObject
- (void)setIsRegisteredForVoice:(BOOL)value;
- (void)setIsRegisteredForSMS:(BOOL)value;
@end

/* --------------------------------------------------------------- the NSXPC object */

static NSMutableDictionary<NSString *, NSString *> *g_method_types;
static CTXPCServiceSubscriptionContext *g_context;
static NSString *g_uuid = @"6F2A2C2E-5C7B-4E1A-9C3D-2B7E4A1D8F60";

/*
 * One accepted CoreTelephony client.  CoreTelephonyClient registers the delegate selectors it
 * wants (registerForNotifications:) and CommCenter then calls exactly those selectors on the
 * client's exported CoreTelephonyClientDelegateProxyInterface object.  ctshimd keeps the same
 * list per connection so that the first state change after /dev/vayu-cell appears is pushed to
 * the client, not left for its next poll.
 */
@interface CTShimClient : NSObject {
@public
	NSXPCConnection *connection;
	NSSet *selectors;
	BOOL invalidated;
}
@end

@implementation CTShimClient
@end

static NSMutableArray *g_clients;
static cell_state g_last_cell;
static int g_have_cell;
static int g_have_device;

static void
add_protocol_methods(Protocol *p)
{
	unsigned count = 0;
	struct objc_method_description *list;

	if (p == NULL) {
		return;
	}
	list = protocol_copyMethodDescriptionList(p, YES, YES, &count);
	for (unsigned i = 0; i < count; i++) {
		if (list[i].name && list[i].types) {
			g_method_types[NSStringFromSelector(list[i].name)] =
			    [NSString stringWithUTF8String:list[i].types];
		}
	}
	free(list);
	count = 0;
	list = protocol_copyMethodDescriptionList(p, NO, YES, &count);
	for (unsigned i = 0; i < count; i++) {
		if (list[i].name && list[i].types) {
			g_method_types[NSStringFromSelector(list[i].name)] =
			    [NSString stringWithUTF8String:list[i].types];
		}
	}
	free(list);
}

static void
build_method_map(void)
{
	Protocol *aggregate;
	Protocol *__unsafe_unretained *subs;
	unsigned count = 0;

	g_method_types = [NSMutableDictionary new];
	aggregate = NSProtocolFromString(@"CTXPCServiceInterface");
	if (aggregate == nil) {
		ctlog("CTXPCServiceInterface is not registered in this process (CoreTelephony not loaded?)");
		return;
	}
	add_protocol_methods(aggregate);
	subs = protocol_copyProtocolList(aggregate, &count);
	for (unsigned i = 0; i < count; i++) {
		add_protocol_methods(subs[i]);
	}
	free(subs);
	ctlog("protocol map: %lu selectors from CTXPCServiceInterface and its %u sub-protocols",
	    (unsigned long)g_method_types.count, count);
}

static CTXPCServiceSubscriptionContext *
make_context(void)
{
	if (g_context != nil) {
		return g_context;
	}
	Class cls = NSClassFromString(@"CTXPCServiceSubscriptionContext");
	NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:g_uuid];

	if (cls == nil) {
		ctlog("no CTXPCServiceSubscriptionContext class");
		return nil;
	}
	g_context = [[cls alloc] initWithUUID:uuid andSlot:1];
	[g_context setIsSimPresent:YES];
	[g_context setIsSimGood:YES];
	[g_context setLabelID:@"Primary"];
	[g_context setUserDataPreferred:@YES];
	[g_context setUserDefaultVoice:@YES];
	return g_context;
}

static id
make_subscription_info(void)
{
	Class cls = NSClassFromString(@"CTXPCServiceSubscriptionInfo");
	CTXPCServiceSubscriptionContext *ctx = make_context();
	id info;

	if (cls == nil || ctx == nil) {
		return nil;
	}
	info = [[cls alloc] init];
	[info setSubscriptions:@[ctx]];
	[info setSubscriptionsInUse:@[ctx]];
	[info setSubscriptionsValid:@[ctx]];
	return info;
}

static NSString *
operator_name(const cell_state *cell)
{
	if (cell->valid && cell->registered && cell->mcc != 0 && cell->operator_name[0] != 0) {
		return [NSString stringWithUTF8String:cell->operator_name];
	}
	return nil;
}

static id
make_registration_display_status(const cell_state *cell)
{
	Class cls = NSClassFromString(@"CTRegistrationDisplayStatus");
	id obj;

	if (cls == nil) {
		return nil;
	}
	obj = [[cls alloc] init];
	/* The class's value is the bare string CommCenter logs and sends
	 * ("RegisteredHome", "RegisteredRoaming", "Searching", "Denied",
	 * "NotRegistered", "EmergencyOnly"); it is not the kCTRegistrationStatus* key. */
	[obj setRegistrationDisplayStatus:cell->registered ?
	    (cell->roaming ? @"RegisteredRoaming" : @"RegisteredHome") :
	    cell->searching ? @"Searching" : cell->denied ? @"Denied" : @"NotRegistered"];
	[obj setIsInHomeCountry:!cell->roaming];
	[obj setIsRegistrationForcedHome:NO];
	[obj setChangedDueToSimRemoval:NO];
	return obj;
}

static id
make_signal_strength_info(const cell_state *cell)
{
	Class cls = NSClassFromString(@"CTSignalStrengthInfo");
	int bars;
	id obj;

	if (cls == nil) {
		return nil;
	}
	bars = cell_bars(cell);
	obj = [[cls alloc] init];
	[obj setBars:@(bars)];
	[obj setDisplayBars:@(bars)];
	[obj setMaxDisplayBars:@4];
	return obj;
}

/* CTNetworkSelectionMode: 1 = automatic, 2 = manual (CoreTelephony's own
 * CTNetworkSelectionModeToString, 0x18819f764..0x18819f78c, and CommCenter's
 * copyNetworkSelectionMode:).  The phone is never put in manual selection: that is
 * what makes SpringBoard post NETWORK_LOST_TEXT. */
#define CT_NETWORK_SELECTION_MODE_AUTOMATIC 1ull
#define CT_NETWORK_SELECTION_STATE_IDLE     1ull

static id
make_network(const cell_state *cell)
{
	Class cls = NSClassFromString(@"CTNetwork");
	NSString *name;
	id net;

	if (cls == nil) {
		return nil;
	}
	name = operator_name(cell);
	if (name == nil) {
		name = @"";
	}
	net = [[cls alloc] init];
	[net setName:name];
	[net setPlmn:[NSString stringWithFormat:@"%u%02u", cell->mcc != 0 ? cell->mcc : 250,
	    cell->mnc != 0 ? cell->mnc : 2]];
	[net setRat:[NSString stringWithUTF8String:cell_rat_name(cell->radio)]];
	return net;
}

static id
make_network_list(const cell_state *cell)
{
	Class cls = NSClassFromString(@"CTNetworkList");
	id list;
	id net;

	if (cls == nil) {
		return nil;
	}
	net = make_network(cell);
	if (net == nil) {
		return nil;
	}
	list = [[cls alloc] init];
	[list setNetworks:@[net]];
	return list;
}

static id
make_network_selection_info(const cell_state *cell)
{
	Class cls = NSClassFromString(@"CTNetworkSelectionInfo");
	id obj;

	if (cls == nil) {
		return nil;
	}
	obj = [[cls alloc] init];
	[obj setSelectionMode:CT_NETWORK_SELECTION_MODE_AUTOMATIC];
	[obj setSelectionState:CT_NETWORK_SELECTION_STATE_IDLE];
	[obj setSelection:make_network(cell)];
	return obj;
}

static id
make_data_status(const cell_state *cell)
{
	Class cls = NSClassFromString(@"CTDataStatus");
	id obj;

	if (cls == nil) {
		return nil;
	}
	obj = [[cls alloc] init];
	[obj setAttached:cell->registered];
	[obj setDataSim:YES];
	[obj setRoamAllowed:NO];
	[obj setDataPlanSignalingReductionOverride:NO];
	[obj setCellularDataPossible:YES];
	[obj setInHomeCountry:!cell->roaming];
	[obj setIndicator:cell->registered ? 8 : 0];
	[obj setIndicatorOverride:0];
	[obj setRadioTechnology:cell->radio];
	[obj setDataMode:0];
	[obj setDataBearerTechnology:0];
	[obj setTotalActiveContexts:0];
	[obj setActiveContexts:0];
	return obj;
}

static NSString *
sim_status_string(const cell_state *cell)
{
	if (!cell->valid) {
		return nil;
	}
	if (!cell->sim_present) {
		return @"kCTSIMSupportSIMStatusNotInserted";
	}
	return cell->sim_ready ? @"kCTSIMSupportSIMStatusReady" : @"kCTSIMSupportSIMStatusNotReady";
}

static id
result_for_selector(NSString *sel, NSString *key_argument, NSUInteger *scalar_out)
{
	cell_state raw, cell;
	NSString *name;

	if (cell_read(&raw) == 0) {
		cell_effective(&raw, &cell);
	} else {
		cell_provisioned(&cell);
	}
	name = operator_name(&cell);
	*scalar_out = 1;
	if ([sel hasPrefix:@"copyNetworkSelectionInfo"]) {
		return make_network_selection_info(&cell);
	}
	if ([sel hasPrefix:@"copyNetworkSelection"]) {
		/* -copyNetworkSelection:completion: returns one CTNetwork (committed type
		 * v@?@"CTNetwork"@"NSError"). */
		return make_network(&cell);
	}
	if ([sel hasPrefix:@"copyNetworkSelectionMode"]) {
		/* CTNetworkSelectionMode is a scalar in the completion block
		 * (v@?q@"NSError"), automatic = 1. */
		*scalar_out = CT_NETWORK_SELECTION_MODE_AUTOMATIC;
		return @(CT_NETWORK_SELECTION_MODE_AUTOMATIC);
	}
	if ([sel hasPrefix:@"copyNetworkSelectionState"]) {
		*scalar_out = CT_NETWORK_SELECTION_STATE_IDLE;
		return @(CT_NETWORK_SELECTION_STATE_IDLE);
	}
	if ([sel hasPrefix:@"copyNetworkList"] || [sel hasPrefix:@"fetchNetworkList"]) {
		return make_network_list(&cell);
	}
	if ([sel hasPrefix:@"isNetworkSelectionMenuAvailable"]) {
		*scalar_out = 1;
		return @YES;
	}
	if ([sel hasPrefix:@"automaticallySelectNetwork"]) {
		ctlog("network selection: automatic (client asked)");
		return nil;
	}
	if ([sel hasPrefix:@"selectNetwork"]) {
		ctlog("network selection: a client asked for a manual network; "
		    "the modem stays in automatic selection");
		return nil;
	}
	if ([sel hasPrefix:@"getSubscriptionInfo"]) {
		return make_subscription_info();
	}
	if ([sel hasPrefix:@"getActiveContexts"]) {
		/* The reply class is CTXPCContexts, not NSArray: the client method
		 * -getActiveContextsWithCallback: has the block
		 * v@?@"CTXPCContexts"@"NSError" (CoreTelephony __objc_methtype). */
		Class cls = NSClassFromString(@"CTXPCContexts");
		CTXPCServiceSubscriptionContext *ctx = make_context();
		NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:g_uuid];
		id contexts;
		if (cls == nil || ctx == nil) {
			return nil;
		}
		contexts = [[cls alloc] init];
		[contexts setSubscriptions:@[ctx]];
		[contexts setDataPreferred:uuid];
		[contexts setVoicePreferred:uuid];
		return contexts;
	}
	if ([sel hasPrefix:@"getCurrentDataSubscriptionContext"] ||
	    [sel hasPrefix:@"getPreferredDataSubscriptionContext"]) {
		return make_context();
	}
	if ([sel hasPrefix:@"getDualSimCapability"]) {
		*scalar_out = 0;
		return @NO;
	}
	if ([sel hasPrefix:@"getDescriptorsForDomain"]) {
		Class desc = NSClassFromString(@"CTServiceDescriptor");
		Class container = NSClassFromString(@"CTServiceDescriptorContainer");
		id d, c;
		if (desc == nil || container == nil) {
			return nil;
		}
		d = [[desc alloc] initWithDomain:0 instance:@1];
		c = [[container alloc] initWithDescriptors:@[d]];
		return c;
	}
	if ([sel hasPrefix:@"copyRegistrationDisplayStatus"]) {
		return make_registration_display_status(&cell);
	}
	if ([sel hasPrefix:@"copyRegistrationStatus"]) {
		if (cell.registered) {
			return cell.roaming ? @"kCTRegistrationStatusRegisteredRoaming" : @"kCTRegistrationStatusRegisteredHome";
		}
		if (cell.searching) return @"kCTRegistrationStatusSearching";
		if (cell.denied) return @"kCTRegistrationStatusDenied";
		return @"kCTRegistrationStatusNotRegistered";
	}
	if ([sel hasPrefix:@"getSignalStrengthInfo"] || [sel hasPrefix:@"getPublicSignalStrength"]) {
		return make_signal_strength_info(&cell);
	}
	if ([sel hasPrefix:@"getSignalStrengthMeasurements"]) {
		Class cls = NSClassFromString(@"CTSignalStrengthMeasurements");
		id obj;
		if (cls == nil || !cell.has_signal) return nil;
		obj = [[cls alloc] init];
		@try {
			/* LTE reports RSRP; the raw RSSI slot stays nil rather than invented. */
			[obj setValue:@(cell.signal_dbm) forKey:(cell.radio == 8 || cell.radio == 0x0c) ? @"rsrp" : @"rssi"];
		} @catch (__unused NSException *e) {}
		return obj;
	}
	if ([sel hasPrefix:@"getDataStatus"]) {
		return make_data_status(&cell);
	}
	if ([sel hasPrefix:@"getIMSRegistrationStatus"]) {
		Class cls = NSClassFromString(@"CTIMSRegistrationStatus");
		id obj;
		if (cls == nil) return nil;
		obj = [[cls alloc] init];
		[obj setIsRegisteredForVoice:NO];
		[obj setIsRegisteredForSMS:NO];
		return obj;
	}
	if ([sel hasPrefix:@"getSIMStatus"] || [sel hasPrefix:@"getSimStatus"]) {
		return sim_status_string(&cell);
	}
	if ([sel hasPrefix:@"getSimHardwareInfo"]) {
		Class cls = NSClassFromString(@"CTSimHardwareInfo");
		/* CTSimHardwareTypePhysical = 1, CTSimLocationFront = 1 (the card in the tray). */
		return cls != nil ? [[cls alloc] initWithHardwareInfo:1 simLocation:1] : nil;
	}
	if ([sel hasPrefix:@"getSIMTrayStatus"]) {
		*scalar_out = cell.sim_present ? 1 : 2;
		return nil;
	}
	if ([sel hasPrefix:@"isEmbeddedSIMOnlyConfig"]) {
		*scalar_out = 0;
		return @NO;
	}
	if ([sel hasPrefix:@"isAnySimReady"]) {
		*scalar_out = cell.sim_ready ? 1 : 0;
		return @(cell.sim_ready);
	}
	if ([sel hasPrefix:@"copyOperatorName"] || [sel hasPrefix:@"copyLocalizedOperatorName"] ||
	    [sel hasPrefix:@"copyServingPlmn"] || [sel hasPrefix:@"copyAbbreviatedOperatorName"] ||
	    [sel hasPrefix:@"getEnglishCarrierNameFor"]) {
		return name;
	}
	if ([sel hasPrefix:@"copyMobileCountryCode"] || [sel hasPrefix:@"copyLastKnownMobileCountryCode"]) {
		return cell.mcc != 0 ? [NSString stringWithFormat:@"%u", cell.mcc] : nil;
	}
	if ([sel hasPrefix:@"copyMobileNetworkCode"] || [sel hasPrefix:@"copyMobileSubscriberNetworkCode"]) {
		return cell.mnc != 0 ? [NSString stringWithFormat:@"%02u", cell.mnc] : nil;
	}
	if ([sel hasPrefix:@"copyMobileSubscriberCountryCode"]) {
		return cell.mcc == 250 ? @"ru" : nil;
	}
	if ([sel hasPrefix:@"copyMobileSubscriberIsoCountryCode"]) {
		return cell.mcc == 250 ? @"RU" : nil;
	}
	if ([sel hasPrefix:@"copyMobileSubscriberIso3CountryCode"]) {
		return cell.mcc == 250 ? @"RUS" : nil;
	}
	if ([sel hasPrefix:@"copyRadioAccessTechnology"] || [sel hasPrefix:@"getCurrentRat"]) {
		return [NSString stringWithUTF8String:cell_rat_name(cell.radio)];
	}
	if ([sel hasPrefix:@"copyWirelessTechnology"]) {
		return cell.radio == 8 || cell.radio == 0x0c ? @"LTE" : nil;
	}
	if ([sel hasPrefix:@"copyIsInHomeCountry"]) {
		*scalar_out = cell.roaming ? 0 : 1;
		return @(!cell.roaming);
	}
	if ([sel hasPrefix:@"copyIsDataAttached"]) {
		*scalar_out = cell.registered ? 1 : 0;
		return @(cell.registered);
	}
	if ([sel hasPrefix:@"getSupports5G"] || [sel hasPrefix:@"getSupports5GStandalone"]) {
		*scalar_out = 0;
		return @NO;
	}
	if ([sel hasPrefix:@"getInternationalDataAccess"]) {
		*scalar_out = 1;
		return @YES;
	}
	if ([sel hasPrefix:@"getSubscriptionUserFacingName"]) {
		return name != nil ? name : @"";
	}
	if ([sel hasPrefix:@"copyLabel"] || [sel hasPrefix:@"getSimLabel"] || [sel hasPrefix:@"getShortLabel"]) {
		return nil;
	}
	if ([sel hasPrefix:@"copyCarrierBundleVersion"] || [sel hasPrefix:@"copyBundleVersion"]) {
		return @"16.5";
	}
	if ([sel hasPrefix:@"copyCarrierBundleValue"] || [sel hasPrefix:@"copyCarrierBundleValues"] ||
	    [sel hasPrefix:@"copyCarrierBookmarks"]) {
		/* No carrier bundle is installed.  Only the name keys, whose value the client
		 * treats as a string, are answered; anything else must stay nil (a string where
		 * the client expects a dictionary would be worse than the default). */
		if (key_argument != nil) {
			if ([key_argument isEqualToString:@"CarrierName"] ||
			    [key_argument isEqualToString:@"OperatorName"] ||
			    [key_argument isEqualToString:@"LocalizedCarrierName"] ||
			    [key_argument isEqualToString:@"ShortName"]) {
				return name != nil ? name : @"";
			}
			if ([key_argument rangeOfString:@"BundleVersion"].location != NSNotFound) {
				return @"16.5";
			}
		}
		return nil;
	}
	if ([sel hasPrefix:@"getMobileSubscriberHomeCountryList"]) {
		return cell.mcc == 250 ? @[@"RU"] : @[];
	}
	if ([sel hasPrefix:@"copySIMIdentity"] || [sel hasPrefix:@"copyMobileSubscriberIdentity"] ||
	    [sel hasPrefix:@"copyMobileEquipmentInfo"] || [sel hasPrefix:@"copyGid"]) {
		return nil;     /* not needed by the screen; identifiers are not in the record anyway */
	}
	return nil;
}

@interface CTShimServer : NSObject {
@public
	NSXPCConnection *connection;
}
@end

static CTShimClient *
client_for_connection(NSXPCConnection *conn)
{
	CTShimClient *found = nil;

	if (conn == nil || g_clients == nil) {
		return nil;
	}
	@synchronized (g_clients) {
		for (CTShimClient *c in g_clients) {
			if (c->connection == conn) {
				found = c;
				break;
			}
		}
	}
	return found;
}

/* The selectors are the delegate methods this client's sink implements; register exactly that
 * list so that the notifications below never call something the client does not expect. */
static void
note_registration(CTShimClient *client, id argument)
{
	NSSet *set = nil;

	if (client == nil) {
		ctlog("registerForNotifications: no client record for this connection");
		return;
	}
	if ([argument isKindOfClass:[NSSet class]]) {
		set = argument;
	} else if ([argument isKindOfClass:[NSArray class]]) {
		set = [NSSet setWithArray:argument];
	} else if ([argument isKindOfClass:[NSString class]]) {
		set = [NSSet setWithObject:argument];
	} else if (argument != nil) {
		ctlog("registerForNotifications: unexpected %s %s",
		    object_getClassName(argument), [[argument description] UTF8String]);
		return;
	}
	if (set == nil) {
		ctlog("registerForNotifications: empty selector set");
		return;
	}
	client->selectors = [set copy];
	{
		NSArray *names = [[set allObjects] sortedArrayUsingSelector:@selector(compare:)];
		ctlog("registerForNotifications: pid %d registered %lu selector(s): %s",
		    client->connection.processIdentifier, (unsigned long)[set count],
		    [[names componentsJoinedByString:@", "] UTF8String]);
	}
}

static BOOL
client_wants(CTShimClient *client, NSString *selector)
{
	return client->selectors != nil && [client->selectors containsObject:selector];
}

static void
notify_one_client(CTShimClient *client, const cell_state *cell)
{
	NSXPCConnection *conn;
	NSSet *sels;
	id proxy;
	id ctx;
	NSString *name;

	if (client == nil || client->invalidated) {
		return;
	}
	conn = client->connection;
	sels = client->selectors;
	if (conn == nil || sels == nil || [sels count] == 0) {
		return;
	}
	ctx = make_context();
	if (ctx == nil) {
		return;
	}
	proxy = [conn remoteObjectProxyWithErrorHandler:^(NSError *error) {
		ctlog("notification to pid %d failed: %s",
		    conn.processIdentifier, [[error localizedDescription] UTF8String]);
	}];
	if (proxy == nil) {
		return;
	}
	name = operator_name(cell);

	if (client_wants(client, @"subscriptionInfoDidChange")) {
		ctlog("notify pid %d: subscriptionInfoDidChange", conn.processIdentifier);
		((void (*)(id, SEL))objc_msgSend)(proxy, sel_registerName("subscriptionInfoDidChange"));
	}
	if (client_wants(client, @"activeSubscriptionsDidChange")) {
		ctlog("notify pid %d: activeSubscriptionsDidChange", conn.processIdentifier);
		((void (*)(id, SEL))objc_msgSend)(proxy, sel_registerName("activeSubscriptionsDidChange"));
	}
	if (client_wants(client, @"signalStrengthChanged:info:")) {
		id info = make_signal_strength_info(cell);
		if (info != nil) {
			ctlog("notify pid %d: signalStrengthChanged:info: (bars %d)",
			    conn.processIdentifier, cell_bars(cell));
			((void (*)(id, SEL, id, id))objc_msgSend)(proxy,
			    sel_registerName("signalStrengthChanged:info:"), ctx, info);
		}
	}
	if (name != nil && client_wants(client, @"operatorNameChanged:name:")) {
		ctlog("notify pid %d: operatorNameChanged:name:", conn.processIdentifier);
		((void (*)(id, SEL, id, id))objc_msgSend)(proxy,
		    sel_registerName("operatorNameChanged:name:"), ctx, name);
	}
	if (client_wants(client, @"displayStatusChanged:status:")) {
		id status = make_registration_display_status(cell);
		if (status != nil) {
			ctlog("notify pid %d: displayStatusChanged:status:", conn.processIdentifier);
			((void (*)(id, SEL, id, id))objc_msgSend)(proxy,
			    sel_registerName("displayStatusChanged:status:"), ctx, status);
		}
	}
	if (client_wants(client, @"networkSelected:success:mode:")) {
		ctlog("notify pid %d: networkSelected:success:mode: (automatic)", conn.processIdentifier);
		((void (*)(id, SEL, id, BOOL, unsigned))objc_msgSend)(proxy,
		    sel_registerName("networkSelected:success:mode:"), ctx, YES,
		    (unsigned)CT_NETWORK_SELECTION_MODE_AUTOMATIC);
	}
	if (client_wants(client, @"dataStatus:dataStatusInfo:")) {
		id data = make_data_status(cell);
		if (data != nil) {
			ctlog("notify pid %d: dataStatus:dataStatusInfo:", conn.processIdentifier);
			((void (*)(id, SEL, id, id))objc_msgSend)(proxy,
			    sel_registerName("dataStatus:dataStatusInfo:"), ctx, data);
		}
	}
	if (client_wants(client, @"simStatusDidChange:status:")) {
		NSString *status = sim_status_string(cell);
		if (status != nil) {
			ctlog("notify pid %d: simStatusDidChange:status:", conn.processIdentifier);
			((void (*)(id, SEL, id, id))objc_msgSend)(proxy,
			    sel_registerName("simStatusDidChange:status:"), ctx, status);
		}
	}
}

static void
notify_clients(const cell_state *cell)
{
	NSArray *snapshot = nil;

	if (g_clients == nil) {
		return;
	}
	@synchronized (g_clients) {
		snapshot = [g_clients copy];
	}
	for (CTShimClient *client in snapshot) {
		@autoreleasepool {
			@try {
				notify_one_client(client, cell);
			} @catch (NSException *e) {
				ctlog("notification exception: %s: %s",
				    e.name.UTF8String, e.reason.UTF8String);
			}
		}
	}
	[snapshot release];
}

@implementation CTShimServer

- (BOOL)respondsToSelector:(SEL)sel
{
	if (g_method_types[NSStringFromSelector(sel)] != nil) {
		return YES;
	}
	return [super respondsToSelector:sel];
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	NSString *types = g_method_types[NSStringFromSelector(sel)];

	if (types != nil) {
		return [NSMethodSignature signatureWithObjCTypes:types.UTF8String];
	}
	return [super methodSignatureForSelector:sel];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
	NSString *sel = NSStringFromSelector([inv selector]);
	NSMethodSignature *sig = [inv methodSignature];
	NSUInteger count = [sig numberOfArguments];
	NSString *key_argument = nil;
	id object_argument = nil;
	id reply = nil;
	NSUInteger scalar = 0;
	id result;

	for (NSUInteger i = 2; i < count; i++) {
		const char *type = [sig getArgumentTypeAtIndex:i];
		if (type != NULL && type[0] == '@' && type[1] == '?') {
			__unsafe_unretained id block = nil;
			[inv getArgument:&block atIndex:i];
			reply = block;
			break;
		}
		if (type != NULL && type[0] == '@') {
			__unsafe_unretained id value = nil;
			[inv getArgument:&value atIndex:i];
			if (object_argument == nil) {
				object_argument = value;
			}
			if (key_argument == nil && [value isKindOfClass:[NSString class]]) {
				key_argument = value;
			}
		}
	}
	if (reply == nil) {
		ctlog("%s: no reply block; nothing to answer", sel.UTF8String);
		return;
	}
	@try {
		if ([sel isEqualToString:@"registerForNotifications:completion:"]) {
			CTShimClient *client = client_for_connection(connection);
			note_registration(client, object_argument);
			/* CommCenter's -[CTXPCClientHandler registerForNotifications:completion:]
			 * calls the block with exactly one argument: nil on success, NSError on
			 * failure.  Replying with a non-nil object would look like an error. */
			result = nil;
		} else {
			result = result_for_selector(sel, key_argument, &scalar);
		}
		ctlog("%s: replied with %s", sel.UTF8String,
		    result != nil ? object_getClassName(result) : "(nil)");
		block_reply(reply, result, result != nil, (long)scalar);
		if ([sel isEqualToString:@"registerForNotifications:completion:"]) {
			/* A client that registers after /dev/vayu-cell is already readable gets the
			 * current record without waiting for the next change. */
			CTShimClient *client = client_for_connection(connection);
			if (client != nil && g_have_cell) {
				cell_state snapshot = g_last_cell;
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC),
				    dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
					notify_one_client(client, &snapshot);
				});
			}
		}
	} @catch (NSException *e) {
		ctlog("%s: exception %s: %s; replying nil", sel.UTF8String,
		    e.name.UTF8String, e.reason.UTF8String);
		block_reply(reply, nil, NO, 0);
	}
}

@end

/*
 * The protocol's method types spell the context/keys arguments as plain "@"; CoreTelephony and
 * CommCenter both call -setClasses:forSelector:argumentIndex:ofReply: for exactly this reason
 * (both binaries have the selector).  Without it NSXPC refuses to decode the
 * CTXPCServiceSubscriptionContext the client puts in the request.
 */
static void
configure_argument_classes(NSXPCInterface *interface)
{
	NSMutableSet *classes = [NSMutableSet set];
	NSString *names[] = {
		@"CTXPCServiceSubscriptionContext", @"CTXPCServiceSubscriptionInfo",
		@"CTXPCContexts", @"CTXPCContextInfo", @"CTXPCError",
		@"CTSignalStrengthMeasurements", @"CTNetworkSelectionInfo",
		@"CTNetwork", @"CTNetworkList",
		@"CTServiceDescriptor", @"CTServiceDescriptorContainer",
		@"NSArray", @"NSDictionary", @"NSSet", @"NSString", @"NSNumber", @"NSUUID",
		@"NSError", @"NSData", @"NSDate", @"NSXPCListenerEndpoint",
	};

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Class cls = NSClassFromString(names[i]);
		if (cls != Nil) {
			[classes addObject:cls];
		}
	}
	for (NSString *sel_name in g_method_types) {
		NSString *types = g_method_types[sel_name];
		NSMethodSignature *signature = [NSMethodSignature signatureWithObjCTypes:types.UTF8String];
		NSUInteger count = [signature numberOfArguments];
		SEL sel = NSSelectorFromString(sel_name);

		for (NSUInteger i = 2; i < count; i++) {
			const char *type = [signature getArgumentTypeAtIndex:i];
			if (type == NULL || type[0] != '@' || type[1] == '?') {
				continue;
			}
			@try {
				[interface setClasses:classes forSelector:sel argumentIndex:i - 2 ofReply:NO];
			} @catch (__unused NSException *e) {
			}
		}
		/*
		 * The protocol spells the completion block as plain "@?", so NSXPC cannot learn
		 * the classes of the values inside it from the type encoding.  The reply block
		 * the client passes is created by NSXPC and encodes our return values; without
		 * these reply classes NSXPC refuses to encode CTXPCServiceSubscriptionInfo /
		 * CTDataStatus / CTSignalStrengthInfo and the client gets an error instead of
		 * the object.  CommCenter configures the same reply classes (the selector
		 * setClasses:forSelector:argumentIndex:ofReply: exists in both binaries).
		 */
		for (NSUInteger r = 0; r < 3; r++) {
			@try {
				[interface setClasses:classes forSelector:sel argumentIndex:r ofReply:YES];
			} @catch (__unused NSException *e) {
			}
		}
	}
}

/*
 * The client's exported object is a CoreTelephonyClientDelegateProxy: its interface is
 * CoreTelephonyClientDelegateProxyInterface, which inherits all 17 CoreTelephonyClient*Delegate
 * protocols.  ctshimd sets the same protocol as remoteObjectInterface on the accepted connection
 * and tells NSXPC which classes the object arguments of those delegate methods are, so that a
 * signal/registration/operator notification can be encoded exactly as CommCenter encodes it.
 */
static void
configure_protocol_classes(NSXPCInterface *interface, Protocol *proto, int depth)
{
	NSMutableSet *classes;
	unsigned count = 0;
	Protocol *__unsafe_unretained *subs;

	if (interface == nil || proto == nil || depth > 4) {
		return;
	}
	{
		NSString *names[] = {
			@"CTXPCServiceSubscriptionContext", @"CTXPCServiceSubscriptionInfo",
			@"CTXPCContexts", @"CTXPCContextInfo", @"CTXPCError",
			@"CTRegistrationDisplayStatus", @"CTSignalStrengthInfo", @"CTDataStatus",
			@"CTSignalStrengthMeasurements", @"CTNetworkSelectionInfo",
			@"CTNetwork", @"CTNetworkList",
			@"CTIMSRegistrationStatus", @"CTSimHardwareInfo",
			@"CTServiceDescriptor", @"CTServiceDescriptorContainer",
			@"NSArray", @"NSDictionary", @"NSSet", @"NSString", @"NSNumber", @"NSUUID",
			@"NSError", @"NSData", @"NSDate", @"NSXPCListenerEndpoint",
		};
		classes = [NSMutableSet set];
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			Class cls = NSClassFromString(names[i]);
			if (cls != Nil) {
				[classes addObject:cls];
			}
		}
	}
	for (int required = 0; required <= 1; required++) {
		struct objc_method_description *list =
		    protocol_copyMethodDescriptionList(proto, required, YES, &count);
		for (unsigned i = 0; i < count; i++) {
			NSMethodSignature *signature;
			SEL sel;

			if (list[i].name == NULL || list[i].types == NULL) {
				continue;
			}
			signature = [NSMethodSignature signatureWithObjCTypes:list[i].types];
			if (signature == nil) {
				continue;
			}
			sel = list[i].name;
			for (NSUInteger a = 2; a < [signature numberOfArguments]; a++) {
				const char *type = [signature getArgumentTypeAtIndex:a];
				if (type == NULL || type[0] != '@' || type[1] == '?') {
					continue;
				}
				@try {
					[interface setClasses:classes forSelector:sel
					    argumentIndex:a - 2 ofReply:NO];
				} @catch (__unused NSException *e) {
				}
			}
		}
		free(list);
	}
	subs = protocol_copyProtocolList(proto, &count);
	for (unsigned i = 0; i < count; i++) {
		configure_protocol_classes(interface, subs[i], depth + 1);
	}
	free(subs);
}

static void
client_path(pid_t pid, char *out, size_t size)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
	struct kinfo_proc kp;
	size_t length = sizeof(kp);

	if (size == 0) {
		return;
	}
	out[0] = 0;
	memset(&kp, 0, sizeof(kp));
	if (sysctl(mib, 4, &kp, &length, NULL, 0) == 0 && length > 0) {
		snprintf(out, size, "%s", kp.kp_proc.p_comm);
	} else {
		snprintf(out, size, "?");
	}
}

@interface CTShimListenerDelegate : NSObject <NSXPCListenerDelegate>
@end

@implementation CTShimListenerDelegate

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	@autoreleasepool {
		@try {
		Protocol *p = NSProtocolFromString(@"CTXPCServiceInterface");
		Protocol *delegate_protocol = NSProtocolFromString(@"CoreTelephonyClientDelegateProxyInterface");
		NSXPCInterface *interface;
		NSXPCInterface *delegate_interface = nil;
		CTShimServer *server;
		CTShimClient *client;
		char name[256];

		if (p == nil) {
			ctlog("connection from pid %d refused: no CTXPCServiceInterface", connection.processIdentifier);
			return NO;
		}
		interface = [NSXPCInterface interfaceWithProtocol:p];
		configure_argument_classes(interface);
		connection.exportedInterface = interface;
		server = [[CTShimServer alloc] init];
		server->connection = connection;
		connection.exportedObject = server;

		if (delegate_protocol != nil) {
			delegate_interface = [NSXPCInterface interfaceWithProtocol:delegate_protocol];
			configure_protocol_classes(delegate_interface, delegate_protocol, 0);
			connection.remoteObjectInterface = delegate_interface;
		} else {
			ctlog("no CoreTelephonyClientDelegateProxyInterface: notifications will not be sent");
		}

		client = [[CTShimClient alloc] init];
		/* MRC: the record outlives the delegate method, keep the connection alive until
		 * the invalidation handler removes the record. */
		client->connection = [connection retain];
		@synchronized (g_clients) {
			[g_clients addObject:client];
		}
		[connection setInvalidationHandler:^{
			client->invalidated = YES;
			@synchronized (g_clients) {
				[g_clients removeObjectIdenticalTo:client];
			}
			[client->connection release];
			client->connection = nil;
			ctlog("connection invalidated (pid %d)", connection.processIdentifier);
		}];
		[connection setInterruptionHandler:^{
			ctlog("connection interrupted (pid %d)", connection.processIdentifier);
		}];
		[connection resume];
		client_path((pid_t)connection.processIdentifier, name, sizeof(name));
		ctlog("accepted NSXPC connection from pid %d (%s, uid %u)",
		    connection.processIdentifier, name, connection.effectiveUserIdentifier);
		return YES;
		} @catch (NSException *e) {
			ctlog("exception while accepting a connection: %s: %s", e.name.UTF8String, e.reason.UTF8String);
			return NO;
		}
	}
}

@end

/* ------------------------------------------------------- legacy CTServerConnection */

static const char *
legacy_request(xpc_object_t message)
{
	return xpc_dictionary_get_string(message, "kRequest");
}

static bool g_cellular_data_enabled = true;

/*
 * A kRegistrationGetProperty / kPacketGetProperty request names the property it wants
 * (the value in the request is a null placeholder) and the reply must carry the same key:
 * CoreTelephony's getters do CFDictionaryGetValue(reply, <that key>) and look at the type
 * (e.g. CTServerConnectionCopyRegistrationStatus 0x188154b7c reads "kRegistrationStatus",
 * CopyNetworkSelectionMode 0x1881cb704 reads "kRegistrationNetworkSelectionMode" and
 * defaults to the Automatic constant).  ctshimd v2 answered only kCTRegistrationStatus,
 * a different key, so SpringBoard saw no registration/selection value at all.
 */
struct legacy_property {
	const char *key;
	char kind;              /* 's' string, 'b' bool, 'i' int64 */
	const char *value;
};

static void
legacy_add_properties(xpc_object_t request, xpc_object_t reply, const cell_state *cell,
    NSString *name)
{
	static const struct legacy_property props[] = {
		{ "kRegistrationStatus",                    's', "kCTRegistrationStatusRegisteredHome" },
		{ "kRegistrationDisplayStatus",             's', "kCTRegistrationStatusRegisteredHome" },
		{ "kRegistrationNetworkSelectionMode",      's', "kCTRegistrationNetworkSelectionModeAutomatic" },
		{ "kRegistrationNetworkSelectionState",     's', "kCTRegistrationNetworkSelectionStateIdle" },
		{ "kRegistrationCurrentNetworkSelection",   's', "" },
		{ "kRegistrationAutomaticNetworkSelection", 'b', NULL },
		{ "kRegistrationMCC",                       's', "250" },
		{ "kRegistrationMNC",                       's', "02" },
		{ "kRegistrationServingPLMN",               's', "25002" },
		{ "kRegistrationISOForMCC",                 's', "RU" },
		{ "kRegistrationOperatorName",              's', "" },
		{ "kRegistrationNetworkName",               's', "" },
		{ "kRegistrationAbbreviatedOperatorName",   's', "" },
		{ "kRegistrationRAT",                       's', "CTRadioAccessTechnologyLTE" },
		{ "kRegistrationUseCarrierBundle",          'b', NULL },
		{ "kRegistrationEmergencyCallBackMode",     'b', NULL },
		{ "kRegistrationIsInHomeCountry",           'b', NULL },
		{ "kRegistrationIsForcedRegisteredHome",    'b', NULL },
		{ "kRegistrationCampOnly",                  'b', NULL },
		{ "kRegistrationDataAttached",              'b', NULL },
		{ "kRegistrationDataActive",                'b', NULL },
		{ "kRegistrationDataStatus",                's', "kCTRegistrationDataStatusAttached" },
		{ "kCellularDataHideRatIndicator",          'b', NULL },
		{ "kCellularDataEnabled",                   'b', NULL },
		{ "kCellularDataCapable",                   'b', NULL },
		{ "kCellularDataAvailable",                 'b', NULL },
		{ "kCellularDataPossible",                  'b', NULL },
		{ "kCellularDataDisallow",                  'b', NULL },
		{ "kCellularDataHideIndicator",             'b', NULL },
		{ "kAirplaneMode",                          'b', NULL },
		{ "kEnableCellularData",                    'b', NULL },
	};
	static char seen[sizeof(props) / sizeof(props[0])];
	char mcc[8], mnc[8], plmn[16];

	snprintf(mcc, sizeof(mcc), "%u", cell->mcc);
	snprintf(mnc, sizeof(mnc), "%02u", cell->mnc);
	snprintf(plmn, sizeof(plmn), "%u%02u", cell->mcc, cell->mnc);

	for (unsigned i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
		const struct legacy_property *p = &props[i];
		const char *value = p->value;

		if (xpc_dictionary_get_value(request, p->key) == NULL) {
			continue;
		}
		if (p->kind == 's') {
			if (strcmp(p->key, "kRegistrationMCC") == 0) value = mcc;
			else if (strcmp(p->key, "kRegistrationMNC") == 0) value = mnc;
			else if (strcmp(p->key, "kRegistrationServingPLMN") == 0) value = plmn;
			else if (strcmp(p->key, "kRegistrationCurrentNetworkSelection") == 0 ||
			         strcmp(p->key, "kRegistrationOperatorName") == 0 ||
			         strcmp(p->key, "kRegistrationNetworkName") == 0 ||
			         strcmp(p->key, "kRegistrationAbbreviatedOperatorName") == 0) {
				value = name != nil ? name.UTF8String : "";
			}
			if ((strcmp(p->key, "kRegistrationStatus") == 0 ||
			     strcmp(p->key, "kRegistrationDisplayStatus") == 0) && !cell->sim_present) {
				value = "kCTRegistrationStatusNotRegistered";
			}
			xpc_dictionary_set_string(reply, p->key, value != NULL ? value : "");
		} else if (p->kind == 'b') {
			bool b = true;

			if (strcmp(p->key, "kCellularDataEnabled") == 0) {
				b = g_cellular_data_enabled;
			} else if (strcmp(p->key, "kRegistrationAutomaticNetworkSelection") == 0 ||
			           strcmp(p->key, "kRegistrationUseCarrierBundle") == 0 ||
			           strcmp(p->key, "kRegistrationIsInHomeCountry") == 0 ||
			           strcmp(p->key, "kRegistrationDataAttached") == 0 ||
			           strcmp(p->key, "kRegistrationDataActive") == 0 ||
			           strcmp(p->key, "kCellularDataCapable") == 0 ||
			           strcmp(p->key, "kCellularDataAvailable") == 0 ||
			           strcmp(p->key, "kCellularDataPossible") == 0 ||
			           strcmp(p->key, "kEnableCellularData") == 0) {
				b = true;
			} else {
				b = false;
			}
			xpc_dictionary_set_bool(reply, p->key, b);
		} else {
			xpc_dictionary_set_int64(reply, p->key, 0);
		}
		if (!seen[i]) {
			seen[i] = 1;
			ctlog("legacy property: %s -> %s", p->key,
			    p->kind == 's' ? (value != NULL ? value : "(null)") :
			    p->kind == 'b' ? "bool" : "int");
		}
	}
}

static xpc_object_t
legacy_reply(xpc_object_t request)
{
	xpc_object_t reply = xpc_dictionary_create(NULL, NULL, 0);
	const char *req = legacy_request(request);

	/* _CTServerConnectionSetCellularDataIsEnabled() sends the new state in the request; keep
	 * it so the getter and the Settings switch agree with each other.  No data call exists,
	 * so this is the switch's own state and nothing more.  Only a set request changes it:
	 * a get request carries the same key with a placeholder value. */
	if (req != NULL && strstr(req, "Set") != NULL &&
	    xpc_dictionary_get_value(request, "kCellularDataEnabled") != NULL) {
		bool wanted = xpc_dictionary_get_bool(request, "kCellularDataEnabled");
		if (wanted != g_cellular_data_enabled) {
			ctlog("legacy: cellular data switch -> %s (request %s)", wanted ? "on" : "off",
			    req != NULL ? req : "?");
		}
		g_cellular_data_enabled = wanted;
	}
	cell_state raw, cell;
	if (cell_read(&raw) == 0) {
		cell_effective(&raw, &cell);
	} else {
		cell_provisioned(&cell);
	}
	NSString *name = operator_name(&cell);
	int bars = cell_bars(&cell);

	xpc_dictionary_set_bool(reply, "kDidSucceed", true);
	xpc_dictionary_set_bool(reply, "kRadioModuleValid", true);
	xpc_dictionary_set_bool(reply, "kRadioModuleIsDead", false);
	xpc_dictionary_set_bool(reply, "kRadioModuleReady", true);
	xpc_dictionary_set_int64(reply, "kRadioModuleState", 1);
	xpc_dictionary_set_bool(reply, "kHasBaseband", true);
	xpc_dictionary_set_bool(reply, "kCellularDataAvailable", true);
	xpc_dictionary_set_bool(reply, "kCellularDataCapable", true);
	xpc_dictionary_set_bool(reply, "kCellularDataEnabled", g_cellular_data_enabled);
	xpc_dictionary_set_bool(reply, "kCellularDataPossible", true);
	xpc_dictionary_set_bool(reply, "kCellularDataDisallow", false);
	xpc_dictionary_set_bool(reply, "kCellularDataHideIndicator", false);
	xpc_dictionary_set_bool(reply, "kAirplaneMode", false);
	xpc_dictionary_set_bool(reply, "kCTRegistrationDataActive", false);
	xpc_dictionary_set_bool(reply, "kCTRegistrationDataAttached", cell.registered);
	xpc_dictionary_set_int64(reply, "kCTIndicatorsSignalStrength", bars);
	xpc_dictionary_set_int64(reply, "kSignalStrength", bars);
	xpc_dictionary_set_int64(reply, "kCTIndicatorsSignalStrengthDisplayValue", bars);
	xpc_dictionary_set_int64(reply, "kCTIndicatorsSignalStrengthMaxDisplayValue", 4);
	xpc_dictionary_set_int64(reply, "kCTIndicatorsGradedSignalStrength", bars);
	xpc_dictionary_set_int64(reply, "kCTIndicatorsRawSignalStrength", cell.has_signal ? cell.signal_dbm : 0);
	if (cell.registered) {
		xpc_dictionary_set_string(reply, "kCTRegistrationStatus", "kCTRegistrationStatusRegisteredHome");
		xpc_dictionary_set_string(reply, "kCTRegistrationDataStatus", "kCTRegistrationDataStatusAttached");
	} else if (cell.searching) {
		xpc_dictionary_set_string(reply, "kCTRegistrationStatus", "kCTRegistrationStatusSearching");
	} else {
		xpc_dictionary_set_string(reply, "kCTRegistrationStatus", "kCTRegistrationStatusNotRegistered");
	}
	if (name != nil) {
		xpc_dictionary_set_string(reply, "kCTRegistrationOperatorName", name.UTF8String);
		xpc_dictionary_set_string(reply, "kCTRegistrationNetworkName", name.UTF8String);
		xpc_dictionary_set_string(reply, "kCTRegistrationAbbreviatedOperatorName", name.UTF8String);
		xpc_dictionary_set_string(reply, "kRegistrationOperatorName", name.UTF8String);
		xpc_dictionary_set_string(reply, "kRegistrationNetworkName", name.UTF8String);
		xpc_dictionary_set_string(reply, "kRegistrationAbbreviatedOperatorName", name.UTF8String);
	}
	/* The same property under the two names the two generations of clients read:
	 * kRegistration* is the CTServerConnection dictionary key, kCTRegistration* is what
	 * CommCenter puts into notification payloads. */
	xpc_dictionary_set_string(reply, "kRegistrationStatus",
	    cell.registered ? "kCTRegistrationStatusRegisteredHome" : "kCTRegistrationStatusNotRegistered");
	xpc_dictionary_set_string(reply, "kRegistrationDisplayStatus",
	    cell.registered ? "kCTRegistrationStatusRegisteredHome" : "kCTRegistrationStatusNotRegistered");
	xpc_dictionary_set_string(reply, "kRegistrationNetworkSelectionMode",
	    "kCTRegistrationNetworkSelectionModeAutomatic");
	xpc_dictionary_set_string(reply, "kRegistrationNetworkSelectionState",
	    "kCTRegistrationNetworkSelectionStateIdle");
	if (cell.valid) {
		xpc_dictionary_set_string(reply, "kCTSIMSupportSIMStatus",
		    cell.sim_ready ? "kCTSIMSupportSIMStatusReady" :
		    cell.sim_present ? "kCTSIMSupportSIMStatusNotReady" : "kCTSIMSupportSIMStatusNotInserted");
	}
	if (cell.mcc != 0) {
		xpc_dictionary_set_string(reply, "kCTRegistrationMobileCountryCode",
		    [[NSString stringWithFormat:@"%u", cell.mcc] UTF8String]);
		xpc_dictionary_set_string(reply, "kCTRegistrationMobileNetworkCode",
		    [[NSString stringWithFormat:@"%02u", cell.mnc] UTF8String]);
	}
	legacy_add_properties(request, reply, &cell, name);
	if (req != NULL) {
		if (strstr(req, "RadioModuleGetProperty") != NULL) {
			/* kRadioModuleGetProperty asks for one named property; the reply is
			 * { kRadioModuleValid: true, <property>: value }.  Every property this
			 * server knows is false/zero; that is the honest answer from a modem the
			 * Apple baseband stack cannot drive. */
		}
	}
	return reply;
}

static void
legacy_peer_event(xpc_connection_t peer, xpc_object_t event)
{
	xpc_type_t type = xpc_get_type(event);

	if (type == XPC_TYPE_ERROR) {
		const char *desc = xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);

		ctlog("legacy peer: %s", desc != NULL ? desc : "(no description)");
		return;
	}
	if (type != XPC_TYPE_DICTIONARY) {
		ctlog("legacy peer: unexpected message type; ignored");
		return;
	}
	char *desc = xpc_copy_description(event);
	ctlog("legacy request: %s", desc ? desc : "(no description)");
	free(desc);
	xpc_object_t reply = legacy_reply(event);
	xpc_connection_send_message(peer, reply);
}

static void
legacy_listener_event(xpc_connection_t listener, xpc_object_t event)
{
	xpc_type_t type = xpc_get_type(event);

	if (type == XPC_TYPE_CONNECTION) {
		/*
		 * For a listener the event object IS the new peer connection; the listener is the
		 * function's first argument.  v1 passed the listener here and called
		 * xpc_connection_resume() on it a second time: libxpc treats that as an API misuse
		 * and traps (SIGTRAP), which is why ctshimd died one second after the first legacy
		 * client appeared.
		 */
		xpc_connection_t peer = event;
		pid_t pid = xpc_connection_get_pid(peer);
		char name[256];

		client_path(pid, name, sizeof(name));
		ctlog("legacy client connected: pid %d (%s)", pid, name);
		xpc_retain(peer);
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			legacy_peer_event(peer, message);
		});
		xpc_connection_resume(peer);
	} else if (type == XPC_TYPE_ERROR) {
		const char *desc = xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);

		ctlog("legacy listener: %s", desc != NULL ? desc : "(no description)");
	}
}

static void
start_legacy_listener(const char *name, dispatch_queue_t queue)
{
	xpc_connection_t listener = xpc_connection_create_mach_service(name, queue,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	if (listener == NULL) {
		ctlog("legacy listener %s: cannot be created", name);
		return;
	}
	xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
		legacy_listener_event(listener, event);
	});
	xpc_connection_resume(listener);
	ctlog("legacy listener %s is up", name);
}

/* ---------------------------------------------------------- state-change logging */

/* CommCenter posts these through notifyd when the SIM or the serving PLMN changes; clients
 * that watch them re-query their CoreTelephony connection.  Posting the same names when the
 * kernel's record changes keeps that part of the system's behaviour. */
extern int notify_post(const char *name);

static void
post_telephony_notifications(void)
{
	notify_post("com.apple.commcenter.SimStatus.changed");
	notify_post("com.apple.commcenter.Plmn.changed");
}

static int
cell_same(const cell_state *a, const cell_state *b)
{
	return a->sim_present == b->sim_present && a->sim_ready == b->sim_ready &&
	    a->registered == b->registered && a->searching == b->searching &&
	    a->denied == b->denied && a->roaming == b->roaming && a->radio == b->radio &&
	    a->has_signal == b->has_signal && a->signal_dbm == b->signal_dbm &&
	    a->mcc == b->mcc && a->mnc == b->mnc &&
	    strcmp(a->operator_name, b->operator_name) == 0;
}

/*
 * /dev/vayu-cell does not exist at boot: vayu_cell.c creates it only after the modem is up
 * (d1081c: kernel [160.50 s], while ctshimd was spawned by launchd at [~159 s]).  The v1 job
 * read the record once at start and never looked again, so even a live process would have kept
 * answering "no service".  This timer polls forever: it never exits on a missing device, logs
 * the transition once, re-reads the record for every answer, and pushes the first real state to
 * every registered CoreTelephony client.
 */
static void
poll_state(void)
{
	static int said_missing = 0;
	cell_state raw, cell;
	int rc = cell_read(&raw);
	int device = (rc == 0 && raw.valid);

	if (device) {
		cell_effective(&raw, &cell);
		said_missing = 0;
	} else {
		cell_provisioned(&cell);
		if (!said_missing) {
			ctlog("state: /dev/vayu-cell is not readable yet (%d: %s); answering the "
			    "provisioned LTE state and still polling every 1 s",
			    rc, rc < 0 ? strerror(-rc) : "invalid record");
			said_missing = 1;
		}
	}
	if (!g_have_cell || !cell_same(&g_last_cell, &cell) || device != g_have_device) {
		int first = !g_have_cell;
		int appeared = device && !g_have_device;
		const char *prefix = first ? (device ? "/dev/vayu-cell appeared, " : "first answer, ") :
		    appeared ? "/dev/vayu-cell appeared, " : "";

		g_last_cell = cell;
		g_have_cell = 1;
		g_have_device = device;
		post_telephony_notifications();
		ctlog("state: %sseq %u, sim %s, registered %d, radio %d, %d dBm, %d bars, "
		    "MCC/MNC %u/%u, operator %s%s",
		    prefix,
		    cell.seq, cell.sim_ready ? "ready" : cell.sim_present ? "present" : "absent",
		    cell.registered, cell.radio, cell.has_signal ? cell.signal_dbm : 0,
		    cell_bars(&cell), cell.mcc, cell.mnc,
		    cell.operator_name[0] ? cell.operator_name : "(none)",
		    device ? "" : " (provisioned, the kernel record is not there yet)");
		if (device && (raw.searching || raw.denied || !raw.registered)) {
			ctlog("state: the modem says %s; the screen keeps RegisteredHome so the "
			    "stock network-lost alert cannot appear",
			    raw.denied ? "denied" : raw.searching ? "searching" : "not registered");
		}
		notify_clients(&cell);
	}
}

static void
start_state_timer(void)
{
	dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);

	if (timer == NULL) {
		ctlog("state: no timer; /dev/vayu-cell will only be read per request");
		return;
	}
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
	    NSEC_PER_SEC, 200 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(timer, ^{
		poll_state();
	});
	dispatch_resume(timer);
}

/* ---------------------------------------------------------------------- main */

static void
ctshim_uncaught_exception(NSException *exception)
{
	ctlog("uncaught exception %s: %s", [[exception name] UTF8String],
	    [[exception reason] UTF8String]);
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		cell_state probe;
		int rc;

		ctlog("start: pid %d", getpid());
		NSSetUncaughtExceptionHandler(ctshim_uncaught_exception);
		g_clients = [NSMutableArray new];
		rc = cell_read(&probe);
		if (rc == 0) {
			ctlog("/dev/vayu-cell: seq %u, sim %s, registered %d, radio %d, signal %d dBm, MCC/MNC %u/%u",
			    probe.seq, probe.sim_ready ? "ready" : probe.sim_present ? "present" : "absent",
			    probe.registered, probe.radio, probe.has_signal ? probe.signal_dbm : 0,
			    probe.mcc, probe.mnc);
		} else {
			ctlog("/dev/vayu-cell: not there yet (%d: %s); answering the provisioned "
			    "LTE state until the kernel creates it (never exits)",
			    rc, strerror(rc < 0 ? -rc : rc));
		}

		/* The protocol objects live in CoreTelephony.framework; load it before asking. */
		void *ct = dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony", RTLD_NOW);
		ctlog("CoreTelephony: %s", ct != NULL ? "loaded" : dlerror());
		build_method_map();
		g_context = make_context();
		ctlog("subscription context: %s", g_context != nil ? "ready" : "unavailable");

		CTShimListenerDelegate *delegate = [[CTShimListenerDelegate alloc] init];
		/* -initWithMachServiceName: is marked API_UNAVAILABLE(ios) in the 16.5 SDK,
		 * but Foundation implements it (0x18121a340 in the 20F66 cache) and CommCenter's
		 * own server uses it. Send it through the runtime. */
		NSXPCListener *listener = ((id (*)(id, SEL, id))objc_msgSend)([NSXPCListener alloc],
		    sel_registerName("initWithMachServiceName:"),
		    @"com.apple.commcenter.coretelephony.xpc");
		listener.delegate = delegate;
		if (listener == nil) {
			/* iOS marks -initWithMachServiceName: unavailable for apps; if this
			 * Foundation really refuses it, +serviceListener uses the first
			 * MachServices name of the job (the plist lists coretelephony first). */
			@try {
				listener = [NSXPCListener serviceListener];
			} @catch (NSException *e) {
				listener = nil;
			}
			ctlog("initWithMachServiceName: refused; +serviceListener fallback -> %s",
			    listener != nil ? "a listener" : "nil");
		}
		if (listener == nil) {
			ctlog("NSXPC listener com.apple.commcenter.coretelephony.xpc could not be created");
		} else {
			[listener resume];
			ctlog("NSXPC listener com.apple.commcenter.coretelephony.xpc is up");
		}

		dispatch_queue_t queue = dispatch_queue_create("com.vayu.ctshim.legacy", DISPATCH_QUEUE_SERIAL);
		start_legacy_listener("com.apple.commcenter.xpc", queue);
		start_state_timer();

		/* NSXPC, the legacy listener and the poll timer all run on dispatch queues.  The v1
		 * process sat in -[NSRunLoop runMode:beforeDate:] with no run-loop sources and spun
		 * at full speed; dispatch_main parks the main thread instead and never returns, so
		 * the job can only go away if launchd kills it. */
		ctlog("running: NSXPC + legacy + 1 s state poll");
		dispatch_main();
	}
	return 0;
}
