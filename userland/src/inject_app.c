/*
 * libhakios.app.dylib: code of ours inside an application, to find out why it dies (see inject.c
 * for how it gets there; build.sh's INJECT names the programs).
 *
 * An application that is launched and gone again in a third of a second leaves nothing behind here:
 * there is no crash reporter in this system, and what it writes on its standard error goes nowhere.
 * This gives it a standard error -- the console -- so that the Swift runtime's last words are read,
 * and says the frames of the stack when a signal kills it, under the dyld shared cache's slide,
 * which `ipsw dyld a2s <cache> <address>` turns back into names.
 */
#include <dispatch/dispatch.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL

static uintptr_t slide;
static int console = -1;
static const char *who = "?";

static void
say(const char *text, size_t length)
{
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, text, length);
}

static void
killed(int number, siginfo_t *info, void *context)
{
	void *frames[32];
	char line[900];
	int n = backtrace(frames, 32), k;

	(void)context;
	k = snprintf(line, sizeof(line), "hakios: %s: SIGNAL %d at %p (the cache's slide %#lx), the stack:", who, number,
	    info != NULL ? info->si_addr : NULL, (unsigned long)slide);
	for (int i = 0; i < n && k < (int)sizeof(line) - 24; i++) {
		k += snprintf(line + k, sizeof(line) - (size_t)k, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
	}
	line[k++] = '\n';
	say(line, (size_t)k);
	signal(number, SIG_DFL);
	raise(number);
}

/*
 * Two things an application does on its way to a download, said out loud. Sileo queues a package and
 * nothing happens: no connection is made and no error is shown, so the question is how far it gets --
 * does it ask UIKit for a background task (a round trip to another daemon, which can wait for ever if
 * that daemon does not answer), and does it ever start a URL session task, for what address.
 */
static unsigned long (*begin_task)(id, SEL, id);
static void (*resume_task)(id, SEL);

static unsigned long
watched_begin_task(id self, SEL cmd, id handler)
{
	unsigned long task;
	char line[160];
	int n;

	n = snprintf(line, sizeof(line), "hakios: %s: asking UIKit for a background task\n", who);
	say(line, (size_t)(n > 0 ? n : 0));
	task = begin_task(self, cmd, handler);
	n = snprintf(line, sizeof(line), "hakios: %s: the background task is %lu\n", who, task);
	say(line, (size_t)(n > 0 ? n : 0));
	return task;
}

static void
watched_resume_task(id self, SEL cmd)
{
	const char *(*string)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *url = "?";
	char line[700];
	id request, address, text;
	int n;

	request = get(self, sel_registerName("originalRequest"));
	address = request != NULL ? get(request, sel_registerName("URL")) : NULL;
	text = address != NULL ? get(address, sel_registerName("absoluteString")) : NULL;
	if (text != NULL) {
		url = string(text, sel_registerName("UTF8String"));
	}
	n = snprintf(line, sizeof(line), "hakios: %s: starting a session task for %s\n", who,
	    url != NULL ? url : "?");
	say(line, (size_t)(n > 0 ? n : 0));
	resume_task(self, cmd);
}

/*
 * One setting of Sileo's, set for it at every start: "Show Packages From Canister" (ShowProvisional,
 * on by default). Canister is the search service Sileo asks about packages, and where its servers
 * cannot be reached the asking never stops -- 3810 session tasks in two minutes in one run here,
 * and the download of a package never gets its turn among them. With the setting off Sileo keeps to
 * the repositories themselves, which work. The prefix lives in memory and is new at every boot, so
 * there is nowhere to keep this but here. It is done on the main queue, not here in the library's
 * own initializer: at that point Foundation is loaded but has not run its own initializers yet, and
 * asking it for the defaults then kills the application before it says a word.
 */
static void
keep_to_the_repositories(void)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*from_text)(id, SEL, const char *) = (id (*)(id, SEL, const char *))objc_msgSend;
	void (*set_bool)(id, SEL, signed char, id) = (void (*)(id, SEL, signed char, id))objc_msgSend;
	id defaults = (id)objc_getClass("NSUserDefaults");
	id key = from_text((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "ShowProvisional");

	if (defaults == NULL || key == NULL) {
		return;
	}
	defaults = get(defaults, sel_registerName("standardUserDefaults"));
	if (defaults != NULL) {
		set_bool(defaults, sel_registerName("setBool:forKey:"), 0, key);
		say("hakios: Sileo: its Canister search is turned off (it is not reachable here)\n", 73);
	}
}

/*
 * apt-get install, not its simulation (-s...): the one that ends in "Unable to fetch some archives"
 * when Sileo runs it and installs the package when a terminal runs the same command.
 */
static int
apt_install(char *const argv[])
{
	int install = 0;

	if (argv == NULL || argv[0] == NULL || strcmp(argv[0], "apt-get") != 0) {
		return 0;
	}
	for (int i = 1; argv[i] != NULL; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 's') {
			return 0;
		}
		install |= strcmp(argv[i], "install") == 0;
	}
	return install;
}

/*
 * What apt says goes to Sileo, which shows a line of it at most. Run the same apt-get, with the same
 * descriptors, credential and environment, under a shell that first says on the console who it runs
 * as, what lies in apt's archives and what Sileo's index says of the package, and afterwards how apt
 * ended and what the archives hold then. This is how "Unable to fetch some archives" was found: the
 * archives were empty, Sileo's /bin/mv having never run.
 */
static int
spawn_apt_said(pid_t *pid, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
    char *const argv[], char *const envp[])
{
	static char script[] =
	    "exec 7>/dev/console; J=/var/jb\n"
	    "echo \"hakios: apt-get runs as $($J/usr/bin/id), in $(pwd)\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives $J/var/lib/apt/sileolists >&7 2>&1\n"
	    "for a in \"$@\"; do case $a in -*|install) ;; *) p=${a%%=*}\n"
	    "  echo \"hakios: $p in sileolists:\" >&7\n"
	    "  $J/usr/bin/grep -h -A30 \"^Package: $p\\$\" $J/var/lib/apt/sileolists/*Packages 2>&1 | $J/usr/bin/head -40 >&7 ;; esac; done\n"
	    "$J/usr/bin/apt-get \"$@\"\n"
	    "s=$?; echo \"hakios: apt-get exits $s; its archives after:\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives >&7 2>&1; exit $s\n";
	char *wrapped[96];
	int k = 0;

	for (int i = 0; envp != NULL && envp[i] != NULL; i++) {
		char line[600];
		int n = snprintf(line, sizeof(line), "hakios: %s: apt-get's environment: %s\n", who, envp[i]);

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	wrapped[k++] = "sh";
	wrapped[k++] = "-c";
	wrapped[k++] = script;
	wrapped[k++] = "apt-get";
	for (int i = 1; argv[i] != NULL && k < 95; i++) {
		wrapped[k++] = argv[i];
	}
	wrapped[k] = NULL;
	return posix_spawn(pid, "/var/jb/bin/sh", actions, attr, wrapped, envp);
}

/*
 * Every program the application starts, with its arguments. Sileo does everything to the system by
 * spawning its set-user-ID helper, which execs apt or dpkg; the arguments are the whole question
 * when apt's answer to Sileo differs from apt's answer on the command line.
 */
static int
watched_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "hakios: %s: (uid %d) spawns %s", who, (int)getuid(),
	    path != NULL ? path : "?");

	for (int i = 0; argv != NULL && argv[i] != NULL && n < (int)sizeof(line) - 40; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", argv[i]);
	}
	/*
	 * The size of what it is about to move, while the file is still there: apt refuses the
	 * package Sileo puts in its archives ("Unable to fetch some archives") and takes the very
	 * same package when apt itself fetched it, so the first thing to know is whether the file
	 * is whole.
	 */
	if (argv != NULL && argv[0] != NULL && strcmp(argv[0], "mv") == 0 && argv[1] != NULL) {
		struct stat st;

		if (stat(argv[1], &st) == 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [%lld bytes]", (long long)st.st_size);
		} else {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [no such file]");
		}
	}
	n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
	say(line, (size_t)n);
	if (apt_install(argv)) {
		return spawn_apt_said(pid, actions, attr, argv, envp);
	}
	n = posix_spawn(pid, path, actions, attr, argv, envp);
	if (n != 0) {
		char said[300];
		int k = snprintf(said, sizeof(said), "hakios: %s: ... and that spawn failed: %s\n", who, strerror(n));

		say(said, (size_t)(k < (int)sizeof(said) ? k : (int)sizeof(said) - 1));
	}
	return n;
}

/*
 * A file the application moves or copies itself (NSFileManager, under Swift's FileManager), when it
 * fails: from where, to where, and what the error says. Sileo moves each download into place this
 * way, and says only "The operation couldn't be completed. No such file or directory".
 */
static const char *
text_of(id object)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id text;

	if (object == NULL) {
		return "(nil)";
	}
	text = (class_respondsToSelector(object_getClass(object), sel_registerName("path")) ?
	    get(object, sel_registerName("path")) : get(object, sel_registerName("description")));
	return text != NULL ? utf8(text, sel_registerName("UTF8String")) : "?";
}

static signed char (*move_url)(id, SEL, id, id, id *), (*copy_url)(id, SEL, id, id, id *);
static signed char (*move_path)(id, SEL, id, id, id *), (*copy_path)(id, SEL, id, id, id *);

static signed char
file_said(const char *what, signed char ok, id from, id to, id error)
{
	if (!ok) {
		char line[1400];
		int n = snprintf(line, sizeof(line), "hakios: %s: %s %s -> %s failed: %s\n", who, what, text_of(from),
		    text_of(to), error != NULL ? text_of(error) : "(no error given)");

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	return ok;
}

static signed char
watched_move_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_move_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} spawn_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched_spawn, (const void *)posix_spawn
};

/* Put ours in the place of a method, keeping the one that was there. */
static void
watch(const char *name, const char *selector, IMP ours, void *theirs)
{
	Class class = objc_getClass(name);
	Method method = class != NULL ? class_getInstanceMethod(class, sel_registerName(selector)) : NULL;

	if (method != NULL && method_getImplementation(method) != ours) {
		*(IMP *)theirs = method_setImplementation(method, ours);
	}
}

/*
 * CoreAnimation's 16.7 ms floor, in an application.
 *
 * fb_120=1 was answered for backboardd and SpringBoard only (src/inject.c, src/inject_sb.c); the
 * floor is per bundle, though -- QuartzCore in THIS process reads its own Info.plist through
 * CFBundleGetValueForInfoDictionaryKey -- so scrolling inside Settings, Safari, Music or Files
 * stayed at 60 a second while SpringBoard's animations ran at 120 (tester, TEST3: "120 fps is smooth
 * but not while scrolling"). The port's identity is iPhone10,3, whose application bundles do not
 * carry the key. Answer both spellings with true here too when the boot asked for 120.
 */
static int
fb120_wanted(void)
{
	static int wanted = -1;

	if (wanted < 0) {
		char arguments[4096] = "";
		size_t size = sizeof(arguments) - 1;

		wanted = sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
		    strstr(arguments, "fb_120=1") != NULL;
	}
	return wanted;
}

static CFTypeRef
fb120_info_value(CFBundleRef bundle, CFStringRef key)
{
	static CFTypeRef (*original)(CFBundleRef, CFStringRef);

	if (original == NULL) {
		original = (CFTypeRef (*)(CFBundleRef, CFStringRef))dlsym(RTLD_NEXT, "CFBundleGetValueForInfoDictionaryKey");
	}
	if (fb120_wanted() && key != NULL &&
	    (CFEqual(key, CFSTR("CADisableMinimumFrameDurationOnPhone")) ||
	     CFEqual(key, CFSTR("CADisableMinimumFrameDuration")))) {
		return kCFBooleanTrue;
	}
	return original != NULL ? original(bundle, key) : NULL;
}

#define HAKIOS_INTERPOSE(replacement, replacee) \
	__attribute__((used)) static struct { const void *replacement; const void *replacee; } \
	_hakios_interpose_##replacee __attribute__((section("__DATA,__interpose"))) = \
	    { (const void *)(uintptr_t) &replacement, (const void *)(uintptr_t) &replacee }

HAKIOS_INTERPOSE(fb120_info_value, CFBundleGetValueForInfoDictionaryKey);

/*
 * Boot-arg t120=1: 120 Hz while a finger is down, in an application.
 *
 * The key above (CADisableMinimumFrameDurationOnPhone) lifts CoreAnimation's 16.7 ms floor, and that
 * is all an application needs on a ProMotion phone. It is not all this port needs: QuartzCore also
 * keeps a frame-rate restriction for a display it does not take for a ProMotion one, and an iPhone
 * X's identity is not. Whatever UIKit asks for -- UIScrollView's update request, a CADisplayLink --
 * it is clamped to the display's "maximum" frames per second, which is 60 for this identity; menu
 * transitions still run at 120 because CoreAnimation's own animation clock is not a display link.
 * (QuartzCore 20F66: the switch names are at 0x1d5b57720, the answer bytes at 0x1e1f6b790, index 119
 * is CA_FORCE_120HZ = 0x1e1f6b807, read at 0x18828b294 and 0x188306184. src/inject.c has the long
 * version.)
 *
 * Two things are done, both under t120=1:
 *   1. the restriction bytes are written in QuartzCore (uuid-checked, string-checked), as in
 *      src/inject.c; and
 *   2. UIScreen's maximumFramesPerSecond answers 120, so UIKit's own range
 *      (CAFrameRateRange/preferredFrameRateRangeMaximum built from it) is not built around 60 in
 *      the first place.
 */
static const unsigned char t120_quartzcore_uuid[16] = {
	0xdc, 0x2d, 0xd4, 0xc1, 0x78, 0x66, 0x34, 0x43, 0x80, 0x50, 0xfc, 0x91, 0x5a, 0x02, 0x04, 0xc1,
};

#define T120_QC_SETTINGS        0x59cf0790UL
#define T120_QC_MARKER          0x31a55bUL      /* 0x18859555b: "CA_FORCE_120HZ" */
#define T120_QC_FORCE_120HZ     119
#define T120_QC_ALL_FRAME_RATES 113
#define T120_QC_RESTRICTION     118
#define T120_QC_FLOOR           0x54cc4e28UL   /* 0x1dcf3fe28: the cached answer of
                                                     CADisableMinimumFrameDurationOnPhone */

static void
sayf(const char *format, ...)
{
	char line[400];
	va_list arguments;
	int n;

	va_start(arguments, format);
	n = vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (n > 0) {
		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static int
t120_wanted(void)
{
	static int wanted = -1;

	if (wanted < 0) {
		char arguments[4096] = "";
		size_t size = sizeof(arguments) - 1;

		wanted = sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
		    strstr(arguments, "t120=1") != NULL;
	}
	return wanted;
}

static const struct mach_header_64 *
t120_image(const unsigned char uuid[16])
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(i);
		const unsigned char *command;

		if (header == NULL || header->magic != MH_MAGIC_64) {
			continue;
		}
		command = (const unsigned char *)(header + 1);
		for (uint32_t c = 0; c < header->ncmds; c++) {
			const struct load_command *lc = (const struct load_command *)command;

			if (lc->cmdsize < sizeof(*lc)) {
				break;
			}
			if (lc->cmd == LC_UUID && memcmp(((const struct uuid_command *)lc)->uuid, uuid, 16) == 0) {
				return header;
			}
			command += lc->cmdsize;
		}
	}
	return NULL;
}

static void
t120_poke(void)
{
	const struct mach_header_64 *quartzcore;
	volatile unsigned char *settings, *floor;

	if (!t120_wanted()) {
		return;
	}
	quartzcore = t120_image(t120_quartzcore_uuid);
	if (quartzcore == NULL) {
		return;
	}
	if (memcmp((const char *)quartzcore + T120_QC_MARKER, "CA_FORCE_120HZ", 14) != 0) {
		sayf("hakios: %s: t120=1: QuartzCore at %p has no CA_FORCE_120HZ where 20F66 has it; left alone\n", who,
		    quartzcore);
		return;
	}
	settings = (volatile unsigned char *)quartzcore + T120_QC_SETTINGS;
	if (settings[T120_QC_FORCE_120HZ] != 1 || settings[T120_QC_RESTRICTION] != 0 ||
	    settings[T120_QC_ALL_FRAME_RATES] != 1) {
		settings[T120_QC_FORCE_120HZ] = 1;
		settings[T120_QC_ALL_FRAME_RATES] = 1;
		settings[T120_QC_RESTRICTION] = 0;
		sayf("hakios: %s: t120=1: QuartzCore's frame-rate restriction is off (CA_FORCE_120HZ=1, "
		    "CA_ENABLE_ALL_FRAME_RATES=1, CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0)\n", who);
	}
	floor = (volatile unsigned char *)quartzcore + T120_QC_FLOOR;
	if (*floor == 0) {
		*floor = 1;
		sayf("hakios: %s: t120=1: CADisableMinimumFrameDurationOnPhone's cached answer is 1 (byte %p)\n", who,
		    floor);
	}
}

/* +[UIScreen maximumFramesPerSecond] (UIKitCore) and its private spelling, both answered 120. */
static long
t120_maximum_frames(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 120;
}

static void
t120_answer_screen_rate(const char *class_name, const char *selector)
{
	Class cls = objc_getClass(class_name);
	Method method = cls != NULL ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (method == NULL || method_getImplementation(method) == (IMP)t120_maximum_frames) {
		return;
	}
	method_setImplementation(method, (IMP)t120_maximum_frames);
	sayf("hakios: %s: t120=1: -[%s %s] answers 120\n", who, class_name, selector);
}

/*
 * Boot-arg t120_link=N (with t120=1): what an application asks a display link for, and what it is
 * answered.
 *
 * t120=1 lifts QuartzCore's frame-rate restriction and makes UIScreen answer 120, and SpringBoard's
 * own animations then run at 119 a second on this port (TEST9: 597 frames in 5004 ms, 576 of 597
 * intervals under 10 ms). Scrolling in an application does not: 60 to 74 (TEST9: 346 frames in
 * 5000 ms while the framebuffer driver, GPU fence and copy together, was busy less than half of
 * every frame -- the frames were not asked for, they were not held up). What is left to look at is
 * the rate the application asks its own display link for, so this says it and, when told to, answers
 * it with the display's:
 *
 *   1: say the first 8 requests of a process, change nothing (the measurement);
 *   2: and give a scroll view's link 120 where it asked for 60 or more (UIScrollView, UITableView,
 *      UICollectionView, WKScrollView: the target's class name has "ScrollView" in it);
 *   3: and every link that asked for 60 or more, scroll view or not.
 *
 * A link that asked for less than 60 (a 30 or a 24 fps animation) is left alone at every level: the
 * point is the scroll, not to make everything in the system cost what 120 costs in heat and power.
 * The five-second "the frames that came late ... neither" line in the kernel says whether this is
 * even the right place: those are the frames nothing in the driver or the GPU delayed.
 */
typedef struct { float minimum, maximum, preferred; } t120_range;

static void (*link_rate_original)(id, SEL, long);
static void (*link_range_original)(id, SEL, t120_range);
static id    (*link_factory_original)(id, SEL, id, SEL);
static int   t120_link_said;
static unsigned t120_link_changed;      /* links whose rate this raised, for the next 5 s line's sake */

#define T120_LINKS      64
static struct { id link; unsigned char scroll; } t120_link_table[T120_LINKS];
static unsigned t120_link_next;

static int
t120_link_level(void)
{
	static int level = -1;

	if (level < 0) {
		char arguments[4096] = "";
		size_t size = sizeof(arguments) - 1;

		level = 0;
		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			const char *at = strstr(arguments, "t120_link=");

			if (at != NULL && at[10] >= '1' && at[10] <= '3') {
				level = at[10] - '0';
			}
		}
		if (level != 0) {
			sayf("hakios: %s: t120_link=%d: display links %s\n", who, level,
			    level == 1 ? "are to be said, not changed" :
			    level == 2 ? "of scroll views are answered 120 where they asked for 60 or more" :
			    "are answered 120 where they asked for 60 or more");
		}
	}
	return level;
}

/* Does the link's target have "ScrollView" in its class name (UIScrollView and everything under it)? */
static int
t120_scroll_class(id target)
{
	for (Class cls = target != nil ? object_getClass(target) : Nil; cls != Nil; cls = class_getSuperclass(cls)) {
		const char *name = class_getName(cls);

		if (name != NULL && strstr(name, "ScrollView") != NULL) {
			return 1;
		}
	}
	return 0;
}

static void
t120_link_remember(id link, int scroll)
{
	if (link == nil) {
		return;
	}
	t120_link_next = (t120_link_next + 1) % T120_LINKS;
	t120_link_table[t120_link_next].link = link;
	t120_link_table[t120_link_next].scroll = (unsigned char) (scroll != 0);
}

/* 1 = a scroll view's, 0 = somebody else's, -1 = not seen being made (fb: no factory call) */
static int
t120_link_scroll(id link)
{
	for (unsigned i = 0; i < T120_LINKS; i++) {
		if (t120_link_table[i].link == link) {
			return t120_link_table[i].scroll;
		}
	}
	return -1;
}

/* +[CADisplayLink displayLinkWithTarget:selector:]: remember what the link was made for. */
static id
t120_link_factory(id self, SEL cmd, id target, SEL selector)
{
	id link = link_factory_original != NULL ? link_factory_original(self, cmd, target, selector) : nil;

	t120_link_remember(link, t120_scroll_class(target));
	return link;
}

static void
t120_link_say(id link, const char *what, const char *asked, const char *answered)
{
	int scroll = t120_link_scroll(link);

	if (t120_link_said >= 8) {
		return;
	}
	t120_link_said++;
	sayf("hakios: %s: t120_link: %s%s: %s -> %s\n", who, what,
	    scroll == 1 ? " on a scroll view's link" : scroll == 0 ? " on another link" : "", asked, answered);
}

static void
t120_link_rate(id self, SEL cmd, long rate)
{
	int level = t120_link_level();
	long wanted = rate;
	char asked[48], answered[48];

	if (level != 0 && ((level == 2 && t120_link_scroll(self) == 1) || level >= 3) && rate >= 60 && rate < 120) {
		wanted = 120;
		t120_link_changed++;
	}
	if (level != 0) {
		snprintf(asked, sizeof(asked), "setPreferredFramesPerSecond: %ld a second", rate);
		snprintf(answered, sizeof(answered), "%ld a second", wanted);
		t120_link_say(self, "the rate", asked, answered);
	}
	if (link_rate_original != NULL) {
		link_rate_original(self, cmd, wanted);
	}
}

static void
t120_link_range(id self, SEL cmd, t120_range range)
{
	int level = t120_link_level();
	t120_range wanted = range;
	char asked[64], answered[64];

	if (level != 0 && ((level == 2 && t120_link_scroll(self) == 1) || level >= 3) && range.preferred >= 60 &&
	    range.preferred < 120) {
		wanted.minimum = range.minimum < 120 ? range.minimum : 120;
		wanted.maximum = 120;
		wanted.preferred = 120;
		t120_link_changed++;
	}
	if (level != 0) {
		snprintf(asked, sizeof(asked), "the range %g..%g, preferred %g", range.minimum, range.maximum, range.preferred);
		snprintf(answered, sizeof(answered), "%g..%g, preferred %g", wanted.minimum, wanted.maximum, wanted.preferred);
		t120_link_say(self, "the range", asked, answered);
	}
	if (link_range_original != NULL) {
		link_range_original(self, cmd, wanted);
	}
}

/* The class method; watch() below is for the instance ones. */
static void
t120_watch_class(const char *name, const char *selector, IMP ours, void *theirs)
{
	Class cls = objc_getClass(name);
	Method method = cls != NULL ? class_getClassMethod(cls, sel_registerName(selector)) : NULL;

	if (method != NULL && method_getImplementation(method) != ours) {
		*(IMP *)theirs = method_setImplementation(method, ours);
	}
}

/* Idempotent: called at the start and again after the process has settled (QuartzCore may be later). */
static void
t120_links(void)
{
	static int missing;

	if (t120_link_level() == 0) {
		return;
	}
	t120_watch_class("CADisplayLink", "displayLinkWithTarget:selector:", (IMP)t120_link_factory, &link_factory_original);
	watch("CADisplayLink", "setPreferredFramesPerSecond:", (IMP)t120_link_rate, &link_rate_original);
	watch("CADisplayLink", "setPreferredFrameRateRange:", (IMP)t120_link_range, &link_range_original);
	if (!missing && link_factory_original == NULL && link_rate_original == NULL && link_range_original == NULL) {
		missing = 1;
		sayf("hakios: %s: t120_link: CADisplayLink has none of the three where this was written for; left alone\n", who);
	}
}

static void
t120_again(void)
{
	t120_poke();
	t120_answer_screen_rate("UIScreen", "maximumFramesPerSecond");
	t120_answer_screen_rate("UIScreen", "_maximumFramesPerSecond");
	t120_answer_screen_rate("CADisplay", "maximumFramesPerSecond");
	t120_links();
}

static void
t120_setup(void)
{
	if (!t120_wanted()) {
		return;
	}
	setenv("CA_FORCE_120HZ", "1", 1);
	setenv("CA_ENABLE_ALL_FRAME_RATES", "1", 1);
	setenv("CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION", "0", 1);
	t120_again();
	/* UIKit is loaded before us in an application, but not necessarily in every program this
	 * library ends up in; so the answer is put in again after the process has settled. */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{
		t120_again();
	});
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{
		t120_again();
	});
}

__attribute__((constructor))
static void
hakios_app_init(void)
{
	static const int signals[] = { SIGTRAP, SIGABRT, SIGILL, SIGSEGV, SIGBUS, SIGSYS };
	struct sigaction action = { 0 };
	const char *name = getprogname();
	char line[200];
	size_t length = 0;
	int n;

	if (name == NULL) {
		return;
	}
	who = name;
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	t120_setup();
	/*
	 * An application's standard error goes nowhere here, and that is where the Swift runtime says
	 * what it is about to die of ("Fatal error: ..."), so it is pointed at the console.
	 */
	console = open("/dev/console", O_WRONLY);
	if (console >= 0) {
		dup2(console, 2);
	}
	action.sa_sigaction = killed;
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
	}
	if (strcmp(name, "Sileo") == 0) {
		dispatch_async(dispatch_get_main_queue(), ^{ keep_to_the_repositories(); });
	}
	watch("UIApplication", "beginBackgroundTaskWithExpirationHandler:", (IMP)watched_begin_task, &begin_task);
	/*
	 * NSURLSessionTask alone: its subclasses inherit the very same method, and watching it twice
	 * would leave ours as the "original" of itself -- a call that never returns.
	 */
	watch("NSURLSessionTask", "resume", (IMP)watched_resume_task, &resume_task);
	watch("NSFileManager", "moveItemAtURL:toURL:error:", (IMP)watched_move_url, &move_url);
	watch("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)watched_copy_url, &copy_url);
	watch("NSFileManager", "moveItemAtPath:toPath:error:", (IMP)watched_move_path, &move_path);
	watch("NSFileManager", "copyItemAtPath:toPath:error:", (IMP)watched_copy_path, &copy_path);
	n = snprintf(line, sizeof(line), "hakios: %s: started as uid %d (effective %d), watching for what kills it\n",
	    name, (int)getuid(), (int)geteuid());
	say(line, (size_t)(n > 0 ? n : 0));
}
