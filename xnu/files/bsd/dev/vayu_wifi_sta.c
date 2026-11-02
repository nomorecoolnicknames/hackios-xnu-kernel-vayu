/*
 * W23a: open-system AUTH + ASSOC to the owner's AP "Gunwest" after the W22 scan
 * (boot-arg vayu_wifi_join=1, only with vayu_wifi_scan=1).  Ported from ios-flame
 * phone/wifi/radio.c (commit 745c4341): w_channel, w_vdev_start, w_vdev_simple,
 * w_peer, w_ps_off, w_mgmt_tx, w_ap_rates, w_mgmt_hdr, w_send_auth,
 * w_build_assoc_req, sta_mgmt_rx, sta_wmi_event, sta_poll (lines 1180-1884); MGMT_TX
 * and its completion event as Linux ath10k wmi-tlv.c / wmi-tlv.h
 * (wmi_tlv_mgmt_tx_cmd, WMI_TLV_MGMT_TX_COMPLETION_EVENTID 0x7006,
 * WMI_TLV_VDEV_STOPPED_EVENTID 0x5002).
 * Changes: synchronous and bounded; only the beacon with SSID "Gunwest" on
 * channel 64 whose BSSID ends in d3:a0 is a target (nothing is sent otherwise);
 * no PEER_ASSOC / VDEV_UP / EAPOL / keys: after "assoc status 0" the RX is counted
 * for 3 s, then DEAUTH (reason 3), PEER_DELETE, VDEV_STOP.  BSSIDs masked in the log.
 *
 * MIT License
 *
 * Copyright (c) 2026 dipa-studio (ios-flame)
 * Copyright (c) 2026 The Redwood Project (W23a adaptation)
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
#include <sys/vayu_wifi_htc_connect.h>
#include <sys/vayu_wifi_net.h>
#include <sys/vayu_wifi_rsn.h>
#include <sys/vayu_wifi_scan.h>
#include <sys/vayu_wifi_sta.h>
#include <sys/vayu_wifi_smmu.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define EV_VDEV_START_RESP      0x5001U
#define EV_VDEV_STOPPED         0x5002U
#define EV_PEER_STA_KICKOUT     0x6001U
#define EV_MGMT_RX              0x7001U
#define EV_MGMT_TX_COMPL        0x7006U
#define CMD_VDEV_START          0x5003U
#define CMD_VDEV_STOP           0x5006U
#define CMD_VDEV_UP             0x5005U
#define CMD_VDEV_DOWN           0x5007U
#define CMD_PEER_CREATE         0x6001U
#define CMD_PEER_SET_PARAM      0x6004U
#define CMD_PEER_ASSOC          0x6005U
#define CMD_PEER_DELETE         0x6002U
#define CMD_MGMT_TX_SEND        0x7008U
#define CMD_STA_PS_MODE         0x9001U
#define TAG_ARRAY_BYTE          0x11U
#define TAG_ARRAY_STRUCT        0x12U
/* W25f: the firmware's own numbering (ath10k wmi-tlv.h enum wmi_tlv_tag): ARRAY_UINT32=0x11,
 * ARRAY_BYTE=0x12, ARRAY_STRUCT=0x13.  The old 0x10/0x11/0x12 triples are what the scan and
 * mgmt paths already send (the firmware accepted them); the peer assoc below uses the real ones. */
#define TAG_ARRAY_BYTE_W25      0x11U   /* W25g: ath10k enum wmi_tlv_tag: ARRAY_UINT32 16, ARRAY_BYTE 17, ARRAY_STRUCT 18 (the scan path uses these) */
#define TAG_MGMT_RX_HDR         0x2cU
#define TAG_CHANNEL             0x50U
#define TAG_VDEV_START          0x58U
#define TAG_VDEV_UP             0x5cU
#define TAG_VDEV_STOP           0x5dU
#define TAG_VDEV_DOWN           0x5eU
#define TAG_PEER_SET_PARAM      0x64U
#define TAG_PEER_ASSOC          0x65U
#define TAG_VHT_RATE_SET        0x66U
#define TAG_PEER_CREATE         0x61U
#define TAG_PEER_DELETE         0x62U
#define TAG_STA_PS_MODE         0x6cU
#define TAG_MGMT_TX             0x1a6U
#define FC_ASSOC_REQ            0x00U
#define FC_ASSOC_RESP           0x10U
#define FC_REASSOC_RESP         0x30U
#define FC_BEACON               0x80U
#define FC_DISASSOC             0xa0U
#define FC_AUTH                 0xb0U
#define FC_DEAUTH               0xc0U
#define MGMT_TX_FRAME_MAX       64U     /* ath10k WMI_TLV_MGMT_TX_FRAME_MAX_LEN */
#define MGMT_SLOT               512U
#define MGMT_SLOTS              8U
#define MGMT_POOL_LIMIT         ((uint64_t)32 << 30)
#define CREDIT_WAIT_MS          1000U
#define START_WAIT_MS           1500U
#define REPLY_WAIT_MS           400U
#define TRIES                   4U
#define WINDOW_MS               3000U
#define PEER_FLAG_AUTH          0x1U
#define PEER_FLAG_NEED_PTK_4WAY 0x4U
#define RSN_WAIT_MS             8000U   /* the AP's 1/4 retries are ~1 s apart (W24) */
#define NET_WINDOW_MS           20000U  /* the link stays up this long for iOS/configd */
#define KEEP_SECS               180U    /* vayu_wifi_keep=1: teardown after this */
#define TX_LOG_MAX              16U
#define RX_LOG_MAX              8U
#define IE_MAX                  768U
/* the only AP this code may talk to (the owner's network) */
#define TARGET_CHAN             64U
#define TARGET_B4               0xd3U
#define TARGET_B5               0xa0U
static const char target_ssid[] = "Gunwest";
#define TARGET_SSID_LEN         7U

enum { ST_IDLE, ST_START, ST_AUTH, ST_ASSOC, ST_WINDOW, ST_DONE };

uint32_t vayu_sta_mode;

static int (*tlog)(const char *format, ...) __printflike(1, 2);
static struct {
	/* the beacon */
	bool have;
	uint8_t bssid[6];
	uint32_t chan, beacons, mismatch;
	int32_t rssi;
	uint16_t bintval, capab;
	uint32_t ie_len;
	uint8_t ie[IE_MAX];
	/* run */
	uint32_t state;
	const uint8_t *mac;
	bool start_resp, stopped, auth_reply, assoc_reply, ap_left;
	int32_t start_status;
	uint32_t auth_status, assoc_status, aid, assoc_cap, ap_reason, ap_fc;
	uint32_t desc, tx_compl, tx_logged, rx_logged;
	uint64_t mgmt_pa;
	vm_offset_t mgmt_va;
	/* window */
	uint32_t w_mgmt, w_beacons, w_other, w_htt_msgs, w_htt_msdus, w_kickout;
	uint8_t rsn_ie[22];
	bool rsn;
} st;

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

static uint8_t *
put_mac(uint8_t *p, const uint8_t *mac)
{
	uint32_t i;

	for (i = 0; i < 8; i++) {
		p[i] = i < 6 ? mac[i] : 0;
	}
	return p + 8;
}

static void
zero(void *b, uint32_t n)
{
	uint8_t *p = b;

	while (n-- != 0) {
		*p++ = 0;
	}
}

static bool
same6(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	for (i = 0; i < 6; i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

static const uint8_t *
find_ie(const uint8_t *ies, uint32_t len, uint8_t id)
{
	uint32_t o;

	for (o = 0; o + 2 <= len && o + 2 + ies[o + 1] <= len; o += 2 + ies[o + 1]) {
		if (ies[o] == id) {
			return ies + o;
		}
	}
	return NULL;
}

void
vayu_sta_reset(int (*log)(const char *format, ...))
{
	uint32_t mode = 0;

	zero(&st, (uint32_t)sizeof(st));
	tlog = log;
	vayu_sta_mode = 0;
	PE_parse_boot_argn("vayu_wifi_join", &mode, sizeof(mode));
	if (mode == 0 || tlog == NULL) {
		return;
	}
	vayu_sta_mode = (mode == 1 && vayu_scan_mode != 0) ? 1 : 0;
	vayu_rsn_reset(log);
	vayu_net_reset(log);
	tlog("vayu-wifi: w23: vayu_wifi_join=%u%s: open auth + assoc to '%s' 5 GHz (any ch, was %u) bssid xx:xx:xx:xx:%02x:%02x "
	    "after the scan, %u ms rx count, deauth; no EAPOL/keys\n", mode,
	    vayu_sta_mode != 0 ? "" : (mode != 1 ? " (unknown, off)" : " (needs vayu_wifi_scan=1, off)"),
	    target_ssid, TARGET_CHAN, TARGET_B4, TARGET_B5, WINDOW_MS);
}

/* ------------------------------------------------------------ RX */

/* beacon / probe response during the scan: keep the target's elements */
static void
capture(const uint8_t *f, uint32_t flen, uint32_t chan, int32_t rssi)
{
	const uint8_t *s, *ht;
	uint32_t i, n;

	if (flen < 36) {
		return;
	}
	s = find_ie(f + 36, flen - 36, 0);
	if (s == NULL || s[1] != TARGET_SSID_LEN) {
		return;
	}
	for (i = 0; i < TARGET_SSID_LEN; i++) {
		if (s[2 + i] != (uint8_t)target_ssid[i]) {
			return;
		}
	}
	ht = find_ie(f + 36, flen - 36, 61);
	if (ht != NULL && ht[1] >= 1) {
		chan = ht[2];           /* HT operation: primary channel */
	}
	if (f[20] != TARGET_B4 || f[21] != TARGET_B5 || chan < 36U || chan > 177U) {
		if (st.mismatch++ < 2) {
			tlog("vayu-wifi: w23: '%s' heard on ch %u bssid xx:xx:xx:xx:%02x:%02x: not the owner's "
			    "AP (want 5 GHz, ..%02x:%02x); ignored\n", target_ssid, chan, f[20], f[21],
			    TARGET_B4, TARGET_B5);
		}
		return;
	}
	if (st.have && !same6(st.bssid, f + 16)) {
		return;                 /* the first matching BSSID only */
	}
	st.beacons++;
	if (st.have && rssi <= st.rssi) {
		return;
	}
	for (i = 0; i < 6; i++) {
		st.bssid[i] = f[16 + i];
	}
	st.chan = chan;
	st.rssi = rssi;
	st.bintval = (uint16_t)rd16(f + 32);
	st.capab = (uint16_t)rd16(f + 34);
	n = flen - 36 < IE_MAX ? flen - 36 : IE_MAX;
	for (i = 0; i < n; i++) {
		st.ie[i] = f[36 + i];
	}
	st.ie_len = n;
	st.have = true;
}

static void
frame_rx(const uint8_t *f, uint32_t flen)
{
	uint32_t fc = f[0] & 0xfcU;

	if (st.state == ST_WINDOW) {
		st.w_mgmt++;
		if (fc == FC_BEACON) {
			st.w_beacons++;
		} else {
			st.w_other++;
		}
	}
	if (fc != FC_BEACON && st.rx_logged < RX_LOG_MAX) {
		st.rx_logged++;
		tlog("vayu-wifi: w23: rx mgmt fc %#x len %u from the AP (state %u)\n", f[0], flen, st.state);
	}
	if (flen < 26 || (fc != FC_DEAUTH && fc != FC_DISASSOC && !same6(f + 4, st.mac))) {
		return;
	}
	if (fc == FC_AUTH && st.state == ST_AUTH && flen >= 30 && rd16(f + 26) == 2) {
		st.auth_status = rd16(f + 28);
		st.auth_reply = true;
		tlog("vayu-wifi: w23: auth status %u (alg %u seq 2)\n", st.auth_status, rd16(f + 24));
	} else if ((fc == FC_ASSOC_RESP || fc == FC_REASSOC_RESP) && st.state == ST_ASSOC && flen >= 30) {
		st.assoc_cap = rd16(f + 24);
		st.assoc_status = rd16(f + 26);
		st.aid = rd16(f + 28) & 0x3fffU;
		st.assoc_reply = true;
		tlog("vayu-wifi: w23: assoc status %u aid %u (cap %#x, %u bytes)\n", st.assoc_status, st.aid,
		    st.assoc_cap, flen);
	} else if ((fc == FC_DEAUTH || fc == FC_DISASSOC) && st.state != ST_IDLE) {
		st.ap_left = true;
		st.ap_fc = fc;
		st.ap_reason = rd16(f + 24);
		tlog("vayu-wifi: w23: the AP sent %s reason %u (state %u)\n",
		    fc == FC_DEAUTH ? "DEAUTH" : "DISASSOC", st.ap_reason, st.state);
	}
}

static void
mgmt_rx(const uint8_t *t, uint32_t len)
{
	const uint8_t *hdr = NULL, *f = NULL;
	uint32_t o, l, tag, flen = 0;

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
	if (hdr == NULL || f == NULL) {
		return;
	}
	if (rd32(hdr + 16) < flen) {
		flen = rd32(hdr + 16);
	}
	if (flen < 24 || (f[0] & 0x0cU) != 0) {
		return;
	}
	if (st.state == ST_IDLE) {
		if ((f[0] & 0xf0U) == 0x80U || (f[0] & 0xf0U) == 0x50U) {
			capture(f, flen, rd32(hdr), (int32_t)rd32(hdr + 4) - 95);
		}
		return;
	}
	if (st.have && same6(f + 10, st.bssid)) {
		frame_rx(f, flen);
	}
}

static uint32_t
first_tlv(const uint8_t *p, uint32_t len, uint32_t *v, uint32_t n)
{
	uint32_t l, i;

	if (len < 4) {
		return 0;
	}
	l = rd16(p);
	if (len < 4 + l) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		v[i] = 4 * i + 4 <= l ? rd32(p + 4 + 4 * i) : 0;
	}
	return l;
}

void
vayu_sta_wmi_rx(uint32_t id, const uint8_t *p, uint32_t len)
{
	uint32_t v[5], l;

	if (vayu_sta_mode == 0 || tlog == NULL) {
		return;
	}
	switch (id) {
	case EV_MGMT_RX:
		mgmt_rx(p, len);
		break;
	case EV_VDEV_START_RESP:      /* {vdev, req, type, status} */
		l = first_tlv(p, len, v, 4);
		if (l >= 16 && st.state == ST_START) {
			st.start_status = (int32_t)v[3];
			st.start_resp = true;
		}
		tlog("vayu-wifi: w23: VDEV_START_RESP vdev %u req %u type %u status %d\n", v[0], v[1], v[2],
		    (int32_t)v[3]);
		break;
	case EV_VDEV_STOPPED:
		(void)first_tlv(p, len, v, 1);
		st.stopped = true;
		tlog("vayu-wifi: w23: VDEV_STOPPED vdev %u\n", v[0]);
		break;
	case EV_MGMT_TX_COMPL:        /* {desc_id, status, pdev_id, ppdu_id, ack_rssi} */
		(void)first_tlv(p, len, v, 5);
		st.tx_compl++;
		if (st.tx_logged < TX_LOG_MAX) {
			st.tx_logged++;
			tlog("vayu-wifi: w23: mgmt tx completion desc %u status %u (0 ok, 1 discard, 2 inspect, "
			    "3 no ack) ack_rssi %u\n", v[0], v[1], v[4]);
		}
		break;
	case 0x5003U: {               /* VDEV_INSTALL_KEY_COMPLETE: {vdev, key_idx, key_flags, status[, mac]} */
		uint32_t n = first_tlv(p, len, v, 4);

		if (n >= 16) {
			tlog("vayu-wifi: W25 wmi: install-key-complete v0 %u v1 %u v2 %u v3 %u (vdev, key idx, "
			    "flags, status as far as the layout is known)\n", v[0], v[1], v[2], v[3]);
		}
		break;
	}
	case EV_PEER_STA_KICKOUT:
		st.w_kickout++;
		tlog("vayu-wifi: w23: PEER_STA_KICKOUT (state %u)\n", st.state);
		break;
	default:
		break;
	}
}

void
vayu_sta_htt_rx(const uint8_t *p, uint32_t len)
{
	if (vayu_sta_mode == 0 || st.state != ST_WINDOW || len == 0) {
		return;
	}
	st.w_htt_msgs++;
	vayu_scan_note_htt(p, len);
	if (p[0] == 0x12U && len >= 8) {        /* RX_IN_ORD_PADDR_IND: msdu count at +6 */
		st.w_htt_msdus += rd16(p + 6);
	}
}

/* ------------------------------------------------------------ TX */

static int
wmi_cmd(const struct vayu_scan_ops *o, uint8_t *m, uint32_t cmd, uint32_t len)
{
	(void)put32(m, cmd);
	return o->wmi_send(m, len, CREDIT_WAIT_MS);
}

/* one page below 32 GiB, identity-mapped in the WLAN SMMU context (as the W22b HTT pool) */
static bool
mgmt_pool(const struct vayu_scan_ops *o)
{
	vm_offset_t kva = 0;
	kern_return_t kr;
	uint64_t pa;

	kr = kmem_alloc_contig(kernel_map, &kva, PAGE_SIZE, PAGE_MASK,
	    (ppnum_t)((MGMT_POOL_LIMIT >> PAGE_SHIFT) - 1), 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	if (kr != KERN_SUCCESS || kva == 0) {
		o->log("vayu-wifi: w23: mgmt pool: no page below 32 GiB (%d)\n", kr);
		return false;
	}
	FlushPoC_DcacheRegion(kva, PAGE_SIZE);
	__asm__ volatile("dsb sy" ::: "memory");
	pa = kvtophys(kva) & ~((uint64_t)PAGE_MASK);
	if (pa == 0 || pa + PAGE_SIZE > MGMT_POOL_LIMIT ||
	    !vayu_wifi_smmu_map_dma(pa, PAGE_SIZE, "w23 mgmt") ||
	    !vayu_wifi_smmu_check_range(pa, PAGE_SIZE, "w23 mgmt")) {
		o->log("vayu-wifi: w23: mgmt pool PA %#llx unusable; kept, not used\n", (unsigned long long)pa);
		return false;
	}
	st.mgmt_va = ml_io_map_unmappable((vm_offset_t)pa, PAGE_SIZE, VM_WIMG_WCOMB);
	if (st.mgmt_va == 0) {
		o->log("vayu-wifi: w23: mgmt pool PA %#llx: cannot map non-cacheable\n", (unsigned long long)pa);
		return false;
	}
	st.mgmt_pa = pa;
	o->log("vayu-wifi: w23: mgmt pool PA %#llx (%u slots x %u bytes)\n", (unsigned long long)pa,
	    MGMT_SLOTS, MGMT_SLOT);
	return true;
}

/* flame w_mgmt_tx: frame in DMA memory, first 64 bytes inline, WMI MGMT_TX_SEND on vdev 0 */
static int
mgmt_tx(const struct vayu_scan_ops *o, const uint8_t *f, uint32_t len, const char *what)
{
	static uint8_t m[4 + 4 + 28 + 4 + MGMT_TX_FRAME_MAX];
	uint32_t buf = ((len < MGMT_TX_FRAME_MAX ? len : MGMT_TX_FRAME_MAX) + 3) & ~3U, i, slot;
	volatile uint8_t *d;
	uint8_t *p;
	int rc;

	if (st.mgmt_va == 0 || len < 24 || len > MGMT_SLOT - 4 || !same6(f + 4, st.bssid)) {
		return -1;              /* addr1 must be the target AP */
	}
	st.desc++;
	slot = st.desc % MGMT_SLOTS;
	d = (volatile uint8_t *)(st.mgmt_va + slot * MGMT_SLOT);
	for (i = 0; i < MGMT_SLOT; i++) {
		d[i] = i < len ? f[i] : 0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	zero(m, sizeof(m));
	p = put_tlv(m + 4, TAG_MGMT_TX, 28);
	p = put32(p, 0);                /* vdev */
	p = put32(p, st.desc);
	p = put32(p, 0);                /* chanfreq: the vdev's */
	p = put64(p, st.mgmt_pa + slot * MGMT_SLOT);
	p = put32(p, len);
	p = put32(p, buf);
	p = put_tlv(p, TAG_ARRAY_BYTE, buf);
	for (i = 0; i < buf; i++) {
		p[i] = i < len ? f[i] : 0;
	}
	rc = wmi_cmd(o, m, CMD_MGMT_TX_SEND, 4 + 4 + 28 + 4 + buf);
	o->log("vayu-wifi: w23: mgmt tx %s desc %u %u bytes to xx:xx:xx:xx:%02x:%02x: %s\n", what, st.desc,
	    len, st.bssid[4], st.bssid[5], rc != 0 ? "FAILED" : "queued");
	return rc;
}

static uint8_t *
mgmt_hdr(uint8_t *f, uint8_t fc)
{
	uint32_t i;

	zero(f, 24);
	f[0] = fc;
	for (i = 0; i < 6; i++) {
		f[4 + i] = st.bssid[i];
		f[10 + i] = st.mac[i];
		f[16 + i] = st.bssid[i];
	}
	return f + 24;
}

static int
send_auth(const struct vayu_scan_ops *o)
{
	uint8_t f[30], *b = mgmt_hdr(f, FC_AUTH);

	zero(b, 6);
	b[2] = 1;                       /* open system, transaction 1 */
	return mgmt_tx(o, f, sizeof(f), "AUTH");
}

static int
send_deauth(const struct vayu_scan_ops *o)
{
	uint8_t f[26], *b = mgmt_hdr(f, FC_DEAUTH);

	(void)put16(b, 3);              /* reason 3: deauthenticated because sending STA is leaving */
	return mgmt_tx(o, f, sizeof(f), "DEAUTH reason 3");
}

static uint32_t
ap_rates(uint8_t *out)
{
	static const uint8_t ids[2] = { 1, 50 };
	uint32_t n = 0, k, i;

	for (k = 0; k < 2; k++) {
		const uint8_t *ie = find_ie(st.ie, st.ie_len, ids[k]);

		for (i = 0; ie != NULL && i < ie[1] && n < 16; i++) {
			out[n++] = ie[2 + i];
		}
	}
	return n;
}

static uint32_t
build_assoc(uint8_t *f)
{
	uint8_t *p = mgmt_hdr(f, FC_ASSOC_REQ), rates[16];
	uint32_t n = ap_rates(rates), cap = 0x0001U | 0x0020U | 0x0400U, k;

	if (st.rsn) {
		cap |= 0x0010U;
	}
	p = put16(p, cap);
	p = put16(p, 10);               /* listen interval */
	*p++ = 0;
	*p++ = TARGET_SSID_LEN;
	for (k = 0; k < TARGET_SSID_LEN; k++) {
		*p++ = (uint8_t)target_ssid[k];
	}
	*p++ = 1;
	*p++ = (uint8_t)(n < 8 ? n : 8);
	for (k = 0; k < n && k < 8; k++) {
		*p++ = rates[k];
	}
	if (n > 8) {
		*p++ = 50;
		*p++ = (uint8_t)(n - 8);
		for (k = 8; k < n; k++) {
			*p++ = rates[k];
		}
	}
	if (st.rsn) {
		for (k = 0; k < sizeof(st.rsn_ie); k++) {
			*p++ = st.rsn_ie[k];
		}
	}
	return (uint32_t)(p - f);
}

/* ------------------------------------------------------------ RSN */

static void
cat(char *b, uint32_t cap, const char *s)
{
	uint32_t n = 0;

	while (n < cap - 1 && b[n] != 0) {
		n++;
	}
	if (n != 0 && n < cap - 1) {
		b[n++] = ',';
	}
	while (n < cap - 1 && *s != 0) {
		b[n++] = *s++;
	}
	b[n] = 0;
}

static const char *
cipher_name(const uint8_t *s)
{
	static const char *const n[] = { "USE-GROUP", "WEP40", "TKIP", "RSV3", "CCMP", "WEP104", "BIP-CMAC",
		                         "NO-GROUP", "GCMP", "GCMP256", "CCMP256", "BIP-GMAC", "BIP-GMAC256",
		                         "BIP-CMAC256" };

	if (s[0] != 0x00 || s[1] != 0x0f || s[2] != 0xac) {
		return "vendor";
	}
	return s[3] < sizeof(n) / sizeof(n[0]) ? n[s[3]] : "other";
}

static const char *
akm_name(const uint8_t *s)
{
	if (s[0] != 0x00 || s[1] != 0x0f || s[2] != 0xac) {
		return "vendor";
	}
	switch (s[3]) {
	case 1: return "8021X";
	case 2: return "PSK";
	case 3: return "FT-8021X";
	case 4: return "FT-PSK";
	case 5: return "8021X-SHA256";
	case 6: return "PSK-SHA256";
	case 8: return "SAE";
	case 9: return "FT-SAE";
	case 18: return "OWE";
	case 24: return "SAE-EXT";
	default: return "other";
	}
}

/* 0 = go (PSK + CCMP or open), -1 = stop */
static int
rsn_parse(const struct vayu_scan_ops *o)
{
	const uint8_t *ie = find_ie(st.ie, st.ie_len, 48), *b, *e;
	char akm[64], pw[48];
	uint32_t i, np, na, caps = 0, our_caps;
	bool psk = false, sae = false, ccmp = false;
	uint8_t group = 4, *q;

	if (ie == NULL) {
		o->log("vayu-wifi: w23: rsn none (privacy %u): open system, no RSN IE in the assoc request\n",
		    (st.capab & 0x10U) != 0 ? 1 : 0);
		st.rsn = false;
		return 0;
	}
	b = ie + 2;
	e = b + ie[1];
	akm[0] = pw[0] = 0;
	if (b + 2 + 4 + 2 > e || rd16(b) != 1) {
		o->log("vayu-wifi: w23: rsn ie len %u version %u: malformed, stop\n", ie[1],
		    b + 2 <= e ? rd16(b) : 0);
		return -1;
	}
	group = b[5];
	b += 2 + 4;
	np = rd16(b);
	b += 2;
	for (i = 0; i < np && b + 4 <= e; i++, b += 4) {
		cat(pw, sizeof(pw), cipher_name(b));
		if (b[0] == 0x00 && b[1] == 0x0f && b[2] == 0xac && b[3] == 4) {
			ccmp = true;
		}
	}
	na = b + 2 <= e ? rd16(b) : 0;
	b += 2;
	for (i = 0; i < na && b + 4 <= e; i++, b += 4) {
		cat(akm, sizeof(akm), akm_name(b));
		if (b[0] == 0x00 && b[1] == 0x0f && b[2] == 0xac) {
			psk |= b[3] == 2;
			sae |= b[3] == 8 || b[3] == 9 || b[3] == 24;
		}
	}
	if (b + 2 <= e) {
		caps = rd16(b);
	}
	o->log("vayu-wifi: w23: rsn akm %s pairwise %s group %s caps %#06x (mfpc %u mfpr %u)\n",
	    akm[0] != 0 ? akm : "-", pw[0] != 0 ? pw : "-", cipher_name(ie + 2 + 2), caps,
	    (caps >> 7) & 1U, (caps >> 6) & 1U);
	if (!psk && sae) {
		o->log("vayu-wifi: w23: WPA3-only, stop\n");
		return -1;
	}
	if (!psk || !ccmp) {
		o->log("vayu-wifi: w23: no WPA2-PSK/CCMP (psk %u ccmp %u), stop\n", psk ? 1 : 0, ccmp ? 1 : 0);
		return -1;
	}
	/* flame w_join_begin: RSN v1, the AP's group cipher, pairwise CCMP, AKM PSK; MFP only if required */
	our_caps = (caps & 0x40U) != 0 ? 0xc0U : 0;
	q = st.rsn_ie;
	*q++ = 48;
	*q++ = 20;
	q = put16(q, 1);
	*q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = group;
	q = put16(q, 1);
	*q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = 4;
	q = put16(q, 1);
	*q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = 2;
	(void)put16(q, our_caps);
	st.rsn = true;
	o->log("vayu-wifi: w23: assoc rsn: group %s pairwise CCMP akm PSK caps %#06x\n",
	    cipher_name(st.rsn_ie + 4), our_caps);
	return 0;
}

/* ------------------------------------------------------------ the join */

static bool
start_done(void)
{
	return st.start_resp;
}

static bool
auth_done(void)
{
	return st.auth_reply || st.ap_left;
}

static bool
assoc_done(void)
{
	return st.assoc_reply || st.ap_left;
}

static bool
stop_done(void)
{
	return st.stopped;
}

static uint32_t
join_freq(void)
{
	if (st.chan <= 13U) return 2407U + st.chan * 5U;
	if (st.chan == 14U) return 2484U;
	return 5000U + st.chan * 5U;
}

static int
vdev_start(const struct vayu_scan_ops *o)
{
	static uint8_t m[4 + 4 + 72 + 4 + 24 + 4];
	const uint8_t *tim = find_ie(st.ie, st.ie_len, 5);
	uint32_t bcn = st.bintval != 0 ? st.bintval : 100, dtim = 1;
	uint8_t *p, *c;

	if (tim != NULL && tim[1] >= 2 && tim[3] != 0) {
		dtim = tim[3];
	}
	zero(m, sizeof(m));
	p = put_tlv(m + 4, TAG_VDEV_START, 72);
	p = put32(p, 0);                /* vdev */
	p = put32(p, 0);                /* requestor */
	p = put32(p, bcn);
	p = put32(p, dtim);
	p = put32(p, 0);                /* flags */
	p += 36 + 16;                   /* ssid (empty for a station), tx rate/power, noa, hw ack */
	c = put_tlv(p, TAG_CHANNEL, 24);        /* flame w_channel: 11A, no passive/DFS flag (STA) */
	c = put32(c, join_freq());
	c = put32(c, join_freq());
	c = put32(c, 0);
	c = put32(c, 0);                /* MODE_11A */
	c = put32(c, (40U << 8) | (40U << 16));
	c = put32(c, 40U << 8);
	p = put_tlv(c, TAG_ARRAY_STRUCT, 0);
	o->log("vayu-wifi: w23: VDEV_START vdev 0 ch %u bcn %u dtim %u\n", st.chan, bcn, dtim);
	return wmi_cmd(o, m, CMD_VDEV_START, (uint32_t)(p - m));
}

static int
vdev_simple(const struct vayu_scan_ops *o, uint32_t cmd, uint32_t tag)
{
	uint8_t m[12], *p = put_tlv(m + 4, tag, 4);

	(void)put32(p, 0);
	return wmi_cmd(o, m, cmd, sizeof(m));
}


/* W25: PEER_ASSOC with the RSN IE and the 4-way flag (ath10k wmi-tlv, flame radio.c w_peer_assoc) */
static int
peer_assoc(const struct vayu_scan_ops *o, uint32_t caps)
{
	uint8_t m[200], rates[16];
	uint32_t n = ap_rates(rates), rl = ((n < 16 ? n : 16) + 3U) & ~3U, i;
	uint8_t *p, *r;

	zero(m, sizeof(m));
	p = put_tlv(m + 4, TAG_PEER_ASSOC, 76);
	p = put_mac(p, st.bssid);
	p = put32(p, 0);                        /* peer type */
	p = put32(p, 1);                        /* new association */
	p = put32(p, st.aid);
	p = put32(p, PEER_FLAG_AUTH | (st.rsn ? PEER_FLAG_NEED_PTK_4WAY : 0));
	p = put32(p, caps);
	p = put32(p, 10);                       /* listen interval */
	p += 16;                                /* ht caps, max mpdu, density, rate caps */
	p = put32(p, 1);                        /* nss */
	p = put32(p, 0);                        /* vht caps */
	p = put32(p, join_freq() < 4000U ? 1U : 0U);
	p += 8;
	p = put32(p, n < 16 ? n : 16);
	p = put32(p, 0);
	r = put_tlv(p, TAG_ARRAY_BYTE_W25, rl);
	for (i = 0; i < n && i < 16; i++) {
		uint8_t v = rates[i] & 0x7fU;

		r[i] = (v == 2 || v == 4 || v == 11 || v == 22) ? (uint8_t)(v | 0x80U) : v;
	}
	p = r + rl;
	p = put_tlv(p, TAG_ARRAY_BYTE_W25, 0);          /* no HT rates (no HT in our assoc request) */
	/* wmi_vht_rate_set is not optional: ath10k always appends it, zeros when VHT is not in use.
	 * W25e sent the TLV header alone (len 16, no data) and the firmware crashed on it. */
	p = put_tlv(p, TAG_VHT_RATE_SET, 16);
	p = put32(p, 0);                                /* rx_max_rate */
	p = put32(p, 0);                                /* rx_mcs_set */
	p = put32(p, 0);                                /* tx_max_rate */
	p = put32(p, 0);                                /* tx_mcs_set (W25h: p must move: the length is p - m) */
	o->log("vayu-wifi: W25 S2 peer_assoc: OK %u bytes, %u legacy rates, 0 ht rates, vht_rate_set 16, "
	    "flags %#x caps %#x phymode %u aid %u\n", (uint32_t)(p - m), n < 16 ? n : 16,
	    PEER_FLAG_AUTH | (st.rsn ? PEER_FLAG_NEED_PTK_4WAY : 0), caps, join_freq() < 4000U ? 1U : 0U,
	    st.aid);
	return wmi_cmd(o, m, CMD_PEER_ASSOC, (uint32_t)(p - m));
}

static int
vdev_up(const struct vayu_scan_ops *o)
{
	uint8_t m[4 + 4 + 16], *p = put_tlv(m + 4, TAG_VDEV_UP, 16);

	p = put32(p, 0);
	p = put32(p, st.aid);
	(void)put_mac(p, st.bssid);
	return wmi_cmd(o, m, CMD_VDEV_UP, sizeof(m));
}

static int
peer(const struct vayu_scan_ops *o, bool create)
{
	uint8_t m[4 + 4 + 16], *p;

	zero(m, sizeof(m));
	p = put_tlv(m + 4, create ? TAG_PEER_CREATE : TAG_PEER_DELETE, create ? 16 : 12);
	p = put32(p, 0);
	p = put_mac(p, st.bssid);
	if (create) {
		(void)put32(p, 0);      /* peer type default */
	}
	return wmi_cmd(o, m, create ? CMD_PEER_CREATE : CMD_PEER_DELETE, create ? 24 : 20);
}

static int
ps_off(const struct vayu_scan_ops *o)
{
	uint8_t m[4 + 12], *p = put_tlv(m + 4, TAG_STA_PS_MODE, 8);

	p = put32(p, 0);
	(void)put32(p, 0);
	return wmi_cmd(o, m, CMD_STA_PS_MODE, sizeof(m));
}

int
vayu_sta_run(const struct vayu_scan_ops *o)
{
	static uint8_t assoc[256];
	uint32_t alen = 0, tries, waited;
	bool started = false, peered = false, authed = false, associated = false, up = false;
	int w = 0, rc = -1;

	if (o == NULL || vayu_sta_mode == 0) {
		return -1;
	}
	st.mac = o->mac;
	vayu_net_set_tx(vayu_scan_tx_frame, o->log);
	vayu_net_sta_attach(o->mac);                  /* W25o: Apple80211 sees a powered station early */
	if (!st.have) {
		o->log("vayu-wifi: W25 S2 join: ERROR no beacon from '%s' (ch %u) bssid ..%02x:%02x in the scan\n",
		    target_ssid, TARGET_CHAN, TARGET_B4, TARGET_B5);
		return -1;
	}
	o->log("vayu-wifi: W25 S2 join: OK target '%s' bssid xx:xx:xx:xx:%02x:%02x ch %u rssi %d bcn %u cap %#x ies %u "
	    "(%u beacons)\n", target_ssid, st.bssid[4], st.bssid[5], st.chan, st.rssi, st.bintval, st.capab,
	    st.ie_len, st.beacons);
	if (rsn_parse(o) != 0 || !mgmt_pool(o)) {
		o->log("vayu-wifi: W25 S2 join: ERROR rsn/mgmt pool\n");
		return -1;
	}
	alen = build_assoc(assoc);
	(void)ps_off(o);
	st.state = ST_START;
	if (vdev_start(o) != 0) {
		o->log("vayu-wifi: W25 S2 join: ERROR VDEV_START not sent\n");
		st.state = ST_DONE;
		return -1;
	}
	started = true;
	w = o->wait(start_done, START_WAIT_MS);
	if (w < 0) {
		goto stopped;
	}
	if (!st.start_resp || st.start_status != 0) {
		o->log("vayu-wifi: W25 S2 join: ERROR vdev start %s (status %d)\n",
		    st.start_resp ? "refused" : "no response", st.start_status);
		goto teardown;
	}
	peered = peer(o, true) == 0;
	if (!peered) {
		o->log("vayu-wifi: W25 S2 join: ERROR PEER_CREATE\n");
		goto teardown;
	}
	st.state = ST_AUTH;
	for (tries = 1; tries <= TRIES && !st.auth_reply && !st.ap_left; tries++) {
		(void)send_auth(o);
		w = o->wait(auth_done, REPLY_WAIT_MS);
		if (w < 0) {
			goto stopped;
		}
	}
	if (!st.auth_reply || st.auth_status != 0) {
		o->log("vayu-wifi: W25 S2 join: ERROR auth %s status %u\n", st.auth_reply ? "reply" : "none",
		    st.auth_status);
		goto teardown;
	}
	authed = true;
	st.state = ST_ASSOC;
	for (tries = 1; tries <= TRIES && !st.assoc_reply && !st.ap_left; tries++) {
		(void)mgmt_tx(o, assoc, alen, "ASSOC_REQ");
		w = o->wait(assoc_done, REPLY_WAIT_MS);
		if (w < 0) {
			goto stopped;
		}
	}
	if (!st.assoc_reply || st.assoc_status != 0) {
		o->log("vayu-wifi: W25 S2 join: ERROR assoc %s status %u\n", st.assoc_reply ? "reply" : "none",
		    st.assoc_status);
		goto teardown;
	}
	associated = true;
	o->log("vayu-wifi: W25 S2 join: OK assoc status 0 aid %u (auth 0)\n", st.aid);
	if (peer_assoc(o, st.assoc_cap) != 0) {
		o->log("vayu-wifi: W25 S2 join: ERROR PEER_ASSOC not sent\n");
		goto teardown;
	}
	if (vdev_up(o) != 0) {
		o->log("vayu-wifi: W25 S2 join: ERROR VDEV_UP not sent\n");
		goto teardown;
	}
	o->log("vayu-wifi: W25 S2 join: OK PEER_ASSOC (%s) + VDEV_UP sent\n", st.rsn ? "rsn, 4way" : "open");

	/* W25 S3: the 4-way handshake, driven by the EAPOL frames the HTT rx path hands to vayu_wifi_rsn.c */
	if (st.rsn) {
		struct vayu_rsn_ops rops;

		if (!vayu_rsn_armed()) {
			o->log("vayu-wifi: W25 S3 4way: ERROR no usable PMK (vayu_wifi_pmk_file)\n");
			goto teardown;
		}
		zero(&rops, (uint32_t)sizeof(rops));
		rops.log = o->log;
		rops.wmi_send = o->wmi_send;
		rops.tx_data = vayu_scan_tx_frame;
		rops.mac = st.mac;
		rops.bssid = st.bssid;
		rops.rsn_ie = st.rsn_ie;
		rops.rsn_ie_len = (uint32_t)sizeof(st.rsn_ie);
		rops.group_cipher = st.rsn_ie[7] == 2 ? 2U : 4U;
		vayu_rsn_start(&rops);
		waited = 0;
		while (waited < RSN_WAIT_MS) {
			int r = vayu_rsn_poll();

			if (r <= 0) {
				break;
			}
			if (o->wait(NULL, 100) < 0) {
				goto stopped;
			}
			waited += 100;
		}
		if (!vayu_rsn_keyed()) {
			o->log("vayu-wifi: W25 S3 4way: ERROR no keys in %u ms\n", RSN_WAIT_MS);
			goto teardown;
		}
	} else {
		o->log("vayu-wifi: W25 S3 4way: OK not needed (open network)\n");
	}

	/* W25 S4: en0 over the link */
	if (vayu_net_start(st.mac, st.bssid, (const uint8_t *)target_ssid, TARGET_SSID_LEN, st.chan,
	    st.rssi) != 0) {
		goto teardown;
	}
	up = true;
	rc = 0;

	/* W25 S5/S6: let iOS see the link, and prove the path with our own DHCP when asked */
	waited = 0;
	for (;;) {
		bool stopped_now = false;
		int d = vayu_net_poll();

		(void)d;
		if (o->wait(NULL, 100) < 0) {
			stopped_now = true;
		}
		waited += 100;
		if (stopped_now) {
			break;
		}
		if (vayu_net_keep()) {
			if (waited >= KEEP_SECS * 1000U) {
				break;
			}
			if (waited % 10000U == 0) {
				vayu_scan_tx_stats(vayu_htcc_credits(), vayu_net_tx_queue_depth());
			}
			if (waited % 30000U == 0) {
				vayu_net_summary();
			}
		} else if (waited >= NET_WINDOW_MS) {
			break;
		}
	}
	vayu_net_summary();

teardown:
	st.state = ST_DONE;
	if (up) {
		vayu_net_stop();
	}
	vayu_rsn_stop();
	if ((authed || associated) && !st.ap_left) {
		(void)send_deauth(o);
		if (o->wait(NULL, 200) < 0) {
			goto stopped;
		}
	}
	if (associated) {
		(void)vdev_simple(o, CMD_VDEV_DOWN, TAG_VDEV_DOWN);
		(void)o->wait(NULL, 50);
	}
	if (peered) {
		o->log("vayu-wifi: W25 S2 join: PEER_DELETE %s\n", peer(o, false) == 0 ? "ok" : "FAILED");
	}
	if (started) {
		o->log("vayu-wifi: W25 S2 join: VDEV_STOP %s\n",
		    vdev_simple(o, CMD_VDEV_STOP, TAG_VDEV_STOP) == 0 ? "sent" : "FAILED");
		if (o->wait(stop_done, 1000) < 0) {
			goto stopped;
		}
	}
	if (rc == 0) {
		o->log("vayu-wifi: W25 S2 join: OK done (auth %u, assoc %u aid %u, mgmt tx %u completions %u)\n",
		    st.auth_status, st.assoc_status, st.aid, st.desc, st.tx_compl);
	}
	return rc;
stopped:
	st.state = ST_DONE;
	o->log("vayu-wifi: W25 join: ERROR stopped (CE stage abort)\n");
	return -1;
}
