/*
 * W25: the Wi-Fi data path for vayu — en0 over the HTT data path, DHCP/ARP/ICMP proof (see
 * bsd/dev/vayu_wifi_net.c).  The frame conversion and the interface live there; this is what the
 * station (bsd/dev/vayu_wifi_sta.c) and the scan/HTT layer (bsd/dev/vayu_wifi_scan.c) know of it.
 */
#ifndef _SYS_VAYU_WIFI_NET_H_
#define _SYS_VAYU_WIFI_NET_H_

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

/* Reads vayu_wifi_dhcp and vayu_wifi_keep, clears the state; call after vayu_sta_reset(). */
void vayu_net_reset(int (*log)(const char *format, ...) __printflike(1, 2));
/* The HTT TX function (bsd/dev/vayu_wifi_scan.c) and the log. */
void vayu_net_set_tx(int (*tx80211)(const uint8_t *frame, uint32_t len, bool clear),
    int (*log)(const char *format, ...) __printflike(1, 2));
/* W25o: the station boundary as soon as the MAC is known (before the scan): wifid then finds a
 * powered station, and gets the power event. */
void vayu_net_sta_attach(const uint8_t *mac);
/* After the keys are in: attach en0 on the link and bring it up, tell Apple80211 the station
 * joined. 0 = up.  chan/rssi are what the beacon of the join said. */
int vayu_net_start(const uint8_t *mac, const uint8_t *bssid, const uint8_t *ssid, uint32_t ssid_len,
    uint32_t chan, int32_t rssi);
void vayu_net_stop(void);
bool vayu_net_linked(void);
/* boot-arg vayu_wifi_keep=1: stay joined after the handshake instead of tearing down. */
bool vayu_net_keep(void);
/* One tick of the DHCP/probe state machine: 0 = done (or off), 1 = waiting, -1 = failed. */
int vayu_net_poll(void);
/* One received MSDU from the HTT ring (native wifi or Ethernet, as the firmware decaps it). */
void vayu_net_rx_80211(const uint8_t *frame, uint32_t len, uint32_t decap, uint32_t hdr_len);
void vayu_net_summary(void);

#endif /* _SYS_VAYU_WIFI_NET_H_ */
