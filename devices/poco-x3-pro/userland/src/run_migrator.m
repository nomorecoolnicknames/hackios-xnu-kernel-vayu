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
#include <mach-o/dyld.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <string.h>
#include <sys/wait.h>
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

/*
 * F6: one migrator per process.
 *
 * FACT (TEST14, run/061845:157258-157268 and run/072836:156702-156778): the boot calls
 *   run_migrator 00LaunchServicesMigrator SystemAppMigrator MediaLibrary
 * and the log has a line for the first one only -- "00LaunchServicesMigrator: LSDatabaseMigrator,
 * performMigration" then "says 1 after 0.0 s" -- and nothing at all for SystemAppMigrator and
 * MediaLibrary, not even the "cannot load" line every failure path in run() prints. Whatever ended the
 * first migrator (a stdio close in DataMigration, a rebuild that invalidates the process, an exit) also
 * ended the run, so two of the three migrators the boot asks for have never run on this device.
 *
 * A migrator is a whole first-boot step: it is allowed to invalidate everything it touched, including
 * this process's view of LaunchServices and MobileInstallation. Running each one in a child of its own
 * keeps that from being the end of the list, and the parent prints the exit status of each child so the
 * next log says which ones ran even when they print nothing themselves.
 *
 * usage: run_migrator [--root] [--one] NAME...
 *        --one   run the names in this process (the child form; the default forks one child per name)
 */
static void
set_mobile(void)
{
	(void)setgroups(0, NULL);
	if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
		fprintf(stderr, "run_migrator: setgid/setuid: %s\n", strerror(errno));
		exit(1);
	}
	setenv("HOME", "/var/mobile", 1);
	setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
}

int
main(int argc, char **argv)
{
	int status = 0, index = 1;
	BOOL one = NO, root = NO;

	@autoreleasepool {
		if (index < argc && strcmp(argv[index], "--root") == 0) { root = YES; index++; }
		if (index < argc && strcmp(argv[index], "--one") == 0) { one = YES; index++; }
		if (!root && getuid() == 0) {
			set_mobile();
		}
		if (index >= argc) { fprintf(stderr, "run_migrator: usage: run_migrator [--root] [--one] NAME...\n"); return 2; }
		if (one) {
			for (; index < argc; index++) {
				status |= run([NSString stringWithUTF8String:argv[index]]);
			}
			return status;
		}
		for (; index < argc; index++) {
			const char *name = argv[index];
			char self[PATH_MAX];
			uint32_t size = sizeof(self);
			pid_t child;
			int child_status = 0;

			if (_NSGetExecutablePath(self, &size) != 0) {
				fprintf(stderr, "run_migrator: %s: this process has no path; running it here\n", name);
				status |= run([NSString stringWithUTF8String:name]);
				continue;
			}
			child = fork();
			if (child < 0) {
				fprintf(stderr, "run_migrator: %s: fork: %s; running it here\n", name, strerror(errno));
				status |= run([NSString stringWithUTF8String:name]);
				continue;
			}
			if (child == 0) {
				/* the child: keep the uid the parent already dropped to, and be this one migrator */
				if (root) {
					execl(self, self, "--root", "--one", name, (char *)NULL);
				} else {
					execl(self, self, "--one", name, (char *)NULL);
				}
				fprintf(stderr, "run_migrator: %s: execl %s: %s\n", name, self, strerror(errno));
				_exit(127);
			}
			if (waitpid(child, &child_status, 0) < 0) {
				fprintf(stderr, "run_migrator: %s: waitpid: %s\n", name, strerror(errno));
				status |= 1;
				continue;
			}
			if (WIFSIGNALED(child_status)) {
				fprintf(stderr, "run_migrator: %s: the child died on signal %d\n", name,
				    WTERMSIG(child_status));
				status |= 1;
			} else {
				fprintf(stderr, "run_migrator: %s: the child exited %d\n", name,
				    WEXITSTATUS(child_status));
				status |= WEXITSTATUS(child_status);
			}
		}
	}
	return status;
}
