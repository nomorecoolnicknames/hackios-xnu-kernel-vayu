/*
 * W25: the WPA2-PSK 4-way handshake of the vayu station (see sys/vayu_wifi_rsn.h).
 *
 * The PMK is read from /redwood/etc/vayu-wifi/gunwest.pmk (the payload image, md1) (boot-arg vayu_wifi_pmk_file=1), never
 * from a boot argument: boot arguments are logged by the loader, the kernel, launchd and panics.
 *
 * Flow: message 1/4 (pairwise, ack, no MIC) -> SNonce random, PTK = PRF-384(PMK, "Pairwise key
 * expansion", AA|SPA|ANonce|SNonce ordered the standard's way) -> 2/4 with the RSN IE of our
 * assoc request and a HMAC-SHA1-128 MIC under the KCK.  Message 3/4 -> MIC checked, GTK
 * unwrapped with the KEK (RFC 3394), 4/4 sent in the clear, WMI_VDEV_INSTALL_KEY for the
 * pairwise TK (index 0) and the group key, then WMI_PEER_SET_PARAM AUTHORIZE.
 *
 * Layouts are ath10k's wmi-tlv (wmi.h: struct wmi_vdev_install_key_cmd, wmi_peer_set_param_cmd)
 * and EAPOL-Key as IEEE 802.1X-2004 7.6 / 8.5.  Nothing secret is printed: no PMK, PTK, KCK,
 * KEK, TK, GTK or nonce ever reaches the log; the addresses are masked the way the rest of the
 * Wi-Fi lines mask them.
 *
 * MIT License.  Copyright (c) 2026 The Redwood Project (W25 adaptation).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/kauth.h>
#include <sys/random.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vayu_wifi_rsn.h>
#include <libkern/crypto/crypto_internal.h>
#include <corecrypto/ccmode.h>

/* ---- EAPOL-Key (802.1X-2004) ------------------------------------------------------------------ */

#define EAPOL_TYPE_KEY          3U
#define KI_PAIRWISE             0x0008U
#define KI_INSTALL              0x0040U
#define KI_ACK                  0x0080U
#define KI_MIC                  0x0100U
#define KI_SECURE               0x0200U
#define KI_ENC_KD               0x1000U
#define EAPOL_KEY_FIXED         95U     /* the Key Descriptor body without key data */
#define EAPOL_HDR               4U

/* ---- WMI -------------------------------------------------------------------------------------- */

#define CMD_VDEV_INSTALL_KEY    0x5009U
#define CMD_PEER_SET_PARAM      0x6004U
#define TAG_VDEV_INSTALL_KEY    0x60U
#define TAG_PEER_SET_PARAM      0x64U
#define PEER_PARAM_AUTHORIZE    3U
#define WMI_CIPHER_TKIP         2U
#define WMI_CIPHER_CCMP         4U

#define PMK_PATH                "/redwood/etc/vayu-wifi/gunwest.pmk"
#define PMK_LEN                 32U
#define PTK_LEN                 48U
#define KCK_LEN                 16U
#define KEK_LEN                 16U
#define TK_LEN                  16U
#define NONCE_LEN               32U
#define MIC_LEN                 16U
#define GTK_MAX                 32U

enum rsn_state { RSN_OFF, RSN_WAIT_M1, RSN_WAIT_M3, RSN_KEYED, RSN_FAILED };

static struct {
	bool pmk_set, pmk_file, tested, armed, started;
	uint32_t state;
	uint8_t pmk[PMK_LEN];
	uint8_t ptk[PTK_LEN], snonce[NONCE_LEN], anonce[NONCE_LEN];
	uint8_t gtk[GTK_MAX];
	uint32_t gtk_len, gtk_idx;
	uint64_t replay, anonce_replay;
	uint32_t eapol_ver, key_ver;
	uint32_t msg1s, msg3s, bad_mic, ignored, late, msg4s, msg4_dumps;
	uint32_t group_cipher;
	const struct vayu_rsn_ops *ops;
	int (*log)(const char *format, ...) __printflike(1, 2);
} rsn;

/* ---- crypto through libkern's provider -------------------------------------------------------- */

static bool
sha1_hmac(const uint8_t *key, uint32_t key_len, const uint8_t *data, uint32_t data_len, uint8_t out[20])
{
	if (g_crypto_funcs == NULL || g_crypto_funcs->cchmac_fn == NULL || g_crypto_funcs->ccsha1_di == NULL) {
		return false;
	}
	g_crypto_funcs->cchmac_fn(g_crypto_funcs->ccsha1_di, key_len, key, data_len, data, out);
	return true;
}

/* AES-ECB decrypt, one block at a time: the whole of RFC 3394 needs of the cipher. */
static bool
aes_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	const struct ccmode_ecb *ecb;
	uint8_t ctx[512] __attribute__((aligned(16)));

	if (g_crypto_funcs == NULL || (ecb = g_crypto_funcs->ccaes_ecb_decrypt) == NULL ||
	    ecb->size > sizeof(ctx) || ecb->init == NULL || ecb->ecb == NULL) {
		return false;
	}
	if (ecb->init(ecb, (ccecb_ctx *)ctx, 16, key) != 0) {
		return false;
	}
	ecb->ecb((ccecb_ctx *)ctx, 1, in, out);
	memset(ctx, 0, sizeof(ctx));
	return true;
}

/* RFC 3394 §2.2.2, n = 1..; plain = len - 8 bytes, len must be a multiple of 8 and >= 24. */
static bool
aes_unwrap(const uint8_t kek[KEK_LEN], const uint8_t *in, uint32_t len, uint8_t *out)
{
	uint8_t a[8], b[16], r[256];
	uint32_t n, i, j, t;

	if (len < 24 || (len & 7U) != 0 || len - 8 > sizeof(r)) {
		return false;
	}
	n = (len - 8) / 8;
	memcpy(a, in, 8);
	memcpy(r, in + 8, len - 8);
	for (j = 6; j-- > 0;) {
		for (i = n; i-- > 0;) {
			t = n * j + (i + 1);
			memcpy(b, a, 8);
			b[4] ^= (uint8_t)(t >> 24);     /* A ^= t, the 32-bit counter in the low half */
			b[5] ^= (uint8_t)(t >> 16);
			b[6] ^= (uint8_t)(t >> 8);
			b[7] ^= (uint8_t)t;
			memcpy(b + 8, r + 8 * i, 8);
			if (!aes_ecb_decrypt(kek, b, b)) {
				return false;
			}
			memcpy(a, b, 8);
			memcpy(r + 8 * i, b + 8, 8);
		}
	}
	static const uint8_t iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };

	if (memcmp(a, iv, 8) != 0) {
		return false;
	}
	memcpy(out, r, len - 8);
	memset(r, 0, sizeof(r));
	memset(b, 0, sizeof(b));
	return true;
}

/* IEEE 802.11i PRF-SHA1 (802.11-2016 12.7.1.2): HMAC-SHA1(PMK, label|0|data|i), i from 0. */
static bool
prf_sha1(const uint8_t *key, uint32_t key_len, const char *label, const uint8_t *data, uint32_t data_len,
    uint8_t *out, uint32_t out_len)
{
	uint8_t buf[128], digest[20], counter = 0;
	uint32_t label_len = (uint32_t)strlen(label);

	if (label_len + 1 + data_len + 1 > sizeof(buf)) {
		return false;
	}
	memcpy(buf, label, label_len);
	buf[label_len] = 0;
	memcpy(buf + label_len + 1, data, data_len);
	while (out_len != 0) {
		uint32_t take;

		buf[label_len + 1 + data_len] = counter++;
		if (!sha1_hmac(key, key_len, buf, label_len + 2 + data_len, digest)) {
			return false;
		}
		take = out_len < sizeof(digest) ? out_len : sizeof(digest);
		memcpy(out, digest, take);
		out += take;
		out_len -= take;
	}
	memset(buf, 0, sizeof(buf));
	memset(digest, 0, sizeof(digest));
	return true;
}

/* Runs once: a wrong provider is better found here than in a key on the air. */
static bool
crypto_selftest(void)
{
	/* RFC 2202 test case 1: key 0x0b * 20, "Hi There" -> b617318655057264e28bc0b6fb378c8ef146be00 */
	static const uint8_t key[20] = { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
		                         0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b };
	static const uint8_t text[8] = { 'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e' };
	static const uint8_t mac[20] = { 0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
		                         0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00 };
	/* RFC 3394 §4.1: KEK 000102..0f, 128-bit key 00112233..ff, wrapped 1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5 */
	static const uint8_t kek[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
	static const uint8_t wrapped[24] = { 0x1f, 0xa6, 0x8b, 0x0a, 0x81, 0x12, 0xb4, 0x47, 0xae, 0xf3, 0x4b, 0xd8,
		                             0xfb, 0x5a, 0x7b, 0x82, 0x9d, 0x3e, 0x86, 0x23, 0x71, 0xd2, 0xcf, 0xe5 };
	static const uint8_t plain[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		                           0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	uint8_t out[20], unwrapped[16];
	bool hmac_ok, unwrap_ok;

	hmac_ok = sha1_hmac(key, sizeof(key), text, sizeof(text), out) && memcmp(out, mac, sizeof(mac)) == 0;
	unwrap_ok = aes_unwrap(kek, wrapped, sizeof(wrapped), unwrapped) && memcmp(unwrapped, plain, 16) == 0;
	if (rsn.log != NULL) {
		rsn.log("vayu-wifi: W25 S3 crypto: %s hmac-sha1 %u rfc3394 %u\n",
		    hmac_ok && unwrap_ok ? "OK" : "ERROR", hmac_ok ? 1 : 0, unwrap_ok ? 1 : 0);
	}
	return hmac_ok && unwrap_ok;
}

/* ---- setup ------------------------------------------------------------------------------------- */

/* The PMK is a file on the root volume, 64 hex digits: boot arguments end up in every log
 * (the bootloader's cmdline, the kernel's boot-args, launchd's kern.bootargs, a panic buffer),
 * so no secret goes there.  Reading it is deferred to the first arming, long after the root is
 * mounted; the buffer is wiped before anything else happens to it. */
static bool
pmk_read_file(void)
{
	char text[160];
	uint8_t pmk[PMK_LEN];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint32_t len, i;
	int left = 0, error;
	bool ok = true;

	if (rootvnode == NULLVP) {
		rsn.log("vayu-wifi: W25 S0 pmk: ERROR no root volume\n");
		return false;
	}
	if (vnode_lookup(PMK_PATH, 0, &vnode, context) != 0) {
		rsn.log("vayu-wifi: W25 S0 pmk: ERROR " PMK_PATH " not found\n");
		return false;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size < 64 ||
	    attributes.va_data_size > (off_t)sizeof(text)) {
		vnode_put(vnode);
		rsn.log("vayu-wifi: W25 S0 pmk: ERROR size %lld (want 64 hex digits)\n",
		    (long long)attributes.va_data_size);
		return false;
	}
	len = (uint32_t)attributes.va_data_size;
	memset(text, 0, sizeof(text));
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)text, (int)len, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		memset(text, 0, sizeof(text));
		rsn.log("vayu-wifi: W25 S0 pmk: ERROR read %d, %d bytes left\n", error, left);
		return false;
	}
	while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ')) {
		len--;
	}
	if (len != 2 * PMK_LEN) {
		ok = false;
	}
	for (i = 0; ok && i < PMK_LEN; i++) {
		uint32_t hi = (uint8_t)text[2 * i], lo = (uint8_t)text[2 * i + 1];
		uint32_t vh = (hi >= '0' && hi <= '9') ? hi - '0' :
		    ((hi | 0x20) >= 'a' && (hi | 0x20) <= 'f') ? (hi | 0x20) - 'a' + 10 : 0xff;
		uint32_t vl = (lo >= '0' && lo <= '9') ? lo - '0' :
		    ((lo | 0x20) >= 'a' && (lo | 0x20) <= 'f') ? (lo | 0x20) - 'a' + 10 : 0xff;

		if (vh == 0xff || vl == 0xff) {
			ok = false;
		} else {
			pmk[i] = (uint8_t)(vh << 4 | vl);
		}
	}
	memset(text, 0, sizeof(text));
	if (!ok) {
		memset(pmk, 0, sizeof(pmk));
		rsn.log("vayu-wifi: W25 S0 pmk: ERROR not 64 hex digits\n");
		return false;
	}
	memcpy(rsn.pmk, pmk, sizeof(pmk));
	memset(pmk, 0, sizeof(pmk));
	rsn.pmk_set = true;
	rsn.log("vayu-wifi: W25 S0 pmk: OK file (len %u)\n", PMK_LEN);
	return true;
}

void
vayu_rsn_reset(int (*log)(const char *format, ...) __printflike(1, 2))
{
	uint32_t enabled = 0;

	memset(&rsn, 0, sizeof(rsn));
	rsn.log = log;
	rsn.state = RSN_OFF;
	if (log == NULL) {
		return;
	}
	PE_parse_boot_argn("vayu_wifi_pmk_file", &enabled, sizeof(enabled));
	rsn.pmk_file = enabled != 0;
	if (!rsn.pmk_file) {
		rsn.log("vayu-wifi: W25 S0 pmk: ERROR not set (vayu_wifi_pmk_file=1 needed for RSN)\n");
	}
}

bool
vayu_rsn_armed(void)
{
	if (!rsn.pmk_set && rsn.pmk_file && !pmk_read_file()) {
		return false;
	}
	if (!rsn.pmk_set) {
		return false;
	}
	if (!rsn.tested) {
		rsn.tested = true;
		rsn.armed = crypto_selftest();
	}
	return rsn.armed;
}

void
vayu_rsn_stop(void)
{
	if (rsn.gtk_len != 0) {
		memset(rsn.gtk, 0, sizeof(rsn.gtk));
	}
	memset(rsn.ptk, 0, sizeof(rsn.ptk));
	memset(rsn.snonce, 0, sizeof(rsn.snonce));
	memset(rsn.anonce, 0, sizeof(rsn.anonce));
	rsn.gtk_len = 0;
	rsn.replay = rsn.anonce_replay = 0;
	rsn.msg1s = rsn.msg3s = rsn.bad_mic = rsn.ignored = 0;
	rsn.started = false;
	rsn.state = RSN_OFF;
}

void
vayu_rsn_start(const struct vayu_rsn_ops *ops)
{
	if (ops == NULL || !vayu_rsn_armed()) {
		return;
	}
	rsn.ops = ops;
	rsn.group_cipher = ops->group_cipher != 0 ? ops->group_cipher : WMI_CIPHER_CCMP;
	rsn.started = true;
	rsn.state = RSN_WAIT_M1;
	rsn.eapol_ver = 0;
	rsn.key_ver = 0;
	rsn.log("vayu-wifi: W25 S3 4way: OK waiting for message 1/4 (pairwise CCMP, AKM PSK)\n");
}

int
vayu_rsn_poll(void)
{
	if (!rsn.started) {
		return -1;
	}
	if (rsn.state == RSN_KEYED) {
		return 0;
	}
	if (rsn.state == RSN_FAILED) {
		return -1;
	}
	return 1;
}

bool
vayu_rsn_keyed(void)
{
	return rsn.state == RSN_KEYED;
}

/* ---- the frames -------------------------------------------------------------------------------- */

static uint32_t
be16(const uint8_t *p)
{
	return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}

static uint64_t
be64(const uint8_t *p)
{
	uint64_t v = 0;

	for (uint32_t i = 0; i < 8; i++) {
		v = v << 8 | p[i];
	}
	return v;
}

static uint8_t *
put16be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
	return p + 2;
}

static uint8_t *
put_tlv(uint8_t *p, uint32_t tag, uint32_t len)
{
	p[0] = (uint8_t)len;
	p[1] = (uint8_t)(len >> 8);
	p[2] = (uint8_t)tag;
	p[3] = (uint8_t)(tag >> 8);
	return p + 4;
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

static uint8_t *
put_mac8(uint8_t *p, const uint8_t *mac)
{
	memset(p, 0, 8);
	memcpy(p, mac, 6);
	return p + 8;
}

/* An EAPOL-Key frame over the 802.11 data path: 24-byte header (to DS), LLC/SNAP 88 8e, EAPOL. */
static int
send_eapol(uint16_t key_info, const uint8_t *nonce, const uint8_t *kd, uint16_t kd_len)
{
	static uint8_t f[24 + 8 + EAPOL_HDR + EAPOL_KEY_FIXED + 64];
	uint8_t mic[20];
	uint8_t *e = f + 24 + 8, *k;
	uint32_t eapol_len, flen;
	uint16_t version = rsn.eapol_ver != 0 ? (uint16_t)rsn.eapol_ver : 1;
	uint8_t desc = rsn.key_ver != 0 ? (uint8_t)rsn.key_ver : 2;

	if (rsn.ops == NULL || rsn.ops->tx_data == NULL || kd_len > 64) {
		return -1;
	}
	/* W25l: the whole buffer, not just the header.  It is static, and W25k left the 2/4 MIC
	 * sitting at k+77 when the 4/4 was built: the 4/4 MIC was then computed over a frame whose
	 * MIC field held the 2/4's MIC instead of zeros, so the AP rejected every 4/4 (invalid MIC)
	 * while the 2/4, built first from a zeroed buffer, was accepted. */
	memset(f, 0, sizeof(f));
	f[0] = 0x08;                            /* data */
	f[1] = 0x01;                            /* to DS */
	memcpy(f + 4, rsn.ops->bssid, 6);       /* addr1: the AP */
	memcpy(f + 10, rsn.ops->mac, 6);        /* addr2: us */
	memcpy(f + 16, rsn.ops->bssid, 6);      /* addr3: the AP */
	f[24] = 0xaa;
	f[25] = 0xaa;
	f[26] = 0x03;
	f[27] = 0x00;
	f[28] = 0x00;
	f[29] = 0x00;
	f[30] = 0x88;
	f[31] = 0x8e;
	e[0] = (uint8_t)version;
	e[1] = EAPOL_TYPE_KEY;
	(void)put16be(e + 2, EAPOL_KEY_FIXED + (uint32_t)kd_len);
	k = e + EAPOL_HDR;
	k[0] = desc;
	(void)put16be(k + 1, key_info);
	k[3] = 0;                               /* key length, informational */
	k[4] = 0;
	for (uint32_t i = 0; i < 8; i++) {
		k[5 + i] = (uint8_t)(rsn.replay >> (56 - 8 * i));
	}
	if (nonce != NULL) {
		memcpy(k + 13, nonce, NONCE_LEN);
	}
	(void)put16be(k + 93, kd_len);
	if (kd_len != 0) {
		memcpy(k + EAPOL_KEY_FIXED, kd, kd_len);
	}
	eapol_len = EAPOL_HDR + EAPOL_KEY_FIXED + kd_len;
	if ((key_info & KI_MIC) != 0) {
		if (!sha1_hmac(rsn.ptk, KCK_LEN, e, eapol_len, mic)) {
			return -1;
		}
		memcpy(k + 77, mic, MIC_LEN);
		memset(mic, 0, sizeof(mic));
	}
	flen = 24 + 8 + eapol_len;
	if (key_info == 0x030a && kd_len == 0) {
		rsn.msg4s++;
	}
	if (key_info == 0x030a && kd_len == 0 && rsn.msg4_dumps < 2) {
		rsn.msg4_dumps++;
		/* W25l: the whole 4/4, as it goes on the air: MAC bytes 0-3 masked, the MIC printed
		 * (a 4/4 has no nonce and no key data, and the MIC of a finished handshake is not a
		 * secret that opens anything). */
		char row[32 * 3 + 1];
		uint32_t row_at, i;

		for (row_at = 0; row_at < flen; row_at += 32) {
			uint32_t n = flen - row_at < 32 ? flen - row_at : 32, j = 0;

			for (i = 0; i < n; i++) {
				static const char hx[] = "0123456789abcdef";
				uint32_t at = row_at + i;
				/* the MIC (frame bytes 32+4+77 .. +16) is masked too: with the nonces it is what an
				 * offline dictionary attack on the passphrase needs (owner rule: no secrets in logs) */
				bool mac = (at >= 4 && at < 8) || (at >= 10 && at < 14) || (at >= 16 && at < 20) ||
				    (at >= 32 + 4 + 77 && at < 32 + 4 + 93);

				row[j++] = mac ? 'x' : hx[f[at] >> 4];
				row[j++] = mac ? 'x' : hx[f[at] & 15];
				row[j++] = ' ';
			}
			if (j != 0) {
				j--;
			}
			row[j] = 0;
			rsn.log("vayu-wifi: W25 S3 4/4 +%03u: %s\n", row_at, row);
		}
	}
	return rsn.ops->tx_data(f, flen, true);
}

static int
wmi_install_key(bool group, uint32_t idx, uint32_t cipher, const uint8_t *key, uint32_t key_len)
{
	static uint8_t m[4 + 4 + 92 + 4 + 40];
	uint8_t *p;
	uint32_t padded = (key_len + 3) & ~3U;

	if (rsn.ops == NULL || rsn.ops->wmi_send == NULL || key_len > 40) {
		return -1;
	}
	memset(m, 0, sizeof(m));
	p = put_tlv(m + 4, TAG_VDEV_INSTALL_KEY, 92);
	p = put32(p, 0);                        /* vdev 0 */
	p = put_mac8(p, rsn.ops->bssid);
	p = put32(p, idx);
	p = put32(p, group ? 1 : 0);            /* key_flags */
	p = put32(p, cipher);
	p += 3 * 8 + 16 + 16;                   /* rsc/global/tsc/wpi counters: zero */
	p = put32(p, key_len);
	p = put32(p, 0);                        /* tx mic length */
	p = put32(p, 0);                        /* rx mic length */
	p = put_tlv(p, 0x11U, padded);          /* WMI_TLV_TAG_ARRAY_BYTE = 17 (W25g) */
	memcpy(p, key, key_len);
	put32(m, CMD_VDEV_INSTALL_KEY);
	return rsn.ops->wmi_send(m, (uint32_t)(4 + 4 + 92 + 4 + padded), 1000);
}

static int
wmi_peer_authorize(void)
{
	static uint8_t m[4 + 4 + 20];
	uint8_t *p;

	memset(m, 0, sizeof(m));
	p = put_tlv(m + 4, TAG_PEER_SET_PARAM, 20);
	p = put32(p, 0);                        /* vdev 0 */
	p = put_mac8(p, rsn.ops->bssid);
	p = put32(p, PEER_PARAM_AUTHORIZE);
	(void)put32(p, 1);
	put32(m, CMD_PEER_SET_PARAM);
	return rsn.ops->wmi_send(m, sizeof(m), 1000);
}

/* message 2/4: SNonce, the RSN IE of our assoc request, MIC under the KCK */
static int
send_msg2(void)
{
	return send_eapol(0x010a, rsn.snonce, rsn.ops->rsn_ie, (uint16_t)rsn.ops->rsn_ie_len);
}

/* message 4/4: no nonce, no key data, MIC under the KCK */
static int
send_msg4(void)
{
	return send_eapol(0x030a, NULL, NULL, 0);
}

/* ---- message 1/4 ------------------------------------------------------------------------------- */

static void
handle_msg1(const uint8_t *k, const uint8_t *eapol, uint32_t len, uint64_t replay)
{
	uint8_t data[6 + 6 + NONCE_LEN + NONCE_LEN];
	const uint8_t *aa = rsn.ops->bssid, *spa = rsn.ops->mac, *first_mac, *second_mac;
	const uint8_t *first_nonce, *second_nonce;

	rsn.eapol_ver = eapol[0];
	rsn.key_ver = k[0];
	rsn.replay = replay;
	rsn.anonce_replay = replay;
	memcpy(rsn.anonce, k + 13, NONCE_LEN);
	read_random(rsn.snonce, NONCE_LEN);

	if (memcmp(aa, spa, 6) < 0) {
		first_mac = aa;
		second_mac = spa;
	} else {
		first_mac = spa;
		second_mac = aa;
	}
	if (memcmp(rsn.anonce, rsn.snonce, NONCE_LEN) < 0) {
		first_nonce = rsn.anonce;
		second_nonce = rsn.snonce;
	} else {
		first_nonce = rsn.snonce;
		second_nonce = rsn.anonce;
	}
	memcpy(data, first_mac, 6);
	memcpy(data + 6, second_mac, 6);
	memcpy(data + 12, first_nonce, NONCE_LEN);
	memcpy(data + 44, second_nonce, NONCE_LEN);
	if (!prf_sha1(rsn.pmk, PMK_LEN, "Pairwise key expansion", data, sizeof(data), rsn.ptk, PTK_LEN)) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR PRF (no crypto provider)\n");
		rsn.state = RSN_FAILED;
		return;
	}
	memset(data, 0, sizeof(data));
	rsn.state = RSN_WAIT_M3;
	rsn.msg1s++;
	if (send_msg2() != 0) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR message 2 not sent\n");
		rsn.state = RSN_FAILED;
		return;
	}
	rsn.log("vayu-wifi: W25 S3 4way: OK message 1/4 (ver %u desc %u replay %llu) -> 2/4 sent\n",
	    rsn.eapol_ver, rsn.key_ver, (unsigned long long)replay);
}

/* ---- message 3/4 ------------------------------------------------------------------------------- */

static bool
mic_ok(const uint8_t *eapol, uint32_t len, const uint8_t *k)
{
	uint8_t want[MIC_LEN], got[20], saved[MIC_LEN];

	memcpy(want, k + 77, MIC_LEN);
	memcpy(saved, k + 77, MIC_LEN);
	memset((uint8_t *)(k + 77), 0, MIC_LEN);
	/* (the frame is ours to change: the RX buffer is reused and the parse below keeps no MIC) */
	bool ok = sha1_hmac(rsn.ptk, KCK_LEN, eapol, len, got) && memcmp(want, got, MIC_LEN) == 0;

	memcpy((uint8_t *)(k + 77), saved, MIC_LEN);
	memset(got, 0, sizeof(got));
	memset(saved, 0, sizeof(saved));
	return ok;
}

/* the GTK KDE (00-0F-AC:1) in the unwrapped key data; RSC follows the key inside the KDE */
static bool
gtk_from_kde(const uint8_t *plain, uint32_t len)
{
	uint32_t o;

	for (o = 0; o + 2 <= len && o + 2 + plain[o + 1] <= len; o += 2 + plain[o + 1]) {
		uint32_t klen;

		if (plain[o] != 0xdd || plain[o + 1] < 6 || plain[o + 2] != 0x00 || plain[o + 3] != 0x0f ||
		    plain[o + 4] != 0xac || plain[o + 5] != 0x01) {
			continue;
		}
		rsn.gtk_idx = plain[o + 6] & 3U;
		klen = plain[o + 1] - 6;
		if (klen > 32) {
			klen = 32;
		}
		if (klen < 16) {
			return false;
		}
		memcpy(rsn.gtk, plain + o + 8, klen);
		rsn.gtk_len = klen;
		return true;
	}
	return false;
}

static void
handle_msg3(const uint8_t *k, const uint8_t *eapol, uint32_t len, uint64_t replay)
{
	uint16_t info = (uint16_t)be16(k + 1), kd_len = (uint16_t)be16(k + 93);
	uint8_t plain[256];
	bool have_gtk = false;

	rsn.msg3s++;
	if (replay < rsn.replay) {
		rsn.ignored++;
		return;
	}
	if (memcmp(k + 13, rsn.anonce, NONCE_LEN) != 0) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR 3/4 ANonce differs from 1/4\n");
		rsn.state = RSN_FAILED;
		return;
	}
	if (!mic_ok(eapol, len, k)) {
		rsn.bad_mic++;
		rsn.log("vayu-wifi: W25 S3 4way: ERROR 3/4 MIC wrong (%u)\n", rsn.bad_mic);
		rsn.state = RSN_FAILED;
		return;
	}
	rsn.replay = replay;
	if ((info & KI_ENC_KD) != 0 && kd_len >= 24 && kd_len <= sizeof(plain) + 8) {
		if (!aes_unwrap(rsn.ptk + KCK_LEN, k + EAPOL_KEY_FIXED, kd_len, plain)) {
			rsn.log("vayu-wifi: W25 S3 4way: ERROR GTK unwrap failed (kd %u)\n", kd_len);
		} else {
			have_gtk = gtk_from_kde(plain, kd_len - 8);
			memset(plain, 0, sizeof(plain));
		}
	} else if (kd_len != 0) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR key data %u without ENC_KD\n", kd_len);
	}
	if (send_msg4() != 0) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR message 4 not sent\n");
		rsn.state = RSN_FAILED;
		return;
	}
	/* 4/4 goes out in the clear: the keys are installed after it. W25k: 5 ms was not enough --
	 * TX is asynchronous, the firmware took the pairwise key first and the AP never accepted the
	 * 4/4 (run 134340: 3/4 repeated with replay 3 and 4, then DEAUTH 15). Leave it time to fly. */
	delay_for_interval(300, NSEC_PER_MSEC);
	if (wmi_install_key(false, 0, WMI_CIPHER_CCMP, rsn.ptk + KCK_LEN + KEK_LEN, TK_LEN) != 0) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR pairwise key install refused\n");
		rsn.state = RSN_FAILED;
		return;
	}
	if (have_gtk && wmi_install_key(true, rsn.gtk_idx, rsn.group_cipher, rsn.gtk, rsn.gtk_len) != 0) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR group key install refused\n");
	}
	if (wmi_peer_authorize() != 0) {
		rsn.log("vayu-wifi: W25 S3 4way: ERROR PEER_AUTHORIZE refused\n");
		rsn.state = RSN_FAILED;
		return;
	}
	rsn.state = RSN_KEYED;
	rsn.log("vayu-wifi: W25 S3 4way: OK 3/4 verified, 4/4 sent, pairwise+group keys installed (gtk %u idx %u, cipher %u)\n",
	    have_gtk ? 1 : 0, rsn.gtk_idx, rsn.group_cipher);
}

/* ---- the received EAPOL frame ------------------------------------------------------------------ */

void
vayu_rsn_eapol_rx(const uint8_t *eapol, uint32_t len)
{
	const uint8_t *k;
	uint16_t info;
	uint32_t body, kd_len;
	uint64_t replay;

	if (!rsn.started || rsn.ops == NULL || rsn.state == RSN_FAILED) {
		return;
	}
	if (len < EAPOL_HDR + EAPOL_KEY_FIXED || eapol[1] != EAPOL_TYPE_KEY) {
		return;
	}
	body = be16(eapol + 2);
	if (body < EAPOL_KEY_FIXED || EAPOL_HDR + body > len) {
		return;
	}
	k = eapol + EAPOL_HDR;
	if (k[0] != 2) {
		return;                         /* only the RSN key descriptor (WPA2) */
	}
	info = (uint16_t)be16(k + 1);
	kd_len = (uint16_t)be16(k + 93);
	if (EAPOL_KEY_FIXED + kd_len > body) {
		return;
	}
	replay = be64(k + 5);
	if (rsn.state == RSN_KEYED) {
		/* The AP resending 3/4 after our 4/4 means it did not take the 4/4.  wpa_supplicant
		 * answers a newer 3/4 with a new 4/4 carrying that replay counter; the keys stay. */
		if ((info & KI_PAIRWISE) != 0 && (info & KI_ACK) != 0 && (info & KI_MIC) != 0 &&
		    (info & KI_INSTALL) != 0 && replay > rsn.replay && rsn.msg4s < 8) {
			rsn.replay = replay;
			rsn.log("vayu-wifi: W25 S3 4way: OK 3/4 again with replay %llu, 4/4 sent again (no "
			    "keys reinstalled)\n", (unsigned long long)replay);
			(void)send_msg4();
		} else if (rsn.late < 4) {
			rsn.late++;
			rsn.log("vayu-wifi: W25 S3 4way: WARNING eapol after the keys (info %#x replay %llu): "
			    "not answered\n", info, (unsigned long long)replay);
		}
		return;
	}
	if ((info & KI_PAIRWISE) != 0 && (info & KI_ACK) != 0 && (info & KI_MIC) == 0) {
		if (rsn.state != RSN_WAIT_M1) {
			rsn.ignored++;          /* a retransmitted 1/4 after our 2/4: answer again, same PTK */
			if (rsn.state == RSN_WAIT_M3 && replay == rsn.anonce_replay) {
				(void)send_msg2();
			}
			return;
		}
		if (rsn.msg1s >= 3) {
			rsn.log("vayu-wifi: W25 S3 4way: ERROR the AP keeps sending message 1 (wrong PMK?)\n");
			rsn.state = RSN_FAILED;
			return;
		}
		handle_msg1(k, eapol, EAPOL_HDR + body, replay);
		return;
	}
	if (rsn.state == RSN_WAIT_M3 && (info & KI_PAIRWISE) != 0 && (info & KI_ACK) != 0 &&
	    (info & KI_MIC) != 0 && (info & KI_INSTALL) != 0) {
		handle_msg3(k, eapol, EAPOL_HDR + body, replay);
		return;
	}
	rsn.ignored++;
}
