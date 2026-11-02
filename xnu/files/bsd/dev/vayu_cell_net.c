/*
 * en1: the modem's raw IPv4 data path as a real interface, for the POCO X3 Pro (vayu, SM8150-AC).
 *
 * The control plane (bsd/dev/vayu_cell_data.c) leaves an IPv4 session behind: WDS says the
 * address, mask, gateway, DNS and MTU, and WDA says the packets are raw IP inside QMAP on mux 1.
 * This file is the smallest interface that can carry that: no Ethernet header, no ARP, no DHCP,
 * no link-layer address, no CommCenter and no configd. en1 is IFNET_FAMILY_CELLULAR/IFT_CELLULAR,
 * a point-to-point link whose peer is the WDS gateway, BPF sees DLT_RAW, and inbound packets are
 * handed to PF_INET with proto_input() after the QMAP layer has removed its 4-byte header
 * (sys/vayu_cell_qmap.h). en0 and en2 are not touched.
 *
 * The shape of the address setup is the proven one of bsd/dev/vayu_usbnet_ecm.c (itself the
 * ifconfig path): SIOCPROTOATTACH for PF_INET through the plumber this file registers, then
 * SIOCAIFADDR with the address, the netmask, and -- because the link is point-to-point -- the
 * gateway in ifra_broadaddr, then SIOCGIFFLAGS/SIOCSIFFLAGS with IFF_UP. IFF_RUNNING is added
 * afterwards, directly and only after the address and peer were installed, so no KEV_DL_LINK_ON
 * is ever posted. bsd/net/if_gif.c is the model for the raw, headerless interface: ifnet_demux
 * reads the IP version nibble, ifnet_attach_protocol registers an input that only calls
 * proto_input(), and BPF is attached with DLT_RAW. Unlike gif there is no encapsulation and no
 * ifra_dstaddr configuration here.
 *
 * What this file does NOT do, and does not pretend to: it does not make the IPA data path work
 * (vayu_ipa_data_transmit() is the main agent's side), it does not tell configd or CommCenter that
 * there is a network service, it does not publish DNS, and it adds no default route unless
 * vayu_cell_net_default=1. IFF_RUNNING means "the address and peer were installed", not "the
 * Internet works". No KEV_DL_LINK_ON/OFF is posted, so no DHCP client is told that a link came up.
 *
 * Boot args:
 *   vayu_cell_net=0          nothing (the default)
 *   vayu_cell_net=1          attach en1 and start the configuration thread
 *   vayu_cell_net_default=1  also add a global default route via the session's gateway
 *
 * Threading: vayu_cell_net_session() only copies its arguments under a mutex and wakes the thread;
 * vayu_cell_net_input() and the output callback never sleep, never open a socket and never log.
 * All socket/ioctl/route work happens in the one thread this file starts. Teardown only removes
 * the route and the address it added and clears the flags; the ifnet stays attached, so a later
 * session can reuse it, and every teardown step is tolerant of being already done.
 */
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <net/route.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/param.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/vayu_cell_net.h>

extern void loader_log_putc(char c);
/* The main agent's IPA transmit; it must copy the bytes before returning (the mbuf is freed below). */
extern bool vayu_ipa_data_transmit(uint8_t mux, const void *packet, size_t length);
/* bsd/net/route.c; net/route.h keeps the flags but not the prototype. */
extern int rtrequest(int req, struct sockaddr *dst, struct sockaddr *gateway,
    struct sockaddr *netmask, int flags, struct rtentry **ret_nrt);

#define CELL_NET_NAME           "en1"
#define CELL_NET_PREFIX         "en"
#define CELL_NET_UNIT           1
#define CELL_NET_UNIQUEID       "vayu-cell-net"
#define CELL_NET_IPV4_MIN       20      /* the smallest IPv4 header */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
}

/* Same index line shape as vayu_cell_data.c: vayu-data: S9 EN1 IFNET: OK|ERROR ... */
static void
cell_net_stage(const char *result, const char *format, ...)
{
	char detail[180], line[260];
	va_list list;

	va_start(list, format);
	vsnprintf(detail, sizeof(detail), format, list);
	va_end(list);
	snprintf(line, sizeof(line), "vayu-data: S9 EN1 IFNET: %s %s\n", result, detail);
	for (const char *c = line; *c != 0; c++) loader_log_putc(*c);
	printf("%s", line);
}

/* The WDS numbers are host order; the log shows them the way ip4_string does in vayu_cell_data.c. */
static void
ip4_string(char *to, size_t size, uint32_t address)
{
	snprintf(to, size, "%u.%u.%u.%u", (address >> 24) & 0xff, (address >> 16) & 0xff,
	    (address >> 8) & 0xff, address & 0xff);
}

/* ---- state --------------------------------------------------------------------------------------- */

struct cell_net_request {
	bool     up;
	uint32_t address;
	uint32_t mask;
	uint32_t gateway;
	uint32_t dns1;
	uint32_t dns2;
	uint32_t mtu;
	uint32_t generation;
};

/* What the interface currently has; owned by the configuration thread, copied under the lock. */
struct cell_net_applied {
	bool     address_set;
	bool     route_set;
	bool     fully_configured;
	uint32_t address;
	uint32_t mask;
	uint32_t gateway;
	uint32_t dns1;
	uint32_t dns2;
	uint32_t mtu;
};

static struct {
	ifnet_t ifp;
	bool    started;
	bool    thread_started;
	bool    default_route;
	struct cell_net_request request;
	bool     pending;
	uint32_t request_generation;    /* the last request copied into `request` */
	struct cell_net_applied applied;
	uint32_t applied_generation;
	/* statistics; approximate by nature, the data path does not take the lock */
	volatile uint64_t rx_packets, rx_bytes, rx_dropped;
	volatile uint64_t tx_packets, tx_bytes, tx_dropped;
} cell_net;

static LCK_GRP_DECLARE(cell_net_lock_group, "vayu-cell-net");
static LCK_MTX_DECLARE(cell_net_lock, &cell_net_lock_group);

/* ---- helpers ------------------------------------------------------------------------------------- */

static bool
cell_net_mask_valid(uint32_t mask)
{
	uint32_t host;

	if (mask == 0) {
		return false;
	}
	host = ~mask;
	return (host & (host + 1)) == 0;        /* the host part is 2^k - 1 */
}

/*
 * A complete IPv4 datagram: version 4, a header that fits, a total length that is at least the
 * header and no larger than what was handed over. `length` comes back as the datagram's own total
 * length; the module trims trailing bytes (padding that survived a QMAP layer) rather than passing
 * them to IP.
 */
static bool
cell_net_ipv4_valid(const uint8_t *packet, size_t available, size_t *length)
{
	size_t header_length, total_length;

	if (packet == NULL || available < CELL_NET_IPV4_MIN || (packet[0] >> 4) != 4) {
		return false;
	}
	header_length = (size_t)(packet[0] & 0x0f) * 4;
	if (header_length < CELL_NET_IPV4_MIN || header_length > available) {
		return false;
	}
	total_length = ((size_t)packet[2] << 8) | packet[3];
	if (total_length < header_length || total_length > available) {
		return false;
	}
	if (length != NULL) {
		*length = total_length;
	}
	return true;
}

static bool
cell_net_open_socket(socket_t *so)
{
	errno_t error;

	if (*so != NULL) {
		return true;
	}
	error = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, so);
	if (error != 0) {
		*so = NULL;
		say("vayu-cell-net: no PF_INET socket: %d\n", error);
		return false;
	}
	return true;
}

/*
 * The usbnet pattern's last step: SIOCGIFFLAGS/SIOCSIFFLAGS with IFF_UP. This is what lets the
 * rest of the stack see an administratively up interface; IFF_RUNNING is not part of it and is
 * added separately, only when the address and peer were really installed.
 */
static int
cell_net_set_up(socket_t so)
{
	struct ifreq ifr;
	int error;

	bzero(&ifr, sizeof(ifr));
	strlcpy(ifr.ifr_name, CELL_NET_NAME, sizeof(ifr.ifr_name));
	error = sock_ioctl(so, SIOCGIFFLAGS, &ifr);
	if (error == 0) {
		ifr.ifr_flags |= IFF_UP;
		error = sock_ioctl(so, SIOCSIFFLAGS, &ifr);
	}
	return error;
}

/* ---- the address, route and flags (only ever called from cell_net_thread) ------------------------ */

/*
 * struct in_aliasreq as bsd/netinet/in.c casts the SIOCAIFADDR argument: three sockaddr_in after
 * the name, the second one being the point-to-point destination (ifra_broadaddr in if.h).
 */
struct cell_net_aliasreq {
	char ifra_name[IFNAMSIZ];
	struct sockaddr_in ifra_addr;
	struct sockaddr_in ifra_broadaddr;
	struct sockaddr_in ifra_mask;
};

static int
cell_net_address_set(socket_t so, const struct cell_net_request *request)
{
	struct cell_net_aliasreq ifra;
	struct sockaddr_in *sin;
	bool numbered = request->gateway == 0;
	int error;

	bzero(&ifra, sizeof(ifra));
	strlcpy(ifra.ifra_name, CELL_NET_NAME, sizeof(ifra.ifra_name));
	sin = &ifra.ifra_addr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(request->address);
	sin = &ifra.ifra_broadaddr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(request->gateway);
	sin = &ifra.ifra_mask;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(request->mask);

	/*
	 * in_control() refuses a point-to-point SIOCAIFADDR with a zero destination (EDESTADDRREQ,
	 * bsd/netinet/in.c). With no gateway there is no peer to name, but the requirement is to
	 * keep a valid address: make this one call as a numbered link address, then put the
	 * interface back to point-to-point. The state stays "not fully configured", no peer route
	 * is made and no default route is added; the log says why.
	 */
	if (numbered) {
		ifnet_set_flags(cell_net.ifp, 0, IFF_POINTOPOINT);
	}
	error = sock_ioctl(so, SIOCAIFADDR, &ifra);
	if (numbered) {
		ifnet_set_flags(cell_net.ifp, IFF_POINTOPOINT, IFF_POINTOPOINT);
	}
	return error;
}

static bool
cell_net_route_add(uint32_t gateway)
{
	struct sockaddr_in dst = { .sin_len = sizeof(dst), .sin_family = AF_INET };
	struct sockaddr_in mask = { .sin_len = sizeof(mask), .sin_family = AF_INET };
	struct sockaddr_in peer = { .sin_len = sizeof(peer), .sin_family = AF_INET };
	char gateway_text[16];
	int error;

	ip4_string(gateway_text, sizeof(gateway_text), gateway);
	peer.sin_addr.s_addr = htonl(gateway);
	error = rtrequest(RTM_ADD, (struct sockaddr *)&dst, (struct sockaddr *)&peer,
	    (struct sockaddr *)&mask, RTF_UP | RTF_GATEWAY | RTF_STATIC, NULL);
	if (error != 0) {
		say("vayu-cell-net: default route via %s was not added: %d\n", gateway_text, error);
		return false;
	}
	say("vayu-cell-net: default route via %s added (vayu_cell_net_default=1)\n", gateway_text);
	return true;
}

static void
cell_net_route_delete(uint32_t gateway)
{
	struct sockaddr_in dst = { .sin_len = sizeof(dst), .sin_family = AF_INET };
	struct sockaddr_in mask = { .sin_len = sizeof(mask), .sin_family = AF_INET };
	struct sockaddr_in peer = { .sin_len = sizeof(peer), .sin_family = AF_INET };
	char gateway_text[16];
	int error;

	ip4_string(gateway_text, sizeof(gateway_text), gateway);
	peer.sin_addr.s_addr = htonl(gateway);
	error = rtrequest(RTM_DELETE, (struct sockaddr *)&dst, (struct sockaddr *)&peer,
	    (struct sockaddr *)&mask, RTF_UP | RTF_GATEWAY | RTF_STATIC, NULL);
	if (error != 0) {
		say("vayu-cell-net: default route via %s delete: %d (already gone)\n", gateway_text, error);
	} else {
		say("vayu-cell-net: default route via %s removed\n", gateway_text);
	}
}

/*
 * Remove only what this module installed: the default route it added, the address it set, and
 * the interface flags. Not the ifnet: en1 stays attached for the next session. Idempotent; the
 * address is named exactly, so en0 and en2 cannot be touched.
 */
static void
cell_net_teardown(struct cell_net_applied *applied, socket_t *so)
{
	bool had_address = applied->address_set;
	bool had_route = applied->route_set;

	if ((had_address || had_route) && cell_net_open_socket(so)) {
		if (had_route) {
			cell_net_route_delete(applied->gateway);
		}
		if (had_address) {
			struct ifreq ifr;
			struct sockaddr_in *sin;
			char address[16];
			int error;

			bzero(&ifr, sizeof(ifr));
			strlcpy(ifr.ifr_name, CELL_NET_NAME, sizeof(ifr.ifr_name));
			sin = (struct sockaddr_in *)&ifr.ifr_addr;
			sin->sin_len = sizeof(*sin);
			sin->sin_family = AF_INET;
			sin->sin_addr.s_addr = htonl(applied->address);
			error = sock_ioctl(*so, SIOCDIFADDR, &ifr);
			ip4_string(address, sizeof(address), applied->address);
			if (error != 0 && error != EADDRNOTAVAIL) {
				say("vayu-cell-net: address %s remove: %d\n", address, error);
			} else {
				say("vayu-cell-net: address %s removed\n", address);
			}
		}
	} else if (had_address || had_route) {
		say("vayu-cell-net: no socket to remove the address or route; flags dropped\n");
	}

	bzero(applied, sizeof(*applied));
	if (cell_net.ifp != NULL) {
		ifnet_set_flags(cell_net.ifp, 0, IFF_UP | IFF_RUNNING);
	}
}

static void
cell_net_apply(const struct cell_net_request *request, struct cell_net_applied *applied,
    socket_t *so)
{
	char address[16], mask[16], gateway_text[16], dns1_text[16], dns2_text[16];
	uint32_t mtu;
	int error;

	if (!request->up) {
		cell_net_teardown(applied, so);
		say("vayu-cell-net: session down, en1 stays attached\n");
		return;
	}

	ip4_string(address, sizeof(address), request->address);
	ip4_string(mask, sizeof(mask), request->mask);
	ip4_string(gateway_text, sizeof(gateway_text), request->gateway);
	ip4_string(dns1_text, sizeof(dns1_text), request->dns1);
	ip4_string(dns2_text, sizeof(dns2_text), request->dns2);

	mtu = request->mtu != 0 ? request->mtu : VAYU_CELL_NET_MTU_DEFAULT;
	if (request->address == 0 || !cell_net_mask_valid(request->mask) ||
	    mtu < VAYU_CELL_NET_MTU_MIN || mtu > VAYU_CELL_NET_MTU_MAX) {
		say("vayu-cell-net: session refused: address %s mask %s mtu %u\n", address, mask, mtu);
		cell_net_teardown(applied, so);
		return;
	}

	if (!cell_net_open_socket(so)) {
		cell_net_teardown(applied, so);
		return;
	}

	/* A new session replaces the previous one; remove its route and address first. */
	cell_net_teardown(applied, so);

	error = ifnet_set_mtu(cell_net.ifp, mtu);
	if (error != 0) {
		say("vayu-cell-net: MTU %u refused: %d\n", mtu, error);
		return;
	}

	{
		struct ifreq ifr;

		bzero(&ifr, sizeof(ifr));
		strlcpy(ifr.ifr_name, CELL_NET_NAME, sizeof(ifr.ifr_name));
		error = sock_ioctl(*so, SIOCPROTOATTACH, &ifr);
		if (error != 0 && error != EEXIST) {
			say("vayu-cell-net: SIOCPROTOATTACH: %d\n", error);
			return;
		}
	}

	error = cell_net_address_set(*so, request);
	if (error != 0) {
		say("vayu-cell-net: address %s/%s peer %s: %d\n", address, mask, gateway_text, error);
		cell_net_teardown(applied, so);  /* in_ifinit() may have raised IFF_UP before failing */
		return;
	}

	applied->address_set = true;
	applied->address = request->address;
	applied->mask = request->mask;
	applied->gateway = request->gateway;
	applied->dns1 = request->dns1;
	applied->dns2 = request->dns2;
	applied->mtu = mtu;
	applied->fully_configured = request->gateway != 0;

	if (cell_net.default_route && applied->fully_configured) {
		applied->route_set = cell_net_route_add(request->gateway);
	}

	error = cell_net_set_up(*so);
	if (error != 0) {
		say("vayu-cell-net: SIOCGIFFLAGS/SIOCSIFFLAGS up: %d\n", error);
	}
	error = ifnet_set_flags(cell_net.ifp, IFF_UP | (applied->fully_configured ? IFF_RUNNING : 0),
	    IFF_UP | IFF_RUNNING);
	if (error != 0) {
		say("vayu-cell-net: flags: %d\n", error);
	}

	say("vayu-cell-net: en1 address %s mask %s peer %s mtu %u, DNS %s %s (recorded only), "
	    "link %s\n", address, mask, gateway_text, mtu, dns1_text, dns2_text,
	    applied->fully_configured ? "configured" : "address only (gateway 0)");
	cell_net_stage(applied->fully_configured ? "OK" : "ERROR",
	    "en1 %s/%s peer %s mtu %u, flags %s%s", address, mask, gateway_text, mtu,
	    applied->fully_configured ? "UP|RUNNING" : "address only",
	    applied->route_set ? ", default route added" : "");
	if (!applied->fully_configured) {
		say("vayu-cell-net: gateway 0: address kept, no default route, not fully configured\n");
	}
}

/* ---- the configuration thread -------------------------------------------------------------------- */

static void
cell_net_thread(void *argument __unused, wait_result_t wait __unused)
{
	struct cell_net_request request;
	socket_t so = NULL;

	lck_mtx_lock(&cell_net_lock);
	for (;;) {
		struct cell_net_applied applied;

		while (!cell_net.pending) {
			msleep(&cell_net.pending, &cell_net_lock, PZERO, "vayu-cell-net", NULL);
		}
		request = cell_net.request;
		cell_net.pending = false;
		applied = cell_net.applied;
		lck_mtx_unlock(&cell_net_lock);

		cell_net_apply(&request, &applied, &so);

		lck_mtx_lock(&cell_net_lock);
		cell_net.applied = applied;
		cell_net.applied_generation = request.generation;
	}
}

/* ---- the ifnet ------------------------------------------------------------------------------------ */

static errno_t
cell_net_output(ifnet_t ifp, mbuf_t packet)
{
	size_t length = mbuf_pkthdr_len(packet);
	size_t total = 0;
	const uint8_t *data = NULL;
	mbuf_t flat = NULL;
	bool sent = false;

	if (length < CELL_NET_IPV4_MIN || length > ifnet_mtu(ifp) ||
	    (ifnet_flags(ifp) & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) {
		goto done;
	}

	if (mbuf_next(packet) == NULL && mbuf_len(packet) >= length) {
		data = mbuf_data(packet);
	} else {
		unsigned int chunks = 1;

		if (mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &flat) != 0) {
			goto done;
		}
		if (mbuf_copydata(packet, 0, length, mbuf_data(flat)) != 0) {
			goto done;
		}
		data = mbuf_data(flat);
	}
	if (!cell_net_ipv4_valid(data, length, &total) || total != length) {
		goto done;
	}

	bpf_tap_out(ifp, DLT_RAW, packet, NULL, 0);
	sent = vayu_ipa_data_transmit(VAYU_CELL_NET_MUX, data, length);

done:
	if (flat != NULL) {
		mbuf_freem(flat);
	}
	if (sent) {
		cell_net.tx_packets++;
		cell_net.tx_bytes += length;
		(void)ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		cell_net.tx_dropped++;
		(void)ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	return 0;
}

static errno_t
cell_net_demux(ifnet_t ifp __unused, mbuf_t packet, char *frame_header __unused,
    protocol_family_t *protocol)
{
	const uint8_t *data;

	while (packet != NULL && mbuf_len(packet) < 1) {
		packet = mbuf_next(packet);
	}
	if (packet == NULL) {
		return ENOENT;
	}
	data = mbuf_data(packet);
	if (data == NULL || (data[0] >> 4) != 4) {
		return ENOENT;
	}
	*protocol = PF_INET;
	return 0;
}

static errno_t
cell_net_add_proto(ifnet_t ifp __unused, protocol_family_t protocol,
    const struct ifnet_demux_desc *demux_array __unused, u_int32_t demux_count __unused)
{
	return protocol == PF_INET ? 0 : ENOPROTOOPT;
}

static errno_t
cell_net_del_proto(ifnet_t ifp __unused, protocol_family_t protocol)
{
	return protocol == PF_INET ? 0 : ENOPROTOOPT;
}

static errno_t
cell_net_proto_input(ifnet_t ifp, protocol_family_t protocol, mbuf_t packet,
    char *frame_header __unused)
{
	if (proto_input(protocol, packet) != 0) {
		(void)ifnet_stat_increment_in(ifp, 0, 0, 1);
		mbuf_freem(packet);
	}
	return 0;
}

static errno_t
cell_net_attach_proto(ifnet_t ifp, protocol_family_t protocol)
{
	struct ifnet_attach_proto_param register_param;
	errno_t error;

	if (protocol != PF_INET) {
		return ENOPROTOOPT;
	}
	bzero(&register_param, sizeof(register_param));
	register_param.input = cell_net_proto_input;
	error = ifnet_attach_protocol(ifp, protocol, &register_param);
	if (error != 0 && error != EEXIST) {
		say("vayu-cell-net: PF_INET attach: %d\n", error);
	}
	return error;
}

static errno_t
cell_net_ioctl(ifnet_t ifp, u_long command, void *data)
{
	struct ifreq *request = data;
	uint16_t flags;
	bool configured;

	switch (command) {
	case SIOCSIFADDR:
		/* bsd/netinet/in.c calls this with a struct ifaddr while it installs the address. */
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCSIFFLAGS:
		lck_mtx_lock(&cell_net_lock);
		configured = cell_net.applied.fully_configured;
		lck_mtx_unlock(&cell_net_lock);
		flags = ifnet_flags(ifp);
		ifnet_set_flags(ifp, ((flags & IFF_UP) && configured) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCSIFMTU:
		if (request == NULL || request->ifr_mtu < VAYU_CELL_NET_MTU_MIN ||
		    request->ifr_mtu > VAYU_CELL_NET_MTU_MAX) {
			return EINVAL;
		}
		return ifnet_set_mtu(ifp, (uint32_t)request->ifr_mtu);
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;                          /* no link layer and no multicast on this mux */
	default:
		return EOPNOTSUPP;
	}
	return 0;
}

static bool
cell_net_attach(void)
{
	struct ifnet_init_eparams init;
	errno_t error;

	bzero(&init, sizeof(init));
	init.ver = IFNET_INIT_CURRENT_VERSION;
	init.len = sizeof(init);
	init.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
	init.uniqueid = CELL_NET_UNIQUEID;
	init.uniqueid_len = (u_int32_t)strlen(CELL_NET_UNIQUEID);
	init.name = CELL_NET_PREFIX;
	init.unit = CELL_NET_UNIT;
	init.family = IFNET_FAMILY_CELLULAR;
	init.type = IFT_CELLULAR;
	init.output = cell_net_output;
	init.demux = cell_net_demux;
	init.add_proto = cell_net_add_proto;
	init.del_proto = cell_net_del_proto;
	init.ioctl = cell_net_ioctl;

	error = ifnet_allocate_extended(&init, &cell_net.ifp);
	if (error != 0) {
		say("vayu-cell-net: en1 cannot be made: %d (already taken?)\n", error);
		cell_net.ifp = NULL;
		return false;
	}
	ifnet_set_capabilities_enabled(cell_net.ifp, 0, (u_int32_t)-1);
	ifnet_set_offload(cell_net.ifp, 0);
	ifnet_set_mtu(cell_net.ifp, VAYU_CELL_NET_MTU_DEFAULT);
	ifnet_set_hdrlen(cell_net.ifp, 0);
	ifnet_set_flags(cell_net.ifp, IFF_POINTOPOINT, IFF_POINTOPOINT);

	error = ifnet_attach(cell_net.ifp, NULL);
	if (error != 0) {
		say("vayu-cell-net: en1 cannot be attached: %d\n", error);
		ifnet_release(cell_net.ifp);
		cell_net.ifp = NULL;
		return false;
	}
	bpfattach(cell_net.ifp, DLT_RAW, 0);
	say("vayu-cell-net: en1 attached, IFT_CELLULAR, raw IPv4, link down\n");
	return true;
}

/* ---- what the rest of the kernel calls --------------------------------------------------------- */

void
vayu_cell_net_init(void)
{
	uint32_t enabled = 0;
	uint32_t default_route = 0;
	thread_t thread;
	errno_t error;

	if (!PE_parse_boot_argn("vayu_cell_net", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	if (enabled != 1) {
		say("vayu-cell-net: vayu_cell_net=%u is refused: only 1 creates en1\n", enabled);
		return;
	}

	lck_mtx_lock(&cell_net_lock);
	if (cell_net.started) {
		lck_mtx_unlock(&cell_net_lock);
		return;
	}
	cell_net.started = true;
	lck_mtx_unlock(&cell_net_lock);

	PE_parse_boot_argn("vayu_cell_net_default", &default_route, sizeof(default_route));
	cell_net.default_route = default_route == 1;

	error = proto_register_plumber(PF_INET, IFNET_FAMILY_CELLULAR, cell_net_attach_proto, NULL);
	if (error != 0 && error != EEXIST) {
		say("vayu-cell-net: proto_register_plumber: %d\n", error);
		return;
	}

	if (!cell_net_attach()) {
		return;
	}
	if (kernel_thread_start(cell_net_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-cell-net: no configuration thread; en1 stays down\n");
		return;
	}
	thread_deallocate(thread);

	lck_mtx_lock(&cell_net_lock);
	cell_net.thread_started = true;
	lck_mtx_unlock(&cell_net_lock);
	say("vayu-cell-net: ready, vayu_cell_net=1 vayu_cell_net_default=%u; no CommCenter, no configd, "
	    "no DNS publication\n", cell_net.default_route ? 1 : 0);
}

bool
vayu_cell_net_ready(void)
{
	bool ready;

	lck_mtx_lock(&cell_net_lock);
	ready = cell_net.ifp != NULL && cell_net.thread_started;
	lck_mtx_unlock(&cell_net_lock);
	return ready;
}

void
vayu_cell_net_session(bool up, uint32_t address, uint32_t mask, uint32_t gateway,
    uint32_t dns1, uint32_t dns2, uint32_t mtu)
{
	lck_mtx_lock(&cell_net_lock);
	/* Copied even before vayu_cell_net_init() has started the thread: the thread will take
	 * the latest request when it appears. Without the opt-in nothing ever reads it. */
	cell_net.request.up = up;
	cell_net.request.address = address;
	cell_net.request.mask = mask;
	cell_net.request.gateway = gateway;
	cell_net.request.dns1 = dns1;
	cell_net.request.dns2 = dns2;
	cell_net.request.mtu = mtu;
	cell_net.request.generation = ++cell_net.request_generation;
	cell_net.pending = true;
	wakeup(&cell_net.pending);
	lck_mtx_unlock(&cell_net_lock);
}

void
vayu_cell_net_input(uint8_t mux, const uint8_t *packet, size_t length)
{
	struct ifnet_stat_increment_param stats = { .packets_in = 1 };
	size_t total = 0;
	unsigned int chunks = 1;
	mbuf_t mbuf;
	ifnet_t ifp;

	if (mux != VAYU_CELL_NET_MUX || !cell_net_ipv4_valid(packet, length, &total)) {
		cell_net.rx_dropped++;
		return;
	}
	ifp = cell_net.ifp;
	if (ifp == NULL || total > ifnet_mtu(ifp) ||
	    (ifnet_flags(ifp) & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) {
		cell_net.rx_dropped++;
		return;
	}
	if (mbuf_allocpacket(MBUF_DONTWAIT, total, &chunks, &mbuf) != 0) {
		cell_net.rx_dropped++;
		return;
	}
	if (mbuf_copyback(mbuf, 0, total, packet, MBUF_DONTWAIT) != 0) {
		mbuf_freem(mbuf);
		cell_net.rx_dropped++;
		return;
	}
	mbuf_pkthdr_setrcvif(mbuf, ifp);
	bpf_tap_in(ifp, DLT_RAW, mbuf, NULL, 0);
	stats.bytes_in = (u_int32_t)total;
	if (ifnet_input(ifp, mbuf, &stats) == 0) {
		cell_net.rx_packets++;
		cell_net.rx_bytes += total;
	} else {
		cell_net.rx_dropped++;
	}
}

void
vayu_cell_net_get_status(struct vayu_cell_net_status *status)
{
	struct cell_net_applied applied;
	ifnet_t ifp;
	bool thread_started;

	if (status == NULL) {
		return;
	}
	bzero(status, sizeof(*status));
	lck_mtx_lock(&cell_net_lock);
	applied = cell_net.applied;
	ifp = cell_net.ifp;
	thread_started = cell_net.thread_started;
	status->generation = cell_net.applied_generation;
	status->rx_packets = cell_net.rx_packets;
	status->rx_bytes = cell_net.rx_bytes;
	status->rx_dropped = cell_net.rx_dropped;
	status->tx_packets = cell_net.tx_packets;
	status->tx_bytes = cell_net.tx_bytes;
	status->tx_dropped = cell_net.tx_dropped;
	lck_mtx_unlock(&cell_net_lock);

	if (ifp != NULL) {
		uint16_t flags = ifnet_flags(ifp);

		status->flags |= VAYU_CELL_NET_FLAG_ATTACHED;
		if ((flags & IFF_UP) != 0) {
			status->flags |= VAYU_CELL_NET_FLAG_UP;
		}
		if ((flags & IFF_RUNNING) != 0) {
			status->flags |= VAYU_CELL_NET_FLAG_RUNNING;
		}
	}
	if (thread_started) {
		status->flags |= VAYU_CELL_NET_FLAG_THREAD;
	}
	if (applied.address_set) {
		status->flags |= VAYU_CELL_NET_FLAG_ADDRESS;
		status->address = applied.address;
		status->mask = applied.mask;
		status->gateway = applied.gateway;
		status->dns1 = applied.dns1;
		status->dns2 = applied.dns2;
		status->mtu = applied.mtu;
	}
	if (applied.route_set) {
		status->flags |= VAYU_CELL_NET_FLAG_DEFAULT_ROUTE;
	}
	if (applied.fully_configured) {
		status->flags |= VAYU_CELL_NET_FLAG_FULLY_CONFIGURED;
	}
}
