/*
 * One TrustZone call at a time: the single way this kernel does `smc #0` (bsd/dev/vayu_scm.c).
 *
 * TrustZone is one monitor behind all of the processors, and the SIP calls that start and stop
 * them (PAS: INIT_IMAGE, MEM_SETUP, AUTH_AND_RESET, SHUTDOWN) and that give memory to a
 * subsystem (MEM_PROT_ASSIGN) share its state.  Two of them at once -- the GPU's zap shader and
 * the audio DSP's image, say, from two threads -- is not something the vendor's kernel does:
 * every qcom_scm call in Linux goes through one mutex (drivers/firmware/qcom_scm.c,
 * qcom_scm_lock).  The same is done here, after hackios_Raphael's commit 13145c3066
 * ("hakios_scm_lock", xnu/files/bsd/dev/hakios.adsp.c and hakios.mpss.c, APSL 2.0/MIT).
 *
 * Not every caller can wait for a mutex: the panic path runs with the other processors stopped,
 * and a lock one of them held never comes back.  Those callers use vayu_scm_call_atomic(), or
 * come in after vayu_scm_quiescing() has said that the kernel is on its way back to the loader.
 */
#ifndef _SYS_VAYU_SCM_H_
#define _SYS_VAYU_SCM_H_

#ifdef KERNEL
#include <stdbool.h>
#include <stdint.h>

/* TrustZone's answer while the call is still going: ask again with x0 = 1 and x6 = session. */
#define VAYU_SCM_INTERRUPTED    1

/*
 * One call.  function is the whole word the secure monitor is given in x0 -- the convention
 * (SCM_64, SCM_FAST) and SCM_SIP already in it, as each caller composes it -- and the four
 * values go in x2..x5 (the SIP calls here use at most three; the modem's uses four).  On a
 * real answer *answer is x1 and the answer itself is returned; while TrustZone keeps saying
 * VAYU_SCM_INTERRUPTED the call is asked up to 100 times, and then VAYU_SCM_INTERRUPTED is
 * returned with *answer untouched (each caller maps that to what it returned before: -1, or
 * the 1 itself).
 *
 * It takes the shared lock unless the lock has been turned off (boot-arg vayu_scm_lock=0) or
 * vayu_scm_quiescing() has been said.  Hold it only for the call: the steps' pauses and the
 * logging of every caller stay outside.
 */
int32_t vayu_scm_call(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t fourth, uint64_t *answer);

/* The same call, never waiting for the lock: for a panic's way back to the loader, where a lock
 * held by a stopped processor would never be given back.  Only one caller at a time there. */
int32_t vayu_scm_call_atomic(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t fourth, uint64_t *answer);

/* From here on every vayu_scm_call() is vayu_scm_call_atomic(): set by each driver's quiesce. */
void vayu_scm_quiescing(void);

/* For the log: is the shared lock in use (boot-arg vayu_scm_lock is not 0, and not quiescing)? */
bool vayu_scm_locked(void);
#endif /* KERNEL */

#endif /* _SYS_VAYU_SCM_H_ */
