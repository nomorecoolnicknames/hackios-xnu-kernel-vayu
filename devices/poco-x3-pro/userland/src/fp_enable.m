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
 * usage: fp_enable [--wait[=SECONDS]] [--trace] [--check] [--reset] [PROVIDER_ID]
 *        --wait    wait up to SECONDS (default 30) for fileproviderd to answer before doing anything: the
 *                  unattended self-test calls this at +45 s and launchd may be starting the daemon on this
 *                  very first connection (the call itself makes the daemon run), so "no connection yet" is
 *                  a state to wait out, not a failure
 *        --check   look only (domains, provider); change nothing
 *        --reset   remove this provider's domains first even if there are some (LocalStorage only: its files stay
 *                  in the app group; the daemon re-creates the default domain)
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
#include <sys/time.h>
#include <unistd.h>

#define MOBILE_ID	501

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
	unsigned i;

	if (NSClassFromString(@"FPDaemonConnection") != Nil) {
		return;
	}
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL) == NULL) {
			fprintf(stderr, "fp_enable: dlopen %s: %s\n", paths[i], dlerror());
		} else {
			fprintf(stderr, "fp_enable: loaded %s\n", paths[i]);
		}
	}
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

	((void (*)(id, SEL, id, id))objc_msgSend)(daemon, @selector(providerDomainForIdentifier:completionHandler:), provider,
	    ^(id domain, NSError *error) {
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

	if (![daemon respondsToSelector:@selector(getDomainsForProviderIdentifier:completionHandler:)]) {
		fprintf(stderr, "fp_enable: the daemon has no getDomainsForProviderIdentifier:\n");
		if (out_error != NULL) {
			*out_error = nil;
		}
		return nil;
	}
	((void (*)(id, SEL, id, id))objc_msgSend)(daemon, @selector(getDomainsForProviderIdentifier:completionHandler:), provider,
	    ^(NSString *identifier, NSArray *domains, NSError *error) {
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
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC))) != 0) {
		fprintf(stderr, "fp_enable: getDomains: no answer in %.0f s\n", seconds);
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
 * answering getDomainsForProviderIdentifier: with a short timeout, retried until SECONDS pass.
 */
static id
wait_for_daemon(NSString *provider, double seconds)
{
	double start = now_seconds(), noted = 0;
	NSError *error = nil;
	NSArray *domains;
	id daemon;

	for (;;) {
		daemon = proxy();
		if (daemon != nil) {
			error = nil;
			domains = get_domains_timeout(daemon, provider, &error, 2.0);
			/* an answer is an array (even an empty one) or an error that is the daemon's own;
			 * a timeout leaves both nil, and a dead connection is Cocoa 4099 */
			if (domains != nil || (error != nil && !connection_error(error))) {
				fprintf(stderr, "fp_enable: fileproviderd answered after %.1f s%s%s\n",
				    now_seconds() - start, error ? ", error " : "",
				    error ? [[error description] UTF8String] : "");
				return daemon;
			}
			fprintf(stderr, "fp_enable: fileproviderd is not up yet (%.1f s of %.0f)\n",
			    now_seconds() - start, seconds);
		}
		if (now_seconds() - start >= seconds) {
			fprintf(stderr, "fp_enable: no answer from fileproviderd in %.0f s\n", seconds);
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
remove_all_domains(id daemon, NSString *provider)
{
	__block NSError *failure = nil;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);

	if (![daemon respondsToSelector:@selector(removeAllDomainsForProviderIdentifier:completionHandler:)]) {
		fprintf(stderr, "fp_enable: the daemon has no removeAllDomainsForProviderIdentifier:\n");
		return nil;
	}
	((void (*)(id, SEL, id, id))objc_msgSend)(daemon, @selector(removeAllDomainsForProviderIdentifier:completionHandler:), provider,
	    ^(NSError *error) {
		failure = error;
		dispatch_semaphore_signal(done);
	});
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) {
		fprintf(stderr, "fp_enable: removeAllDomains: no answer in 30 s\n");
	}
	return failure;
}

static int
set_enabled(id daemon, NSString *provider, NSString *domain)
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	__block int failed = 0;

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

int
main(int argc, char **argv)
{
	@autoreleasepool {
		int index = 1;
		BOOL check = NO, reset = NO, wait = NO;
		double wait_seconds = 30.0;
		NSString *provider = @"com.apple.FileProvider.LocalStorage";
		id daemon;

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
		ensure_storage();
		load_fileprovider();
		daemon = wait ? wait_for_daemon(provider, wait_seconds) : proxy();
		if (daemon == nil) { fprintf(stderr, "fp_enable: no connection to fileproviderd\n"); return 1; }
		show_provider(daemon, provider, "provider");
		{
			NSError *error = nil;
			NSArray *domains = get_domains(daemon, provider, &error);
			int failures = 0, enabled;

			fprintf(stderr, "fp_enable: domains before: %lu%s%s\n", (unsigned long)[domains count],
			    error ? ", error " : "", error ? [[error description] UTF8String] : "");
			for (NSUInteger i = 0; i < [domains count]; i++) {
				show_domain(domains[i], "before", i);
			}
			enabled = enabled_count(domains);
			if (check) {
				show_provider(daemon, provider, "after (no change)");
				fprintf(stderr, "fp_enable: --check: %d of %lu domain(s) enabled\n", enabled,
				    (unsigned long)[domains count]);
				return enabled > 0 ? 0 : 1;
			}
			/* no domain at all (the state every run so far): make the daemon create the default one;
			 * a domain that is there but disabled (TEST7): the daemon never re-enables it by itself,
			 * _recreateDefaultDomainIfNeeded sets userEnabled only where it creates the domain, so the
			 * reset is the only call that gives setEnabled:YES an identifier to work on. --reset does the
			 * same on purpose. LocalStorage's files stay in the app group; the daemon re-creates the
			 * default domain. */
			if ([domains count] == 0 || reset || enabled == 0) {
				NSError *failure;

				fprintf(stderr, "fp_enable: reset (%s): %lu domain(s), %d enabled\n",
				    [domains count] == 0 ? "no domain" : reset ? "--reset" : "none enabled",
				    (unsigned long)[domains count], enabled);
				failure = remove_all_domains(daemon, provider);

				fprintf(stderr, "fp_enable: removeAllDomains: %s\n",
				    failure ? [[failure description] UTF8String] : "done");
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
					failures++;
				}
			}
			/* The guessed identifiers of fp_enable 15:18 are kept only as a last resort: the default
			 * domain ID if the daemon still has not listed one. Not after a successful enable -- the
			 * domain list here is the one the reset returned, and an identifier the daemon does not have
			 * answers NSFileNoSuchFileError (4), which would turn a working enable into a failure. */
			if ([domains count] == 0) {
				failures += set_enabled(daemon, provider, @"NSFileProviderDomainDefaultIdentifier") != 0;
			}
			error = nil;
			domains = get_domains(daemon, provider, &error);
			enabled = enabled_count(domains);
			fprintf(stderr, "fp_enable: domains at the end: %lu, %d enabled%s%s\n", (unsigned long)[domains count],
			    enabled, error ? ", error " : "", error ? [[error description] UTF8String] : "");
			for (NSUInteger i = 0; i < [domains count]; i++) {
				show_domain(domains[i], "at the end", i);
			}
			show_provider(daemon, provider, "after");
			fprintf(stderr, "fp_enable: verdict: %s %s\n", [provider UTF8String],
			    enabled > 0 ? "has an enabled domain" : "has NO enabled domain");
			if ([domains count] == 0) {
				fprintf(stderr, "fp_enable: the provider still has no domain\n");
				return 1;
			}
			if (failures != 0) {
				fprintf(stderr, "fp_enable: %d enable call(s) answered with an error\n", failures);
				return 1;
			}
			if (enabled == 0) {
				fprintf(stderr, "fp_enable: the domain is listed but still disabled: the daemon "
				    "answered, the enable did not take (fileproviderctl domain list is the same state)\n");
				return 1;
			}
		}
	}
	return 0;
}
