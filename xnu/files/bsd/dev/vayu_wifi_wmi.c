/*
 * W21b: WMI (TLV) SERVICE_READY parse, WMI_INIT_CMD, READY_EVENT parse for the
 * WCN3990.  Ported from ios-flame phone/wifi/radio.c (commit 745c4341,
 * wmi_svc / put_tlv / wmi_svc_ready / wmi_send_init / wmi_rx), which follows
 * Linux ath10k wmi-tlv.c.  Layouts cross-checked against ath10k 4.9 wmi-tlv.h
 * (struct wmi_tlv_svc_rdy_ev, wmi_tlv_rdy_ev, wmi_tlv_resource_config: 44 u32,
 * wmi_tlv_init_cmd) and wmi.h (wlan_host_mem_req, NUM_UNITS_IS_*).
 * Changes: freestanding, bounded, every event hexdumped (first events only),
 * one contiguous host-memory allocation from the CE layer, MAC masked in logs.
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
#include <sys/vayu_wifi_wmi.h>
#include <sys/vayu_wifi_ce_wire.h>
#include <sys/vayu_wifi_scan.h>
#include <sys/vayu_wifi_sta.h>

#define WMI_EV_SERVICE_READY    0x1U
#define WMI_EV_READY            0x2U
#define WMI_EV_SERVICE_AVAILABLE 0x3U
#define WMI_CMD_INIT            0x1U
#define TAG_ARRAY_UINT32        0x10U
#define TAG_ARRAY_STRUCT        0x12U
#define TAG_SERVICE_READY       0x20U
#define TAG_HOST_MEM_REQ        0x22U
#define TAG_READY_EVENT         0x23U
#define TAG_INIT_CMD            0x4aU
#define TAG_RESOURCE_CONFIG     0x4bU
#define TAG_HOST_MEM_CHUNK      0x4cU
#define TAG_SERVICE_AVAILABLE   0x22fU
#define ABI_VER0                0x01000000U
#define ABI_VER1                53U
#define ABI_NS0                 0x5f414351U     /* "QCA_" */
#define ABI_NS1                 0x00004c4dU     /* "ML" */
#define SVC_RX_FULL_REORDER     65U
#define SVC_EXTEND_ADDRESS      179U
#define NUM_PEERS               33U
#define NUM_VDEVS               4U
#define MAX_MEM_REQS            8U
#define DUMP_EVENTS             8U
#define DUMP_BYTES              1024U

uint32_t vayu_wmi_mode;

struct wmi_mem_req {
	uint32_t req_id, unit_size, num_unit_info, num_units;
};

static const struct vayu_wmi_env *env;
static struct {
	uint32_t svc_map[32], svc_ext[4], nreq;
	struct wmi_mem_req req[MAX_MEM_REQS];
	bool seen_bmap, seen_reqs, svc_ready, ready, svc_avail;
	uint32_t events, dumped, other;
	uint32_t ready_status, ready_word9;
	uint32_t fw_abi[6];
	bool fw_abi_seen;
	uint8_t mac[6];
} wmi;

static uint32_t
rd16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t
rd32(const uint8_t *p)
{
	return rd16(p) | rd16(p + 2) << 16;
}

static uint8_t *
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
	return p + 4;
}

/* flame put_tlv(): le16 length, le16 tag. */
static uint8_t *
put_tlv(uint8_t *p, uint32_t tag, uint32_t len)
{
	p[0] = (uint8_t)len;
	p[1] = (uint8_t)(len >> 8);
	p[2] = (uint8_t)tag;
	p[3] = (uint8_t)(tag >> 8);
	return p + 4;
}

/* ath10k WMI_SERVICE_IS_ENABLED: 4 service bits per 32-bit word; ext bits from SERVICE_AVAILABLE. */
static bool
wmi_svc(uint32_t bit)
{
	if (bit < 128) {
		return (wmi.svc_map[bit / 4] & (1U << (bit % 4))) != 0;
	}
	bit -= 128;
	return bit < 128 && (wmi.svc_ext[bit / 32] & (1U << (bit % 32))) != 0;
}

static void
hexdump(uint32_t id, const uint8_t *p, uint32_t len)
{
	static const char digits[] = "0123456789abcdef";
	char line[3 * 32 + 1];
	uint32_t o, i, n, lim = len < DUMP_BYTES ? len : DUMP_BYTES;

	for (o = 0; o < lim; o += 32) {
		n = lim - o < 32 ? lim - o : 32;
		for (i = 0; i < n; i++) {
			line[3 * i] = ' ';
			line[3 * i + 1] = digits[p[o + i] >> 4];
			line[3 * i + 2] = digits[p[o + i] & 0xf];
		}
		line[3 * n] = 0;
		env->log("vayu-wifi: wmi: ev %#x +%03x:%s\n", id, o, line);
	}
	if (lim < len) {
		env->log("vayu-wifi: wmi: ev %#x dump cut at %u of %u bytes\n", id, lim, len);
	}
}

/* The TLVs of SERVICE_READY (flame wmi_svc_ready, ath10k_wmi_tlv_svc_rdy_parse). */
static void
svc_ready(const uint8_t *p, uint32_t len)
{
	uint32_t o, l, tag;

	for (o = 0; o + 4 <= len; o += 4 + l) {
		const uint8_t *v = p + o + 4;

		l = rd16(p + o);
		tag = rd16(p + o + 2);
		if (o + 4 + l > len) {
			env->log("vayu-wifi: wmi: SERVICE_READY tlv tag %#x len %u overruns %u\n", tag, l, len);
			break;
		}
		env->log("vayu-wifi: wmi: SERVICE_READY tlv @%u tag %#x len %u\n", o, tag, l);
		if (tag == TAG_SERVICE_READY && l >= 84) {
			/* wmi_tlv_svc_rdy_ev: fw_build, abi[6], phy_cap, max_frag, num_rf_chains, ... */
			env->log("vayu-wifi: wmi: fw_build %#x abi %#x/%u ns %#x %#x phy_cap %#x max_frag %u "
			    "rf_chains %u\n", rd32(v), rd32(v + 4), rd32(v + 8), rd32(v + 12), rd32(v + 16),
			    rd32(v + 28), rd32(v + 32), rd32(v + 36));
			env->log("vayu-wifi: wmi: ht_cap %#x vht_cap %#x vht_mcs %#x txpwr %u..%u sys_cap %#x "
			    "max_bcn_ie %u num_mem_reqs %u max_scan_chans %u hw_bd_id %u\n", rd32(v + 40),
			    rd32(v + 44), rd32(v + 48), rd32(v + 52), rd32(v + 56), rd32(v + 60),
			    rd32(v + 68), rd32(v + 72), rd32(v + 76), rd32(v + 80));
			for (uint32_t i = 0; i < 6; i++) {
				wmi.fw_abi[i] = rd32(v + 4 + 4 * i);
			}
			wmi.fw_abi_seen = true;
		} else if (tag == TAG_ARRAY_UINT32 && !wmi.seen_bmap) {
			uint32_t i;

			wmi.seen_bmap = true;
			for (i = 0; i < 32 && 4 * i + 4 <= l; i++) {
				wmi.svc_map[i] = rd32(v + 4 * i);
			}
			env->log("vayu-wifi: wmi: service bitmap %u words, rx_full_reorder %u\n", i,
			    wmi_svc(SVC_RX_FULL_REORDER) ? 1 : 0);
		} else if (tag == TAG_ARRAY_STRUCT && !wmi.seen_reqs) {
			uint32_t q, ql, qt;

			wmi.seen_reqs = true;
			for (q = 0; q + 4 <= l; q += 4 + ql) {
				ql = rd16(v + q);
				qt = rd16(v + q + 2);
				if (q + 4 + ql > l) {
					break;
				}
				if (qt == TAG_HOST_MEM_REQ && ql >= 16 && wmi.nreq < MAX_MEM_REQS) {
					struct wmi_mem_req *r = &wmi.req[wmi.nreq];

					r->req_id = rd32(v + q + 4);
					r->unit_size = rd32(v + q + 8);
					r->num_unit_info = rd32(v + q + 12);
					r->num_units = rd32(v + q + 16);
					env->log("vayu-wifi: wmi: mem_req %u: req_id %u unit_size %u num_unit_info %#x "
					    "num_units %u\n", wmi.nreq, r->req_id, r->unit_size, r->num_unit_info,
					    r->num_units);
					wmi.nreq++;
				}
			}
			env->log("vayu-wifi: wmi: %u mem_reqs\n", wmi.nreq);
		}
	}
}

static void
ready_event(const uint8_t *p, uint32_t len)
{
	uint32_t o, l, tag;

	for (o = 0; o + 4 <= len; o += 4 + l) {
		const uint8_t *v = p + o + 4;

		l = rd16(p + o);
		tag = rd16(p + o + 2);
		if (o + 4 + l > len) {
			break;
		}
		/* wmi_tlv_rdy_ev: abi[6] (24), mac_addr (6 + 2 pad), status; newer firmware appends words. */
		if (tag == TAG_READY_EVENT && l >= 24 + 8 + 4) {
			uint32_t i;

			for (i = 0; i < 6; i++) {
				wmi.mac[i] = v[24 + i];
			}
			wmi.ready_status = rd32(v + 32);
			wmi.ready_word9 = l >= 40 ? rd32(v + 36) : 0xffffffffU;
			wmi.ready = true;
		}
	}
	env->log("vayu-wifi: wmi: READY status %u num_phy %u mac xx:xx:xx:xx:%02x:%02x%s\n",
	    wmi.ready_status, wmi.ready_word9, wmi.mac[4], wmi.mac[5],
	    wmi.ready ? "" : " (no READY_EVENT tlv)");
}

void
vayu_wmi_reset(const struct vayu_wmi_env *e)
{
	uint8_t *b = (uint8_t *)&wmi;

	for (uint32_t i = 0; i < (uint32_t)sizeof(wmi); i++) {
		b[i] = 0;
	}
	env = e;
}

void
vayu_wmi_rx(const uint8_t *d, uint32_t len)
{
	uint32_t id;

	if (env == NULL) {
		return;
	}
	if (len < 4) {
		env->log("vayu-wifi: wmi: short message %u bytes\n", len);
		return;
	}
	id = rd32(d) & 0xffffffU;
	wmi.events++;
	vayu_sta_wmi_rx(id, d + 4, len - 4);
	if (vayu_scan_mode != 0) {
		vayu_scan_wmi_rx(id, d + 4, len - 4);
		if (id == VAYU_SCAN_WMI_EV_MGMT_RX || id == VAYU_SCAN_WMI_EV_SCAN) {
			wmi.other++;
			return;         /* W22: one line per frame from the scan code only */
		}
	}
	env->log("vayu-wifi: wmi: event %#x len %u\n", id, len);
	if (wmi.dumped < DUMP_EVENTS) {
		wmi.dumped++;
		hexdump(id, d, len);
	}
	switch (id) {
	case WMI_EV_SERVICE_AVAILABLE:
		/* flame: tag at +6, ext bitmap (4 words) at +12 */
		if (len >= 12 + 16 && rd16(d + 6) == TAG_SERVICE_AVAILABLE) {
			for (uint32_t i = 0; i < 4; i++) {
				wmi.svc_ext[i] = rd32(d + 12 + 4 * i);
			}
			wmi.svc_avail = true;
			env->log("vayu-wifi: wmi: SERVICE_AVAILABLE ext bitmap %08x %08x %08x %08x "
			    "(segment word %#x), extend_address %u\n", wmi.svc_ext[0], wmi.svc_ext[1],
			    wmi.svc_ext[2], wmi.svc_ext[3], rd32(d + 8), wmi_svc(SVC_EXTEND_ADDRESS) ? 1 : 0);
		}
		break;
	case WMI_EV_SERVICE_READY:
		svc_ready(d + 4, len - 4);
		wmi.svc_ready = true;
		break;
	case WMI_EV_READY:
		ready_event(d + 4, len - 4);
		break;
	default:
		wmi.other++;
		if (len >= 8) {
			env->log("vayu-wifi: wmi: event %#x first tlv tag %#x len %u (not parsed)\n", id,
			    rd16(d + 6), rd16(d + 4));
		}
		break;
	}
}

bool
vayu_wmi_service_ready_seen(void)
{
	return wmi.svc_ready;
}

const uint8_t *
vayu_wmi_mac(void)
{
	return wmi.mac;
}

bool
vayu_wmi_ready_seen(void)
{
	return wmi.ready;
}

/* flame wmi_send_init(): INIT_CMD tlv, resource config (ath10k's WCN3990 values), mem chunks. */
uint32_t
vayu_wmi_build_init(uint8_t *m, uint32_t cap)
{
	uint32_t total = 0, n = wmi.nreq, len[MAX_MEM_REQS], rc[44], i, off;
	uint64_t pa = 0;
	uint8_t *p = m;

	if (env == NULL || cap < 4 + 4 + 28 + 4 + sizeof(rc) + 4 + 20 * MAX_MEM_REQS) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		uint32_t units = wmi.req[i].num_units;

		if ((wmi.req[i].num_unit_info & 6U) != 0) {
			units = NUM_PEERS + 1;          /* NUM_UNITS_IS_NUM_PEERS / _ACTIVE_PEERS */
		} else if ((wmi.req[i].num_unit_info & 1U) != 0) {
			units = NUM_VDEVS + 1;          /* NUM_UNITS_IS_NUM_VDEVS */
		}
		len[i] = units * ((wmi.req[i].unit_size + 3U) & ~3U);
		total += (len[i] + 0xfffU) & ~0xfffU;
	}
	if (total != 0 && total <= VAYU_WMI_MEM_TOTAL_MAX && env->dma_alloc != NULL) {
		pa = env->dma_alloc(total);
	}
	if (total != 0 && (pa == 0 || pa + total > ((uint64_t)1 << 32))) {
		env->log("vayu-wifi: wmi: INIT without mem chunks (%u requests, %u bytes not given%s)\n",
		    n, total, pa != 0 ? ", above 4 GiB" : "");
		n = 0;
		pa = 0;
	}
	for (i = 0; i < cap; i++) {
		m[i] = 0;
	}
	p = put32(p, WMI_CMD_INIT);
	p = put_tlv(p, TAG_INIT_CMD, 24 + 4);
	{
		/* flame radio.c wmi_send_init: abi_ver0/1, ns0..ns3 = constants; W21e =5: the firmware's */
		uint32_t abi[6] = { ABI_VER0, ABI_VER1, ABI_NS0, ABI_NS1, 0, 0 };

		if (vayu_wmi_mode == 5 && wmi.fw_abi_seen) {
			env->log("vayu-wifi: wmi: =5: INIT abi %#x/%u ns %#x %#x %#x %#x -> fw %#x/%u ns %#x %#x "
			    "%#x %#x\n", abi[0], abi[1], abi[2], abi[3], abi[4], abi[5], wmi.fw_abi[0],
			    wmi.fw_abi[1], wmi.fw_abi[2], wmi.fw_abi[3], wmi.fw_abi[4], wmi.fw_abi[5]);
			for (i = 0; i < 6; i++) {
				abi[i] = wmi.fw_abi[i];
			}
		} else if (vayu_wmi_mode == 5) {
			env->log("vayu-wifi: wmi: =5: no fw abi from SERVICE_READY; INIT abi unchanged %u\n",
			    abi[1]);
		}
		for (i = 0; i < 6; i++) {
			p = put32(p, abi[i]);
		}
	}
	p = put32(p, n);
	for (i = 0; i < 44; i++) {
		rc[i] = 0;
	}
	rc[0] = NUM_VDEVS;
	rc[1] = NUM_PEERS;
	if (wmi_svc(SVC_RX_FULL_REORDER)) {
		rc[2] = NUM_VDEVS;
		rc[3] = NUM_VDEVS;
	}
	rc[4] = 2;                      /* peer keys */
	rc[5] = NUM_PEERS * 2;          /* tids */
	rc[6] = 16;                     /* ast skid limit */
	rc[7] = 0x7;                    /* tx chain mask */
	rc[8] = 0x7;                    /* rx chain mask */
	rc[9] = 0x64; rc[10] = 0x64; rc[11] = 0x64; rc[12] = 0x28;
	rc[13] = 1;                     /* rx decap: native wifi */
	rc[14] = 4;                     /* scan max pending */
	rc[15] = NUM_VDEVS;             /* bmiss offload */
	rc[16] = NUM_VDEVS;             /* roam offload */
	rc[17] = 8;                     /* roam ap profiles */
	rc[21] = 0x400;                 /* tx dbglog size */
	rc[22] = 2;                     /* wds entries */
	rc[27] = 2;                     /* gtk offload */
	rc[28] = 1024 + 32;             /* msdu descs */
	rc[29] = 2;                     /* frag entries */
	rc[30] = 1;                     /* tdls vdevs */
	rc[31] = 0x20;                  /* tdls conn table */
	rc[32] = 2;                     /* beacon tx offload */
	rc[33] = 5;                     /* mcast filter entries */
	rc[34] = 22;                    /* wow filters */
	rc[35] = 6;                     /* keepalive patterns */
	rc[37] = 1;
	rc[38] = 1;
	rc[43] = 1U << 9;               /* MGMT_BUNDLE_TX_COMPL */
	p = put_tlv(p, TAG_RESOURCE_CONFIG, sizeof(rc));
	for (i = 0; i < 44; i++) {
		p = put32(p, rc[i]);
	}
	p = put_tlv(p, TAG_ARRAY_STRUCT, n * 20);
	for (i = 0, off = 0; i < n; i++) {
		uint64_t cpa = pa + off;

		off += (len[i] + 0xfffU) & ~0xfffU;
		p = put_tlv(p, TAG_HOST_MEM_CHUNK, 16);
		p = put32(p, wmi.req[i].req_id);
		p = put32(p, (uint32_t)cpa);
		p = put32(p, len[i]);
		p = put32(p, wmi_svc(SVC_EXTEND_ADDRESS) ? (uint32_t)(cpa >> 32) : 0);
		env->log("vayu-wifi: wmi: chunk %u req_id %u pa %#llx len %u\n", i, wmi.req[i].req_id,
		    (unsigned long long)cpa, len[i]);
	}
	env->log("vayu-wifi: wmi: INIT built: %u chunks, %u bytes host memory, %u bytes message\n",
	    n, n != 0 ? total : 0, (uint32_t)(p - m));
	return (uint32_t)(p - m);
}

void
vayu_wmi_summary(void)
{
	if (env == NULL) {
		return;
	}
	env->log("vayu-wifi: wmi: summary: %u events (%u other), service_available %u service_ready %u "
	    "mem_reqs %u ready %u status %u\n", wmi.events, wmi.other, wmi.svc_avail ? 1 : 0,
	    wmi.svc_ready ? 1 : 0, wmi.nreq, wmi.ready ? 1 : 0, wmi.ready_status);
}

/*
 * W21d (vayu_wifi_wmi=3) candidate: CE4 (host->target HTT, DIS_INTR) source ring
 * 2048 -> 256 entries, as ath10k snoc.c host_ce_config_wlan CE4 .src_nentries = 256
 * and as our target config CE4 nentries 256 (vayu_wifi.c tgt cfg).  Other modes
 * get the table unchanged.  W21f (=6): =3 plus CE5 receive buffers 512 -> 2048
 * bytes, as flame radio.c RX_BUF_SIZE for all host RX CEs.
 */
const struct vayu_ce_cfg *
vayu_wmi_host_cfg(const struct vayu_ce_cfg *base)
{
	static struct vayu_ce_cfg cfg[VAYU_CE_COUNT];
	uint32_t i;

	if (vayu_wmi_mode < 3) {
		return base;
	}
	for (i = 0; i < VAYU_CE_COUNT; i++) {
		cfg[i] = base[i];
	}
	cfg[4].src_nentries = 256;
	if (vayu_wmi_mode == 6 || vayu_wmi_mode == 8) {
		/* flame radio.c:177,240: every host RX CE gets 2048-byte buffers (ce5 included). */
		cfg[5].src_sz_max = 2048;
	}
	return cfg;
}

/*
 * W21h (vayu_wifi_wmi=8): =6 plus ce5 filled to nentries-1 like ath10k
 * ath10k_snoc_rx_post_pipe() (__ath10k_ce_rx_num_free_bufs = nentries-1); flame
 * RX_POST stays 32 on every other host RX CE.  Pool cost: 512 x 2048 bytes for ce5.
 */
void
vayu_wmi_rx_post_want(uint32_t *want, uint32_t count, uint32_t def)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		want[i] = def;
	}
	if (vayu_wmi_mode == 8 && count > 5) {
		want[5] = 0xffffffffU;  /* clamped to nentries-1 by vayu_ce_rx_post_count() */
	}
}
