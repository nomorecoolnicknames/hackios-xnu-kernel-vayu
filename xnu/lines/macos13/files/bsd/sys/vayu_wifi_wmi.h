/*
 * W21b WMI (TLV) bring-up after HTC SETUP_COMPLETE_EX (boot-arg vayu_wifi_wmi=1,
 * only together with vayu_wifi_htc_connect).  SERVICE_READY parsing, WMI_INIT_CMD
 * and READY_EVENT parsing are ported from ios-flame phone/wifi/radio.c (commit
 * 745c4341, wmi_svc_ready / wmi_send_init / wmi_rx, lines 504-702 and 1921-1960),
 * itself a port of Linux ath10k wmi-tlv.c for WCN3990.
 *
 * MIT License
 *
 * Copyright (c) 2026 dipa-studio (ios-flame)
 * Copyright (c) 2026 The Redwood Project (W21 adaptation)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SYS_VAYU_WIFI_WMI_H_
#define _SYS_VAYU_WIFI_WMI_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#define VAYU_WMI_MSG_MAX        1024U   /* WMI_INIT is 4 + 32 + 180 + 4 + 20 * chunks */
#define VAYU_WMI_MEM_TOTAL_MAX  (4U << 20)

struct vayu_wmi_env {
	int (*log)(const char *format, ...) __printflike(1, 2);
	/* Zeroed, physically contiguous, below 4 GiB, SMMU-mapped (IOVA = PA); never freed. 0 = none. */
	uint64_t (*dma_alloc)(uint32_t size);
};

/* boot-arg vayu_wifi_wmi, only with htc_connect: 1 = W21b, 2 = W21c (shadow SR doorbell, 5 s READY, TX dump),
 * 3 = W21d (=2 + per-CE misc/host IS snapshots after INIT, CE4 src 256),
 * 4 = W21e (=3, INIT sent from the SERVICE_READY handler, flame order),
 * 5 = W21e (=3, INIT abi words = the firmware's from SERVICE_READY),
 * 6 = W21f (=3, CE5 receive buffers 2048 bytes as flame),
 * 8 = W21h (=6, CE5 dest ring filled to nentries-1 as ath10k) */
extern uint32_t vayu_wmi_mode;

void vayu_wmi_reset(const struct vayu_wmi_env *env);
/* One WMI message (HTC payload on the WMI endpoint, trailer already removed). */
void vayu_wmi_rx(const uint8_t *msg, uint32_t len);
bool vayu_wmi_service_ready_seen(void);
bool vayu_wmi_ready_seen(void);
/* W22: the MAC from READY_EVENT (6 bytes, zero before READY). */
const uint8_t *vayu_wmi_mac(void);
/* Build WMI_INIT_CMD (cmd id + TLVs) into buf; returns the length, 0 on error. */
uint32_t vayu_wmi_build_init(uint8_t *buf, uint32_t cap);
void vayu_wmi_summary(void);
/* W21d: host CE config for the pool layout (=3: CE4 src 256, else base). */
struct vayu_ce_cfg;
const struct vayu_ce_cfg *vayu_wmi_host_cfg(const struct vayu_ce_cfg *base);
/* W21h: per-CE receive post count wanted (count entries, def = flame RX_POST). */
void vayu_wmi_rx_post_want(uint32_t *want, uint32_t count, uint32_t def);

#endif /* _SYS_VAYU_WIFI_WMI_H_ */
