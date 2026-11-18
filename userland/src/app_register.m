/*
 * app_register: ask LaunchServices to take an application bundle that lives outside the
 * system image's /Applications (a read-only volume here) and to publish it to SpringBoard.
 *
 * why: the port mounts the "system" image at / (with /Applications, 168 apps, EROFS) and the
 * root image's files under /redwood. Everything the root image adds lands in
 * /redwood/Applications, which no LaunchServices scan ever looks at, so the app count stays
 * 168 and VayuNFC / NFC Tools have no record and no icon. ls_rebuild --app=<arg> is
 * applicationProxyForIdentifier: (an identifier, not a path) and answers "registered,
 * bundle (no bundle URL)" for the path string - it never registered anything.
 *
 * usage: app_register [--check] BUNDLE_PATH...
 *   --check: only print what LaunchServices holds now.
 *
 * The private call is -[LSApplicationWorkspace registerApplication:] (present in the 16.5
 * CoreServices: the selector name is in __objc_methname at 0x181d97084, next to
 * installApplication:withOptions:error: 0x181d93175 and unregisterApplication: 0x181d9ad51).
 * If a build of the framework does not answer it, installApplication:withOptions:error: is
 * tried next, and the tool says exactly which selector it used, so the log decides.
 */
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <errno.h>
#include <grp.h>
#include <string.h>
#include <unistd.h>

#define MOBILE_ID 501

static void
load_launchservices(void)
{
	static const char *const paths[] = {
		"/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices",
		"/System/Library/Frameworks/CoreServices.framework/CoreServices",
	};
	unsigned i;

	if (NSClassFromString(@"LSApplicationWorkspace") != Nil) {
		return;
	}
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL) != NULL) {
			fprintf(stderr, "app_register: loaded %s\n", paths[i]);
		}
	}
}

static id
proxy_for(NSString *identifier)
{
	Class app_class = NSClassFromString(@"LSApplicationProxy");

	if (app_class == Nil || ![app_class respondsToSelector:@selector(applicationProxyForIdentifier:)]) {
		return nil;
	}
	return ((id (*)(id, SEL, id))objc_msgSend)(app_class,
	    @selector(applicationProxyForIdentifier:), identifier);
}

static NSString *
proxy_path(id proxy)
{
	NSURL *url;

	if (proxy == nil || ![proxy respondsToSelector:@selector(bundleURL)]) {
		return nil;
	}
	url = ((id (*)(id, SEL))objc_msgSend)(proxy, @selector(bundleURL));
	if (![url isKindOfClass:[NSURL class]]) {
		return nil;
	}
	return [url path];
}

static NSString *
bundle_identifier(NSString *path)
{
	NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:
	    [path stringByAppendingPathComponent:@"Info.plist"]];
	id value = [plist objectForKey:@"CFBundleIdentifier"];

	return [value isKindOfClass:[NSString class]] ? value : nil;
}

static void
say_state(NSString *path, NSString *identifier)
{
	id proxy = proxy_for(identifier);
	NSString *held = proxy_path(proxy);

	fprintf(stderr, "app_register: %s: %s record %s, bundle %s%s%s\n",
	    [identifier UTF8String],
	    [path UTF8String],
	    proxy != nil ? "present" : "ABSENT",
	    held != nil ? [held UTF8String] : "(no bundle URL)",
	    held != nil && [held isEqualToString:path] ? " (this bundle)" : "",
	    proxy != nil && held == nil ? "  <- the 'registered, bundle (no bundle URL)' trap" : "");
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		BOOL check = NO;
		int index = 1, failures = 0;
		Class workspace_class;
		id workspace;

		if (index < argc && strcmp(argv[index], "--check") == 0) {
			check = YES;
			index++;
		}
		if (index >= argc) {
			fprintf(stderr, "usage: app_register [--check] BUNDLE_PATH...\n");
			return 2;
		}
		/* the database SpringBoard reads is mobile's */
		if (getuid() == 0) {
			(void)setgroups(0, NULL);
			if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
				fprintf(stderr, "app_register: setgid/setuid: %s\n", strerror(errno));
				return 1;
			}
			setenv("HOME", "/var/mobile", 1);
			setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
		}
		load_launchservices();
		workspace_class = NSClassFromString(@"LSApplicationWorkspace");
		workspace = [workspace_class performSelector:@selector(defaultWorkspace)];
		fprintf(stderr, "app_register: LSApplicationWorkspace %s, workspace %s\n",
		    workspace_class != Nil ? "found" : "MISSING", workspace != nil ? "made" : "nil");
		if (workspace == nil) {
			return 1;
		}
		/* what this process can actually send, said once: the log then explains the result */
		fprintf(stderr, "app_register: workspace answers: registerApplication: %d, "
		    "installApplication:withOptions:error: %d, unregisterApplication: %d, "
		    "registerPlugin: %d\n",
		    (int)[workspace respondsToSelector:@selector(registerApplication:)],
		    (int)[workspace respondsToSelector:@selector(installApplication:withOptions:error:)],
		    (int)[workspace respondsToSelector:@selector(unregisterApplication:)],
		    (int)[workspace respondsToSelector:@selector(registerPlugin:)]);

		for (; index < argc; index++) {
			NSString *path = [NSString stringWithUTF8String:argv[index]];
			NSString *identifier = bundle_identifier(path);
			NSDictionary *plist;

			if (identifier == nil) {
				fprintf(stderr, "app_register: %s: no Info.plist/CFBundleIdentifier\n", argv[index]);
				failures++;
				continue;
			}
			fprintf(stderr, "app_register: --- %s (%s) ---\n",
			    argv[index], [identifier UTF8String]);
			say_state(path, identifier);
			if (check) {
				continue;
			}
			plist = [NSDictionary dictionaryWithContentsOfFile:
			    [path stringByAppendingPathComponent:@"Info.plist"]];
			fprintf(stderr, "app_register:   CFBundleExecutable = %s, CFBundleVersion = %s\n",
			    [[plist objectForKey:@"CFBundleExecutable"] isKindOfClass:[NSString class]] ?
			    [[plist objectForKey:@"CFBundleExecutable"] UTF8String] : "(none)",
			    [[plist objectForKey:@"CFBundleVersion"] isKindOfClass:[NSString class]] ?
			    [[plist objectForKey:@"CFBundleVersion"] UTF8String] : "(none)");
			if (proxy_path(proxy_for(identifier)) != nil) {
				fprintf(stderr, "app_register:   already has a bundle URL; nothing to register\n");
				continue;
			}
			if ([workspace respondsToSelector:@selector(registerApplication:)]) {
				NSURL *url = [NSURL fileURLWithPath:path];
				BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(workspace,
				    @selector(registerApplication:), url);

				fprintf(stderr, "app_register:   -[LSApplicationWorkspace registerApplication:%s] -> %d\n",
				    argv[index], (int)ok);
			} else if ([workspace respondsToSelector:@selector(installApplication:withOptions:error:)]) {
				NSURL *url = [NSURL fileURLWithPath:path];
				NSError *error = nil;
				BOOL ok = ((BOOL (*)(id, SEL, id, id, NSError **))objc_msgSend)(workspace,
				    @selector(installApplication:withOptions:error:), url, nil, &error);

				fprintf(stderr, "app_register:   -[LSApplicationWorkspace "
				    "installApplication:withOptions:error:%s] -> %d%s%s\n", argv[index], (int)ok,
				    error != nil ? ", error " : "",
				    error != nil ? [[error description] UTF8String] : "");
			} else {
				fprintf(stderr, "app_register:   no registration selector answers; "
				    "the bundle stays unknown to LaunchServices\n");
			}
			/*
			 * lsd is a separate process: the record may show up in this process's view of
			 * the database a moment later. Poll for it instead of calling a successful
			 * registration a failure.
			 */
			{
				unsigned tries;

				for (tries = 0; tries < 30; tries++) {
					if (proxy_path(proxy_for(identifier)) != nil) {
						break;
					}
					[NSThread sleepForTimeInterval:0.1];
				}
				if (tries != 0) {
					fprintf(stderr, "app_register:   the record appeared after %u.%u s\n",
					    tries / 10, tries % 10);
				}
			}
			say_state(path, identifier);
			if (proxy_path(proxy_for(identifier)) == nil) {
				fprintf(stderr, "app_register:   still no bundle URL here; the authoritative "
				    "check is the next /redwood/bin/ls_rebuild --app=<bundle id>\n");
				failures++;
			}
		}
		if (!check) {
			Class app_class = NSClassFromString(@"LSApplicationProxy");
			(void)app_class;
			fprintf(stderr, "app_register: done, %d bundle(s) still without a bundle URL\n", failures);
		}
		return failures == 0 ? 0 : 1;
	}
}
