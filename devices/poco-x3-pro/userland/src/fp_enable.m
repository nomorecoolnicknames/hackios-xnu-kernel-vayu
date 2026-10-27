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
 * exit: 0 the provider has at least one domain and every enable call answered without an error;
 *       1 no daemon, no domain after the reset, or an error; 2 bad usage
 */
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
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

static id
proxy(void)
{
	Class connection = NSClassFromString(@"FPDaemonConnection");
	id shared;

	if ([connection respondsToSelector:@selector(synchronousSharedConnectionProxy)]) {
		return [connection performSelector:@selector(synchronousSharedConnectionProxy)];
	}
	shared = [connection respondsToSelector:@selector(sharedConnection)] ? [connection performSelector:@selector(sharedConnection)] : nil;
	return [shared respondsToSelector:@selector(remoteObjectProxyWithErrorHandler:)] ?
	    ((id (*)(id, SEL, id))objc_msgSend)(shared, @selector(remoteObjectProxyWithErrorHandler:), ^(NSError *error) {
		    fprintf(stderr, "fp_enable: connection: %s\n", [[error description] UTF8String]);
	    }) : nil;
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
		daemon = wait ? wait_for_daemon(provider, wait_seconds) : proxy();
		if (daemon == nil) { fprintf(stderr, "fp_enable: no connection to fileproviderd\n"); return 1; }
		show_provider(daemon, provider, "provider");
		{
			NSError *error = nil;
			NSArray *domains = get_domains(daemon, provider, &error);
			int failures = 0;

			fprintf(stderr, "fp_enable: domains before: %lu%s%s\n", (unsigned long)[domains count],
			    error ? ", error " : "", error ? [[error description] UTF8String] : "");
			for (NSUInteger i = 0; i < [domains count]; i++) {
				show_domain(domains[i], "before", i);
			}
			if (check) {
				show_provider(daemon, provider, "after (no change)");
				return 0;
			}
			/* no domain at all (the state every run so far): make the daemon create the default one.
			 * --reset does the same on purpose when a domain exists but is disabled. */
			if ([domains count] == 0 || reset) {
				NSError *failure = remove_all_domains(daemon, provider);

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

				if (identifier != nil && set_enabled(daemon, provider, identifier) != 0) {
					failures++;
				}
			}
			/* The guessed identifiers of fp_enable 15:18 are kept only as a last resort: the default domain ID
			 * if the daemon still has not listed one. */
			if ([domains count] == 0) {
				failures += set_enabled(daemon, provider, @"NSFileProviderDomainDefaultIdentifier") != 0;
			}
			error = nil;
			domains = get_domains(daemon, provider, &error);
			fprintf(stderr, "fp_enable: domains at the end: %lu%s%s\n", (unsigned long)[domains count],
			    error ? ", error " : "", error ? [[error description] UTF8String] : "");
			for (NSUInteger i = 0; i < [domains count]; i++) {
				show_domain(domains[i], "at the end", i);
			}
			show_provider(daemon, provider, "after");
			if ([domains count] == 0) {
				fprintf(stderr, "fp_enable: the provider still has no domain\n");
				return 1;
			}
			if (failures != 0) {
				fprintf(stderr, "fp_enable: %d enable call(s) answered with an error\n", failures);
				return 1;
			}
		}
	}
	return 0;
}
