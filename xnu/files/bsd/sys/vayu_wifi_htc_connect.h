/*
 * W21 HTC CONNECT_SERVICE / SETUP_COMPLETE_EX after HTC READY (boot-arg
 * vayu_wifi_htc_connect=1).  The HTC message layouts and the connect flags are
 * ported from ios-flame phone/wifi/radio.c (commit 745c4341, htc_send /
 * htc_connect / htc_rx, lines 360-500), itself a port of Linux ath10k htc.c.
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

#ifndef _SYS_VAYU_WIFI_HTC_CONNECT_H_
#define _SYS_VAYU_WIFI_HTC_CONNECT_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#define VAYU_HTCC_SVC_WMI_CONTROL   0x0100U
#define VAYU_HTCC_SVC_HTT_DATA      0x0300U
/* WLAN_CFG pipe map (183808-run.log): svc 0x1 out pipe 0, 0x100 out 3 / in 2, 0x300 out 4 / in 1. */
#define VAYU_HTCC_CTRL_TX_CE        0U
#define VAYU_HTCC_WMI_TX_CE         3U
#define VAYU_HTCC_TX_MAX            2048U   /* per TX CE; the buffer holds two (ce0, ce3) */
#define VAYU_HTCC_HTT_TX_CE         4U      /* W22: HTT_DATA out pipe 4 */
#define VAYU_HTCC_HTT_TX_OFF        1024U   /* W22: CE4 messages in the upper half of the ce0 buffer */

/* What the CE layer provides (bsd/dev/vayu_wifi_ce.c): registers, rings, a TX buffer, RX polling. */
struct vayu_htcc_ops {
	uint32_t (*rd)(uint32_t ce_id, uint32_t offset);
	void (*wr)(uint32_t ce_id, uint32_t offset, uint32_t value);
	void (*shadow_wr)(uint32_t offset, uint32_t value);
	/* Host view of the CE source ring, its entry count and the SR_WR_IDX read at programming. */
	bool (*src_ring)(uint32_t ce_id, uint8_t **desc, uint32_t *nentries, uint32_t *wr0);
	uint8_t *tx_buf;                /* 2 x VAYU_HTCC_TX_MAX bytes, non-cacheable, SMMU-mapped */
	uint64_t tx_pa;
	/* Drain every host RX CE once, frames go to vayu_htcc_rx(); false = stop (abort/blocked). */
	bool (*poll)(void);
	void (*delay_ms)(uint32_t ms);
	/* W21b: zeroed contiguous DMA memory below 4 GiB, SMMU-mapped, never freed; 0 = none. */
	uint64_t (*dma_alloc)(uint32_t size);
	int (*log)(const char *format, ...) __printflike(1, 2);
};

/* boot-arg vayu_wifi_htc_connect: 0 off (W20), 1 = WMI 0x100 then HTT 0x300, 2 = HTT then WMI (flame order). */
extern uint32_t vayu_htcc_mode;
extern bool vayu_htcc_active;           /* true only while vayu_htcc_run() polls */

/* One received CE frame (any CE).  Only called while vayu_htcc_active. */
void vayu_htcc_rx(uint32_t ce_id, const uint8_t *frame, uint32_t len);

/* CONNECT x2, SETUP_COMPLETE_EX, 3 s RX dump.  0 = everything sent and both services connected. */
int vayu_htcc_run(const struct vayu_htcc_ops *ops);
/* W25n: the WMI endpoint's HTC credits, as last seen (HTT has none). */
uint32_t vayu_htcc_credits(void);

#endif /* _SYS_VAYU_WIFI_HTC_CONNECT_H_ */
