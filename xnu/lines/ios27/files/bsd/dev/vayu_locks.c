/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/hakios.buttons.c:129-147 (SPMI), bsd/dev/hakios.wifi.c:196-198 (SMMU),
 * bsd/dev/vayu_pas.c:990-1068 (RSC TCS), bsd/dev/hakios.input.c:40-59 (hakios_tl); same authors, one file (K3).
 */
/*
 * VAYU: the locks and the trace line the vayu drivers share. What each is for: bsd/sys/vayu_locks.h.
 */
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_loader.h>
#include <sys/vayu_locks.h>

static LCK_GRP_DECLARE(vayu_locks_grp, "vayu-shared");

/* ---- SPMI ---- */

static LCK_SPIN_DECLARE(spmi_lock, &vayu_locks_grp);

void
hakios_spmi_lock(void)
{
	lck_spin_lock(&spmi_lock);
}

void
hakios_spmi_unlock(void)
{
	lck_spin_unlock(&spmi_lock);
}

/* ---- SMMU ---- */

static LCK_MTX_DECLARE(smmu_lock, &vayu_locks_grp);

void
hakios_smmu_lock(void)
{
	lck_mtx_lock(&smmu_lock);
}

bool
hakios_smmu_try_lock(void)
{
	return lck_mtx_try_lock(&smmu_lock);
}

void
hakios_smmu_unlock(void)
{
	lck_mtx_unlock(&smmu_lock);
}

/* ---- hakios_tl ---- */

static uint32_t tl_lines, tl_cut;

void
hakios_tl(const char *format, ...)
{
	char line[192];
	va_list list;
	int n;

	va_start(list, format);
	n = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	os_atomic_inc(&tl_lines, relaxed);
	if (n >= (int)sizeof(line)) {
		os_atomic_inc(&tl_cut, relaxed);
		if (format[strlen(format) - 1] == '\n') {
			line[sizeof(line) - 2] = '\n';  /* the cut line still ends its line */
		}
	}
	loader_log_line(line);
}

/* ---- RSC TCS 0 ---- */

#define RPMH_TCS_WARN_US        1000000ull      /* 1 s: say who holds the slot */
#define RPMH_TCS_REPORT_US      5000000ull      /* then every 5 s */
#define RPMH_TCS_GIVE_UP_US     60000000ull     /* 60 s: fail the request rather than wedge the thread */
#define RPMH_TCS_POLL_US        200             /* between two tries: nothing is held while waiting */

static LCK_MTX_DECLARE(rsc_tcs_lock, &vayu_locks_grp);
static const char *volatile rsc_tcs_holder;
static volatile uint64_t rsc_tcs_held_since;
static bool rsc_tcs_leaving;
static uint32_t rsc_tcs_unlocked;               /* SHUTDOWN answers given */

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

void
vayu_rsc_tcs_quiescing(void)
{
	rsc_tcs_leaving = true;
}

enum vayu_rsc_tcs_result
vayu_rsc_tcs_lock(const char *who)
{
	uint64_t began, waited, next;

	if (rsc_tcs_leaving || !ml_get_interrupts_enabled() || panic_active()) {
		/*
		 * No mutex can be taken (interrupts off, a panic: a holder may be a stopped processor) or the run is ending:
		 * the request goes on without the lock, as it always did on the 8796 line -- the regulators and DSPs given
		 * back at the end of a run matter more than the wait this lock is for. Said, the first times, in the loader's
		 * log only (no printf here).
		 */
		if (os_atomic_inc_orig(&rsc_tcs_unlocked, relaxed) < 8) {
			hakios_tl("rpmh: %s: TCS taken without its lock (interrupts off, a panic, or the end of the run)\n",
			    who != NULL ? who : "?");
		}
		return VAYU_RSC_TCS_SHUTDOWN;
	}
	if (!lck_mtx_try_lock(&rsc_tcs_lock)) {
		began = now_us();
		next = RPMH_TCS_WARN_US;
		for (;;) {
			const char *holder = rsc_tcs_holder;
			uint64_t since = rsc_tcs_held_since;

			if (lck_mtx_try_lock(&rsc_tcs_lock)) {
				break;
			}
			waited = now_us() - began;
			if (waited >= next) {
				if (holder != NULL) {
					printf("rpmh: TCS 0: %s has waited %llu ms; %s has held it for %llu ms\n",
					    who != NULL ? who : "?", waited / 1000, holder, (now_us() - since) / 1000);
				} else {
					printf("rpmh: TCS 0: %s has waited %llu ms; the holder was not recorded\n",
					    who != NULL ? who : "?", waited / 1000);
				}
				next = waited + RPMH_TCS_REPORT_US;
			}
			if (waited >= RPMH_TCS_GIVE_UP_US) {
				printf("rpmh: TCS 0: %s gives up after %llu ms; this request is not made\n",
				    who != NULL ? who : "?", waited / 1000);
				return VAYU_RSC_TCS_FAILED;
			}
			delay_for_interval(RPMH_TCS_POLL_US, NSEC_PER_USEC);     /* (no lock is held while waiting) */
		}
	}
	rsc_tcs_holder = who;
	rsc_tcs_held_since = now_us();
	return VAYU_RSC_TCS_HELD;
}

void
vayu_rsc_tcs_unlock(void)
{
	rsc_tcs_holder = NULL;
	lck_mtx_unlock(&rsc_tcs_lock);
}

/* ---- vayu27=1 vayu_locks_test=1: a self-test before the root (no SMC, SPMI or RPMh is touched) ---- */

static volatile int locks_test_state;           /* 1: the helper holds both mutexes; 2: it has given them back */

static void
locks_test_holder(__unused void *arg, __unused wait_result_t wr)
{
	enum vayu_rsc_tcs_result held;

	hakios_smmu_lock();
	held = vayu_rsc_tcs_lock("locks-test-holder");
	locks_test_state = held == VAYU_RSC_TCS_HELD ? 1 : -1;
	delay_for_interval(50, NSEC_PER_MSEC);
	if (held == VAYU_RSC_TCS_HELD) {
		vayu_rsc_tcs_unlock();
	}
	hakios_smmu_unlock();
	locks_test_state = 2;
	/* (returning ends the thread: Call_continuation) */
}

static void
vayu_locks_test(void)
{
	thread_t thread;
	enum vayu_rsc_tcs_result r;
	uint64_t t0, waited_tcs, waited_smmu;
	bool pass = true, spl;
	uint32_t lines0 = tl_lines;

	if (!vayu_arg("vayu_locks_test")) {
		return;
	}
	/* SPMI: a spin lock, taken and given back */
	hakios_spmi_lock();
	hakios_spmi_unlock();
	/* SMMU and TCS uncontended */
	if (!hakios_smmu_try_lock()) {
		pass = false;
	} else {
		hakios_smmu_unlock();
	}
	r = vayu_rsc_tcs_lock("locks-test");
	if (r != VAYU_RSC_TCS_HELD) {
		pass = false;
	} else {
		vayu_rsc_tcs_unlock();
	}
	/* contended: a helper thread holds both for 50 ms; this one must wait for each, then get it */
	locks_test_state = 0;
	if (kernel_thread_start(locks_test_holder, NULL, &thread) != KERN_SUCCESS) {
		printf("VAYU LOCKS: no helper thread: FAIL\n");
		return;
	}
	thread_deallocate(thread);
	for (int i = 0; i < 1000 && locks_test_state == 0; i++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	t0 = now_us();
	r = vayu_rsc_tcs_lock("locks-test");
	waited_tcs = now_us() - t0;
	if (r == VAYU_RSC_TCS_HELD) {
		vayu_rsc_tcs_unlock();
	}
	if (locks_test_state != 2 && locks_test_state != 1) {
		pass = false;
	}
	if (r != VAYU_RSC_TCS_HELD || waited_tcs < 20000) {
		pass = false;
	}
	t0 = now_us();
	hakios_smmu_lock();
	waited_smmu = now_us() - t0;
	hakios_smmu_unlock();
	for (int i = 0; i < 1000 && locks_test_state != 2; i++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (locks_test_state != 2) {
		pass = false;
	}
	/* interrupts off: never waits, answers SHUTDOWN */
	spl = ml_set_interrupts_enabled(FALSE);
	r = vayu_rsc_tcs_lock("locks-test-irqs-off");
	(void)ml_set_interrupts_enabled(spl);
	if (r != VAYU_RSC_TCS_SHUTDOWN) {
		pass = false;
	}
	hakios_tl("VAYU LOCKS: hakios_tl reaches the loader's log %s\n", "(line from the self-test)");
	hakios_tl("VAYU LOCKS: a long line is cut: %0180d|past the cut\n", 0);
	if (tl_lines < lines0 + 2 || tl_cut == 0) {
		pass = false;
	}
	printf("VAYU LOCKS: spmi ok, smmu waited %llu us, rsc tcs waited %llu us (holder 50 ms), irqs off: %s, "
	    "hakios_tl lines %u (cut %u): selftest %s\n", waited_smmu, waited_tcs,
	    r == VAYU_RSC_TCS_SHUTDOWN ? "not taken" : "TAKEN", tl_lines, tl_cut, pass ? "PASS" : "FAIL");
}

VAYU_HOOK(VAYU_PHASE_BEFORE_ROOT, locks, vayu_locks_test);
