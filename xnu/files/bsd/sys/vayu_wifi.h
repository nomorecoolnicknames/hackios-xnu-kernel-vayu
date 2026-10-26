/*
 * Wi-Fi's second step on the POCO X3 Pro (vayu, SM8150-AC): the two services of ours the modem's
 * firmware looks for before it starts its WLAN firmware (a pd-mapper -- SERVREG locator -- and a
 * read-only tftp server) and a look at WLFW, all in bsd/dev/vayu_wifi.c.
 */
#ifndef _SYS_VAYU_WIFI_H_
#define _SYS_VAYU_WIFI_H_

#ifdef KERNEL

#include <stdint.h>

struct vayu_qrtr;

/* After vayu_qrtr_attach(q) and GLINK up: starts what boot-arg vayu_wifi asks for. listen_s: how long the WLFW
 * probe waits for service 0x45. Returns at once (the WLFW probe runs in a thread of its own). */
void vayu_wifi_start(struct vayu_qrtr *q, uint32_t listen_s);
/* Before vayu_qrtr_detach(q): the WLFW thread ended (waits up to 5 s for it), our ports closed, a summary line. */
void vayu_wifi_stop(void);

#endif /* KERNEL */

#endif /* _SYS_VAYU_WIFI_H_ */
