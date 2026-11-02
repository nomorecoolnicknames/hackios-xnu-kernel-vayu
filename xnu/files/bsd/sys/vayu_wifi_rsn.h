/*
 * W25: WPA2-PSK (RSN) 4-way handshake for the vayu station, and the data path that follows it.
 *
 * The PMK comes from /private/etc/vayu-wifi/gunwest.pmk (64 hex digits, one line), read when the
 * handshake arms and only with the boot argument vayu_wifi_pmk_file=1: boot arguments are logged by
 * the loader, the kernel, launchd and a panic buffer, so no secret may travel in one.  The handshake is IEEE 802.11i / 802.1X-2004 with key descriptor
 * version 2: PRF-SHA1 for the PTK, HMAC-SHA1-128 for the MIC, RFC 3394 AES key unwrap for the
 * GTK.  Crypto goes through libkern's registered provider (osfmk/minicrypto in this kernel):
 * g_crypto_funcs->cchmac_fn + ccsha1_di, and g_crypto_funcs->ccaes_ecb_decrypt.  A self-test
 * (RFC 2202 HMAC-SHA1, RFC 3394 unwrap) runs once before anything is used; a failure stops the
 * handshake instead of installing a wrong key.
 *
 * EAPOL frames reach this file from the HTT rx path (bsd/dev/vayu_wifi_scan.c), frames of the
 * data path from vayu_wifi_net.c.
 */
#ifndef _SYS_VAYU_WIFI_RSN_H_
#define _SYS_VAYU_WIFI_RSN_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

/* What the station (bsd/dev/vayu_wifi_sta.c) and the HTC/HTT layer provide. */
struct vayu_rsn_ops {
	int (*log)(const char *format, ...) __printflike(1, 2);
	/* One WMI message (cmd id + TLVs) on the WMI endpoint. */
	int (*wmi_send)(const uint8_t *msg, uint32_t len, uint32_t wait_ms);
	/* One native-wifi data frame on the HTT endpoint; clear = send unencrypted (EAPOL). */
	int (*tx_data)(const uint8_t *frame, uint32_t len, bool clear);
	const uint8_t *mac;             /* our address, from WMI READY (6 bytes) */
	const uint8_t *bssid;           /* the AP's (6 bytes) */
	const uint8_t *rsn_ie;          /* the RSN IE of our assoc request */
	uint32_t rsn_ie_len;
	uint32_t group_cipher;          /* WMI_CIPHER_*: 2 TKIP, 4 CCMP */
};

/* Reads vayu_wifi_pmk_file and clears the state; call after vayu_sta_reset(). */
void vayu_rsn_reset(int (*log)(const char *format, ...) __printflike(1, 2));
/* True when a PMK is set and the crypto self-test passed: the handshake can run. */
bool vayu_rsn_armed(void);
/* Wipe the keys (VDEV_STOP, a failed join, the end of the run). */
void vayu_rsn_stop(void);

/* Start waiting for message 1/4 of the handshake. */
void vayu_rsn_start(const struct vayu_rsn_ops *ops);
/* One EAPOL frame (starting at the EAPOL version byte), only while started. */
void vayu_rsn_eapol_rx(const uint8_t *eapol, uint32_t len);
/* 0 = keys installed, 1 = still waiting, -1 = failed. */
int vayu_rsn_poll(void);
/* The pairwise key is installed (frames may pass). */
bool vayu_rsn_keyed(void);

#endif /* _SYS_VAYU_WIFI_RSN_H_ */
