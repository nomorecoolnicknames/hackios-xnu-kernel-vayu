/*
 * /dev/vayu-qmux: the QMUX side of the cellular bridge for iOS's CommCenter.
 *
 * CommCenter speaks QMI inside Apple's QMux framing (libATCommandStudioDynamic: a 5- or
 * 6-byte QMux header -- version, length, control flags, service, client id -- in front of the
 * usual 7-byte QMI header). The modem on this phone (vayu, SM8150-AC, X24) speaks QMI over
 * QRTR/GLINK and has no QMux multiplexer at all. This device carries QMux frames between a
 * userland shim inside CommCenter and the kernel driver (bsd/dev/vayu_qmux.c), which answers
 * the QMux control service (0x00) locally and forwards every other service to the modem's
 * QRTR ports.
 *
 * One read() or one write() carries exactly one record:
 *
 *     +---------------------------+--------------------------+
 *     | struct vayu_qmux_hdr (32) | payload (hdr.length bytes)|
 *     +---------------------------+--------------------------+
 *
 * write(): one whole QMux frame, AP -> modem (direction 0). A write shorter than the header,
 *          or one whose vm_length is not exactly the rest of the write, is EINVAL. The driver
 *          does not split or coalesce records.
 * read():  one record (header + payload). A buffer smaller than the record is EMSGSIZE and
 *          the record stays queued. O_NONBLOCK: EAGAIN when the queue is empty. select()/poll()
 *          report readable when a record is queued.
 * close(): every client id the opener allocated is released; queued records are dropped.
 *
 * The driver may also queue status records (kind 2) with no payload: vm_status is then an
 * errno, vm_message a driver code and vm_cookie the cookie of the frame it refers to. The
 * shim logs them; they never come from the modem.
 *
 * Boot-args (bsd/dev/vayu_qmux.c has their full meaning):
 *   vayu_qmux=0       nothing, and no /dev/vayu-qmux (the default)
 *   vayu_qmux=1       the bridge is up: the node exists, control service local, whitelisted
 *                     services forwarded to the modem
 *   vayu_qmux_wds=1   also allow WDS (0x01); default 0
 *   vayu_qmux_wds_bind=1  and point CommCenter's WDS Bind Mux Data Port at the IPA's port, mux 1
 *   vayu_qmux_trace=1 one line per frame header in the kernel log and the trace ring (default 1)
 *   vayu_qmux_timeout_ms=N  a forwarded request with no reply is answered with a QMI error
 *                     after N ms (default 15000, at least 1000, at most 60000)
 *
 * Endianness: little-endian throughout (arm64). No pointers: 4 + 2 + 2 + 4 + 2 + 2 + 2 +
 * 2 + 4 + 8 = 32 bytes, no padding.
 */
#ifndef _SYS_VAYU_QMUX_H_
#define _SYS_VAYU_QMUX_H_

#include <stdint.h>

#define VAYU_QMUX_MAGIC         0x58554d51u     /* "QMUX" in memory, little-endian */
#define VAYU_QMUX_VERSION       1
#define VAYU_QMUX_MAX_PAYLOAD   8192            /* one QMux frame; larger is refused */

/* vm_kind */
#define VAYU_QMUX_KIND_FRAME    1               /* payload is one QMux frame, header included */
#define VAYU_QMUX_KIND_STATUS   2               /* no payload; vm_status is an errno */

/* vm_direction */
#define VAYU_QMUX_AP_TO_MODEM   0
#define VAYU_QMUX_MODEM_TO_AP   1

/* vm_qmi_type, the first byte of the QMI header */
#define VAYU_QMUX_QMI_REQUEST       0
#define VAYU_QMUX_QMI_RESPONSE      2
#define VAYU_QMUX_QMI_INDICATION    4
#define VAYU_QMUX_QMI_TYPE_UNKNOWN  0xff

/* vm_flags */
#define VAYU_QMUX_F_CONTROL     0x0001          /* service 0x00: QMux control, answered here */
#define VAYU_QMUX_F_LOCAL       0x0002          /* the kernel built/answered this; no modem */
#define VAYU_QMUX_F_REDACTED    0x0004          /* identifier-bearing (UIM/DMS/PBM/VOICE/WMS): no
                                                 * payload is ever logged for these services */
#define VAYU_QMUX_F_UNPARSED    0x0008          /* no valid 5/6/7-byte QMux header was found */
#define VAYU_QMUX_F_DROPPED     0x0010          /* the queue was full: the record was dropped */
#define VAYU_QMUX_F_TIMEOUT     0x0020          /* the kernel answered a request the modem did
                                                 * not answer; vm_status is ETIMEDOUT */
#define VAYU_QMUX_F_DENIED      0x0040          /* service not in the whitelist */

/* vm_status codes for a status record (plain errno values; these are the driver's own) */
#define VAYU_QMUX_ST_NOT_READY  1               /* the modem's QRTR transport is not up yet */
#define VAYU_QMUX_ST_NO_SERVICE 2               /* the service is whitelisted but not announced */

struct vayu_qmux_hdr {
	uint32_t vm_magic;              /* VAYU_QMUX_MAGIC */
	uint16_t vm_version;            /* VAYU_QMUX_VERSION */
	uint16_t vm_kind;               /* VAYU_QMUX_KIND_* */
	uint8_t  vm_direction;          /* VAYU_QMUX_AP_TO_MODEM / _MODEM_TO_AP */
	uint8_t  vm_service;            /* QMI service id, 0xff if unknown */
	uint8_t  vm_client;             /* QMux client id, 0xff if unknown */
	uint8_t  vm_qmi_type;           /* VAYU_QMUX_QMI_* */
	uint16_t vm_transaction;        /* QMI transaction id, as the QMux side saw it */
	uint16_t vm_message;            /* QMI message id */
	uint16_t vm_length;             /* payload bytes that follow this header */
	uint16_t vm_flags;              /* VAYU_QMUX_F_* */
	uint32_t vm_status;             /* 0, or an errno / VAYU_QMUX_ST_* */
	uint64_t vm_cookie;             /* opaque; a status record echoes the frame's cookie */
};

/* ioctl: counters, for a run's log. Never contains anything from a QMI payload. */
struct vayu_qmux_stats {
	uint32_t writes;                /* records written by userland */
	uint32_t reads;                 /* records read by userland */
	uint32_t frames_ap;             /* well-formed AP -> modem frames */
	uint32_t frames_modem;          /* well-formed modem -> AP frames */
	uint32_t control;               /* control-service requests answered locally */
	uint32_t forwarded;             /* requests handed to the modem */
	uint32_t replies;               /* modem responses routed back */
	uint32_t indications;           /* modem indications routed back */
	uint32_t timeouts;              /* requests answered locally after the modem was silent */
	uint32_t denied;                /* frames refused by the whitelist */
	uint32_t unparsed;              /* frames the QMux parser did not accept */
	uint32_t not_ready;             /* frames refused because QRTR was not up */
	uint32_t clients;               /* client ids allocated now */
	uint32_t txns;                  /* requests in flight now */
};

/*
 * A plain number, not _IOR(): the shim is built against the iOS SDK and not against the
 * kernel headers, and both sides must agree on one value. 0x71010001: 'q'-ish group, one
 * command, size left to the structure both sides compile.
 */
#define VAYU_QMUX_IOC_STATS     0x71010001u

#ifdef KERNEL
struct vayu_qrtr;

/* Called by vayu_modem.c once GLINK/QRTR is attached; reads vayu_qmux and starts nothing
 * unless it is 1. vayu_qmux_stop() is called on the modem's way down, before qrtr detach. */
void vayu_qmux_start(struct vayu_qrtr *q);
void vayu_qmux_stop(void);
#endif

#endif /* _SYS_VAYU_QMUX_H_ */
