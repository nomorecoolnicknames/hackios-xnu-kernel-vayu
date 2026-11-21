/*
 * /dev/nbd0: a disk image on the computer at the other end of the USB cable, over the network block
 * device protocol (NBD, https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md), so that
 * a root of 20-32 GiB (iOS 27) or 15-17 GiB (macOS) can be mounted on the phone without being written
 * to its flash first. The phone is 10.11.0.2 on en2 (vayu_usbnet=3, bsd/dev/vayu_usbnet_ecm.c), the
 * host is 10.11.0.1 and serves the image with tools/nbd/nbd-server.py (read only unless told).
 *
 * This is a client of the standard protocol, the fixed newstyle handshake (NBD_OPT_GO, and
 * NBD_OPT_EXPORT_NAME for a server that has no GO; the oldstyle one too), simple replies only, one
 * request on the wire at a time. It is a block device (/dev/nbd0, /dev/rnbd0) as the virtio disk is
 * (hakios.virtio_blk.c): a file system mounts it, init can take it as the root (root_apfs=nbd0,
 * userland/src/init.c flash_root), and the APFS driver gathers its requests as it does the flash's.
 * The request is done in the caller's thread, under one lock -- as the virtio disk does: reads come
 * from file systems' cache misses, and one in flight is what the host's disk would serve anyway.
 *
 * If the connection breaks (the cable, the host's server restarted), the request that met it is
 * sent again on a new connection, as is every request after it, until the host answers again: a
 * root does not survive an I/O error, so the disk waits as a "hard" NFS mount does. nbd_giveup=S
 * says to fail a request with EIO instead once S seconds have gone by without a connection.
 *
 * Boot-args (nbd=1 is needed for any of it; nothing here runs, and nothing is on the network, without it):
 *   nbd=1               the client, and /dev/nbd0 and /dev/rnbd0
 *   nbd_host=A.B.C.D    the server (default 10.11.0.1, the host over USB)
 *   nbd_port=N          its port (default 10809, NBD's)
 *   nbd_export=NAME     the export (default "": the server's default one)
 *   nbd_rw=1            writes too (default 0: read only; a write is refused with EROFS, and so is an
 *                       open for writing). A server that says the export is read only keeps it so.
 *                       DKIOCSYNCHRONIZE is NBD_CMD_FLUSH; a write the file system marks B_FUA is FUA.
 *   nbd_bs=4096         answer in 4096-byte blocks, as /dev/ufs_* do (default 512)
 *   nbd_timeout=S       a send or a receive that takes longer than S seconds breaks the connection (default 15)
 *   nbd_wait=S          how long an open waits for the first connection (default 120: the cable enumerated,
 *                       en2 up, the host's server started; 0: as long as it takes)
 *   nbd_giveup=S        (see above; default 0: never)
 *   nbd_if=enN          the connection goes out of that interface alone (IP_BOUND_IF)
 *   nbd_ip=A.B.C.D      and nbd_if is given that address (/24) first. For QEMU (en0 = virtio_net=1), where nothing
 *                       else would; on the phone vayu_usbnet does it for en2
 *   nbd_selftest=MB     once connected, the kernel reads the first MB MiB of the export itself and says how fast, and
 *                       their CRC-32 (zlib's: tools/nbd/nbd-server.py --crc MB says the host's) -- a first look on
 *                       the phone that needs no file system, no user space and no image made for it
 *
 * A second disk, /dev/nbd1 (and /dev/rnbd1), of another export of the same server -- macOS's system volume
 * read only on nbd0 and its Data volume writable on nbd1 (init: macos_root=nbd0 macos_data=nbd1). Its own
 * connection and its own lock: a request to one disk does not wait for the other's. Only with nbd=1 as well:
 *   nbd1=1              /dev/nbd1
 *   nbd1_export=NAME    its export; a name is needed, and not nbd0's (the server's default export is nbd0's,
 *                       and one image on two disks would be written behind the other's back)
 *   nbd1_rw=1           writes too, as nbd_rw for nbd0 (default 0)
 *   nbd1_bs=4096        as nbd_bs
 *   nbd1_host=, nbd1_port=  another server (default: nbd0's)
 * nbd_if, nbd_ip, nbd_timeout, nbd_wait, nbd_giveup and nbd_selftest are for both.
 *
 * With macos_abi=1 each disk, once its first connection has said its size, is an IOMedia as macOS's storage
 * stack has it (apfs_media_disk_register, bsd/miscfs/apfs/apfs_media.c): apfs_boot_util finds the volumes of
 * the APFS container on it by role.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/OSByteOrder.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <net/if.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kpi_socket.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vayu_nbd.h>
#include <miscfs/apfs/apfs_media.h>

extern void IOSleep(unsigned int milliseconds);

/* the protocol (doc/proto.md); everything on the wire is big-endian */
#define NBD_MAGIC               0x4e42444d41474943ULL   /* "NBDMAGIC" */
#define NBD_IHAVEOPT            0x49484156454f5054ULL   /* "IHAVEOPT": newstyle */
#define NBD_OLDSTYLE            0x0000420281861253ULL
#define NBD_REP_MAGIC           0x0003e889045565a9ULL
#define NBD_REQUEST_MAGIC       0x25609513
#define NBD_REPLY_MAGIC         0x67446698

#define NBD_FLAG_FIXED_NEWSTYLE 0x0001                  /* handshake flags, and the client's */
#define NBD_FLAG_NO_ZEROES      0x0002
#define NBD_OPT_EXPORT_NAME     1
#define NBD_OPT_GO              7
#define NBD_REP_ACK             1
#define NBD_REP_INFO            3
#define NBD_REP_ERR_UNSUP       0x80000001u
#define NBD_INFO_EXPORT         0

#define NBD_FLAG_HAS_FLAGS      0x0001                  /* transmission flags */
#define NBD_FLAG_READ_ONLY      0x0002
#define NBD_FLAG_SEND_FLUSH     0x0004
#define NBD_FLAG_SEND_FUA       0x0008
#define NBD_CMD_READ            0
#define NBD_CMD_WRITE           1
#define NBD_CMD_DISC            2
#define NBD_CMD_FLUSH           3
#define NBD_CMD_FLAG_FUA        0x0001

#define NBD_SECTOR              512
#define NBD_MAX_XFER            (512 * 1024)            /* one request; servers take up to 32 MiB */
#define NBD_REPORT_BYTES        (256ULL << 20)          /* a line about the speed every 256 MiB read */

/* the request and the simple reply, as they go on the wire */
struct nbd_request {
	uint32_t magic;
	uint16_t flags;
	uint16_t type;
	uint64_t cookie;
	uint64_t offset;
	uint32_t length;
} __attribute__((packed));
struct nbd_reply {
	uint32_t magic;
	uint32_t error;
	uint64_t cookie;
} __attribute__((packed));

/* what a request comes back as: done, refused by the server (not sent again), or lost with the connection */
enum { NBD_OK, NBD_REFUSED, NBD_BROKEN };

#define NBD_UNITS               2                       /* nbd0, nbd1 */

static LCK_GRP_DECLARE(nbd_lck_grp, "vayu-nbd");
static LCK_MTX_DECLARE(nbd_ip_lock, &nbd_lck_grp);      /* nbd_ip=: given once, by whichever disk is first */

/* for both disks */
static char nbd_ifname[IFNAMSIZ];
static uint32_t nbd_ip;                                 /* nbd_ip=, host order; 0: none (or given already) */
static uint32_t nbd_timeout = 15, nbd_wait = 120, nbd_giveup, nbd_selftest;
static int nbd_bmajor = -1, nbd_cmajor = -1;

/* a disk: /dev/nbdN, an export, a connection */
struct nbd_unit {
	lck_mtx_t lock;                                 /* the connection, and one request at a time on it */
	uint32_t unit;
	char name[8];                                   /* "nbd0" */
	struct sockaddr_in to;
	char export[128];
	bool want_rw;

	socket_t so;                                    /* NULL: no connection */
	bool ready;                                     /* the export's size is known: opens go on */
	bool writable;
	uint64_t size;                                  /* bytes; the first connection's */
	uint16_t tflags;
	uint64_t cookie;
	uint32_t connections;
	uint32_t bsize, native;                         /* nbd_bs=4096: native 4096, as the virtio disk's virtio_blk_bs */
	struct {
		uint64_t requests, read_bytes, written_bytes, resent, next_report;
		uint64_t started;                           /* mach_absolute_time of the first read */
	} count;
};
static struct nbd_unit nbd_units[NBD_UNITS];
static bool nbd_unit_on[NBD_UNITS];

/* the disk of a device number; NULL: not one of ours that is on */
static struct nbd_unit *
nbd_unit_of(dev_t dev)
{
	int n = minor(dev);

	return n >= 0 && n < NBD_UNITS && nbd_unit_on[n] ? &nbd_units[n] : NULL;
}

bool
vayu_nbd_dev(dev_t dev)
{
	return nbd_bmajor >= 0 && major(dev) == nbd_bmajor;
}

/* ---- the wire ---------------------------------------------------------------------------------- */

static bool
nbd_send(struct nbd_unit *u, const void *data, size_t length)
{
	while (length > 0) {
		struct iovec iov = { .iov_base = (void *)(uintptr_t)data, .iov_len = length };
		struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
		size_t sent = 0;

		if (u->so == NULL || sock_send(u->so, &msg, 0, &sent) != 0 || sent == 0) {
			return false;
		}
		data = (const uint8_t *)data + sent;
		length -= sent;
	}
	return true;
}

static bool
nbd_receive(struct nbd_unit *u, void *data, size_t length)
{
	while (length > 0) {
		struct iovec iov = { .iov_base = data, .iov_len = length };
		struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
		size_t got = 0;
		errno_t error;

		if (u->so == NULL) {
			return false;
		}
		error = sock_receive(u->so, &msg, MSG_WAITALL, &got);
		/* (a timeout can come back with part of it: that part is kept, and a timeout with nothing is the end) */
		if (got == 0) {
			if (error != 0 && error != EWOULDBLOCK) {
				printf("nbd: %s: receive: %d\n", u->name, error);
			}
			return false;
		}
		data = (uint8_t *)data + got;
		length -= got;
	}
	return true;
}

static bool
nbd_receive_be64(struct nbd_unit *u, uint64_t *value)
{
	if (!nbd_receive(u, value, sizeof(*value))) {
		return false;
	}
	*value = OSSwapBigToHostInt64(*value);
	return true;
}

static bool
nbd_receive_be32(struct nbd_unit *u, uint32_t *value)
{
	if (!nbd_receive(u, value, sizeof(*value))) {
		return false;
	}
	*value = OSSwapBigToHostInt32(*value);
	return true;
}

static bool
nbd_receive_be16(struct nbd_unit *u, uint16_t *value)
{
	if (!nbd_receive(u, value, sizeof(*value))) {
		return false;
	}
	*value = OSSwapBigToHostInt16(*value);
	return true;
}

/* n bytes the client has no use for */
static bool
nbd_skip(struct nbd_unit *u, uint32_t n)
{
	uint8_t scrap[64];

	while (n > 0) {
		uint32_t chunk = MIN(n, (uint32_t)sizeof(scrap));

		if (!nbd_receive(u, scrap, chunk)) {
			return false;
		}
		n -= chunk;
	}
	return true;
}

static void
nbd_drop(struct nbd_unit *u)
{
	if (u->so != NULL) {
		sock_close(u->so);
		u->so = NULL;
	}
}

/* ---- the address (nbd_ip=, for QEMU): as vayu_usbnet_ecm.c's usbnet_ipv4() gives en2 its own --------- */

/* struct in_aliasreq, which in_control() casts SIOCAIFADDR to (see vayu_usbnet_ecm.c) */
struct nbd_aliasreq {
	char ifra_name[IFNAMSIZ];
	struct sockaddr_in ifra_addr;
	struct sockaddr_in ifra_broadaddr;
	struct sockaddr_in ifra_mask;
};

static bool
nbd_give_address(void)
{
	struct nbd_aliasreq ifra;
	struct ifreq ifr;
	socket_t so = NULL;
	errno_t error;

	if (sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so) != 0) {
		return false;
	}
	bzero(&ifr, sizeof(ifr));
	strlcpy(ifr.ifr_name, nbd_ifname, sizeof(ifr.ifr_name));
	(void)sock_ioctl(so, SIOCPROTOATTACH, &ifr);
	bzero(&ifra, sizeof(ifra));
	strlcpy(ifra.ifra_name, nbd_ifname, sizeof(ifra.ifra_name));
	ifra.ifra_addr.sin_len = ifra.ifra_mask.sin_len = ifra.ifra_broadaddr.sin_len = sizeof(struct sockaddr_in);
	ifra.ifra_addr.sin_family = ifra.ifra_mask.sin_family = ifra.ifra_broadaddr.sin_family = AF_INET;
	ifra.ifra_addr.sin_addr.s_addr = htonl(nbd_ip);
	ifra.ifra_mask.sin_addr.s_addr = htonl(0xffffff00);
	ifra.ifra_broadaddr.sin_addr.s_addr = htonl((nbd_ip & 0xffffff00) | 0xff);
	error = sock_ioctl(so, SIOCAIFADDR, &ifra);
	if (error == 0) {
		bzero(&ifr, sizeof(ifr));
		strlcpy(ifr.ifr_name, nbd_ifname, sizeof(ifr.ifr_name));
		error = sock_ioctl(so, SIOCGIFFLAGS, &ifr);
		if (error == 0) {
			ifr.ifr_flags |= IFF_UP;
			error = sock_ioctl(so, SIOCSIFFLAGS, &ifr);
		}
	}
	sock_close(so);
	printf("nbd: %s given %u.%u.%u.%u/24: %d\n", nbd_ifname, nbd_ip >> 24, (nbd_ip >> 16) & 0xff,
	    (nbd_ip >> 8) & 0xff, nbd_ip & 0xff, error);
	return error == 0;
}

/* ---- a connection: the socket, then the handshake ----------------------------------------------- */

/* the reply to NBD_OPT_GO; -1: the server has no GO (ask with EXPORT_NAME), 0: refused, 1: in */
static int
nbd_go(struct nbd_unit *u, uint64_t *size, uint16_t *tflags)
{
	uint32_t name_length = (uint32_t)strlen(u->export);
	struct {
		uint64_t magic;
		uint32_t option, length, name_length;
	} __attribute__((packed)) option = {
		.magic = OSSwapHostToBigInt64(NBD_IHAVEOPT),
		.option = OSSwapHostToBigInt32(NBD_OPT_GO),
		.length = OSSwapHostToBigInt32(4 + name_length + 2),
		.name_length = OSSwapHostToBigInt32(name_length),
	};
	uint16_t no_info = 0;
	bool have_export = false;

	if (!nbd_send(u, &option, sizeof(option)) || !nbd_send(u, u->export, name_length) ||
	    !nbd_send(u, &no_info, sizeof(no_info))) {
		return 0;
	}
	for (;;) {
		uint64_t magic;
		uint32_t opt, type, length;

		if (!nbd_receive_be64(u, &magic) || !nbd_receive_be32(u, &opt) || !nbd_receive_be32(u, &type) ||
		    !nbd_receive_be32(u, &length) || magic != NBD_REP_MAGIC) {
			printf("nbd: %s: the server's answer to GO is not an option reply\n", u->name);
			return 0;
		}
		if (type == NBD_REP_ACK) {
			if (!have_export) {
				printf("nbd: %s: the server said GO without the export's size\n", u->name);
				return 0;
			}
			return nbd_skip(u, length) ? 1 : 0;
		}
		if (type == NBD_REP_INFO && length >= 12) {
			uint16_t info;

			if (!nbd_receive_be16(u, &info)) {
				return 0;
			}
			if (info == NBD_INFO_EXPORT) {
				if (!nbd_receive_be64(u, size) || !nbd_receive_be16(u, tflags)) {
					return 0;
				}
				have_export = true;
				length -= 10;
			}
			if (!nbd_skip(u, length - 2)) {
				return 0;
			}
			continue;
		}
		if (type == NBD_REP_ERR_UNSUP) {
			return nbd_skip(u, length) ? -1 : 0;
		}
		if (type & 0x80000000u) {
			char text[96] = "";
			uint32_t n = MIN(length, (uint32_t)sizeof(text) - 1);

			if (!nbd_receive(u, text, n) || !nbd_skip(u, length - n)) {
				return 0;
			}
			text[n] = 0;
			printf("nbd: %s: the server refuses export \"%s\": error 0x%x %s\n", u->name, u->export, type, text);
			return 0;
		}
		if (!nbd_skip(u, length)) {        /* a reply this client did not ask for */
			return 0;
		}
	}
}

static bool
nbd_handshake(struct nbd_unit *u, uint64_t *size, uint16_t *tflags)
{
	uint64_t magic, style;
	uint16_t hflags;
	uint32_t cflags;
	int go;

	if (!nbd_receive_be64(u, &magic) || magic != NBD_MAGIC || !nbd_receive_be64(u, &style)) {
		printf("nbd: %s: no NBD server there\n", u->name);
		return false;
	}
	if (style == NBD_OLDSTYLE) {
		uint32_t flags32;

		if (!nbd_receive_be64(u, size) || !nbd_receive_be32(u, &flags32) || !nbd_skip(u, 124)) {
			return false;
		}
		*tflags = (uint16_t)flags32;
		return true;
	}
	if (style != NBD_IHAVEOPT || !nbd_receive_be16(u, &hflags) || !(hflags & NBD_FLAG_FIXED_NEWSTYLE)) {
		printf("nbd: %s: the server speaks neither oldstyle nor fixed newstyle\n", u->name);
		return false;
	}
	cflags = OSSwapHostToBigInt32(NBD_FLAG_FIXED_NEWSTYLE | (hflags & NBD_FLAG_NO_ZEROES));
	if (!nbd_send(u, &cflags, sizeof(cflags))) {
		return false;
	}
	go = nbd_go(u, size, tflags);
	if (go >= 0) {
		return go == 1;
	}
	/* a server from before GO: the export by name; it hangs up if there is no such export */
	{
		uint32_t name_length = (uint32_t)strlen(u->export);
		struct {
			uint64_t magic;
			uint32_t option, length;
		} __attribute__((packed)) option = {
			.magic = OSSwapHostToBigInt64(NBD_IHAVEOPT),
			.option = OSSwapHostToBigInt32(NBD_OPT_EXPORT_NAME),
			.length = OSSwapHostToBigInt32(name_length),
		};

		if (!nbd_send(u, &option, sizeof(option)) || !nbd_send(u, u->export, name_length) ||
		    !nbd_receive_be64(u, size) || !nbd_receive_be16(u, tflags) ||
		    !nbd_skip(u, (hflags & NBD_FLAG_NO_ZEROES) ? 0 : 124)) {
			printf("nbd: %s: the server took no export \"%s\"\n", u->name, u->export);
			return false;
		}
	}
	return true;
}

/* one try at a connection; called with u->lock held. Quiet unless asked (a host that is not up yet is expected). */
static bool
nbd_connect(struct nbd_unit *u, bool say)
{
	struct timeval patience = { .tv_sec = nbd_timeout, .tv_usec = 0 };
	uint32_t to = ntohl(u->to.sin_addr.s_addr);
	uint64_t size = 0;
	uint16_t tflags = 0;
	errno_t error;
	int on = 1;

	nbd_drop(u);
	if (nbd_ifname[0] != 0) {
		ifnet_t ifp = NULL;
		int index;
		bool given = true;

		if (ifnet_find_by_name(nbd_ifname, &ifp) != 0 || ifp == NULL) {
			if (say) {
				printf("nbd: %s: no %s yet\n", u->name, nbd_ifname);
			}
			return false;
		}
		index = (int)ifnet_index(ifp);
		ifnet_release(ifp);
		lck_mtx_lock(&nbd_ip_lock);
		if (nbd_ip != 0) {
			given = nbd_give_address();
			if (given) {
				nbd_ip = 0;             /* once, for both disks */
			}
		}
		lck_mtx_unlock(&nbd_ip_lock);
		if (!given) {
			return false;
		}
		if (sock_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, &u->so) != 0) {
			u->so = NULL;
			return false;
		}
		(void)sock_setsockopt(u->so, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index));
	} else if (sock_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, &u->so) != 0) {
		u->so = NULL;
		return false;
	}
	/* (the caller may be any thread, a process's with a signal pending among them: no EINTR mid-request) */
	(void)sock_nointerrupt(u->so, 1);
	(void)sock_setsockopt(u->so, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	(void)sock_setsockopt(u->so, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	(void)sock_setsockopt(u->so, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
	(void)sock_setsockopt(u->so, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof(patience));
	error = sock_connect(u->so, (struct sockaddr *)&u->to, MSG_DONTWAIT);
	if (error == EINPROGRESS) {
		error = sock_connectwait(u->so, &patience);
	}
	if (error != 0) {
		if (say) {
			printf("nbd: %s: connect to %u.%u.%u.%u:%u: %d\n", u->name, to >> 24, (to >> 16) & 0xff,
			    (to >> 8) & 0xff, to & 0xff, ntohs(u->to.sin_port), error);
		}
		nbd_drop(u);
		return false;
	}
	if (!nbd_handshake(u, &size, &tflags)) {
		nbd_drop(u);
		return false;
	}
	size &= ~(uint64_t)(NBD_SECTOR - 1);
	if (size == 0) {
		printf("nbd: %s: export \"%s\" is empty\n", u->name, u->export);
		nbd_drop(u);
		return false;
	}
	if (u->ready && size != u->size) {
		/* not the disk the file systems on it have read: nothing more from it */
		printf("nbd: %s: export \"%s\" is %llu bytes now, not %llu: refused\n", u->name, u->export, size, u->size);
		nbd_drop(u);
		return false;
	}
	u->connections++;
	if (!u->ready) {
		u->size = size;
		u->tflags = tflags;
		u->writable = u->want_rw && !(tflags & NBD_FLAG_READ_ONLY);
		printf("nbd: /dev/%s is export \"%s\" of %u.%u.%u.%u:%u, %llu bytes (%llu MB), %s%s%s\n", u->name,
		    u->export, to >> 24, (to >> 16) & 0xff, (to >> 8) & 0xff, to & 0xff, ntohs(u->to.sin_port), size,
		    size >> 20, u->writable ? "writable" : "read only",
		    u->want_rw && !u->writable ? " (the server's export is read only)" : "",
		    u->writable && !(tflags & NBD_FLAG_SEND_FLUSH) ? ", no flush" : "");
	} else {
		printf("nbd: %s: connected again (connection %u)\n", u->name, u->connections);
	}
	return true;
}

/* ---- a request: sent, and sent again on a new connection until done --------------------------- */

static int
nbd_exchange(struct nbd_unit *u, uint16_t type, uint16_t flags, uint64_t offset, uint32_t length, void *data)
{
	struct nbd_request request = {
		.magic = OSSwapHostToBigInt32(NBD_REQUEST_MAGIC),
		.flags = OSSwapHostToBigInt16(flags),
		.type = OSSwapHostToBigInt16(type),
		.cookie = ++u->cookie,
		.offset = OSSwapHostToBigInt64(offset),
		.length = OSSwapHostToBigInt32(length),
	};
	struct nbd_reply reply;

	if (!nbd_send(u, &request, sizeof(request)) ||
	    (type == NBD_CMD_WRITE && !nbd_send(u, data, length)) ||
	    !nbd_receive(u, &reply, sizeof(reply))) {
		return NBD_BROKEN;
	}
	if (OSSwapBigToHostInt32(reply.magic) != NBD_REPLY_MAGIC || reply.cookie != request.cookie) {
		printf("nbd: %s: a reply out of step (magic 0x%x): the connection is dropped\n", u->name,
		    OSSwapBigToHostInt32(reply.magic));
		return NBD_BROKEN;
	}
	if (reply.error != 0) {
		printf("nbd: %s: the server refused a %s of %u bytes at %llu: error %u\n", u->name,
		    type == NBD_CMD_READ ? "read" : type == NBD_CMD_WRITE ? "write" : "flush", length, offset,
		    OSSwapBigToHostInt32(reply.error));
		return NBD_REFUSED;
	}
	if (type == NBD_CMD_READ && !nbd_receive(u, data, length)) {
		return NBD_BROKEN;
	}
	return NBD_OK;
}

static void
nbd_report(struct nbd_unit *u, uint32_t length)
{
	uint64_t now = mach_absolute_time(), ns;

	if (u->count.started == 0) {
		u->count.started = now;
		u->count.next_report = NBD_REPORT_BYTES;
	}
	u->count.read_bytes += length;
	if (u->count.read_bytes >= u->count.next_report) {
		absolutetime_to_nanoseconds(now - u->count.started, &ns);
		printf("nbd: %s: %llu MB read in %llu ms (%llu KB/s), %llu requests, %llu sent again, %u connections\n",
		    u->name, u->count.read_bytes >> 20, ns / NSEC_PER_MSEC,
		    ns ? u->count.read_bytes * (NSEC_PER_SEC / 1024) / ns : 0, u->count.requests, u->count.resent,
		    u->connections);
		u->count.next_report += NBD_REPORT_BYTES;
	}
}

static int
nbd_do(struct nbd_unit *u, uint16_t type, uint16_t flags, uint64_t offset, uint32_t length, void *data)
{
	uint64_t broken_since = 0, deadline = 0;
	unsigned tries = 0;
	int result;

	lck_mtx_lock(&u->lock);
	for (;;) {
		if (u->so != NULL) {
			result = nbd_exchange(u, type, flags, offset, length, data);
			if (result != NBD_BROKEN) {
				break;
			}
			printf("nbd: %s: the connection broke during a %s of %u bytes at %llu: sent again once there is another\n",
			    u->name, type == NBD_CMD_READ ? "read" : type == NBD_CMD_WRITE ? "write" : "flush", length, offset);
			nbd_drop(u);
			u->count.resent++;
		}
		if (broken_since == 0) {
			broken_since = mach_absolute_time();
			if (nbd_giveup != 0) {
				clock_interval_to_deadline(nbd_giveup, NSEC_PER_SEC, &deadline);
			}
		}
		if (deadline != 0 && mach_absolute_time() > deadline) {
			printf("nbd: %s: no connection for %u s: a %s at %llu fails\n", u->name, nbd_giveup,
			    type == NBD_CMD_READ ? "read" : "write", offset);
			result = NBD_REFUSED;
			break;
		}
		if (!nbd_connect(u, tries % 10 == 0)) {
			tries++;
			lck_mtx_unlock(&u->lock);
			IOSleep(tries < 5 ? 200 : 1000);
			lck_mtx_lock(&u->lock);
		}
	}
	if (result == NBD_OK) {
		u->count.requests++;
		if (type == NBD_CMD_READ) {
			nbd_report(u, length);
		} else if (type == NBD_CMD_WRITE) {
			u->count.written_bytes += length;
		}
	}
	lck_mtx_unlock(&u->lock);
	return result == NBD_OK ? 0 : EIO;
}

/* ---- the block device -------------------------------------------------------------------------- */

static int
nbd_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	struct nbd_unit *u = nbd_unit_of(dev);

	if (u == NULL) {
		return ENXIO;
	}
	if (!u->ready) {
		uint64_t deadline;

		/* (nbd_wait=0: for as long as it takes) */
		clock_interval_to_deadline(nbd_wait, NSEC_PER_SEC, &deadline);
		lck_mtx_lock(&u->lock);
		while (!u->ready && (nbd_wait == 0 || mach_absolute_time() < deadline)) {
			struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

			(void)msleep(&u->ready, &u->lock, PRIBIO, "nbd-open", &ts);
		}
		lck_mtx_unlock(&u->lock);
		if (!u->ready) {
			printf("nbd: /dev/%s opened, and no server answered in %u s\n", u->name, nbd_wait);
			return ENXIO;
		}
	}
	if ((flags & FWRITE) && !u->writable) {
		return EROFS;
	}
	return 0;
}

static int
nbd_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
nbd_strategy(buf_t bp)
{
	struct nbd_unit *u = nbd_unit_of(buf_device(bp));
	bool write = !(buf_flags(bp) & B_READ);
	uint64_t offset;
	size_t length = buf_count(bp), done = 0;
	caddr_t mapped;
	int error = 0;

	if (u == NULL || !u->ready) {
		error = ENXIO;
	} else if (write && !u->writable) {
		error = EROFS;
	}
	if (error != 0) {
		buf_seterror(bp, error);
		buf_biodone(bp);
		return;
	}
	offset = (uint64_t)buf_blkno(bp) * u->bsize;
	/* a request that runs off the end is cut there; one that starts past it is refused */
	if (offset >= u->size) {
		buf_seterror(bp, offset == u->size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + length > u->size) {
		length = (size_t)(u->size - offset);
	}
	/* (HFS Plus's cluster I/O hands over pages with no kernel address yet: buf_map gives them one) */
	if ((error = buf_map(bp, &mapped)) != 0) {
		buf_seterror(bp, error);
		buf_biodone(bp);
		return;
	}
	while (done < length && error == 0) {
		uint32_t chunk = (uint32_t)MIN(length - done, (size_t)NBD_MAX_XFER);
		uint16_t flags = write && (buf_flags(bp) & B_FUA) && (u->tflags & NBD_FLAG_SEND_FUA) ? NBD_CMD_FLAG_FUA : 0;

		error = nbd_do(u, write ? NBD_CMD_WRITE : NBD_CMD_READ, flags, offset + done, chunk, (uint8_t *)mapped + done);
		if (error == 0) {
			done += chunk;
		}
	}
	buf_setresid(bp, (uint32_t)(buf_count(bp) - done));
	buf_unmap(bp);
	if (error != 0) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
nbd_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	struct nbd_unit *u = nbd_unit_of(dev);

	if (u == NULL || !u->ready) {
		return ENXIO;
	}
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = u->bsize;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		if (u->native == 0) {
			return ENOTTY;
		}
		*(uint32_t *)data = u->native;
		return 0;
	case DKIOCSETBLOCKSIZE: {
		uint32_t size = *(uint32_t *)data;

		/* as the virtio disk: spec_open() sets 512 and back, and takes a refusal as a broken device */
		if (u->native != 0) {
			if (size < NBD_SECTOR || size > u->native || (size & (size - 1)) != 0) {
				return EINVAL;
			}
			u->bsize = size;
			return 0;
		}
		return size == NBD_SECTOR ? 0 : EINVAL;
	}
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = u->size / u->bsize;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = u->writable;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = NBD_MAX_XFER / u->bsize;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		/* a write that came back is in the server's hands; a flush puts it on the server's disk */
		if (!u->writable || !(u->tflags & NBD_FLAG_SEND_FLUSH)) {
			return 0;
		}
		return nbd_do(u, NBD_CMD_FLUSH, 0, 0, 0, NULL);
	default:
		return ENOTTY;
	}
}

static int
nbd_psize(dev_t dev)
{
	struct nbd_unit *u = nbd_unit_of(dev);

	return u != NULL && u->ready ? (int)u->bsize : -1;
}

/*
 * The character device, for tools (dd, a checksum) rather than file systems: not physio(), which hands the strategy
 * the caller's own pages at their user address -- not for the kernel to touch (PAN), and the socket copies into
 * what it is given. A buffer of the kernel's in between, uiomove() to and from the caller.
 */
static int
nbd_rw_uio(dev_t dev, struct uio *uio, bool write)
{
	struct nbd_unit *u = nbd_unit_of(dev);
	uint8_t *bounce;
	int error = 0;

	if (u == NULL || !u->ready) {
		return ENXIO;
	}
	if (write && !u->writable) {
		return EROFS;
	}
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	bounce = kalloc_data(NBD_MAX_XFER, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < u->size) {
		uint64_t offset = (uint64_t)uio_offset(uio);
		uint32_t chunk = (uint32_t)MIN(MIN((uint64_t)uio_resid(uio), (uint64_t)NBD_MAX_XFER), u->size - offset);

		if (write) {
			error = uiomove((caddr_t)bounce, (int)chunk, uio);
			if (error == 0) {
				error = nbd_do(u, NBD_CMD_WRITE, 0, offset, chunk, bounce);
			}
		} else {
			error = nbd_do(u, NBD_CMD_READ, 0, offset, chunk, bounce);
			if (error == 0) {
				error = uiomove((caddr_t)bounce, (int)chunk, uio);
			}
		}
	}
	kfree_data(bounce, NBD_MAX_XFER);
	return error;
}

static int
nbd_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return nbd_rw_uio(dev, uio, false);
}

static int
nbd_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return nbd_rw_uio(dev, uio, true);
}

static const struct bdevsw nbd_bdevsw = {
	.d_open     = nbd_open,
	.d_close    = nbd_close,
	.d_strategy = nbd_strategy,
	.d_ioctl    = nbd_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = nbd_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw nbd_cdevsw = {
	.d_open       = nbd_open,
	.d_close      = nbd_close,
	.d_read       = nbd_read,
	.d_write      = nbd_write,
	.d_ioctl      = nbd_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/* ---- bringing it up ------------------------------------------------------------------------------ */

/* "A.B.C.D" in host order; 0 if it is not one */
static uint32_t
nbd_parse_ip(const char *text)
{
	uint32_t ip = 0, part;
	int parts = 0;

	while (parts < 4) {
		if (*text < '0' || *text > '9') {
			return 0;
		}
		for (part = 0; *text >= '0' && *text <= '9'; text++) {
			part = part * 10 + (uint32_t)(*text - '0');
			if (part > 255) {
				return 0;
			}
		}
		ip = ip << 8 | part;
		parts++;
		if (parts < 4 && *text++ != '.') {
			return 0;
		}
	}
	return *text == 0 ? ip : 0;
}

/* nbd_selftest=MB: the first MB MiB through nbd_do(), as a file system's reads would go, with their CRC-32 */
static void
nbd_self_test(struct nbd_unit *u)
{
	uint64_t total = MIN((uint64_t)nbd_selftest << 20, u->size), done = 0, started, ns;
	uint32_t crc = 0;
	uint8_t *buffer = kalloc_data(NBD_MAX_XFER, Z_WAITOK);
	int error = 0;

	if (buffer == NULL) {
		return;
	}
	started = mach_absolute_time();
	while (done < total && error == 0) {
		uint32_t chunk = (uint32_t)MIN(total - done, (uint64_t)NBD_MAX_XFER);

		error = nbd_do(u, NBD_CMD_READ, 0, done, chunk, buffer);
		if (error == 0) {
			crc = crc32(crc, buffer, chunk);
			done += chunk;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
	printf("nbd: %s: selftest: %llu of %llu bytes read in %llu ms (%llu KB/s), crc32 %08x%s\n", u->name, done,
	    total, ns / NSEC_PER_MSEC, ns ? done * (NSEC_PER_SEC / 1024) / ns : 0, crc, error ? ", then an error" : "");
	kfree_data(buffer, NBD_MAX_XFER);
}

/* the first connection, in the background: the cable, en2 and the host's server come when they come */
static void
nbd_boot_thread(void *arg, __unused wait_result_t wr)
{
	struct nbd_unit *u = arg;
	unsigned tries = 0;

	lck_mtx_lock(&u->lock);
	while (!nbd_connect(u, tries % 30 == 0)) {
		tries++;
		lck_mtx_unlock(&u->lock);
		IOSleep(tries < 10 ? 200 : 1000);
		lck_mtx_lock(&u->lock);
	}
	lck_mtx_unlock(&u->lock);
	/*
	 * macos_abi=1: an IOMedia for it, as macOS's storage stack has (bsd/miscfs/apfs/apfs_media.c; else nothing),
	 * before an open can go on and a file system mount it: the APFS driver finds its containers by the disk
	 */
	apfs_media_disk_register(u->name, makedev(nbd_bmajor, u->unit), makedev(nbd_cmajor, u->unit), u->bsize,
	    u->size / u->bsize, u->writable);
	lck_mtx_lock(&u->lock);
	u->ready = true;
	wakeup(&u->ready);
	lck_mtx_unlock(&u->lock);
	if (nbd_selftest != 0) {
		nbd_self_test(u);
	}
}

/*
 * One disk's boot-args: PREFIX ("nbd" or "nbd1") _host, _port, _export, _rw, _bs; the host and the port default to
 * DEFAULTS' (nbd0's, for nbd1). false: a host that is not an address.
 */
static bool
nbd_unit_args(struct nbd_unit *u, const char *prefix, const struct nbd_unit *defaults)
{
	uint32_t port = defaults ? ntohs(defaults->to.sin_port) : 10809, rw = 0, bs = 0, host;
	char text[32] = "10.11.0.1", arg[32];

	if (defaults != NULL) {
		uint32_t h = ntohl(defaults->to.sin_addr.s_addr);

		snprintf(text, sizeof(text), "%u.%u.%u.%u", h >> 24, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff);
	}
	snprintf(arg, sizeof(arg), "%s_host", prefix);
	(void)PE_parse_boot_arg_str(arg, text, sizeof(text));
	host = nbd_parse_ip(text);
	if (host == 0) {
		printf("nbd: %s=%s is not an IPv4 address: no /dev/%s\n", arg, text, u->name);
		return false;
	}
	snprintf(arg, sizeof(arg), "%s_port", prefix);
	(void)PE_parse_boot_argn(arg, &port, sizeof(port));
	if (port == 0 || port > 65535) {
		port = 10809;
	}
	u->to.sin_len = sizeof(u->to);
	u->to.sin_family = AF_INET;
	u->to.sin_addr.s_addr = htonl(host);
	u->to.sin_port = htons((uint16_t)port);
	snprintf(arg, sizeof(arg), "%s_export", prefix);
	(void)PE_parse_boot_arg_str(arg, u->export, sizeof(u->export));
	snprintf(arg, sizeof(arg), "%s_rw", prefix);
	(void)PE_parse_boot_argn(arg, &rw, sizeof(rw));
	u->want_rw = rw != 0;
	u->bsize = NBD_SECTOR;
	snprintf(arg, sizeof(arg), "%s_bs", prefix);
	if (PE_parse_boot_argn(arg, &bs, sizeof(bs)) && bs == 4096) {
		u->native = u->bsize = 4096;
	}
	return true;
}

/* /dev/nbdN and /dev/rnbdN, and the first connection in the background */
static void
nbd_unit_start(struct nbd_unit *u)
{
	uint32_t host = ntohl(u->to.sin_addr.s_addr);
	char rname[12];
	thread_t thread;

	snprintf(rname, sizeof(rname), "r%s", u->name);
	if (devfs_make_node(makedev(nbd_bmajor, u->unit), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0600, u->name) == NULL ||
	    devfs_make_node(makedev(nbd_cmajor, u->unit), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0600, rname) == NULL) {
		printf("nbd: devfs_make_node failed for %s\n", u->name);
		return;
	}
	nbd_unit_on[u->unit] = true;
	printf("nbd: /dev/%s will be export \"%s\" of %u.%u.%u.%u:%u%s%s, %s, blocks of %u; an open waits %u s for it\n",
	    u->name, u->export, host >> 24, (host >> 16) & 0xff, (host >> 8) & 0xff, host & 0xff, ntohs(u->to.sin_port),
	    nbd_ifname[0] ? " through " : "", nbd_ifname, u->want_rw ? "writable if the server agrees" : "read only",
	    u->bsize, nbd_wait);
	if (kernel_thread_start(nbd_boot_thread, u, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
vayu_nbd_init(void)
{
	uint32_t enabled = 0, second = 0;
	char text[32] = "";

	if (!PE_parse_boot_argn("nbd", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	for (uint32_t i = 0; i < NBD_UNITS; i++) {
		lck_mtx_init(&nbd_units[i].lock, &nbd_lck_grp, LCK_ATTR_NULL);
		nbd_units[i].unit = i;
		snprintf(nbd_units[i].name, sizeof(nbd_units[i].name), "nbd%u", i);
	}
	if (!nbd_unit_args(&nbd_units[0], "nbd", NULL)) {
		return;
	}
	(void)PE_parse_boot_argn("nbd_timeout", &nbd_timeout, sizeof(nbd_timeout));
	if (nbd_timeout == 0) {
		nbd_timeout = 15;
	}
	(void)PE_parse_boot_argn("nbd_wait", &nbd_wait, sizeof(nbd_wait));
	(void)PE_parse_boot_argn("nbd_giveup", &nbd_giveup, sizeof(nbd_giveup));
	(void)PE_parse_boot_argn("nbd_selftest", &nbd_selftest, sizeof(nbd_selftest));
	(void)PE_parse_boot_arg_str("nbd_if", nbd_ifname, sizeof(nbd_ifname));
	if (PE_parse_boot_arg_str("nbd_ip", text, sizeof(text)) && text[0] != 0) {
		nbd_ip = nbd_parse_ip(text);
		if (nbd_ip == 0 || nbd_ifname[0] == 0) {
			printf("nbd: nbd_ip=%s wants an address and nbd_if=: ignored\n", text);
			nbd_ip = 0;
		}
	}

	nbd_bmajor = bdevsw_add(-1, &nbd_bdevsw);
	if (nbd_bmajor < 0) {
		printf("nbd: bdevsw_add failed\n");
		return;
	}
	nbd_cmajor = cdevsw_add_with_bdev(-1, &nbd_cdevsw, nbd_bmajor);
	if (nbd_cmajor < 0) {
		printf("nbd: cdevsw_add failed\n");
		return;
	}
	nbd_unit_start(&nbd_units[0]);

	if (!PE_parse_boot_argn("nbd1", &second, sizeof(second)) || second == 0) {
		return;
	}
	if (!nbd_unit_args(&nbd_units[1], "nbd1", &nbd_units[0])) {
		return;
	}
	if (nbd_units[1].export[0] == 0 || (strcmp(nbd_units[1].export, nbd_units[0].export) == 0 &&
	    nbd_units[1].to.sin_addr.s_addr == nbd_units[0].to.sin_addr.s_addr &&
	    nbd_units[1].to.sin_port == nbd_units[0].to.sin_port)) {
		printf("nbd: nbd1 wants an export of its own (nbd1_export=, not nbd0's \"%s\"): no /dev/nbd1\n",
		    nbd_units[0].export);
		return;
	}
	nbd_unit_start(&nbd_units[1]);
}
