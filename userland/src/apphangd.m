/*
 * apphangd — the "no such service, and quickly" shim for Phone and Messages.
 *
 * Why it exists.  Two applications opened by the owner hang on vayu:
 *
 *   MobilePhone (com.apple.mobilephone)  asks CallServicesDaemon for
 *       com.apple.telephonyutilities.callservicesdaemon.callcapabilities,
 *       ...callprovidermanager, ...callstatecontroller (and ...conversationmanager);
 *       it also asks IDS for com.apple.identityservicesd.idquery.embedded.auth.
 *   MobileSMS   (com.apple.MobileSMS)    asks IMCore for the same CallServices names
 *       and, through IMCore, CoreTelephony.
 *
 * None of those names has an owner in the root image:
 *
 *   - there is no LaunchDaemon plist for callservicesd or identityservicesd in
 *     /redwood/LaunchDaemons (418 jobs, checked), and
 *   - the system image does not even carry their programs: select-system.py takes a
 *     framework file only up to 4 MB (callservicesd is 4 959 792 B, identityservicesd
 *     11 306 400 B), so mkjobs.py refused both jobs when the image was built
 *     (mkjobs.py's comment names callservicesd and identityservicesd among the seven
 *     jobs that exist only on a flash boot).
 *
 * A missing name makes launchd answer "No such process" immediately, and the clients
 * retry: in the 3 Nov run d15/141639-run.log MobileSMS asks
 * com.apple.commcenter.coretelephony.xpc 306 times in 24 s and MobilePhone asks
 * callcapabilities 33 times.  The CoreTelephony half is fixed by ctshimd (the base
 * image root30-ctshim owns com.apple.commcenter.coretelephony.xpc and
 * com.apple.commcenter.xpc).  This daemon is the other half.
 *
 * What it answers.  A generic "connection refused" would stop the lookup but not the
 * wait: TUCallProviderManagerXPCClient has -blockUntilInitialStateReceived and
 * TUCallCapabilities has a state reply, and both wait for a *reply*, not for a
 * connection error.  So for the ten CallServices names the shim exports the real
 * server protocol from TelephonyUtilities.framework (the same protocol objects the
 * client uses, so NSXPC's encoding is symmetric by construction) and answers the
 * launch-time questions with empty but valid objects:
 *
 *   TUCallCapabilitiesXPCServer        callCapabilitiesState:  -> TUCallCapabilitiesState
 *   TUCallProviderManagerXPCServer     providersByIdentifier:  -> empty NSDictionary
 *   TUCallCenterXPCServer              requestInitialState:    -> empty NSArray
 *   TUCallHistoryManagerXPCServer      recentCalls             -> empty NSArray
 *   TUConversationManagerXPCServer     (no launch-time data)   -> nil
 *   TUMomentsControllerXPCServer       (no launch-time data)   -> nil
 *
 * Every other selector of those protocols gets nil (and its reply block is still
 * called, so the client does not wait).
 *
 * It deliberately does NOT take:
 *   com.apple.imagent / com.apple.madrid-idswake / com.apple.imdpersistence.IMDPersistenceAgent
 * — those are real in this image: imagent (597 568 B, inside IMCore.framework) and the
 * IMDPersistenceAgent.xpc service are in the system image and do spawn (d15:321, d15:322).
 * Taking their names would replace working services with this shim.
 *
 * apphang2 (05.11 evening, after d1081c2 and d1081c3).  Three changes:
 *
 *   0. THE IDENTITYSERVICESD NAMES ARE GONE AGAIN, and that is the main lesson of d1081c3.
 *      apphang1 held com.apple.identityservicesd.embedded.auth with an "error-only"
 *      interface (every selector is an immediate NSXPC error).  In d1081c3 that name
 *      produced 36 113 accepts and 35 433 invalidations in 9 minutes — 34 connections a
 *      second from SpringBoard[9], apsd/aggregated[49], etc. (131417-run.log; the pairs
 *      "accepting pid N with an error-only interface" / "connection from pid N invalidated"
 *      fill ~71 000 lines of the 86 000-line log).  The clients do not back off after an
 *      NSXPC error: they drop the connection and connect again at once.  An absent name is
 *      strictly better: d15 has 34 embedded.auth lookups in a whole run, SpringBoard one.
 *      So this daemon now owns ONLY names for which it can export a real TelephonyUtilities
 *      server protocol and give a valid reply.  All error-only names (identityservicesd.*,
 *      callkit hosts, idswake routes, usernotifications delegates, voip) are left to
 *      launchd's "No such process", which is the cheap failure.
 *
 *   1. The dial path, for the owner's one approved number 0500.  Phone's call button
 *      ends in -[TUCallCenterXPCServer dialWithRequest:reply:] (protocol TUCallCenterXPCServer,
 *      the type string Vv32@0:8@16@?24).  The shim reads TUDialRequest's destinationID (or
 *      its handle's value), normalises it to digits and, only when it is exactly "0500",
 *      writes "dial 0500\n" to /dev/vayu-voice — the text protocol of the k31-voice QMI
 *      VOICE client (bsd/dev/vayu_voice.c: voice_command() -> VOICE_JOB_DIAL).  Every
 *      other number is refused with a line in the console log; the reply block is called
 *      at once with (nil, NSError), so Phone shows its normal failure instead of waiting.
 *      The call is ended with "end 1" after APPHANG_DIAL_HOLD seconds (default 45, 0 =
 *      never) and on -disconnectCall... / -disconnectAllCalls from the UI.  APPHANG_DIAL_DRYRUN=1
 *      logs the decision without touching the device.
 *
 *   2. A connection-count limiter: the first 20 accepts/invalidations of each service are
 *      printed, then every 1000th.  d1081c3 showed that a client loop can drown the console
 *      (and the run log) in one line per connection; this keeps the evidence readable even
 *      if a future client loops, without hiding the fact that it does.
 *
 *   3. Console logging of the decision ("dial 0500: written to /dev/vayu-voice"), which goes
 *      straight to /dev/console and therefore survives even when oslogfwd does not: in
 *      d1081c2 oslogfwd printed only 477 of 62 544 events, dropped 1 583 at its rate limit
 *      and was reported "burning CPU" (d1081c2/125721-run.log:136318), so the application's
 *      own os_log never reached the run log.  That is also why this build adds MobileSMS,
 *      MobilePhone, imagent and IMDPersistenceAgent to /redwood/etc/oslogfwd.filter and puts
 *      libhakios.app.dylib into both applications (their stderr then goes to /dev/console,
 *      not through oslogfwd).
 *
 * Build: iOS 16.5 SDK, arm64, -fno-objc-arc, Foundation + TelephonyUtilities; see
 * scripts/build-artifacts.sh.
 */

#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* An interface with no methods: every real selector is absent, so NSXPC answers
 * "unrecognized selector" at once.  A custom empty protocol is used instead of
 * NSObject, whose -retain/-release methods are not NSXPC material. */
@protocol AppHangNoMethods
@end

static void
apphanglog(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "apphangd: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

/* ------------------------------------------------------------- block by signature */

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
	p = skip_encoding(sig);
	if (p == NULL) return -1;
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
		n = 2;
		first = "@";
	}
	apphanglog("reply %p: sig \"%s\" argc %d", block, sig ? sig : "(none)", n);
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
		((void (*)(void *, void *, void *, void *, void *, void *))b->invoke)(block,
		    object_valid ? object : nil, nil, nil, nil, nil);
		break;
	}
}

/* --------------------------------------------------------------- protocol map */

static NSMutableDictionary<NSString *, NSString *> *g_method_types;

static void
add_protocol_methods(Protocol *proto)
{
	unsigned int count = 0, i;
	struct objc_method_description *descs;

	if (proto == nil) {
		return;
	}
	descs = protocol_copyMethodDescriptionList(proto, YES, YES, &count);
	for (i = 0; i < count; i++) {
		if (descs[i].name != NULL && descs[i].types != NULL) {
			g_method_types[NSStringFromSelector(descs[i].name)] =
			    [NSString stringWithUTF8String:descs[i].types];
		}
	}
	free(descs);
	descs = protocol_copyMethodDescriptionList(proto, NO, YES, &count);
	for (i = 0; i < count; i++) {
		if (descs[i].name != NULL && descs[i].types != NULL) {
			g_method_types[NSStringFromSelector(descs[i].name)] =
			    [NSString stringWithUTF8String:descs[i].types];
		}
	}
	free(descs);

	Protocol *__unsafe_unretained *subs = protocol_copyProtocolList(proto, &count);
	for (i = 0; i < count; i++) {
		add_protocol_methods(subs[i]);
	}
	free(subs);
}

static NSSet *
telephony_classes(void)
{
	static NSSet *set;
	int count, i;

	if (set != nil) {
		return set;
	}
	NSMutableSet *classes = [NSMutableSet setWithObjects:
	    [NSObject class], [NSString class], [NSArray class], [NSDictionary class],
	    [NSSet class], [NSNumber class], [NSData class], [NSDate class],
	    [NSUUID class], [NSError class], [NSXPCListenerEndpoint class], nil];
	count = objc_getClassList(NULL, 0);
	if (count > 0) {
		Class *list = calloc((size_t)count, sizeof(Class));
		if (list != NULL) {
			count = objc_getClassList(list, count);
			for (i = 0; i < count; i++) {
				const char *name = class_getName(list[i]);
				/* NSXPC's setClasses: accepts only NSSecureCoding classes; a
				 * non-conforming class in the set makes the whole call throw and
				 * leaves the client without the reply classes it needs. */
				if (name != NULL && strncmp(name, "TU", 2) == 0 &&
				    [list[i] conformsToProtocol:@protocol(NSSecureCoding)]) {
					[classes addObject:list[i]];
				}
			}
			free(list);
		}
	}
	set = [classes copy];
	return set;
}

static void
configure_argument_classes(NSXPCInterface *interface)
{
	NSSet *classes = telephony_classes();

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
		/* The protocol spells a reply block as "@?" and says nothing about what the
		 * block carries; the launch-time replies carry TUCallCapabilitiesState,
		 * NSDictionary, NSArray.  Allow them at every reply index (0..3); NSXPC
		 * throws where the index does not exist and the @try keeps that harmless. */
		for (NSUInteger r = 0; r < 4; r++) {
			@try {
				[interface setClasses:classes forSelector:sel argumentIndex:r ofReply:YES];
			} @catch (__unused NSException *e) {
			}
		}
	}
}

/* Call a reply block with (nil, error).  The dial reply's signature is
 * (TUCall *, NSError *) in this TelephonyUtilities; a nil call alone could be read as
 * success, so the error is what tells the UI to take its failure path. */
static void
block_reply_error(id block, id error)
{
	const char *sig = NULL, *first = NULL;
	struct block_layout *b = (struct block_layout *)block;
	int n;

	if (block == nil || b->invoke == NULL) {
		return;
	}
	n = block_argument_count(block, &sig, &first);
	apphanglog("reply %p: sig \"%s\" argc %d (error)", block, sig ? sig : "(none)", n);
	if (n == 2) {
		((void (*)(void *, void *, void *))b->invoke)(block, nil, error);
		return;
	}
	block_reply(block, nil, NO, 0);
}

/* ------------------------------------------------------------ the one number */

/* /dev/vayu-voice is the text protocol of the kernel VOICE client.  It exists only when
 * the kernel was built with vayu_voice and booted with vayu_call=2 vayu_call_dial=1; when
 * it does not exist the open fails, the line below says so, and the application still gets
 * its reply (no hang). */

static NSString *
voice_device_path(void)
{
	const char *path = getenv("APPHANG_VOICE");

	return path != NULL ? [NSString stringWithUTF8String:path] : @"/dev/vayu-voice";
}

static BOOL
voice_write_line(NSString *line, NSString **why)
{
	const char *path = voice_device_path().UTF8String;
	char buffer[128];
	int fd, n, saved;

	n = snprintf(buffer, sizeof(buffer), "%s\n", line.UTF8String);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		saved = errno;
		if (why != NULL) {
			*why = [NSString stringWithFormat:@"open %s: %s", path, strerror(saved)];
		}
		return NO;
	}
	if (write(fd, buffer, (size_t)n) != n) {
		saved = errno;
		close(fd);
		if (why != NULL) {
			*why = [NSString stringWithFormat:@"write %s: %s", path, strerror(saved)];
		}
		return NO;
	}
	close(fd);
	return YES;
}

static int
dial_hold_seconds(void)
{
	const char *text = getenv("APPHANG_DIAL_HOLD");
	int seconds;

	if (text == NULL) {
		return 45;
	}
	seconds = atoi(text);
	if (seconds < 0) {
		seconds = 0;
	}
	if (seconds > 600) {
		seconds = 600;
	}
	return seconds;
}

static void
voice_end_call(const char *why)
{
	NSString *error = nil;

	if (getenv("APPHANG_DIAL_DRYRUN") != NULL) {
		apphanglog("end 1 (%s): dry run, the device is not touched", why);
		return;
	}
	if (voice_write_line(@"end 1", &error)) {
		apphanglog("end 1 (%s): written to %s", why, voice_device_path().UTF8String);
	} else {
		apphanglog("end 1 (%s): %s", why, error.UTF8String);
	}
}

/* The reply to Phone's dial: there is no TUCall on this side, and a nil call without an
 * error could be read as a success.  The UI shows this text and stays alive. */
static id
dial_failure_error(void)
{
	return [NSError errorWithDomain:@"com.vayu.apphang" code:1 userInfo:@{
		NSLocalizedDescriptionKey: @"Calling is not available on this device"
	}];
}

static void
voice_dial_0500(void)
{
	NSString *error = nil;
	int hold = dial_hold_seconds();

	if (getenv("APPHANG_DIAL_DRYRUN") != NULL) {
		apphanglog("dial 0500: dry run, the device is not touched");
		return;
	}
	if (!voice_write_line(@"dial 0500", &error)) {
		apphanglog("dial 0500: %s", error.UTF8String);
		return;
	}
	apphanglog("dial 0500: written to %s; the kernel prints the modem's answer on the console",
	    voice_device_path().UTF8String);
	if (hold > 0) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)hold * NSEC_PER_SEC),
		    dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
			NSString *late = nil;

			if (voice_write_line(@"end 1", &late)) {
				apphanglog("dial 0500: %d s are over, end 1 written", hold);
			} else {
				apphanglog("dial 0500: %d s are over, end 1 failed: %s", hold,
				    late.UTF8String);
			}
		});
	}
}

/* TUDialRequest carries the number in -destinationID (a string) or in -handle, a TUHandle
 * whose -value is that string.  Both are read with objc_msgSend so this file needs no TU
 * headers. */
static NSString *
dial_destination(id request)
{
	id value;

	if (request == nil) {
		return nil;
	}
	if ([request respondsToSelector:@selector(destinationID)]) {
		value = ((id (*)(id, SEL))objc_msgSend)(request, @selector(destinationID));
		if ([value isKindOfClass:[NSString class]] && [value length] > 0) {
			return value;
		}
	}
	if ([request respondsToSelector:@selector(handle)]) {
		id handle = ((id (*)(id, SEL))objc_msgSend)(request, @selector(handle));
		if (handle != nil && [handle respondsToSelector:@selector(value)]) {
			value = ((id (*)(id, SEL))objc_msgSend)(handle, @selector(value));
			if ([value isKindOfClass:[NSString class]] && [value length] > 0) {
				return value;
			}
		}
	}
	return nil;
}

/* Digits, '+', '*' and '#': enough to compare a dialled number with the allowed one. */
static NSString *
dial_digits(NSString *value)
{
	NSMutableString *digits = [NSMutableString string];

	for (NSUInteger i = 0; i < value.length; i++) {
		unichar c = [value characterAtIndex:i];

		if ((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#') {
			[digits appendFormat:@"%C", c];
		}
	}
	return digits;
}

/* --------------------------------------------------------------- the answers */

static id
capabilities_state(void)
{
	Class cls = NSClassFromString(@"TUCallCapabilitiesState");
	id state;

	if (cls == Nil) {
		return nil;
	}
	state = [[cls alloc] init];
	@try {
		/* Telephony is present through ctshimd; FaceTime/relay are not.  The class
		 * exposes these as setters (dump: TUCallCapabilitiesState @0x1e206d800). */
		[state setSupportsBasebandCalling:YES];
		[state setSupportsPrimaryCalling:YES];
		[state setSupportsDisplayingTelephonyCalls:YES];
		[state setSupportsCellularData:YES];
		[state setCtCapabilitiesValid:YES];
		[state setSupportsDisplayingFaceTimeAudioCalls:NO];
		[state setSupportsDisplayingFaceTimeVideoCalls:NO];
		[state setFaceTimeAudioAvailable:NO];
		[state setFaceTimeVideoAvailable:NO];
		[state setRelayCallingEnabled:NO];
		[state setSupportsRelayCalling:NO];
		[state setSupportsTelephonyRelayCalling:NO];
		[state setSupportsFaceTimeAudioRelayCalling:NO];
		[state setSupportsFaceTimeVideoRelayCalling:NO];
		[state setEmergencyCallbackModeEnabled:NO];
	} @catch (__unused NSException *e) {
	}
	return state;
}

static id
result_for(NSString *service, NSString *sel, BOOL *valid)
{
	*valid = NO;

	if ([service rangeOfString:@"callcapabilities"].location != NSNotFound &&
	    [sel hasPrefix:@"callCapabilitiesState"]) {
		id state = capabilities_state();
		*valid = state != nil;
		return state;
	}
	if ([service rangeOfString:@"callprovidermanager"].location != NSNotFound &&
	    [sel hasPrefix:@"providersByIdentifier"]) {
		*valid = YES;
		return @{};
	}
	if ([service rangeOfString:@"callstatecontroller"].location != NSNotFound &&
	    [sel hasPrefix:@"requestInitialState"]) {
		*valid = YES;
		return @[];
	}
	if (([service rangeOfString:@"callhistorymanager"].location != NSNotFound ||
	     [service rangeOfString:@"callhistorycontroller"].location != NSNotFound) &&
	    ([sel hasPrefix:@"recentCalls"] || [sel hasPrefix:@"callHistory"])) {
		*valid = YES;
		return @[];
	}
	/* Every other selector is answered with (nil[, nil]) by the caller; a reply block
	 * that exists is still invoked, so no client waits for a daemon that is not there. */
	return nil;
}

@interface AppHangServer : NSObject
@property (nonatomic, copy) NSString *serviceName;
@end

@implementation AppHangServer

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
	id reply = nil;
	id result;
	BOOL valid = NO;

	for (NSUInteger i = 2; i < count; i++) {
		const char *type = [sig getArgumentTypeAtIndex:i];
		if (type != NULL && type[0] == '@' && type[1] == '?') {
			__unsafe_unretained id block = nil;
			[inv getArgument:&block atIndex:i];
			reply = block;
			break;
		}
	}

	/* The one number the owner approved.  Phone's call button arrives here as
	 * -dialWithRequest:reply: (or -dialWithRequest:displayContext:, which has no reply). */
	if ([self.serviceName rangeOfString:@"callstatecontroller"].location != NSNotFound &&
	    [sel hasPrefix:@"dialWithRequest"]) {
		id request = nil;
		NSString *destination = nil;
		NSString *digits = nil;

		if (count > 2) {
			__unsafe_unretained id argument = nil;
			[inv getArgument:&argument atIndex:2];
			@try {
				destination = dial_destination(argument);
			} @catch (NSException *e) {
				apphanglog("%s: %s: reading the destination failed: %s: %s",
				    self.serviceName.UTF8String, sel.UTF8String, e.name.UTF8String,
				    e.reason.UTF8String);
			}
		}
		digits = destination != nil ? dial_digits(destination) : nil;
		if (digits != nil && [digits isEqualToString:@"0500"]) {
			apphanglog("%s: %s: destination 0500 (the approved number)",
			    self.serviceName.UTF8String, sel.UTF8String);
			voice_dial_0500();
		} else if (destination != nil) {
			/* Never write the owner's number to the log: only its length. */
			apphanglog("%s: %s: a %lu-digit destination, masked; only 0500 is dialled",
			    self.serviceName.UTF8String, sel.UTF8String, (unsigned long)digits.length);
		} else {
			apphanglog("%s: %s: no destination this shim can read; nothing is dialled",
			    self.serviceName.UTF8String, sel.UTF8String);
		}
		if (reply != nil) {
			/* No TUCall object exists on this side, so the reply is (nil, NSError):
			 * the UI shows its normal failure at once instead of waiting for a daemon
			 * that is not there.  The modem's call, if one was placed, is independent
			 * of it. */
			apphanglog("%s: %s -> (no call object, an error)", self.serviceName.UTF8String,
			    sel.UTF8String);
			block_reply_error(reply, dial_failure_error());
		}
		return;
	}

	/* The red button.  The kernel numbers the first call 1; that is the only call this
	 * shim ever places. */
	if ([self.serviceName rangeOfString:@"callstatecontroller"].location != NSNotFound &&
	    ([sel hasPrefix:@"disconnectCall"] || [sel hasPrefix:@"disconnectAllCalls"] ||
	     [sel hasPrefix:@"disconnectCurrentCall"])) {
		voice_end_call(sel.UTF8String);
	}

	if (reply == nil) {
		/* A void method without a reply block: give the invocation a defined
		 * zero return value so NSXPC's automatic reply is well formed. */
		NSUInteger length = [sig methodReturnLength];
		if (length > 0) {
			void *zero = calloc(1, length);
			if (zero != NULL) {
				[inv setReturnValue:zero];
				free(zero);
			}
		}
		apphanglog("%s: %s (no reply block)", self.serviceName.UTF8String, sel.UTF8String);
		return;
	}
	@try {
		result = result_for(self.serviceName, sel, &valid);
		apphanglog("%s: %s -> %s", self.serviceName.UTF8String, sel.UTF8String,
		    valid ? object_getClassName(result) : "(nil)");
		block_reply(reply, result, valid, 0);
	} @catch (NSException *e) {
		apphanglog("%s: %s exception %s: %s; replying nil",
		    self.serviceName.UTF8String, sel.UTF8String,
		    e.name.UTF8String, e.reason.UTF8String);
		block_reply(reply, nil, NO, 0);
	}
}

@end

/* ------------------------------------------------------- connection counting */

/* A client that gets an NSXPC error does not back off: in d1081c3 the error-only
 * identityservicesd.embedded.auth was accepted 36 113 times in nine minutes.  There are
 * no error-only names left, but a protocol client can still loop; this prints the first
 * 20 events of each kind and then every 1000th, so the console (and the run log) survives
 * a loop while still showing that it happens. */
static NSMutableDictionary *g_connection_counts;

static NSUInteger
count_connection_event(NSString *service, const char *event)
{
	NSString *key = [NSString stringWithFormat:@"%@:%s", service, event];
	NSUInteger count;

	@synchronized (g_connection_counts) {
		count = [g_connection_counts[key] unsignedIntegerValue] + 1;
		g_connection_counts[key] = [NSNumber numberWithUnsignedInteger:count];
	}
	return count;
}

static BOOL
connection_event_is_printed(NSUInteger count)
{
	return count <= 20 || (count % 1000) == 0;
}

@interface AppHangNoService : NSObject <NSXPCListenerDelegate>
@property (nonatomic, copy) NSString *serviceName;
@end

@implementation AppHangNoService

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	BOOL refuse = getenv("APPHANG_REFUSE") != NULL;
	NSString *protocol_name = nil;
	Protocol *proto = nil;
	NSUInteger accepts;

	if (refuse) {
		accepts = count_connection_event(self.serviceName, "accept");
		if (connection_event_is_printed(accepts)) {
			apphanglog("%s: connection from pid %d refused (no service)",
			    self.serviceName.UTF8String, connection.processIdentifier);
		}
		return NO;
	}

	if ([self.serviceName rangeOfString:@"callcapabilities"].location != NSNotFound) {
		protocol_name = @"TUCallCapabilitiesXPCServer";
	} else if ([self.serviceName rangeOfString:@"callprovidermanager"].location != NSNotFound) {
		protocol_name = @"TUCallProviderManagerXPCServer";
	} else if ([self.serviceName rangeOfString:@"callstatecontroller"].location != NSNotFound) {
		protocol_name = @"TUCallCenterXPCServer";
	} else if ([self.serviceName rangeOfString:@"callhistorymanager"].location != NSNotFound) {
		protocol_name = @"TUCallHistoryManagerXPCServer";
	} else if ([self.serviceName rangeOfString:@"callhistorycontroller"].location != NSNotFound) {
		protocol_name = @"TUCallHistoryControllerXPCServer";
	} else if ([self.serviceName rangeOfString:@"conversationmanager"].location != NSNotFound) {
		protocol_name = @"TUConversationManagerXPCServer";
	} else if ([self.serviceName rangeOfString:@"momentscontroller"].location != NSNotFound) {
		protocol_name = @"TUMomentsControllerXPCServer";
	} else if ([self.serviceName rangeOfString:@"reportingcontroller"].location != NSNotFound) {
		protocol_name = @"TUReportingControllerXPCServer";
	} else if ([self.serviceName rangeOfString:@"usernotificationprovider"].location != NSNotFound) {
		protocol_name = @"TUUserNotificationsProviderXPCServer";
	} else if ([self.serviceName rangeOfString:@"neighborhood-activities"].location != NSNotFound) {
		protocol_name = @"TUNeighborhoodActivityConduitXPCServer";
	}
	if (protocol_name != nil) {
		proto = NSProtocolFromString(protocol_name);
	}

	@try {
		if (proto != nil) {
			NSXPCInterface *interface = [NSXPCInterface interfaceWithProtocol:proto];
			AppHangServer *server = [[AppHangServer alloc] init];
			__unsafe_unretained NSXPCConnection *watched = connection;
			NSString *service = self.serviceName;

			configure_argument_classes(interface);
			server.serviceName = self.serviceName;
			connection.exportedInterface = interface;
			connection.exportedObject = server;
			accepts = count_connection_event(service, "accept");
			if (connection_event_is_printed(accepts)) {
				apphanglog("%s: serving %s (pid %d)", service.UTF8String,
				    protocol_name.UTF8String, connection.processIdentifier);
			} else if (accepts == 21) {
				apphanglog("%s: more than 20 connections; further accepts and "
				    "invalidations are counted, not printed (every 1000th)",
				    service.UTF8String);
			}
			[connection setInvalidationHandler:^{
				NSUInteger invalidations =
				    count_connection_event(service, "invalidate");

				if (connection_event_is_printed(invalidations)) {
					apphanglog("%s: connection from pid %d invalidated",
					    service.UTF8String, watched.processIdentifier);
				}
			}];
		} else {
			/* No TU protocol for this name.  In d1081c3 an error-only interface made
			 * the clients reconnect 34 times a second; an absent service is cheaper. */
			accepts = count_connection_event(self.serviceName, "no-protocol");
			if (connection_event_is_printed(accepts)) {
				apphanglog("%s: no TU protocol; connection from pid %d refused",
				    self.serviceName.UTF8String, connection.processIdentifier);
			}
			return NO;
		}
		[connection resume];
		return YES;
	} @catch (NSException *e) {
		apphanglog("%s: exception while accepting pid %d: %s: %s",
		    self.serviceName.UTF8String, connection.processIdentifier,
		    e.name.UTF8String, e.reason.UTF8String);
		return NO;
	}
}

@end

static NSString *const kNoServiceNames[] = {
	/* Only names whose server protocol is in TelephonyUtilities and whose launch-time
	 * questions this daemon can answer with valid objects.  d1081c3 showed that an
	 * "error-only" name is worse than an absent one: the clients reconnect in a tight
	 * loop (36 113 accepts/35 433 invalidations for identityservicesd.embedded.auth in
	 * nine minutes).  Every other name is deliberately left to "No such process". */
	@"com.apple.telephonyutilities.callservicesdaemon.callcapabilities",
	@"com.apple.telephonyutilities.callservicesdaemon.callstatecontroller",
	@"com.apple.telephonyutilities.callservicesdaemon.callprovidermanager",
	@"com.apple.telephonyutilities.callservicesdaemon.conversationmanager",
	@"com.apple.telephonyutilities.callservicesdaemon.callhistorymanager",
	@"com.apple.telephonyutilities.callservicesdaemon.callhistorycontroller",
	@"com.apple.telephonyutilities.callservicesdaemon.momentscontroller",
	@"com.apple.telephonyutilities.callservicesdaemon.reportingcontroller",
	@"com.apple.telephonyutilities.callservicesdaemon.usernotificationprovider",
	@"com.apple.telephonyutilities.callservicesdaemon.neighborhood-activities",
};

int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSMutableArray *listeners = [NSMutableArray array];
		NSMutableArray *delegates = [NSMutableArray array];
		void *tu;

		apphanglog("start: pid %d, %u no-service names",
		    getpid(), (unsigned)(sizeof(kNoServiceNames) / sizeof(kNoServiceNames[0])));

		g_method_types = [NSMutableDictionary dictionary];
		g_connection_counts = [[NSMutableDictionary alloc] init];
		tu = dlopen("/System/Library/PrivateFrameworks/TelephonyUtilities.framework/TelephonyUtilities",
		    RTLD_NOW);
		apphanglog("TelephonyUtilities: %s", tu != NULL ? "loaded" : dlerror());
		if (tu != NULL) {
			const char *protocols[] = {
				"TUCallCapabilitiesXPCServer", "TUCallProviderManagerXPCServer",
				"TUCallCenterXPCServer", "TUCallHistoryManagerXPCServer",
				"TUCallHistoryControllerXPCServer", "TUConversationManagerXPCServer",
				"TUMomentsControllerXPCServer", "TUReportingControllerXPCServer",
				"TUUserNotificationsProviderXPCServer",
				"TUNeighborhoodActivityConduitXPCServer",
			};
			for (unsigned i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
				Protocol *p = NSProtocolFromString([NSString stringWithUTF8String:protocols[i]]);
				if (p != nil) {
					add_protocol_methods(p);
				} else {
					apphanglog("protocol %s: not found", protocols[i]);
				}
			}
		}
		apphanglog("protocol map: %lu selectors", (unsigned long)g_method_types.count);
		{
			const char *voice = voice_device_path().UTF8String;

			apphanglog("voice: %s %s; dial 0500 %s; hold %d s", voice,
			    access(voice, W_OK) == 0 ? "present and writable" :
			    "absent (a dial will fail fast, the reply still comes)",
			    getenv("APPHANG_DIAL_DRYRUN") != NULL ? "dry run" : "enabled",
			    dial_hold_seconds());
		}
		/* Fill the class set once, on this thread, before any listener can accept:
		 * NSXPC calls the delegates on its own queues and the set is read-only after. */
		apphanglog("argument classes: %lu", (unsigned long)telephony_classes().count);

		for (unsigned i = 0; i < sizeof(kNoServiceNames) / sizeof(kNoServiceNames[0]); i++) {
			NSString *name = kNoServiceNames[i];
			AppHangNoService *delegate = [[AppHangNoService alloc] init];
			NSXPCListener *listener = ((id (*)(id, SEL, id))objc_msgSend)([NSXPCListener alloc],
			    sel_registerName("initWithMachServiceName:"), name);

			delegate.serviceName = name;
			if (listener == nil) {
				apphanglog("%s: listener could not be created", name.UTF8String);
				continue;
			}
			listener.delegate = delegate;
			[delegates addObject:delegate];
			[listeners addObject:listener];
			[listener resume];
			apphanglog("%s: listener is up", name.UTF8String);
		}

		if (listeners.count == 0) {
			apphanglog("no listener could be created; exiting so launchd retries");
			return 1;
		}

		for (;;) {
			@autoreleasepool {
				[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
							 beforeDate:[NSDate dateWithTimeIntervalSinceNow:3600]];
			}
		}
	}
	return 0;
}
