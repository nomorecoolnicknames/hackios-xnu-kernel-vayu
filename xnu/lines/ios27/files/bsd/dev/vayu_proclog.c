/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU diagnostics, vayu27=1 vayu_exitlog=1 (default 0; 2: clean exits too): who kills whom, and how each process
 * ended -- for U3G1's backboardd, which dies 6 to 10 s in under QEMU with nothing said.
 *
 *   VAYU SIG: sender[pid] -> target[pid] SIGKILL (9) by psignal, reason RUNNINGBOARD (15) code 0x...
 *             every SIGKILL, SIGTERM, SIGABRT, SIGQUIT, SIGHUP, SIGINT, SIGSTOP, SIGSEGV, SIGBUS a process is sent
 *             (kern_sig.c psignal_internal: every path -- kill(2), terminate_with_reason(2), proc_terminate, the
 *             kernel's own -- comes through it, the sender being the process whose thread sends it)
 *   VAYU SIG: name[pid] sends it to itself: user 0: frame ...   (where, in its own code: the user backtrace,
 *             with the main image's header and the shared cache's slide to symbolicate it)
 *   VAYU EXIT: name[pid] signal 9 (SIGKILL), exit status 0; reason SIGNAL (2) code 0x9 flags 0x...: "desc";
 *             sent by name[pid]   (os_reason's namespace, code and description; for a signal, the sender the
 *             reason recorded)
 * At most 400 lines of each kind. Nothing is changed, only said.
 */
#include <kern/backtrace.h>
#include <machine/machine_routines.h>
#include <kern/kcdata.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/event_private.h>
#include <sys/reason.h>
#include <sys/signal.h>
#include <sys/systm.h>
#include <sys/wait.h>
#include <sys/vayu_12377_abi.h>

#pragma clang diagnostic error "-Wuninitialized"
#pragma clang diagnostic error "-Wsometimes-uninitialized"

extern int get_preemption_level(void);
extern void IOSleep(unsigned int milliseconds);           /* osfmk/arm/cpu_data.h */
extern uint64_t sweet_sample_slide(task_t task, uint64_t *mh);  /* osfmk/kern/exception.c: shared cache slide, header */

void vayu_sig_note(proc_t from, proc_t to, int sig, const char *how, uint64_t ns, uint64_t code);
void vayu_exit_note(proc_t p, int rv);
void vayu_memstat_note(proc_t p, const char *why, uint32_t pcontrol, uint32_t old_dirty);

extern uint32_t memorystatus_get_available_page_count(void);
extern uint32_t memorystatus_get_critical_page_shortage_threshold(void);
extern uint32_t memorystatus_get_idle_exit_page_shortage_threshold(void);
extern unsigned int memorystatus_level;
extern vm_pressure_level_t memorystatus_vm_pressure_level;  /* kern_memorystatus_notify.c */

static uint32_t vayu_seen_memstat, vayu_seen_jetsam, vayu_seen_signal, vayu_seen_other;

static int
vayu_exitlog(void)
{
	static int on = -1;

	if (on < 0) {
		uint32_t v = 0;

		(void)PE_parse_boot_argn("vayu_exitlog", &v, sizeof(v));
		on = vayu27_enabled() ? (int)(v > 2 ? 2 : v) : 0;
	}
	return on;
}

int vayu_exitlog_level(void);
int
vayu_exitlog_level(void)
{
	return vayu_exitlog();
}

static const char *
vayu_reason_name(uint32_t ns)
{
	static const char *const names[] = {
		"INVALID", "JETSAM", "SIGNAL", "CODESIGNING", "HANGTRACER", "TEST", "DYLD", "LIBXPC", "OBJC", "EXEC",
		"SPRINGBOARD", "TCC", "REPORTCRASH", "COREANIMATION", "AGGREGATED", "RUNNINGBOARD", "SKYWALK", "SETTINGS",
		"LIBSYSTEM", "FOUNDATION", "WATCHDOG", "METAL", "WATCHKIT", "GUARD", "ANALYTICS", "SANDBOX", "SECURITY",
		"ENDPOINTSECURITY", "PAC_EXCEPTION", "BLUETOOTH_CHIP", "PORT_SPACE", "WEBKIT", "BACKLIGHTSERVICES", "MEDIA",
		"ROSETTA", "LIBIGNITION", "BOOTMOUNT", "MTE_FAIL", "REALITYKIT", "AUDIO", "WAKEBOARD", "CORERC",
		"SELF_RESTRICT", "ARKIT", "CAMERA", "BACKBOARD", "POWEREXCEPTIONS", "SECINIT", "SECURITY_SOFT_TRAPS",
		"RESOURCES",
	};

	return ns < sizeof(names) / sizeof(names[0]) ? names[ns] : "?";
}

static const char *
vayu_sig_name(int sig)
{
	switch (sig) {
	case SIGHUP: return "SIGHUP";
	case SIGINT: return "SIGINT";
	case SIGQUIT: return "SIGQUIT";
	case SIGILL: return "SIGILL";
	case SIGTRAP: return "SIGTRAP";
	case SIGABRT: return "SIGABRT";
	case SIGBUS: return "SIGBUS";
	case SIGSEGV: return "SIGSEGV";
	case SIGKILL: return "SIGKILL";
	case SIGTERM: return "SIGTERM";
	case SIGSTOP: return "SIGSTOP";
	case SIGSYS: return "SIGSYS";
	case SIGPIPE: return "SIGPIPE";
	case SIGUSR1: return "SIGUSR1";
	case SIGUSR2: return "SIGUSR2";
	default: return "signal";
	}
}

/* a process's dirty-tracking bits, by name (sys/proc.h P_DIRTY_*) */
static void
vayu_dirty_names(char *out, size_t size, uint32_t d)
{
	static const char *const names[] = {
		"track", "allow-idle-exit", "defer", "dirty", "shutdown-hold", "terminated", "busy", "marked", "aging",
		"launching", "defer-always", "shutdown-on-clean", "new-here",
	};
	size_t at = 0;

	out[0] = '\0';
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((d & (1u << i)) && at + strlen(names[i]) + 2 < size) {
			at += (size_t)snprintf(out + at, size - at, "%s%s", at ? "," : "", names[i]);
		}
	}
	if (at == 0) {
		strlcpy(out, "-", size);
	}
}

/* kern_memorystatus.c, where it decides to kill or terminate (the proc list lock held: printing only) */
void
vayu_memstat_note(proc_t p, const char *why, uint32_t pcontrol, uint32_t old_dirty)
{
	static int left = 400;
	char was[160], now[160];
	proc_t by = current_proc();

	if (vayu_exitlog() == 0 || left <= 0) {
		return;
	}
	left--;
	vayu_seen_memstat++;
	vayu_dirty_names(was, sizeof(was), old_dirty);
	vayu_dirty_names(now, sizeof(now), p->p_memstat_dirty);
	printf("VAYU MEMSTAT: %s[%d] (called by %s[%d]): %s; pcontrol %#x, dirty %#x (%s) -> %#x (%s), priority %d "
	    "(requested %d), state %#x\n", p->p_comm, proc_getpid(p), by ? by->p_comm : "?", by ? proc_getpid(by) : -1, why,
	    pcontrol, old_dirty, was, p->p_memstat_dirty, now, p->p_memstat_effectivepriority,
	    p->p_memstat_requestedpriority, p->p_memstat_state);
}

void
vayu_sig_note(proc_t from, proc_t to, int sig, const char *how, uint64_t ns, uint64_t code)
{
	static int left = 400;
	char fname[MAXCOMLEN + 1] = "kernel", tname[MAXCOMLEN + 1] = "?";

	if (vayu_exitlog() == 0 || left <= 0 || strcmp(how, "kill") == 0) {
		return;                 /* (kill(2) is said again by psignal) */
	}
	if (sig != SIGKILL && sig != SIGTERM && sig != SIGABRT && sig != SIGQUIT && sig != SIGHUP && sig != SIGINT &&
	    sig != SIGSTOP && sig != SIGSEGV && sig != SIGBUS) {
		return;
	}
	left--;
	if (from != PROC_NULL) {
		strlcpy(fname, from->p_comm, sizeof(fname));
	}
	if (to != PROC_NULL) {
		strlcpy(tname, to->p_comm, sizeof(tname));
	}
	if (ns != 0 || code != 0) {
		printf("VAYU SIG: %s[%d] -> %s[%d] %s (%d) by %s, reason %s (%llu) code %#llx\n", fname,
		    from ? proc_getpid(from) : -1, tname, to ? proc_getpid(to) : -1, vayu_sig_name(sig), sig, how,
		    vayu_reason_name((uint32_t)ns), ns, code);
	} else {
		printf("VAYU SIG: %s[%d] -> %s[%d] %s (%d) by %s\n", fname, from ? proc_getpid(from) : -1, tname,
		    to ? proc_getpid(to) : -1, vayu_sig_name(sig), sig, how);
	}
	/* who in the kernel sent it: the return addresses, unslid (symbolicate against the kernel file: nm -n) */
	{
		uintptr_t kf[14];
		unsigned int kn = backtrace(kf, 14, NULL, NULL);
		char line[7 * 19 + 1], dirty[160];

		for (unsigned int first = 0; first < kn; first += 7) {
			size_t at = 0;

			line[0] = '\0';
			for (unsigned int i = first; i < first + 7 && i < kn; i++) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %lx",
				    (unsigned long)VM_KERNEL_UNSLIDE(kf[i]));
			}
			printf("VAYU SIG: %s[%d] kernel %u (unslid):%s\n", tname, to ? proc_getpid(to) : -1, first, line);
		}
		if (to != PROC_NULL) {
			vayu_dirty_names(dirty, sizeof(dirty), to->p_memstat_dirty);
			printf("VAYU SIG: %s[%d] dirty %#x (%s), priority %d (requested %d), memorystatus state %#x\n", tname,
			    proc_getpid(to), to->p_memstat_dirty, dirty, to->p_memstat_effectivepriority,
			    to->p_memstat_requestedpriority, to->p_memstat_state);
		}
	}
	/* sent to itself, from its own thread: where in its code */
	if (from != PROC_NULL && from == to && from != kernproc && current_proc() == from && ml_get_interrupts_enabled() &&
	    !ml_at_interrupt_context() && get_preemption_level() == 0) {
		uintptr_t frames[24];
		struct backtrace_user_info info = BTUINFO_INIT;
		unsigned int n = backtrace_user(frames, 24, NULL, &info);
		uint64_t mh = 0, slide = sweet_sample_slide(proc_task(from), &mh);

		printf("VAYU SIG: %s[%d] sends it to itself (main image header %#llx, shared cache slide %#llx), %u user frames%s\n",
		    fname, proc_getpid(from), mh, slide, n, info.btui_error != 0 ? " (the walk stopped on an error)" : "");
		for (unsigned int first = 0; first < n; first += 8) {
			char line[8 * 19 + 1];
			size_t at = 0;

			line[0] = '\0';
			for (unsigned int i = first; i < first + 8 && i < n; i++) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %lx", (unsigned long)frames[i]);
			}
			printf("VAYU SIG: %s[%d] user %u:%s\n", fname, proc_getpid(from), first, line);
		}
	}
}

/* the sender a signal's reason recorded (kern_sig.c build_signal_reason: KCDATA_TYPE_PID, KCDATA_TYPE_PROCNAME) */
static bool
vayu_reason_sender(os_reason_t r, int *pid, char *name, size_t size)
{
	bool found = false;

	if (r == OS_REASON_NULL || r->osr_kcd_buf == NULL || r->osr_bufsize == 0) {
		return false;
	}
	kcdata_iter_t it = kcdata_iter(r->osr_kcd_buf, r->osr_bufsize);
	if (!kcdata_iter_valid(it) || it.item->type != KCDATA_BUFFER_BEGIN_OS_REASON) {
		return false;
	}
	it = kcdata_iter_next(it);
	KCDATA_ITER_FOREACH(it) {
		uint32_t type = kcdata_iter_type(it), n = kcdata_iter_size(it);

		if (type == KCDATA_TYPE_PID && n >= sizeof(int32_t)) {
			*pid = *(int32_t *)kcdata_iter_payload(it);
			found = true;
		} else if (type == KCDATA_TYPE_PROCNAME && n != 0) {
			strlcpy(name, kcdata_iter_payload(it), MIN(size, (size_t)n + 1));
			found = true;
		}
	}
	return found;
}

void
vayu_exit_note(proc_t p, int rv)
{
	static int left = 400;
	os_reason_t r = p->p_exit_reason;
	char *desc;
	char sender[MAXCOMLEN + 1] = "?";
	int spid = -1;
	int on = vayu_exitlog();

	if (on == 0 || left <= 0) {
		return;
	}
	if (on < 2 && r == OS_REASON_NULL && !WIFSIGNALED(rv) && WEXITSTATUS(rv) == 0) {
		return;                 /* (a clean exit: vayu_exitlog=2 says those too) */
	}
	left--;
	if (r != OS_REASON_NULL && r->osr_namespace == OS_REASON_JETSAM) {
		vayu_seen_jetsam++;
	} else if (r != OS_REASON_NULL && r->osr_namespace == OS_REASON_SIGNAL) {
		vayu_seen_signal++;
	} else {
		vayu_seen_other++;
	}
	desc = exit_reason_get_string_desc(r);
	if (r == OS_REASON_NULL) {
		printf("VAYU EXIT: %s[%d] %s %d (%s), exit status %d; no reason\n", p->p_comm, proc_getpid(p),
		    WIFSIGNALED(rv) ? "signal" : "no signal", WIFSIGNALED(rv) ? WTERMSIG(rv) : 0,
		    WIFSIGNALED(rv) ? vayu_sig_name(WTERMSIG(rv)) : "-", WEXITSTATUS(rv));
		return;
	}
	char by[MAXCOMLEN + 32] = "";
	if (r->osr_namespace == OS_REASON_SIGNAL && vayu_reason_sender(r, &spid, sender, sizeof(sender))) {
		snprintf(by, sizeof(by), "; sent by %s[%d]", sender, spid);
	}
	printf("VAYU EXIT: %s[%d] %s %d (%s), exit status %d; reason %s (%u) code %#llx flags %#llx: \"%s\"%s\n",
	    p->p_comm, proc_getpid(p), WIFSIGNALED(rv) ? "signal" : "no signal", WIFSIGNALED(rv) ? WTERMSIG(rv) : 0,
	    WIFSIGNALED(rv) ? vayu_sig_name(WTERMSIG(rv)) : "-", WEXITSTATUS(rv), vayu_reason_name(r->osr_namespace),
	    r->osr_namespace, r->osr_code, r->osr_flags, desc != NULL ? desc : "", by);
}

/* vayu_exitlog=1: memorystatus as it is, every 30 s -- is the memory thought short? */
static void
vayu_memstat_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		uint32_t avail = memorystatus_get_available_page_count();

		printf("VAYU MEMSTAT: available %u pages (%u MB), critical below %u, idle-exit below %u, level %u%%, pressure %d; "
		    "seen: %u memorystatus kill/terminate decisions, %u jetsam exits, %u signal exits, %u other\n", avail,
		    (uint32_t)(((uint64_t)avail * PAGE_SIZE) >> 20), memorystatus_get_critical_page_shortage_threshold(),
		    memorystatus_get_idle_exit_page_shortage_threshold(), memorystatus_level, (int)memorystatus_vm_pressure_level,
		    vayu_seen_memstat, vayu_seen_jetsam, vayu_seen_signal, vayu_seen_other);
		IOSleep(30000);
	}
}

static void
vayu_proclog_start(void)
{
	thread_t thread;

	if (vayu_exitlog() != 0 && kernel_thread_start(vayu_memstat_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, proclog, vayu_proclog_start);
