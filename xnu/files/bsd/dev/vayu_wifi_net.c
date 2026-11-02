/*
 * W25: the data path after the handshake — en0 on the Wi-Fi link, and (behind vayu_wifi_dhcp)
 * a minimal DHCP client with an ARP/ICMP proof that the network is reachable.  vayu_wifi_dhcp=1
 * starts it at once (a run without userland), vayu_wifi_dhcp=2 only as a fallback: if iOS's
 * configd has not put an IPv4 address on en0 within 30 s of the link coming up, the kernel asks
 * for a lease itself and gives en0 the address and default route (W25o).
 *
 * Frames cross here in Ethernet form: the driver converts to and from the native-wifi shape the
 * firmware's HTT data path carries (24-byte header, to/from DS, LLC/SNAP), and hands what arrives
 * to hakios_net_input() (bsd/dev/hakios.net.c), which is the interface iOS and configd see as en0.
 * With vayu_wifi_dhcp=1 the kernel also asks for a lease itself, so a run without userland still
 * proves the path end to end: DISCOVER/OFFER/REQUEST/ACK, then ARP for the router and one ICMP
 * echo to it.  iOS's configd stays free to do its own DHCP; the two do not interfere (different
 * xids, one lease).
 *
 * Nothing secret is logged: no passphrase, no keys, no addresses beyond what the rest of the
 * Wi-Fi lines already say.
 *
 * MIT License.  Copyright (c) 2026 The Redwood Project (W25 adaptation).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_socket.h>
#include <sys/random.h>
#include <sys/sockio.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/hakios.net.h>
#include <sys/hakios.sta.h>
#include <sys/vayu_wifi_net.h>
#include <sys/vayu_wifi_rsn.h>
#include <sys/vayu_wifi_scan.h>

#define ETH_HLEN                14U
#define ETH_TYPE_IP             0x0800U
#define ETH_TYPE_ARP            0x0806U
#define ETH_TYPE_EAPOL          0x888eU
#define IP_PROTO_ICMP           1U
#define IP_PROTO_UDP            17U
#define DHCP_CLIENT_PORT        68U
#define DHCP_SERVER_PORT        67U
#define DHCP_MAGIC              0x63825363U
#define IP4(a)                  (a)[0], (a)[1], (a)[2], (a)[3]

enum dhcp_state { DHCP_OFF, DHCP_INIT, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_FAILED };

static struct {
	bool started, linked, sta_attached, dhcp_on, keep;
	uint32_t dhcp_mode;             /* 0 off, 1 at once, 2 fallback after 30 s */
	bool fallback_armed, fallback_said, fallback_bound;
	uint64_t link_ms;
	uint32_t chan;                  /* the channel and rssi of the join, for Apple80211's state */
	int32_t rssi;
	uint32_t dhcp_state, dhcp_tries, arp_tries, icmp_tries, probe_stage;
	uint32_t xid;
	/* W25n: addresses stay in the packet's byte order, as four bytes.  They used to be read
	 * with memcpy into a u32 and written back the same way, but the requested-address option
	 * (50) was built from a host-order value, so the REQUEST asked for 41.30.1.10 instead of
	 * 10.1.30.41 and the server NAKed us.  Printing was swapped for the same reason. */
	uint8_t offered_ip[4], server_ip[4], lease_ip[4], mask[4], router[4], dns[4];
	uint32_t dhcp_seen;
	uint64_t next_send_ms, deadline_ms;
	uint32_t tx_frames, tx_bytes, tx_drop, rx_frames, rx_bytes, rx_drop;
	uint8_t mac[6], bssid[6], router_mac[6], ssid[32];
	uint32_t ssid_len;
	int (*tx80211)(const uint8_t *frame, uint32_t len, bool clear);
	int (*log)(const char *format, ...) __printflike(1, 2);
} net;

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000ULL;
}

/* ---- checksums --------------------------------------------------------------------------------- */

static uint32_t
sum16(const uint8_t *p, uint32_t len, uint32_t sum)
{
	while (len > 1) {
		sum += (uint32_t)p[0] << 8 | p[1];
		p += 2;
		len -= 2;
	}
	if (len != 0) {
		sum += (uint32_t)p[0] << 8;
	}
	return sum;
}

static uint16_t
fold(uint32_t sum)
{
	while ((sum >> 16) != 0) {
		sum = (sum & 0xffffU) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

/* ---- sending ----------------------------------------------------------------------------------- */

static bool
send_eth(const uint8_t *frame, uint32_t len)
{
	uint8_t f[1600];
	uint32_t flen;

	if (!net.started || net.tx80211 == NULL || len < ETH_HLEN || len - ETH_HLEN + 32 > sizeof(f)) {
		return false;
	}
	/* Ethernet -> native wifi: 24-byte data header, to DS, LLC/SNAP, then the payload */
	memset(f, 0, 24);
	f[0] = 0x08;
	f[1] = 0x41;                            /* to DS + protected (the pairwise key is in) */
	memcpy(f + 4, net.bssid, 6);            /* addr1: the AP */
	memcpy(f + 10, net.mac, 6);             /* addr2: us */
	memcpy(f + 16, frame, 6);               /* addr3: the destination */
	f[24] = 0xaa;
	f[25] = 0xaa;
	f[26] = 0x03;
	f[27] = 0x00;
	f[28] = 0x00;
	f[29] = 0x00;
	f[30] = frame[12];
	f[31] = frame[13];
	memcpy(f + 32, frame + ETH_HLEN, len - ETH_HLEN);
	flen = len - ETH_HLEN + 32;
	if (net.tx80211(f, flen, false) != 0) {
		net.tx_drop++;
		return false;
	}
	net.tx_frames++;
	net.tx_bytes += len;
	return true;
}

/* ---- DHCP -------------------------------------------------------------------------------------- */

static uint8_t *
dhcp_options(uint8_t *p, uint8_t code, uint8_t len, const uint8_t *data)
{
	*p++ = code;
	*p++ = len;
	if (data != NULL) {
		memcpy(p, data, len);
	} else {
		memset(p, 0, len);
	}
	return p + len;
}

static bool
dhcp_send(uint8_t type)
{
	static uint8_t e[300 + 40], *b, *p;
	uint32_t total;
	uint16_t udp_len;
	uint8_t *ip;

	memset(e, 0, sizeof(e));
	memset(e, 0xff, 6);                     /* broadcast */
	memcpy(e + 6, net.mac, 6);
	e[12] = 0x08;
	e[13] = 0x00;
	ip = e + ETH_HLEN;
	ip[0] = 0x45;
	ip[8] = 64;                             /* ttl */
	ip[9] = (uint8_t)IP_PROTO_UDP;
	/* 0.0.0.0 -> 255.255.255.255 */
	memset(ip + 16, 0xff, 4);
	b = ip + 20 + 8;                        /* BOOTP */
	b[0] = 1;                               /* BOOTREQUEST */
	b[1] = 1;                               /* Ethernet */
	b[2] = 6;
	b[4] = (uint8_t)(net.xid >> 24);
	b[5] = (uint8_t)(net.xid >> 16);
	b[6] = (uint8_t)(net.xid >> 8);
	b[7] = (uint8_t)net.xid;
	b[10] = 0x80;                           /* broadcast flag */
	memcpy(b + 28, net.mac, 6);
	p = b + 236;
	*p++ = 0x63;
	*p++ = 0x82;
	*p++ = 0x53;
	*p++ = 0x63;
	p = dhcp_options(p, 53, 1, &type);
	if (type == 3) {                        /* REQUEST */
		p = dhcp_options(p, 50, 4, net.offered_ip);
		p = dhcp_options(p, 54, 4, net.server_ip);
	}
	static const uint8_t want[4] = { 1, 3, 6, 15 };
	p = dhcp_options(p, 55, sizeof(want), want);
	*p++ = 255;
	total = (uint32_t)(p - ip);             /* IP total length */
	ip[2] = (uint8_t)(total >> 8);
	ip[3] = (uint8_t)total;
	{
		uint16_t sum = fold(sum16(ip, 20, 0));

		ip[10] = (uint8_t)(sum >> 8);
		ip[11] = (uint8_t)sum;
	}
	udp_len = (uint16_t)(total - 20);
	ip[20] = 0;
	ip[21] = (uint8_t)DHCP_CLIENT_PORT;
	ip[22] = 0;
	ip[23] = (uint8_t)DHCP_SERVER_PORT;
	ip[24] = (uint8_t)(udp_len >> 8);
	ip[25] = (uint8_t)udp_len;
	return send_eth(e, ETH_HLEN + total);
}


/* ---- W25o: give the lease to en0 (the kernel's fallback when iOS's configd never got one) ------ */

/* The same bytes as net/if.h's ifaliasreq, which in_control() casts SIOCAIFADDR to (vayu_usbnet_ecm.c). */
struct in_aliasreq {
	char ifra_name[IFNAMSIZ];
	struct sockaddr_in ifra_addr;
	struct sockaddr_in ifra_broadaddr;
	struct sockaddr_in ifra_mask;
};

static uint32_t
ip4_host(const uint8_t a[4])
{
	return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
}

static void
net_assign_address(void)
{
	struct in_aliasreq ifra;
	struct sockaddr_in dst, rtmask, gateway;
	struct ifreq ifr;
	struct sockaddr_in *sin;
	socket_t so = NULL;
	uint32_t ip = ip4_host(net.lease_ip), mask = ip4_host(net.mask);
	errno_t error;

	if (ip == 0) {
		return;
	}
	if (mask == 0) {
		mask = 0xffffff00U;
	}
	error = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	if (error != 0) {
		net.log("vayu-wifi: W25 S5 dhcp: ERROR no socket to give en0 its address: %d\n", error);
		return;
	}
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, "en0", sizeof(ifr.ifr_name));
	(void)sock_ioctl(so, SIOCPROTOATTACH, &ifr);

	memset(&ifra, 0, sizeof(ifra));
	strlcpy(ifra.ifra_name, "en0", sizeof(ifra.ifra_name));
	sin = &ifra.ifra_addr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(ip);
	sin = &ifra.ifra_mask;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(mask);
	sin = &ifra.ifra_broadaddr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(ip | ~mask);
	error = sock_ioctl(so, SIOCAIFADDR, &ifra);
	net.log("vayu-wifi: W25 S5 dhcp: %s en0 %u.%u.%u.%u/%u.%u.%u.%u: %d\n",
	    error == 0 ? "OK address" : "ERROR address", IP4(net.lease_ip), IP4(net.mask), error);

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, "en0", sizeof(ifr.ifr_name));
	if (sock_ioctl(so, SIOCGIFFLAGS, &ifr) == 0) {
		ifr.ifr_flags |= IFF_UP;
		(void)sock_ioctl(so, SIOCSIFFLAGS, &ifr);
	}
	sock_close(so);

	if (ip4_host(net.router) != 0) {
		memset(&dst, 0, sizeof(dst));
		dst.sin_len = sizeof(dst);
		dst.sin_family = AF_INET;
		memset(&rtmask, 0, sizeof(rtmask));
		rtmask.sin_len = sizeof(rtmask);
		rtmask.sin_family = AF_INET;
		memset(&gateway, 0, sizeof(gateway));
		gateway.sin_len = sizeof(gateway);
		gateway.sin_family = AF_INET;
		gateway.sin_addr.s_addr = htonl(ip4_host(net.router));
		error = rtrequest(RTM_ADD, (struct sockaddr *)(void *)&dst, (struct sockaddr *)(void *)&gateway,
		    (struct sockaddr *)(void *)&rtmask, RTF_UP | RTF_GATEWAY | RTF_STATIC, NULL);
		net.log("vayu-wifi: W25 S5 dhcp: %s default route via %u.%u.%u.%u: %d\n",
		    error == 0 ? "OK" : "ERROR", IP4(net.router), error);
	}
}

/* a DHCP message for our xid, from the server's port to ours */
static void
dhcp_rx(const uint8_t *e, uint32_t len)
{
	const uint8_t *ip = e + ETH_HLEN, *udp, *b, *p, *end;
	uint32_t ihl, xid;
	uint8_t type = 0;

	if (len < ETH_HLEN + 20 + 8 + 240 || e[12] != 0x08 || e[13] != 0x00) {
		return;
	}
	ihl = (uint32_t)(ip[0] & 0x0f) * 4;
	if (ip[0] >> 4 != 4 || ihl < 20 || ip[9] != IP_PROTO_UDP) {
		return;
	}
	udp = ip + ihl;
	if (((udp[0] << 8 | udp[1]) != DHCP_SERVER_PORT) || ((udp[2] << 8 | udp[3]) != DHCP_CLIENT_PORT)) {
		return;
	}
	b = udp + 8;
	if (len < (uint32_t)(b - e) + 240 || b[0] != 2) {
		return;
	}
	xid = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
	if (net.dhcp_seen < 8) {
		net.dhcp_seen++;
		net.log("vayu-wifi: W25 S5 dhcp: frame from %u.%u.%u.%u:%u to :%u xid %s\n",
		    ip[12], ip[13], ip[14], ip[15], (uint32_t)udp[0] << 8 | udp[1],
		    (uint32_t)udp[2] << 8 | udp[3], xid == net.xid ? "ours" : "other");
	}
	if (xid != net.xid) {
		return;
	}
	p = b + 236;                            /* magic cookie */
	if (p[0] != 0x63 || p[1] != 0x82 || p[2] != 0x53 || p[3] != 0x63) {
		return;
	}
	p = b + 240;
	end = e + len;                         /* every option has to lie inside the frame */
	for (; p + 2 <= end && *p != 255 && p + 2 + p[1] <= end; p += 2 + p[1]) {
		if (p[0] == 53 && p[1] >= 1) {
			type = p[2];
		} else if (p[0] == 1 && p[1] == 4) {
			memcpy(net.mask, p + 2, 4);
		} else if (p[0] == 3 && p[1] >= 4) {
			memcpy(net.router, p + 2, 4);
		} else if (p[0] == 6 && p[1] >= 4) {
			memcpy(net.dns, p + 2, 4);
		}
	}
	if (type == 2 && net.dhcp_state == DHCP_SELECTING) {     /* OFFER */
		memcpy(net.offered_ip, b + 16, 4);
		memset(net.server_ip, 0, 4);                    /* server id is an option */
		for (p = b + 240; p + 2 <= end && *p != 255 && p + 2 + p[1] <= end; p += 2 + p[1]) {
			if (p[0] == 54 && p[1] >= 4) {
				memcpy(net.server_ip, p + 2, 4);
			}
		}
		net.dhcp_state = DHCP_REQUESTING;
		net.dhcp_tries = 0;
		net.next_send_ms = now_ms();
		net.log("vayu-wifi: W25 S5 dhcp: OK offer %u.%u.%u.%u from %u.%u.%u.%u\n",
		    IP4(net.offered_ip), IP4(net.server_ip));
	} else if (type == 5 && (net.dhcp_state == DHCP_REQUESTING || net.dhcp_state == DHCP_SELECTING)) {  /* ACK */
		memcpy(net.lease_ip, b + 16, 4);
		net.dhcp_state = DHCP_BOUND;
		net.probe_stage = 1;
		net.arp_tries = 0;
		net.next_send_ms = now_ms();
		net.log("vayu-wifi: W25 S5 dhcp: OK bound %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u%s\n",
		    IP4(net.lease_ip), IP4(net.mask), IP4(net.router), IP4(net.dns),
		    net.fallback_bound ? " (fallback)" : "");
		if (net.fallback_bound || net.dhcp_mode == 1) {
			net_assign_address();
		}
	} else if (type == 6) {                 /* NAK */
		net.dhcp_state = DHCP_FAILED;
		net.log("vayu-wifi: W25 S5 dhcp: ERROR nak from the server\n");
	}
}

/* ---- ARP and ICMP to the router ---------------------------------------------------------------- */

static bool
send_arp(void)
{
	uint8_t e[42];

	memset(e, 0xff, 6);
	memcpy(e + 6, net.mac, 6);
	e[12] = 0x08;
	e[13] = 0x06;
	e[14] = 0x00;
	e[15] = 0x01;                           /* Ethernet */
	e[16] = 0x08;
	e[17] = 0x00;                           /* IPv4 */
	e[18] = 6;
	e[19] = 4;
	e[20] = 0x00;
	e[21] = 0x01;                           /* request */
	memcpy(e + 22, net.mac, 6);
	memcpy(e + 28, net.lease_ip, 4);
	memset(e + 32, 0, 6);
	memcpy(e + 38, net.router, 4);
	return send_eth(e, sizeof(e));
}

static void
arp_rx(const uint8_t *e, uint32_t len)
{
	if (len < 42 || e[20] != 0 || e[21] != 2) {
		return;
	}
	if (memcmp(e + 38, net.lease_ip, 4) == 0 && net.probe_stage == 1) {
		memcpy(net.router_mac, e + 22, 6);
		net.probe_stage = 2;
		net.icmp_tries = 0;
		net.next_send_ms = now_ms();
		net.log("vayu-wifi: W25 S6 probe: OK arp reply from the router %u.%u.%u.%u\n", IP4(net.router));
	}
}

static bool
send_icmp(void)
{
	uint8_t e[14 + 20 + 8 + 32];
	uint8_t *ip = e + ETH_HLEN, *icmp = ip + 20;
	uint16_t sum;
	uint32_t total = 20 + 8 + 32;

	memset(e, 0, sizeof(e));
	/* the router's MAC from its ARP reply; if that never came, the AP's address (a home router is
	 * usually the access point here) */
	memcpy(e, net.probe_stage >= 2 ? net.router_mac : net.bssid, 6);
	memcpy(e + 6, net.mac, 6);
	e[12] = 0x08;
	e[13] = 0x00;
	ip[0] = 0x45;
	ip[2] = (uint8_t)(total >> 8);
	ip[3] = (uint8_t)total;
	ip[8] = 64;
	ip[9] = (uint8_t)IP_PROTO_ICMP;
	memcpy(ip + 12, net.lease_ip, 4);
	memcpy(ip + 16, net.router, 4);
	{
		uint16_t sum = fold(sum16(ip, 20, 0));

		ip[10] = (uint8_t)(sum >> 8);
		ip[11] = (uint8_t)sum;
	}
	icmp[0] = 8;
	icmp[4] = 0x25;
	icmp[5] = 0x25;
	icmp[6] = 0;
	icmp[7] = 1;
	for (uint32_t i = 0; i < 32; i++) {
		icmp[8 + i] = (uint8_t)i;
	}
	sum = fold(sum16(icmp, 8 + 32, 0));
	icmp[2] = (uint8_t)(sum >> 8);
	icmp[3] = (uint8_t)sum;
	return send_eth(e, sizeof(e));
}

static void
icmp_rx(const uint8_t *e, uint32_t len)
{
	const uint8_t *ip = e + ETH_HLEN, *icmp;
	uint32_t ihl;

	if (len < ETH_HLEN + 20 + 8 || e[12] != 0x08 || e[13] != 0x00 || ip[9] != IP_PROTO_ICMP) {
		return;
	}
	ihl = (uint32_t)(ip[0] & 0x0f) * 4;
	icmp = ip + ihl;
	if (icmp[0] == 0 && net.probe_stage == 2) {      /* echo reply */
		net.probe_stage = 3;
		net.log("vayu-wifi: W25 S6 probe: OK icmp echo reply from the router\n");
	}
}

/* ---- RX from the HTT ring ---------------------------------------------------------------------- */

void
vayu_net_rx_80211(const uint8_t *f, uint32_t len, uint32_t decap, uint32_t hdr)
{
	static uint8_t e[1600];
	const uint8_t *da, *sa, *llc;
	uint32_t elen;

	if (!net.started || len < 16) {
		net.rx_drop++;
		return;
	}
	if (decap == 2 || decap == 3) {         /* the firmware already gave us Ethernet */
		if (len > sizeof(e)) {
			net.rx_drop++;
			return;
		}
		memcpy(e, f, len);
		elen = len;
	} else {
		if (hdr < 24 || len < hdr + 8 || (f[0] & 0x0c) != 0x08) {
			return;                 /* not a data frame (management, control) */
		}
		da = f + 4;
		sa = (f[1] & 0x02) != 0 ? f + 16 : f + 10;      /* from DS: addr3 is the source */
		llc = f + hdr;
		if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03 || len - hdr - 8 > sizeof(e) - ETH_HLEN) {
			net.rx_drop++;
			return;
		}
		memcpy(e, da, 6);
		memcpy(e + 6, sa, 6);
		e[12] = llc[6];
		e[13] = llc[7];
		memcpy(e + ETH_HLEN, llc + 8, len - hdr - 8);
		elen = len - hdr - 8 + ETH_HLEN;
	}
	net.rx_frames++;
	net.rx_bytes += elen;
	if (elen >= ETH_HLEN && e[12] == 0x88 && e[13] == 0x8e) {
		return;                         /* EAPOL goes to the handshake, not to the stack */
	}
	hakios_net_input(e, elen);
	if (e[12] == 0x08 && e[13] == 0x00) {
		dhcp_rx(e, elen);
		icmp_rx(e, elen);
	} else if (e[12] == 0x08 && e[13] == 0x06) {
		arp_rx(e, elen);
	}
}

/* ---- the interface's transmit callback --------------------------------------------------------- */

static bool
net_transmit(mbuf_t packet, size_t length)
{
	static uint8_t e[1600];

	if (length < ETH_HLEN || length > sizeof(e)) {
		return false;
	}
	if (mbuf_copydata(packet, 0, length, e) != 0) {
		return false;
	}
	return send_eth(e, (uint32_t)length);
}

/* ---- the station boundary (what wifid's Apple80211 requests are answered from) ------------------ */

static int
sta_power(bool on)
{
	net.log("vayu-wifi: W25 sta: power %u (taken; the radio is up)\n", on ? 1 : 0);
	return 0;
}

static int
sta_scan(const struct hakios_sta_scan *what)
{
	(void)what;
	return EOPNOTSUPP;              /* the scan is the boot-arg one; wifid is told so */
}

static void
sta_heard(void (*each)(const struct hakios_sta_bss *bss, void *context), void *context)
{
	(void)each;
	(void)context;
}

static int
sta_join(const struct hakios_sta_join *what)
{
	if (what == NULL || what->ssid_length != net.ssid_len || memcmp(what->ssid, net.ssid, net.ssid_len) != 0) {
		return ENOENT;
	}
	return net.linked ? 0 : EAGAIN;
}

static int
sta_leave(void)
{
	return 0;
}

static void
sta_state(struct hakios_sta_state *state)
{
	if (state == NULL) {
		return;
	}
	memset(state, 0, sizeof(*state));
	state->phase = net.linked ? HAKIOS_STA_JOINED :
	    (net.sta_attached || net.started ? HAKIOS_STA_IDLE : HAKIOS_STA_OFF);
	memcpy(state->bss.bssid, net.bssid, 6);
	memcpy(state->bss.ssid, net.ssid, net.ssid_len);
	state->bss.ssid_length = (uint8_t)net.ssid_len;
	state->bss.frequency = (uint16_t)(net.chan <= 13 ? 2407 + net.chan * 5 :
	    net.chan == 14 ? 2484 : 5000 + net.chan * 5);
	state->bss.rssi = (int16_t)net.rssi;
}

static unsigned
sta_channels(struct hakios_sta_channel *channels, unsigned most)
{
	(void)channels;
	(void)most;
	return 0;
}

/* version of the station ops: the address is a copy, filled at attach */
static struct hakios_sta_ops net_sta_ops;

/* ---- start / stop ------------------------------------------------------------------------------- */

void
vayu_net_reset(int (*log)(const char *format, ...) __printflike(1, 2))
{
	uint32_t v = 0;

	memset(&net, 0, sizeof(net));
	net.log = log != NULL ? log : printf;
	PE_parse_boot_argn("vayu_wifi_dhcp", &v, sizeof(v));
	net.dhcp_mode = v <= 2 ? v : 2;         /* (an unknown value behaves as the fallback) */
	net.dhcp_on = net.dhcp_mode != 0;
	v = 0;
	PE_parse_boot_argn("vayu_wifi_keep", &v, sizeof(v));
	net.keep = v != 0;
}

bool
vayu_net_keep(void)
{
	return net.keep;
}

void
vayu_net_set_tx(int (*tx80211)(const uint8_t *frame, uint32_t len, bool clear),
    int (*log)(const char *format, ...) __printflike(1, 2))
{
	net.tx80211 = tx80211;
	if (log != NULL) {
		net.log = log;
	}
}

/* W25o: the radio is there as soon as the MAC is known (before the scan and the join), so
 * wifid finds a powered station instead of an interface with nothing behind it. */
void
vayu_net_sta_attach(const uint8_t *mac)
{
	if (net.mac[0] != 0 && mac != NULL) {
		return;                         /* (already attached) */
	}
	if (mac != NULL) {
		memcpy(net.mac, mac, 6);
	}
	memset(&net_sta_ops, 0, sizeof(net_sta_ops));
	net_sta_ops.power = sta_power;
	net_sta_ops.scan = sta_scan;
	net_sta_ops.heard = sta_heard;
	net_sta_ops.join = sta_join;
	net_sta_ops.leave = sta_leave;
	net_sta_ops.state = sta_state;
	net_sta_ops.channels = sta_channels;
	memcpy(net_sta_ops.address, net.mac, 6);
	hakios_sta_attach(&net_sta_ops);
	net.sta_attached = true;
	hakios_apple80211_event(HAKIOS_STA_POWER, 1);
}

/* Called after the keys are in: en0 comes up and the data path is open. */
int
vayu_net_start(const uint8_t *mac, const uint8_t *bssid, const uint8_t *ssid, uint32_t ssid_len,
    uint32_t chan, int32_t rssi)
{
	static struct hakios_net_link wifi_link;
	int error;

	if (net.started) {
		return 0;
	}
	memcpy(net.mac, mac, 6);
	memcpy(net.bssid, bssid, 6);
	net.ssid_len = ssid_len > sizeof(net.ssid) ? (uint32_t)sizeof(net.ssid) : ssid_len;
	memcpy(net.ssid, ssid, net.ssid_len);
	net.chan = chan;
	net.rssi = rssi;

	memset(&wifi_link, 0, sizeof(wifi_link));
	wifi_link.name = "vayu-wifi";
	wifi_link.unit = 0;
	memcpy(wifi_link.address, net.mac, 6);
	wifi_link.wireless = true;
	wifi_link.up = false;
	wifi_link.transmit = net_transmit;
	wifi_link.poll = NULL;

	if (!hakios_net_attach(&wifi_link)) {
		net.log("vayu-wifi: W25 S4 link: ERROR en0 not attached\n");
		return -1;
	}
	net.started = true;
	net.linked = true;
	hakios_net_link_state(true);
	net.log("vayu-wifi: W25 S4 link: OK en0 up, MAC xx:xx:xx:xx:%02x:%02x, 802.11 to xx:xx:xx:xx:%02x:%02x\n",
	    net.mac[4], net.mac[5], net.bssid[4], net.bssid[5]);
	/* W25o: wifid learns that the station joined from these events as well as from the state
	 * request; without them the interface is "up" but nothing tells iOS's Wi-Fi side that a
	 * network is associated. */
	hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, 0);
	hakios_apple80211_event(HAKIOS_STA_LINK, 1);
	net.link_ms = now_ms();
	if (net.dhcp_mode == 1) {
		net.dhcp_state = DHCP_INIT;
		net.dhcp_tries = 0;
		read_random(&net.xid, sizeof(net.xid));
		net.next_send_ms = now_ms();
		net.log("vayu-wifi: W25 S5 dhcp: OK client on (xid %#x)\n", net.xid);
	} else if (net.dhcp_mode == 2) {
		net.fallback_armed = true;
		net.log("vayu-wifi: W25 S5 dhcp: OK fallback armed: iOS has 30 s, then the kernel asks\n");
	}
	return 0;
}

void
vayu_net_stop(void)
{
	if (net.linked) {
		net.linked = false;
		hakios_net_link_state(false);
	}
}

bool
vayu_net_linked(void)
{
	return net.linked;
}

/* ---- the 100 ms tick of the station loop -------------------------------------------------------- */

int
vayu_net_poll(void)
{
	uint64_t now = now_ms();

	/* W25o: the fallback (vayu_wifi_dhcp=2): give iOS's configd 30 s, then ask ourselves --
	 * but only if en0 really has no IPv4 address by then. */
	if (net.dhcp_mode == 2 && net.dhcp_state == DHCP_OFF && net.fallback_armed &&
	    now - net.link_ms >= 30000) {
		net.fallback_armed = false;
		if (hakios_net_has_ipv4(0)) {
			if (!net.fallback_said) {
				net.fallback_said = true;
				net.log("vayu-wifi: W25 S5 dhcp: OK iOS has an IPv4 address on en0, no fallback\n");
			}
		} else {
			net.fallback_said = true;
			net.fallback_bound = true;
			net.dhcp_state = DHCP_INIT;
			net.dhcp_tries = 0;
			read_random(&net.xid, sizeof(net.xid));
			net.next_send_ms = now;
			net.log("vayu-wifi: W25 S5 dhcp: ERROR no IPv4 on en0 after 30 s, fallback client on (xid %#x)\n",
			    net.xid);
		}
	}
	if (!net.dhcp_on || net.dhcp_state == DHCP_OFF || net.dhcp_state == DHCP_FAILED) {
		return net.dhcp_state == DHCP_BOUND ? 0 : (net.dhcp_state == DHCP_FAILED ? -1 : 1);
	}
	now = now_ms();
	if (net.dhcp_state == DHCP_BOUND) {
		if (net.probe_stage < 3 && now >= net.next_send_ms) {
			if (net.probe_stage == 1) {
				if (net.arp_tries++ < 3) {
					(void)send_arp();
				} else {
					net.log("vayu-wifi: W25 S6 probe: ERROR no arp reply from the router\n");
				}
				net.next_send_ms = now + 1000;
			} else if (net.probe_stage == 2) {
				if (net.icmp_tries++ < 3) {
					(void)send_icmp();
				} else {
					net.log("vayu-wifi: W25 S6 probe: ERROR no icmp reply from the router\n");
				}
				net.next_send_ms = now + 1000;
			}
		}
		return net.probe_stage >= 3 ? 0 : 1;
	}
	if (now < net.next_send_ms) {
		return 1;
	}
	if (net.dhcp_state == DHCP_INIT) {
		if (net.dhcp_tries++ >= 5) {
			net.dhcp_state = DHCP_FAILED;
			net.log("vayu-wifi: W25 S5 dhcp: ERROR no offer in %u tries\n", net.dhcp_tries);
			return -1;
		}
		(void)dhcp_send(1);             /* DISCOVER */
		net.dhcp_state = DHCP_SELECTING;
		net.next_send_ms = now + 2000;
		return 1;
	}
	if (net.dhcp_state == DHCP_SELECTING) {
		if (net.dhcp_tries++ >= 5) {
			net.dhcp_state = DHCP_FAILED;
			net.log("vayu-wifi: W25 S5 dhcp: ERROR no offer in %u tries\n", net.dhcp_tries);
			return -1;
		}
		(void)dhcp_send(1);
		net.next_send_ms = now + 2000;
		return 1;
	}
	if (net.dhcp_state == DHCP_REQUESTING) {
		if (net.dhcp_tries++ >= 5) {
			net.dhcp_state = DHCP_FAILED;
			net.log("vayu-wifi: W25 S5 dhcp: ERROR no ack in %u tries\n", net.dhcp_tries);
			return -1;
		}
		(void)dhcp_send(3);             /* REQUEST */
		net.next_send_ms = now + 2000;
		return 1;
	}
	return 1;
}

void
vayu_net_summary(void)
{
	net.log("vayu-wifi: W25 data: frames tx %u (%u bytes, %u dropped), rx %u (%u bytes, %u dropped); "
	    "dhcp %s, probe %s\n", net.tx_frames, net.tx_bytes, net.tx_drop, net.rx_frames, net.rx_bytes,
	    net.rx_drop, net.dhcp_state == DHCP_BOUND ? "bound" : (net.dhcp_on ? "no lease" : "off"),
	    net.probe_stage >= 3 ? "ok" : "no");
}
