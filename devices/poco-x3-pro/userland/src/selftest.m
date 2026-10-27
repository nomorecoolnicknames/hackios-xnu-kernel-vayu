/*
 * selftest: the evidence an overnight run leaves by itself.
 *
 * A run is started with "selftest=1" among the kernel's boot-args; without it every mode below says
 * one SKIP line and exits, so the same image can be used for an ordinary run. The stages run at fixed
 * times after boot (kern.boottime, not the process's own start), each in a process of its own so a
 * crash in one cannot take the rest, and each writes one PASS or FAIL line with the prefix
 * "SELFTEST:" -- that prefix is the contract; a script can grep for it and nothing else:
 *
 *   SELFTEST: [+ 45.02 s] files PASS domain=listed+enabled file=/private/var/... size=74 seen=yes
 *   SELFTEST: [+ 80.11 s] music PASS medialibraryd=1 songs=1 play=AVPlayer t=1.23
 *   SELFTEST: [+140.03 s] mic PASS peak=412/32768 (-38.0 dBFS) rms=63.1 ... all equal no
 *   SELFTEST: [+170.24 s] camera PASS still=... (183402 bytes, jpeg) video=... (1312445 bytes, ftyp,mdat,moov)
 *   SELFTEST: [+205.00 s] summary files=PASS music=PASS mic=FAIL camera=PASS
 *   SELFTEST: [+205.00 s] RESULT 3/4 stages passed
 *
 * usage:
 *   selftest run [--source FILE] [--track NAME] [--seconds N] [--app-timeout N]
 *                the launchd job / rc.jobs.selftest driver: waits for each stage's time
 *   selftest files|music|mic|camera [--at SECONDS] [--results FILE] ...
 *                one stage, for rc.jobs or by hand
 *   selftest summary [--results FILE]
 *   selftest poweroff        (also needs selftest_poweroff=1)
 *   selftest --write-file NAME     internal: write the Files test file as mobile
 *   selftest --exec-mobile PROGRAM [ARG...]
 *                internal: drop to mobile (501) and exec; the files stage runs the
 *                provider's `fileproviderctl ls` this way, as the client Files.app is
 *
 * The log goes to stdout (the job's /dev/console) and to
 * /var/mobile/Media/Selftest/selftest-<bootsecs>.log; the machine-readable results to
 * /var/mobile/Media/Selftest/results-<bootsecs>.txt. Both live on the data volume, which
 * var_store writes into the flash when the run ends (boot-arg var_store=1), so the evidence
 * survives the power-off even when nobody reads the console.
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <MediaPlayer/MediaPlayer.h>
#import <objc/message.h>

#include "selftest_util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef RB_HALT
#define RB_HALT 0x08
#endif

#define MOBILE_ID		501
#define SELFTEST_DIR		"/var/mobile/Media/Selftest"
#define CAMERA_APP_LOG		"/var/mobile/Library/Logs/SelftestCamera.log"
#define FILEPROVIDERCTL		"/usr/bin/fileproviderctl"
#define PROVIDER_ID		"com.apple.FileProvider.LocalStorage"
#define FILEPROVIDER_GROUP	@"group.com.apple.FileProvider.LocalStorage"
#define MUSIC_SOURCE		"/redwood/share/music/Тёмный_принц-Отвратительный_король.mp3"
#define MUSIC_TRACK		"RDWD.mp3"

/* when each stage is due, seconds after boot; keep in step with userland/rc.jobs.selftest */
#define AT_FILES		45
#define AT_MUSIC		80
#define AT_MIC			140
#define AT_CAMERA		170
/* the power-off is last: the experiment's own jobs (GNSS, modem listen, sound) run to about +700 s */
#define AT_POWEROFF		780

#define MAX_SLEEP		1800.0
#define MAX_DETAIL_LINES	160

extern char **environ;

static int selftest_run_capture(char *const argv[], char *out, size_t cap, int timeout_seconds);

static char			g_bootargs[4096];
static int			g_have_bootargs;
static double			g_boot_epoch;		/* time(NULL) at boot (kern.boottime) */
static unsigned long long	g_run_id;		/* boot's seconds; names this run's files */
static char			g_results[PATH_MAX];
static char			g_logpath[PATH_MAX];
static FILE *			g_log;
static const char *		g_stage = "selftest";
static int			g_allow_duplicate;	/* --again: run a stage whose lock exists */

/* ---- time ----------------------------------------------------------------------------------- */

static double
now_seconds(void)
{
	struct timeval tv = { 0, 0 };

	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static double
uptime_now(void)
{
	return now_seconds() - g_boot_epoch;
}

static void
read_boot(void)
{
	struct timeval boot = { 0, 0 };
	size_t length = sizeof(boot), args_length = sizeof(g_bootargs);
	time_t fallback = time(NULL);

	if (sysctlbyname("kern.boottime", &boot, &length, NULL, 0) == 0 && boot.tv_sec > 0) {
		g_boot_epoch = (double)boot.tv_sec + (double)boot.tv_usec / 1e6;
		g_run_id = (unsigned long long)boot.tv_sec;
	} else {
		g_boot_epoch = (double)fallback;
		g_run_id = (unsigned long long)fallback;
	}
	g_bootargs[0] = '\0';
	g_have_bootargs = sysctlbyname("kern.bootargs", g_bootargs, &args_length, NULL, 0) == 0;
	if (g_have_bootargs) {
		g_bootargs[args_length < sizeof(g_bootargs) ? args_length : sizeof(g_bootargs) - 1] = '\0';
	}
	snprintf(g_results, sizeof(g_results), SELFTEST_DIR "/results-%llu.txt", g_run_id);
	snprintf(g_logpath, sizeof(g_logpath), SELFTEST_DIR "/selftest-%llu.log", g_run_id);
}

static void
sleep_seconds(double seconds)
{
	double until = now_seconds() + seconds;

	while (now_seconds() < until) {
		double left = until - now_seconds();

		usleep(left > 0.2 ? 200000 : (useconds_t)(left > 0 ? left * 1e6 : 0));
	}
}

/* ---- logging -------------------------------------------------------------------------------- */

/* mkdir -p, so /var/mobile/Media/Selftest can be made on a /var that is a fresh ramfs. */
static void
ensure_dirs(const char *path)
{
	char copy[PATH_MAX];

	if (path == NULL || strlen(path) >= sizeof(copy)) {
		return;
	}
	strcpy(copy, path);
	for (char *p = copy + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(copy, 0755);
			*p = '/';
		}
	}
	(void)mkdir(copy, 0755);
}

static void
log_open(void)
{
	struct stat status;

	ensure_dirs(SELFTEST_DIR);
	/* the data volume's own directory belongs to mobile; if this run made it, it must not stay root's */
	(void)chown("/var/mobile/Media", MOBILE_ID, MOBILE_ID);
	(void)chown(SELFTEST_DIR, MOBILE_ID, MOBILE_ID);
	(void)chmod(SELFTEST_DIR, 0755);
	if (stat(SELFTEST_DIR, &status) != 0) {
		return;
	}
	g_log = fopen(g_logpath, "a");
	if (g_log != NULL) {
		setvbuf(g_log, NULL, _IOLBF, 0);
		(void)chmod(g_logpath, 0644);
	}
}

static void
say_line(const char *body)
{
	fprintf(stdout, "SELFTEST: [+%7.2f s] %s\n", uptime_now(), body);
	fflush(stdout);
	if (g_log != NULL) {
		fprintf(g_log, "SELFTEST: [+%7.2f s] %s\n", uptime_now(), body);
		fflush(g_log);
	}
}

static void
say(const char *format, ...)
{
	char body[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(body, sizeof(body), format, arguments);
	va_end(arguments);
	say_line(body);
}

static void
stage_say(const char *format, ...)
{
	char text[1800], body[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	snprintf(body, sizeof(body), "%s: %s", g_stage, text);
	say_line(body);
}

/* One tool's output, line by line, prefixed so a grep for SELFTEST: has the evidence with it. */
static void
detail_text(const char *text)
{
	const char *line = text;
	int lines = 0;

	if (text == NULL) {
		return;
	}
	while (*line != '\0' && lines < MAX_DETAIL_LINES) {
		const char *end = strchr(line, '\n');
		size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
		char body[1600];

		if (length > 0) {
			if (length > 1000) {
				length = 1000;
			}
			snprintf(body, sizeof(body), "%s: | %.*s", g_stage, (int)length, line);
			say_line(body);
			lines++;
		}
		if (end == NULL) {
			break;
		}
		line = end + 1;
	}
	if (lines == MAX_DETAIL_LINES) {
		stage_say("| (more lines were cut)");
	}
}

/* ---- small helpers ------------------------------------------------------------------------- */

static int
read_file(const char *path, char *out, size_t size)
{
	ssize_t got;
	int fd;

	if (size == 0) {
		return -1;
	}
	out[0] = '\0';
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	got = read(fd, out, size - 1);
	close(fd);
	if (got < 0) {
		return -1;
	}
	out[got] = '\0';
	return (int)got;
}

/* The rest of the line that begins with NEEDLE (or follows it on the same line), trimmed. */
static int
line_after(const char *text, const char *needle, char *out, size_t size)
{
	const char *where = text != NULL ? strstr(text, needle) : NULL;
	size_t length;

	if (where == NULL || size == 0) {
		return 0;
	}
	where += strlen(needle);
	length = strcspn(where, "\r\n");
	if (length >= size) {
		length = size - 1;
	}
	memcpy(out, where, length);
	out[length] = '\0';
	return 1;
}

/* The value of a key in a key=value file (the camera app's result), one line per key. */
static int
value_of(const char *text, const char *key, char *out, size_t size)
{
	const char *p = text;
	size_t key_length = strlen(key);

	if (size == 0) {
		return 0;
	}
	out[0] = '\0';
	while (p != NULL && *p != '\0') {
		const char *end = strchr(p, '\n');
		size_t length = end != NULL ? (size_t)(end - p) : strlen(p);

		if (length > key_length && memcmp(p, key, key_length) == 0 && p[key_length] == '=') {
			size_t value_length = length - key_length - 1;

			if (value_length >= size) {
				value_length = size - 1;
			}
			memcpy(out, p + key_length + 1, value_length);
			out[value_length] = '\0';
			while (value_length > 0 && (out[value_length - 1] == '\r' || out[value_length - 1] == ' ')) {
				out[--value_length] = '\0';
			}
			return 1;
		}
		p = end != NULL ? end + 1 : NULL;
	}
	return 0;
}

static int
become_mobile(void)
{
	if (getuid() == MOBILE_ID) {
		return 0;
	}
	(void)setgroups(0, NULL);
	if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
		return -1;
	}
	(void)setenv("HOME", "/var/mobile", 1);
	(void)setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
	return 0;
}

/* How many processes named NAME are in the table; -1 when the table cannot be read. */
static int
process_count(const char *name)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0, count;
	char *buffer;
	int found = 0;

	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || size == 0) {
		return -1;
	}
	buffer = malloc(size);
	if (buffer == NULL) {
		return -1;
	}
	if (sysctl(mib, 3, buffer, &size, NULL, 0) != 0) {
		free(buffer);
		return -1;
	}
	count = size / sizeof(struct kinfo_proc);
	{
		struct kinfo_proc *processes = (struct kinfo_proc *)buffer;

		for (size_t i = 0; i < count; i++) {
			if (strncmp(processes[i].kp_proc.p_comm, name, sizeof(processes[i].kp_proc.p_comm)) == 0) {
				found++;
			}
		}
	}
	free(buffer);
	return found;
}

/* A process named NAME, through the kernel's table and, if that is refused, through /bin/ps -A. */
static int
process_seen(const char *name)
{
	int count = process_count(name);

	if (count >= 0) {
		return count > 0;
	}
	{
		char *argv[] = { (char *)"/bin/ps", (char *)"-A", NULL };
		char out[65536];
		int status;
		int found;

		stage_say("kern.proc.all is not readable (%s): asking /bin/ps -A", strerror(errno));
		memset(out, 0, sizeof(out));
		status = selftest_run_capture(argv, out, sizeof(out), 20);
		found = status >= 0 && strstr(out, name) != NULL;
		stage_say("/bin/ps -A: exit %d, the name is %s", status, found ? "there" : "NOT there");
		return found;
	}
}

static int
wait_for_process(const char *name, int seconds)
{
	double deadline = now_seconds() + seconds;
	double last = 0;

	for (;;) {
		if (process_seen(name)) {
			return 1;
		}
		if (now_seconds() >= deadline) {
			return 0;
		}
		if (now_seconds() - last > 10.0) {
			stage_say("still waiting for \"%s\" (%.0f s left)", name, deadline - now_seconds());
			last = now_seconds();
		}
		sleep_seconds(1.0);
	}
}

/* ---- running another program ---------------------------------------------------------------- */

/*
 * Run ARGV with its standard output and standard error in OUT (bounded, NUL-terminated), waiting no
 * longer than TIMEOUT_SECONDS; a child that overstays is killed. Returns its exit status, 128+signal
 * when it was killed, -1 when it could not be started and -2 when it was killed for the timeout.
 */
static int
selftest_run_capture(char *const argv[], char *out, size_t cap, int timeout_seconds)
{
	int fds[2], status = 0, timed_out = 0, spawn_error;
	size_t used = 0;
	double deadline;
	pid_t pid;
	posix_spawn_file_actions_t actions;

	if (cap > 0) {
		out[0] = '\0';
	}
	if (pipe(fds) != 0) {
		return -1;
	}
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, fds[0]);
	spawn_error = posix_spawn(&pid, argv[0], &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(fds[1]);
	if (spawn_error != 0) {
		close(fds[0]);
		if (cap > 0) {
			snprintf(out, cap, "%s: %s", argv[0], strerror(spawn_error));
		}
		return -1;
	}
	deadline = now_seconds() + (double)timeout_seconds;
	for (;;) {
		struct pollfd p = { fds[0], POLLIN, 0 };
		int left = (int)((deadline - now_seconds()) * 1000.0);
		char scratch[4096];
		ssize_t got;

		if (left < 0) {
			left = 0;
		}
		{
			int ready = poll(&p, 1, left);

			if (ready < 0) {
				if (errno == EINTR) {
					continue;
				}
				break;
			}
			if (ready == 0) {
				kill(pid, SIGKILL);
				timed_out = 1;
				break;
			}
		}
		/* read what is there; when OUT is full keep draining so the child is not blocked */
		got = read(fds[0], used + 1 < cap ? out + used : scratch,
		    used + 1 < cap ? cap - 1 - used : sizeof(scratch));
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (got == 0) {
			break;
		}
		if (used + 1 < cap) {
			used += (size_t)got;
			out[used] = '\0';
		}
	}
	close(fds[0]);
	deadline = now_seconds() + 10.0;
	for (;;) {
		pid_t waited = waitpid(pid, &status, WNOHANG);

		if (waited == pid) {
			break;
		}
		if (waited < 0 && errno != EINTR) {
			status = 0;
			break;
		}
		if (now_seconds() >= deadline) {
			kill(pid, SIGKILL);
			(void)waitpid(pid, &status, 0);
			timed_out = 1;
			break;
		}
		usleep(20000);
	}
	if (timed_out) {
		return -2;
	}
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}
	return -1;
}

static int
run_capture(char *const argv[], char *out, size_t cap, int timeout_seconds)
{
	return selftest_run_capture(argv, out, cap, timeout_seconds);
}

/* ---- the gate and the results -------------------------------------------------------------- */

static int
poweroff_allowed(void)
{
	return g_have_bootargs && st_boot_arg_is(g_bootargs, "selftest_poweroff", "1");
}

static void
result_write(const char *stage, int pass, const char *details)
{
	char clean[1200];
	size_t i;
	int fd;

	for (i = 0; details != NULL && details[i] != '\0' && i + 1 < sizeof(clean); i++) {
		clean[i] = details[i] == '\n' || details[i] == '\r' ? ' ' : details[i];
	}
	clean[i] = '\0';
	fd = open(g_results, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0) {
		stage_say("the results file %s cannot be written: %s", g_results, strerror(errno));
		return;
	}
	dprintf(fd, "%s=%s at=+%.1f details=%s\n", stage, pass ? "PASS" : "FAIL", uptime_now(), clean);
	close(fd);
	/* the music stage becomes mobile before it asks MediaPlayer; the file must be appendable then */
	(void)chmod(g_results, 0666);
}

static void
stage_result(int pass, const char *format, ...)
{
	char details[1200];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(details, sizeof(details), format, arguments);
	va_end(arguments);
	stage_say("%s%s%s", pass ? "PASS" : "FAIL", details[0] != '\0' ? " " : "", details);
	result_write(g_stage, pass, details);
}

/*
 * One stage per boot, by a lock in the results directory: the launchd driver and the rc.jobs list
 * are two ways to run the same stages, and a build that somehow carries both must not open the
 * camera twice or import the track twice. The lock's name carries the boot's seconds, so a rerun
 * after a reboot has its own. --again skips the lock (a stage retried by hand).
 */
static int
claim_stage(void)
{
	char path[PATH_MAX];
	int fd;

	if (g_allow_duplicate) {
		return 1;
	}
	snprintf(path, sizeof(path), SELFTEST_DIR "/stage-%llu-%s.lock", g_run_id, g_stage);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		if (errno == EEXIST) {
			stage_say("SKIP: another process already ran or runs the %s stage of this boot (%s)", g_stage,
			    path);
			return 0;
		}
		stage_say("the stage lock %s cannot be made: %s (carrying on)", path, strerror(errno));
		return 1;
	}
	dprintf(fd, "pid %d\n", (int)getpid());
	close(fd);
	return 1;
}

static void
wait_until(double at)
{
	double now = uptime_now();

	if (at > MAX_SLEEP) {
		stage_say("the stage is due at +%.0f s, which is further than this program will wait", at);
		return;
	}
	if (now < at) {
		stage_say("due at +%.0f s after boot; waiting %.1f s", at, at - now);
		while (uptime_now() < at) {
			sleep_seconds(0.2);
		}
	} else if (now > at + 1.0) {
		stage_say("due at +%.0f s after boot; starting at +%.1f s (late by %.1f s)", at, now, now - at);
		return;
	}
	stage_say("starting at +%.1f s (due at +%.0f s)", uptime_now(), at);
}

/* ---- files ---------------------------------------------------------------------------------- */

/*
 * The Files test file, written as mobile into the LocalStorage provider's own document storage
 * (its app group's "File Provider Storage"), the directory Files shows as "On My iPhone". Prints
 * what it did; the caller reads "readback=ok", "path ..." and "dir ..." out of it.
 */
static int
write_test_file(const char *name)
{
	NSFileManager *manager = [NSFileManager defaultManager];
	NSURL *container;
	NSString *directory, *path, *content;
	NSData *data, *back;
	NSError *error = nil;
	struct stat status;
	int ok;

	if (become_mobile() != 0) {
		printf("SELFTEST: files: becoming mobile (501:501) failed: %s\n", strerror(errno));
		return 2;
	}
	container = [manager containerURLForSecurityApplicationGroupIdentifier:FILEPROVIDER_GROUP];
	if (container == nil || [container path] == nil) {
		printf("SELFTEST: files: no container for %s (the app-group entitlement is missing?)\n",
		    [FILEPROVIDER_GROUP UTF8String]);
		return 2;
	}
	directory = [[container path] stringByAppendingPathComponent:@"File Provider Storage"];
	if (![manager fileExistsAtPath:directory]) {
		if (![manager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error]) {
			printf("SELFTEST: files: %s: %s\n", [directory UTF8String],
			    error != nil ? [[error localizedDescription] UTF8String] : "cannot make it");
			return 2;
		}
		printf("SELFTEST: files: made %s\n", [directory UTF8String]);
	}
	path = [directory stringByAppendingPathComponent:[NSString stringWithUTF8String:name]];
	content = [NSString stringWithFormat:@"selftest files\nrun=%llu\nuid=%d\nwritten by selftest --write-file\n",
	    g_run_id, (int)getuid()];
	data = [content dataUsingEncoding:NSUTF8StringEncoding];
	if (![data writeToFile:path options:0 error:&error]) {
		printf("SELFTEST: files: %s: %s\n", [path UTF8String],
		    error != nil ? [[error localizedDescription] UTF8String] : "cannot write it");
		return 1;
	}
	back = [NSData dataWithContentsOfFile:path options:0 error:&error];
	memset(&status, 0, sizeof(status));
	(void)stat([path fileSystemRepresentation], &status);
	ok = back != nil && [back isEqualToData:data] && status.st_size == (off_t)[data length];
	printf("SELFTEST: files: dir %s\n", [directory UTF8String]);
	printf("SELFTEST: files: path %s\n", [path UTF8String]);
	printf("SELFTEST: files: wrote %lu bytes, read back %lu, stat size %lld, owner %d:%d, compare %s\n",
	    (unsigned long)[data length], (unsigned long)[back length], (long long)status.st_size,
	    (int)status.st_uid, (int)status.st_gid, ok ? "equal" : "DIFFERENT");
	printf("SELFTEST: files: readback=%s\n", ok ? "ok" : "MISMATCH");
	return ok ? 0 : 1;
}

/*
 * What `fileproviderctl domain list` says about the LocalStorage domain: LISTED when its name or
 * identifier is in the text, ENABLED from the "(disabled)"/"(hidden)" word that follows it. The
 * list is one entry over several lines, so the word is looked for on the next three lines only.
 *
 * R16: the list is the FPProviderDomain list (the object fp_enable's providerDomainForIdentifier: returns),
 * and it prints "(hidden)" -- not "(disabled)" -- for a provider that is userEnabled=YES and hiddenByUser=YES.
 * TEST10 read that as "not enabled" and the stage failed on a state whose only problem was the hidden flag,
 * so "(hidden)" now counts as enabled; fp_enable's own summary line says which of the two it is.
 */
static void
parse_domain_list(const char *text, int *listed, int *enabled)
{
	const char *p = text;
	int window = 0;

	*listed = 0;
	*enabled = 0;
	while (*p != '\0') {
		const char *end = strchr(p, '\n');
		size_t length = end != NULL ? (size_t)(end - p) : strlen(p);
		char lower[1024];
		size_t copy = length < sizeof(lower) - 1 ? length : sizeof(lower) - 1;

		for (size_t i = 0; i < copy; i++) {
			char c = p[i];

			lower[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		}
		lower[copy] = '\0';
		if (strstr(lower, "localstorage") != NULL || strstr(lower, "local storage") != NULL) {
			*listed = 1;
			window = 3;
		}
		if (window > 0) {
			if (strstr(lower, "disabled") != NULL) {
				*enabled = 0;
			} else if (strstr(lower, "hidden") != NULL) {
				*enabled = 1;
			} else if (strstr(lower, "enabled") != NULL) {
				*enabled = 1;
			}
			window--;
		}
		p = end != NULL ? end + 1 : p + length;
	}
}

static int
stage_files(double at)
{
	static char list[65536], list2[65536], dump[65536], listing[65536], mobile_listing[16384], wrote[16384], enable[32768];
	static char again[32768];
	char name[80], path[PATH_MAX] = "", directory[PATH_MAX] = "", readback[32] = "";
	char line[1024], summary[512] = "";
	int listed = 0, enabled = 0, written = 0, seen = 0, mobile_seen = 0, exit_code, enable_exit = 0;
	int again_exit = 0;
	/* R16: fp_enable's own verdict (the provider object) next to fileproviderctl's list (the same object,
	 * printed). summary_seen is what decides which one the stage believes. */
	int summary_seen = 0, summary_ok = 0, summary_hidden = 0;
	int domain_ok;
	int extension_running;

	g_stage = "files";
	if (!claim_stage()) {
		return 0;
	}
	wait_until(at);
	stage_say("the domain list and dump are %s's own words; the file is written as mobile and then "
	    "looked for in the provider's listing", "fileproviderd");

	/*
	 * Before the daemon is asked anything: LaunchServices' and pkd's records of the system's app extensions.
	 * fileproviderd's FPDProviderDescriptor takes the LocalStorage domain's initial userEnabled from the
	 * extension's NSExtensionFileProviderEnabledByDefault attribute (FileProviderDaemon 0x1c3c65dd0:
	 * setEnabledByDefault: = attributes[...] boolValue, NO when the key is absent), so a record built
	 * without that key is a domain created disabled -- TEST7's "Local Storage (disabled)" while the appex's
	 * own Info.plist says true. ls_rebuild is that rebuild; it printed "LSApplicationWorkspace MISSING" in
	 * TEST7's camera stage because it was linked against Foundation only (fixed in build.sh), and it ran
	 * after this stage, never before it.
	 */
	if (access("/redwood/bin/ls_rebuild", X_OK) == 0) {
		char rebuild[16384];

		rebuild[0] = '\0';
		exit_code = run_capture((char *const[]){ (char *)"/redwood/bin/ls_rebuild", NULL },
		    rebuild, sizeof(rebuild), 60);
		stage_say("/redwood/bin/ls_rebuild (system app and plug-in database), before the daemon is asked: exit %d",
		    exit_code);
		detail_text(rebuild);
	} else {
		stage_say("ls_rebuild: /redwood/bin/ls_rebuild is not in this image");
	}

	/*
	 * R14: make pkd's own record of the appex explicit, and print it. -2001 is a provider the daemon
	 * cannot resolve, and the daemon resolves providers through LaunchServices/pkd, so the first thing
	 * the next log has to answer is whether pkd has the appex at all: "pluginkit -a" adds it when the
	 * record is missing (on TEST9's boot it was: media_import's NSExtension lookup answered "NOT FOUND"
	 * at 16:57:57, before ls_rebuild), "pluginkit -m -p com.apple.fileprovider-nonui" is the list the
	 * daemon is supposed to see. Both are best effort: a missing pluginkit is a line, not a failure.
	 */
	if (access("/usr/bin/pluginkit", X_OK) == 0) {
		static const char appex[] =
		    "/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex";
		char plugins[16384];

		plugins[0] = '\0';
		exit_code = run_capture((char *const[]){ (char *)"/usr/bin/pluginkit", (char *)"-a",
		    (char *)appex, NULL }, plugins, sizeof(plugins), 30);
		stage_say("pluginkit -a %s: exit %d (the appex in pkd's own database, before the daemon is asked)",
		    appex, exit_code);
		detail_text(plugins);
		plugins[0] = '\0';
		exit_code = run_capture((char *const[]){ (char *)"/usr/bin/pluginkit", (char *)"-m",
		    (char *)"-p", (char *)"com.apple.fileprovider-nonui", NULL },
		    plugins, sizeof(plugins), 30);
		stage_say("pluginkit -m -p com.apple.fileprovider-nonui: exit %d", exit_code);
		detail_text(plugins);
	} else {
		stage_say("pluginkit: /usr/bin/pluginkit is not in this image (pkd is asked through "
		    "ls_rebuild only)");
	}

	list[0] = '\0';
	exit_code = run_capture((char *const[]){ (char *)FILEPROVIDERCTL, (char *)"domain", (char *)"list", NULL },
	    list, sizeof(list), 30);
	stage_say("fileproviderctl domain list: exit %d", exit_code);
	detail_text(list);
	parse_domain_list(list, &listed, &enabled);
	stage_say("the LocalStorage domain: listed=%s enabled=%s", listed ? "yes" : "NO",
	    enabled ? "yes" : "NO");

	/*
	 * The enable. On a fresh /var the domain is listed but not enabled (TEST5Q at +45 s:
	 * "Local Storage (disabled)", and fileproviderctl ls -> -2001). /redwood/bin/fp_enable (device build,
	 * signed with fileproviderctl's entitlements) asks the daemon for the provider's domains, has
	 * removeAllDomainsForProviderIdentifier: create the default one when there is none or the one there is
	 * disabled, enables it, and drops to mobile (501) itself -- the daemon serves user/501 and Files.app is
	 * mobile. It waits for the daemon first: at +45 s launchd may be starting fileproviderd on this very
	 * connection (TEST5Q shows the spawn at 11:09:29 and a Cocoa 4099 right behind it). The verdict below
	 * is read from the second domain list, not from the first.
	 *
	 * R13 (TEST8): the wait is not the whole story -- the daemon may be up while the domain, its extension
	 * or the daemon's own state is not, so fp_enable is asked to repeat the pass itself (--retry=120) and,
	 * if the domain is still not enabled when the stage has done everything else, it is asked once more,
	 * later, and the domain list is read again (see "fp_enable again" below).
	 */
	if (access("/redwood/bin/fp_enable", X_OK) == 0) {
		enable[0] = '\0';
		enable_exit = run_capture((char *const[]){ (char *)"/redwood/bin/fp_enable", (char *)"--wait=20",
		    (char *)"--retry=120", (char *)"--restart=20", NULL }, enable, sizeof(enable), 260);
		stage_say("fp_enable (fileprovider domain enable, as mobile): exit %d", enable_exit);
		detail_text(enable);
		summary[0] = '\0';
		if (line_after(enable, "fp_enable: summary:", summary, sizeof(summary))) {
			summary_seen = 1;
			summary_ok = strstr(summary, "result=PASS") != NULL;
			summary_hidden = strstr(summary, "hidden=1") != NULL;
			stage_say("fp_enable's own verdict (R16, the provider object): %s", summary);
		} else {
			stage_say("fp_enable printed no summary line (an older binary): the fileproviderctl list below "
			    "is the verdict");
		}
	} else {
		stage_say("fp_enable: /redwood/bin/fp_enable is not in this image, the domain cannot be enabled here");
	}

	list2[0] = '\0';
	exit_code = run_capture((char *const[]){ (char *)FILEPROVIDERCTL, (char *)"domain", (char *)"list", NULL },
	    list2, sizeof(list2), 30);
	stage_say("fileproviderctl domain list, after the enable: exit %d", exit_code);
	detail_text(list2);
	if (exit_code == 0) {
		parse_domain_list(list2, &listed, &enabled);
	} else {
		stage_say("the second domain list failed; the first one's answer stands");
	}
	stage_say("the LocalStorage domain after the enable: listed=%s enabled=%s", listed ? "yes" : "NO",
	    enabled ? "yes" : "NO");

	dump[0] = '\0';
	exit_code = run_capture((char *const[]){ (char *)FILEPROVIDERCTL, (char *)"dump", (char *)PROVIDER_ID, NULL },
	    dump, sizeof(dump), 30);
	stage_say("fileproviderctl dump %s: exit %d", PROVIDER_ID, exit_code);
	detail_text(dump);

	snprintf(name, sizeof(name), "selftest-files-%llu.txt", g_run_id);
	wrote[0] = '\0';
	exit_code = run_capture((char *const[]){ (char *)"/redwood/bin/selftest", (char *)"--write-file", name, NULL },
	    wrote, sizeof(wrote), 60);
	stage_say("the test file, written as mobile: exit %d", exit_code);
	detail_text(wrote);
	(void)line_after(wrote, "SELFTEST: files: path ", path, sizeof(path));
	(void)line_after(wrote, "SELFTEST: files: dir ", directory, sizeof(directory));
	(void)line_after(wrote, "SELFTEST: files: readback=", readback, sizeof(readback));
	written = exit_code == 0 && strcmp(readback, "ok") == 0 && path[0] != '\0';
	if (written) {
		struct stat status;

		if (stat(path, &status) == 0) {
			stage_say("the file on disk: %s, %lld bytes, owner %d:%d, mode %o", path,
			    (long long)status.st_size, (int)status.st_uid, (int)status.st_gid,
			    status.st_mode & 07777);
		} else {
			stage_say("the file on disk: %s: %s", path, strerror(errno));
			written = 0;
		}
	}
	if (directory[0] != '\0' && access("/redwood/bin/vol_ls", X_OK) == 0) {
		static char listing2[16384];
		char *argv[] = { (char *)"/redwood/bin/vol_ls", (char *)"ls", directory, NULL };

		listing2[0] = '\0';
		(void)run_capture(argv, listing2, sizeof(listing2), 20);
		stage_say("/redwood/bin/vol_ls ls %s:", directory);
		detail_text(listing2);
	}

	/*
	 * The listing exactly as TEST5Q made it -- as root -- so this run's PASS/FAIL is comparable with
	 * that baseline: one variable changed (the domain enable), not the client too. The same call as
	 * mobile follows as extra evidence, because that is the client Files.app is; a difference between
	 * the two is logged and points at the client's connection, not at the domain.
	 */
	listing[0] = '\0';
	exit_code = run_capture((char *const[]){ (char *)FILEPROVIDERCTL, (char *)"ls", (char *)PROVIDER_ID, NULL },
	    listing, sizeof(listing), 30);
	stage_say("fileproviderctl ls %s (the TEST5Q call, as root): exit %d", PROVIDER_ID, exit_code);
	detail_text(listing);
	seen = exit_code == 0 && strstr(listing, name) != NULL;

	mobile_listing[0] = '\0';
	exit_code = run_capture((char *const[]){ (char *)"/redwood/bin/selftest", (char *)"--exec-mobile",
	    (char *)FILEPROVIDERCTL, (char *)"ls", (char *)PROVIDER_ID, NULL }, mobile_listing, sizeof(mobile_listing), 30);
	stage_say("fileproviderctl ls %s as mobile: exit %d", PROVIDER_ID, exit_code);
	detail_text(mobile_listing);
	mobile_seen = exit_code == 0 && strstr(mobile_listing, name) != NULL;
	if (seen != mobile_seen) {
		stage_say("the provider's listing differs by client: root=%s mobile=%s (the domain itself is %s)",
		    seen ? "lists the file" : "does not list it", mobile_seen ? "lists it" : "does not",
		    seen || mobile_seen ? "there" : "not the reason");
	}
	if (!seen && !mobile_seen) {
		stage_say("the provider did not list its storage from either client (the \"Could not find a matching "
		    "provider\" -2001 of the earlier diagnostics looks like this)");
	}
	/*
	 * R13: the same step again, later. Everything above took the files stage's own time (three
	 * fileproviderctl calls and a file written through the domain), so this is a second process, tens of
	 * seconds after the first, on the state the daemon has settled into since. It runs only when the domain
	 * is not enabled -- a working enable is not disturbed -- and the domain list is read once more after it,
	 * so the verdict below is the last word rather than the first pass's.
	 */
	if ((!listed || !enabled) && access("/redwood/bin/fp_enable", X_OK) == 0) {
		again[0] = '\0';
		again_exit = run_capture((char *const[]){ (char *)"/redwood/bin/fp_enable", (char *)"--wait=10",
		    (char *)"--retry=30", (char *)"--restart=20", NULL }, again, sizeof(again), 120);
		stage_say("fp_enable again, later (the domain was %s after the first pass): exit %d",
		    enabled ? "enabled" : "not enabled", again_exit);
		detail_text(again);
		if (line_after(again, "fp_enable: summary:", summary, sizeof(summary))) {
			summary_seen = 1;
			summary_ok = strstr(summary, "result=PASS") != NULL;
			summary_hidden = strstr(summary, "hidden=1") != NULL;
			stage_say("fp_enable's own verdict, second process: %s", summary);
		}
		list2[0] = '\0';
		exit_code = run_capture((char *const[]){ (char *)FILEPROVIDERCTL, (char *)"domain", (char *)"list", NULL },
		    list2, sizeof(list2), 30);
		stage_say("fileproviderctl domain list, after the second enable: exit %d", exit_code);
		detail_text(list2);
		if (exit_code == 0) {
			parse_domain_list(list2, &listed, &enabled);
			stage_say("the LocalStorage domain after the second enable: listed=%s enabled=%s",
			    listed ? "yes" : "NO", enabled ? "yes" : "NO");
		}
	}
	extension_running = process_count("LocalStorageFileProvider") > 0;
	stage_say("LocalStorageFileProvider: %s", extension_running ? "running" : "not seen (the ``ls`` above is the proof that matters)");
	/* R16: the verdict. fp_enable's summary describes the provider object -- enabled or not, hidden or
	 * not -- which is also the object fileproviderctl prints; it is believed when present. The list alone
	 * cannot tell an enabled-but-hidden provider (R16's TEST10 state, "(hidden)") from a working one, and
	 * it is empty for LocalStorage's default domain while the daemon is still using it. */
	domain_ok = summary_seen ? summary_ok : (listed && enabled);
	if (!domain_ok) {
		stage_say("the provider is still not enabled and visible after fp_enable (two processes, the second "
		    "one later): read its output above -- \"no connection to fileproviderd\" and \"no answer\" are "
		    "different faults (this program's FileProvider link, then the daemon), \"the proxy says it does "
		    "not answer\" means the call went out anyway (TEST8), an error 4 is a domain the daemon does not "
		    "have (the fallback reset creates it), \"hidden=1\" without an error is the hidden default domain "
		    "the daemon ignores at startup (R16: setHiddenByUser:NO did not take, its visibility may be "
		    "fixed), and a domain that is listed and disabled is the daemon's own descriptor (see ls_rebuild "
		    "above) -- then fileproviderd's log and whether LocalStorageFileProvider.appex is in the image");
	} else if (summary_seen && summary_hidden) {
		stage_say("the provider is enabled but hidden (R16): Files.app gets no location for it even though "
		    "fp_enable's enable calls answered without an error -- this is the remaining fault to chase");
	}

	{
		struct stat status;

		if (path[0] != '\0' && stat(path, &status) == 0) {
			snprintf(line, sizeof(line), "domain=%s+%s provider=%s fp_enable_exit=%d again_exit=%d file=%s "
			    "size=%lld readback=%s provider_ls=%s", listed ? "listed" : "NOT-listed",
			    enabled ? "enabled" : "NOT-enabled",
			    summary_seen ? (summary_ok ? "enabled+visible" : summary_hidden ? "enabled+hidden" :
			    "not-enabled") : "unreported", enable_exit, again_exit, path,
			    (long long)status.st_size, written ? "ok" : "no", seen ? "yes" : "NO");
		} else {
			snprintf(line, sizeof(line), "domain=%s+%s provider=%s fp_enable_exit=%d again_exit=%d file=%s "
			    "readback=%s provider_ls=%s", listed ? "listed" : "NOT-listed",
			    enabled ? "enabled" : "NOT-enabled",
			    summary_seen ? (summary_ok ? "enabled+visible" : summary_hidden ? "enabled+hidden" :
			    "not-enabled") : "unreported", enable_exit, again_exit, path[0] != '\0' ? path : "(none)",
			    written ? "ok" : "no", seen ? "yes" : "NO");
		}
	}
	stage_result(domain_ok && written && seen, "%s", line);
	return domain_ok && written && seen;
}

/* ---- music ---------------------------------------------------------------------------------- */

/*
 * What the music stage learned, for the result line and for a log that can be read without the
 * source: the last AVPlayerItem status and error, and how many audio outputs the session had.
 * TEST7's "status=failed" was this file's word for "did not play", read as if it were the item's own
 * status; the item's was AVPlayerItemStatusUnknown (0) for all twelve seconds, with no error at all.
 */
static long g_av_status = -1;
static char g_av_error[512];
static long g_av_outputs = -1;   /* outputs on the audio session's route */

static const char *
av_status_name(long status)
{
	if (status == AVPlayerItemStatusReadyToPlay) {
		return "readyToPlay";
	}
	if (status == AVPlayerItemStatusFailed) {
		return "failed";
	}
	if (status == AVPlayerItemStatusUnknown) {
		return "unknown";
	}
	return "none";
}

static const char *
error_text(NSError *error, char *buffer, size_t capacity)
{
	if (error == nil) {
		snprintf(buffer, capacity, "(none)");
	} else {
		snprintf(buffer, capacity, "%s %ld: %s", [[error domain] UTF8String], (long)[error code],
		    [[error localizedDescription] UTF8String]);
	}
	return buffer;
}

/*
 * AVFoundation tells AVPlayerItem about its status, and the player about timeControlStatus, through
 * the main queue: a stage that busy-waits in usleep() never drains it. In TEST7 the item sat at
 * AVPlayerItemStatusUnknown for the whole twelve seconds with item.error nil -- what a blocked main
 * queue looks like, and the same reason the other AVFoundation tools of this port wait by running
 * the run loop (play_test.m, video_test.m). Running it also lets AVURLAsset's asynchronous loads
 * finish, which is where the asset's own error would come from.
 */
static void
pump_main(double seconds)
{
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

static void
log_error_log(AVPlayerItem *item)
{
	NSArray *events = [item errorLog] != nil ? [[item errorLog] events] : nil;
	AVPlayerItemAccessLog *access = [item accessLog];

	stage_say("AVPlayer: errorLog %lu event(s), accessLog %lu event(s)",
	    (unsigned long)[events count], (unsigned long)[[access events] count]);
	for (AVPlayerItemErrorLogEvent *event in events) {
		stage_say("AVPlayer: errorLog: status %ld, domain %s, comment %s",
		    (long)[event errorStatusCode],
		    event.errorDomain != nil ? [event.errorDomain UTF8String] : "(none)",
		    event.errorComment != nil ? [event.errorComment UTF8String] : "(none)");
	}
}

/*
 * The asset's own answer, asked the way AVFoundation asks it: loadValuesAsynchronouslyForKeys with
 * the run loop turning while it happens. If none of the keys ever loads, the asset layer is the
 * fault; if they load and the item still stays unknown, the player has no render pipeline to build
 * (the audio route: see the HAL lines).
 */
static void
probe_asset(NSURL *url)
{
	AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
	char text[512];
	char *buffer = text;            /* a block cannot capture an array; a pointer to it, it can */
	__block volatile int loaded = 0;
	double started;

	if (asset == nil) {
		stage_say("AVURLAsset: nil for %s", [[url absoluteString] UTF8String]);
		return;
	}
	[asset loadValuesAsynchronouslyForKeys:@[ @"playable", @"duration", @"tracks" ] completionHandler:^{
		loaded = 1;
	}];
	started = now_seconds();
	while (!loaded && now_seconds() - started < 3.0) {
		pump_main(0.1);
	}
	stage_say("AVURLAsset %s: keys loaded %s after %.1f s", [[url absoluteString] UTF8String],
	    loaded ? "yes" : "NO", now_seconds() - started);
	void (^key_status)(NSString *key) = ^(NSString *key) {
		NSError *error = nil;
		long status = (long)[asset statusOfValueForKey:key error:&error];

		stage_say("AVURLAsset: %s status %ld (%s)", [key UTF8String], status,
		    error_text(error, buffer, sizeof(text)));
	};

	key_status(@"playable");
	key_status(@"duration");
	key_status(@"tracks");
	if (loaded) {
		stage_say("AVURLAsset: playable %d, duration %.3f s, %lu track(s)", (int)[asset isPlayable],
		    CMTimeGetSeconds([asset duration]), (unsigned long)[[asset tracks] count]);
	}
}

static void
join_ports(NSArray *ports, char *buffer, size_t capacity)
{
	size_t used = 0;

	buffer[0] = 0;
	for (AVAudioSessionPortDescription *port in ports) {
		int n = snprintf(buffer + used, capacity - used, "%s%s/%s", used > 0 ? ", " : "",
		    [[port portType] UTF8String], [[port portName] UTF8String]);

		if (n < 0 || (size_t)n >= capacity - used) {
			break;
		}
		used += (size_t)n;
	}
}

static long
log_audio_route(const char *when)
{
	AVAudioSession *session = [AVAudioSession sharedInstance];
	AVAudioSessionRouteDescription *route = [session currentRoute];
	char outputs[512], inputs[256];

	join_ports([route outputs], outputs, sizeof(outputs));
	join_ports([route inputs], inputs, sizeof(inputs));
	stage_say("the audio session's route %s: category %s, %lu output(s) [%s], %lu input(s) [%s], "
	    "%.0f Hz, I/O buffer %.4f s, volume %.2f", when, [[session category] UTF8String],
	    (unsigned long)[[route outputs] count], outputs, (unsigned long)[[route inputs] count], inputs,
	    [session sampleRate], [session IOBufferDuration], [session outputVolume]);
	g_av_outputs = (long)[[route outputs] count];
	return g_av_outputs;
}

/*
 * The route is built by the audio server out of the devices its HAL plug-ins publish, and one that
 * has just loaded can be a moment behind the session's activation: wait a few seconds before calling
 * the absence of a route a player failure (it is not: it is the reason there is nothing to play to).
 */
static void
wait_for_route(double seconds)
{
	AVAudioSession *session = [AVAudioSession sharedInstance];
	double started = now_seconds();
	long outputs = log_audio_route("after activation");

	while (outputs == 0 && now_seconds() - started < seconds) {
		pump_main(0.25);
		outputs = (long)[[session currentRoute] outputs].count;
	}
	if (outputs == 0) {
		stage_say("the audio session has NO OUTPUT ROUTE after %.1f s: mediaserverd has no output "
		    "device, so a player has nowhere to play to (the HAL plug-in did not load: see the HAL "
		    "lines)", now_seconds() - started);
		g_av_outputs = 0;
	} else {
		log_audio_route("with the device up");
	}
}

/*
 * Where the audio device has to come from, in the log: the audio server's plug-in directory of the
 * system image (as old as the image -- the prebuilt ones name the bundle RedwoodAudio.driver, which
 * this tree no longer ships), our own copy of it, and the kernel's device. A link whose target is
 * missing is exactly TEST7's fault, so it is said in one line.
 */
static void
hal_report(void)
{
	static const char *dirs[] = { "/System/Library/Audio/Plug-Ins/HAL", "/redwood/hal" };
	struct stat st;

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		DIR *dir = opendir(dirs[i]);
		struct dirent *entry;
		unsigned count = 0;

		if (dir == NULL) {
			stage_say("HAL %s: %s", dirs[i], strerror(errno));
			continue;
		}
		while ((entry = readdir(dir)) != NULL && count < 16) {
			char path[PATH_MAX], target[PATH_MAX], resolved[PATH_MAX];
			ssize_t length;

			if (entry->d_name[0] == '.') {
				continue;
			}
			snprintf(path, sizeof(path), "%s/%s", dirs[i], entry->d_name);
			length = readlink(path, target, sizeof(target) - 1);
			if (length >= 0) {
				target[length] = 0;
				if (target[0] == '/') {
					snprintf(resolved, sizeof(resolved), "%s", target);
				} else {
					snprintf(resolved, sizeof(resolved), "%s/%s", dirs[i], target);
				}
				stage_say("HAL %s: %s -> %s: %s", dirs[i], entry->d_name, target,
				    access(resolved, R_OK) == 0 ? "present" : "MISSING (no such file)");
			} else {
				stage_say("HAL %s: %s", dirs[i], entry->d_name);
			}
			count++;
		}
		closedir(dir);
		if (count == 0) {
			stage_say("HAL %s: EMPTY: the audio server has nothing to load", dirs[i]);
		}
	}
	stage_say("the kernel's /dev/hakios-audio: %s",
	    stat("/dev/hakios-audio", &st) == 0 ? "present" : strerror(errno));
}

static unsigned long
songs_count(unsigned long *without_filters)
{
	MPMediaQuery *query = [MPMediaQuery songsQuery];
	unsigned long count = [[query items] count];

	if (without_filters != NULL) {
		MPMediaQuery *wide = [MPMediaQuery songsQuery];

		if ([wide respondsToSelector:@selector(setIgnoreSystemFilterPredicates:)]) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(wide, @selector(setIgnoreSystemFilterPredicates:), YES);
		}
		if ([wide respondsToSelector:@selector(setIgnoreRestrictionsPredicates:)]) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(wide, @selector(setIgnoreRestrictionsPredicates:), YES);
		}
		*without_filters = [[wide items] count];
	}
	return count;
}

static MPMediaItem *
find_song(NSString *track, NSString **how)
{
	NSArray *items = [[MPMediaQuery songsQuery] items];

	*how = @"none";
	for (MPMediaItem *item in items) {
		NSString *asset = [[item assetURL] lastPathComponent];

		if (asset != nil && [asset isEqualToString:track]) {
			*how = @"assetURL";
			return item;
		}
	}
	/* a fresh library of this port holds exactly the track the stage imported; the row's assetURL
	 * may be empty even when the file plays (the row repairs are what music_import does), and then
	 * this is still the item MPMusicPlayerController would be asked to play */
	if ([items count] == 1) {
		*how = @"the only song in the library";
		return items[0];
	}
	return nil;
}

static const char *
utf8(id object)
{
	id description = object != nil ? [object description] : nil;
	const char *text = description != nil ? [description UTF8String] : NULL;

	return text != NULL ? text : "(none)";
}

static void
log_error(const char *what, NSError *error)
{
	if (error == nil) {
		stage_say("%s: no NSError", what);
		return;
	}
	stage_say("%s: %s %ld: %s", what, [[error domain] UTF8String], (long)[error code],
	    [[error localizedDescription] UTF8String]);
}

static int
play_with_avplayer(NSURL *url, double seconds)
{
	AVPlayer *player;
	AVPlayerItem *item = nil;
	char text[512];
	double started, last_position = 0;
	long last_status = -1;
	int reached = 0, said_error = 0;

	probe_asset(url);
	player = [AVPlayer playerWithURL:url];
	if (player == nil) {
		stage_say("AVPlayer: playerWithURL gave nil");
		return 0;
	}
	item = [player currentItem];
	stage_say("AVPlayer: playing %s", [[url absoluteString] UTF8String]);
	if (item != nil) {
		stage_say("AVPlayer: item status %s (%ld), duration %.3f s, error %s",
		    av_status_name((long)[item status]), (long)[item status],
		    CMTimeGetSeconds([item duration]), error_text([item error], text, sizeof(text)));
	}
	[player play];
	started = now_seconds();
	while (now_seconds() - started < seconds) {
		double position;

		pump_main(0.1);
		position = CMTimeGetSeconds([player currentTime]);
		if (item == nil) {
			continue;
		}
		if ((long)[item status] != last_status) {
			last_status = (long)[item status];
			g_av_status = last_status;
			stage_say("AVPlayer: item status is now %s (%ld) at t=%.2f s",
			    av_status_name(last_status), last_status, position);
		}
		if ([item error] != nil) {
			snprintf(g_av_error, sizeof(g_av_error), "%s",
			    error_text([item error], text, sizeof(text)));
			if (!said_error) {
				said_error = 1;
				stage_say("AVPlayer: the item's error: %s", g_av_error);
				log_error_log(item);
			}
		}
		if (position - last_position >= 1.0 || now_seconds() - started < 0.25) {
			stage_say("AVPlayer: t=%.2f s, status=%s (%ld), timeControlStatus=%ld, rate=%.1f, "
			    "waiting=%s, ranges=%lu, likely=%d", position,
			    av_status_name((long)[item status]), (long)[item status],
			    (long)[player timeControlStatus], (double)[player rate],
			    [player reasonForWaitingToPlay] != nil ?
			    [[player reasonForWaitingToPlay] UTF8String] : "(nothing)",
			    (unsigned long)[[item loadedTimeRanges] count],
			    (int)[item isPlaybackLikelyToKeepUp]);
			last_position = position;
		}
		if ([player timeControlStatus] == AVPlayerTimeControlStatusPlaying && position > 0.25) {
			reached = 1;
			stage_say("AVPlayer: playing at t=%.2f s after %.1f s", position,
			    now_seconds() - started);
			break;
		}
		if ((long)[item status] == AVPlayerItemStatusFailed) {
			stage_say("AVPlayer: the item failed at t=%.2f s", position);
			break;
		}
	}
	if (!reached) {
		stage_say("AVPlayer: it did NOT reach the playing state (t=%.2f s, status=%s (%ld), "
		    "timeControlStatus=%ld, rate=%.1f)", CMTimeGetSeconds([player currentTime]),
		    item != nil ? av_status_name((long)[item status]) : "no item",
		    item != nil ? (long)[item status] : -1, (long)[player timeControlStatus],
		    (double)[player rate]);
		if (item != nil) {
			g_av_status = (long)[item status];
			if ([item error] != nil && !said_error) {
				snprintf(g_av_error, sizeof(g_av_error), "%s",
				    error_text([item error], text, sizeof(text)));
				stage_say("AVPlayer: the item's error: %s", g_av_error);
			}
			log_error_log(item);
		}
	}
	[player pause];
	return reached;
}

static int
play_with_music_player(MPMediaItem *item, double seconds)
{
	Class controller_class = NSClassFromString(@"MPMusicPlayerController");
	id player;
	MPMediaItemCollection *collection;
	int reached = 0;

	if (controller_class == Nil) {
		stage_say("MPMusicPlayerController: not in this system");
		return 0;
	}
	player = ((id (*)(id, SEL))objc_msgSend)(controller_class, @selector(applicationMusicPlayer));
	if (player == nil) {
		stage_say("MPMusicPlayerController: applicationMusicPlayer gave nil");
		return 0;
	}
	collection = [MPMediaItemCollection collectionWithItems:@[ item ]];
	@try {
		((void (*)(id, SEL, id))objc_msgSend)(player, @selector(setQueueWithItemCollection:), collection);
		((void (*)(id, SEL))objc_msgSend)(player, @selector(prepareToPlay));
		((void (*)(id, SEL))objc_msgSend)(player, @selector(play));
	} @catch (NSException *exception) {
		stage_say("MPMusicPlayerController: %s: %s", [[exception name] UTF8String],
		    [[exception reason] UTF8String]);
		return 0;
	}
	for (double elapsed = 0; elapsed < seconds; elapsed += 0.1) {
		long state = (long)((NSInteger (*)(id, SEL))objc_msgSend)(player, @selector(playbackState));
		double position = ((double (*)(id, SEL))objc_msgSend)(player, @selector(currentPlaybackTime));

		pump_main(0.1);
		if (elapsed < 0.2 || ((int)(elapsed * 10)) % 10 == 0) {
			stage_say("MPMusicPlayerController: state=%ld t=%.2f s", state, position);
		}
		if (state == MPMusicPlaybackStatePlaying && position > 0.25) {
			reached = 1;
			stage_say("MPMusicPlayerController: playing at t=%.2f s after %.1f s", position, elapsed);
			break;
		}
	}
	if (!reached) {
		stage_say("MPMusicPlayerController: it did NOT reach the playing state");
	}
	@try {
		((void (*)(id, SEL))objc_msgSend)(player, @selector(stop));
	} @catch (NSException *exception) {
		(void)exception;
	}
	return reached;
}

static int
stage_music(double at, const char *source, const char *track_name)
{
	char marker[PATH_MAX];
	NSString *track = [NSString stringWithUTF8String:track_name];
	NSString *how = @"none";
	MPMediaItem *item = nil;
	unsigned long songs = 0, wide = 0;
	int import_ok = 0, alive, played = 0, exit_code;
	int explicit = -1;   /* R2: -1 = the item was not found, 0/1 = MPMediaItemPropertyIsExplicit */
	const char *played_with = "none";

	g_stage = "music";
	if (!claim_stage()) {
		return 0;
	}
	g_av_status = -1;
	g_av_error[0] = 0;
	g_av_outputs = -1;
	wait_until(at);
	/* it may well not be running yet: launchd starts it when the importer asks. This first look is
	 * only for the log; the one that counts is after the import. */
	alive = wait_for_process("medialibraryd", 5);
	stage_say("medialibraryd before the import: %s", alive ? "alive" : "not running (the import starts it)");

	snprintf(marker, sizeof(marker), "/var/mobile/Media/iTunes_Control/.hakios-imported-%s", track_name);
	if (access(source, R_OK) != 0) {
		stage_say("the source track %s is not there: %s", source, strerror(errno));
	} else {
		static char output[524288];
		char *argv[] = { (char *)"/redwood/bin/music_import", (char *)"--no-late-checks",
		    (char *)"--wait", (char *)"0", (char *)source, (char *)track_name, NULL };

		memset(output, 0, sizeof(output));
		exit_code = run_capture(argv, output, sizeof(output), 240);
		stage_say("music_import %s: exit %d", source, exit_code);
		detail_text(output);
		import_ok = exit_code == 0 && strstr(output, "music_import: done") != NULL;
		if (!import_ok) {
			stage_say("music_import did not say \"done\" (its lines above are the reason)");
		}
	}
	if (!import_ok && access(marker, F_OK) == 0) {
		import_ok = 1;
		stage_say("the marker %s says the track was imported by an earlier boot", marker);
	}
	alive = wait_for_process("medialibraryd", 15);
	stage_say("medialibraryd after the import: %s", alive ? "alive" : "NOT FOUND for 15 s");

	/* MediaPlayer's client is the user, not the daemon that imported the track: music_import
	 * becomes mobile before it asks, and a root process asking was what the first diagnostics
	 * had to explain away. The music stage has nothing left to do as root. */
	if (become_mobile() != 0) {
		stage_say("becoming mobile (501:501) failed: %s", strerror(errno));
		stage_result(0, "cannot become mobile: %s", strerror(errno));
		return 0;
	}
	stage_say("now mobile (501:501), as Music and MediaPlayer expect");
	stage_say("MPMediaLibrary authorization: %ld", (long)[MPMediaLibrary authorizationStatus]);
	songs = songs_count(&wide);
	/* medialibraryd may still be opening its database when the importer returns; a fresh library
	 * has one row, and a few seconds of retries turn a race into evidence either way */
	for (int attempt = 0; import_ok && songs == 0 && attempt < 10; attempt++) {
		stage_say("MPMediaQuery songsQuery: 0 items (attempt %d/10), waiting a second", attempt + 1);
		sleep_seconds(1.0);
		songs = songs_count(&wide);
	}
	stage_say("MPMediaQuery songsQuery: %lu item(s), %lu with the system and restriction filters off",
	    songs, wide);
	item = find_song(track, &how);
	if (item != nil) {
		stage_say("the track in the library: title \"%s\", artist \"%s\", %.1f s, assetURL %s (found by %s)",
		    utf8([item title]), utf8([item artist]), (double)[item playbackDuration],
		    utf8([item assetURL]), [how UTF8String]);
		/* R2/TEST5Q: ML3's isExplicit is (item_extra.content_rating == 1); Music's
		 * "Content restriction" screen follows this flag and the device's explicit-content
		 * setting, so the next log has to say which of the two the item carries */
		explicit = [[item valueForProperty:MPMediaItemPropertyIsExplicit] boolValue] ? 1 : 0;
		stage_say("the track's explicit flag: %d (ML3 item_extra.content_rating == 1), media type %lu", explicit,
		    (unsigned long)[[item valueForProperty:MPMediaItemPropertyMediaType] integerValue]);
	} else {
		stage_say("the track is NOT in the songs query (no assetURL for %s)", track_name);
	}

	{
		NSError *audio_error = nil;
		BOOL category = [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback
		    error:&audio_error];
		BOOL active = [[AVAudioSession sharedInstance] setActive:YES error:&audio_error];

		stage_say("the audio session for playback: category %s, active %s", category ? "playback" : "NOT SET",
		    active ? "yes" : "NO");
		if (!category || !active) {
			log_error("the playback audio session", audio_error);
		}
	}
	/* where the output device has to come from, and whether it is there: TEST7's player had no route
	 * at all, and nothing below could have played (the HAL plug-in was not loaded; see the report) */
	hal_report();
	wait_for_route(5.0);
	if (item != nil && [item assetURL] != nil) {
		played = play_with_avplayer([item assetURL], 12.0);
		played_with = "AVPlayer";
		if (!played) {
			log_audio_route("after the ipod-library attempt failed");
			played = play_with_music_player(item, 12.0);
			played_with = played ? "MPMusicPlayerController" : "neither";
		}
	} else if (item != nil) {
		stage_say("the library item has no assetURL; asking MPMusicPlayerController for it");
		played = play_with_music_player(item, 12.0);
		played_with = played ? "MPMusicPlayerController" : "neither";
	}
	if (!played) {
		NSString *fallback = [NSString stringWithFormat:@"/var/mobile/Media/iTunes_Control/Music/F00/%s",
		    track_name];

		if (access([fallback UTF8String], R_OK) == 0) {
			stage_say("the file is there although the library query did not show it: %s", [fallback UTF8String]);
			played = play_with_avplayer([NSURL fileURLWithPath:fallback], 12.0);
			played_with = played ? "AVPlayer(file)" : "none";
		}
	}
	if (!played) {
		played_with = "none";
	}
	stage_result(alive && import_ok && songs > 0 && played,
	    "medialibraryd=%d imported=%s songs=%lu play=%s timeControlStatus=%s item=%s route=%ld explicit=%s",
	    alive ? 1 : 0, import_ok ? "yes" : "no", songs, played_with,
	    played ? "playing" : "not-playing",
	    g_av_status < 0 ? "none" : av_status_name(g_av_status), g_av_outputs,
	    explicit < 0 ? "unknown" : explicit ? "yes" : "no");
	return alive && import_ok && songs > 0 && played;
}

/* ---- microphone ----------------------------------------------------------------------------- */

static int
stage_mic(double at, double seconds)
{
	AVAudioSession *session;
	AVAudioRecorder *recorder;
	NSDictionary *settings;
	NSString *path;
	NSError *error = nil;
	struct st_wave wave;
	char line[1024];
	BOOL ok;
	double started;
	int frames_wanted, pass;
	struct stat status;

	g_stage = "mic";
	if (!claim_stage()) {
		return 0;
	}
	wait_until(at);
	session = [AVAudioSession sharedInstance];
	stage_say("the session: category play-and-record, %s", session != nil ? "sharedInstance" : "NO SESSION");
	if (session == nil) {
		stage_result(0, "no AVAudioSession");
		return 0;
	}
	ok = [session setCategory:AVAudioSessionCategoryPlayAndRecord error:&error];
	stage_say("setCategory: %s%s", ok ? "set" : "NOT SET", ok ? "" : " --");
	if (!ok) {
		log_error("setCategory", error);
	}
	ok = [session setActive:YES error:&error];
	stage_say("setActive: %s", ok ? "active" : "NOT ACTIVE");
	if (!ok) {
		log_error("setActive", error);
	}
	stage_say("input available: %s, record permission: %ld", [session isInputAvailable] ? "yes" : "NO",
	    (long)[session recordPermission]);
	if ([session recordPermission] == AVAudioSessionRecordPermissionUndetermined) {
		dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
		__block BOOL granted = NO;

		[session requestRecordPermission:^(BOOL allowed) {
			granted = allowed;
			dispatch_semaphore_signal(semaphore);
		}];
		(void)dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
		stage_say("the permission was asked for: %s", granted ? "granted" : "DENIED");
	}
	if ([session recordPermission] == AVAudioSessionRecordPermissionDenied) {
		stage_result(0, "the record permission is denied (kTCCServiceMicrophone)");
		return 0;
	}
	path = [NSString stringWithFormat:@"/tmp/selftest-mic-%llu.wav", g_run_id];
	settings = @{
		AVFormatIDKey: @(kAudioFormatLinearPCM),
		AVSampleRateKey: @48000,
		AVNumberOfChannelsKey: @1,
		AVLinearPCMBitDepthKey: @16,
		AVLinearPCMIsFloatKey: @NO,
		AVLinearPCMIsBigEndianKey: @NO,
	};
	recorder = [[AVAudioRecorder alloc] initWithURL:[NSURL fileURLWithPath:path] settings:settings error:&error];
	if (recorder == nil) {
		log_error("AVAudioRecorder init", error);
		stage_result(0, "AVAudioRecorder could not be made");
		return 0;
	}
	[recorder setMeteringEnabled:YES];
	if (![recorder prepareToRecord]) {
		stage_say("prepareToRecord: NO");
		stage_result(0, "prepareToRecord failed");
		return 0;
	}
	stage_say("recording %.1f s to %s", seconds, [path UTF8String]);
	started = now_seconds();
	if (![recorder recordForDuration:seconds]) {
		stage_say("recordForDuration: NO");
		stage_result(0, "recordForDuration failed");
		return 0;
	}
	while ([recorder isRecording] && now_seconds() - started < seconds + 6.0) {
		usleep(100000);
	}
	[recorder stop];
	usleep(300000);   /* AVAudioRecorder closes the file in its own time */
	stage_say("the recorder stopped after %.2f s, isRecording=%s", now_seconds() - started,
	    [recorder isRecording] ? "yes" : "no");
	memset(&status, 0, sizeof(status));
	if (stat([path fileSystemRepresentation], &status) == 0) {
		stage_say("the file: %s, %lld bytes", [path UTF8String], (long long)status.st_size);
	} else {
		stage_say("the file: %s: %s", [path UTF8String], strerror(errno));
	}
	memset(&wave, 0, sizeof(wave));
	st_wave_analyze([path fileSystemRepresentation], &wave);
	if (!wave.ok) {
		stage_say("the capture cannot be read: %s", wave.why);
		stage_result(0, "no usable samples: %s", wave.why);
		return 0;
	}
	st_wave_describe(&wave, line, sizeof(line));
	stage_say("the capture: %u Hz, %u channel(s), %u bits, %.2f s; %s", wave.rate, wave.channels,
	    wave.bits, (double)wave.frames / (double)(wave.rate > 0 ? wave.rate : 1), line);
	frames_wanted = (int)((double)wave.rate * seconds * 0.5);
	pass = wave.frames >= (unsigned long long)(frames_wanted > 0 ? frames_wanted : 1) &&
	    wave.peak > 0 && !wave.all_equal;
	if (wave.peak == 0) {
		stage_say("every sample is 0: the path gave silence");
	} else if (wave.all_equal) {
		stage_say("every sample is the same value (%d): a stuck converter, not a room", wave.min);
	}
	stage_result(pass, "peak=%d rms=%.1f dBFS=%.1f samples=%llu distinct=%llu all_equal=%s "
	    "zero_crossings=%llu", wave.peak, wave.rms,
	    wave.peak > 0 ? 20.0 * log10((double)wave.peak / 32768.0) : -999.0, wave.samples,
	    wave.distinct, wave.all_equal ? "YES" : "no", wave.crossings);
	return pass;
}

/* ---- camera --------------------------------------------------------------------------------- */

static void
print_file_tail(const char *path, int lines)
{
	FILE *file = fopen(path, "r");
	char line[1024];

	if (file == NULL) {
		stage_say("the camera application's log %s: %s", path, strerror(errno));
		return;
	}
	/* a small ring of the last LINES lines, so a long log still shows its end */
	{
		char **ring = calloc((size_t)lines, sizeof(*ring));
		int count = 0, next = 0;

		if (ring == NULL) {
			fclose(file);
			return;
		}
		while (fgets(line, sizeof(line), file) != NULL) {
			size_t length = strcspn(line, "\r\n");

			free(ring[next]);
			ring[next] = strndup(line, length);
			next = (next + 1) % lines;
			if (count < lines) {
				count++;
			}
		}
		fclose(file);
		stage_say("the camera application's log, last %d line(s):", count);
		for (int i = 0; i < count; i++) {
			int index = (next - count + i + lines * 2) % lines;
			char body[1200];

			if (ring[index] != NULL) {
				snprintf(body, sizeof(body), "%s: | (app) %s", g_stage, ring[index]);
				say_line(body);
			}
		}
		for (int i = 0; i < lines; i++) {
			free(ring[i]);
		}
		free(ring);
	}
}

static int
stage_camera(double at, int app_timeout)
{
	char result_path[PATH_MAX], url[256];
	static char result[65536];
	char still[PATH_MAX] = "", video[PATH_MAX] = "", atoms[256] = "", kind[32] = "", why[256] = "";
	char video_why[256] = "";
	struct stat still_status, video_status;
	double deadline, started, last_note = 0;
	int exit_code, have_result = 0, retried = 0, has_moov = 0;
	long long video_size = 0;
	int still_ok = 0, video_ok = 0;

	g_stage = "camera";
	if (!claim_stage()) {
		return 0;
	}
	wait_until(at);
	ensure_dirs(SELFTEST_DIR);
	(void)chown(SELFTEST_DIR, MOBILE_ID, MOBILE_ID);
	snprintf(result_path, sizeof(result_path), SELFTEST_DIR "/camera-%llu.result", g_run_id);
	snprintf(url, sizeof(url), "hakiostest://camera?run=%llu", g_run_id);
	result[0] = '\0';

	stage_say("the foreground application is the one that can be given frames (userland/src/camera_avtest.m: "
	    "\"a program like this one is not given any\"); asking SpringBoard to open %s", url);
	exit_code = run_capture((char *const[]){ (char *)"/redwood/bin/battery_test", (char *)"open", url, NULL },
	    result, sizeof(result), 30);
	stage_say("battery_test open %s: exit %d", url, exit_code);
	detail_text(result);
	if (exit_code != 0) {
		stage_say("SpringBoard was not asked (battery_test is missing or refused); the application may "
		    "still have been opened by other means");
	}

	started = now_seconds();
	deadline = started + (double)app_timeout;
	while (now_seconds() < deadline) {
		if (read_file(result_path, result, sizeof(result)) > 0 && strstr(result, "result=") != NULL) {
			have_result = 1;
			break;
		}
		if (!retried && now_seconds() > started + (double)app_timeout / 2.0) {
			char again[4096];

			retried = 1;
			memset(again, 0, sizeof(again));
			stage_say("no result after %.0f s: asking SpringBoard to open %s once more", now_seconds() - started, url);
			(void)run_capture((char *const[]){ (char *)"/redwood/bin/battery_test", (char *)"open", url, NULL },
			    again, sizeof(again), 30);
			detail_text(again);
		}
		if (now_seconds() - last_note > 10.0) {
			stage_say("waiting for %s (%.0f s left)", result_path, deadline - now_seconds());
			last_note = now_seconds();
		}
		sleep_seconds(0.5);
	}
	if (!have_result) {
		static char lscheck[16384];

		stage_say("no result was left in %s within %d s", result_path, app_timeout);
		print_file_tail(CAMERA_APP_LOG, 30);
		/* the other half of the question: is the bundle known to LaunchServices at all? */
		if (access("/redwood/bin/ls_rebuild", X_OK) == 0) {
			memset(lscheck, 0, sizeof(lscheck));
			(void)run_capture((char *const[]){ (char *)"/redwood/bin/ls_rebuild", (char *)"--check", NULL },
			    lscheck, sizeof(lscheck), 60);
			stage_say("/redwood/bin/ls_rebuild --check:");
			detail_text(lscheck);
		}
		stage_result(0, "the application left no result (see its log and the LaunchServices check above)");
		return 0;
	}
	stage_say("the application's result, %s:", result_path);
	detail_text(result);
	(void)value_of(result, "still", still, sizeof(still));
	(void)value_of(result, "video", video, sizeof(video));
	(void)value_of(result, "video_atoms", atoms, sizeof(atoms));
	(void)value_of(result, "still_kind", kind, sizeof(kind));

	memset(&still_status, 0, sizeof(still_status));
	if (still[0] != '\0' && stat(still, &still_status) == 0 && still_status.st_size > 4096) {
		const char *detected = st_image_kind(still, why, sizeof(why));

		stage_say("the still: %s, %lld bytes, %s", still, (long long)still_status.st_size, detected);
		snprintf(kind, sizeof(kind), "%s", detected);
		still_ok = strcmp(detected, "jpeg") == 0 || strcmp(detected, "heic") == 0;
	} else {
		stage_say("the still: %s is missing or too small (%lld bytes)", still[0] ? still : "(no path)",
		    (long long)still_status.st_size);
	}
	memset(&video_status, 0, sizeof(video_status));
	if (video[0] != '\0' && stat(video, &video_status) == 0 && video_status.st_size > 4096) {
		if (st_atoms(video, atoms, sizeof(atoms), &has_moov, &video_size, video_why, sizeof(video_why)) == 0) {
			stage_say("the movie: %s, %lld bytes, atoms %s, moov %s", video,
			    (long long)video_status.st_size, atoms, has_moov ? "present" : "MISSING");
			/* moov is the recording's index and is written when the writer closes the file;
			 * mdat is the media itself. A QuickTime .mov need not begin with ftyp, so that one
			 * is logged but not required. */
			video_ok = has_moov && strstr(atoms, "mdat") != NULL;
		} else {
			stage_say("the movie: %s, %lld bytes, atoms cannot be read: %s", video,
			    (long long)video_status.st_size, video_why);
		}
	} else {
		stage_say("the movie: %s is missing or too small (%lld bytes)", video[0] ? video : "(no path)",
		    (long long)video_status.st_size);
	}
	if (!still_ok || !video_ok) {
		print_file_tail(CAMERA_APP_LOG, 30);
		if (access("/redwood/bin/ls_rebuild", X_OK) == 0) {
			static char lscheck[16384];

			memset(lscheck, 0, sizeof(lscheck));
			(void)run_capture((char *const[]){ (char *)"/redwood/bin/ls_rebuild", (char *)"--check", NULL },
			    lscheck, sizeof(lscheck), 60);
			stage_say("/redwood/bin/ls_rebuild --check:");
			detail_text(lscheck);
		}
	}
	stage_result(still_ok && video_ok,
	    "still=%s (%lld bytes, %s) video=%s (%lld bytes, %s, moov=%s)",
	    still[0] ? still : "(none)", (long long)still_status.st_size, kind[0] ? kind : "unknown",
	    video[0] ? video : "(none)", (long long)video_status.st_size, atoms[0] ? atoms : "(none)",
	    has_moov ? "present" : "MISSING");
	return still_ok && video_ok;
}

/* ---- summary and power-off ----------------------------------------------------------------- */

static const char *
result_of(const char *text, const char *stage, char *out, size_t size)
{
	const char *p = text;
	size_t length = strlen(stage);

	/* the last line of the stage wins: a rerun of one stage replaces nothing, it appends */
	out[0] = '\0';
	while (p != NULL && *p != '\0') {
		const char *end = strchr(p, '\n');

		if (strncmp(p, stage, length) == 0 && p[length] == '=') {
			const char *value = p + length + 1;
			size_t value_length = end != NULL ? (size_t)(end - value) : strlen(value);

			if (value_length >= size) {
				value_length = size - 1;
			}
			memcpy(out, value, value_length);
			out[value_length] = '\0';
			{
				char *space = strchr(out, ' ');

				if (space != NULL) {
					*space = '\0';
				}
			}
		}
		p = end != NULL ? end + 1 : NULL;
	}
	return out;
}

static int
stage_summary(void)
{
	static const char *stages[] = { "files", "music", "mic", "camera" };
	static char text[262144];
	char line[1024] = "";
	int passed = 0;

	g_stage = "summary";
	memset(text, 0, sizeof(text));
	if (read_file(g_results, text, sizeof(text)) < 0) {
		stage_say("the results file %s cannot be read: %s", g_results, strerror(errno));
	}
	for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
		char value[16];

		result_of(text, stages[i], value, sizeof(value));
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s=%s", i > 0 ? " " : "",
		    stages[i], value[0] != '\0' ? value : "NOT-RUN");
		if (strcmp(value, "PASS") == 0) {
			passed++;
		}
	}
	{
		char body[1300];

		snprintf(body, sizeof(body), "summary %s", line);
		say_line(body);
	}
	say("RESULT %d/%u stages passed%s", passed, (unsigned)(sizeof(stages) / sizeof(stages[0])),
	    passed == (int)(sizeof(stages) / sizeof(stages[0])) ? "" : " (the FAIL lines above say why)");
	return passed == (int)(sizeof(stages) / sizeof(stages[0]));
}

static int
stage_poweroff(void)
{
	int state = -1;
	size_t length = sizeof(state);

	g_stage = "poweroff";
	if (!poweroff_allowed()) {
		stage_say("SKIP: selftest_poweroff=1 is not among the boot-args; the phone stays on");
		return 2;
	}
	(void)sysctlbyname("kern.hakios.var_save", &state, &length, NULL, 0);
	stage_say("step 1/6: kern.hakios.var_save = %d (1 = a saver is registered, 2 = the kernel is asking, "
	    "3 = done; -1 with %s)", state, strerror(errno));
	stage_say("step 2/6: sync()");
	sync();
	stage_say("step 3/6: flushing the log and the results file");
	fflush(stdout);
	if (g_log != NULL) {
		fflush(g_log);
		(void)fsync(fileno(g_log));
	}
	stage_say("step 4/6: reboot(RB_HALT) -- the same syscall launchd makes at the end of a \"slide to "
	    "power off\" (TEST1's log: \"Will be calling reboot(2) with flags: 0x8\"); the kernel side is "
	    "reboot_kernel -> host_reboot(HOST_REBOOT_HALT) -> halt_all_cpus -> PSCI SYSTEM_OFF "
	    "(xnu/files/bsd/kern/kern_shutdown.c, xnu/files/osfmk/arm64/loader_return.c)");
	stage_say("step 5/6: var_store's save and the kernel's \"--- PSCI SYSTEM_OFF ---\" are the next "
	    "lines on this console");
	(void)fflush(stdout);
	(void)fflush(stderr);
	sync();
	reboot(RB_HALT);
	stage_say("step 6/6: FAIL: reboot(2) returned, errno %d (%s)", errno, strerror(errno));
	result_write("poweroff", 0, "reboot(2) returned");
	return 1;
}

/* ---- the driver ----------------------------------------------------------------------------- */

struct stage_plan {
	const char *	name;
	double		at;
	int		timeout;
	const char *	extra[8];
};

static int
wait_child(pid_t pid, int timeout_seconds)
{
	double deadline = now_seconds() + (double)timeout_seconds;
	int status = 0;

	for (;;) {
		pid_t waited = waitpid(pid, &status, WNOHANG);

		if (waited == pid) {
			break;
		}
		if (waited < 0 && errno != EINTR) {
			return -1;
		}
		if (now_seconds() >= deadline) {
			kill(pid, SIGKILL);
			(void)waitpid(pid, &status, 0);
			return -2;
		}
		usleep(50000);
	}
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}
	return -1;
}

static int
run_all(const char *source, const char *track, double mic_seconds, int app_timeout)
{
	char lock_path[PATH_MAX];
	char at_text[32], results_arg[PATH_MAX + 16], seconds_text[32], timeout_text[32];
	struct stage_plan plan[4];
	int lock_fd, failures = 0, ran = 0;

	snprintf(lock_path, sizeof(lock_path), SELFTEST_DIR "/run-%llu.lock", g_run_id);
	lock_fd = open(lock_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (lock_fd < 0) {
		if (errno == EEXIST) {
			stage_say("SKIP: another selftest run of this boot holds %s", lock_path);
			return 0;
		}
		stage_say("the lock %s cannot be made: %s (carrying on)", lock_path, strerror(errno));
	} else {
		dprintf(lock_fd, "pid %d\n", (int)getpid());
		close(lock_fd);
	}
	say("run started: pid %d, boot %llu, %.1f s after boot", (int)getpid(), g_run_id, uptime_now());
	say("the boot-args: \"%s\"", g_have_bootargs ? g_bootargs : "(not readable)");
	say("the results file: %s; the log: %s", g_results, g_logpath);
	snprintf(results_arg, sizeof(results_arg), "--results=%s", g_results);

	/* the timeouts are generous on purpose: the point of killing a stage is a hang, not a slow
	 * phone. files: three fileproviderctl calls (30 s each), fp_enable's wait, retries and work (200 s),
	 * the write (60 s), vol_ls (20 s), the provider's ls from both clients (30 s each) and fp_enable again,
	 * later (90 s); music: the import's own 120 s alarm and three minutes of library work; camera: the 90 s
	 * wait for the application plus its report. */
	plan[0] = (struct stage_plan){ "files", AT_FILES, 480, { at_text, results_arg, NULL } };
	plan[1] = (struct stage_plan){ "music", AT_MUSIC, 360, { at_text, results_arg, "--source", source,
	    "--track", track, NULL } };
	plan[2] = (struct stage_plan){ "mic", AT_MIC, 180, { at_text, results_arg, seconds_text, NULL } };
	plan[3] = (struct stage_plan){ "camera", AT_CAMERA, 300, { at_text, results_arg, timeout_text, NULL } };
	snprintf(seconds_text, sizeof(seconds_text), "--seconds=%.1f", mic_seconds);
	snprintf(timeout_text, sizeof(timeout_text), "--app-timeout=%d", app_timeout);

	for (unsigned i = 0; i < sizeof(plan) / sizeof(plan[0]); i++) {
		char status_text[64];
		char *argv[12];
		int count = 0, status;
		pid_t pid;
		posix_spawn_file_actions_t actions;

		snprintf(at_text, sizeof(at_text), "--at=%.0f", plan[i].at);
		while (uptime_now() < plan[i].at) {
			sleep_seconds(plan[i].at - uptime_now() > 0.5 ? 0.5 : 0.05);
		}
		argv[count++] = (char *)"/redwood/bin/selftest";
		argv[count++] = (char *)plan[i].name;
		for (unsigned j = 0; plan[i].extra[j] != NULL; j++) {
			argv[count++] = (char *)plan[i].extra[j];
		}
		argv[count] = NULL;
		posix_spawn_file_actions_init(&actions);
		if (posix_spawn(&pid, argv[0], &actions, NULL, argv, environ) != 0) {
			stage_say("the %s stage could not be started: %s", plan[i].name, strerror(errno));
			snprintf(status_text, sizeof(status_text), "the stage process did not start");
			result_write(plan[i].name, 0, status_text);
			failures++;
			continue;
		}
		posix_spawn_file_actions_destroy(&actions);
		ran++;
		status = wait_child(pid, plan[i].timeout);
		if (status == 0) {
			stage_say("the %s stage passed (exit 0)", plan[i].name);
		} else if (status == -2) {
			stage_say("the %s stage was killed after %d s", plan[i].name, plan[i].timeout);
			snprintf(status_text, sizeof(status_text), "the stage process was killed after %d s",
			    plan[i].timeout);
			result_write(plan[i].name, 0, status_text);
			failures++;
		} else {
			stage_say("the %s stage did not pass (exit %d)", plan[i].name, status);
			/* a crash leaves no result line: make one so the summary cannot hide it */
			{
				static char text[262144];
				char value[16];

				memset(text, 0, sizeof(text));
				(void)read_file(g_results, text, sizeof(text));
				(void)result_of(text, plan[i].name, value, sizeof(value));
				if (strcmp(value, "PASS") != 0 && strcmp(value, "FAIL") != 0) {
					snprintf(status_text, sizeof(status_text), "the stage process exited %d",
					    status);
					result_write(plan[i].name, 0, status_text);
				}
			}
			failures++;
		}
	}
	stage_summary();
	if (poweroff_allowed()) {
		g_stage = "poweroff";
		wait_until(AT_POWEROFF);
		stage_poweroff();
	} else {
		stage_say("SKIP: selftest_poweroff=1 is not among the boot-args; the phone stays on");
	}
	say("run finished: %d stage(s) ran, %d failure(s)", ran, failures);
	return 0;
}

/* ---- main ----------------------------------------------------------------------------------- */

static void
usage(void)
{
	printf("selftest: run|files|music|mic|camera|summary|poweroff [--at=S] [--results=FILE]\n"
	    "          [--source=FILE] [--track=NAME] [--seconds=N] [--app-timeout=N] [--again]\n"
	    "          selftest --write-file NAME (internal)\n"
	    "          selftest --exec-mobile PROGRAM [ARG...] (internal: drop to 501, exec)\n"
	    "Everything is skipped unless selftest=1 is among the kernel's boot-args; the power-off\n"
	    "also needs selftest_poweroff=1.\n");
}

int
main(int argc, char **argv)
{
	const char *command;
	const char *source = MUSIC_SOURCE;
	const char *track = MUSIC_TRACK;
	double at = -1, mic_seconds = 3.0;
	int app_timeout = 90;
	int result = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	read_boot();

	command = argc > 1 ? argv[1] : "run";
	if (strcmp(command, "--write-file") == 0) {
		@autoreleasepool {
			return argc > 2 ? write_test_file(argv[2]) : 2;
		}
	}
	/*
	 * The files stage's client-side checks (fileproviderctl ls) have to be what mobile sees, not what
	 * root sees: fileproviderd serves user/501 and fileproviderctl warns about root itself. This mode
	 * is the same drop write_test_file does, followed by exec, so the program keeps its own signature
	 * and entitlements and its output lands in the caller's pipe.
	 */
	if (strcmp(command, "--exec-mobile") == 0) {
		if (argc < 3) {
			fprintf(stderr, "selftest: --exec-mobile needs a program\n");
			return 2;
		}
		if (become_mobile() != 0) {
			fprintf(stderr, "selftest: becoming mobile (501:501) failed: %s\n", strerror(errno));
			return 1;
		}
		execv(argv[2], &argv[2]);
		fprintf(stderr, "selftest: exec %s: %s\n", argv[2], strerror(errno));
		return errno == ENOENT ? 127 : 126;
	}
	if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0 || strcmp(command, "help") == 0) {
		usage();
		return 0;
	}
	for (int i = 2; i < argc; i++) {
		if (strncmp(argv[i], "--at=", 5) == 0) {
			at = atof(argv[i] + 5);
		} else if (strncmp(argv[i], "--results=", 10) == 0) {
			snprintf(g_results, sizeof(g_results), "%s", argv[i] + 10);
		} else if (strncmp(argv[i], "--source=", 9) == 0) {
			source = argv[i] + 9;
		} else if (strncmp(argv[i], "--track=", 8) == 0) {
			track = argv[i] + 8;
		} else if (strncmp(argv[i], "--seconds=", 10) == 0) {
			mic_seconds = atof(argv[i] + 10);
		} else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
			source = argv[++i];
		} else if (strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
			track = argv[++i];
		} else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			mic_seconds = atof(argv[++i]);
		} else if (strcmp(argv[i], "--app-timeout") == 0 && i + 1 < argc) {
			app_timeout = atoi(argv[++i]);
		} else if (strncmp(argv[i], "--app-timeout=", 14) == 0) {
			app_timeout = atoi(argv[i] + 14);
		} else if (strcmp(argv[i], "--again") == 0) {
			g_allow_duplicate = 1;
		} else if (strncmp(argv[i], "--at", 4) == 0 && i + 1 < argc) {
			at = atof(argv[++i]);
		} else if (strncmp(argv[i], "--results", 9) == 0 && i + 1 < argc) {
			snprintf(g_results, sizeof(g_results), "%s", argv[++i]);
		} else {
			fprintf(stderr, "selftest: unknown argument \"%s\"\n", argv[i]);
			usage();
			return 2;
		}
	}
	if (at < 0) {
		at = strcmp(command, "files") == 0 ? AT_FILES :
		    strcmp(command, "music") == 0 ? AT_MUSIC :
		    strcmp(command, "mic") == 0 ? AT_MIC :
		    strcmp(command, "camera") == 0 ? AT_CAMERA : 0;
	}
	g_stage = command;
	/* every mode is behind the boot-arg; the power-off's second gate is inside stage_poweroff() */
	if (!g_have_bootargs || !st_boot_arg_is(g_bootargs, "selftest", "1")) {
		stage_say("SKIP: selftest=1 is not among the boot-args (kern.bootargs: \"%s\")",
		    g_have_bootargs ? g_bootargs : "(sysctl not readable)");
		return 0;
	}
	log_open();

	@autoreleasepool {
		if (strcmp(command, "run") == 0) {
			g_stage = "run";
			result = run_all(source, track, mic_seconds, app_timeout);
		} else if (strcmp(command, "files") == 0) {
			result = stage_files(at) ? 0 : 1;
		} else if (strcmp(command, "music") == 0) {
			result = stage_music(at, source, track) ? 0 : 1;
		} else if (strcmp(command, "mic") == 0) {
			result = stage_mic(at, mic_seconds) ? 0 : 1;
		} else if (strcmp(command, "camera") == 0) {
			result = stage_camera(at, app_timeout) ? 0 : 1;
		} else if (strcmp(command, "summary") == 0) {
			result = stage_summary() ? 0 : 1;
		} else if (strcmp(command, "poweroff") == 0) {
			int power = stage_poweroff();

			result = power == 1 ? 1 : 0;   /* 2 is the SKIP that selftest_poweroff=0 asks for */
		} else {
			fprintf(stderr, "selftest: unknown command \"%s\"\n", command);
			usage();
			result = 2;
		}
	}
	return result;
}
