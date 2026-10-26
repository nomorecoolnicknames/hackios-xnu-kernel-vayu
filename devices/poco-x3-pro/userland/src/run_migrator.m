/*
 * run_migrator: run iOS's own first-boot data migrators (/System/Library/DataClassMigrators/NAME.migrator) in
 * this process, the way com.apple.datamigrator does on a device: load the bundle, make its DataClassMigrator,
 * performMigration.
 *
 * Our boot never runs the data migration, and live (2026-10-25) two things are missing that migrators make:
 * LaunchServices knows no system plug-in, so Files' "On My iPhone" provider is not found (00LaunchServicesMigrator,
 * SystemAppMigrator); and the Music library's sort data is not there, so MediaPlayer lists no song
 * (MediaLibrary.migrator). Signed with datamigrator's own entitlements (taken from the system image at build time).
 *
 * usage: run_migrator [--root] NAME...   (as mobile, datamigrator's role account, unless --root)
 */
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <errno.h>
#include <grp.h>
#include <string.h>
#include <unistd.h>

#define MOBILE_ID	501
#define MIGRATORS	@"/System/Library/DataClassMigrators"

/* bundles without NSPrincipalClass: the class their binary holds */
static NSString *
class_name(NSString *name)
{
	return [@{ @"00LaunchServicesMigrator": @"LSDatabaseMigrator" } objectForKey:name];
}

static int
run(NSString *name)
{
	NSString *path = [NSString stringWithFormat:@"%@/%@.migrator", MIGRATORS, name];
	NSBundle *bundle = [NSBundle bundleWithPath:path];
	NSError *error = nil;
	Class class;
	id migrator;
	BOOL ok;
	NSDate *start;

	if (bundle == nil || ![bundle loadAndReturnError:&error]) {
		fprintf(stderr, "run_migrator: %s: cannot load: %s\n", [name UTF8String], [[error description] UTF8String] ?: "no bundle");
		return 1;
	}
	class = [bundle principalClass];
	if (class_name(name) != nil) class = NSClassFromString(class_name(name));
	if (class == Nil) { fprintf(stderr, "run_migrator: %s: no migrator class\n", [name UTF8String]); return 1; }
	migrator = [class alloc];
	migrator = [migrator respondsToSelector:@selector(initWithContext:)] ?
	    ((id (*)(id, SEL, id))objc_msgSend)(migrator, @selector(initWithContext:), @{}) : [migrator init];
	fprintf(stderr, "run_migrator: %s: %s, performMigration\n", [name UTF8String], class_getName(class));
	start = [NSDate date];
	@try {
		ok = ((BOOL (*)(id, SEL))objc_msgSend)(migrator, @selector(performMigration));
	} @catch (NSException *exception) {
		fprintf(stderr, "run_migrator: %s: %s: %s\n", [name UTF8String], [[exception name] UTF8String],
		    [[exception reason] UTF8String]);
		return 1;
	}
	fprintf(stderr, "run_migrator: %s: says %d after %.1f s\n", [name UTF8String], (int)ok, -[start timeIntervalSinceNow]);
	return ok ? 0 : 1;
}

int
main(int argc, char **argv)
{
	int status = 0, index = 1;

	@autoreleasepool {
		if (index < argc && strcmp(argv[index], "--root") == 0) {
			index++;
		} else if (getuid() == 0) {
			(void)setgroups(0, NULL);
			if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
				fprintf(stderr, "run_migrator: setgid/setuid: %s\n", strerror(errno)); return 1;
			}
			setenv("HOME", "/var/mobile", 1);
			setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
		}
		if (index >= argc) { fprintf(stderr, "run_migrator: usage: run_migrator [--root] NAME...\n"); return 2; }
		for (; index < argc; index++) {
			status |= run([NSString stringWithUTF8String:argv[index]]);
		}
	}
	return status;
}
