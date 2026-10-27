/*
 * fp_enable: make the Files app's local provider ("On My iPhone", com.apple.FileProvider.LocalStorage) exist and be
 * enabled, and print every answer on the way.
 *
 * owner8 (2026-10-25): pkd hands fileproviderd all 8 file providers, LocalStorage among them, yet its domain stays
 * "(disabled)", the extension is never launched and the owner's toggle does nothing. fp_enable (15:18) called
 * setEnabled:YES for three guessed domain identifiers and got Cocoa 4 ("The file doesn't exist") each time.
 *
 * Why (FACT, FileProviderDaemon 16.5, /tmp/opencode/fpd.txt):
 *  - -[FPDXPCServicer setEnabled:forDomainIdentifier:providerIdentifier:completionHandler:] @0x1c3cc9968 hands the
 *    work to -[FPDProvider setEnabled:forDomainIdentifier:request:completionHandler:] @0x1c3cb2ce8, which looks the
 *    identifier up in the provider's domainsByID and answers NSFileNoSuchFileError (code 4) when it is absent
 *    (error built at 0x1c3cb28f8).
 *  - a domain is only ever created by -[FPDProvider _recreateDefaultDomainIfNeeded] @0x1c3c71e64, and the only
 *    callers that matter here are -[FPDProvider removeAllDomainsForRequest:completionHandler:] @0x1c3cb17f0
 *    (call at 0x1c3cb1bd8) and -[FPDProvider addDomain:...] @0x1c3cb0450. The default domain is built from
 *    -[FPDProvider defaultNSDomain] @0x1c3c6a078: identifier NSFileProviderDomainDefaultIdentifier, display name
 *    from the extension, pathRelativeToDocumentStorage empty, hidden = !defaultDomainVisible, and
 *    userEnabled = -[FPDProviderDescriptor enabledByDefault]; for LocalStorage that is YES
 *    (LocalStorageFileProvider.appex/Info.plist: NSExtensionFileProviderEnabledByDefault,
 *    NSFileProviderDomainDefaultEnabled). So one removeAllDomains of this provider (it has no domains, nothing is
 *    removed) makes the daemon create and start the default domain with no user election needed.
 *
 * The provider's document storage is the app group container + "File Provider Storage"
 * (media_import makes it; here it is made too, because the daemon starts the extension on the domain start).
 *
 * TEST7 (2026-10-27, receipt: TEST7 section of 2026-10-23-vayu-backup-fw-hwruns.md): "no answer from
 * fileproviderd in 20 s / no connection to fileproviderd", and the daemon was NOT at fault --
 * launchd's own lines say "(user/501/com.apple.FileProvider [120]) <Notice>: Successfully spawned
 * fileproviderd[120] because ipc (mach)" and "job state = running", fileproviderctl answered
 * "domain list" (exit 0, " - Local Storage (disabled)") and "dump" (exit 0, "8 providers") in the
 * same run, and /var/mobile/Library/Application Support/FileProvider/com.apple.FileProvider.LocalStorage
 * was made at 13:30. What failed was this program's first line of contact: wait_for_daemon() printed
 * only its "waiting ... (N s of 20)" lines, never "fileproviderd is not up yet" (which needs a
 * non-nil proxy), and ended in "no connection to fileproviderd" -- so proxy() was nil, i.e.
 * NSClassFromString(@"FPDaemonConnection") found nothing. That class is exported by
 * FileProvider.framework (/System/Library/Frameworks/FileProvider.framework/FileProvider:
 * _OBJC_CLASS_$_FPDaemonConnection, and the framework has synchronousSharedConnectionProxy,
 * getDomainsForProviderIdentifier:completionHandler:, removeAllDomainsForProviderIdentifier:
 * completionHandler:, setEnabled:forDomainIdentifier:providerIdentifier:completionHandler: and
 * providerDomainForIdentifier:completionHandler:), and build.sh linked Foundation only: the class is
 * not in the process unless the framework is. build.sh now links it; load_fileprovider() below is the
 * same thing at run time for a build that does not, and proxy() says which of the two it was.
 *
 * The other half of TEST7: the domain is listed and "(disabled)", and the daemon does not re-enable an
 * existing domain by itself -- _recreateDefaultDomainIfNeeded (FileProviderDaemon 0x1c3c71e64) sets
 * userEnabled = [descriptor enabledByDefault] only where it creates the default domain, and the FPDProvider
 * methods that call it are removeAllDomainsForProviderIdentifier: and addDomain:. An enable call on a
 * domain that is there but disabled answers NSFileNoSuchFileError when the daemon's domainsByID does not
 * have the identifier (0x1c3cb28f8, the error the 15:18 run saw), so --reset / a reset driven by the
 * domain's own state comes first, then setEnabled:YES, then the state is read back and is the verdict.
 *
 * TEST8 (2026-10-27, log artifacts/vayu/test8-20261027/run/121732-run.log; "SELFTEST: [+ 92.21 s] files:
 * fp_enable ... exit 1" at :94125): the daemon was NOT late, and launchd was not the problem. launchd spawned
 * fileproviderd[97] at 15:17:43.022659 -- "(user/501/com.apple.FileProvider [97]) <Notice>: Successfully
 * spawned fileproviderd[97] because ipc (mach)" (:13926) -- it answered `fileproviderctl domain list` with exit
 * 0 in the very second fp_enable gave up (:94317 " - Local Storage (disabled)"), and it lived until the
 * shutdown SIGKILL at 15:30:18 (:9663467). What failed was this program, again, in the one place the TEST7 fix
 * did not reach: the two calls that do all the work, getDomainsForProviderIdentifier: and
 * removeAllDomainsForProviderIdentifier:, stood behind [daemon respondsToSelector:], and the XPC proxy answers
 * that with NO -- it claims the selectors of the protocol its NSXPCInterface was built with, not the daemon's
 * method list. So the log shows "the daemon has no getDomainsForProviderIdentifier:" six times (:19341, 19982,
 * 20829, 21712, 22393, 22904, 23256) and "fileproviderd is not up yet (N s of 20)" for twenty seconds
 * (:94129-94164), while the same proxy, called without the guard, reached the daemon in the same run:
 * providerDomainForIdentifier: answered with a real <FPProvider> (:19337), and
 * setEnabled:forDomainIdentifier:providerIdentifier: answered with the daemon's own Cocoa 4 (:23272, error
 * built at FileProviderDaemon 0x1c3cb28f8). And because removeAllDomainsForProviderIdentifier: never went out,
 * the default domain was never created; the guessed identifier that followed had nothing to enable. Both
 * symptoms -- "the daemon is not up" and "the provider has no domain" -- are artifacts of that one guard.
 *
 * So this version:
 *  - reads the job's MachServices from its plist (mach_service_names), and, when the job is not running,
 *    connects to that MachService and sends one message: a job that is not running has launchd's demand port
 *    for its service, and the first message on it is what makes launchd spawn the job ("launching: ipc
 *    (mach)"). This is the "start it, do not wait for something else to start it" half (poke_launchd).
 *  - treats [proxy respondsToSelector:] as a line in the log, not as a decision (proxy_claims): the calls go
 *    out in @try/@catch, and a real "does not recognize selector" is reported and turns into nil.
 *  - --retry=SECONDS repeats the whole pass (probe, reset, enable, verify) until the deadline, so a domain or
 *    an extension that comes up late is still caught (the self-test asks for 120 s).
 *  - add_domain_public() is the public door to the same daemon -- +[NSFileProviderManager
 *    addDomain:completionHandler:], whose daemon-side addDomain:... is the other caller of
 *    _recreateDefaultDomainIfNeeded -- used only when the private calls have left no enabled domain.
 *
 * usage: fp_enable [--wait[=SECONDS]] [--retry[=SECONDS]] [--trace] [--check] [--reset] [PROVIDER_ID]
 *        --wait    wait up to SECONDS (default 30) for fileproviderd to answer before doing anything: the
 *                  unattended self-test calls this at +45 s and launchd may be starting the daemon on this
 *                  very first connection (the call itself makes the daemon run), so "no connection yet" is
 *                  a state to wait out, not a failure
 *        --retry   after a pass that left no enabled domain, wait 5 s and do the whole pass again, until
 *                  SECONDS (default 120) have gone by; the domain, its extension and the daemon's own state
 *                  are all allowed to settle late
 *        --check   look only (domains, provider); change nothing
 *        --reset   remove this provider's domains first even if there are some (LocalStorage only: its files stay
 *                  in the app group; the daemon re-creates the default domain)
 *        --restart[=SECONDS]  when a pass ends with no domain at all and no error, stop fileproviderd (SIGTERM,
 *                  SIGKILL after SECONDS, default 20) and make launchd spawn a new one, then repeat the pass:
 *                  a daemon that started before LaunchServices/pkd held the appex keeps a provider list that
 *                  answers -2001 for it (R14, TEST9) and only a new process reads the completed registry
 *        as mobile, from root too; signed with fileproviderctl's entitlements
 *
 * exit: 0 the provider has a domain whose userEnabled is YES and every enable call answered without an
 *          error (--check: a domain is enabled);
 *       1 no daemon, no domain after the reset, a domain that stayed disabled, or an error; 2 bad usage
 */
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#define MOBILE_ID	501

/*
 * libxpc, declared here instead of #include <xpc/xpc.h>: the theos SDK this tree cross-builds against
 * (iPhoneOS16.5.sdk) ships the Mach and Foundation headers but not xpc/, and these five calls are all this
 * program needs. The symbols are in libSystem (libSystem.tbd names xpc_connection_create_mach_service).
 */
typedef void *fp_xpc_connection_t;
typedef void *fp_xpc_object_t;
extern fp_xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t target_queue,
    uint64_t flags);
extern void xpc_connection_set_event_handler(fp_xpc_connection_t connection, void (^handler)(fp_xpc_object_t event));
extern void xpc_connection_resume(fp_xpc_connection_t connection);
extern void xpc_connection_cancel(fp_xpc_connection_t connection);
extern pid_t xpc_connection_get_pid(fp_xpc_connection_t connection);
extern fp_xpc_object_t xpc_dictionary_create(const void *keys, const void *values, size_t count);
extern void xpc_connection_send_message(fp_xpc_connection_t connection, fp_xpc_object_t message);

static double
now_seconds(void)
{
	struct timeval tv = { 0, 0 };

	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* An NSError whole: domain, code, every userInfo key (paths and URLs first), and the errors under it. */
static void
say_error(const char *what, NSError *error, unsigned depth)
{
	if (error == nil || depth > 6) {
		return;
	}
	fprintf(stderr, "fp_enable: %s: %*s%s %ld: %s\n", what, (int)depth * 2, "", [[error domain] UTF8String], (long)[error code],
	    [[error localizedDescription] UTF8String]);
	for (NSString *key in @[ NSFilePathErrorKey, NSURLErrorKey, @"NSFileProviderErrorItemKey", @"NSDebugDescription" ]) {
		id value = [error userInfo][key];
		if (value != nil) {
			fprintf(stderr, "fp_enable: %s: %*s  %s = %s\n", what, (int)depth * 2, "", [key UTF8String],
			    [[value description] UTF8String]);
		}
	}
	[[error userInfo] enumerateKeysAndObjectsUsingBlock:^(id key, id value, __unused BOOL *stop) {
		if (![key isEqual:NSUnderlyingErrorKey] && ![key isEqual:NSFilePathErrorKey] && ![key isEqual:NSURLErrorKey]) {
			fprintf(stderr, "fp_enable: %s: %*s  [%s] = %s\n", what, (int)depth * 2, "", [[key description] UTF8String],
			    [[[value description] substringToIndex:MIN((NSUInteger)300, [[value description] length])] UTF8String]);
		}
	}];
	say_error(what, [error userInfo][NSUnderlyingErrorKey], depth + 1);
}

/*
 * --trace: where in this process a Cocoa error 4 is made -- +[NSError errorWithDomain:code:userInfo:] and
 * -[NSError initWithDomain:code:userInfo:] wrapped for the run, each such error said with its userInfo and the
 * calling frames (an error that comes from fileproviderd over XPC is decoded here too, and says so by its frames).
 */
static id (*real_error_class)(id, SEL, NSErrorDomain, NSInteger, NSDictionary *);
static id (*real_error_init)(id, SEL, NSErrorDomain, NSInteger, NSDictionary *);

static void
note_error(NSErrorDomain domain, NSInteger code, NSDictionary *info)
{
	if (code == NSFileNoSuchFileError && [domain isEqualToString:NSCocoaErrorDomain]) {
		fprintf(stderr, "fp_enable: trace: Cocoa 4 made, userInfo %s\n", [[info description] UTF8String]);
		for (NSString *frame in [[NSThread callStackSymbols] subarrayWithRange:NSMakeRange(0, MIN((NSUInteger)14,
		    [[NSThread callStackSymbols] count]))]) {
			fprintf(stderr, "fp_enable: trace:   %s\n", [frame UTF8String]);
		}
	}
}

static id
traced_error_class(id cls, SEL sel, NSErrorDomain domain, NSInteger code, NSDictionary *info)
{
	note_error(domain, code, info);
	return real_error_class(cls, sel, domain, code, info);
}

static id
traced_error_init(id self, SEL sel, NSErrorDomain domain, NSInteger code, NSDictionary *info)
{
	note_error(domain, code, info);
	return real_error_init(self, sel, domain, code, info);
}

static void
trace_errors(void)
{
	Method m = class_getClassMethod([NSError class], @selector(errorWithDomain:code:userInfo:));
	Method i = class_getInstanceMethod([NSError class], @selector(initWithDomain:code:userInfo:));

	real_error_class = (void *)method_setImplementation(m, (IMP)traced_error_class);
	real_error_init = (void *)method_setImplementation(i, (IMP)traced_error_init);
	fprintf(stderr, "fp_enable: trace: NSError creation watched for Cocoa 4\n");
}

/*
 * FPDaemonConnection is in FileProvider.framework, and a program that links only Foundation does not have
 * it: NSClassFromString() answers nil, proxy() makes nothing, and every call says "no connection to
 * fileproviderd" while the daemon is up and answering fileproviderctl (TEST7). build.sh links the
 * framework (-framework FileProvider); this is the belt for a build that does not, and it says so.
 */
static void
load_fileprovider(void)
{
	static const char *const paths[] = {
		"/System/Library/Frameworks/FileProvider.framework/FileProvider",
		"/System/Library/PrivateFrameworks/FileProviderDaemon.framework/FileProviderDaemon",
	};

	if (NSClassFromString(@"FPDaemonConnection") != Nil) {
		return;
	}
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL) == NULL) {
			fprintf(stderr, "fp_enable: dlopen %s: %s\n", paths[i], dlerror());
		} else {
			fprintf(stderr, "fp_enable: loaded %s\n", paths[i]);
		}
	}
}

/*
 * The job's MachServices, from its own plist: the system image's copy and the one this image boots with
 * (/redwood/LaunchDaemons, the directory launchd is pointed at here) describe the same job. The label's own
 * name comes first, because that is the service the daemon listens on; the rest are its side services.
 * nil when neither plist can be read: the trigger is then skipped, not guessed.
 */
static NSArray *
mach_service_names(NSString **out_label)
{
	static const char *const paths[] = {
		"/System/Library/LaunchDaemons/com.apple.FileProvider.plist",
		"/redwood/LaunchDaemons/com.apple.FileProvider.plist",
	};
	NSArray *names = nil;

	if (out_label != NULL) {
		*out_label = nil;
	}
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		NSDictionary *job = [NSDictionary dictionaryWithContentsOfFile:[NSString stringWithUTF8String:paths[i]]];
		NSDictionary *services;
		NSString *label;

		if (job == nil) {
			fprintf(stderr, "fp_enable: plist %s: not readable\n", paths[i]);
			continue;
		}
		label = [job objectForKey:@"Label"];
		services = [job objectForKey:@"MachServices"];
		fprintf(stderr, "fp_enable: plist %s: label \"%s\", MachServices %s\n", paths[i],
		    label ? [label UTF8String] : "(none)",
		    services ? [[[services allKeys] componentsJoinedByString:@", "] UTF8String] : "(none)");
		if (names == nil && [services isKindOfClass:[NSDictionary class]] && [services count] > 0) {
			NSMutableArray *ordered = [NSMutableArray array];

			if (label != nil && [services objectForKey:label] != nil) {
				[ordered addObject:label];
			}
			for (NSString *name in [[services allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
				if (![name isEqual:label]) {
					[ordered addObject:name];
				}
			}
			names = ordered;
			if (out_label != NULL) {
				*out_label = label;
			}
		}
	}
	return names;
}

/*
 * fileproviderd's pid through the kernel's process table: 0 when it is not running, -1 when the table cannot
 * be read. This is the "is it up" the old wait loop asked by proxy instead of by process; a daemon that is
 * already running is a different situation from one launchd has yet to spawn, and the log should say which.
 */
static int
daemon_processes(pid_t *first_pid)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0, count;
	char *buffer;
	int found = 0;

	if (first_pid != NULL) {
		*first_pid = 0;
	}
	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || size == 0) {
		return -1;
	}
	if ((buffer = malloc(size)) == NULL) {
		return -1;
	}
	if (sysctl(mib, 3, buffer, &size, NULL, 0) != 0) {
		free(buffer);
		return -1;
	}
	count = size / sizeof(struct kinfo_proc);
	for (size_t i = 0; i < count; i++) {
		struct kinfo_proc *process = (struct kinfo_proc *)buffer + i;

		if (strncmp(process->kp_proc.p_comm, "fileproviderd", sizeof(process->kp_proc.p_comm)) == 0) {
			if (found == 0 && first_pid != NULL) {
				*first_pid = process->kp_proc.p_pid;
			}
			found++;
		}
	}
	free(buffer);
	return found;
}

static void
say_daemon_state(const char *when)
{
	pid_t pid = 0;
	int count = daemon_processes(&pid);

	if (count < 0) {
		fprintf(stderr, "fp_enable: %s: fileproviderd: the process table is not readable (%s)\n", when,
		    strerror(errno));
	} else if (count == 0) {
		fprintf(stderr, "fp_enable: %s: fileproviderd: not running (launchd starts it on demand)\n", when);
	} else {
		fprintf(stderr, "fp_enable: %s: fileproviderd: running as pid %d (%d process(es))\n", when, (int)pid,
		    count);
	}
}

/*
 * Any process whose comm is `name', the way daemon_processes() looks for fileproviderd. Needed for the two
 * processes a -2001 is usually blamed on: pkd (the app-extension registry) and the provider's own extension
 * (comm "LocalStorageFil", launchd truncates to 15 characters).
 */
static int
named_processes(const char *name, pid_t *first_pid)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0, count;
	char *buffer;
	int found = 0;

	if (first_pid != NULL) {
		*first_pid = 0;
	}
	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || size == 0) {
		return -1;
	}
	if ((buffer = malloc(size)) == NULL) {
		return -1;
	}
	if (sysctl(mib, 3, buffer, &size, NULL, 0) != 0) {
		free(buffer);
		return -1;
	}
	count = size / sizeof(struct kinfo_proc);
	for (size_t i = 0; i < count; i++) {
		struct kinfo_proc *process = (struct kinfo_proc *)buffer + i;

		if (strncmp(process->kp_proc.p_comm, name, sizeof(process->kp_proc.p_comm)) == 0) {
			if (found == 0 && first_pid != NULL) {
				*first_pid = process->kp_proc.p_pid;
			}
			found++;
		}
	}
	free(buffer);
	return found;
}

/*
 * What a -2001 is usually blamed on, printed instead of assumed (R14). Four facts, in the order a failure
 * needs them: pkd (the app-extension registry fileproviderd reads), the appex on disk, the executable its
 * own Info.plist names, and the provider's extension process if one is running.
 *
 * The third line is not academic: the iOS 16.5 image we build /System from ships
 * LocalStorageFileProvider.appex with Info.plist, InfoPlist.loctable and _CodeSignature only -- there is no
 * LocalStorageFileProvider binary in the appex, and none in the dyld shared cache either (the cache's
 * imagesText has no appex at all). TEST9's launchd log nevertheless shows "(pid/285 [LocalStorageFil])
 * extension checked in as alive", so the device's root has a binary this tree's does not; printing both
 * paths is what tells the two apart on the next run instead of guessing.
 */
static void
say_registration_state(const char *appex)
{
	static const char *const suspects[] = { "pkd", "LocalStorageFil", "fileproviderd" };
	NSFileManager *files = [NSFileManager defaultManager];
	NSString *bundle = [NSString stringWithUTF8String:appex];
	NSString *plist = [bundle stringByAppendingPathComponent:@"Info.plist"];
	NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:plist];

	for (size_t i = 0; i < sizeof(suspects) / sizeof(suspects[0]); i++) {
		pid_t pid = 0;
		int count = named_processes(suspects[i], &pid);

		if (count < 0) {
			fprintf(stderr, "fp_enable: registry: %s: the process table is not readable (%s)\n",
			    suspects[i], strerror(errno));
		} else if (count == 0) {
			fprintf(stderr, "fp_enable: registry: %s: not running\n", suspects[i]);
		} else {
			fprintf(stderr, "fp_enable: registry: %s: running as pid %d (%d process(es))\n",
			    suspects[i], (int)pid, count);
		}
	}
	fprintf(stderr, "fp_enable: registry: appex %s: %s\n", appex,
	    [files fileExistsAtPath:plist] ? "Info.plist is there" : "NO Info.plist");
	if (info != nil) {
		NSString *executable = info[@"CFBundleExecutable"];
		NSString *path = executable ? [bundle stringByAppendingPathComponent:executable] : nil;
		NSDictionary *extension = info[@"NSExtension"];

		fprintf(stderr, "fp_enable: registry:   CFBundleIdentifier = %s\n",
		    [info[@"CFBundleIdentifier"] UTF8String] ?: "(none)");
		fprintf(stderr, "fp_enable: registry:   NSExtensionPointIdentifier = %s\n",
		    [extension[@"NSExtensionPointIdentifier"] UTF8String] ?: "(none)");
		fprintf(stderr, "fp_enable: registry:   NSExtensionFileProviderEnabledByDefault = %s\n",
		    [extension[@"NSExtensionFileProviderEnabledByDefault"] boolValue] ? "true" : "false/absent");
		fprintf(stderr, "fp_enable: registry:   executable %s: %s\n", executable ? [executable UTF8String] : "(none)",
		    path == nil ? "no CFBundleExecutable" :
		    [files fileExistsAtPath:path] ? "is there" : "IS MISSING (the extension cannot be exec'd)");
	}
}

/*
 * The trigger. A job with MachServices that is not running has launchd's demand port registered under the
 * service name: a client's lookup gets a send right to it, and the first message sent there is what makes
 * launchd spawn the job ("launching: ipc (mach)", then "Successfully spawned fileproviderd[NN] because ipc
 * (mach)"). The message is an empty dictionary; fileproviderd's listener is an NSXPCListener, which drops a
 * dictionary that is not an NSXPC request, so no request of its own is disturbed -- and it is only sent while
 * the process table says the job is not running, so a daemon that is up never sees it.
 */
static BOOL
poke_launchd(NSString *service)
{
	__block BOOL saw_event = NO;
	fp_xpc_connection_t connection;
	fp_xpc_object_t message;
	pid_t pid;

	if (service == nil || [service length] == 0) {
		fprintf(stderr, "fp_enable: trigger: no MachService name to ask launchd for\n");
		return NO;
	}
	pid = 0;
	(void)daemon_processes(&pid);
	if (pid > 0) {
		fprintf(stderr, "fp_enable: trigger: fileproviderd is running as pid %d, nothing to wake\n", (int)pid);
		return NO;
	}
	connection = xpc_connection_create_mach_service([service UTF8String], NULL, 0);
	if (connection == NULL) {
		fprintf(stderr, "fp_enable: trigger: xpc_connection_create_mach_service(%s) answered NULL\n",
		    [service UTF8String]);
		return NO;
	}
	xpc_connection_set_event_handler(connection, ^(fp_xpc_object_t event) {
		(void)event;
		saw_event = YES;
	});
	xpc_connection_resume(connection);
	usleep(200000);
	fprintf(stderr, "fp_enable: trigger: a connection to MachService %s is up (peer pid %d), sending one "
	    "message so launchd spawns the job\n", [service UTF8String], (int)xpc_connection_get_pid(connection));
	message = xpc_dictionary_create(NULL, NULL, 0);
	xpc_connection_send_message(connection, message);
	usleep(300000);
	pid = 0;
	(void)daemon_processes(&pid);
	fprintf(stderr, "fp_enable: trigger: after the message: fileproviderd %s%s\n",
	    pid > 0 ? "is running, pid " : "is still not in the process table",
	    pid > 0 ? [[NSString stringWithFormat:@"%d", (int)pid] UTF8String] : "");
	if (saw_event) {
		fprintf(stderr, "fp_enable: trigger: the connection got an event (a peer, or launchd refusing)\n");
	}
	xpc_connection_cancel(connection);
	return pid > 0;
}

/*
 * The restart, and why it is here (R14, FACT from TEST9's launchd log):
 *
 *   - 16:57:57  media_import: "extension com.apple.FileProvider.LocalStorage: NOT FOUND ((null))"  (the
 *               appex is not in LaunchServices/pkd yet)
 *   - 16:58:16  launchd: "Successfully spawned fileproviderd[121] because ipc (mach)"                (the
 *               daemon starts and builds its FPDProviderDescriptor list from that incomplete registry)
 *   - 16:58:18-20  /redwood/bin/ls_rebuild: "rebuilding (system, internal, user)", "rebuild says 1";
 *               after it "com.apple.fileprovider-nonui: 8 found", LocalStorage among them
 *
 * A daemon that starts before the registry is complete is the one case the on-demand re-sync does not
 * cover by itself: -[FPDXPCServicer ...] only re-syncs when a lookup already failed ("No provider found
 * with identifier %@ for calling bundle %@. Syncing with LS and retrying.", 0x1c3c7123c; "... on second
 * attempt.", 0x1c3c71300; -[FPDExtensionManager forceSynchronousProviderUpdate]), and the provider
 * registry it then builds is the one the daemon was born with for everything else, including the
 * startup step that creates the default domain from -[FPDProviderDescriptor enabledByDefault]. Stopping
 * the process is the cheap way to make it read the registry again: launchd spawns the job on the next
 * connection, which poke_launchd() makes, and the new process starts against a complete LaunchServices.
 */
static BOOL
restart_daemon(NSString *service, double seconds)
{
	pid_t pid = 0, old_pid;
	int tries;

	(void)daemon_processes(&pid);
	if (pid <= 0) {
		fprintf(stderr, "fp_enable: restart: fileproviderd is not running, waking it instead\n");
		return poke_launchd(service);
	}
	old_pid = pid;
	fprintf(stderr, "fp_enable: restart: stopping fileproviderd pid %d so that it reads the "
	    "LaunchServices/pkd registry again\n", (int)old_pid);
	if (kill(old_pid, SIGTERM) != 0) {
		fprintf(stderr, "fp_enable: restart: kill(%d, SIGTERM): %s\n", (int)old_pid, strerror(errno));
	}
	for (tries = 0; tries < (int)(seconds * 10.0); tries++) {
		pid = 0;
		(void)daemon_processes(&pid);
		if (pid != old_pid) {
			break;
		}
		usleep(100000);
	}
	if (pid == old_pid) {
		fprintf(stderr, "fp_enable: restart: pid %d is still there after %.0f s, SIGKILL\n",
		    (int)old_pid, seconds);
		(void)kill(old_pid, SIGKILL);
		for (tries = 0; tries < 50; tries++) {
			pid = 0;
			(void)daemon_processes(&pid);
			if (pid != old_pid) {
				break;
			}
			usleep(100000);
		}
	}
	if (pid == old_pid) {
		fprintf(stderr, "fp_enable: restart: fileproviderd pid %d did not go away\n", (int)old_pid);
		return NO;
	}
	if (pid > 0) {
		/* launchd's job came back by itself (KeepAlive): nothing to poke, and poke_launchd() would
		 * answer NO for a daemon that is already up, which is not a failure */
		fprintf(stderr, "fp_enable: restart: launchd already started a new fileproviderd, pid %d\n",
		    (int)pid);
		return YES;
	}
	fprintf(stderr, "fp_enable: restart: the old daemon is gone, asking launchd for a new one\n");
	if (!poke_launchd(service)) {
		fprintf(stderr, "fp_enable: restart: launchd did not start fileproviderd again\n");
		return NO;
	}
	for (tries = 0; tries < (int)(seconds * 10.0); tries++) {
		pid = 0;
		if (daemon_processes(&pid) > 0 && pid != old_pid) {
			fprintf(stderr, "fp_enable: restart: fileproviderd is running again as pid %d\n",
			    (int)pid);
			return YES;
		}
		usleep(100000);
	}
	fprintf(stderr, "fp_enable: restart: no new fileproviderd in %.0f s\n", seconds);
	return NO;
}

static id
proxy(void)
{
	Class connection = NSClassFromString(@"FPDaemonConnection");
	id shared;

	if (connection == Nil) {
		fprintf(stderr, "fp_enable: FPDaemonConnection is not in this process: FileProvider.framework is not "
		    "loaded (build with -framework FileProvider)\n");
		return nil;
	}
	if ([connection respondsToSelector:@selector(synchronousSharedConnectionProxy)]) {
		return [connection performSelector:@selector(synchronousSharedConnectionProxy)];
	}
	if (![connection respondsToSelector:@selector(sharedConnection)]) {
		fprintf(stderr, "fp_enable: FPDaemonConnection has neither synchronousSharedConnectionProxy nor "
		    "sharedConnection\n");
		return nil;
	}
	shared = [connection performSelector:@selector(sharedConnection)];
	if (![shared respondsToSelector:@selector(remoteObjectProxyWithErrorHandler:)]) {
		fprintf(stderr, "fp_enable: the shared FPDaemonConnection has no remoteObjectProxyWithErrorHandler:\n");
		return nil;
	}
	return ((id (*)(id, SEL, id))objc_msgSend)(shared, @selector(remoteObjectProxyWithErrorHandler:), ^(NSError *error) {
		fprintf(stderr, "fp_enable: connection: %s\n", [[error description] UTF8String]);
	});
}

/*
 * The object proxy() hands out is an NSXPC remote proxy: its -respondsToSelector: claims the selectors of the
 * protocol its NSXPCInterface was built with, not the daemon's method list. TEST8 (:19341, :19982 ...) is
 * what that costs: "the daemon has no getDomainsForProviderIdentifier:" six times, twenty seconds of
 * "fileproviderd is not up yet", and no reset -- while providerDomainForIdentifier:, called two lines earlier
 * without a guard, answered with a real <FPProvider> (:19337). So a NO here is worth a line in the log and
 * nothing else: the call goes out in @try, and a real "does not recognize selector" is caught by the caller.
 */
static void
proxy_claims(id daemon, SEL selector, const char *what)
{
	@try {
		if (![daemon respondsToSelector:selector]) {
			fprintf(stderr, "fp_enable: the proxy says it does not answer %s; sending it anyway (its "
			    "-respondsToSelector: is not the daemon's method list, TEST8)\n", what);
		}
	} @catch (NSException *exception) {
		fprintf(stderr, "fp_enable: the proxy raised on respondsToSelector %s: %s\n", what,
		    [[exception reason] UTF8String]);
	}
}

/* A domain's identifier: the public NSFileProviderDomain answers "identifier"; a daemon-side object may use another. */
static NSString *
domain_identifier(id domain)
{
	for (NSString *key in @[ @"identifier", @"domainID", @"providerDomainID" ]) {
		if (![domain respondsToSelector:NSSelectorFromString(key)]) {
			continue;
		}
		id value = [domain valueForKey:key];
		if ([value isKindOfClass:[NSString class]] && [value length] > 0) {
			return value;
		}
	}
	return nil;
}

static void
show_domain(id domain, const char *when, NSUInteger index)
{
	NSString *identifier = domain_identifier(domain);

	fprintf(stderr, "fp_enable: %s: domain %lu: %s\n", when, (unsigned long)index,
	    [[domain description] UTF8String]);
	fprintf(stderr, "fp_enable: %s:   identifier = %s\n", when, identifier ? [identifier UTF8String] : "(none)");
	for (NSString *key in @[ @"displayName", @"isEnabled", @"userEnabled", @"isHidden", @"isHiddenByUser", @"pathRelativeToDocumentStorage" ]) {
		if ([domain respondsToSelector:NSSelectorFromString(key)]) {
			id value = [domain valueForKey:key];
			fprintf(stderr, "fp_enable: %s:   %s = %s\n", when, [key UTF8String], [[value description] UTF8String]);
		}
	}
}

/* The state that decides "(disabled)" in `fileproviderctl domain list`: the domain's userEnabled
 * (NSFileProviderDomain, iOS 16.5), or the daemon-side object's isEnabled. -1: neither answers. */
static int
domain_enabled(id domain)
{
	for (NSString *key in @[ @"userEnabled", @"isEnabled" ]) {
		if (![domain respondsToSelector:NSSelectorFromString(key)]) {
			continue;
		}
		id value = [domain valueForKey:key];
		if ([value respondsToSelector:@selector(boolValue)]) {
			return [value boolValue] ? 1 : 0;
		}
	}
	return -1;
}

static int
enabled_count(NSArray *domains)
{
	int enabled = 0;

	for (NSUInteger i = 0; i < [domains count]; i++) {
		if (domain_enabled(domains[i]) == 1) {
			enabled++;
		}
	}
	return enabled;
}

/* The provider as fileproviderd describes it (the call fileproviderctl dump uses). */
static void
show_provider(id daemon, NSString *provider, const char *when)
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);

	@try {
		((void (*)(id, SEL, id, id))objc_msgSend)(daemon, @selector(providerDomainForIdentifier:completionHandler:),
		    provider, ^(id domain, NSError *error) {
			fprintf(stderr, "fp_enable: %s: provider %s%s%s\n", when, domain ? [[domain description] UTF8String] : "(none)",
			    error ? ", error " : "", error ? [[error description] UTF8String] : "");
			for (NSString *key in @[ @"isEnabled", @"userEnabled", @"isHidden", @"identifier", @"domainID" ]) {
				if ([domain respondsToSelector:NSSelectorFromString(key)]) {
					id value = [domain valueForKey:key];
					fprintf(stderr, "fp_enable: %s:   %s = %s\n", when, [key UTF8String], [[value description] UTF8String]);
				}
			}
			dispatch_semaphore_signal(done);
		});
	} @catch (NSException *exception) {
		fprintf(stderr, "fp_enable: %s: providerDomainForIdentifier: raised %s: %s\n", when,
		    [[exception name] UTF8String], [[exception reason] UTF8String]);
		return;
	}
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
		fprintf(stderr, "fp_enable: %s: no answer in 10 s\n", when);
	}
}

/*
 * -[FPDXPCServicer getDomainsForProviderIdentifier:completionHandler:] (0x1c3cc9464) answers its completion with
 * (identifier, domains, error) -- three arguments, checked in the daemon's block invoke at 0x1c3cc9714.
 */
static NSArray *
get_domains_timeout(id daemon, NSString *provider, NSError **out_error, double seconds)
{
	__block NSArray *result = nil;
	__block NSError *failure = nil;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);

	if (out_error != NULL) {
		*out_error = nil;
	}
	proxy_claims(daemon, @selector(getDomainsForProviderIdentifier:completionHandler:),
	    "getDomainsForProviderIdentifier:completionHandler:");
	@try {
		((void (*)(id, SEL, id, id))objc_msgSend)(daemon, @selector(getDomainsForProviderIdentifier:completionHandler:),
		    provider, ^(NSString *identifier, NSArray *domains, NSError *error) {
			/* The daemon invokes this with (identifier, domains, error) -- three arguments, checked at
			 * 0x1c3cc9714 -- but the ABI would hand us (domains, error) just as happily if the protocol ever
			 * declared two; take whichever of the first two is the array. */
			if ([identifier isKindOfClass:[NSArray class]]) {
				result = (NSArray *)identifier;
				failure = (NSError *)domains;
			} else {
				result = domains;
				failure = error;
			}
			dispatch_semaphore_signal(done);
		});
	} @catch (NSException *exception) {
		fprintf(stderr, "fp_enable: getDomains: the call raised %s: %s (it did not reach the daemon)\n",
		    [[exception name] UTF8String], [[exception reason] UTF8String]);
		return nil;
	}
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC))) != 0) {
		fprintf(stderr, "fp_enable: getDomains: no answer in %.0f s (sent; the daemon did not answer)\n", seconds);
	}
	if (out_error != NULL) {
		*out_error = failure;
	}
	if (result != nil && ![result isKindOfClass:[NSArray class]]) {
		fprintf(stderr, "fp_enable: getDomains: the answer is %s, not an array: %s\n",
		    [[result class] description].UTF8String, [[result description] UTF8String]);
		result = nil;
	}
	return result;
}

static NSArray *
get_domains(id daemon, NSString *provider, NSError **out_error)
{
	return get_domains_timeout(daemon, provider, out_error, 10.0);
}

/* A dead connection (fileproviderd not started yet, or restarted under us), not the daemon's answer. */
static BOOL
connection_error(NSError *error)
{
	return error != nil && [[error domain] isEqualToString:NSCocoaErrorDomain] && [error code] == 4099;
}

/*
 * --wait: fileproviderd answering is not the same as the proxy existing -- the synchronous proxy is made
 * whether or not the daemon runs, and the first call is what makes launchd start it (media_import's
 * fileproviderctl saw Cocoa 4099 right after that spawn, at +45 s of TEST5Q). So the probe is the daemon
 * answering getDomainsForProviderIdentifier: with a short timeout, retried until SECONDS pass; when the
 * process table says the job is not running, the job's own MachService is woken first (poke_launchd), which
 * is what a daemon started by somebody else's first request would have done for us.
 */
static id
wait_for_daemon(NSString *provider, double seconds, NSString *service)
{
	double start = now_seconds(), noted = 0, poked_at = -1;
	NSError *error = nil;
	NSArray *domains;
	id daemon;
	BOOL poked = NO;

	for (;;) {
		pid_t pid = 0;
		int running = daemon_processes(&pid);

		daemon = proxy();
		if (daemon != nil) {
			error = nil;
			domains = get_domains_timeout(daemon, provider, &error, 2.0);
			/* an answer is an array (even an empty one) or an error that is the daemon's own;
			 * a timeout leaves both nil, and a dead connection is Cocoa 4099 */
			if (domains != nil || (error != nil && !connection_error(error))) {
				fprintf(stderr, "fp_enable: fileproviderd answered after %.1f s%s%s (%lu domain(s))\n",
				    now_seconds() - start, error ? ", error " : "",
				    error ? [[error description] UTF8String] : "", (unsigned long)[domains count]);
				return daemon;
			}
			fprintf(stderr, "fp_enable: fileproviderd is not up yet (%.1f s of %.0f)%s\n",
			    now_seconds() - start, seconds,
			    error != nil ? [[NSString stringWithFormat:@", %@", [error description]] UTF8String] : "");
		}
		/* not running, no answer, and the first real call did not wake it: ask launchd for the job's
		 * MachService, which is the door launchd watches. Once per run. */
		if (!poked && running == 0 && now_seconds() - start > 3.0) {
			poke_launchd(service);
			poked = YES;
			poked_at = now_seconds() - start;
		}
		if (now_seconds() - start >= seconds) {
			fprintf(stderr, "fp_enable: no answer from fileproviderd in %.0f s%s%s\n", seconds,
			    poked ? ", after the MachService trigger at " : "",
			    poked ? [[NSString stringWithFormat:@"%.1f s", poked_at] UTF8String] : "");
			say_daemon_state("at the deadline");
			return daemon;
		}
		if (now_seconds() - start >= noted) {
			fprintf(stderr, "fp_enable: waiting for fileproviderd (%.0f s of %.0f)\n",
			    now_seconds() - start, seconds);
			noted += 5.0;
		}
		usleep(500000);
	}
}

/*
 * -[FPDXPCServicer removeAllDomainsForProviderIdentifier:completionHandler:] (0x1c3cc8ec0) ->
 * -[FPDProvider removeAllDomainsForRequest:completionHandler:] (0x1c3cb17f0), which removes the provider's domain
 * objects, then calls _recreateDefaultDomainIfNeeded and starts the new default domain. Completion: (NSError *).
 */
static NSError *
remove_all_domains(id daemon, NSString *provider, BOOL *out_sent)
{
	__block NSError *failure = nil;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);

	if (out_sent != NULL) {
		*out_sent = NO;
	}
	proxy_claims(daemon, @selector(removeAllDomainsForProviderIdentifier:completionHandler:),
	    "removeAllDomainsForProviderIdentifier:completionHandler:");
	@try {
		((void (*)(id, SEL, id, id))objc_msgSend)(daemon,
		    @selector(removeAllDomainsForProviderIdentifier:completionHandler:), provider, ^(NSError *error) {
			failure = error;
			dispatch_semaphore_signal(done);
		});
	} @catch (NSException *exception) {
		fprintf(stderr, "fp_enable: removeAllDomains: the call raised %s: %s (it did not reach the daemon; "
		    "the default domain cannot be created this way)\n", [[exception name] UTF8String],
		    [[exception reason] UTF8String]);
		return nil;
	}
	if (out_sent != NULL) {
		*out_sent = YES;
	}
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) {
		fprintf(stderr, "fp_enable: removeAllDomains: no answer in 30 s (sent)\n");
	}
	return failure;
}

static int
set_enabled(id daemon, NSString *provider, NSString *domain)
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	__block int failed = 0;

	proxy_claims(daemon, @selector(setEnabled:forDomainIdentifier:providerIdentifier:completionHandler:),
	    "setEnabled:forDomainIdentifier:providerIdentifier:completionHandler:");
	@try {
		((void (*)(id, SEL, BOOL, id, id, id))objc_msgSend)(daemon,
		    @selector(setEnabled:forDomainIdentifier:providerIdentifier:completionHandler:), YES, domain, provider,
		    ^(NSError *error) {
			fprintf(stderr, "fp_enable: setEnabled YES, domain \"%s\": %s\n", [domain UTF8String],
			    error ? [[error description] UTF8String] : "done");
			say_error("setEnabled error", error, 0);
			failed = error != nil;
			dispatch_semaphore_signal(done);
		});
	} @catch (NSException *exception) {
		fprintf(stderr, "fp_enable: setEnabled, domain \"%s\": %s\n", [domain UTF8String], [[exception reason] UTF8String]);
		return 1;
	}
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
		fprintf(stderr, "fp_enable: setEnabled, domain \"%s\": no answer in 10 s\n", [domain UTF8String]);
		return 1;
	}
	return failed;
}

/*
 * The public door to the same daemon: +[NSFileProviderManager addDomain:completionHandler:] goes through
 * FPDaemonConnection with the interface Apple built for it, and the daemon's addDomain:forProviderIdentifier:
 * path is the other caller of _recreateDefaultDomainIfNeeded (0x1c3c71e64) -- the same place the reset lands.
 * Used only when the private calls have left no enabled domain: an "already exists" answer is not a failure,
 * and a public API needs no protocol of ours to be guessed right.
 */
static void
add_domain_public(NSString *provider, NSString *display_name)
{
	Class manager = NSClassFromString(@"NSFileProviderManager");
	Class domain_class = NSClassFromString(@"NSFileProviderDomain");
	dispatch_semaphore_t done;
	id domain;

	if (manager == Nil || domain_class == Nil) {
		fprintf(stderr, "fp_enable: NSFileProviderManager/NSFileProviderDomain are not in this process "
		    "(FileProvider.framework not loaded)\n");
		return;
	}
	if (![manager respondsToSelector:@selector(addDomain:completionHandler:)]) {
		fprintf(stderr, "fp_enable: NSFileProviderManager has no +addDomain:completionHandler:\n");
		return;
	}
	domain = ((id (*)(id, SEL, id, id))objc_msgSend)([domain_class alloc],
	    @selector(initWithIdentifier:displayName:), @"NSFileProviderDomainDefaultIdentifier", display_name);
	if (domain == nil) {
		fprintf(stderr, "fp_enable: NSFileProviderDomain -initWithIdentifier:displayName: answered nil\n");
		return;
	}
	done = dispatch_semaphore_create(0);
	fprintf(stderr, "fp_enable: public: +[NSFileProviderManager addDomain:%s] for %s\n",
	    [display_name UTF8String], [provider UTF8String]);
	@try {
		((void (*)(id, SEL, id, id))objc_msgSend)(manager, @selector(addDomain:completionHandler:), domain,
		    ^(NSError *error) {
			fprintf(stderr, "fp_enable: public: addDomain: %s\n",
			    error ? [[error description] UTF8String] : "done");
			say_error("public addDomain error", error, 0);
			dispatch_semaphore_signal(done);
		});
	} @catch (NSException *exception) {
		fprintf(stderr, "fp_enable: public: addDomain: raised %s: %s\n", [[exception name] UTF8String],
		    [[exception reason] UTF8String]);
		return;
	}
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) != 0) {
		fprintf(stderr, "fp_enable: public: addDomain: no answer in 20 s\n");
	}
}

/*
 * The app group's container without the app-group entitlement: the container manager writes each shared
 * container's group into .com.apple.mobile_container_manager.metadata.plist, which is what
 * -containerURLForSecurityApplicationGroupIdentifier: reads after the entitlement check. fp_enable is
 * signed with fileproviderctl's entitlements, and they name no app group, so the API answers nil while
 * the container is right there (TEST7: "group group.com.apple.FileProvider.LocalStorage: (none)").
 */
static NSString *
group_container_path(NSString *group)
{
	NSFileManager *manager = [NSFileManager defaultManager];
	NSString *root = @"/private/var/mobile/Containers/Shared/AppGroup";
	NSString *metadata = @".com.apple.mobile_container_manager.metadata.plist";
	NSArray *entries = [manager contentsOfDirectoryAtPath:root error:NULL];

	for (NSString *entry in entries) {
		NSString *path = [root stringByAppendingPathComponent:entry];
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:
		    [path stringByAppendingPathComponent:metadata]];

		if ([group isEqual:[info objectForKey:@"MCMMetadataIdentifier"]]) {
			return path;
		}
	}
	return nil;
}

/* The LocalStorage extension's document storage: app group + "File Provider Storage" (media_import does the same). */
static void
ensure_storage(void)
{
	NSFileManager *manager = [NSFileManager defaultManager];
	NSURL *container = [manager containerURLForSecurityApplicationGroupIdentifier:
	    @"group.com.apple.FileProvider.LocalStorage"];
	NSError *error = nil;
	NSString *directory;

	fprintf(stderr, "fp_enable: group group.com.apple.FileProvider.LocalStorage: %s\n",
	    container ? [[container path] UTF8String] : "(none)");
	if (container == nil) {
		/* fileproviderctl's entitlements, which this program is signed with, do not name the group:
		 * the container manager's own metadata does, so read the directory's identity instead. */
		container = [NSURL fileURLWithPath:group_container_path(@"group.com.apple.FileProvider.LocalStorage")];
		fprintf(stderr, "fp_enable: group group.com.apple.FileProvider.LocalStorage (by metadata): %s\n",
		    container ? [[container path] UTF8String] : "(not found)");
	}
	if (container == nil || [[container path] length] == 0) {
		fprintf(stderr, "fp_enable: group group.com.apple.FileProvider.LocalStorage: (none)\n");
		return;
	}
	directory = [[container path] stringByAppendingPathComponent:@"File Provider Storage"];
	if (![manager fileExistsAtPath:directory]) {
		if ([manager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error]) {
			fprintf(stderr, "fp_enable: made %s\n", [directory UTF8String]);
		} else {
			fprintf(stderr, "fp_enable: %s: %s\n", [directory UTF8String], [[error description] UTF8String]);
		}
	} else {
		fprintf(stderr, "fp_enable: %s is there\n", [directory UTF8String]);
	}
}

/* What one pass leaves behind: the domain count, how many of them are enabled, and how many enable calls
 * answered with an error. The caller decides whether another pass is worth it (--retry). */
struct pass_result {
	NSUInteger	domains;
	int		enabled;
	int		failures;
};

/*
 * One whole look-reset-enable-verify pass. With --check it changes nothing and returns what it found; otherwise
 * it resets the provider's domains when there is none or none is enabled (the only call that makes the daemon
 * create the default domain), enables every domain that is not, falls back to the public NSFileProviderManager
 * when the private calls left nothing enabled, and reads the state back -- the readback, not the calls, is the
 * verdict.
 */
static struct pass_result
enable_pass(id daemon, NSString *provider, BOOL reset, BOOL check)
{
	struct pass_result result = { 0, 0, 0 };
	NSError *error = nil;
	NSArray *domains;
	BOOL reset_sent = NO;

	show_provider(daemon, provider, "provider");
	domains = get_domains(daemon, provider, &error);
	fprintf(stderr, "fp_enable: domains before: %lu%s%s\n", (unsigned long)[domains count],
	    error ? ", error " : "", error ? [[error description] UTF8String] : "");
	for (NSUInteger i = 0; i < [domains count]; i++) {
		show_domain(domains[i], "before", i);
	}
	result.domains = [domains count];
	result.enabled = enabled_count(domains);
	if (check) {
		show_provider(daemon, provider, "after (no change)");
		fprintf(stderr, "fp_enable: --check: %d of %lu domain(s) enabled\n", result.enabled,
		    (unsigned long)[domains count]);
		return result;
	}
	/* no domain at all (the state every run before TEST7): make the daemon create the default one;
	 * a domain that is there but disabled (TEST7, TEST8): the daemon never re-enables it by itself,
	 * _recreateDefaultDomainIfNeeded sets userEnabled only where it creates the domain, so the
	 * reset is the only call that gives setEnabled:YES an identifier to work on. --reset does the
	 * same on purpose. LocalStorage's files stay in the app group; the daemon re-creates the
	 * default domain. */
	if (result.domains == 0 || reset || result.enabled == 0) {
		NSError *failure;

		fprintf(stderr, "fp_enable: reset (%s): %lu domain(s), %d enabled\n",
		    result.domains == 0 ? "no domain" : reset ? "--reset" : "none enabled",
		    (unsigned long)result.domains, result.enabled);
		failure = remove_all_domains(daemon, provider, &reset_sent);

		fprintf(stderr, "fp_enable: removeAllDomains: %s%s\n",
		    failure ? [[failure description] UTF8String] : "done", reset_sent ? "" : " (not sent)");
		say_error("removeAllDomains error", failure, 0);
		error = nil;
		domains = get_domains_timeout(daemon, provider, &error, 5.0);
		/* the domain is started before removeAllDomains answers, but the list is another
		 * round trip; an empty first answer is worth half a second and a second look */
		for (int attempt = 0; attempt < 6 && [domains count] == 0; attempt++) {
			fprintf(stderr, "fp_enable: no domain listed yet (attempt %d), waiting\n", attempt + 1);
			usleep(500000);
			error = nil;
			domains = get_domains_timeout(daemon, provider, &error, 2.0);
		}
		fprintf(stderr, "fp_enable: domains after the reset: %lu%s%s\n", (unsigned long)[domains count],
		    error ? ", error " : "", error ? [[error description] UTF8String] : "");
		for (NSUInteger i = 0; i < [domains count]; i++) {
			show_domain(domains[i], "after the reset", i);
		}
		result.domains = [domains count];
		result.enabled = enabled_count(domains);
		/* The private reset is the call that creates the default domain, and the proxy may have refused
		 * it (TEST8: it never even went out). The public addDomain: lands in the same daemon code; worth
		 * one try before the pass is called a failure. */
		if ([domains count] == 0) {
			add_domain_public(provider, @"Local Storage");
			error = nil;
			domains = get_domains_timeout(daemon, provider, &error, 5.0);
			fprintf(stderr, "fp_enable: domains after the public addDomain: %lu%s%s\n",
			    (unsigned long)[domains count], error ? ", error " : "",
			    error ? [[error description] UTF8String] : "");
			for (NSUInteger i = 0; i < [domains count]; i++) {
				show_domain(domains[i], "after the public addDomain", i);
			}
			result.domains = [domains count];
			result.enabled = enabled_count(domains);
		}
	}
	for (NSUInteger i = 0; i < [domains count]; i++) {
		NSString *identifier = domain_identifier(domains[i]);

		/* the re-created default domain already has userEnabled = [descriptor enabledByDefault];
		 * an enable call on top is what the Files toggle does, and it is not an error either way */
		if (domain_enabled(domains[i]) == 1) {
			fprintf(stderr, "fp_enable: domain \"%s\" is enabled already\n",
			    identifier ? [identifier UTF8String] : "(no identifier)");
			continue;
		}
		if (identifier != nil && set_enabled(daemon, provider, identifier) != 0) {
			result.failures++;
		}
	}
	/* The guessed identifiers of fp_enable 15:18 are kept only as a last resort: the default
	 * domain ID if the daemon still has not listed one. Not after a successful enable -- the
	 * domain list here is the one the reset returned, and an identifier the daemon does not have
	 * answers NSFileNoSuchFileError (4), which would turn a working enable into a failure. */
	if ([domains count] == 0) {
		result.failures += set_enabled(daemon, provider, @"NSFileProviderDomainDefaultIdentifier") != 0;
	}
	error = nil;
	domains = get_domains(daemon, provider, &error);
	result.domains = [domains count];
	result.enabled = enabled_count(domains);
	fprintf(stderr, "fp_enable: domains at the end: %lu, %d enabled%s%s\n", (unsigned long)[domains count],
	    result.enabled, error ? ", error " : "", error ? [[error description] UTF8String] : "");
	for (NSUInteger i = 0; i < [domains count]; i++) {
		show_domain(domains[i], "at the end", i);
	}
	show_provider(daemon, provider, "after");
	fprintf(stderr, "fp_enable: verdict: %s %s\n", [provider UTF8String],
	    result.enabled > 0 ? "has an enabled domain" : "has NO enabled domain");
	if ([domains count] == 0) {
		fprintf(stderr, "fp_enable: the provider still has no domain\n");
	} else if (result.failures != 0) {
		fprintf(stderr, "fp_enable: %d enable call(s) answered with an error\n", result.failures);
	} else if (result.enabled == 0) {
		fprintf(stderr, "fp_enable: the domain is listed but still disabled: the daemon "
		    "answered, the enable did not take (fileproviderctl domain list is the same state)\n");
	}
	return result;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		int index = 1;
		BOOL check = NO, reset = NO, wait = NO, restart = NO, restarted = NO;
		double wait_seconds = 30.0, retry_seconds = 0, restart_seconds = 20.0;
		NSString *provider = @"com.apple.FileProvider.LocalStorage";
		NSString *service = nil;
		NSArray *services;
		id daemon = nil;
		double deadline = 0;
		unsigned attempt;

		while (index < argc && argv[index][0] == '-') {
			if (strcmp(argv[index], "--trace") == 0) {
				trace_errors();
				index++;
			} else if (strcmp(argv[index], "--check") == 0) {
				check = YES;
				index++;
			} else if (strcmp(argv[index], "--reset") == 0) {
				reset = YES;
				index++;
			} else if (strncmp(argv[index], "--wait", 6) == 0 &&
			    (argv[index][6] == '\0' || argv[index][6] == '=')) {
				wait = YES;
				wait_seconds = argv[index][6] == '=' ? atof(argv[index] + 7) : 30.0;
				if (wait_seconds <= 0) {
					wait_seconds = 30.0;
				}
				index++;
			} else if (strncmp(argv[index], "--retry", 7) == 0 &&
			    (argv[index][7] == '\0' || argv[index][7] == '=')) {
				retry_seconds = argv[index][7] == '=' ? atof(argv[index] + 8) : 120.0;
				if (retry_seconds <= 0) {
					retry_seconds = 120.0;
				}
				index++;
			} else if (strncmp(argv[index], "--restart", 9) == 0 &&
			    (argv[index][9] == '\0' || argv[index][9] == '=')) {
				restart = YES;
				restart_seconds = argv[index][9] == '=' ? atof(argv[index] + 10) : 20.0;
				if (restart_seconds <= 0) {
					restart_seconds = 20.0;
				}
				index++;
			} else {
				fprintf(stderr, "fp_enable: %s: unknown option\n", argv[index]);
				return 2;
			}
		}
		if (index < argc) {
			provider = [NSString stringWithUTF8String:argv[index++]];
		}
		if (index < argc) {
			fprintf(stderr, "fp_enable: %s: too many arguments\n", argv[index]);
			return 2;
		}
		if (getuid() == 0) {
			(void)setgroups(0, NULL);
			if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
				fprintf(stderr, "fp_enable: setgid/setuid: %s\n", strerror(errno)); return 1;
			}
			setenv("HOME", "/var/mobile", 1);
			setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
		}
		fprintf(stderr, "fp_enable: running as uid %d gid %d (%s)\n", (int)getuid(), (int)getgid(),
		    getuid() == MOBILE_ID ? "mobile" : "NOT mobile");
		say_daemon_state("start");
		if ([provider isEqualToString:@"com.apple.FileProvider.LocalStorage"]) {
			say_registration_state(
			    "/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex");
		}
		ensure_storage();
		load_fileprovider();
		services = mach_service_names(&service);
		fprintf(stderr, "fp_enable: the job's service: %s%s\n", service ? [service UTF8String] : "(none)",
		    [services count] > 1 ? " (and its side services)" : "");
		daemon = wait ? wait_for_daemon(provider, wait_seconds, service) : proxy();
		if (!wait && daemon != nil && service != nil) {
			/* not asked to wait: still do not make the daemon wait for us -- one poke at the job
			 * costs nothing and turns a Cocoa 4099 below into a started daemon. */
			pid_t pid = 0;
			if (daemon_processes(&pid) == 0) {
				poke_launchd(service);
			}
		}
		if (daemon == nil) { fprintf(stderr, "fp_enable: no connection to fileproviderd\n"); return 1; }
		if (retry_seconds > 0) {
			deadline = now_seconds() + retry_seconds;
		}
		for (attempt = 1; ; attempt++) {
			char when[32];
			struct pass_result result;
			BOOL pass;

			snprintf(when, sizeof(when), "pass %u", attempt);
			say_daemon_state(when);
			result = enable_pass(daemon, provider, reset, check);
			pass = result.domains > 0 && result.failures == 0 && result.enabled > 0;

			if (check) {
				return result.enabled > 0 ? 0 : 1;
			}
			if (pass) {
				fprintf(stderr, "fp_enable: pass %u: %lu domain(s), %d enabled\n", attempt,
				    (unsigned long)result.domains, result.enabled);
				return 0;
			}
			/* R14: the provider is there and the reset was answered, but the daemon's list is
			 * still empty -- the case TEST9 showed, where fileproviderd had started 1.4 s before
			 * the LS/pkd rebuild finished. One restart, then the pass is repeated; a daemon born
			 * against the complete registry is the only state difference a client can make. */
			if (restart && !restarted && result.domains == 0 && result.failures == 0) {
				restarted = YES;
				fprintf(stderr, "fp_enable: pass %u: %lu domain(s) and no error, but no domain: "
				    "the daemon may have started before the registry was complete; restarting it\n",
				    attempt, (unsigned long)result.domains);
				if (restart_daemon(service, restart_seconds)) {
					daemon = wait ? wait_for_daemon(provider,
					    MIN(10.0, MAX(1.0, restart_seconds)), service) : proxy();
					if (daemon == nil) {
						fprintf(stderr, "fp_enable: no connection to fileproviderd "
						    "after the restart\n");
						return 1;
					}
					continue;
				}
				fprintf(stderr, "fp_enable: the restart did not happen; the pass goes on as before\n");
			}
			if (deadline == 0 || now_seconds() >= deadline) {
				fprintf(stderr, "fp_enable: pass %u: %lu domain(s), %d enabled, %d error(s); "
				    "%s\n", attempt, (unsigned long)result.domains, result.enabled, result.failures,
				    deadline == 0 ? "no --retry, giving up" : "the retry deadline has passed, giving up");
				return 1;
			}
			fprintf(stderr, "fp_enable: pass %u: %lu domain(s), %d enabled, %d error(s); %.0f s left, "
			    "waiting 5 s and trying again\n", attempt, (unsigned long)result.domains, result.enabled,
			    result.failures, deadline - now_seconds());
			sleep(5);
			daemon = wait ? wait_for_daemon(provider, MIN(10.0, MAX(1.0, deadline - now_seconds())), service)
			    : proxy();
			if (daemon == nil) {
				fprintf(stderr, "fp_enable: no connection to fileproviderd for the next pass\n");
				return 1;
			}
		}
	}
}
