/*
 * ls_rebuild: have LaunchServices rebuild its database of system apps and plug-ins, then say how many plug-ins it
 * knows and whether one (by default Files' "On My iPhone" provider) is among them.
 *
 * Live 2026-10-25 13:56: lsdiagnose listed no app extension at all, so fileproviderd finds no LocalStorage provider
 * (-2001) and the Files toggle does nothing. On a device the first-boot data migration (SystemAppMigrator, which
 * our boot does not run) has installd and lsd register system plug-ins; this asks lsd for the same rebuild by
 * the call LSApplicationWorkspace offers for it, entitled com.apple.lsapplicationworkspace.rebuildappdatabases.
 *
 * usage: ls_rebuild [--check | --register BUNDLE_PATH] [PLUGIN_ID]
 *   (--check: no rebuild, only the counts; --register: registerPlugin: of one bundle instead of the rebuild)
 */
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <errno.h>
#include <grp.h>
#include <string.h>
#include <unistd.h>

#define MOBILE_ID	501

static void
report(const char *when, NSString *plugin)
{
	Class workspace_class = NSClassFromString(@"LSApplicationWorkspace");
	Class proxy_class = NSClassFromString(@"LSPlugInKitProxy");
	id workspace = [workspace_class performSelector:@selector(defaultWorkspace)];

	/* run 14:10 printed "0 plug-ins" and "no rebuild call" from a build without CoreServices linked: a nil
	 * workspace answers 0 and NO to everything, so say so instead */
	fprintf(stderr, "ls_rebuild: %s: LSApplicationWorkspace %s, workspace %s, LSPlugInKitProxy %s\n", when,
	    workspace_class ? "found" : "MISSING", workspace ? "made" : "nil", proxy_class ? "found" : "MISSING");
	id proxy = nil;
	NSArray *plugins = nil;

	if ([workspace respondsToSelector:@selector(installedPlugins)]) {
		plugins = [workspace performSelector:@selector(installedPlugins)];
	}
	fprintf(stderr, "ls_rebuild: %s: %lu plug-ins registered\n", when, (unsigned long)[plugins count]);
	if ([proxy_class respondsToSelector:@selector(pluginKitProxyForIdentifier:)]) {
		proxy = [proxy_class performSelector:@selector(pluginKitProxyForIdentifier:) withObject:plugin];
	}
	fprintf(stderr, "ls_rebuild: %s: %s: %s\n", when, [plugin UTF8String],
	    proxy ? [[[proxy performSelector:@selector(bundleURL)] path] ?: @"(registered, no URL)" UTF8String] : "not registered");
	/* what else LS holds: applications at all (the home screen has them), Files' own record and its plug-ins */
	if ([workspace respondsToSelector:@selector(allInstalledApplications)]) {
		fprintf(stderr, "ls_rebuild: %s: %lu applications registered\n", when,
		    (unsigned long)[[workspace performSelector:@selector(allInstalledApplications)] count]);
	}
	if ([workspace respondsToSelector:NSSelectorFromString(@"_LSPrivateDatabaseNeedsRebuild")]) {
		fprintf(stderr, "ls_rebuild: %s: LS says its database needs a rebuild: %d\n", when,
		    (int)((BOOL (*)(id, SEL))objc_msgSend)(workspace, NSSelectorFromString(@"_LSPrivateDatabaseNeedsRebuild")));
	}
	Class app_class = NSClassFromString(@"LSApplicationProxy");
	id files = [app_class respondsToSelector:@selector(applicationProxyForIdentifier:)] ?
	    [app_class performSelector:@selector(applicationProxyForIdentifier:) withObject:@"com.apple.DocumentsApp"] : nil;
	if (files != nil && [files respondsToSelector:@selector(plugInKitPlugins)]) {
		fprintf(stderr, "ls_rebuild: %s: com.apple.DocumentsApp: %lu plug-ins of its own\n", when,
		    (unsigned long)[[files performSelector:@selector(plugInKitPlugins)] count]);
	} else fprintf(stderr, "ls_rebuild: %s: com.apple.DocumentsApp: %s\n", when, files ? "no plug-in list" : "no record");
	/* and what PlugInKit (which fileproviderd asks) finds for the identifier */
	NSError *error = nil;
	id extension = nil;
	Class extension_class = NSClassFromString(@"NSExtension");
	if ([extension_class respondsToSelector:@selector(extensionWithIdentifier:error:)]) {
		extension = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(extension_class, @selector(extensionWithIdentifier:error:),
		    plugin, &error);
	}
	fprintf(stderr, "ls_rebuild: %s: NSExtension %s: %s\n", when, [plugin UTF8String],
	    extension ? "found" : [[error description] ?: @"not found" UTF8String]);
}

/*
 * GPU-B (14:31): LS holds 506 plug-ins and the LocalStorage one among them, fileproviderd lists it among its 8
 * providers, yet NSExtension finds no such extension (nil, no error) and fileproviderctl ls says -2001. That
 * lookup goes through pkd. So: what pkd finds for a few extension points, matched asynchronously (which
 * reports pkd's errors), and the LocalStorage extension's election once found.
 */
static void
probe_points(void)
{
	static NSString *const points[] = { @"com.apple.fileprovider-nonui", @"com.apple.fileprovider-actionsui",
	    @"com.apple.share-services", @"com.apple.widgetkit-extension", @"com.apple.keyboard-service" };
	Class extension_class = NSClassFromString(@"NSExtension");
	SEL begin = @selector(beginMatchingExtensionsWithAttributes:completion:);
	SEL end = @selector(endMatchingExtensions:);

	if (![extension_class respondsToSelector:begin]) {
		fprintf(stderr, "ls_rebuild: points: NSExtension has no beginMatchingExtensions\n"); return;
	}
	for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block BOOL reported = NO;
		id context;

		context = ((id (*)(id, SEL, id, void (^)(NSArray *, NSError *)))objc_msgSend)(extension_class, begin,
		    @{ @"NSExtensionPointName": points[i] }, ^(NSArray *found, NSError *error) {
			if (reported) return;
			reported = YES;
			fprintf(stderr, "ls_rebuild: points: %s: %lu found%s%s\n", [points[i] UTF8String],
			    (unsigned long)[found count], error ? ", error " : "", error ? [[error description] UTF8String] : "");
			unsigned shown = 0;
			for (id extension in found) {
				NSString *identifier = [extension performSelector:@selector(identifier)];
				/* which ones pkd does hand out: in an app, or in a framework like the file providers */
				if (shown++ < 6) {
					NSURL *url = [extension respondsToSelector:@selector(_extensionBundle)] ?
					    [[extension performSelector:@selector(_extensionBundle)] bundleURL] : nil;
					fprintf(stderr, "ls_rebuild: points:   %s %s\n", [identifier UTF8String], [[url path] ?: @"" UTF8String]);
				}
				if ([identifier hasPrefix:@"com.apple.FileProvider.LocalStorage"]) {
					fprintf(stderr, "ls_rebuild: points: %s found, election %ld\n", [identifier UTF8String],
					    [extension respondsToSelector:@selector(userElection)] ?
					    (long)((NSInteger (*)(id, SEL))objc_msgSend)(extension, @selector(userElection)) : -99L);
				}
			}
			dispatch_semaphore_signal(done);
		});
		if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
			fprintf(stderr, "ls_rebuild: points: %s: no answer in 10 s\n", [points[i] UTF8String]);
		}
		if (context != nil && [extension_class respondsToSelector:end]) {
			((void (*)(id, SEL, id))objc_msgSend)(extension_class, end, context);
		}
	}
}

/* the plug-in registered by its bundle, one at a time, as uicache -p tries it (live 14:03: "Unable to register") */
static void
register_plugin(const char *path)
{
	id workspace = [NSClassFromString(@"LSApplicationWorkspace") performSelector:@selector(defaultWorkspace)];
	NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];

	if (![workspace respondsToSelector:@selector(registerPlugin:)]) {
		fprintf(stderr, "ls_rebuild: no registerPlugin:\n"); return;
	}
	fprintf(stderr, "ls_rebuild: registerPlugin %s says %d\n", path,
	    (int)((BOOL (*)(id, SEL, id))objc_msgSend)(workspace, @selector(registerPlugin:), url));
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		int index = 1;
		BOOL check = NO;
		NSString *plugin = @"com.apple.FileProvider.LocalStorage";
		id workspace;
		SEL rebuild = NSSelectorFromString(@"_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:");

		const char *register_path = NULL;

		if (index < argc && strcmp(argv[index], "--check") == 0) { check = YES; index++; }
		else if (index + 1 < argc && strcmp(argv[index], "--register") == 0) { register_path = argv[index + 1]; index += 2; }
		if (index < argc) plugin = [NSString stringWithUTF8String:argv[index++]];
		/* the database that apps and fileproviderd read is mobile's */
		if (getuid() == 0) {
			(void)setgroups(0, NULL);
			if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
				fprintf(stderr, "ls_rebuild: setgid/setuid: %s\n", strerror(errno)); return 1;
			}
			setenv("HOME", "/var/mobile", 1);
			setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
		}
		report("before", plugin);
		probe_points();
		if (check) return 0;
		if (register_path != NULL) {
			register_plugin(register_path);
			report("after", plugin);
			return 0;
		}
		workspace = [NSClassFromString(@"LSApplicationWorkspace") performSelector:@selector(defaultWorkspace)];
		/* as 00LaunchServicesMigrator does it: say a migrator runs, then rebuild */
		if ([workspace respondsToSelector:NSSelectorFromString(@"_LSPrivateNoteMigratorRunning")]) {
			((void (*)(id, SEL))objc_msgSend)(workspace, NSSelectorFromString(@"_LSPrivateNoteMigratorRunning"));
			fprintf(stderr, "ls_rebuild: noted a migrator running\n");
		}
		if (![workspace respondsToSelector:rebuild]) {
			fprintf(stderr, "ls_rebuild: LSApplicationWorkspace has no rebuild call\n"); return 1;
		}
		fprintf(stderr, "ls_rebuild: rebuilding (system, internal, user)\n");
		BOOL ok = ((BOOL (*)(id, SEL, BOOL, BOOL, BOOL))objc_msgSend)(workspace, rebuild, YES, YES, YES);
		fprintf(stderr, "ls_rebuild: rebuild says %d\n", (int)ok);
		report("after", plugin);
	}
	return 0;
}
