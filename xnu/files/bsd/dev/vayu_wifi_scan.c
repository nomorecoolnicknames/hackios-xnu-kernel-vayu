/*
 * W22: HTT host rx ring + passive scan after WMI READY (boot-arg vayu_wifi_scan=1).
 * Ported from ios-flame phone/wifi/radio.c (commit 745c4341): scan_freqs,
 * scan_start, scan_send_start, scan_event, scan_mgmt_rx (lines 700-947),
 * htt_post / htt_publish / htt_configure / htt_start / htt_rx (lines 960-1120);
 * that code follows Linux ath10k wmi-tlv.c, htt_tx.c and htt_rx.c.  Flame's notes
 * (HANDOFF.md:190): without the HTT host rx ring no beacon reaches the scan, so
 * the ring is set up first, exactly as flame does; beacons arrive as WMI MGMT_RX.
 * REG_CHAN_LIST_CC (0x3a001) is decoded as ath11k wmi.h wmi_reg_chan_list_cc_event /
 * wmi_regulatory_rule_struct; 0x1d00a is WMI_TLV_WLAN_FREQ_AVOID_EVENTID (ath10k
 * wmi-tlv.h, GRP_MISC + 9; tags 0x47 AVOID_FREQ_RANGES_EVENT, 0x48 RANGE_DESC).
 * W22b: the HTT ring has its own pool below 32 GiB (not the < 4 GiB WMI allocator).
 * Changes: synchronous and bounded, channel list cut to the regulatory rules,
 * every channel passive (nothing is transmitted), BSSIDs masked in the log.
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

#include <stddef.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <sys/vayu_wifi_net.h>
#include <sys/vayu_wifi_rsn.h>
#include <sys/vayu_wifi_scan.h>
#include <sys/vayu_wifi_smmu.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define EV_REG_CHAN_LIST_CC     0x3a001U
#define EV_FREQ_AVOID           0x1d00aU
#define CMD_START_SCAN          0x3001U
#define CMD_SCAN_CHAN_LIST      0x3003U
#define CMD_VDEV_CREATE         0x5001U
#define TAG_ARRAY_UINT32        0x10U
#define TAG_ARRAY_BYTE          0x11U
#define TAG_ARRAY_STRUCT        0x12U
#define TAG_ARRAY_FIXED_STRUCT  0x13U
#define TAG_SCAN_EVENT          0x24U
#define TAG_MGMT_RX_HDR         0x2cU
#define TAG_AVOID_FREQ_RANGES   0x47U
#define TAG_AVOID_FREQ_RANGE    0x48U
#define TAG_START_SCAN_CMD      0x4dU
#define TAG_SCAN_CHAN_LIST_CMD  0x4fU
#define TAG_CHANNEL             0x50U
#define TAG_VDEV_CREATE_CMD     0x56U
#define TAG_REG_RULE            0x260U
#define TAG_REG_CHAN_LIST_CC    0x261U
#define SCAN_EV_STARTED         0x001U
#define SCAN_EV_COMPLETED       0x002U
#define SCAN_EV_BSS_CHANNEL     0x004U
#define SCAN_EV_FOREIGN         0x008U
#define SCAN_EV_DEQUEUED        0x010U
#define SCAN_EV_PREEMPTED       0x020U
#define SCAN_EV_START_FAILED    0x040U
#define SCAN_EV_FOREIGN_EXIT    0x100U
#define SCAN_PASSIVE            0x01U   /* WMI_SCAN_FLAG_PASSIVE */
#define SCAN_CHAN_STAT          0x10U
#define SCAN_FILTER_PROBE_REQ   0x20U   /* inverted on TLV firmware (ath10k wmi-tlv.c:1652) */
#define CHAN_PASSIVE            (1U << 7)
#define CHAN_ALLOW_HT           (1U << 11)
#define CHAN_ALLOW_VHT          (1U << 12)
#define VDEV_TYPE_STA           2U
#define SCAN_ID                 0xa001U
#define SCAN_REQ_ID             0xa001U
#define DWELL_ACTIVE_MS         50U
#define DWELL_PASSIVE_MS        150U
#define MAX_SCAN_MS             20000U
#define SCAN_WAIT_MS            25000U
#define CREDIT_WAIT_MS          1000U
#define MAX_RULES               8U
#define MAX_CHANS               40U
#define NBSS                    64U
#define FRAME_LOG_MAX           256U
/* HTT (flame radio.c:967-988, ath10k htt.h, wcn3990 htt_rx_desc_v2 offsets in words) */
#define H2T_VERSION_REQ         0U
#define H2T_RX_RING_CFG         2U
#define H2T_AGGR_CFG            5U
#define H2T_FRAG_DESC_BANK_CFG  6U
#define H2T_TX_FRM              1U
/* W25: HTT data TX (ath10k_htt_tx_64, flame radio.c) */
#define TX_SLOTS                64U
#define TX_SLOT_SIZE            2048U
#define TXF0_MAC_HDR            0x01U
#define TXF0_NO_ENCRYPT         0x04U
#define TXF0_NATIVE             0x20U
#define TXF1_TID_NONQOS         (16U << 6)
#define TXF1_POSTPONED          (1U << 11)
#define PREFETCH_LEN            50U
#define T2H_TX_COMPL_IND        0x07U
#define T2H_VERSION_CONF        0x00U
#define T2H_RX_IN_ORD_PADDR_IND 0x12U
#define HTT_RING_MAX            256U    /* flame HTT_RING_SIZE */
#define HTT_RING_MIN            128U    /* ath10k htt.h HTT_RX_RING_SIZE_MIN */
#define HTT_RING_SIZE           (sc.ring_n)
/* W22b: own pool, below 32 GiB (35-bit CE/HTT addresses, ath10k snoc.c dma_mask 35; HTT ring cfg is 64-bit) */
#define HTT_POOL_LIMIT          ((uint64_t)32 << 30)
#define HTT_BUF_SIZE            2048U
#define HTT_MAX_PENDING_TX      (1024U + 32U)
#define HTT_EXT_DESC_SIZE       72U
#define OFF_ATTENTION           1U
#define OFF_FRAG_INFO           2U
#define OFF_MPDU_START          4U
#define OFF_MSDU_START          7U
#define OFF_MSDU_END            12U
#define OFF_MPDU_END            26U
#define OFF_PPDU_START          27U
#define OFF_PPDU_END            37U
#define OFF_HDR_STATUS          74U
#define OFF_MSDU_PAYLOAD        90U
#define HTT_VERSION_WAIT_MS     2000U

uint32_t vayu_scan_mode;

/* flame scan_freqs: 2.4 GHz 1-13, 5 GHz UNII-1/2/2e/3 */
static const uint16_t scan_freqs[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
	5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
	5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700,
	5745, 5765, 5785, 5805, 5825,
};
#define NFREQ ((uint32_t)(sizeof(scan_freqs) / sizeof(scan_freqs[0])))

struct reg_rule {
	uint16_t lo, hi;
	uint8_t bw, pwr;
	uint32_t flags;
};

struct bss {
	uint8_t bssid[6];
	uint8_t chan;
	uint16_t seen;
};

static int (*slog)(const char *format, ...) __printflike(1, 2);
static struct {
	bool reg_seen;
	char alpha2[3];
	uint32_t reg_events, country, domain, dfs, nrules;
	struct reg_rule rule[MAX_RULES];
	uint32_t avoid_events;
	/* scan */
	bool started, done;
	uint32_t done_event, done_reason, foreign, scan_events;
	uint32_t frames, beacons, probe_resp, bad, logged, other_mgmt;
	uint32_t nbss;
	struct bss bss[NBSS];
	uint8_t chan_seen[200];
	/* htt */
	bool htt_version;
	uint32_t ring_n, htt_reposts;
	uint32_t htt_major, htt_minor, htt_msgs, htt_msdus, htt_alloc;
	uint64_t htt_pa, htt_ring_pa, htt_idx_pa, htt_bank_pa, htt_buf_pa;
	vm_offset_t htt_va;
	uint32_t eapol_seen;
	uint32_t dumps, notes_off;
	/* W24d */
	bool in_ord_parsed, rx_dumped_first, rx_dumped_eapol;
	uint32_t rx_msdus, rx_not_done, rx_offload;
} sc;

/* W25: the HTT TX pool and the bank slots it uses (ids 0..TX_SLOTS-1 of the frag bank). */
static LCK_GRP_DECLARE(tx_lck_grp, "vayu-wifi-tx");
static LCK_MTX_DECLARE(tx_lock, &tx_lck_grp);
static const struct vayu_scan_ops *tx_ops;
static uint64_t tx_pool_pa;
static vm_offset_t tx_pool_va;
static uint64_t tx_busy;
static uint32_t tx_sent, tx_done, tx_fail;
static uint32_t tx_status[8];           /* completions by the firmware's status nibble */
/* W25n: why a frame was not sent (the numbers the 10 s line prints). */
static uint32_t tx_drop_noslot, tx_drop_send, tx_drop_len, tx_drop_pool;
/* W25j: every data frame gets a fresh 802.11 sequence number.  The AP's duplicate cache holds
 * the last sequence number per TID and address for a second or two and drops a frame that
 * repeats it: W25i sent 2/4 and 4/4 (and then every data frame) as sequence 0, so the AP took
 * the 2/4 and dropped the 4/4 as a duplicate, timed the 4-way out (deauth reason 15) and never
 * accepted the DHCP frames. */
static uint32_t tx_seq;

static void vayu_scan_htt_tx_compl(const uint8_t *p, uint32_t len);
int vayu_scan_tx_frame(const uint8_t *f, uint32_t len, bool clear);

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
put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	return p + 2;
}

static uint8_t *
put32(uint8_t *p, uint32_t v)
{
	return put16(put16(p, v), v >> 16);
}

static uint8_t *
put64(uint8_t *p, uint64_t v)
{
	return put32(put32(p, (uint32_t)v), (uint32_t)(v >> 32));
}

static uint8_t *
put_tlv(uint8_t *p, uint32_t tag, uint32_t len)
{
	return put16(put16(p, len), tag);
}

static void
zero(void *b, uint32_t n)
{
	uint8_t *p = b;

	while (n-- != 0) {
		*p++ = 0;
	}
}

void
vayu_scan_reset(int (*log)(const char *format, ...))
{
	uint32_t mode = 0;

	zero(&sc, (uint32_t)sizeof(sc));
	slog = log;
	PE_parse_boot_argn("vayu_wifi_scan", &mode, sizeof(mode));
	vayu_scan_mode = mode == 1 ? 1 : 0;
	if (mode != 0 && slog != NULL) {
		slog("vayu-wifi: w22: vayu_wifi_scan=%u%s: HTT rx ring + passive scan after WMI READY\n",
		    mode, vayu_scan_mode != 0 ? "" : " (unknown, off)");
	}
}

/* ------------------------------------------------------------ 0x3a001 / 0x1d00a */

static void
reg_chan_list(const uint8_t *p, uint32_t len)
{
	uint32_t o, l, tag;

	sc.reg_events++;
	for (o = 0; o + 4 <= len; o += 4 + l) {
		const uint8_t *v = p + o + 4;

		l = rd16(p + o);
		tag = rd16(p + o + 2);
		if (o + 4 + l > len) {
			break;
		}
		if (tag == TAG_REG_CHAN_LIST_CC && l >= 56) {
			/* status, phy_id, alpha2, num_phy, country_id, domain_code, dfs_region,
			 * phybitmask, min/max_bw_2g, min/max_bw_5g, num_2g_rules, num_5g_rules */
			sc.alpha2[0] = (v[8] >= 'A' && v[8] <= 'Z') ? (char)v[8] : '?';
			sc.alpha2[1] = (v[9] >= 'A' && v[9] <= 'Z') ? (char)v[9] : '?';
			sc.country = rd32(v + 16);
			sc.domain = rd32(v + 20);
			sc.dfs = rd32(v + 24);
			slog("vayu-wifi: w22: REG_CHAN_LIST_CC #%u: status %u phy %u country %s (%u) "
			    "domain %#x dfs_region %u num_phy %u phybitmap %#x bw2g %u..%u bw5g %u..%u "
			    "rules 2g %u 5g %u\n", sc.reg_events, rd32(v), rd32(v + 4), sc.alpha2,
			    sc.country, sc.domain, sc.dfs, rd32(v + 12), rd32(v + 28), rd32(v + 32),
			    rd32(v + 36), rd32(v + 40), rd32(v + 44), rd32(v + 48), rd32(v + 52));
		} else if (tag == TAG_ARRAY_STRUCT) {
			uint32_t q, ql, n = 0;

			for (q = 0; q + 4 <= l; q += 4 + ql) {
				ql = rd16(v + q);
				if (q + 4 + ql > l) {
					break;
				}
				if (rd16(v + q + 2) == TAG_REG_RULE && ql >= 12) {
					const uint8_t *r = v + q + 4;
					struct reg_rule rr = {
						.lo = (uint16_t)rd16(r), .hi = (uint16_t)rd16(r + 2),
						.bw = (uint8_t)rd16(r + 4), .pwr = r[6], .flags = rd32(r + 8),
					};

					slog("vayu-wifi: w22: reg rule %u: %u-%u MHz bw %u max %u dBm "
					    "gain %u flags %#x%s\n", n, rr.lo, rr.hi, rr.bw, rr.pwr, r[7],
					    rr.flags, (rr.flags & 0x8U) != 0 ? " (DFS)" : "");
					if (sc.reg_events == 1 && sc.nrules < MAX_RULES) {
						sc.rule[sc.nrules++] = rr;
					}
					n++;
				}
			}
		}
	}
	sc.reg_seen = sc.nrules != 0;
}

static void
freq_avoid(const uint8_t *p, uint32_t len)
{
	uint32_t o, l, tag, n = 0;

	sc.avoid_events++;
	for (o = 0; o + 4 <= len; o += 4 + l) {
		const uint8_t *v = p + o + 4;

		l = rd16(p + o);
		tag = rd16(p + o + 2);
		if (o + 4 + l > len) {
			break;
		}
		if (tag == TAG_AVOID_FREQ_RANGES && l >= 4) {
			n = rd32(v);
		} else if (tag == TAG_ARRAY_STRUCT) {
			uint32_t q, ql;

			for (q = 0; q + 4 <= l; q += 4 + ql) {
				ql = rd16(v + q);
				if (q + 4 + ql > l) {
					break;
				}
				if (rd16(v + q + 2) == TAG_AVOID_FREQ_RANGE && ql >= 8) {
					slog("vayu-wifi: w22: FREQ_AVOID #%u range %u-%u MHz\n", sc.avoid_events,
					    rd32(v + q + 4), rd32(v + q + 8));
				}
			}
		}
	}
	slog("vayu-wifi: w22: FREQ_AVOID #%u (WLAN_FREQ_AVOID_EVENTID): %u ranges\n",
	    sc.avoid_events, n);
}

/* ------------------------------------------------------------ scan events */

static void
scan_event(const uint8_t *p, uint32_t len)
{
	uint32_t e, reason, freq;

	if (len < 4 + 24 || rd16(p + 2) != TAG_SCAN_EVENT) {
		return;
	}
	e = rd32(p + 4);
	reason = rd32(p + 8);
	freq = rd32(p + 12);
	sc.scan_events++;
	if (e == SCAN_EV_FOREIGN) {
		if (sc.foreign++ < 4) {
			slog("vayu-wifi: w22: scan event FOREIGN_CHANNEL %u MHz\n", freq);
		}
		return;
	}
	if (e == SCAN_EV_FOREIGN_EXIT || e == SCAN_EV_BSS_CHANNEL) {
		return;                 /* flame: foreign exit / bss channel are not logged */
	}
	if (e == SCAN_EV_STARTED) {
		sc.started = true;
		slog("vayu-wifi: w22: scan event STARTED (scan_id %#x vdev %u)\n", rd32(p + 20),
		    rd32(p + 24));
		return;
	}
	if (e == SCAN_EV_COMPLETED || e == SCAN_EV_START_FAILED || e == SCAN_EV_DEQUEUED ||
	    e == SCAN_EV_PREEMPTED) {
		sc.done = true;
		sc.done_event = e;
		sc.done_reason = reason;
		slog("vayu-wifi: w22: scan event %s %#x reason %u (0 completed, 1 cancelled, "
		    "2 preempted, 3 timedout, 4 internal)\n", e == SCAN_EV_COMPLETED ? "COMPLETED" :
		    e == SCAN_EV_START_FAILED ? "START_FAILED" : "ENDED", e, reason);
		return;
	}
	slog("vayu-wifi: w22: scan event %#x reason %u freq %u\n", e, reason, freq);
}

/* flame scan_mgmt_rx(): MGMT_RX_HDR {channel, snr, rate, phy_mode, buf_len, status, ...} + frame bytes */
static void
mgmt_rx(const uint8_t *t, uint32_t len)
{
	const uint8_t *hdr = NULL, *f = NULL;
	uint32_t o, l, tag, flen = 0, chan, snr, status, i;
	int32_t rssi;
	char ssid[33];
	struct bss *b = NULL;
	bool fresh = false;

	for (o = 0; o + 4 <= len; o += 4 + l) {
		l = rd16(t + o);
		tag = rd16(t + o + 2);
		if (o + 4 + l > len) {
			break;
		}
		if (tag == TAG_MGMT_RX_HDR && l >= 24 && hdr == NULL) {
			hdr = t + o + 4;
		} else if (tag == TAG_ARRAY_BYTE && f == NULL) {
			f = t + o + 4;
			flen = l;
		}
	}
	sc.frames++;
	if (hdr == NULL || f == NULL) {
		sc.bad++;
		return;
	}
	chan = rd32(hdr);
	snr = rd32(hdr + 4);
	status = rd32(hdr + 20);
	if (rd32(hdr + 16) < flen) {
		flen = rd32(hdr + 16);
	}
	if (flen < 36 || (f[0] & 0x0cU) != 0 || ((f[0] >> 4) != 8 && (f[0] >> 4) != 5)) {
		sc.other_mgmt++;
		return;                 /* beacons and probe responses only */
	}
	if ((f[0] >> 4) == 8) {
		sc.beacons++;
	} else {
		sc.probe_resp++;
	}
	rssi = (int32_t)snr - 95;       /* flame: snr over ath10k's default noise floor */
	ssid[0] = 0;
	for (o = 36; o + 2 <= flen && o + 2 + f[o + 1] <= flen; o += 2 + f[o + 1]) {
		if (f[o] == 0) {
			uint32_t n = f[o + 1] < 32 ? f[o + 1] : 32;

			for (i = 0; i < n; i++) {
				uint8_t ch = f[o + 2 + i];

				ssid[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
			}
			ssid[n] = 0;
			if (n == 0 || f[o + 2] == 0) {
				ssid[0] = '<', ssid[1] = 'h', ssid[2] = 'i', ssid[3] = 'd', ssid[4] = 'd';
				ssid[5] = 'e', ssid[6] = 'n', ssid[7] = '>', ssid[8] = 0;
			}
		} else if (f[o] == 3 && f[o + 1] == 1) {
			chan = f[o + 2];        /* DS parameter set */
		}
	}
	if (chan < sizeof(sc.chan_seen)) {
		sc.chan_seen[chan] = 1;
	}
	for (i = 0; i < sc.nbss && b == NULL; i++) {
		uint32_t k;

		for (k = 0; k < 6 && sc.bss[i].bssid[k] == f[16 + k]; k++) {
		}
		if (k == 6) {
			b = &sc.bss[i];
		}
	}
	if (b == NULL && sc.nbss < NBSS) {
		b = &sc.bss[sc.nbss++];
		for (i = 0; i < 6; i++) {
			b->bssid[i] = f[16 + i];
		}
		fresh = true;
	}
	if (b != NULL) {
		b->seen++;
		b->chan = (uint8_t)chan;
	}
	if (fresh || sc.logged < 32) {
		sc.logged++;
		slog("vayu-wifi: w22: %s ch %u rssi %d bssid xx:xx:xx:xx:%02x:%02x ssid '%s' status %#x%s\n",
		    (f[0] >> 4) == 8 ? "beacon" : "probe_resp", chan, rssi, f[20], f[21], ssid, status,
		    fresh ? " (new)" : "");
	}
}

void
vayu_scan_wmi_rx(uint32_t id, const uint8_t *p, uint32_t len)
{
	if (slog == NULL || vayu_scan_mode == 0) {
		return;
	}
	switch (id) {
	case EV_REG_CHAN_LIST_CC:
		reg_chan_list(p, len);
		break;
	case EV_FREQ_AVOID:
		freq_avoid(p, len);
		break;
	case VAYU_SCAN_WMI_EV_SCAN:
		scan_event(p, len);
		break;
	case VAYU_SCAN_WMI_EV_MGMT_RX:
		mgmt_rx(p, len);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------ HTT host rx ring (flame htt_*) */

static void
htt_wr32(uint64_t pa, uint32_t v)
{
	*(volatile uint32_t *)(sc.htt_va + (vm_offset_t)(pa - sc.htt_pa)) = v;
}

static void
htt_post(uint64_t pa)
{
	volatile uint32_t *slot;

	htt_wr32(pa + 4 * OFF_ATTENTION, 0);
	slot = (volatile uint32_t *)(sc.htt_va + (vm_offset_t)(sc.htt_ring_pa - sc.htt_pa) + 8U * sc.htt_alloc);
	slot[0] = (uint32_t)pa;
	slot[1] = (uint32_t)(pa >> 32);
	sc.htt_alloc = (sc.htt_alloc + 1) & (HTT_RING_SIZE - 1);
}

static void
htt_publish(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	htt_wr32(sc.htt_idx_pa, sc.htt_alloc);
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * W24d: one in-order rx msdu buffer (ath10k htt.h struct htt_rx_desc, rx_desc.h; flame htt_rx()):
 *   word 0  fw_desc pad              word 1  attention, bit 31 MSDU_DONE
 *   word 4  mpdu_start info0: peer 10:0, FROM_DS 11, seq 27:16, encrypt type 31:28 (7 = none)
 *   word 7  msdu_start info0: length 13:0, ip offset 19:14;  word 9 info1: decap 9:8 (1 = native wifi)
 *   word 90 payload (OFF_MSDU_PAYLOAD, the offset this driver gives the target in RX_RING_CFG).
 * The W23/W24 runs (193128-run.log) show that layout: attention 0x82300043, seq 2..5, encrypt 7,
 * msdu length 131 = 24 (802.11 without QoS) + 8 (LLC/SNAP) + 99 (EAPOL-Key, no key data), decap 1.
 * W24a-k found the LLC/SNAP at +360 but dropped the frame on "EAPOL version != 2" without a word;
 * the version is 1 or 2 (802.1X-2001/2004), and every EAPOL-Key field is big-endian.
 * The length now comes from the in-order descriptor, as in flame.  Nonces are never printed.
 */
#define ATTN_MSDU_DONE          0x80000000U
#define IN_ORD_INFO_OFFLOAD     0x01U   /* ath10k HTT_RX_IN_ORD_IND_INFO_OFFLOAD_MASK */
#define OFFLOAD_HDR_LEN         7U      /* ath10k struct htt_rx_offload_msdu */
#define DECAP_RAW               0U
#define DECAP_NWIFI             1U
#define DECAP_ETH2              2U
#define DECAP_8023              3U
#define RX_DUMP_LEN             96U
#define RX_DECODE_LOGS          8U

/* W24E: the RX path may not printf while the ring is being consumed; the W24D kernel took a
 * data abort inside __doprnt from htt_in_ord (esr 0x96000005, far 0x100000007). Diagnostics are
 * opt-in: vayu_wifi_rxlog=1 restores the per-msdu lines and the hex dump. */
static int rxlog = -1;
static inline bool
rx_logging(void)
{
	if (rxlog < 0) {
		uint32_t v = 0;
		PE_parse_boot_argn("vayu_wifi_rxlog", &v, sizeof(v));
		rxlog = v != 0;
	}
	return rxlog != 0;
}
#define KI_PAIRWISE             0x0008U
#define KI_INSTALL              0x0040U
#define KI_ACK                  0x0080U
#define KI_MIC                  0x0100U
#define KI_SECURE               0x0200U
#define KI_ERROR                0x0400U
#define KI_REQUEST              0x0800U
#define KI_ENC_KD               0x1000U

static const uint8_t llc_eapol[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };

static uint32_t
be16(const uint8_t *p)
{
	return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}

static bool
same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

/* offset of the EAPOL header (version byte) in the payload, 0 when it is not EAPOL; *hdr = L2 header length */
static uint32_t
rx_eapol_off(const uint8_t *f, uint32_t len, uint32_t decap, uint32_t *hdr)
{
	uint32_t h;

	*hdr = 0;
	if (decap == DECAP_ETH2) {
		*hdr = 14;
		return len >= 18 && f[12] == 0x88 && f[13] == 0x8e ? 14U : 0U;
	}
	if (decap == DECAP_8023) {
		*hdr = 14;
		return len >= 26 && same(f + 14, llc_eapol, 8) ? 22U : 0U;
	}
	if (len < 24 || (f[0] & 0x0c) != 0x08) {
		return 0;               /* not a data frame */
	}
	h = 24;
	if ((f[1] & 0x03) == 0x03) {
		h += 6;                 /* 4-address */
	}
	if (decap == DECAP_RAW && (f[0] & 0x80) != 0) {
		h += 2;                 /* raw keeps QoS control; native wifi drops it */
	}
	*hdr = h;
	return len >= h + 8 + 4 && same(f + h, llc_eapol, 8) ? h + 8 : 0U;
}

static bool
rx_masked_mac(uint32_t i, uint32_t hdr)
{
	if (hdr == 14) {        /* Ethernet: DA 0-5, SA 6-11 */
		return i < 4 || (i >= 6 && i < 10);
	}
	if (hdr >= 24) {        /* 802.11: addr1 4-9, addr2 10-15, addr3 16-21 */
		return (i >= 4 && i < 8) || (i >= 10 && i < 14) || (i >= 16 && i < 20);
	}
	return false;
}

static void __attribute__((noinline))
rx_dump(const char *what, const uint8_t *f, uint32_t len, uint32_t hdr, uint32_t eo)
{
	static const char hx[] = "0123456789abcdef";
	char s[RX_DUMP_LEN * 3 + 1];
	uint32_t n = len < RX_DUMP_LEN ? len : RX_DUMP_LEN, i, j = 0;

	for (i = 0; i < n; i++) {
		if (rx_masked_mac(i, hdr)) {
			s[j++] = 'x';
			s[j++] = 'x';
		} else if (eo != 0 && i >= eo + 17 && i < eo + 49) {
			s[j++] = '.';   /* key nonce */
			s[j++] = '.';
		} else {
			s[j++] = hx[f[i] >> 4];
			s[j++] = hx[f[i] & 15];
		}
		s[j++] = ' ';
	}
	if (j != 0) {
		j--;
	}
	s[j] = 0;
	printf("vayu-wifi: w24: rx %s frame %u bytes, hex %u (MAC bytes 0-3 xx, nonce ..): %s\n", what, len, n, s);
}

/* e = EAPOL header (version, type, body length), len = bytes from e to the end of the msdu */
static void __attribute__((noinline))
eapol_key(const uint8_t *e, uint32_t len, uint32_t mlen)
{
	const uint8_t *k = e + 4;
	uint32_t ver = e[0], type = e[1], body = be16(e + 2), info, klen, kd, msg;
	uint64_t replay = 0;
	bool nonzero = false;
	const char *grp = "";

	if (type != 3) {
		printf("vayu-wifi: w24: eapol ver %u type %u body %u: not EAPOL-Key\n", ver, type, body);
		return;
	}
	if (len < 4 + 95) {
		printf("vayu-wifi: w24: eapol-key short: %u bytes (body %u)\n", len, body);
		return;
	}
	info = be16(k + 1);
	klen = be16(k + 3);
	for (unsigned i = 0; i < 8; i++) {
		replay = replay << 8 | k[5 + i];
	}
	for (unsigned i = 0; i < 32; i++) {
		nonzero |= k[13 + i] != 0;
	}
	kd = be16(k + 93);
	msg = 0;
	if ((info & KI_PAIRWISE) == 0) {
		grp = "group ";
		msg = (info & KI_ACK) != 0 ? 1 : 2;
	} else if ((info & KI_ACK) != 0) {
		msg = (info & KI_MIC) != 0 ? 3 : 1;
	} else if ((info & KI_MIC) != 0) {
		msg = (info & KI_SECURE) != 0 ? 4 : 2;
	}
	sc.eapol_seen++;
	/* W24G: no %s in this line. The W24D/F kernels died in __doprnt reading address 1
	 * (esr 0x96000005, far 0x1) right after this call, so the line is all-numeric now. */
	if (sc.eapol_seen <= 8) {
	printf("vayu-wifi: w24: eapol group %u %u/%u ver %u desc %u key_info 0x%04x (kver %u "
	    "pairwise %u install %u ack %u mic %u secure %u err %u req %u enc_kd %u) key_len %u "
	    "replay %llu key data length %u body %u msdu %u nonce_set %u (#%u)\n",
	    grp[0] != 0, msg, grp[0] != 0 ? 2U : 4U, ver, k[0], info, info & 7U,
	    (info & KI_PAIRWISE) != 0, (info & KI_INSTALL) != 0, (info & KI_ACK) != 0,
	    (info & KI_MIC) != 0, (info & KI_SECURE) != 0, (info & KI_ERROR) != 0,
	    (info & KI_REQUEST) != 0, (info & KI_ENC_KD) != 0, klen,
	    (unsigned long long)replay, kd, body, mlen, nonzero != 0, sc.eapol_seen);
	}
}

static void
htt_rx_msdu(const uint8_t *buf, uint32_t mlen, uint32_t fw_desc, uint32_t info)
{
	uint32_t attn = rd32(buf + 4 * OFF_ATTENTION);
	uint32_t mpdu0 = rd32(buf + 4 * OFF_MPDU_START);
	uint32_t msdu0 = rd32(buf + 4 * OFF_MSDU_START);
	uint32_t msdu1 = rd32(buf + 4 * (OFF_MSDU_START + 2));
	uint32_t dlen = msdu0 & 0x3fffU, decap = (msdu1 >> 8) & 3U;
	uint32_t room = HTT_BUF_SIZE - 4 * OFF_MSDU_PAYLOAD, len, hdr = 0, eo = 0, n;
	const uint8_t *f = buf + 4 * OFF_MSDU_PAYLOAD;

	n = ++sc.rx_msdus;
	if ((info & IN_ORD_INFO_OFFLOAD) != 0) {
		/* no rx descriptor: a 7-byte header (len, peer, vdev, tid, fw_desc), then an 802.11 frame */
		sc.rx_offload++;
		f = buf + OFFLOAD_HDR_LEN;
		room = HTT_BUF_SIZE - OFFLOAD_HDR_LEN;
		len = rd16(buf);
		len = len < room ? len : room;
		decap = DECAP_RAW;
		eo = rx_eapol_off(f, len, DECAP_RAW, &hdr);
		if (n <= RX_DECODE_LOGS && rx_logging()) {
			printf("vayu-wifi: w24: rx msdu #%u offload: len %u peer %u vdev %u tid %u fw_desc %#x eapol_off %u\n",
			    n, len, rd16(buf + 2), buf[4], buf[5], buf[6], eo);
		}
	} else {
		len = mlen != 0 ? mlen : dlen;
		len = len < room ? len : room;
		if ((attn & ATTN_MSDU_DONE) != 0) {
			eo = rx_eapol_off(f, len, decap, &hdr);
		}
		if (n <= RX_DECODE_LOGS && rx_logging()) {
			printf("vayu-wifi: w24: rx msdu #%u in-ord len %u fw_desc %#x info %#x | attn %#010x done %u | "
			    "mpdu seq %u enc %u from_ds %u peer %u | msdu len %u ip_off %u decap %u | fc %02x %02x eapol_off %u\n",
			    n, mlen, fw_desc, info, attn, (attn & ATTN_MSDU_DONE) != 0, (mpdu0 >> 16) & 0xfffU,
			    mpdu0 >> 28, (mpdu0 >> 11) & 1U, mpdu0 & 0x7ffU, dlen, (msdu0 >> 14) & 0x3fU, decap,
			    f[0], f[1], eo);
		}
		if ((attn & ATTN_MSDU_DONE) == 0) {
			sc.rx_not_done++;
			return;
		}
	}
	if (n <= 16 && eo == 0 && hdr >= 24 && len >= hdr + 8) {
		uint32_t etype = (uint32_t)f[hdr + 6] << 8 | f[hdr + 7], sp = 0, dp = 0;
		uint32_t mde = rd32(buf + 4 * OFF_MPDU_END);

		if (etype == 0x0800 && len >= hdr + 8 + 20 && (f[hdr + 8] >> 4) == 4 &&
		    f[hdr + 8 + 9] == 17) {
			uint32_t ihl = (uint32_t)(f[hdr + 8] & 0x0f) * 4;

			if (ihl >= 20 && len >= hdr + 8 + ihl + 4) {
				sp = (uint32_t)f[hdr + 8 + ihl] << 8 | f[hdr + 8 + ihl + 1];
				dp = (uint32_t)f[hdr + 8 + ihl + 2] << 8 | f[hdr + 8 + ihl + 3];
			}
		}
		printf("vayu-wifi: W25 rxdata #%u len %u decap %u attn %#010x dec_err %u mpdu_enc %u "
		    "enctype %u prot %u mde %#x type %#x sport %u dport %u\n", n, len, decap, attn,
		    (attn & (1U << 29)) != 0, (mpdu0 >> 13) & 1U, mpdu0 >> 28, (f[1] & 0x40) != 0, mde,
		    etype, sp, dp);
	}
	if (rx_logging() && (!sc.rx_dumped_first || (eo != 0 && !sc.rx_dumped_eapol))) {
		rx_dump(sc.rx_dumped_first ? "eapol" : "first", f, len, hdr, eo);
		sc.rx_dumped_first = true;
		sc.rx_dumped_eapol |= eo != 0;
	}
	if (eo != 0) {
		eapol_key(f + eo, len - eo, len);
		vayu_rsn_eapol_rx(f + eo, len - eo);
	} else {
		vayu_net_rx_80211(f, len, decap, hdr);
	}
}

static bool
htt_pa_ok(uint64_t pa)
{
	return pa >= sc.htt_buf_pa && pa < sc.htt_buf_pa + (uint64_t)HTT_RING_SIZE * HTT_BUF_SIZE &&
	       (pa - sc.htt_buf_pa) % HTT_BUF_SIZE == 0;
}

/* ath10k struct htt_rx_in_ord_ind: type, info, peer_id, vdev_id, reserved, msdu_count, then
 * htt_rx_in_ord_msdu_desc_ext { le64 paddr; le16 msdu_len; u8 fw_desc; u8 reserved; } */
static void
htt_in_ord(const uint8_t *p, uint32_t len, bool repost)
{
	uint32_t n = rd16(p + 6), i;
	const uint8_t *d = p + 8;

	for (i = 0; i < n && 8 + (i + 1) * 12 <= len; i++, d += 12) {
		uint64_t pa = (uint64_t)rd32(d) | (uint64_t)rd32(d + 4) << 32;

		if (!htt_pa_ok(pa)) {
			sc.notes_off++;         /* W24F: no printf in the RX path */
			continue;
		}
		sc.htt_msdus++;
		htt_rx_msdu((const uint8_t *)(sc.htt_va + (vm_offset_t)(pa - sc.htt_pa)), rd16(d + 8), d[10], p[1]);
		if (repost) {
			htt_post(pa);   /* flame: recycle the same buffer, after it has been read */
			sc.htt_reposts++;
		}
	}
	if (repost) {
		htt_publish();
	}
}

/* W24: the join path also gets every HTT message (vayu_sta_htt_rx); it parses only those
 * vayu_scan_htt_rx did not (W24d: before, both parsed the same buffer, the second after the repost) */
void
vayu_scan_note_htt(const uint8_t *p, uint32_t len)
{
	if (sc.in_ord_parsed) {
		sc.in_ord_parsed = false;
		return;
	}
	if (p == NULL || len < 8) {
		return;
	}
	if (sc.htt_va == 0 || sc.ring_n == 0) {
		if (sc.notes_off++ < 2) {
			printf("vayu-wifi: w24: note_htt: ring not up (va %#llx ring_n %u mode %u slog %d)\n",
			    (unsigned long long)sc.htt_va, sc.ring_n, vayu_scan_mode, slog != NULL);
		}
		return;
	}
	if (p[0] == T2H_RX_IN_ORD_PADDR_IND) {
		htt_in_ord(p, len, false);
	}
}

void
vayu_scan_htt_rx(const uint8_t *p, uint32_t len)
{
	sc.in_ord_parsed = false;
	if (slog == NULL || vayu_scan_mode == 0 || len == 0) {
		return;
	}
	sc.htt_msgs++;
	if (p[0] == T2H_TX_COMPL_IND) {
		if (len >= 4) {
			vayu_scan_htt_tx_compl(p, len);
		}
		return;
	}
	if (p[0] == T2H_VERSION_CONF && len >= 3) {
		sc.htt_major = p[2];
		sc.htt_minor = p[1];
		sc.htt_version = true;
		return;
	}
	if (p[0] == T2H_RX_IN_ORD_PADDR_IND && len >= 8 && sc.htt_va != 0 && sc.ring_n != 0) {
		htt_in_ord(p, len, true);
		sc.in_ord_parsed = true;
	}
}

static bool
htt_version_done(void)
{
	return sc.htt_version;
}

/*
 * W22b: the W22 run (094746-run.log) got KERN_NO_SPACE from the WMI host-memory
 * allocator, which insists on < 4 GiB (the WMI chunk address word is 32-bit).
 * HTT needs no such limit, so the ring gets its own contiguous allocation below
 * 32 GiB, as the CE pool (vayu_wifi_ce.c), identity-mapped in the WLAN SMMU
 * context and checked page by page.  Never freed (the target may still DMA).
 */
static uint64_t
htt_pool_alloc(const struct vayu_scan_ops *o, uint32_t size)
{
	vm_offset_t kva = 0;
	vm_size_t bytes = round_page((vm_size_t)size);
	kern_return_t kr;
	uint64_t pa;

	kr = kmem_alloc_contig(kernel_map, &kva, bytes, PAGE_MASK,
	    (ppnum_t)((HTT_POOL_LIMIT >> PAGE_SHIFT) - 1), 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS || kva == 0) {
		o->log("vayu-wifi: w22b: htt pool: no %u contiguous bytes below 32 GiB (%d)\n",
		    (unsigned int)bytes, kr);
		return 0;
	}
	FlushPoC_DcacheRegion(kva, bytes);
	__asm__ volatile("dsb sy" ::: "memory");
	pa = kvtophys(kva) & ~((uint64_t)PAGE_MASK);
	if (pa == 0 || pa + (uint64_t)bytes > HTT_POOL_LIMIT) {
		o->log("vayu-wifi: w22b: htt pool PA %#llx unusable; kept, not used\n", (unsigned long long)pa);
		return 0;
	}
	if (!vayu_wifi_smmu_map_dma(pa, bytes, "w22b htt pool")) {
		o->log("vayu-wifi: w22b: htt pool PA %#llx: SMMU map refused; kept, not used\n",
		    (unsigned long long)pa);
		return 0;
	}
	if (!vayu_wifi_smmu_check_range(pa, bytes, "w22b htt pool")) {
		o->log("vayu-wifi: w22b: htt pool PA %#llx: SMMU range check failed; not used\n",
		    (unsigned long long)pa);
		return 0;
	}
	o->log("vayu-wifi: w22b: htt pool PA %#llx size %u mapped %u/%u pages (context 4 identity)\n",
	    (unsigned long long)pa, (unsigned int)bytes, (unsigned int)(bytes >> 12),
	    (unsigned int)(bytes >> 12));
	return pa;
}

static uint32_t
htt_pool_size(uint32_t n)
{
	uint32_t ring = (n * 8 + 64 + 0xfff) & ~0xfffU;
	uint32_t bank = (HTT_MAX_PENDING_TX * HTT_EXT_DESC_SIZE + 0xfff) & ~0xfffU;

	return ring + bank + n * HTT_BUF_SIZE;
}

static int
htt_setup(const struct vayu_scan_ops *o)
{
	uint32_t ring, bank, total, i;
	uint8_t m[64], r[48], g[4] = { H2T_AGGR_CFG, 64, 3, 0 }, v[4] = { H2T_VERSION_REQ, 0, 0, 0 };
	uint8_t *p;
	int a, b, c;

	sc.ring_n = HTT_RING_MAX;
	sc.htt_pa = htt_pool_alloc(o, htt_pool_size(sc.ring_n));
	if (sc.htt_pa == 0) {
		sc.ring_n = HTT_RING_MIN;
		sc.htt_pa = htt_pool_alloc(o, htt_pool_size(sc.ring_n));
	}
	if (sc.htt_pa == 0) {
		sc.ring_n = 0;
		o->log("vayu-wifi: w22: htt: no DMA memory for %u or %u buffers; scanning without the rx ring\n",
		    HTT_RING_MAX, HTT_RING_MIN);
		return -1;
	}
	ring = (sc.ring_n * 8 + 64 + 0xfff) & ~0xfffU;
	bank = (HTT_MAX_PENDING_TX * HTT_EXT_DESC_SIZE + 0xfff) & ~0xfffU;
	total = htt_pool_size(sc.ring_n);
	sc.htt_va = ml_io_map_unmappable((vm_offset_t)sc.htt_pa, total, VM_WIMG_WCOMB);
	if (sc.htt_va == 0) {
		o->log("vayu-wifi: w22: htt: cannot map %#llx non-cacheable\n", (unsigned long long)sc.htt_pa);
		return -1;
	}
	sc.htt_ring_pa = sc.htt_pa;
	sc.htt_idx_pa = sc.htt_pa + HTT_RING_SIZE * 8;
	sc.htt_bank_pa = sc.htt_pa + ring;
	sc.htt_buf_pa = sc.htt_pa + ring + bank;
	o->log("vayu-wifi: w22: htt: %u bytes at PA %#llx (ring %u x %u)\n", total,
	    (unsigned long long)sc.htt_pa, HTT_RING_SIZE, HTT_BUF_SIZE);
	o->log("vayu-wifi: w22: htt: VERSION_REQ %s\n", o->htt_send(v, sizeof(v)) != 0 ? "FAILED" : "sent");
	if (o->wait(htt_version_done, HTT_VERSION_WAIT_MS) < 0) {
		return -1;
	}
	if (sc.htt_version) {
		o->log("vayu-wifi: w22: htt: VERSION_CONF %u.%u\n", sc.htt_major, sc.htt_minor);
	} else {
		o->log("vayu-wifi: w22: htt: no VERSION_CONF in %u ms, configuring anyway (flame)\n",
		    HTT_VERSION_WAIT_MS);
	}
	/* ath10k_htt_send_frag_desc_bank_cfg_64 */
	zero(m, sizeof(m));
	p = m;
	*p++ = H2T_FRAG_DESC_BANK_CFG;
	*p++ = 0;
	*p++ = 1;
	*p++ = HTT_EXT_DESC_SIZE;
	p = put64(p, sc.htt_bank_pa);
	p += 3 * 8;
	p = put16(p, 0);
	p = put16(p, HTT_MAX_PENDING_TX - 1);
	p += 3 * 4 + 4 + 2 + 2;
	*p++ = 1;
	a = o->htt_send(m, sizeof(m));
	/* ath10k_htt_send_rx_ring_cfg_64 */
	for (i = 0; i < HTT_RING_SIZE - 1; i++) {
		htt_post(sc.htt_buf_pa + (uint64_t)i * HTT_BUF_SIZE);
	}
	zero(r, sizeof(r));
	p = r;
	*p++ = H2T_RX_RING_CFG;
	*p++ = 1;
	p = put16(p, 0);
	p = put64(p, sc.htt_idx_pa);
	p = put64(p, sc.htt_ring_pa);
	p = put16(p, HTT_RING_SIZE);
	p = put16(p, HTT_BUF_SIZE);
	p = put16(p, 0xffff);           /* every frame type, management included */
	p = put16(p, 0);
	p = put16(p, OFF_HDR_STATUS);
	p = put16(p, OFF_MSDU_PAYLOAD);
	p = put16(p, OFF_PPDU_START);
	p = put16(p, OFF_PPDU_END);
	p = put16(p, OFF_MPDU_START);
	p = put16(p, OFF_MPDU_END);
	p = put16(p, OFF_MSDU_START);
	p = put16(p, OFF_MSDU_END);
	p = put16(p, OFF_ATTENTION);
	(void)put16(p, OFF_FRAG_INFO);
	htt_publish();
	b = o->htt_send(r, sizeof(r));
	c = o->htt_send(g, sizeof(g));
	o->log("vayu-wifi: w22: htt: frag bank %s, rx ring cfg %s (%u buffers), aggr cfg %s\n",
	    a != 0 ? "FAILED" : "ok", b != 0 ? "FAILED" : "ok", HTT_RING_SIZE - 1, c != 0 ? "FAILED" : "ok");
	return (a | b | c) != 0 ? -1 : 0;
}

/* ------------------------------------------------------------ channel list, vdev, start scan */

static bool
freq_allowed(uint32_t f)
{
	uint32_t i;

	if (!sc.reg_seen) {
		return true;            /* no REG_CHAN_LIST_CC: flame's list as is */
	}
	for (i = 0; i < sc.nrules; i++) {
		if (f - 10 >= sc.rule[i].lo && f + 10 <= sc.rule[i].hi) {
			return true;
		}
	}
	return false;
}

static int
wmi_cmd(const struct vayu_scan_ops *o, uint8_t *m, uint32_t cmd, uint32_t len)
{
	(void)put32(m, cmd);
	return o->wmi_send(m, len, CREDIT_WAIT_MS);
}

static bool
scan_done(void)
{
	return sc.done;
}

static bool
scan_started(void)
{
	return sc.started || sc.done;
}

/* ---- W25: HTT data TX ------------------------------------------------------------------------- */

/* One native-wifi data frame (see radio.c htt_tx_frame): the message carries the first 50 bytes,
 * the whole frame is in the TX pool and the bank entry points at it. 0 = sent. */
int
vayu_scan_tx_frame(const uint8_t *f, uint32_t len, bool clear)
{
	uint8_t m[20 + 52], *p, *ext;
	uint32_t id, pf, lo;
	uint16_t hi, l16;
	uint64_t pa;
	int rc;

	if (tx_ops == NULL || sc.htt_va == 0 || f == NULL || len == 0 || len > TX_SLOT_SIZE) {
		tx_drop_len++;
		return -1;
	}
	if (tx_pool_pa == 0) {
		/* W25i: htcc_dma_alloc caps at VAYU_WMI_MEM_TOTAL_MAX and does not map the SMMU; the HTT RX
		 * pool allocator does both (below 32 GiB, context 4 identity) */
		uint64_t pool = htt_pool_alloc(tx_ops, TX_SLOTS * TX_SLOT_SIZE);

		if (pool == 0) {
			tx_ops->log("vayu-wifi: W25 S4 tx: ERROR no HTT tx pool\n");
			tx_drop_pool++;
			return -1;
		}
		tx_pool_va = ml_io_map_unmappable((vm_offset_t)pool, TX_SLOTS * TX_SLOT_SIZE, VM_WIMG_WCOMB);
		if (tx_pool_va == 0) {
			tx_ops->log("vayu-wifi: W25 S4 tx: ERROR cannot map the HTT tx pool\n");
			return -1;
		}
		tx_pool_pa = pool;
		tx_ops->log("vayu-wifi: W25 S4 tx: OK HTT tx pool %u x %u at PA %#llx\n", TX_SLOTS, TX_SLOT_SIZE,
		    (unsigned long long)pool);
	}
	lck_mtx_lock(&tx_lock);
	for (id = 0; id < TX_SLOTS && (tx_busy & (1ULL << id)) != 0; id++) {
		;
	}
	if (id == TX_SLOTS) {
		tx_drop_noslot++;
		lck_mtx_unlock(&tx_lock);
		return -1;
	}
	/* W25n: the bit goes up before the frame does.  tx_frame() runs on the interface's thread
	 * while the completion is processed by the poll thread; the send waits for the CE doorbell
	 * (about a millisecond), so a completion could clear a bit that was not set yet and the
	 * slot stayed busy for good.  After ~64 such races every new frame found the pool full --
	 * the log's "frames tx 92 ... 371 dropped". */
	tx_busy |= 1ULL << id;
	pa = tx_pool_pa + (uint64_t)id * TX_SLOT_SIZE;
	{
		uint8_t *dst = (uint8_t *)(tx_pool_va + (vm_offset_t)(pa - tx_pool_pa));

		memcpy(dst, f, len);
		if (len >= 24) {
			tx_seq = (tx_seq + 1) & 0xfffU;
			dst[22] = (uint8_t)(tx_seq << 4);       /* sequence control, little-endian: seq << 4 */
			dst[23] = (uint8_t)(tx_seq >> 4);
		}
		if (tx_sent < 8 && slog != NULL) {
			slog("vayu-wifi: W25 txdata #%u len %u clear %u mcast %u seq %u\n", tx_sent, len,
			    clear ? 1 : 0, len >= 22 && (f[16] & 1) != 0, tx_seq);
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	ext = (uint8_t *)(sc.htt_va + (vm_offset_t)(sc.htt_bank_pa - sc.htt_pa) + (vm_offset_t)id * HTT_EXT_DESC_SIZE);
	memset(ext, 0, HTT_EXT_DESC_SIZE);
	lo = (uint32_t)pa;
	hi = (uint16_t)(pa >> 32);
	l16 = (uint16_t)len;
	memcpy(ext + 24, &lo, 4);
	memcpy(ext + 28, &hi, 2);
	memcpy(ext + 30, &l16, 2);
	__asm__ volatile("dsb sy" ::: "memory");
	pf = (len < PREFETCH_LEN ? len : PREFETCH_LEN) + 3U & ~3U;
	memset(m, 0, sizeof(m));
	p = m;
	*p++ = H2T_TX_FRM;
	*p++ = (uint8_t)(TXF0_MAC_HDR | TXF0_NATIVE | (clear ? TXF0_NO_ENCRYPT : 0));
	*p++ = (uint8_t)(TXF1_TID_NONQOS | TXF1_POSTPONED);
	*p++ = (uint8_t)((TXF1_TID_NONQOS | TXF1_POSTPONED) >> 8);
	*p++ = (uint8_t)len;
	*p++ = (uint8_t)(len >> 8);
	*p++ = (uint8_t)id;
	*p++ = (uint8_t)(id >> 8);
	{
		uint64_t bank = sc.htt_bank_pa + (uint64_t)id * HTT_EXT_DESC_SIZE;   /* this slot's ext descriptor */

		memcpy(p, &bank, 8);
		p += 8;
	}
	*p++ = 0xff;
	*p++ = 0xff;                                    /* peer id: invalid, the firmware resolves it */
	*p++ = 0;
	*p++ = 0;
	/* the prefetch is the frame's head as the firmware will read it (sequence number included) */
	memcpy(p, (const void *)(tx_pool_va + (vm_offset_t)(pa - tx_pool_pa)), pf);
	rc = tx_ops->htt_send(m, 20 + pf);
	if (rc != 0) {
		tx_busy &= ~(1ULL << id);
		tx_drop_send++;
		lck_mtx_unlock(&tx_lock);
		return -1;
	}
	tx_sent++;
	lck_mtx_unlock(&tx_lock);
	return 0;
}

static void
vayu_scan_htt_tx_compl(const uint8_t *p, uint32_t len)
{
	uint32_t n = p[2], st = p[1] & 7U, i;

	lck_mtx_lock(&tx_lock);
	for (i = 0; i < n && 4 + 2 * (i + 1) <= len; i++) {
		uint32_t id = rd16(p + 4 + 2 * i);

		if (id < TX_SLOTS) {
			tx_busy &= ~(1ULL << id);
		}
	}
	lck_mtx_unlock(&tx_lock);
	tx_done += n;
	if (st < 8) {
		tx_status[st] += n;
	}
	if (st != 0) {
		tx_fail += n;
		if (tx_fail <= 4 && slog != NULL) {
			slog("vayu-wifi: W25 tx: ERROR %u completion(s) status %u\n", n, st);
		}
	}
}


void
vayu_scan_tx_stats(uint32_t credits)
{
	uint32_t busy;

	if (slog == NULL) {
		return;
	}
	lck_mtx_lock(&tx_lock);
	busy = (uint32_t)__builtin_popcountll(tx_busy);
	lck_mtx_unlock(&tx_lock);
	slog("vayu-wifi: W25 tx: %u sent, %u completed (status 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u); "
	    "slots in flight %u, free %u of %u; drops: no slot %u, send %u, length %u, pool %u; wmi credits %u\n",
	    tx_sent, tx_done, tx_status[0], tx_status[1], tx_status[2], tx_status[3], tx_status[4],
	    tx_status[5], tx_status[6], tx_status[7], busy, TX_SLOTS - busy, TX_SLOTS, tx_drop_noslot,
	    tx_drop_send, tx_drop_len, tx_drop_pool, credits);
}

int
vayu_scan_run(const struct vayu_scan_ops *o)
{
	static uint8_t m[1600];
	uint16_t freqs[MAX_CHANS];
	uint32_t n = 0, i, total = 0;
	uint8_t *p, *c;
	char list[3 * MAX_CHANS + 64];
	uint32_t ll = 0;
	int a, b, s, w;

	if (o == NULL || vayu_scan_mode == 0) {
		return -1;
	}
	tx_ops = o;
	vayu_net_sta_attach(o->mac);            /* W25o: Apple80211 has a station before the scan */
	o->log("vayu-wifi: w22: scan stage: reg events %u (rules %u, country %s), freq_avoid events %u\n",
	    sc.reg_events, sc.nrules, sc.reg_seen ? sc.alpha2 : "-", sc.avoid_events);
	(void)htt_setup(o);
	for (i = 0; i < NFREQ && n < MAX_CHANS; i++) {
		if (freq_allowed(scan_freqs[i])) {
			freqs[n++] = scan_freqs[i];
		}
	}
	/* WMI_SCAN_CHAN_LIST: every channel passive, as flame (nothing is transmitted) */
	zero(m, sizeof(m));
	p = put32(put_tlv(m + 4, TAG_SCAN_CHAN_LIST_CMD, 4), n);
	p = put_tlv(p, TAG_ARRAY_STRUCT, n * (4 + 24));
	for (i = 0; i < n; i++) {
		uint32_t mode = freqs[i] < 4000 ? 1 : 0;        /* MODE_11G : MODE_11A */
		uint32_t flags = mode | CHAN_PASSIVE | CHAN_ALLOW_HT | (freqs[i] < 4000 ? 0 : CHAN_ALLOW_VHT);

		c = put_tlv(p, TAG_CHANNEL, 24);
		c = put32(c, freqs[i]);
		c = put32(c, freqs[i]);
		c = put32(c, 0);
		c = put32(c, flags);
		c = put32(c, (40U << 8) | (40U << 16));
		p = put32(c, 40U << 8);
	}
	a = wmi_cmd(o, m, CMD_SCAN_CHAN_LIST, (uint32_t)(p - m));
	o->log("vayu-wifi: w22: scan chan list (%u channels, all passive) %s\n", n, a != 0 ? "FAILED" : "ok");
	/* WMI_VDEV_CREATE vdev 0, STA, the WLFW/READY MAC */
	zero(m, sizeof(m));
	c = put_tlv(m + 4, TAG_VDEV_CREATE_CMD, 20);
	c = put32(c, 0);
	c = put32(c, VDEV_TYPE_STA);
	c = put32(c, 0);
	for (i = 0; i < 6; i++) {
		c[i] = o->mac[i];
	}
	b = wmi_cmd(o, m, CMD_VDEV_CREATE, 4 + 4 + 20);
	o->log("vayu-wifi: w22: vdev create 0 type STA mac xx:xx:xx:xx:%02x:%02x %s\n", o->mac[4], o->mac[5],
	    b != 0 ? "FAILED" : "ok");
	/* WMI_START_SCAN: flame scan_send_start */
	zero(m, sizeof(m));
	c = put_tlv(m + 4, TAG_START_SCAN_CMD, 100);
	c = put32(c, SCAN_ID);
	c = put32(c, SCAN_REQ_ID);
	c = put32(c, 0);                /* vdev */
	c = put32(c, 1);                /* priority low */
	c = put32(c, SCAN_EV_STARTED | SCAN_EV_COMPLETED | SCAN_EV_BSS_CHANNEL | SCAN_EV_FOREIGN |
	    SCAN_EV_FOREIGN_EXIT | SCAN_EV_DEQUEUED);
	c = put32(c, DWELL_ACTIVE_MS);
	c = put32(c, DWELL_PASSIVE_MS);
	c = put32(c, 50);               /* min rest */
	c = put32(c, 500);              /* max rest */
	c = put32(c, 0);
	c = put32(c, 0);
	c = put32(c, 0);
	c = put32(c, MAX_SCAN_MS);
	c = put32(c, 5);                /* probe delay */
	c = put32(c, (SCAN_PASSIVE | SCAN_CHAN_STAT) ^ SCAN_FILTER_PROBE_REQ);
	c = put32(c, 0);
	c = put32(c, n);
	c = put32(c, 1);                /* bssids */
	c = put32(c, 0);                /* ssids: none, no probe request */
	c = put32(c, 0);                /* ie_len */
	c = put32(c, 3);                /* num_probes (unused: passive) */
	p = c + 16;                     /* mac addr + mask: zero */
	c = put_tlv(p, TAG_ARRAY_UINT32, n * 4);
	for (i = 0; i < n; i++) {
		c = put32(c, freqs[i]);
	}
	p = put_tlv(c, TAG_ARRAY_FIXED_STRUCT, 0);
	c = put_tlv(p, TAG_ARRAY_FIXED_STRUCT, 8);
	for (i = 0; i < 6; i++) {
		c[i] = 0xff;            /* bssid filter: broadcast */
	}
	p = put_tlv(c + 8, TAG_ARRAY_BYTE, 0);
	s = wmi_cmd(o, m, CMD_START_SCAN, (uint32_t)(p - m));
	o->log("vayu-wifi: w22: start scan passive dwell %u ms, %u channels, max %u ms: %s\n",
	    DWELL_PASSIVE_MS, n, MAX_SCAN_MS, s != 0 ? "FAILED" : "requested");
	if (s == 0) {
		w = o->wait(scan_started, 2000);
		if (w > 0 && !sc.done) {
			w = o->wait(scan_done, SCAN_WAIT_MS);
		}
		if (w <= 0 && !sc.done) {
			o->log("vayu-wifi: w22: scan %s (started %u)\n", w < 0 ? "stopped (CE stage abort)" :
			    "not completed in time", sc.started ? 1 : 0);
		}
		(void)o->wait(NULL, 200);
	}
	for (i = 0; i < sizeof(sc.chan_seen) && ll + 5 < sizeof(list); i++) {
		if (sc.chan_seen[i] != 0) {
			uint32_t v = i, d = v >= 100 ? 3 : v >= 10 ? 2 : 1, k;

			if (ll != 0) {
				list[ll++] = ',';
			}
			for (k = d; k > 0; k--) {
				list[ll + k - 1] = (char)('0' + v % 10);
				v /= 10;
			}
			ll += d;
		}
	}
	list[ll] = 0;
	for (i = 0; i < sc.nbss; i++) {
		total += sc.bss[i].seen;
	}
	o->log("vayu-wifi: w22: scan done: %u beacons, %u unique BSSIDs, channels %s "
	    "(probe_resp %u, other mgmt %u, bad %u, mgmt events %u, scan events %u, end %#x reason %u, "
	    "htt ring %u msgs %u msdus %u reposts %u)\n", sc.beacons, sc.nbss, ll != 0 ? list : "-",
	    sc.probe_resp, sc.other_mgmt, sc.bad, sc.frames, sc.scan_events, sc.done_event, sc.done_reason,
	    sc.ring_n, sc.htt_msgs, sc.htt_msdus, sc.htt_reposts);
	return sc.done && sc.done_event == SCAN_EV_COMPLETED ? 0 : -1;
}
