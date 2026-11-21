/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/vayu_scm.c (the 8796 line, same authors). Taken from hackios_Raphael
 * 13145c3066 (hakios_scm_lock, xnu/files/bsd/dev/hakios.adsp.c: the same one mutex around the same `scm()` in adsp,
 * mpss, adreno and wifi), APSL 2.0/MIT; no line of theirs is copied but the idea. xnu-12377 (K3): the lock is not
 * taken with interrupts off or during a panic, where lck_mtx_lock() cannot be called.
 */
/*
 * The one lock around every TrustZone call in this kernel -- see bsd/sys/vayu_scm.h for why.
 *
 * The call itself is the same SMC64/SMC32 sequence the callers had inline before: x0 = the
 * function word, x1 = the argument type, x2..x5 = the values, x6 = the session, x7 = 0; x0 = 1
 * and x6 = the session again for as long as TrustZone says the call was interrupted, up to 100
 * times.  What is new is only the lck_mtx around it.
 *
 * The lock is declared with LCK_MTX_DECLARE, so it is initialized at compile time and can be
 * taken from the first kernel C code that runs; there is no separate "the lock is up yet" step.
 */
#include <kern/debug.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/systm.h>
#include <sys/vayu_scm.h>

#define SCM_INTERRUPTED         VAYU_SCM_INTERRUPTED
#define SCM_RETRIES             100

static LCK_GRP_DECLARE(scm_lock_group, "vayu-scm");
static LCK_MTX_DECLARE(scm_lock, &scm_lock_group);      /* one TrustZone call at a time */

static bool scm_shared = true;          /* the shared lock is in use (boot-arg vayu_scm_lock=0 turns it off) */
static bool scm_settled;                /* the boot-arg has been read */
static bool scm_leaving;                /* the kernel is on its way back to the loader: never wait for the lock */

static int32_t
scm_ask(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth,
    uint64_t *answer)
{
	uint64_t session = 0;

	for (int i = 0; i < SCM_RETRIES; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = fourth;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			if (answer != NULL) {
				*answer = x1;
			}
			return (int32_t)x0;
		}
	}
	return (int32_t)SCM_INTERRUPTED;
}

/* (read once; two threads reading it at the same time write the same value) */
static void
scm_settings(void)
{
	uint32_t on = 1;

	if (scm_settled) {
		return;
	}
	if (PE_parse_boot_argn("vayu_scm_lock", &on, sizeof(on))) {
		scm_shared = on != 0;
	}
	scm_settled = true;
}

/* can this caller wait for the mutex? */
static bool
scm_may_wait(void)
{
	return scm_shared && !scm_leaving && ml_get_interrupts_enabled() && !panic_active();
}

int32_t
vayu_scm_call(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth,
    uint64_t *answer)
{
	int32_t result;

	scm_settings();
	if (scm_may_wait()) {
		lck_mtx_lock(&scm_lock);
		result = scm_ask(function, arguments, first, second, third, fourth, answer);
		lck_mtx_unlock(&scm_lock);
	} else {
		result = scm_ask(function, arguments, first, second, third, fourth, answer);
	}
	return result;
}

int32_t
vayu_scm_call_atomic(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t fourth, uint64_t *answer)
{
	return scm_ask(function, arguments, first, second, third, fourth, answer);
}

void
vayu_scm_quiescing(void)
{
	scm_leaving = true;
}

bool
vayu_scm_locked(void)
{
	scm_settings();
	return scm_shared && !scm_leaving;
}
