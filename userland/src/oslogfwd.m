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
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_RULES       32
#define MAX_PROCS       8
#define SUBSYS_LEN      160
#define CATEGORY_LEN    96
#define LINE_LEN        768
#define MSG_LEN         420
#define OSLFWD_VERSION  "x1"

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
static unsigned long long n_printed, n_dropped, n_seen;
static int probe_left = -1;   /* --probe N: stop after N printed lines */

static int console_fd = -1;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t stop_now;

static void
out_line(const char *line)
{
	size_t len = strlen(line);

	pthread_mutex_lock(&out_lock);
	if (console_fd >= 0) {
		ssize_t n = write(console_fd, line, len);
		/* A console that cannot take the line is not a reason to spin; the
		 * loader drains it at its own pace, so say nothing and move on. */
		(void)n;
	} else {
		(void)write(STDERR_FILENO, line, len);
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
		}
	}
	if (!have_subsystems) {
		parse_spec_list(default_spec);
	}
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
static unsigned long
ev_uint(id ev, const char *sel_name)
{
	SEL sel = sel_getUid(sel_name);
	unsigned long (*fn)(id, SEL);

	if (![ev respondsToSelector:sel]) {
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

static void
handle_event(id ev)
{
	static const char *type_names[] = { "default", "info", "debug", "error", "fault" };
	NSString *subsystem, *category, *process, *message;
	char sub[SUBSYS_LEN], cat[CATEGORY_LEN], proc[96], msg[MSG_LEN], clock[32];
	rule_t *rule;
	NSUInteger type = 0;
	long pid = 0;
	char line[LINE_LEN];
	int n;

	if (![ev respondsToSelector:@selector(subsystem)]) {
		return;
	}
	subsystem = ev_get(ev, "subsystem", nil);
	if (![subsystem isKindOfClass:[NSString class]] || [subsystem length] == 0) {
		return;         /* an activity or a signpost, not an os_log entry */
	}
	category = ev_get(ev, "category", nil);
	process = ev_get(ev, "process", nil);
	if (![process isKindOfClass:[NSString class]]) {
		process = @"?";
	}

	escape([subsystem UTF8String], sub, sizeof(sub));
	escape([category isKindOfClass:[NSString class]] ? [category UTF8String] : "", cat, sizeof(cat));
	escape([process UTF8String], proc, sizeof(proc));

	n_seen++;
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
	type = (NSUInteger)ev_uint(ev, "type");
	pid = (long)(int)ev_uint(ev, "processIdentifier");
	stamp(clock, sizeof(clock));
	if (type < sizeof(type_names) / sizeof(type_names[0])) {
		n = snprintf(line, sizeof(line), "oslog %s %s[%ld] %s%s%s %s: %s\n",
		    clock, proc, pid, sub, cat[0] ? ":" : "", cat,
		    type_names[type], msg);
	} else {
		n = snprintf(line, sizeof(line), "oslog %s %s[%ld] %s%s%s t=%lu: %s\n",
		    clock, proc, pid, sub, cat[0] ? ":" : "", cat, (unsigned long)type, msg);
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

static BOOL
attach_and_activate(id stream, NSPredicate *predicate, NSString **why)
{
	SEL set_stream_handler = sel_getUid("setStreamHandler:");
	SEL set_event_handler = sel_getUid("setEventHandler:");
	void (^handler)(id) = ^(id event) {
		@autoreleasepool {
			handle_event(event);
		}
	};

	if (!stream) {
		return NO;
	}
	@try {
		if ([stream respondsToSelector:@selector(setFilterPredicate:)] && predicate) {
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

	say("oslogfwd: selftest (%s), pid %d uid %d", OSLFWD_VERSION, getpid(), getuid());
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
	say("boot-args: oslog=sub[,sub:category]... oslog_proc=a,b oslog_rate=N oslog_burst=N");
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

	say("oslogfwd: started (%s), pid %d, console %s", OSLFWD_VERSION, getpid(),
	    console_fd >= 0 ? "/dev/console" : "stderr");
	print_rules();
	if (nprocs) {
		char buf[256] = "";
		int n = 0;
		for (i = 0; i < nprocs; i++) {
			n += snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "," : "", procs[i]);
		}
		say("oslogfwd: processes %s", buf);
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
			unsigned long long last_printed = n_printed;
			unsigned long long seen_at_start = n_seen;
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
			say("oslogfwd: streaming, backend %s", [backend_name UTF8String]);
			while (!stop_now) {
				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:60.0]];
				heartbeat++;
				if (n_dropped != last_dropped) {
					say("oslogfwd: %llu dropped by the rate limit so far", n_dropped);
					last_dropped = n_dropped;
				}
				if (heartbeat >= 5 && n_printed == last_printed) {
					if (n_seen == seen_at_start) {
						say("oslogfwd: alive, the stream has delivered nothing (%llu seen)", n_seen);
					} else {
						say("oslogfwd: alive, nothing matched in %d min (%llu seen)",
						    heartbeat, n_seen);
					}
					heartbeat = 0;
				}
				last_printed = n_printed;
				if (ev_uint(stream, "invalidated")) {
					say("oslogfwd: stream invalidated, restarting");
					break;
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
