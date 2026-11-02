/*
 * W21: HTC CONNECT_SERVICE (WMI_CONTROL 0x100, HTT_DATA 0x300), SETUP_COMPLETE_EX
 * and a bounded 3 s dump of whatever arrives next (WMI_SERVICE_READY expected).
 * Runs once, after a real HTC READY on CE2, only with vayu_wifi_htc_connect != 0.
 *
 * Ported from ios-flame phone/wifi/radio.c (commit 745c4341): ce_send (one
 * source descriptor, transfer id in the descriptor flags, write index through
 * the shadow register), htc_send (8-byte HTC header on ep0), htc_connect (the
 * CONNECT message and its flags) and the CONNECT_RESP / SETUP_COMPLETE_EX part
 * of htc_rx.  Changes: synchronous (each TX waits for CUR_SRRI, each CONNECT
 * waits for its response), bounded, everything logged, nothing sent on WMI.
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

#include <stddef.h>
#include <sys/vayu_wifi_htc_connect.h>
#include <sys/vayu_wifi_wmi.h>
#include <sys/vayu_wifi_scan.h>
#include <sys/vayu_wifi_sta.h>

#define HTCC_SR_WR_IDX          0x3cU
#define HTCC_CUR_SRRI           0x44U
#define HTCC_SHADOW_SR          0x000U          /* + 4*ce, as flame SHADOW_PA + 4*c */
#define HTCC_DESC_SIZE          16U
#define HTCC_ADDR_MASK          0x00000007ffffffffULL
#define HTCC_HDR                8U
#define HTCC_MSG_CONNECT        2U
#define HTCC_MSG_CONNECT_RESP   3U
#define HTCC_MSG_SETUP_COMPLETE_EX 5U
#define HTCC_CONN_RECV_ALLOC    0x0100U         /* flame: WMI flags 1 << 8 */
#define HTCC_CONN_NO_CREDIT_FLOW 0x0008U        /* flame: HTT flags */
#define HTCC_TX_WAIT_MS         200U
#define HTCC_RESP_WAIT_MS       1000U
#define HTCC_DUMP_MS            3000U
#define HTCC_LOG_RX_MAX         48U
#define HTCC_READY_WAIT_MS      5000U   /* W21c (wmi=2): READY wait */
#define HTCC_WMI_SERVICE_READY  0x1U
/* W21d (wmi=3): wcn3990 per-CE status, ath10k hw.c wcn3990_wm_reg/misc_reg, ce.h:376 */
#define HTCC_HOST_IS            0x30U   /* cc 0x1, watermarks 0x1e */
#define HTCC_MISC_IS            0x38U   /* err_mask 0x3e0 */
#define HTCC_DR_WR_IDX          0x40U
#define HTCC_CUR_DRRI           0x48U
#define HTCC_WRAPPER            12U     /* rd(12, 0) = CE base + 0xc000, INTERRUPT_SUMMARY */
#define HTCC_CE_COUNT           12U

uint32_t vayu_htcc_mode;
bool vayu_htcc_active;

static const struct vayu_htcc_ops *htcc_ops;
static struct {
	bool tx_init[3];
	uint32_t tx_wr[3];
	uint8_t seq, wmi_seq, wmi_credits, htt_seq;
	uint32_t credit_reports;
	/* last CONNECT_RESP */
	bool resp_seen;
	uint16_t resp_svc;
	uint8_t resp_status, resp_eid;
	uint16_t resp_max;
	uint8_t wmi_eid, htt_eid;
	uint32_t rx_frames, rx_logged, rx_wmi, svc_ready;
	bool init_sent;
	uint32_t init_len;
	int init_rc;
} htcc;

static int htcc_send_init(void);

uint32_t
vayu_htcc_credits(void)
{
	return htcc.wmi_credits;
}

static uint16_t
htcc_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static void
htcc_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void
htcc_barrier(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
htcc_hex16(char *out, const uint8_t *p, uint32_t len)
{
	static const char digits[] = "0123456789abcdef";
	uint32_t i, n = len < 16 ? len : 16;

	for (i = 0; i < n; i++) {
		out[3 * i] = ' ';
		out[3 * i + 1] = digits[p[i] >> 4];
		out[3 * i + 2] = digits[p[i] & 0xf];
	}
	out[3 * n] = 0;
}

/* W21c: the whole TX frame (HTC header + payload), 32 bytes per line. */
static void
htcc_txdump(uint32_t ce_id, const uint8_t *p, uint32_t len)
{
	static const char digits[] = "0123456789abcdef";
	char line[3 * 32 + 1];
	uint32_t o, i, n;

	for (o = 0; o < len; o += 32) {
		n = len - o < 32 ? len - o : 32;
		for (i = 0; i < n; i++) {
			line[3 * i] = ' ';
			line[3 * i + 1] = digits[p[o + i] >> 4];
			line[3 * i + 2] = digits[p[o + i] & 0xf];
		}
		line[3 * n] = 0;
		htcc_ops->log("vayu-wifi: htc: tx ce%u +%03x:%s\n", ce_id, o, line);
	}
}

/*
 * flame ce_send(), made synchronous: one source descriptor (transfer id in the
 * descriptor flags, (id << 4) & 0xfff0 as flame), then the write index and a
 * bounded wait for CUR_SRRI to pass the slot.  The doorbell is the CE SR_WR_IDX
 * register, the same path that W20's RX doorbell (DR_WR_IDX) proved; if SRRI
 * has not moved after HTCC_TX_WAIT_MS the shadow SR index (flame's path) is
 * written once as well and the wait repeats.
 */
static int
htcc_ce_tx(uint32_t ce_id, const uint8_t *data, uint32_t len, uint32_t transfer_id)
{
	const struct vayu_htcc_ops *o = htcc_ops;
	uint8_t *ring, *d;
	uint32_t n = 0, wr0 = 0, mask, slot, next, srri, waited, i, pass;
	uint64_t a;
	uint16_t flags;

	/* W22: CE4 (HTT) uses the upper half of the CE0 buffer; ep0 messages are <= 24 bytes */
	uint32_t k = ce_id == VAYU_HTCC_CTRL_TX_CE ? 0 : ce_id == VAYU_HTCC_HTT_TX_CE ? 2 : 1;
	uint32_t boff = k == 2 ? VAYU_HTCC_HTT_TX_OFF : k * VAYU_HTCC_TX_MAX;
	uint8_t *buf = o->tx_buf + boff;

	if (len == 0 || len > (k == 2 ? VAYU_HTCC_TX_MAX - VAYU_HTCC_HTT_TX_OFF : VAYU_HTCC_TX_MAX) ||
	    o->tx_buf == NULL || (ce_id != VAYU_HTCC_CTRL_TX_CE && ce_id != VAYU_HTCC_WMI_TX_CE &&
	    !(ce_id == VAYU_HTCC_HTT_TX_CE && vayu_scan_mode != 0))) {
		o->log("vayu-wifi: htc: tx: bad length %u\n", len);
		return -1;
	}
	if (!o->src_ring(ce_id, &ring, &n, &wr0) || ring == NULL || n < 2 || (n & (n - 1)) != 0) {
		o->log("vayu-wifi: htc: tx: ce%u has no host source ring\n", ce_id);
		return -1;
	}
	mask = n - 1;
	htcc_barrier();
	slot = o->rd(ce_id, HTCC_SR_WR_IDX) & mask;
	srri = o->rd(ce_id, HTCC_CUR_SRRI) & mask;
	if (!htcc.tx_init[k]) {
		htcc.tx_init[k] = true;
		htcc.tx_wr[k] = slot;
		o->log("vayu-wifi: htc: tx: ce%u src ring %u entries, sr_wr %u (at programming %u) srri %u\n",
		    ce_id, n, slot, wr0, srri);
	}
	if (slot != htcc.tx_wr[k] || srri != slot) {
		o->log("vayu-wifi: htc: tx: ce%u not idle: sr_wr %u srri %u host wr %u; not sending\n",
		    ce_id, slot, srri, htcc.tx_wr[k]);
		return -1;
	}
	for (i = 0; i < len; i++) {
		buf[i] = data[i];
	}
	a = (o->tx_pa + boff) & HTCC_ADDR_MASK;
	flags = (uint16_t)((transfer_id << 4) & 0xfff0U);
	d = ring + (uint64_t)slot * HTCC_DESC_SIZE;
	for (i = 0; i < 8; i++) {
		d[i] = (uint8_t)(a >> (8 * i));
	}
	htcc_put16(d + 8, (uint16_t)len);
	htcc_put16(d + 10, flags);
	d[12] = d[13] = d[14] = d[15] = 0;
	htcc_barrier();
	next = (slot + 1) & mask;
	if (vayu_wmi_mode >= 2 && len > 64) {
		htcc_txdump(ce_id, buf, len);
	}
	/*
	 * W21c (vayu_wifi_wmi=2): WCN3990 has shadow_reg_support, so ath10k moves the
	 * source write index only through the shadow register (ce.c
	 * ath10k_ce_send_nolock: shadow_sr_wr_ind_addr, ce0 0x32000, ce3 0x3200c), and
	 * so does flame (radio.c ce_send).  W21/W21b wrote CE SR_WR_IDX directly.
	 */
	if (vayu_wmi_mode >= 2) {
		o->shadow_wr(HTCC_SHADOW_SR + 4 * ce_id, next);
	} else {
		o->wr(ce_id, HTCC_SR_WR_IDX, next);
	}
	for (pass = 0; pass < 2; pass++) {
		for (waited = 0; waited < HTCC_TX_WAIT_MS; waited++) {
			htcc_barrier();
			srri = o->rd(ce_id, HTCC_CUR_SRRI) & mask;
			if (srri == next) {
				htcc.tx_wr[k] = next;
				o->log("vayu-wifi: htc: tx: ce%u slot %u %u bytes id %u done after %u ms%s\n",
				    ce_id, slot, len, transfer_id, waited + pass * HTCC_TX_WAIT_MS,
				    pass ? (vayu_wmi_mode >= 2 ? " (after SR_WR_IDX write)" :
				    " (after shadow write)") : (vayu_wmi_mode >= 2 ? " (shadow doorbell)" : ""));
				return 0;
			}
			o->delay_ms(1);
		}
		if (pass == 0 && vayu_wmi_mode >= 2) {
			o->log("vayu-wifi: htc: tx: ce%u srri %u after %u ms (want %u); writing SR_WR_IDX %u\n",
			    ce_id, srri, HTCC_TX_WAIT_MS, next, next);
			o->wr(ce_id, HTCC_SR_WR_IDX, next);
		} else if (pass == 0) {
			o->log("vayu-wifi: htc: tx: ce%u srri %u after %u ms (want %u); writing shadow sr %u\n",
			    ce_id, srri, HTCC_TX_WAIT_MS, next, next);
			o->shadow_wr(HTCC_SHADOW_SR + 4 * ce_id, next);
		}
	}
	htcc.tx_wr[k] = next;      /* the descriptor is advertised: never reuse the slot */
	o->log("vayu-wifi: htc: tx: ce%u slot %u NOT completed: srri %u sr_wr %u\n",
	    ce_id, slot, srri, o->rd(ce_id, HTCC_SR_WR_IDX) & mask);
	return -1;
}

/* flame htc_send() for ep0: no credit flow on the control endpoint. */
static int
htcc_send_ep0(const uint8_t *payload, uint32_t len)
{
	uint8_t b[HTCC_HDR + 16];
	uint32_t i;

	if (len > sizeof(b) - HTCC_HDR) {
		return -1;
	}
	b[0] = 0;               /* eid 0 */
	b[1] = 0;               /* flags */
	htcc_put16(b + 2, (uint16_t)len);
	b[4] = 0;               /* trailer */
	b[5] = htcc.seq++;
	b[6] = b[7] = 0;
	for (i = 0; i < len; i++) {
		b[HTCC_HDR + i] = payload[i];
	}
	return htcc_ce_tx(VAYU_HTCC_CTRL_TX_CE, b, HTCC_HDR + len, 0);
}

/*
 * flame htc_send() on the WMI endpoint: credit flow (one credit at connect, more from
 * credit-report trailers), NEED_CREDIT_UPDATE, transfer id = eid, pipe 3 (WLAN_CFG 0x100 out).
 */
static int
htcc_send_wmi(const uint8_t *payload, uint32_t len)
{
	static uint8_t b[VAYU_HTCC_TX_MAX];
	uint32_t i;

	if (htcc.wmi_eid >= 8 || len > sizeof(b) - HTCC_HDR) {
		return -1;
	}
	if (htcc.wmi_credits == 0) {
		htcc_ops->log("vayu-wifi: htc: wmi eid %u has no credit; not sending\n", htcc.wmi_eid);
		return -1;
	}
	htcc.wmi_credits--;
	b[0] = htcc.wmi_eid;
	b[1] = 0x01;            /* NEED_CREDIT_UPDATE */
	htcc_put16(b + 2, (uint16_t)len);
	b[4] = 0;
	b[5] = htcc.wmi_seq++;
	b[6] = b[7] = 0;
	for (i = 0; i < len; i++) {
		b[HTCC_HDR + i] = payload[i];
	}
	htcc_ops->log("vayu-wifi: htc: wmi tx eid %u on ce%u %u bytes, credits left %u\n",
	    htcc.wmi_eid, VAYU_HTCC_WMI_TX_CE, len, htcc.wmi_credits);
	return htcc_ce_tx(VAYU_HTCC_WMI_TX_CE, b, HTCC_HDR + len, htcc.wmi_eid);
}

/* W22: flame htt_send(): HTT endpoint, no credit flow, CE4 (HTT_DATA out pipe 4). */
static int
htcc_send_htt(const uint8_t *payload, uint32_t len)
{
	uint8_t b[HTCC_HDR + 128];      /* W25: data TX messages are 20 + 52 bytes */
	uint32_t i;

	if (htcc.htt_eid >= 8 || len > sizeof(b) - HTCC_HDR) {
		return -1;
	}
	b[0] = htcc.htt_eid;
	b[1] = 0;
	htcc_put16(b + 2, (uint16_t)len);
	b[4] = 0;
	b[5] = htcc.htt_seq++;
	b[6] = b[7] = 0;
	for (i = 0; i < len; i++) {
		b[HTCC_HDR + i] = payload[i];
	}
	return htcc_ce_tx(VAYU_HTCC_HTT_TX_CE, b, HTCC_HDR + len, htcc.htt_eid);
}

/* flame htc_rx(): credit-report trailer records {id 1, len, (eid, credits)...}. */
static void
htcc_trailer(const uint8_t *t, uint32_t tlen)
{
	uint32_t o, i;

	for (o = 0; o + 2 <= tlen && o + 2 + t[o + 1] <= tlen; o += 2 + t[o + 1]) {
		if (t[o] != 1) {
			htcc_ops->log("vayu-wifi: htc: trailer record id %u len %u (ignored)\n", t[o], t[o + 1]);
			continue;
		}
		for (i = 0; i + 1 < t[o + 1]; i += 2) {
			uint8_t eid = t[o + 2 + i], n = t[o + 3 + i];

			htcc.credit_reports++;
			if (eid == htcc.wmi_eid && eid < 8) {
				htcc.wmi_credits = (uint8_t)(htcc.wmi_credits + n);
			}
			htcc_ops->log("vayu-wifi: htc: credit report eid %u +%u (wmi credits %u)\n", eid, n,
			    htcc.wmi_credits);
		}
	}
}

static bool htcc_stopped;       /* poll() said stop: the CE block may be down, no reads */

static bool
htcc_wait(bool (*done)(void), uint32_t ms)
{
	uint32_t waited;

	for (waited = 0; waited < ms; waited++) {
		if (!htcc_ops->poll()) {
			htcc_stopped = true;
			return false;
		}
		if (done != NULL && done()) {
			return true;
		}
		htcc_ops->delay_ms(1);
	}
	return done == NULL;
}

static bool
htcc_resp_done(void)
{
	return htcc.resp_seen;
}

/* flame htc_connect(): message id, service, flags (WMI 1 << 8, HTT NO_CREDIT_FLOW). */
static int
htcc_connect(uint16_t svc)
{
	uint8_t m[8] = { 0 };
	uint16_t flags = svc == VAYU_HTCC_SVC_WMI_CONTROL ? HTCC_CONN_RECV_ALLOC :
	    HTCC_CONN_NO_CREDIT_FLOW;

	htcc_put16(m, HTCC_MSG_CONNECT);
	htcc_put16(m + 2, svc);
	htcc_put16(m + 4, flags);
	htcc.resp_seen = false;
	htcc_ops->log("vayu-wifi: htc: CONNECT svc %#x flags %#x on ce%u\n", svc, flags,
	    VAYU_HTCC_CTRL_TX_CE);
	if (htcc_send_ep0(m, sizeof(m)) != 0) {
		htcc_ops->log("vayu-wifi: htc: CONNECT svc %#x: send failed\n", svc);
		return -1;
	}
	if (!htcc_wait(htcc_resp_done, HTCC_RESP_WAIT_MS)) {
		htcc_ops->log("vayu-wifi: htc: CONNECT svc %#x: no CONNECT_RESP in %u ms\n", svc,
		    HTCC_RESP_WAIT_MS);
		return -1;
	}
	if (htcc.resp_svc != svc || htcc.resp_status != 0 || htcc.resp_eid == 0 ||
	    htcc.resp_eid >= 8) {
		htcc_ops->log("vayu-wifi: htc: CONNECT svc %#x: refused (resp svc %#x status %u eid %u)\n",
		    svc, htcc.resp_svc, htcc.resp_status, htcc.resp_eid);
		return -1;
	}
	if (svc == VAYU_HTCC_SVC_WMI_CONTROL) {
		htcc.wmi_eid = htcc.resp_eid;
		htcc.wmi_credits = 1;   /* flame: credit_flow endpoint starts with one credit */
	} else {
		htcc.htt_eid = htcc.resp_eid;
	}
	return 0;
}

void
vayu_htcc_rx(uint32_t ce_id, const uint8_t *f, uint32_t len)
{
	const struct vayu_htcc_ops *o = htcc_ops;
	char hex[3 * 16 + 1];
	uint8_t eid, flags, trailer;
	uint16_t hlen, msg = 0xffff;
	uint32_t plen;
	const uint8_t *p;

	if (o == NULL || !vayu_htcc_active) {
		return;
	}
	htcc.rx_frames++;
	if (len < HTCC_HDR) {
		o->log("vayu-wifi: htc: rx ce%u short frame %u bytes\n", ce_id, len);
		return;
	}
	eid = f[0];
	flags = f[1];
	hlen = htcc_le16(f + 2);
	trailer = f[4];
	p = f + HTCC_HDR;
	plen = hlen < len - HTCC_HDR ? hlen : len - HTCC_HDR;
	if ((flags & 0x02U) != 0 && trailer <= plen) {
		plen -= trailer;
		htcc_trailer(p + plen, trailer);
	}
	if (plen >= 2) {
		msg = htcc_le16(p);
	}
	if ((flags & ~0xf3U) != 0) {
		o->log("vayu-wifi: htc: rx ce%u eid %u undefined-htc-flag-bits %#x (logged, accepted)\n",
		    ce_id, eid, flags);
	}
	if (htcc.rx_logged < HTCC_LOG_RX_MAX) {
		htcc.rx_logged++;
		htcc_hex16(hex, p, plen);
		o->log("vayu-wifi: htc: rx ce%u eid %u flags %#x len %u (frame %u, trailer %u):%s\n",
		    ce_id, eid, flags, hlen, len, trailer, hex);
	}
	if (eid == 0 && msg == HTCC_MSG_CONNECT_RESP && plen >= 8) {
		htcc.resp_svc = htcc_le16(p + 2);
		htcc.resp_status = p[4];
		htcc.resp_eid = p[5];
		htcc.resp_max = htcc_le16(p + 6);
		htcc.resp_seen = true;
		htcc_hex16(hex, p, plen);
		o->log("vayu-wifi: htc: CONNECT svc %#x -> eid %u status %u max_msg %u\n",
		    htcc.resp_svc, htcc.resp_eid, htcc.resp_status, htcc.resp_max);
		o->log("vayu-wifi: htc: CONNECT_RESP raw (%u bytes):%s\n", plen, hex);
		if (ce_id != 2) {
			o->log("vayu-wifi: htc: CONNECT_RESP on ce%u: not-ce2 (logged, accepted)\n", ce_id);
		}
		return;
	}
	if (eid != 0 && eid == htcc.wmi_eid) {
		htcc.rx_wmi++;
		if (vayu_wmi_mode != 0) {
			vayu_wmi_rx(p, plen);
		}
		if (plen >= 4 && (htcc_le16(p) | (uint32_t)p[2] << 16) == HTCC_WMI_SERVICE_READY) {
			htcc.svc_ready++;
			o->log("vayu-wifi: htc: WMI_SERVICE_READY on ce%u eid %u, %u bytes\n", ce_id, eid,
			    plen);
			/* W21e =4: flame radio.c wmi_rx: INIT straight from the SERVICE_READY handler */
			if (vayu_wmi_mode == 4 && !htcc.init_sent) {
				o->log("vayu-wifi: wmi: =4: INIT on SERVICE_READY (flame order)\n");
				(void)htcc_send_init();
			}
		}
	} else if (eid != 0 && eid == htcc.htt_eid && vayu_scan_mode != 0) {
		vayu_scan_htt_rx(p, plen);
		vayu_sta_htt_rx(p, plen);
	}
}

/*
 * W21d (vayu_wifi_wmi=3): per-CE misc IS / host IS / ring indices, so the
 * _CE_misc_handler assert after WMI_INIT can be tied to one CE and one error bit.
 * Never reads once poll() reported an abort (PFR may have collapsed the block).
 */
static void
htcc_ce_snapshot(const char *tag)
{
	const struct vayu_htcc_ops *o = htcc_ops;
	uint32_t c;

	if (vayu_wmi_mode < 3) {
		return;
	}
	if (htcc_stopped) {
		o->log("vayu-wifi: w21d: snap %s skipped: CE stage stopped\n", tag);
		return;
	}
	o->log("vayu-wifi: w21d: snap %s wrapper_summary %#x\n", tag, o->rd(HTCC_WRAPPER, 0));
	for (c = 0; c < HTCC_CE_COUNT; c++) {
		uint32_t misc = o->rd(c, HTCC_MISC_IS);

		o->log("vayu-wifi: w21d: snap %s ce%u misc_is %#x%s host_is %#x sr_wr %u srri %u "
		    "dr_wr %u drri %u\n", tag, c, misc, (misc & 0x3e0U) != 0 ? " ERR" : "",
		    o->rd(c, HTCC_HOST_IS), o->rd(c, HTCC_SR_WR_IDX), o->rd(c, HTCC_CUR_SRRI),
		    o->rd(c, HTCC_DR_WR_IDX), o->rd(c, HTCC_CUR_DRRI));
	}
}

static int
htcc_send_init(void)
{
	static uint8_t m[VAYU_WMI_MSG_MAX];
	uint32_t len;

	htcc.init_sent = true;
	len = vayu_wmi_build_init(m, sizeof(m));
	htcc.init_len = len;
	htcc.init_rc = (len == 0 || htcc_send_wmi(m, len) != 0) ? -1 : 0;
	return htcc.init_rc;
}

static int
htcc_wmi_init(void)
{
	uint32_t len;

	htcc_ops->log("vayu-wifi: wmi: vayu_wifi_wmi=%u: waiting %u ms for SERVICE_READY\n",
	    vayu_wmi_mode, HTCC_DUMP_MS);
	if (!htcc_wait(vayu_wmi_service_ready_seen, HTCC_DUMP_MS)) {
		htcc_ops->log("vayu-wifi: wmi: no SERVICE_READY in %u ms; INIT not sent\n", HTCC_DUMP_MS);
		return -1;
	}
	if (!htcc.init_sent) {
		/* let SERVICE_AVAILABLE / SERVICE_READY_EXT that follow land before INIT */
		(void)htcc_wait(NULL, 20);
		(void)htcc_send_init();
	}
	len = htcc.init_len;
	if (htcc.init_rc != 0) {
		htcc_ops->log("vayu-wifi: wmi: INIT FAILED (len %u)\n", len);
		return -1;
	}
	htcc_ops->log("vayu-wifi: wmi: INIT sent (%u bytes) on eid %u ce%u; waiting %u ms for READY\n",
	    len, htcc.wmi_eid, VAYU_HTCC_WMI_TX_CE, HTCC_DUMP_MS);
	if (vayu_wmi_mode >= 3) {
		static const uint32_t at[3] = { 50, 200, 1000 };
		static const char *const name[3] = { "init+50ms", "init+200ms", "init+1000ms" };
		uint32_t i, t = 0;

		htcc_ce_snapshot("init+0ms");
		for (i = 0; i < 3; i++) {
			if (htcc_wait(vayu_wmi_ready_seen, at[i] - t)) {
				htcc_ce_snapshot("ready");
				return 0;
			}
			t = at[i];
			htcc_ce_snapshot(name[i]);
			if (htcc_stopped) {
				htcc_ops->log("vayu-wifi: wmi: stopped %u ms after INIT\n", t);
				return -1;
			}
		}
		if (!htcc_wait(vayu_wmi_ready_seen, HTCC_READY_WAIT_MS - t)) {
			htcc_ops->log("vayu-wifi: wmi: no READY_EVENT in %u ms\n", HTCC_READY_WAIT_MS);
			return -1;
		}
		return 0;
	}
	if (!htcc_wait(vayu_wmi_ready_seen, vayu_wmi_mode >= 2 ? HTCC_READY_WAIT_MS : HTCC_DUMP_MS)) {
		htcc_ops->log("vayu-wifi: wmi: no READY_EVENT in %u ms\n",
		    vayu_wmi_mode >= 2 ? HTCC_READY_WAIT_MS : HTCC_DUMP_MS);
		return -1;
	}
	return 0;
}

/* W22 scan ops */
static int
htcc_scan_wait(bool (*done)(void), uint32_t ms)
{
	if (htcc_stopped) {
		return -1;
	}
	if (htcc_wait(done, ms)) {
		return 1;
	}
	return htcc_stopped ? -1 : 0;
}

static bool
htcc_have_credit(void)
{
	return htcc.wmi_credits != 0;
}

static int
htcc_scan_wmi_send(const uint8_t *msg, uint32_t len, uint32_t wait_ms)
{
	if (htcc.wmi_credits == 0 && !htcc_wait(htcc_have_credit, wait_ms)) {
		htcc_ops->log("vayu-wifi: w22: no WMI credit in %u ms (reports %u)\n", wait_ms,
		    htcc.credit_reports);
		return -1;
	}
	return htcc_send_wmi(msg, len);
}

static void
htcc_scan(void)
{
	static struct vayu_scan_ops sops;

	sops.log = htcc_ops->log;
	sops.wait = htcc_scan_wait;
	sops.wmi_send = htcc_scan_wmi_send;
	sops.htt_send = htcc_send_htt;
	sops.dma_alloc = htcc_ops->dma_alloc;
	sops.mac = vayu_wmi_mac();
	if (vayu_scan_run(&sops) == 0) {
		sops.log("vayu-wifi: W25 S1 scan: OK passive scan completed\n");
	} else {
		sops.log("vayu-wifi: W25 S1 scan: ERROR (scan did not complete; see the w22 lines)\n");
	}
	(void)vayu_sta_run(&sops);
}

int
vayu_htcc_run(const struct vayu_htcc_ops *ops)
{
	uint8_t m[12] = { 0 };
	uint16_t first, second;
	int rc = 0;

	if (ops == NULL || vayu_htcc_mode == 0) {
		return -1;
	}
	htcc_ops = ops;
	htcc.wmi_eid = 0xff;
	htcc.htt_eid = 0xff;
	vayu_htcc_active = true;
	if (vayu_wmi_mode != 0) {
		static struct vayu_wmi_env wenv;

		wenv.log = ops->log;
		wenv.dma_alloc = ops->dma_alloc;
		vayu_wmi_reset(&wenv);
		vayu_scan_reset(ops->log);
		vayu_sta_reset(ops->log);
	}
	first = vayu_htcc_mode == 2 ? VAYU_HTCC_SVC_HTT_DATA : VAYU_HTCC_SVC_WMI_CONTROL;
	second = vayu_htcc_mode == 2 ? VAYU_HTCC_SVC_WMI_CONTROL : VAYU_HTCC_SVC_HTT_DATA;
	ops->log("vayu-wifi: htc: connect stage (vayu_wifi_htc_connect=%u): svc %#x, then %#x, "
	    "SETUP_COMPLETE_EX, %u ms RX dump\n", vayu_htcc_mode, first, second, HTCC_DUMP_MS);
	if (htcc_connect(first) != 0 || htcc_connect(second) != 0) {
		rc = -1;
	} else {
		htcc_put16(m, HTCC_MSG_SETUP_COMPLETE_EX);
		if (htcc_send_ep0(m, sizeof(m)) != 0) {
			ops->log("vayu-wifi: htc: SETUP_COMPLETE_EX FAILED\n");
			rc = -1;
		} else {
			ops->log("vayu-wifi: htc: SETUP_COMPLETE_EX sent\n");
		}
	}
	if (rc == 0 && vayu_wmi_mode != 0) {
		rc = htcc_wmi_init();
		if (rc == 0 && vayu_scan_mode != 0) {
			htcc_scan();
		}
	}
	ops->log("vayu-wifi: htc: dumping RX for %u ms\n", HTCC_DUMP_MS);
	(void)htcc_wait(NULL, HTCC_DUMP_MS);
	if (vayu_wmi_mode != 0) {
		vayu_wmi_summary();
	}
	vayu_htcc_active = false;
	ops->log("vayu-wifi: htc: connect stage done: rc %d wmi eid %u htt eid %u, %u frames "
	    "(%u logged), %u on wmi eid, WMI_SERVICE_READY %u\n", rc, htcc.wmi_eid, htcc.htt_eid,
	    htcc.rx_frames, htcc.rx_logged, htcc.rx_wmi, htcc.svc_ready);
	return rc;
}
