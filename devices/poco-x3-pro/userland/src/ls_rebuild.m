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
#include <dlfcn.h>
#include <errno.h>
#include <grp.h>
#include <string.h>
#include <unistd.h>

#define MOBILE_ID	501

/*
 * LSApplicationWorkspace, LSPlugInKitProxy and LSApplicationProxy live in MobileCoreServices (CoreServices is
 * its umbrella on iOS 16), and a program that links only Foundation does not have them: NSClassFromString()
 * answers nil, installedPlugins says 0, the rebuild call runs on nothing. TEST7's log is exactly that --
 * "ls_rebuild: before: LSApplicationWorkspace MISSING, workspace nil, LSPlugInKitProxy MISSING" and
 * "0 plug-ins registered" -- so LaunchServices' records of the system's app extensions were never rebuilt.
 * build.sh links MobileCoreServices now; this is the belt for a build that does not, and it says so.
 */
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
		if (dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL) == NULL) {
			fprintf(stderr, "ls_rebuild: dlopen %s: %s\n", paths[i], dlerror());
		} else {
			fprintf(stderr, "ls_rebuild: loaded %s\n", paths[i]);
		}
	}
}

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
 * F5: the camera stage's question, for an *application* rather than a plug-in. selftest asks SpringBoard
 * to open hakiostest://camera and SBSOpenSensitiveURLAndUnlock answers NO (battery_test's "it REFUSES",
 * TEST13B :71928); that is either "no bundle in LaunchServices claims the scheme" (this prints the
 * record, or its absence) or "the bundle is there and SpringBoard still refused" -- two different faults
 * that the old stage could not tell apart. com.hakios.selftest.camera is SelftestCamera.app's id.
 */
static void
report_app(const char *bundle_id)
{
	Class app_class = NSClassFromString(@"LSApplicationProxy");
	NSString *identifier = [NSString stringWithUTF8String:bundle_id];
	id app = nil;
	NSString *path = nil;

	if ([app_class respondsToSelector:@selector(applicationProxyForIdentifier:)]) {
		app = [app_class performSelector:@selector(applicationProxyForIdentifier:) withObject:identifier];
	}
	if (app == nil) {
		fprintf(stderr, "ls_rebuild: app %s: not registered (no handler for its URL schemes)\n", bundle_id);
		return;
	}
	if ([app respondsToSelector:@selector(bundleURL)]) {
		path = [[app performSelector:@selector(bundleURL)] path];
	}
	fprintf(stderr, "ls_rebuild: app %s: registered, bundle %s\n", bundle_id,
	    path != nil ? [path UTF8String] : "(no bundle URL)");
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

/*
 * F6: one plug-in's record, as LaunchServices holds it. pluginKitDictionary is the NSExtension dictionary
 * the plug-in was registered with -- the same dictionary NSExtension.attributes answers with, and the one
 * fileproviderd builds FPDProviderDescriptor from. TEST14's LocalStorage record did not carry
 * NSExtensionFileProviderDocumentGroup while the appex's Info.plist did, so this is the line that says
 * whether a re-registration changed anything.
 */
static void
say_plugin_record(NSString *identifier)
{
	Class proxy_class = NSClassFromString(@"LSPlugInKitProxy");
	id proxy = nil, dictionary = nil;
	NSArray *keys;

	if (proxy_class == Nil) { fprintf(stderr, "ls_rebuild: record: LSPlugInKitProxy is MISSING\n"); return; }
	if ([proxy_class respondsToSelector:@selector(pluginKitProxyForIdentifier:)]) {
		proxy = ((id (*)(id, SEL, id))objc_msgSend)(proxy_class,
		    @selector(pluginKitProxyForIdentifier:), identifier);
	}
	if (proxy == nil) {
		fprintf(stderr, "ls_rebuild: record: %s: not registered\n", [identifier UTF8String]);
		return;
	}
	fprintf(stderr, "ls_rebuild: record: %s: bundle %s\n", [identifier UTF8String],
	    [[[proxy performSelector:@selector(bundleURL)] path] ?: @"(no bundle URL)" UTF8String]);
	if ([proxy respondsToSelector:@selector(pluginKitDictionary)]) {
		dictionary = ((id (*)(id, SEL))objc_msgSend)(proxy, @selector(pluginKitDictionary));
	}
	if (![dictionary isKindOfClass:[NSDictionary class]]) {
		fprintf(stderr, "ls_rebuild: record: %s: no pluginKitDictionary (%s)\n", [identifier UTF8String],
		    dictionary != nil ? "not a dictionary" : "nil");
		return;
	}
	keys = [[dictionary allKeys] sortedArrayUsingSelector:@selector(compare:)];
	fprintf(stderr, "ls_rebuild: record: %s: %lu key(s)%s\n", [identifier UTF8String],
	    (unsigned long)[keys count],
	    [dictionary objectForKey:@"NSExtensionFileProviderDocumentGroup"] != nil ?
	    ", NSExtensionFileProviderDocumentGroup = " : ", NO NSExtensionFileProviderDocumentGroup");
	for (NSString *key in keys) {
		id value = [dictionary objectForKey:key];
		NSString *text = [value isKindOfClass:[NSString class]] ? value : [value description];

		fprintf(stderr, "ls_rebuild:   %s = %s\n", [key UTF8String],
		    [[text substringToIndex:MIN((NSUInteger)160, [text length])] UTF8String]);
	}
}

/*
 * F6: --plugins. Every file provider LaunchServices already knows (the com.apple.fileprovider-nonui
 * point) is registered again by its own bundle. registerPlugin: is the per-bundle call that reads the
 * bundle's Info.plist and stores its plug-in record; the bulk database rebuild need not carry the
 * point-specific NSExtension keys, and TEST14 is exactly that state -- the plist has
 * NSExtensionFileProviderDocumentGroup, the record NSExtension.attributes answers with does not. It is
 * idempotent and creates nothing, so it runs before fileproviderd is asked anything.
 *
 * Returns the number of bundles that were offered to registerPlugin:.
 */
static unsigned
reregister_file_providers(NSString *plugin)
{
	id workspace = [NSClassFromString(@"LSApplicationWorkspace") performSelector:@selector(defaultWorkspace)];
	NSArray *plugins = nil;
	unsigned offered = 0, accepted = 0;

	if ([workspace respondsToSelector:@selector(installedPlugins)]) {
		plugins = [workspace performSelector:@selector(installedPlugins)];
	}
	fprintf(stderr, "ls_rebuild: --plugins: %lu plug-in(s) to look at\n", (unsigned long)[plugins count]);
	for (id proxy in plugins) {
		NSDictionary *dictionary = nil;
		NSString *point = nil;
		NSURL *url = nil;

		if (![proxy respondsToSelector:@selector(pluginKitDictionary)]) { continue; }
		dictionary = ((id (*)(id, SEL))objc_msgSend)(proxy, @selector(pluginKitDictionary));
		if (![dictionary isKindOfClass:[NSDictionary class]]) { continue; }
		point = [dictionary objectForKey:@"NSExtensionPointIdentifier"];
		if (![point isKindOfClass:[NSString class]] ||
		    ![point isEqualToString:@"com.apple.fileprovider-nonui"]) { continue; }
		if ([proxy respondsToSelector:@selector(bundleURL)]) {
			url = [proxy performSelector:@selector(bundleURL)];
		}
		if (url == nil) {
			fprintf(stderr, "ls_rebuild: --plugins: %s: no bundle URL, cannot re-register\n",
			    [[[proxy performSelector:@selector(identifier)] description] UTF8String]);
			continue;
		}
		offered++;
		if ([workspace respondsToSelector:@selector(registerPlugin:)]) {
			BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(workspace,
			    @selector(registerPlugin:), url);
			accepted += ok ? 1 : 0;
			fprintf(stderr, "ls_rebuild: --plugins: registerPlugin %s says %d\n",
			    [[url path] UTF8String], (int)ok);
		}
	}
	fprintf(stderr, "ls_rebuild: --plugins: %u file provider bundle(s) offered, %u accepted\n",
	    offered, accepted);
	say_plugin_record(plugin);
	return offered;
}

/*
 * F6: --fresh. The LaunchServices database is a cache of the plists on disk; when the record itself is
 * wrong, rebuilding it from the same stale store keeps the wrong record. This moves the stores aside
 * (renames, never deletes: the suffix is .f6-backup) so the next lsd reads the bundles into fresh
 * records. It is a boot-time experiment, not something the self-test does on its own: the UI reads this
 * database, and a failed rebuild leaves it empty until the next one.
 */
static void
move_launchservices_stores(void)
{
	static const char *const directories[] = {
		"/var/mobile/Library/Caches",
		"/var/mobile/Library/Preferences",
	};
	NSFileManager *files = [NSFileManager defaultManager];

	for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		NSString *directory = [NSString stringWithUTF8String:directories[i]];
		NSArray *names = [files contentsOfDirectoryAtPath:directory error:NULL];

		for (NSString *name in names) {
			BOOL wanted = [name hasPrefix:@"com.apple.LaunchServices-"] ||
			    [name hasPrefix:@"com.apple.LaunchServices."];

			if (!wanted) { continue; }
			NSString *from = [directory stringByAppendingPathComponent:name];
			NSString *to = [from stringByAppendingString:@".f6-backup"];
			if ([files moveItemAtPath:from toPath:to error:NULL]) {
				fprintf(stderr, "ls_rebuild: --fresh: moved %s aside\n", [name UTF8String]);
			} else {
				fprintf(stderr, "ls_rebuild: --fresh: could not move %s aside\n",
				    [name UTF8String]);
			}
		}
	}
	fprintf(stderr, "ls_rebuild: --fresh: the stores are gone; the rebuild below, or the next lsd "
	    "start, reads the bundles again\n");
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

		const char *register_path = NULL, *app_id = NULL;
		BOOL plugins = NO, fresh = NO;

		if (index < argc && strcmp(argv[index], "--check") == 0) { check = YES; index++; }
		else if (index < argc && strcmp(argv[index], "--plugins") == 0) { plugins = YES; index++; }
		else if (index < argc && strcmp(argv[index], "--fresh") == 0) { fresh = YES; index++; }
		else if (index + 1 < argc && strcmp(argv[index], "--register") == 0) { register_path = argv[index + 1]; index += 2; }
		else if (index + 1 < argc && strcmp(argv[index], "--app") == 0) { app_id = argv[index + 1]; index += 2; }
		else if (index < argc && strncmp(argv[index], "--app=", 6) == 0) { app_id = argv[index] + 6; index++; }
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
		load_launchservices();
		if (app_id != NULL) {
			report_app(app_id);
			return 0;
		}
		report("before", plugin);
		probe_points();
		if (check) {
			say_plugin_record(plugin);
			return 0;
		}
		if (register_path != NULL) {
			register_plugin(register_path);
			report("after", plugin);
			say_plugin_record(plugin);
			return 0;
		}
		/* F6: --plugins re-registers each known file provider by its bundle and prints the record; it does
		 * not rebuild the database (that is the default path below, which still runs after it). */
		if (plugins) {
			reregister_file_providers(plugin);
			report("after --plugins", plugin);
			return 0;
		}
		if (fresh) {
			move_launchservices_stores();
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
		say_plugin_record(plugin);
	}
	return 0;
}
