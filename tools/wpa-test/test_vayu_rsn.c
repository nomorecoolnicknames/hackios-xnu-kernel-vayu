/*
 * The vayu station's own WPA2 code (bsd/dev/vayu_wifi_rsn.c of the kernel tree) against the same
 * independent access point as lx_wpa (reference.py): the 4-way handshake, a 3/4 that comes again,
 * forged ones, and the group key handshake (a new GTK while the link is up, and the 2/2 it wants); and which
 * file the PMK is looked for in (wifi.pmk, or the one vayu_wifi_pmk_name= names, never a path).
 * The driver's file is taken as it is, its #include lines replaced by the host stand-ins below.
 */
#define main lx_test_main
#include "test_wpa.c"
#undef main
#include <stdbool.h>
#include <stdarg.h>
#include <sys/types.h>
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#include "vayu_wifi_rsn.h"

/* Host stand-ins for XNU's file IO, boot arguments, random source, delay and crypto provider. */
typedef void *vfs_context_t;
typedef void *vnode_t;
#define NULLVP NULL
static vnode_t rootvnode;
struct vnode_attr { off_t va_data_size; };
#define VATTR_INIT(a) memset(a, 0, sizeof(*(a)))
#define VATTR_WANTED(a, field) ((void)(a))
#define UIO_READ 0
#define UIO_SYSSPACE 0
#define NSEC_PER_MSEC 1000000
static vfs_context_t vfs_context_kernel(void) { return NULL; }
static void *vfs_context_ucred(vfs_context_t c) { return c; }
static void *vfs_context_proc(vfs_context_t c) { return c; }
static char looked_up[256];                     /* the last path vnode_lookup was asked for; none is there */
static int vnode_lookup(const char *p, int f, vnode_t *v, vfs_context_t c)
{ snprintf(looked_up, sizeof(looked_up), "%s", p); return -1; }
static int vnode_getattr(vnode_t v, struct vnode_attr *a, vfs_context_t c) { return -1; }
static void vnode_put(vnode_t v) { }
static int vn_rdwr(int rw, vnode_t v, caddr_t b, int n, off_t o, int s, int f, void *c, int *left, void *p)
{ return -1; }
static int boot_gtk_rekey = -1;                 /* vayu_wifi_gtk_rekey= on the "command line": -1 absent */
static int boot_pmk_file = -1;                  /* vayu_wifi_pmk_file= */
static const char *boot_pmk_name;               /* vayu_wifi_pmk_name=: NULL absent */
static bool PE_parse_boot_argn(const char *name, void *out, int size)
{
	if (strcmp(name, "vayu_wifi_gtk_rekey") == 0 && boot_gtk_rekey >= 0 && size == 4) {
		*(uint32_t *)out = (uint32_t)boot_gtk_rekey;
		return true;
	}
	if (strcmp(name, "vayu_wifi_pmk_file") == 0 && boot_pmk_file >= 0 && size == 4) {
		*(uint32_t *)out = (uint32_t)boot_pmk_file;
		return true;
	}
	return false;
}
/* as XNU's: at most size - 1 characters of the value, NUL-terminated */
static bool PE_parse_boot_arg_str(const char *name, char *out, int size)
{
	if (strcmp(name, "vayu_wifi_pmk_name") == 0 && boot_pmk_name != NULL && size > 0) {
		snprintf(out, (size_t)size, "%s", boot_pmk_name);
		return true;
	}
	return false;
}
static void delay_for_interval(unsigned count, unsigned scale) { }
static const uint8_t *random_nonce;
static void read_random(void *out, unsigned len) { memcpy(out, random_nonce, len); }

typedef uint8_t ccecb_ctx;
struct ccmode_ecb {
	size_t size;
	int (*init)(const struct ccmode_ecb *, ccecb_ctx *, size_t, const void *);
	int (*ecb)(const ccecb_ctx *, size_t, const void *, void *);
};
static int host_aes_init(const struct ccmode_ecb *mode, ccecb_ctx *ctx, size_t len, const void *key)
{ aes_keys(key, ctx); return len == 16 ? 0 : -1; }
static int host_aes_decrypt(const ccecb_ctx *ctx, size_t count, const void *in, void *out)
{
	memcpy(out, in, count * 16);
	for (size_t i = 0; i < count; i++) aes_decrypt(ctx, (uint8_t *)out + i * 16);
	return 0;
}
static void host_hmac(const void *di, size_t kn, const void *key, size_t n, const void *data, void *out)
{
	const uint8_t *parts[] = { data };
	unsigned lengths[] = { (unsigned)n };
	hmac_sha1(key, (unsigned)kn, parts, lengths, 1, out);
}
static const struct ccmode_ecb host_ecb = { 176, host_aes_init, host_aes_decrypt };
static const struct {
	void (*cchmac_fn)(const void *, size_t, const void *, size_t, const void *, void *);
	const void *ccsha1_di;
	const struct ccmode_ecb *ccaes_ecb_decrypt;
} host_crypto = { host_hmac, &host_ecb, &host_ecb }, *g_crypto_funcs = &host_crypto;

#define aes_unwrap vayu_aes_unwrap
#include "vayu_rsn.inc"
#undef aes_unwrap

/* What the station would have put on the air, and in the firmware. */
static uint8_t sent[512], installed[40];
static uint32_t sent_len, sends, installs, group_installs, installed_len, installed_idx;
static bool sent_clear;
static char last_line[256];
static int quiet_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(last_line, sizeof(last_line), fmt, ap);
	va_end(ap);
	return 0;
}
static int host_tx(const uint8_t *frame, uint32_t len, bool clear)
{
	check(len <= sizeof(sent) && len > 32, "an EAPOL frame of a sane length", (int)len);
	memcpy(sent, frame, len);
	sent_len = len;
	sent_clear = clear;
	sends++;
	return 0;
}
/* WMI_VDEV_INSTALL_KEY: key index at +4+4+4+8, the group flag after it, the key after the 92-byte body + TLV */
static int host_wmi(const uint8_t *msg, uint32_t len, uint32_t wait)
{
	installs++;
	if (len >= 4 && msg[0] == 0x09 && msg[1] == 0x50) {
		uint32_t idx = msg[20], flags = msg[24], klen = msg[24 + 4 + 4 + 56];

		if (flags == 1) {
			group_installs++;
			installed_idx = idx;
			installed_len = klen <= sizeof(installed) ? klen : 0;
			memcpy(installed, msg + 4 + 4 + 92 + 4, installed_len);
		}
	}
	return 0;
}

static void resign(uint8_t *msg, unsigned len)
{
	uint8_t digest[20];
	memset(msg + AT_MIC, 0, 16);
	host_hmac(NULL, KCK_LEN, rsn.ptk, len, msg, digest);
	memcpy(msg + AT_MIC, digest, 16);
}

static void set_replay(uint8_t *msg, uint64_t replay)
{
	for (unsigned i = 0; i < 8; i++) msg[AT_REPLAY + i] = (uint8_t)(replay >> (56 - 8 * i));
}

/* the sent frame is the reference's answer, after the 24-byte 802.11 header and the 8 of LLC/SNAP */
static bool sent_is(const unsigned char *want, size_t want_length)
{
	return sent_len == want_length + 32 && memcmp(sent + 32, want, want_length) == 0;
}

static void vayu_whole(void)
{
	for (unsigned n = 0; n < sizeof(handshakes) / sizeof(handshakes[0]); n++) {
		const struct handshake *h = &handshakes[n];
		uint8_t m[MESSAGE_MOST], wanted[MESSAGE_MOST];
		struct vayu_rsn_ops ops = {
			.log = quiet_log, .wmi_send = host_wmi, .tx_data = host_tx,
			.mac = h->station, .bssid = h->ap, .rsn_ie = h->rsn, .rsn_ie_len = (uint32_t)h->rsn_length,
		};
		uint64_t replay;

		boot_gtk_rekey = n == 0 ? 0 : -1;       /* the first network with vayu_wifi_gtk_rekey=0, the rest default */
		vayu_rsn_reset(quiet_log);
		check(vayu_rsn_set_pmk(h->pmk, 32), "vayu PMK set", n);
		vayu_rsn_start(&ops);
		memset(&ops, 0, sizeof(ops));           /* the station's block-local ends here (R2.4) */
		random_nonce = h->snonce;
		sends = installs = group_installs = 0;

		/* the 4-way */
		memcpy(m, h->m1, h->m1_length);
		vayu_rsn_eapol_rx(m, h->m1_length);
		check(sends == 1 && sent_is(h->m2, h->m2_length) && sent_clear && sent[1] == 0x01,
		    "message 2 is the reference's, in the clear, with the station's operations gone", n);
		memcpy(m, h->m3_wrong, h->m3_wrong_length);
		vayu_rsn_eapol_rx(m, h->m3_wrong_length);
		check(rsn.state == RSN_WAIT_M3 && sends == 1 && installs == 0, "a 3/4 under another passphrase leaves it waiting", n);
		memcpy(m, h->m3, h->m3_length);
		vayu_rsn_eapol_rx(m, h->m3_length);
		check(vayu_rsn_keyed() && sends == 2 && installs == 3 && group_installs == 1, "the keys are installed once", n);
		check(sent_is(h->m4, h->m4_length) && sent_clear, "message 4 is the reference's, in the clear", n);
		check(rsn.gtk_len == 16 && memcmp(rsn.gtk, h->group, 16) == 0 && rsn.gtk_idx == h->group_index &&
		    installed_len == 16 && memcmp(installed, h->group, 16) == 0 && installed_idx == h->group_index,
		    "the group key and its index are the reference's, and so is what the firmware got", n);

		/* a 3/4 again: forged ones change nothing, a genuine newer one gets a 4/4 and no reinstall */
		replay = rsn.replay;
		for (unsigned attack = 0; attack < 5; attack++) {
			memcpy(m, h->m3, h->m3_length);
			set_replay(m, replay + 1);
			if (attack == 1) m[AT_NONCE] ^= 1;              /* another ANonce */
			if (attack == 2) m[AT_INFO + 1] ^= 1;           /* descriptor version */
			if (attack == 3) m[AT_INFO] |= 0x08;            /* Request */
			if (attack == 4) m[AT_INFO] |= 0x04;            /* Error */
			if (attack != 0) resign(m, h->m3_length);       /* (a valid MIC, a wrong session or wrong flags) */
			vayu_rsn_eapol_rx(m, h->m3_length);
			check(rsn.replay == replay && sends == 2 && installs == 3 && rsn.state == RSN_KEYED,
			    "a forged 3/4 does not move the counter, the keys or the 4/4 budget", attack);
		}
		memcpy(m, h->m3, h->m3_length);
		set_replay(m, replay + 1);
		resign(m, h->m3_length);
		memcpy(wanted, m, h->m3_length);
		vayu_rsn_eapol_rx(m, h->m3_length);
		check(rsn.replay == replay + 1 && sends == 3 && installs == 3 && memcmp(wanted, m, h->m3_length) == 0,
		    "a genuine 3/4 again is answered, nothing reinstalled, its bytes left as they were", n);
		check(be64(sent + 32 + AT_REPLAY) == replay + 1 && mic_ok(sent + 32, sent_len - 32, sent + 32 + EAPOL_HDR),
		    "that 4/4 has the new counter and a right MIC", n);
		vayu_rsn_eapol_rx(m, h->m3_length);
		check(sends == 3 && installs == 3, "the same 3/4 once more is ignored", n);

		if (n == 0) {
			/* vayu_wifi_gtk_rekey=0: as before, the group message is only logged */
			memcpy(m, h->g1, h->g1_length);
			vayu_rsn_eapol_rx(m, h->g1_length);
			check(sends == 3 && installs == 3 && memcmp(rsn.gtk, h->group, 16) == 0,
			    "vayu_wifi_gtk_rekey=0: a group 1/2 is not answered", n);
			vayu_rsn_stop();
			continue;
		}

		/* the group key handshake: a forged or spoiled 1/2 changes nothing */
		replay = rsn.replay;
		for (unsigned attack = 0; attack < 6; attack++) {
			memcpy(m, h->g1, h->g1_length);
			if (attack == 0) m[AT_DATA + 3] ^= 0x40;        /* a bit of the key data turned, the MIC left */
			if (attack == 1) m[AT_INFO] &= (uint8_t)~0x10;  /* no "encrypted key data" */
			if (attack == 2) m[AT_INFO] |= 0x08;            /* Request */
			if (attack == 3) m[AT_INFO + 1] ^= 1;           /* descriptor version */
			if (attack == 4) set_replay(m, replay);         /* a counter that is not beyond the last */
			if (attack == 5) m[AT_INFO] &= (uint8_t)~0x02;  /* not secure */
			if (attack != 0) resign(m, h->g1_length);
			vayu_rsn_eapol_rx(m, h->g1_length);
			check(rsn.replay == replay && sends == 3 && installs == 3 && memcmp(rsn.gtk, h->group, 16) == 0,
			    "a forged group 1/2 changes no counter, no key and sends nothing", attack);
		}
		/* the AP's 1/2: the new GTK installed, 2/2 the reference's, under the pairwise key */
		memcpy(m, h->g1, h->g1_length);
		vayu_rsn_eapol_rx(m, h->g1_length);
		check(sends == 4 && installs == 4 && group_installs == 2, "a group 1/2: one install, one answer", n);
		check(sent_is(h->g2, h->g2_length), "the 2/2 is the reference's, byte for byte", n);
		check(!sent_clear && sent[0] == 0x08 && sent[1] == 0x41, "the 2/2 is a Protected data frame, encrypted", n);
		check(memcmp(sent + 4, h->ap, 6) == 0 && memcmp(sent + 10, h->station, 6) == 0 && memcmp(sent + 16, h->ap, 6) == 0,
		    "the 2/2 goes to the AP from us", n);
		check(rsn.gtk_len == 16 && memcmp(rsn.gtk, h->group2, 16) == 0 && rsn.gtk_idx == h->group_index % 3 + 1 &&
		    installed_len == 16 && memcmp(installed, h->group2, 16) == 0 && installed_idx == h->group_index % 3 + 1,
		    "the new group key and its index are the reference's, in the firmware too", n);
		check(strstr(last_line, "new group key installed") != NULL, "the log says so", n);
		check(rsn.state == RSN_KEYED && vayu_rsn_keyed(), "the link stays keyed", n);
		/* the same 1/2 played again, and the AP sending it once more with a newer counter */
		vayu_rsn_eapol_rx(m, h->g1_length);
		check(sends == 4 && installs == 4, "the same group 1/2 again is a replay: ignored", n);
		replay = rsn.replay;
		memcpy(m, h->g1, h->g1_length);
		set_replay(m, replay + 1);
		resign(m, h->g1_length);
		vayu_rsn_eapol_rx(m, h->g1_length);
		check(sends == 5 && installs == 4 && be64(sent + 32 + AT_REPLAY) == replay + 1 &&
		    mic_ok(sent + 32, sent_len - 32, sent + 32 + EAPOL_HDR) && !sent_clear,
		    "a newer 1/2 with the key in use: answered again, not reinstalled", n);
		check(strstr(last_line, "the group key in use kept") != NULL, "the log says the key was kept", n);
		/* a 3/4 is still a 3/4 after a group rekey */
		memcpy(m, h->m3, h->m3_length);
		set_replay(m, rsn.replay + 1);
		resign(m, h->m3_length);
		vayu_rsn_eapol_rx(m, h->m3_length);
		check(sends == 6 && installs == 4, "a genuine 3/4 after a rekey: a 4/4, no reinstall", n);

		vayu_rsn_stop();
		memcpy(m, h->g1, h->g1_length);
		set_replay(m, 1ULL << 62);
		resign(m, h->g1_length);
		vayu_rsn_eapol_rx(m, h->g1_length);
		check(sends == 6 && installs == 4 && rsn.gtk_len == 0, "a stopped RSN answers nothing and keeps no key", n);
	}
}

/* The PMK's file: wifi.pmk in /redwood/etc/vayu-wifi by default; vayu_wifi_pmk_name=NAME another file of that
 * directory, and a name that is not a plain file name (a path, a dot first, too long) leaves the default. */
static void vayu_pmk_file(void)
{
	static int root;
	static const char *const dflt = "/redwood/etc/vayu-wifi/wifi.pmk";
	static const char long_ok[] = "a23456789012345678901234567890123456789012345678901234567890123";       /* 63 */
	static const char long_bad[] = "a234567890123456789012345678901234567890123456789012345678901234";     /* 64 */
	static const struct { const char *name, *path; } cases[] = {
		{ NULL, "/redwood/etc/vayu-wifi/wifi.pmk" },
		{ "home.pmk", "/redwood/etc/vayu-wifi/home.pmk" },
		{ "Net-5G_2.pmk", "/redwood/etc/vayu-wifi/Net-5G_2.pmk" },
		{ long_ok, NULL },
		{ long_bad, "/redwood/etc/vayu-wifi/wifi.pmk" },
		{ "../../etc/master.passwd", "/redwood/etc/vayu-wifi/wifi.pmk" },
		{ "sub/home.pmk", "/redwood/etc/vayu-wifi/wifi.pmk" },
		{ ".hidden", "/redwood/etc/vayu-wifi/wifi.pmk" },
		{ "", "/redwood/etc/vayu-wifi/wifi.pmk" },
		{ "a b.pmk", "/redwood/etc/vayu-wifi/wifi.pmk" },
	};
	char want[256];

	rootvnode = &root;
	boot_pmk_file = 1;
	for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
		snprintf(want, sizeof(want), "%s", cases[n].path != NULL ? cases[n].path : "");
		if (cases[n].path == NULL) {
			snprintf(want, sizeof(want), "/redwood/etc/vayu-wifi/%s", cases[n].name);
		}
		boot_pmk_name = cases[n].name;
		vayu_rsn_reset(quiet_log);
		looked_up[0] = 0;
		check(!vayu_rsn_armed(), "no PMK file there: not armed", (int)n);
		check(strcmp(looked_up, want) == 0, "the PMK is looked for in the file the boot arguments say", (int)n);
		check(strstr(last_line, want) != NULL && strstr(last_line, "not found") != NULL,
		    "the log names the file it did not find", (int)n);
	}
	/* without vayu_wifi_pmk_file=1 no file is read at all */
	boot_pmk_file = -1;
	boot_pmk_name = NULL;
	vayu_rsn_reset(quiet_log);
	looked_up[0] = 0;
	check(!vayu_rsn_armed() && looked_up[0] == 0, "without vayu_wifi_pmk_file=1 nothing is looked up", 0);
	check(strcmp(rsn.pmk_path, dflt) == 0, "the default file is wifi.pmk", 0);
	rootvnode = NULL;
}

int main(void)
{
	vayu_whole();
	vayu_pmk_file();
	printf("vayu RSN: %u checks, %u failed\n", checks, failed);
	return failed != 0;
}
