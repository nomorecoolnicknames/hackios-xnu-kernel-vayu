/*
 * W22: passive Wi-Fi scan after WMI READY (boot-arg vayu_wifi_scan=1, only with
 * vayu_wifi_htc_connect and vayu_wifi_wmi).  Sequence and message layouts are
 * ported from ios-flame phone/wifi/radio.c (commit 745c4341, scan_start /
 * scan_send_start / scan_event / scan_mgmt_rx, lines 700-947, and htt_start /
 * htt_configure / htt_rx, lines 960-1120), itself a port of Linux ath10k
 * wmi-tlv.c / htt_tx.c / htt_rx.c for WCN3990.  REG_CHAN_LIST_CC layout as
 * ath11k wmi.h (wmi_reg_chan_list_cc_event, wmi_regulatory_rule_struct).
 *
 * MIT License
 *
 * Copyright (c) 2026 dipa-studio (ios-flame)
 * Copyright (c) 2026 The Redwood Project (W22 adaptation)
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

#ifndef _SYS_VAYU_WIFI_SCAN_H_
#define _SYS_VAYU_WIFI_SCAN_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#define VAYU_SCAN_WMI_EV_SCAN       0x3001U
#define VAYU_SCAN_WMI_EV_MGMT_RX    0x7001U

/* What the HTC layer (bsd/dev/vayu_wifi_htc_connect.c) provides. */
struct vayu_scan_ops {
	int (*log)(const char *format, ...) __printflike(1, 2);
	/* Poll RX until done() or ms elapse: 1 = done (or ms elapsed with done NULL), 0 = timeout, -1 = stopped. */
	int (*wait)(bool (*done)(void), uint32_t ms);
	/* One WMI message (cmd id + TLVs) on the WMI endpoint; waits up to wait_ms for an HTC credit. */
	int (*wmi_send)(const uint8_t *msg, uint32_t len, uint32_t wait_ms);
	/* One HTT message on the HTT endpoint (CE4, no credit flow). */
	int (*htt_send)(const uint8_t *msg, uint32_t len);
	/* Zeroed contiguous DMA memory below 4 GiB, SMMU-mapped (IOVA = PA), never freed; 0 = none. */
	uint64_t (*dma_alloc)(uint32_t size);
	const uint8_t *mac;             /* the MAC from WMI READY (6 bytes) */
	uint32_t tx_ring;               /* W25p: CE4 source ring entries (0: unknown) */
};

/* boot-arg vayu_wifi_scan: 0 off (W21h behaviour), 1 = HTT rx ring + passive scan after READY. */
extern uint32_t vayu_scan_mode;

/* Reads the boot-arg and clears the state; call before the first WMI event. */
void vayu_scan_reset(int (*log)(const char *format, ...) __printflike(1, 2));
/* One WMI event (payload after the 4-byte id).  Only called with vayu_scan_mode != 0. */
void vayu_scan_wmi_rx(uint32_t id, const uint8_t *p, uint32_t len);
/* One HTT T2H message on the HTT endpoint.  Only called with vayu_scan_mode != 0. */
void vayu_scan_htt_rx(const uint8_t *p, uint32_t len);
void vayu_scan_note_htt(const uint8_t *p, uint32_t len);
/* After WMI READY: HTT rx ring, channel list, vdev, passive scan, summary.  0 = scan completed. */
int vayu_scan_run(const struct vayu_scan_ops *ops);
/* W25: one native-wifi data frame on the HTT endpoint (clear = unencrypted, for EAPOL). 0 = sent. */
int vayu_scan_tx_frame(const uint8_t *frame, uint32_t len, bool clear);
/* W25p: the TX counters, one line: sent, completed, status, slots in flight/free, ring cap, queue,
 * drops, credits. */
void vayu_scan_tx_stats(uint32_t wmi_credits, uint32_t queued);

#endif /* _SYS_VAYU_WIFI_SCAN_H_ */
