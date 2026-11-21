/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 *
 * Origin: what the 8796 line's drivers shared, each defined in one driver there (the iOS 16.5 line (tree 6a36d992)):
 * hakios_spmi_lock/unlock in bsd/dev/hakios.buttons.c:129-147, hakios_smmu_lock in bsd/dev/hakios.wifi.c:196-198,
 * vayu_rsc_tcs_lock/unlock in bsd/dev/vayu_pas.c:990-1068 (+ bsd/sys/vayu_pas.h:106-114), hakios_tl in
 * bsd/dev/hakios.input.c:40-59. Same authors; rewritten for xnu-12377 in one file with one owner (K3).
 */
/*
 * VAYU: the locks and the trace line that several vayu drivers share (bsd/dev/vayu_locks.c). On the 8796 line each
 * lived in the first driver that needed it and the others declared it extern; here they have one owner, so a driver
 * port (K5 WLED, K6, K7 buttons/haptics/QUP, K8 GPU, the radio and audio later) brings no copy of its own:
 *
 *   hakios_spmi_lock/unlock     the SPMI arbiter of the PMICs: one transfer at a time (two at once and it drops one).
 *                               A spin lock (preemption off): hold it only for one transfer, never sleep inside.
 *                               The prototype is the one sys/hakios.input.h (Scion's, S9 drivers) declares too.
 *   hakios_smmu_lock/unlock     the apps SMMU's stream match registers and context banks: finding a free one and taking
 *                               it. A mutex. 8796 callers wrote lck_mtx_lock(&hakios_smmu_lock) on a global object;
 *                               here it is a call (the object is private).
 *   vayu_rsc_tcs_lock/unlock    TCS 0 of the apps RSC (0x18220000): the one RPMh command slot every vayu driver writes
 *                               (PAS, cpufreq's bandwidth vote, QUP, Wi-Fi, BT rails, ADSP, MPSS). Waits up to 60 s,
 *                               saying who holds it after 1 s and every 5 s, then fails the request; with interrupts off,
 *                               during a panic or after vayu_rsc_tcs_quiescing() it is not taken (SHUTDOWN): the caller
 *                               goes on unlocked, as on the 8796 line at the end of a run. Only HELD needs an unlock.
 *                               A port of vayu_pas.h drops its copy of the enum and includes this header.
 *   hakios_tl(fmt, ...)         a driver's trace line straight into the loader's log (sys/vayu_loader.h), so a live
 *                               look at that log misses none: formatted on the stack (191 characters at most, longer is
 *                               cut), no lock but the loader log's bounded one, no allocation -- usable with interrupts
 *                               off and from a quiesce. The format's text carries its own "\n".
 *
 * None of this does anything by itself: no hardware is touched here. vayu27=1 vayu_locks_test=1 runs a self-test of
 * the locks before the root (no SMC, no SPMI, no RPMh).
 */
#ifndef _SYS_VAYU_LOCKS_H_
#define _SYS_VAYU_LOCKS_H_

#ifdef KERNEL
#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

void hakios_spmi_lock(void);
void hakios_spmi_unlock(void);

void hakios_smmu_lock(void);
bool hakios_smmu_try_lock(void);
void hakios_smmu_unlock(void);

enum vayu_rsc_tcs_result {
	VAYU_RSC_TCS_FAILED,     /* the wait ran out: leave the slot alone */
	VAYU_RSC_TCS_HELD,
	VAYU_RSC_TCS_SHUTDOWN,   /* not taken, and the caller goes on without it: the end of a run (or interrupts off) */
};
enum vayu_rsc_tcs_result vayu_rsc_tcs_lock(const char *who);
void vayu_rsc_tcs_unlock(void);
/* from here on vayu_rsc_tcs_lock() answers SHUTDOWN (a driver's stop/quiesce on the way back to the loader) */
void vayu_rsc_tcs_quiescing(void);

void hakios_tl(const char *format, ...) __printflike(1, 2);

__END_DECLS
#endif /* KERNEL */

#endif /* _SYS_VAYU_LOCKS_H_ */
