/*
 * oslogfwd -- carry the os_log (unified logging) messages of selected subsystems
 *             and processes into the run log, i.e. onto /dev/console.
 *
 * Why this exists (task X1, vayu iOS 16.5 bring-up):
 *
 *   The run log has launchd's lines, the daemons' stderr lines (mkjobs.py sends them to
 *   /dev/console) and the kernel's own printf, but nothing that an application wrote with
 *   os_log: mkjobs.py can only give OS_ACTIVITY_DT_MODE to the jobs whose labels are named
 *   in OSLOG= at build time, and an application started by SpringBoard/runningboardd is not
 *   one of them.  The unified log itself is alive in this port -- /dev/oslog and
 *   /dev/oslog_stream exist (captures/vayu-20261025-sens2/195412-run.log:1489-1490), logd
 *   runs (pid 18), diagnosticd offers com.apple.os_trace_relay -- only nobody reads it.
 *
 *   This program is that reader.  It asks LoggingSupport.framework (the private framework
 *   behind `log stream`, verified present in the iOS 16.5 dyld cache at
 *   0x19ecd8000: OSLogEventLiveStore / OSLogEventLiveStream / OSLogEventStreamBase /
 *   OSLogEventProxy, ivar OSLogEventLiveStream._diagdconn, strings
 *   com.apple.diagnosticd / com.apple.os_trace_relay) for a live stream, keeps the entries
 *   whose subsystem (or subsystem:category) is named in the boot-arg
 *
 *       oslog=com.apple.amp.mediaplaybackcore,com.apple.UIKit:Scrolling,...
 *
 *   and writes them to the console, rate limited, one line each -- a console write here is
 *   a character at a time through the loader, so a busy subsystem must not be allowed to
 *   push everything else out of the log.
 *
 *   The private API's exact creation selector is not visible in the cache (the cached
 *   class_ro_t method lists do not decode with the usual relative-method-list rules), so
 *   the program is written to try the documented shapes in order and to say which one
 *   worked: "backend=" in the first line of a session, and --selftest prints what the
 *   framework on this system actually answers to.
 *
 * Options (a --subsystem on the command line replaces the boot-arg list):
 *
 *   oslogfwd [--selftest] [--all] [--subsystem SPEC]... [--process NAME]...
 *            [--rate N] [--burst N] [--console PATH]
 *
 * Boot-args:
 *
 *   oslog=a,b:c,d          subsystems (b:c = subsystem b, category c); "all" for every one
 *   oslog_proc=Music,Files process names (default: all)
 *   oslog_rate=N           messages per second per subsystem (default 30, 0 = unlimited)
 *   oslog_burst=N          ... and the burst above that rate (default 60)
 *
 * Everything under /redwood is ours, so this file is ours too; the framework it talks to
 * is Apple's and is used, not copied.
 */

#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

/* libproc, for the name/path of a pid when the event's catalog entry is missing (X3:
 * -process was one of the fields the decoder could not fill when it had no catalog). */
extern int proc_name(int pid, void *buffer, uint32_t size);
extern int proc_pidpath(int pid, void *buffer, uint32_t size);

#define MAX_RULES       32
#define MAX_PROCS       8
#define MAX_APPS        8
#define SUBSYS_LEN      160
#define CATEGORY_LEN    96
#define LINE_LEN        768
#define DUMP_LEN        1400
#define MSG_LEN         420
#define OSLFWD_VERSION  "x3"

/* The subsystems a Music/Files/scroll investigation needs, unless the boot-arg says
 * otherwise.  com.apple.UIKit:Scrolling is a subsystem and a category, the rest are
 * whole subsystems. */
static const char *default_spec =
	"com.apple.amp.mediaplaybackcore,com.apple.amp.MediaPlayer,"
	"com.apple.FileProvider,com.apple.DocumentManager,com.apple.coremedia,"
	"com.apple.UIKit:Scrolling,com.apple.coreaudio,com.apple.avfaudio";

typedef struct {
	char subsystem[SUBSYS_LEN];
	char category[CATEGORY_LEN];
	int  has_category;
	int  prefix;                 /* the last character of the subsystem was '*' */
	double tokens;
	double last;
	unsigned long long seen;
	unsigned long long printed;
} rule_t;

static rule_t rules[MAX_RULES];
static int nrules;
static char procs[MAX_PROCS][64];
static int nprocs;

static double rate = 10.0;       /* per subsystem, messages a second */
static int    burst = 20;
static double global_rate;       /* derived from rate, see load_config() */
static int    global_burst;
static double global_tokens;
static double global_last;
/* n_events counts what the stream handler was called with, before anything is looked at;
 * n_seen is the part of it that had a subsystem, i.e. was an os_log entry and not an
 * activity or a signpost.  The two together are what tells a silent diagnosticd (both 0)
 * from a channel that works (n_events grows) and a filter that matches nothing. */
static unsigned long long n_printed, n_dropped, n_seen, n_events;
static unsigned long long live_events;   /* the live handler only, so that the archive
                                          * reads below cannot look like a live stream */
static int probe_left = -1;   /* --probe N: stop after N printed lines */

/* X3: the raw dump.  The X2 reader required a non-empty -subsystem before it looked at
 * anything else; TEST14 showed 1136 handler calls and not one event with a subsystem, so
 * every application line was thrown away before -composedMessage was ever asked for.
 * The dump path prints the fields that exist (and says which are absent) without a
 * process/category/subsystem filter, and stops on a byte/line budget instead of on a
 * filter that cannot match.  The budgets are split: app/error lines may spend the last
 * reserve, everything else stops before it, so a chatty daemon cannot use up the space
 * an application error would have needed. */
static int dump_mode;                 /* 0 off, 1 every log event, 2 error/fault only */
static int dump_activity;             /* oslog_dump_activity=1: activities/signposts too */
static int classes_mode;              /* oslog_classes=1: list the private classes' methods */
static unsigned long long dump_bytes_limit = 64 * 1024;
static unsigned long long dump_lines_limit = 400;
static unsigned long long dump_bytes_used, dump_lines_used;
static unsigned long long dump_events, dump_with_msg, dump_no_msg, dump_dropped_events;
static unsigned long long dump_bare_shown, dump_bare_limit = 40;
static int dump_exhausted_said;
static double dump_rate = 20.0;       /* dump lines a second, app/error exempt */
static int dump_burst = 40;
static double dump_tokens, dump_last;
static char app_names[MAX_APPS][64];
static int napps;
static const char *const default_app_names[] = {
	"Music", "Files", "selftest", "camera", "MediaPlayer", "fileprovider", NULL
};

/* X2: a stream that diagnosticd refuses is invalidated, and the old code had no handler
 * for that, no counter of the events it was handed, and did not say which uid it ran as
 * -- the "alive, 0 seen" heartbeat was therefore compatible with three different faults. */
static volatile sig_atomic_t stream_invalidated;
static volatile sig_atomic_t store_done;
static int send_predicate = 1;   /* oslog_pred=0: filter here only, not in diagnosticd */
static int store_fallback = 1;   /* oslog_store=0: never read the tracev3 archive */
static int store_secs = 120;     /* first archive read after N s, and every N s after */
static int store_wait = 20;      /* seconds to let one archive read run */

static int console_fd = -1;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t stop_now;

static void
out_line(const char *line)
{
	size_t len = strlen(line);
	size_t done = 0;
	int tries;

	pthread_mutex_lock(&out_lock);
	if (console_fd >= 0) {
		/* X2: the old write was a single non-blocking write and threw the rest of the
		 * line away on EAGAIN.  In TEST13B that is how "streaming, backend ..." was
		 * lost from run.log while the heartbeat four minutes later survived, which
		 * cost the reader the one line that named the backend.  Keep the bytes. */
		for (tries = 0; done < len && tries < 400; tries++) {
			ssize_t n = write(console_fd, line + done, len - done);
			if (n > 0) {
				done += (size_t)n;
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				usleep(500);
				continue;
			}
			if (n < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		/* a line that could not be finished is still terminated, so that it does not
		 * swallow the one after it */
		if (done > 0 && done < len) {
			(void)write(console_fd, "\n", 1);
		}
	} else {
		while (done < len) {
			ssize_t n = write(STDERR_FILENO, line + done, len - done);
			if (n <= 0) {
				break;
			}
			done += (size_t)n;
		}
	}
	pthread_mutex_unlock(&out_lock);
}

static void
say(const char *fmt, ...)
{
	char buf[LINE_LEN];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(buf) - 2) {
		n = (int)sizeof(buf) - 2;
	}
	buf[n++] = '\n';
	buf[n] = 0;
	out_line(buf);
}

static double
now_sec(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

/* wall clock, for a run log that has launchd's timestamps beside ours */
static void
stamp(char *out, size_t outlen)
{
	struct timeval tv;
	struct tm tm;

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	snprintf(out, outlen, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
	    (int)(tv.tv_usec / 1000));
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */

static void
add_rule(const char *spec, size_t len)
{
	rule_t *r;
	const char *colon;
	size_t sub_len;

	if (nrules == MAX_RULES || len == 0) {
		return;
	}
	r = &rules[nrules];
	memset(r, 0, sizeof(*r));

	colon = memchr(spec, ':', len);
	sub_len = colon ? (size_t)(colon - spec) : len;
	if (sub_len >= SUBSYS_LEN) {
		sub_len = SUBSYS_LEN - 1;
	}
	memcpy(r->subsystem, spec, sub_len);
	r->subsystem[sub_len] = 0;

	if (colon && len > sub_len + 1) {
		size_t cat_len = len - sub_len - 1;
		if (cat_len >= CATEGORY_LEN) {
			cat_len = CATEGORY_LEN - 1;
		}
		memcpy(r->category, colon + 1, cat_len);
		r->category[cat_len] = 0;
		r->has_category = 1;
	}
	sub_len = strlen(r->subsystem);
	if (sub_len && r->subsystem[sub_len - 1] == '*') {
		r->subsystem[sub_len - 1] = 0;
		r->prefix = 1;
	}
	r->last = now_sec();
	r->tokens = (double)burst;
	nrules++;
}

static void
parse_spec_list(const char *spec)
{
	const char *p = spec;

	while (*p) {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);

		if (len == 3 && strncmp(p, "all", 3) == 0) {
			nrules = 0;
			add_rule("*", 1);
			return;
		}
		if (len == 4 && strncmp(p, "none", 4) == 0) {
			nrules = 0;
			return;
		}
		add_rule(p, len);
		if (!comma) {
			break;
		}
		p = comma + 1;
	}
}

static void
add_proc(const char *name)
{
	if (nprocs < MAX_PROCS) {
		snprintf(procs[nprocs], sizeof(procs[0]), "%s", name);
		nprocs++;
	}
}

/* X3: the processes whose lines are kept in the last reserve of the dump budget.  A
 * substring, because the camera selftest can be com.hakios.selftest.camera, and the
 * decoder may hand us "Music" or the executable's basename depending on the catalog. */
static void
add_app(const char *name)
{
	int i;

	for (i = 0; i < napps; i++) {
		if (strcmp(app_names[i], name) == 0) {
			return;
		}
	}
	if (napps < MAX_APPS) {
		snprintf(app_names[napps], sizeof(app_names[0]), "%s", name);
		napps++;
	}
}

static int
contains_nocase(const char *haystack, const char *needle)
{
	size_t nlen = strlen(needle);
	const char *p;

	if (!haystack || !nlen) {
		return 0;
	}
	for (p = haystack; *p; p++) {
		size_t i;

		for (i = 0; i < nlen; i++) {
			unsigned char a = (unsigned char)p[i];
			unsigned char b = (unsigned char)needle[i];

			if (a >= 'A' && a <= 'Z') {
				a = (unsigned char)(a - 'A' + 'a');
			}
			if (b >= 'A' && b <= 'Z') {
				b = (unsigned char)(b - 'A' + 'a');
			}
			if (a != b) {
				break;
			}
		}
		if (i == nlen) {
			return 1;
		}
	}
	return 0;
}

static int
app_match(const char *name)
{
	int i;

	for (i = 0; i < napps; i++) {
		if (contains_nocase(name, app_names[i])) {
			return 1;
		}
	}
	return 0;
}

static char *
read_bootargs(void)
{
	size_t len = 0;
	char *buf;

	if (sysctlbyname("kern.bootargs", NULL, &len, NULL, 0) != 0 || len == 0) {
		return NULL;
	}
	buf = calloc(1, len + 1);
	if (!buf) {
		return NULL;
	}
	if (sysctlbyname("kern.bootargs", buf, &len, NULL, 0) != 0) {
		free(buf);
		return NULL;
	}
	buf[len] = 0;
	return buf;
}

/* the value of "word=" in a space separated command line, into out; 1 if found */
static int
bootarg_value(const char *cmdline, const char *word, char *out, size_t outlen)
{
	size_t wlen = strlen(word);
	const char *p = cmdline;

	while (p && *p) {
		const char *end = strchr(p, ' ');
		size_t tlen = end ? (size_t)(end - p) : strlen(p);

		if (tlen > wlen && strncmp(p, word, wlen) == 0 && p[wlen] == '=') {
			size_t vlen = tlen - wlen - 1;
			if (vlen >= outlen) {
				vlen = outlen - 1;
			}
			memcpy(out, p + wlen + 1, vlen);
			out[vlen] = 0;
			return 1;
		}
		p = end ? end + 1 : NULL;
	}
	return 0;
}

static void
load_config(int argc, char **argv)
{
	char *cmdline = read_bootargs();
	char value[512];
	int i;
	int have_subsystems = 0;
	int have_procs = 0;

	if (cmdline) {
		if (bootarg_value(cmdline, "oslog", value, sizeof(value))) {
			parse_spec_list(value);
			have_subsystems = 1;
		}
		if (bootarg_value(cmdline, "oslog_proc", value, sizeof(value))) {
			const char *p = value;
			while (*p) {
				const char *comma = strchr(p, ',');
				size_t len = comma ? (size_t)(comma - p) : strlen(p);
				char name[64];
				if (len >= sizeof(name)) {
					len = sizeof(name) - 1;
				}
				memcpy(name, p, len);
				name[len] = 0;
				if (len) {
					add_proc(name);
				}
				if (!comma) {
					break;
				}
				p = comma + 1;
			}
			have_procs = 1;
		}
		if (bootarg_value(cmdline, "oslog_rate", value, sizeof(value))) {
			rate = atof(value);
		}
		if (bootarg_value(cmdline, "oslog_burst", value, sizeof(value))) {
			burst = atoi(value);
		}
		if (bootarg_value(cmdline, "oslog_pred", value, sizeof(value))) {
			send_predicate = atoi(value) != 0;
		}
		if (bootarg_value(cmdline, "oslog_store", value, sizeof(value))) {
			store_fallback = atoi(value) != 0;
		}
		if (bootarg_value(cmdline, "oslog_store_secs", value, sizeof(value))) {
			store_secs = atoi(value);
		}
		if (bootarg_value(cmdline, "oslog_store_wait", value, sizeof(value))) {
			store_wait = atoi(value);
		}
		if (bootarg_value(cmdline, "oslog_dump", value, sizeof(value))) {
			if (strcmp(value, "0") == 0) {
				dump_mode = 0;
			} else if (strcmp(value, "errors") == 0 || strcmp(value, "2") == 0) {
				dump_mode = 2;
			} else {
				dump_mode = 1;
			}
		}
		if (bootarg_value(cmdline, "oslog_dump_bytes", value, sizeof(value))) {
			dump_bytes_limit = strtoull(value, NULL, 0);
		}
		if (bootarg_value(cmdline, "oslog_dump_lines", value, sizeof(value))) {
			dump_lines_limit = strtoull(value, NULL, 0);
		}
		if (bootarg_value(cmdline, "oslog_dump_rate", value, sizeof(value))) {
			dump_rate = atof(value);
		}
		if (bootarg_value(cmdline, "oslog_dump_burst", value, sizeof(value))) {
			dump_burst = atoi(value);
		}
		if (bootarg_value(cmdline, "oslog_dump_bare", value, sizeof(value))) {
			dump_bare_limit = strtoull(value, NULL, 0);
		}
		if (bootarg_value(cmdline, "oslog_dump_activity", value, sizeof(value))) {
			dump_activity = atoi(value) != 0;
		}
		if (bootarg_value(cmdline, "oslog_classes", value, sizeof(value))) {
			classes_mode = atoi(value) != 0;
		}
		if (bootarg_value(cmdline, "oslog_app", value, sizeof(value))) {
			const char *p = value;

			while (*p) {
				const char *comma = strchr(p, ',');
				size_t len = comma ? (size_t)(comma - p) : strlen(p);
				char name[64];

				if (len >= sizeof(name)) {
					len = sizeof(name) - 1;
				}
				memcpy(name, p, len);
				name[len] = 0;
				if (len) {
					add_app(name);
				}
				if (!comma) {
					break;
				}
				p = comma + 1;
			}
		}
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--subsystem") == 0 && i + 1 < argc) {
			if (!have_subsystems) {
				nrules = 0;
				have_subsystems = 1;
			}
			parse_spec_list(argv[++i]);
		} else if (strcmp(argv[i], "--all") == 0) {
			nrules = 0;
			add_rule("*", 1);
			have_subsystems = 1;
		} else if (strcmp(argv[i], "--process") == 0 && i + 1 < argc) {
			if (!have_procs) {
				nprocs = 0;
				have_procs = 1;
			}
			add_proc(argv[++i]);
		} else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
			rate = atof(argv[++i]);
		} else if (strcmp(argv[i], "--burst") == 0 && i + 1 < argc) {
			burst = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc) {
			/* what the stream is delivering at all, without the filter and
			 * without the rate limit: the first N events, then exit */
			probe_left = atoi(argv[++i]);
			nrules = 0;
			add_rule("*", 1);
			have_subsystems = 1;
			rate = 0.0;
			global_rate = 0.0;
		} else if (strcmp(argv[i], "--console") == 0 && i + 1 < argc) {
			if (console_fd >= 0) {
				close(console_fd);
			}
			console_fd = open(argv[++i], O_WRONLY | O_NONBLOCK | O_NOCTTY);
		} else if (strcmp(argv[i], "--pred") == 0) {
			send_predicate = 1;
		} else if (strcmp(argv[i], "--no-store") == 0) {
			store_fallback = 0;
		} else if (strcmp(argv[i], "--store-secs") == 0 && i + 1 < argc) {
			store_secs = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--store") == 0) {
			store_fallback = 1;
			store_secs = 0;         /* read the archive now, not after a silence */
		} else if (strcmp(argv[i], "--dump") == 0) {
			dump_mode = 1;
		} else if (strcmp(argv[i], "--dump-errors") == 0) {
			dump_mode = 2;
		} else if (strcmp(argv[i], "--no-dump") == 0) {
			dump_mode = 0;
		} else if (strcmp(argv[i], "--dump-bytes") == 0 && i + 1 < argc) {
			dump_bytes_limit = strtoull(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--dump-lines") == 0 && i + 1 < argc) {
			dump_lines_limit = strtoull(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--dump-activity") == 0) {
			dump_activity = 1;
		} else if (strcmp(argv[i], "--classes") == 0) {
			classes_mode = 1;
		} else if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
			add_app(argv[++i]);
		}
	}
	if (!have_subsystems) {
		parse_spec_list(default_spec);
	}
	if (napps == 0) {
		int i;

		for (i = 0; default_app_names[i]; i++) {
			add_app(default_app_names[i]);
		}
	}
	/* X3: a dump is the default.  TEST14's live stream had 1136 handler calls and not
	 * one event with a subsystem; a reader that waits for a matching subsystem prints
	 * nothing at all there.  The budget keeps the console safe; oslog_dump=0 gives the
	 * old subsystem-filtered behaviour back. */
	/* the predicate and the dump are mutually exclusive: a server-side subsystem
	 * filter in dump mode is the X3 fault again, one layer down */
	if (dump_mode) {
		send_predicate = 0;
	}
	if (dump_bytes_limit == 0) {
		dump_bytes_limit = 64 * 1024;
	}
	if (dump_lines_limit == 0) {
		dump_lines_limit = 400;
	}
	if (dump_rate < 0.0) {
		dump_rate = 0.0;
	}
	if (dump_burst <= 0) {
		dump_burst = 1;
	}
	dump_tokens = (double)dump_burst;
	dump_last = now_sec();
	if (burst <= 0) {
		burst = 1;
	}
	/* the shared bucket: a few subsystems' worth of lines, so that one daemon alone can
	 * never take the console */
	global_burst = burst * 6;
	global_rate = rate > 0.0 ? rate * 6.0 : 0.0;
	global_tokens = (double)global_burst;
	global_last = now_sec();
	free(cmdline);
}

/* ------------------------------------------------------------------ */
/* filtering                                                           */

static int
rule_matches(const rule_t *r, const char *subsystem, const char *category)
{
	if (r->prefix) {
		size_t n = strlen(r->subsystem);
		if (strncmp(subsystem, r->subsystem, n) != 0) {
			return 0;
		}
	} else if (strcmp(r->subsystem, subsystem) != 0) {
		return 0;
	}
	if (r->has_category && strcmp(r->category, category) != 0) {
		return 0;
	}
	return 1;
}

static rule_t *
find_rule(const char *subsystem, const char *category)
{
	int i;

	for (i = 0; i < nrules; i++) {
		if (rule_matches(&rules[i], subsystem, category)) {
			return &rules[i];
		}
	}
	return NULL;
}

static int
proc_allowed(const char *process)
{
	int i;

	if (nprocs == 0) {
		return 1;
	}
	for (i = 0; i < nprocs; i++) {
		if (strcmp(procs[i], process) == 0) {
			return 1;
		}
	}
	return 0;
}

/* one token per subsystem per second, one shared bucket for all of them */
static int
take_token(rule_t *r)
{
	double t = now_sec();
	double elapsed;
	int ok = 1;

	if (rate > 0.0) {
		elapsed = t - r->last;
		if (elapsed > 0.0) {
			r->tokens += elapsed * rate;
			if (r->tokens > (double)burst) {
				r->tokens = (double)burst;
			}
			r->last = t;
		}
		if (r->tokens < 1.0) {
			ok = 0;
		} else {
			r->tokens -= 1.0;
		}
	}
	if (ok && global_rate > 0.0) {
		elapsed = t - global_last;
		if (elapsed > 0.0) {
			global_tokens += elapsed * global_rate;
			if (global_tokens > (double)global_burst) {
				global_tokens = (double)global_burst;
			}
			global_last = t;
		}
		if (global_tokens < 1.0) {
			ok = 0;
		} else {
			global_tokens -= 1.0;
		}
	}
	return ok;
}

/* ------------------------------------------------------------------ */
/* one event                                                           */

static id
ev_get(id ev, const char *sel_name, id fallback)
{
	SEL sel = sel_getUid(sel_name);

	if ([ev respondsToSelector:sel]) {
		return ((id (*)(id, SEL))objc_msgSend)(ev, sel);
	}
	return fallback;
}

/* An integer-returning method has to be called through its IMP: an objc_msgSend cast that
 * says "id" makes ARC treat the number as an object and retain it. */
/* X5: ...and only when the method really is a no-argument getter of an integer scalar.
 * respondsToSelector: alone is not enough on a private class: a selector that returns a
 * struct by value is called with a hidden result pointer in x8, and an (unsigned long
 * (*)(id, SEL)) call passes garbage there -- a memory write into nowhere.  The encoding
 * ("Q16@0" and the like) says which ABI the callee expects, so the ones this cast cannot
 * serve are skipped instead of guessed at. */
static int
ev_is_uint_getter(id ev, SEL sel)
{
	Method m;
	const char *enc;

	if (![ev respondsToSelector:sel]) {
		return 0;
	}
	m = class_getInstanceMethod(object_getClass(ev), sel);
	if (!m) {
		return 0;
	}
	enc = method_getTypeEncoding(m);
	if (!enc || !enc[0] || !strchr(enc, ':')) {
		return 0;
	}
	switch (enc[0]) {
	case 'c': case 'C': case 's': case 'S': case 'i': case 'I':
	case 'l': case 'L': case 'q': case 'Q': case 'B':
		return 1;
	default:
		return 0;               /* struct, float, object, pointer, array, ... */
	}
}

static unsigned long
ev_uint(id ev, const char *sel_name)
{
	SEL sel = sel_getUid(sel_name);
	unsigned long (*fn)(id, SEL);

	if (!ev_is_uint_getter(ev, sel)) {
		return 0;
	}
	fn = (unsigned long (*)(id, SEL))(void *)[ev methodForSelector:sel];
	return fn(ev, sel);
}

static void
escape(const char *in, char *out, size_t outlen)
{
	size_t o = 0;

	if (!in) {
		in = "";
	}
	for (; *in && o + 1 < outlen; in++) {
		unsigned char c = (unsigned char)*in;
		if (c == '\n' || c == '\r' || c == '\t') {
			out[o++] = ' ';
		} else if (c < 0x20 || c == 0x7f) {
			out[o++] = '?';
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* X3: the unfiltered dump                                             */

static const char *
type_name(unsigned long type)
{
	switch (type) {
	case 0:  return "default";
	case 1:  return "info";
	case 2:  return "debug";
	case 16: return "error";
	case 17: return "fault";
	default: return "?";
	}
}

static void
append(char *buf, size_t len, int *used, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*used < 0 || (size_t)*used >= len) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(buf + *used, len - (size_t)*used, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if ((size_t)n >= len - (size_t)*used) {
		*used = (int)len - 1;
	} else {
		*used += n;
	}
}

static void
append_quoted(char *buf, size_t len, int *used, const char *key, NSString *s)
{
	char tmp[MSG_LEN];

	if (![s isKindOfClass:[NSString class]]) {
		return;
	}
	escape([s UTF8String], tmp, sizeof(tmp));
	append(buf, len, used, " %s=\"%s\"", key, tmp);
}

/* The raw value of one decomposed argument.  -rawValue is a small struct (the raw
 * placeholder's value, not its formatted representation); NSInvocation copies it by
 * the method's own type encoding, so no guess about the layout is needed here. */
static void
raw_value_hex(id arg, char *out, size_t outlen)
{
	SEL sel = sel_getUid("rawValue");
	NSMethodSignature *sig;
	NSInvocation *inv;
	NSUInteger n, i;
	unsigned char bytes[32];

	out[0] = 0;
	if (![arg respondsToSelector:sel]) {
		return;
	}
	@try {
		sig = [arg methodSignatureForSelector:sel];
		if (!sig) {
			return;
		}
		n = [sig methodReturnLength];
		if (n == 0 || n > sizeof(bytes)) {
			n = (n > sizeof(bytes)) ? sizeof(bytes) : 0;
		}
		if (n == 0) {
			return;
		}
		inv = [NSInvocation invocationWithMethodSignature:sig];
		[inv setTarget:arg];
		[inv setSelector:sel];
		[inv invoke];
		[inv getReturnValue:bytes];
		for (i = 0; i < n && i * 2 + 2 < outlen; i++) {
			snprintf(out + i * 2, outlen - i * 2, "%02x", bytes[i]);
		}
	} @catch (NSException *e) {
		out[0] = 0;
	}
}

/* Walk OSLogEventDecomposedMessage/OSLogEventMessageArgument without assuming a
 * particular selector set: the class lists in the dyld cache do not decode, and the
 * next iOS may move a method.  Every step is optional. */
static void
append_args(char *buf, size_t len, int *used, id ev)
{
	id dm = ev_get(ev, "decomposedMessage", nil);
	unsigned long count, i, shown = 0;

	if (!dm) {
		return;
	}
	count = ev_uint(dm, "placeholderCount");
	if (count > 16) {
		count = 16;
	}
	for (i = 0; i < count; i++) {
		id arg = nil;
		id rep;
		char repr[160], raw[80];

		if ([dm respondsToSelector:sel_getUid("argumentAtIndex:")]) {
			arg = ((id (*)(id, SEL, unsigned long))objc_msgSend)(dm,
			    sel_getUid("argumentAtIndex:"), i);
		}
		if (!arg && [dm respondsToSelector:@selector(segments)]) {
			id segs = ev_get(dm, "segments", nil);
			if ([segs isKindOfClass:[NSArray class]] && i < [segs count]) {
				id seg = [segs objectAtIndex:i];
				id ph = ev_get(seg, "placeholder", nil);
				arg = ev_get(ph, "argument", nil);
			}
		}
		if (!arg) {
			continue;
		}
		rep = ev_get(arg, "objectRepresentation", nil);
		escape([rep isKindOfClass:[NSString class]] ? [rep UTF8String] :
		    (rep ? [[rep description] UTF8String] : ""), repr, sizeof(repr));
		raw_value_hex(arg, raw, sizeof(raw));
		append(buf, len, used, " a%lu={t=%lu p=%lu v=\"%s\"%s%s}",
		    i, ev_uint(arg, "scalarType"), ev_uint(arg, "privacy"), repr,
		    raw[0] ? " raw=" : "", raw);
		shown++;
	}
	if (count && !shown) {
		append(buf, len, used, " args=%lu(unreadable)", count);
	}
}

static void
append_raw_bytes(char *buf, size_t len, int *used, id ev)
{
	id data = ev_get(ev, "rawBytes", nil);
	NSUInteger n, i;

	if (![data isKindOfClass:[NSData class]]) {
		return;
	}
	n = [data length];
	if (n == 0) {
		return;
	}
	if (n > 32) {
		n = 32;
	}
	append(buf, len, used, " bytes=");
	for (i = 0; i < n; i++) {
		append(buf, len, used, "%02x", ((const unsigned char *)[data bytes])[i]);
	}
	if ([data length] > n) {
		append(buf, len, used, "..");
	}
}

static int
dump_take(size_t bytes, int priority)
{
	double t = now_sec();
	unsigned long long line_reserve = priority ? 0 : 100;
	unsigned long long byte_reserve = priority ? 0 : 16 * 1024;

	if (dump_lines_used + line_reserve >= dump_lines_limit ||
	    dump_bytes_used + byte_reserve + bytes > dump_bytes_limit) {
		dump_dropped_events++;
		if (!dump_exhausted_said) {
			dump_exhausted_said = 1;
			say("oslogfwd: dump budget exhausted (%llu lines, %llu bytes): "
			    "the rest is counted, not printed",
			    dump_lines_used, dump_bytes_used);
		}
		return 0;
	}
	if (!priority && dump_rate > 0.0) {
		double elapsed = t - dump_last;

		if (elapsed > 0.0) {
			dump_tokens += elapsed * dump_rate;
			if (dump_tokens > (double)dump_burst) {
				dump_tokens = (double)dump_burst;
			}
			dump_last = t;
		}
		if (dump_tokens < 1.0) {
			dump_dropped_events++;
			return 0;
		}
		dump_tokens -= 1.0;
	}
	dump_lines_used++;
	dump_bytes_used += bytes;
	return 1;
}

/* X3: the uuid of a sender image, and its path, are what a line needs to be decoded
 * offline.  On a system without uuidtext the format string is not in the event; it is in
 * the sender's own __TEXT,__oslogstring, and the image is what names it. */
static void
append_uuid(char *buf, size_t len, int *used, const char *key, id uuid)
{
	char tmp[64];

	tmp[0] = 0;
	if ([uuid isKindOfClass:[NSUUID class]]) {
		snprintf(tmp, sizeof(tmp), "%s", [[uuid UUIDString] UTF8String]);
	} else if ([uuid isKindOfClass:[NSString class]]) {
		snprintf(tmp, sizeof(tmp), "%s", [uuid UTF8String]);
	}
	if (tmp[0]) {
		append(buf, len, used, " %s=%s", key, tmp);
	}
}

static int
dump_event(id ev, const char *proc, long pid, const char *sub, const char *cat,
    unsigned long type, int priority)
{
	char line[DUMP_LEN], clock[32];
	NSString *message, *format, *raw;
	int used = 0, bare;

	message = ev_get(ev, "composedMessage", nil);
	format = ev_get(ev, "formatString", nil);
	raw = ev_get(ev, "rawString", nil);

	/* One handler call is only a log line if it has something to say.  An activity or
	 * a signpost has neither a format string nor a composed message; printing its
	 * proxy would spend the budget the application's error needs.  An app line and an
	 * error/fault are always kept, with whatever fields they have. */
	bare = 0;
	if (![message isKindOfClass:[NSString class]] &&
	    ![format isKindOfClass:[NSString class]] &&
	    ![raw isKindOfClass:[NSString class]]) {
		dump_no_msg++;
		if (dump_activity) {
			bare = 0;
		} else if (dump_bare_shown < dump_bare_limit) {
			dump_bare_shown++;
			bare = 1;
		} else if (!priority) {
			return 0;
		}
	}
	dump_events++;
	if (!bare && ([message isKindOfClass:[NSString class]] ||
	    [format isKindOfClass:[NSString class]] || [raw isKindOfClass:[NSString class]])) {
		dump_with_msg++;
	}

	stamp(clock, sizeof(clock));
	append(line, sizeof(line), &used, "oslog-dump %s %s[%ld] t=%lu/%s sub=%s cat=%s",
	    clock, proc, pid, type, type_name(type), sub[0] ? sub : "-", cat[0] ? cat : "-");
	append_quoted(line, sizeof(line), &used, "msg", message);
	append_quoted(line, sizeof(line), &used, "fmt", format);
	append_quoted(line, sizeof(line), &used, "raw", raw);
	append_args(line, sizeof(line), &used, ev);
	append_raw_bytes(line, sizeof(line), &used, ev);
	append_quoted(line, sizeof(line), &used, "img", ev_get(ev, "senderImagePath", nil));
	append_uuid(line, sizeof(line), &used, "uuid", ev_get(ev, "senderImageUUID", nil));
	if (bare) {
		append(line, sizeof(line), &used, " bare=1");
	}
	append(line, sizeof(line), &used, " at=%llu thr=%llu",
	    (unsigned long long)ev_uint(ev, "senderImageOffset"),
	    (unsigned long long)ev_uint(ev, "threadIdentifier"));
	if (used <= 0) {
		dump_dropped_events++;
		return 0;
	}
	if (!dump_take((size_t)used + 1, priority)) {
		return 0;
	}
	append(line, sizeof(line), &used, "\n");
	out_line(line);
	n_printed++;
	return 1;
}

/* ------------------------------------------------------------------ */
/* X3: what the private classes actually offer on this system          */

static void
dump_class(const char *name)
{
	Class cls = NSClassFromString([NSString stringWithUTF8String:name]);
	unsigned int i, n;
	Method *methods;
	Ivar *ivars;

	if (!cls) {
		say("oslogfwd: class %s: NO", name);
		return;
	}
	methods = class_copyMethodList(cls, &n);
	say("oslogfwd: class %s: %u methods", name, n);
	for (i = 0; i < n && i < 96; i++) {
		const char *types = method_getTypeEncoding(methods[i]);

		say("oslogfwd:   -%s %s", sel_getName(method_getName(methods[i])),
		    types ? types : "");
	}
	free(methods);
	ivars = class_copyIvarList(cls, &n);
	say("oslogfwd: class %s: %u ivars", name, n);
	for (i = 0; i < n && i < 64; i++) {
		const char *iname = ivar_getName(ivars[i]);
		const char *itypes = ivar_getTypeEncoding(ivars[i]);

		say("oslogfwd:   %s %s", iname ? iname : "?", itypes ? itypes : "");
	}
	free(ivars);
}

static void
handle_event(id ev, const char *tag)
{
	static const char *type_names[] = { "default", "info", "debug", "error", "fault" };
	NSString *subsystem, *category, *process, *message;
	char sub[SUBSYS_LEN], cat[CATEGORY_LEN], proc[96], msg[MSG_LEN], clock[32];
	char path[512];
	rule_t *rule;
	NSUInteger type = 0;
	long pid = 0;
	char line[LINE_LEN];
	int n, app, priority, printed;

	n_events++;
	subsystem = ev_get(ev, "subsystem", nil);
	category = ev_get(ev, "category", nil);
	process = ev_get(ev, "process", nil);
	type = (NSUInteger)ev_uint(ev, "type");
	pid = (long)(int)ev_uint(ev, "processIdentifier");

	/* X3: the catalog entry that carries -process is one of the things that is missing
	 * when the stream has no subsystem.  The pid is in every tracepoint, so ask libproc
	 * rather than print "?" and lose the one field that identifies the application. */
	if (![process isKindOfClass:[NSString class]] || [process length] == 0) {
		char pname[64];

		if (pid > 0 && proc_name((int)pid, pname, sizeof(pname)) > 0) {
			process = [NSString stringWithUTF8String:pname];
		}
	}
	if (![process isKindOfClass:[NSString class]] || [process length] == 0) {
		process = @"?";
	}

	escape([subsystem isKindOfClass:[NSString class]] ? [subsystem UTF8String] : "",
	    sub, sizeof(sub));
	escape([category isKindOfClass:[NSString class]] ? [category UTF8String] : "",
	    cat, sizeof(cat));
	escape([process UTF8String], proc, sizeof(proc));

	if (sub[0]) {
		n_seen++;
	}

	app = app_match(proc);
	if (!app && pid > 0 && proc_pidpath((int)pid, path, sizeof(path)) > 0) {
		path[sizeof(path) - 1] = 0;
		app = app_match(path);
	}
	priority = app || type == 16 || type == 17;

	/* X3: the dump is before every filter.  It has its own byte/line budget and its
	 * own rate limit; app and error/fault lines have the last reserve of that budget. */
	if (dump_mode) {
		if (dump_mode == 2 && type != 16 && type != 17) {
			return;
		}
		printed = dump_event(ev, proc, pid, sub, cat, (unsigned long)type, priority);
		if (printed && probe_left > 0 && --probe_left == 0) {
			stop_now = 1;
		}
		return;
	}

	if (!sub[0]) {
		return;         /* the old, subsystem-only path */
	}
	rule = find_rule(sub, cat);
	if (!rule || !proc_allowed(proc)) {
		return;
	}
	rule->seen++;
	if (!take_token(rule)) {
		n_dropped++;            /* before -composedMessage: a dropped line costs nothing */
		return;
	}
	/* the message is composed here and not before: a big %{public}@ that nobody was
	 * going to see is work done in somebody else's process */
	message = ev_get(ev, "composedMessage", nil);
	if (![message isKindOfClass:[NSString class]]) {
		message = ev_get(ev, "formatString", nil);
	}
	escape([message isKindOfClass:[NSString class]] ? [message UTF8String] : "", msg, sizeof(msg));
	stamp(clock, sizeof(clock));
	if (type < sizeof(type_names) / sizeof(type_names[0])) {
		n = snprintf(line, sizeof(line), "%s %s %s[%ld] %s%s%s %s: %s\n",
		    tag, clock, proc, pid, sub, cat[0] ? ":" : "", cat,
		    type_names[type], msg);
	} else {
		n = snprintf(line, sizeof(line), "%s %s %s[%ld] %s%s%s t=%lu: %s\n",
		    tag, clock, proc, pid, sub, cat[0] ? ":" : "", cat, (unsigned long)type, msg);
	}
	if (n > 0) {
		rule->printed++;
		n_printed++;
		out_line(line);
		if (probe_left > 0 && --probe_left == 0) {
			stop_now = 1;
		}
	}
}

/* ------------------------------------------------------------------ */
/* the live stream                                                     */

static NSString *backend_name;

/* The shapes the private framework may want, tried in order.  Only two things are certain
 * from the cache: the classes exist, and these selectors exist somewhere in the framework.
 * A shape that does not fit returns nil or an object that cannot be activated, and the next
 * one is tried; whatever worked is what the first console line reports. */
static id
candidate(id object, const char *how, NSString **why)
{
	if (!object) {
		return nil;
	}
	if (![object respondsToSelector:sel_getUid("activate")]) {
		if (why) {
			*why = [NSString stringWithFormat:@"%s gave an object with no -activate", how];
		}
		return nil;
	}
	backend_name = [NSString stringWithUTF8String:how];
	return object;
}

static id
make_stream_with_predicate(NSPredicate *predicate, NSString **why)
{
	Class store = NSClassFromString(@"OSLogEventLiveStore");
	Class live = NSClassFromString(@"OSLogEventLiveStream");
	Class source_cls = NSClassFromString(@"OSLogEventLiveSource");
	id object = nil;
	NSError *error = nil;

	if (store && [store respondsToSelector:@selector(liveStreamWithFilterPredicate:error:)]) {
		object = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(store,
		    @selector(liveStreamWithFilterPredicate:error:), predicate, &error);
		object = candidate(object, "OSLogEventLiveStore/liveStreamWithFilterPredicate", why);
		if (object) {
			return object;
		}
		if (why && !*why && error) {
			*why = [error description];
		}
	}

	if (store && [store respondsToSelector:@selector(liveLocalStore)]) {
		object = candidate(((id (*)(id, SEL))objc_msgSend)(store, @selector(liveLocalStore)),
		    "OSLogEventLiveStore/liveLocalStore", why);
		if (object) {
			return object;
		}
	}

	if (live && [live instancesRespondToSelector:@selector(initWithLiveSource:)] && source_cls) {
		id source = ((id (*)(id, SEL))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(source_cls,
		    @selector(alloc)), @selector(init));
		if (source) {
			object = ((id (*)(id, SEL, id))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(live,
			    @selector(alloc)), @selector(initWithLiveSource:), source);
			object = candidate(object, "OSLogEventLiveStream/initWithLiveSource", why);
			if (object) {
				return object;
			}
		}
	}

	if (live && [live instancesRespondToSelector:@selector(initWithPredicate:)]) {
		object = ((id (*)(id, SEL, id))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(live,
		    @selector(alloc)), @selector(initWithPredicate:), predicate);
		object = candidate(object, "OSLogEventLiveStream/initWithPredicate", why);
		if (object) {
			return object;
		}
	}

	if (live && [live instancesRespondToSelector:@selector(initWithSource:)]) {
		id source = nil;
		if (source_cls) {
			source = ((id (*)(id, SEL))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(source_cls,
			    @selector(alloc)), @selector(init));
		}
		object = ((id (*)(id, SEL, id))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(live,
		    @selector(alloc)), @selector(initWithSource:), source);
		object = candidate(object, "OSLogEventLiveStream/initWithSource", why);
		if (object) {
			return object;
		}
	}

	if (live) {
		object = ((id (*)(id, SEL))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(live,
		    @selector(alloc)), @selector(init));
		object = candidate(object, "OSLogEventLiveStream/init", why);
		if (object) {
			return object;
		}
	}
	if (why && !*why) {
		*why = @"no usable OSLogEventLiveStream initializer";
	}
	return nil;
}

static void
on_stream_invalidated(id stream)
{
	id why = nil;

	stream_invalidated = 1;
	@try {
		why = ev_get(stream, "error", nil);
	} @catch (NSException *e) {
		why = nil;
	}
	say("oslogfwd: the stream was invalidated%s%s",
	    why ? ": " : " with no error object",
	    why ? [[why description] UTF8String] : "");
}

static BOOL
attach_and_activate(id stream, NSPredicate *predicate, NSString **why)
{
	SEL set_stream_handler = sel_getUid("setStreamHandler:");
	SEL set_event_handler = sel_getUid("setEventHandler:");
	void (^handler)(id) = ^(id event) {
		@autoreleasepool {
			live_events++;
			handle_event(event, "oslog");
		}
	};

	if (!stream) {
		return NO;
	}
	stream_invalidated = 0;
	@try {
		if ([stream respondsToSelector:@selector(setInvalidationHandler:)]) {
			void (^invalid)(id) = ^(id s) {
				@autoreleasepool {
					on_stream_invalidated(s);
				}
			};
			((void (*)(id, SEL, id))objc_msgSend)(stream,
			    @selector(setInvalidationHandler:), invalid);
		}
		/* The predicate is translated by the framework's own mapper inside
		 * diagnosticd: cheap (only the named subsystems cross the XPC) but a shape
		 * the mapper does not understand can leave a stream that is perfectly
		 * connected and delivers nothing, which is indistinguishable from the refusal
		 * of TEST13B.  The main loop therefore drops it and reconnects when the
		 * stream has said nothing for a minute; the filter here (find_rule) is always
		 * the one that decides what is printed. */
		if (send_predicate && [stream respondsToSelector:@selector(setFilterPredicate:)] && predicate) {
			((void (*)(id, SEL, id))objc_msgSend)(stream, @selector(setFilterPredicate:), predicate);
		}
		if ([stream respondsToSelector:set_stream_handler]) {
			((void (*)(id, SEL, id))objc_msgSend)(stream, set_stream_handler, handler);
		} else if ([stream respondsToSelector:set_event_handler]) {
			((void (*)(id, SEL, id))objc_msgSend)(stream, set_event_handler, handler);
		} else {
			if (why) {
				*why = @"stream takes neither setStreamHandler: nor setEventHandler:";
			}
			return NO;
		}
		if (![stream respondsToSelector:@selector(activate)]) {
			if (why) {
				*why = @"stream has no -activate";
			}
			return NO;
		}
		((void (*)(id, SEL))objc_msgSend)(stream, @selector(activate));
	} @catch (NSException *e) {
		if (why) {
			*why = [NSString stringWithFormat:@"%@: %@", [e name], [e reason]];
		}
		return NO;
	}
	return YES;
}

/* A predicate only for the rules the mapper certainly understands: exact subsystem and
 * exact subsystem+category.  Anything else (a prefix rule, "all") is left to handle_event,
 * because a predicate the framework cannot translate would fail the whole stream. */
static NSPredicate *
make_predicate(void)
{
	NSMutableArray *parts = [NSMutableArray array];
	int i;

	/* the dump wants every event, not just the named subsystems: a server-side
	 * predicate here would hide exactly the events that have no subsystem */
	if (dump_mode) {
		return nil;
	}
	for (i = 0; i < nrules; i++) {
		if (rules[i].prefix) {
			continue;
		}
		if (rules[i].has_category) {
			[parts addObject:[NSString stringWithFormat:@"(subsystem == \"%s\" AND category == \"%s\")",
			    rules[i].subsystem, rules[i].category]];
		} else {
			[parts addObject:[NSString stringWithFormat:@"subsystem == \"%s\"", rules[i].subsystem]];
		}
	}
	if ([parts count] == 0) {
		return nil;
	}
	@try {
		return [NSPredicate predicateWithFormat:[parts componentsJoinedByString:@" OR "]];
	} @catch (NSException *e) {
		return nil;
	}
}

/* ------------------------------------------------------------------ */
/* the archive logd writes, for when the live stream says nothing      */

/* logd writes .tracev3 files into /private/var/db/diagnostics; var_store leaves that
 * directory out of the saved volume on purpose ("tens of MB a boot, and remade"), which
 * is also the evidence that it fills up.  Its size answers the one question that has to
 * be answered before anything else in the next log: is os_log itself alive in this boot
 * (files growing) or not (no files)?  Without that, "0 seen" is unattributable. */
typedef struct {
	unsigned long long files;
	unsigned long long bytes;
	long newest_age;
} tree_count_t;

/* X3: logd does not write tracev3 files at the top of /private/var/db/diagnostics.  Its
 * book is Persist/logdata.Persistent.<uuid>.tracev3 (strings "Persist" and
 * "logdata.Persistent." in logd), and uuidtext is /private/var/db/uuidtext/<XX>/<uuid>
 * and .../dsc/... ("%02X" in logd).  The X2 statistic counted the top level only, so
 * "0 .tracev3, 0 uuidtext" in TEST14 was not evidence that logd wrote nothing. */
static void
count_tree(const char *path, int depth, const char *suffix, tree_count_t *tc, time_t now)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (!d) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char child[512];
		struct stat st;

		if (e->d_name[0] == '.') {
			continue;
		}
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		if (lstat(child, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth > 0) {
				count_tree(child, depth - 1, suffix, tc, now);
			}
			continue;
		}
		if (!S_ISREG(st.st_mode)) {
			continue;
		}
		if (suffix && strstr(e->d_name, suffix) == NULL) {
			continue;
		}
		tc->files++;
		tc->bytes += (unsigned long long)st.st_size;
		if (tc->newest_age < 0 || st.st_mtime > now - tc->newest_age) {
			tc->newest_age = (long)(now - st.st_mtime);
		}
	}
	closedir(d);
}

static void
keybag_state(void)
{
	static void *handle;
	static int tried;
	int (*unlocked)(void) = NULL;
	int (*formatted)(void) = NULL;

	if (!tried) {
		tried = 1;
		handle = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",
		    RTLD_LAZY);
	}
	if (!handle) {
		say("oslogfwd: keybag: MobileKeyBag is not loadable (%s)", dlerror());
		return;
	}
	unlocked = (int (*)(void))dlsym(handle, "MKBDeviceUnlockedSinceBoot");
	formatted = (int (*)(void))dlsym(handle, "MKBDeviceFormattedForContentProtection");
	say("oslogfwd: keybag: MKBDeviceUnlockedSinceBoot=%s MKBDeviceFormattedForContentProtection=%s",
	    unlocked ? (unlocked() ? "1" : "0") : "?",
	    formatted ? (formatted() ? "1" : "0") : "?");
}

static void
archive_stats(void)
{
	static const char *const dirs[] = { "/private/var/db/diagnostics", "/var/db/diagnostics" };
	static const char *const uuid_dirs[] = { "/private/var/db/uuidtext", "/var/db/uuidtext" };
	static const char *const books[] = { "Persist", "Special", "Signpost", "HighVolume", "Subsystems" };
	int i, b;

	for (i = 0; i < 2; i++) {
		struct stat dst;
		struct statfs sf;
		tree_count_t all = { 0, 0, -1 }, us = { 0, 0, -1 };
		char sub[512];
		time_t now = time(NULL);
		int writable;

		if (stat(dirs[i], &dst) != 0) {
			continue;
		}
		count_tree(dirs[i], 4, ".tracev3", &all, now);
		count_tree(uuid_dirs[i], 4, NULL, &us, now);
		writable = access(dirs[i], W_OK) == 0;
		memset(&sf, 0, sizeof(sf));
		(void)statfs(dirs[i], &sf);
		say("oslogfwd: archive %s: %llu .tracev3 in the tree, %llu bytes, newest %ld s old; "
		    "uuidtext %llu files, %llu bytes; mode %o uid %d gid %d writable=%d free=%llu",
		    dirs[i], all.files, all.bytes, all.newest_age, us.files, us.bytes,
		    dst.st_mode & 07777, dst.st_uid, dst.st_gid, writable,
		    (unsigned long long)sf.f_bavail * (unsigned long long)sf.f_bsize);
		for (b = 0; b < (int)(sizeof(books) / sizeof(books[0])); b++) {
			tree_count_t one = { 0, 0, -1 };
			struct stat bs;

			snprintf(sub, sizeof(sub), "%s/%s", dirs[i], books[b]);
			count_tree(sub, 2, ".tracev3", &one, now);
			/* X3: printed even when the book is absent.  "the book does not exist" and
			 * "the book exists and is empty" are different faults -- logd creates its
			 * books when it starts persisting -- and the old statistic could say
			 * neither. */
			if (stat(sub, &bs) != 0) {
				say("oslogfwd: archive %s/%s: absent", dirs[i], books[b]);
			} else {
				say("oslogfwd: archive %s/%s: %llu files, %llu bytes, newest %ld s old, "
				    "mode %o uid %d gid %d",
				    dirs[i], books[b], one.files, one.bytes, one.newest_age,
				    bs.st_mode & 07777, bs.st_uid, bs.st_gid);
			}
		}
		/* logd refuses to persist without a usable timesync file (the timestamps of
		 * a record are stored against it), and mkvarskel.py makes it a directory. */
		snprintf(sub, sizeof(sub), "%s/timesync", dirs[i]);
		{
			struct stat ts;

			if (stat(sub, &ts) == 0) {
				say("oslogfwd: archive %s: timesync %s, %llu bytes, %ld s old, mode %o",
				    sub, S_ISDIR(ts.st_mode) ? "a directory" :
				    (S_ISREG(ts.st_mode) ? "a file" : "neither"),
				    (unsigned long long)ts.st_size, (long)(now - ts.st_mtime),
				    ts.st_mode & 07777);
			} else {
				say("oslogfwd: archive %s: absent", sub);
			}
		}
		keybag_state();
		return;
	}
	say("oslogfwd: archive: neither %s nor %s can be opened", dirs[0], dirs[1]);
}

static id store_obj;            /* OSLogEventStore: the local (this boot's) store */
static id store_stream;         /* one OSLogEventStream over it */
static NSString *store_how;     /* the shape that worked, for the log */
static unsigned long long store_last_ts;   /* dedup between polls */
static unsigned long long store_events, store_printed;   /* cumulative */
static unsigned long long store_poll_printed;

static id
make_local_store(NSString **why)
{
	Class st = NSClassFromString(@"OSLogEventStore");

	if (!st) {
		*why = @"no OSLogEventStore in LoggingSupport";
		return nil;
	}
	if ([st respondsToSelector:@selector(localStore)]) {
		id s = ((id (*)(id, SEL))objc_msgSend)(st, @selector(localStore));
		if (s) {
			store_how = @"OSLogEventStore/localStore";
			return s;
		}
		*why = @"+localStore returned nil";
		return nil;
	}
	if ([st respondsToSelector:@selector(localStoreWithError:)]) {
		NSError *error = nil;
		id s = ((id (*)(id, SEL, NSError **))objc_msgSend)(st, @selector(localStoreWithError:), &error);
		if (s) {
			store_how = @"OSLogEventStore/localStoreWithError:";
			return s;
		}
		*why = error ? [error description] : @"+localStoreWithError: returned nil";
		return nil;
	}
	*why = @"OSLogEventStore has no +localStore";
	return nil;
}

/* The store-to-stream step is the one shape nobody has documented for iOS 16.5 and the
 * class's method list in the dyld cache does not decode, so try what the selector names
 * allow, in order, and say which one produced a stream. */
static id
make_store_stream(id store, NSPredicate *predicate, NSString **why)
{
	static const char *const with_pred[] = {
		"streamWithFilterPredicate:error:", "streamWithPredicate:error:",
		"streamWithFilterPredicate:", "streamWithPredicate:", NULL
	};
	Class stream_class = NSClassFromString(@"OSLogEventStream");
	int i;

	for (i = 0; with_pred[i]; i++) {
		SEL sel = sel_getUid(with_pred[i]);
		id s;

		if (![store respondsToSelector:sel]) {
			continue;
		}
		if (strstr(with_pred[i], "error:") != NULL) {
			NSError *error = nil;
			s = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(store, sel, predicate, &error);
			if (!s && error) {
				*why = [error description];
			}
		} else {
			s = ((id (*)(id, SEL, id))objc_msgSend)(store, sel, predicate);
		}
		if (s) {
			store_how = [NSString stringWithFormat:@"OSLogEventStore/%s", with_pred[i]];
			return s;
		}
	}
	if ([store respondsToSelector:@selector(stream)]) {
		id s = ((id (*)(id, SEL))objc_msgSend)(store, @selector(stream));
		if (s) {
			store_how = @"OSLogEventStore/stream";
			return s;
		}
	}
	if (stream_class && [stream_class instancesRespondToSelector:@selector(initWithPredicate:)]) {
		id s = ((id (*)(id, SEL, id))objc_msgSend)(
		    ((id (*)(id, SEL))objc_msgSend)(stream_class, @selector(alloc)),
		    @selector(initWithPredicate:), predicate);
		if (s) {
			store_how = @"OSLogEventStream/initWithPredicate:";
			return s;
		}
	}
	if (stream_class) {
		@try {
			id s = ((id (*)(id, SEL))objc_msgSend)(
			    ((id (*)(id, SEL))objc_msgSend)(stream_class, @selector(alloc)),
			    @selector(init));
			if (s && [s respondsToSelector:@selector(activateStreamFromLastBoot)]) {
				store_how = @"OSLogEventStream/init";
				return s;
			}
		} @catch (NSException *e) {
			(void)e;
		}
	}
	if (why && *why == nil) {
		*why = @"no store-to-stream selector answered";
	}
	return nil;
}

static void
handle_store_event(id ev)
{
	SEL sel = sel_getUid("timestamp");
	unsigned long long ts = 0;

	if ([ev respondsToSelector:sel]) {
		unsigned long long (*fn)(id, SEL) = (unsigned long long (*)(id, SEL))(void *)
		    [ev methodForSelector:sel];
		ts = fn(ev, sel);
	}
	store_events++;
	if (ts && ts <= store_last_ts) {
		return;                 /* printed by an earlier poll */
	}
	if (ts) {
		store_last_ts = ts;
	}
	{
		unsigned long long before = n_printed;

		handle_event(ev, "oslog-ar");
		if (n_printed != before) {
			store_printed++;
			store_poll_printed++;
		}
	}
}

static void
store_stop(void)
{
	if (store_stream && [store_stream respondsToSelector:@selector(invalidate)]) {
		@try {
			((void (*)(id, SEL))objc_msgSend)(store_stream, @selector(invalidate));
		} @catch (NSException *e) {
			(void)e;
		}
	}
	store_stream = nil;
}

static int
store_activate(NSPredicate *predicate, NSString **why)
{
	void (^handler)(id) = ^(id event) {
		@autoreleasepool {
			handle_store_event(event);
		}
	};

	store_stream = make_store_stream(store_obj, predicate, why);
	if (!store_stream) {
		return 0;
	}
	@try {
		if ([store_stream respondsToSelector:@selector(setInvalidationHandler:)]) {
			void (^invalid)(id) = ^(id s) {
				(void)s;
				store_done = 1;
			};
			((void (*)(id, SEL, id))objc_msgSend)(store_stream,
			    @selector(setInvalidationHandler:), invalid);
		}
		if ([store_stream respondsToSelector:@selector(setEventHandler:)]) {
			((void (*)(id, SEL, id))objc_msgSend)(store_stream,
			    @selector(setEventHandler:), handler);
		} else if ([store_stream respondsToSelector:@selector(setStreamHandler:)]) {
			((void (*)(id, SEL, id))objc_msgSend)(store_stream,
			    @selector(setStreamHandler:), handler);
		} else {
			*why = @"the store stream takes neither setEventHandler: nor setStreamHandler:";
			return 0;
		}
		if (send_predicate && predicate &&
		    [store_stream respondsToSelector:@selector(setFilterPredicate:)]) {
			((void (*)(id, SEL, id))objc_msgSend)(store_stream,
			    @selector(setFilterPredicate:), predicate);
		}
		if ([store_stream respondsToSelector:@selector(activateStreamFromLastBoot)]) {
			((void (*)(id, SEL))objc_msgSend)(store_stream,
			    @selector(activateStreamFromLastBoot));
		} else if ([store_stream respondsToSelector:@selector(activate)]) {
			((void (*)(id, SEL))objc_msgSend)(store_stream, @selector(activate));
		} else {
			*why = @"the store stream has no -activateStreamFromLastBoot";
			return 0;
		}
	} @catch (NSException *e) {
		*why = [NSString stringWithFormat:@"%@: %@", [e name], [e reason]];
		return 0;
	}
	return 1;
}

/* One read of the archive from the last boot; entries already printed by an earlier read
 * are skipped by their timestamp.  Returns 0 when the path works. */
static int
store_poll(void)
{
	NSPredicate *predicate = make_predicate();
	NSString *why = nil;
	double until;
	int attempt;

	if (!store_obj) {
		store_obj = make_local_store(&why);
		if (!store_obj) {
			say("oslogfwd: archive: no local store (%s)", why ? [why UTF8String] : "unknown");
			return -1;
		}
		say("oslogfwd: archive: local store, %s", [store_how UTF8String]);
	}
	/* the predicate keeps the read cheap; if the mapper will not have it, read the boot
	 * without one rather than lose the fallback to the same shape question as the live
	 * stream (the filter here decides what is printed either way) */
	for (attempt = 0; attempt < 2; attempt++) {
		store_stop();
		store_done = 0;
		store_events = 0;
		store_poll_printed = 0;
		why = nil;
		if (!store_activate(predicate, &why)) {
			say("oslogfwd: archive: cannot stream the store (%s); %s",
			    why ? [why UTF8String] : "unknown",
			    store_how ? [store_how UTF8String] : "no shape tried");
			if (!predicate) {
				return -1;
			}
			say("oslogfwd: archive: retrying without the predicate");
			predicate = nil;
			continue;
		}
		say("oslogfwd: archive: reading from the last boot through %s", [store_how UTF8String]);
		until = now_sec() + (double)store_wait;
		while (!stop_now && !store_done && now_sec() < until) {
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
		}
		say("oslogfwd: archive: %llu entries offered, %llu printed, last timestamp %llu%s",
		    store_events, store_poll_printed, store_last_ts,
		    store_done ? "" : " (timed out)");
		store_stop();
		return 0;
	}
	return -1;
}

static void
print_rules(void)
{
	int i;
	char buf[512];
	int n = 0;

	buf[0] = 0;
	for (i = 0; i < nrules && n < (int)sizeof(buf) - 80; i++) {
		n += snprintf(buf + n, sizeof(buf) - n, "%s%s%s%s",
		    i ? "," : "", rules[i].subsystem, rules[i].prefix ? "*" : "",
		    rules[i].has_category ? ":" : "");
		if (rules[i].has_category) {
			n += snprintf(buf + n, sizeof(buf) - n, "%s", rules[i].category);
		}
	}
	say("oslogfwd: subsystems %s", buf[0] ? buf : "(none)");
}

/* ------------------------------------------------------------------ */
/* selftest                                                            */

static int
selftest(void)
{
	static const char *class_names[] = {
		"OSLogEventLiveStore", "OSLogEventLiveStream", "OSLogEventStreamBase",
		"OSLogEventStream", "OSLogEventLiveSource", "OSLogEventProxy",
	};
	static const char *selectors[] = {
		"liveStreamWithFilterPredicate:error:", "liveLocalStore",
		"initWithLiveSource:", "initWithPredicate:", "initWithSource:", "init",
		"setStreamHandler:", "setEventHandler:", "setFilterPredicate:",
		"activate", "invalidate", "invalidated",
		"subsystem", "category", "process", "processIdentifier",
		"composedMessage", "formatString", "type",
	};
	char *cmdline;
	NSPredicate *predicate;
	NSString *why = nil;
	id stream;
	int i, j;

	say("oslogfwd: selftest (%s), pid %d uid %d euid %d gid %d", OSLFWD_VERSION, getpid(),
	    getuid(), geteuid(), getgid());
	cmdline = read_bootargs();
	say("oslogfwd: kern.bootargs \"%s\"", cmdline ? cmdline : "(unreadable)");
	free(cmdline);
	print_rules();
	if (nprocs) {
		char buf[256] = "";
		int n = 0;
		for (i = 0; i < nprocs; i++) {
			n += snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "," : "", procs[i]);
		}
		say("oslogfwd: processes %s, rate %.1f/s burst %d", buf, rate, burst);
	} else {
		say("oslogfwd: processes (all), rate %.1f/s burst %d", rate, burst);
	}

	if (dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW) == NULL) {
		say("oslogfwd: LoggingSupport: %s", dlerror());
		return 1;
	}
	say("oslogfwd: LoggingSupport loaded");

	for (i = 0; i < (int)(sizeof(class_names) / sizeof(class_names[0])); i++) {
		Class cls = NSClassFromString([NSString stringWithUTF8String:class_names[i]]);
		char yes[600] = "";

		if (!cls) {
			say("oslogfwd: class %-22s NO", class_names[i]);
			continue;
		}
		for (j = 0; j < (int)(sizeof(selectors) / sizeof(selectors[0])); j++) {
			SEL sel = sel_getUid(selectors[j]);
			if ([cls instancesRespondToSelector:sel]) {
				size_t used = strlen(yes);
				snprintf(yes + used, sizeof(yes) - used, " -%s", selectors[j]);
			}
		}
		say("oslogfwd: class %-22s %s", class_names[i], yes[0] ? yes + 1 : "(no instance methods of interest)");
		for (j = 0; j < (int)(sizeof(selectors) / sizeof(selectors[0])); j++) {
			SEL sel = sel_getUid(selectors[j]);
			if ([cls respondsToSelector:sel]) {
				say("oslogfwd:   +%s", selectors[j]);
			}
		}
	}

	if (classes_mode) {
		static const char *const dump_names[] = {
			"OSLogEventProxy", "OSLogEventDecomposedMessage",
			"OSLogEventMessageArgument", "OSLogEventSegment",
			"OSLogEventStream", NULL
		};

		/* X3: the public field list says nothing about how to get a format string
		 * when the catalog is missing.  What this system's own classes offer is the
		 * only honest answer, and it is a method list, not a guess. */
		for (i = 0; dump_names[i]; i++) {
			dump_class(dump_names[i]);
		}
	}

	predicate = make_predicate();
	stream = make_stream_with_predicate(predicate, &why);
	say("oslogfwd: create stream: %s%s%s", stream ? "ok, " : "failed",
	    stream ? [backend_name UTF8String] : "",
	    stream ? "" : [why UTF8String] ?: "");
	if (stream) {
		why = nil;
		say("oslogfwd: attach and activate: %s%s%s",
		    attach_and_activate(stream, predicate, &why) ? "ok" : "failed",
		    why ? " - " : "", why ? [why UTF8String] : "");
		/* X2: activation that returns is not the same as a stream that delivers;
		 * what the next log needs is the count of handler calls, not just "ok". */
		if (store_secs < 5) {
			store_secs = 5;
		}
		{
			double until = now_sec() + 5.0;
			while (!stop_now && now_sec() < until) {
				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
			}
		}
		say("oslogfwd: selftest: %llu events, %llu with a subsystem, %llu printed, %llu dropped%s",
		    n_events, n_seen, n_printed, n_dropped,
		    stream_invalidated ? ", the stream was invalidated" : "");
	}
	archive_stats();
	{
		Class store_class = NSClassFromString(@"OSLogEventStore");
		static const char *const store_sels[] = {
			"localStore", "localStoreWithError:", "stream", "streamWithPredicate:",
			"streamWithFilterPredicate:error:", "storeWithArchiveURL:", NULL
		};
		char yes[400] = "";

		if (!store_class) {
			say("oslogfwd: store class OSLogEventStore NO");
		} else {
			for (j = 0; store_sels[j]; j++) {
				SEL sel = sel_getUid(store_sels[j]);
				if ([store_class respondsToSelector:sel]) {
					size_t used = strlen(yes);
					snprintf(yes + used, sizeof(yes) - used, " +%s", store_sels[j]);
				}
			}
			say("oslogfwd: store class OSLogEventStore%s", yes[0] ? yes : " (no class methods of interest)");
		}
		say("oslogfwd: archive probe: %s", store_poll() == 0 ? "ran" : "failed");
	}
	say("oslogfwd: selftest done");
	return stream ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static void
on_signal(int sig)
{
	(void)sig;
	stop_now = 1;
}

static void
usage(void)
{
	say("usage: oslogfwd [--selftest] [--all] [--subsystem SPEC]... [--process NAME]...");
	say("                [--rate N] [--burst N] [--console PATH] [--probe N]");
	say("                [--pred] [--store] [--no-store] [--store-secs N]");
	say("boot-args: oslog=sub[,sub:category]... oslog_proc=a,b oslog_rate=N oslog_burst=N");
	say("           oslog_pred=1 oslog_store=0|1 oslog_store_secs=N oslog_store_wait=N");
}

int
main(int argc, char **argv)
{
	int selftest_only = 0;
	int backoff = 10;
	int i;
	struct sigaction sa;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--selftest") == 0) {
			selftest_only = 1;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		}
	}

	setvbuf(stdout, NULL, _IONBF, 0);
	console_fd = open("/dev/console", O_WRONLY | O_NONBLOCK | O_NOCTTY);
	load_config(argc, argv);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (selftest_only) {
		return selftest();
	}

	say("oslogfwd: started (%s), pid %d, uid %d euid %d gid %d, console %s", OSLFWD_VERSION,
	    getpid(), getuid(), geteuid(), getgid(),
	    console_fd >= 0 ? "/dev/console" : "stderr");
	/* X2: diagnosticd lets a client read the stream if it is root ("admin status") or
	 * carries com.apple.private.logging.stream; a job in the user/501 launchd domain is
	 * neither, and the run log has to say which one this boot is. */
	say("oslogfwd: filter %s; live stream enabled; archive fallback %s (%d s)",
	    dump_mode ? "none, the dump takes every event"
	              : (send_predicate ? "subsystem predicate sent first, dropped after 60 s without an event"
	                                : "checked here only, the predicate is not sent to diagnosticd"),
	    store_fallback ? "enabled" : "off", store_secs);
	print_rules();
	if (nprocs) {
		char buf[256] = "";
		int n = 0;
		for (i = 0; i < nprocs; i++) {
			n += snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "," : "", procs[i]);
		}
		say("oslogfwd: processes %s", buf);
	}
	/* X3: the mode is a boot-arg and the log has to say which one ran, or the next
	 * "0 printed" cannot be attributed to the reader or to the stream. */
	if (dump_mode) {
		char apps[320] = "";
		int n = 0;

		for (i = 0; i < napps && n < (int)sizeof(apps) - 32; i++) {
			n += snprintf(apps + n, sizeof(apps) - n, "%s%s", i ? "," : "", app_names[i]);
		}
		say("oslogfwd: dump %s: %llu lines / %llu bytes, %.1f lines a second (burst %d), "
		    "app/error reserve 100 lines/16 KB, bare sample %llu, apps %s",
		    dump_mode == 2 ? "errors only" : "every event", dump_lines_limit,
		    dump_bytes_limit, dump_rate, dump_burst, dump_bare_limit, apps);
	} else {
		say("oslogfwd: dump off: only the subsystems above are printed");
	}
	if (dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW) == NULL) {
		say("oslogfwd: no LoggingSupport: %s", dlerror());
		return 1;
	}

	while (!stop_now) {
		@autoreleasepool {
			NSPredicate *predicate = make_predicate();
			NSString *why = nil;
			id stream = make_stream_with_predicate(predicate, &why);
			unsigned long long last_dropped = n_dropped;
			unsigned long long live_at_start = live_events;
			unsigned long long store_at_start = store_printed;
			unsigned long long printed_at_start = n_printed;
			double stream_start;
			double next_archive = 20.0;    /* the archive exists only once logd is up */
			double next_store = (double)store_secs;
			int heartbeat = 0;

			if (!stream || !attach_and_activate(stream, predicate, &why)) {
				/* Every failure of every attempt would be six lines a minute on a
				 * console that is a character at a time: back off, and say the same
				 * thing only once. */
				say("oslogfwd: no live stream (%s); retry in %d s",
				    why ? [why UTF8String] : "unknown", backoff);
				sleep((unsigned)backoff);
				backoff = backoff < 60 ? backoff * 2 : 60;
				continue;
			}
			backoff = 10;
			stream_start = now_sec();
			say("oslogfwd: streaming, backend %s", [backend_name UTF8String]);
			while (!stop_now) {
				double elapsed;

				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:10.0]];
				elapsed = now_sec() - stream_start;
				if (elapsed >= next_archive) {
					archive_stats();
					next_archive = elapsed + 300.0;
				}
				/* X2: the handler's call count, not the printed count, is what
				 * says whether diagnosticd is feeding us at all. */
				if (live_events != live_at_start || store_printed != store_at_start) {
					say("oslogfwd: %llu events, %llu with a subsystem, %llu printed, %llu dropped by the rate limit; "
					    "dump %llu lines/%llu bytes, %llu with a message, %llu without (%llu shown bare), "
					    "%llu not printed",
					    live_events - live_at_start, n_seen, n_printed, n_dropped,
					    dump_lines_used, dump_bytes_used, dump_with_msg, dump_no_msg,
					    dump_bare_shown, dump_dropped_events);
					live_at_start = live_events;
					store_at_start = store_printed;
					heartbeat = 0;
				} else if (n_dropped != last_dropped) {
					say("oslogfwd: %llu dropped by the rate limit so far", n_dropped);
					last_dropped = n_dropped;
				}
				heartbeat++;
				if (heartbeat >= 6) {          /* a minute of ten-second slices */
					if (live_events == live_at_start) {
						say("oslogfwd: alive, the live stream has delivered nothing in %d min (%llu handler calls since the start)",
						    heartbeat / 6, live_events);
					} else {
						say("oslogfwd: alive, nothing matched in %d min (%llu handler calls, %llu seen)",
						    heartbeat / 6, live_events - live_at_start, n_seen);
					}
					heartbeat = 0;
				}
				if (stream_invalidated || ev_uint(stream, "invalidated")) {
					say("oslogfwd: stream invalidated, restarting");
					break;
				}
				/* X2: a predicate diagnosticd cannot translate looks exactly like a
				 * refusal -- a live stream that delivers nothing.  Give it a minute,
				 * then reconnect without it (and say so once). */
				if (send_predicate && live_events == 0 && elapsed >= 60.0) {
					send_predicate = 0;
					say("oslogfwd: nothing through the subsystem predicate in 60 s; reconnecting without it");
					break;
				}
				/* the fallback: the live stream has said nothing for store_secs
				 * and logd's archive is the only other reader of the same data;
				 * keep reading it, so entries written after the first read are
				 * printed too (the timestamps in handle_store_event dedup).
				 * X3: "live_events == 0" was the wrong trigger.  TEST14's stream
				 * delivered 1136 handler calls (so live_events was never 0) but not
				 * one event with a subsystem, and the archive was never read.  A
				 * stream that carries events but no subsystem is exactly the case
				 * the archive is for. */
				if (store_fallback && elapsed >= next_store &&
				    (n_seen == 0 || n_printed == printed_at_start)) {
					say("oslogfwd: the live stream has no subsystem (or nothing printed) after %.0f s; reading logd's archive",
					    elapsed);
					store_poll();
					next_store = elapsed + (double)store_secs;
					if (store_secs <= 0) {
						next_store = elapsed + 60.0;
					}
				}
			}
		}
	}
	say("oslogfwd: stopped");
	if (console_fd >= 0) {
		close(console_fd);
	}
	return 0;
}
