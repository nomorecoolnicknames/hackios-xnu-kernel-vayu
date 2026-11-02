/*
 * W15-A: one bounded host copy-engine attempt, from WLAN_MODE MISSION to a
 * real incoming HTC_MSG_READY.  Kernel-facing interface; the wire format and
 * the parser live in vayu_wifi_ce_wire.h and vayu_wifi_htc.h (freestanding,
 * host-tested).
 *
 * Copyright (c) 2026 The Redwood Project.  APSL 2.0.
 */

#ifndef _SYS_VAYU_WIFI_CE_H_
#define _SYS_VAYU_WIFI_CE_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/vayu_wifi_htc.h>

enum vayu_ce_result {
	VAYU_CE_OFF = 0,        /* never attempted */
	VAYU_CE_BLOCKED,        /* a gate refused, or the attempt stopped on PFR/stop */
	VAYU_CE_TIMEOUT,        /* rings ran, no valid READY inside the cap */
	VAYU_CE_READY,          /* a real HTC_MSG_READY frame was parsed on CE2 */
};

/* Why the CE stage must stop.  PFR/CRASH mean the WLAN block may have been
 * collapsed by the modem restart, so no CE register may be touched afterwards. */
enum vayu_ce_abort {
	VAYU_CE_ABORT_NONE = 0,
	VAYU_CE_ABORT_STOP,     /* the driver is stopping */
	VAYU_CE_ABORT_PFR,      /* a pd-mapper PFR arrived */
	VAYU_CE_ABORT_CRASH,    /* the SMP2P wlan early-crash-ind rose */
};

/* Polled before every CE step and once per ms during the READY wait. */
typedef enum vayu_ce_abort (*vayu_ce_abort_fn)(void);

/* One attempt per boot.  qmi_ok must mean WLAN_CFG and WLAN_MODE both 0/0. */
enum vayu_ce_result vayu_wifi_ce_stage(bool qmi_ok, vayu_ce_abort_fn abort);

enum vayu_ce_result vayu_wifi_ce_result(void);
const char *vayu_wifi_ce_result_name(enum vayu_ce_result result);

/* Valid only when the result is VAYU_CE_READY. */
const struct vayu_htc_ready *vayu_wifi_ce_ready(void);

/* true once the first CE register was written: the pool is firmware-owned. */
bool vayu_wifi_ce_programmed(void);

/* Stop the rings if they were programmed; idempotent.  Never frees live DMA memory. */
void vayu_wifi_ce_teardown(void);

/* The caller saw a PFR/crash after the CE stage: the WLAN block may be collapsed,
 * so teardown must not touch a CE register. */
void vayu_wifi_ce_mark_block_down(void);

#endif /* _SYS_VAYU_WIFI_CE_H_ */
