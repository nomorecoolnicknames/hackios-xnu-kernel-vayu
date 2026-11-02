/*
 * The cellular data interface, en1: raw IPv4 in QMAP mux 1, for the POCO X3 Pro (vayu, SM8150-AC).
 * bsd/dev/vayu_cell_net.c is the implementation; this header is what the rest of the kernel uses.
 *
 * en1 is not Ethernet and carries no Ethernet header: the modem hands over IP packets in QMAP
 * mux 1 (sys/vayu_cell_qmap.h), vayu_cell_net_input() passes them to PF_INET unchanged, and the
 * output path hands raw IPv4 to vayu_ipa_data_transmit() for the same mux. There is no ARP, no
 * DHCP, no link-layer address and no link header -- DLT_RAW on BPF. en0 and en2 are not touched.
 *
 * What is deliberately absent: CommCenter and configd know nothing of this interface (it is made
 * with IFNET_INIT_NX_NOAUTO and no KEV_DL_LINK_ON/OFF is posted), DNS is recorded in the status and
 * the log only, and no default route is added unless the boot arg vayu_cell_net_default=1 says so.
 * A link that is UP and RUNNING means only that the address/mask/peer were installed; it is not a
 * claim that the modem, IPA or the network beyond it can carry a packet.
 *
 * Boot args (bsd/dev/vayu_cell_net.c):
 *   vayu_cell_net=1         create en1 and start its configuration thread (default 0: nothing)
 *   vayu_cell_net_default=1 also add a global default route via the session's gateway (default 0)
 *
 * Addresses are host order, the way vayu_cell_data.c holds the WDS words (it prints them with
 * shifts and applies htonl only when building a sockaddr). vayu_cell_net_session() copies the
 * arguments and wakes its thread; it never opens a socket or sleeps on the configuration work.
 * The caller must still be a sleepable kernel thread (QMI/driver threads here), not an ISR:
 * a short mutex protects the request. vayu_cell_net_input() does no allocation that can wait.
 */
#ifndef _SYS_VAYU_CELL_NET_H_
#define _SYS_VAYU_CELL_NET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The one mux this port binds (WDS Bind Mux Data Port, vayu_cell_data.c). */
#define VAYU_CELL_NET_MUX               1
#define VAYU_CELL_NET_MTU_DEFAULT       1500
#define VAYU_CELL_NET_MTU_MIN           576
#define VAYU_CELL_NET_MTU_MAX           1500

/* vayu_cell_net_get_status(): what exists and what was last applied. */
#define VAYU_CELL_NET_FLAG_ATTACHED     0x00000001u     /* en1 was made and attached */
#define VAYU_CELL_NET_FLAG_THREAD       0x00000002u     /* the configuration thread runs */
#define VAYU_CELL_NET_FLAG_UP           0x00000004u     /* IFF_UP */
#define VAYU_CELL_NET_FLAG_RUNNING      0x00000008u     /* IFF_RUNNING: address and peer installed */
#define VAYU_CELL_NET_FLAG_ADDRESS      0x00000010u     /* en1 has the IPv4 address below */
#define VAYU_CELL_NET_FLAG_DEFAULT_ROUTE 0x00000020u    /* the module added the default route */
#define VAYU_CELL_NET_FLAG_FULLY_CONFIGURED 0x00000040u /* address, mask and a non-zero gateway */

struct vayu_cell_net_status {
	uint32_t flags;                 /* VAYU_CELL_NET_FLAG_* */
	uint32_t address;               /* host order, 0 when none */
	uint32_t mask;
	uint32_t gateway;               /* 0: address kept, link not RUNNING, no default route */
	uint32_t dns1;                  /* recorded and logged only, never published to configd */
	uint32_t dns2;
	uint32_t mtu;
	uint32_t generation;            /* the last session request taken up */
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t rx_dropped;
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t tx_dropped;
};

#ifdef KERNEL

/* Opt-in vayu_cell_net=1: attach en1 and start the thread. Idempotent; nothing without the arg. */
void vayu_cell_net_init(void);
/* True only when en1 and its configuration thread exist; says nothing about configuration. */
bool vayu_cell_net_ready(void);

/*
 * Ask for a session or its end. `up` false means teardown (route and address removed, flags
 * down) and ignores the other arguments. For up: address and mask must be valid, mtu 0 means
 * VAYU_CELL_NET_MTU_DEFAULT, and gateway 0 means "keep the address but do not claim a complete
 * configuration". Copies only; the socket and route work happens in the module's thread.
 */
void vayu_cell_net_session(bool up, uint32_t address, uint32_t mask, uint32_t gateway,
    uint32_t dns1, uint32_t dns2, uint32_t mtu);

/*
 * One raw IPv4 packet from the modem, already stripped of its QMAP header (sys/vayu_cell_qmap.h
 * does the aggregation walk). Only mux 1, only a complete IPv4 header/packet, only while en1 is
 * UP and RUNNING; anything else is counted and dropped. No sleep, no socket, no link header.
 */
void vayu_cell_net_input(uint8_t mux, const uint8_t *packet, size_t length);

/* Copy the state and counters; safe from any thread, takes only the module lock. */
void vayu_cell_net_get_status(struct vayu_cell_net_status *status);

#endif /* KERNEL */

#endif /* _SYS_VAYU_CELL_NET_H_ */
