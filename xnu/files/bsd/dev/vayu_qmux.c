/*
 * /dev/vayu-qmux: the QMux side of the CommCenter cellular bridge.
 *
 * What this is:
 *   CommCenter speaks QMI inside Apple's QMux framing. This device carries those frames
 *   between a userland shim inside CommCenter and the modem, which on vayu (SM8150-AC, X24)
 *   speaks QMI over QRTR/GLINK and has no QMux multiplexer. The QMux control service 0x00
 *   (GET_VERSION_INFO 0x0021, GET_CLIENT_ID 0x0022, RELEASE_CLIENT_ID 0x0023, SYNC 0x0027)
 *   is answered here and never reaches the modem; every other whitelisted service is
 *   forwarded to the QRTR port the modem announced for it.
 *
 * Why the control service is local:
 *   A QMux client id exists only in the QMux header. The modem's QMI-over-QRTR transport
 *   identifies a client by its QRTR port, so the modem has no idea what "client id 3" means
 *   and would reject or misroute a request that carried one. The shim therefore asks us for
 *   ids, we hand out synthetic ones, remember which service each names, and rewrite the QMI
 *   transaction id of each forwarded request to a value that is unique on our one QRTR port
 *   (two QMux clients may both use transaction 1). Responses are matched back and the AP's
 *   own transaction id and client id are put back into the frame handed to the shim.
 *
 * Boot-args:
 *   vayu_qmux=0        nothing, and no /dev/vayu-qmux (the default)
 *   vayu_qmux=1        the bridge is up
 *   vayu_qmux_wds=1    also allow WDS 0x01; default 0 (data is another stream's work)
 *   vayu_qmux_wds_bind=1  with vayu_qmux_wds=1: CommCenter's WDS Bind Mux Data Port is pointed at
 *                      this SoC's embedded IPA port and the kernel's one mux (vqm_wds_bind_rewrite();
 *                      sysctl kern.vayu_qmux_wds_bind shows what was asked and what was sent)
 *   vayu_qmux_trace=0  no per-frame header lines (default 1)
 *   vayu_qmux_timeout_ms=N  answer a forwarded request locally with a QMI error after N ms
 *                      of modem silence (default 15000, 1000..60000)
 *   vayu_qmux_per_client=1  one QRTR port of our node for each QMux client (default 0: all of
 *                      them share the data port). The modem keeps a client's Bind Subscription,
 *                      Bind Mux Data Port and Indication Register per QRTR port, so on one shared
 *                      port CommCenter's two VOICE clients, its WDS clients and, with two SIMs, its
 *                      per-subscription clients overwrite each other's; and every indication is
 *                      fanned out to every client of the service. With it each client's requests
 *                      leave from its own port, what comes back on that port is that client's
 *                      alone, and the port is closed (the modem hears DEL_CLIENT and forgets the
 *                      client) on RELEASE_CLIENT_ID and when /dev/vayu-qmux is closed. A client
 *                      for which no port is left shares the data port, as without the argument.
 *   vayu_call_bb=1     (bsd/dev/vayu_voice.c's) Apple's QMI Audio Service 0xE1 is allowed here and
 *                      answered by vayu_voice_bb_request(), its indications come back through
 *                      vayu_voice_bb_attach(); no modem port carries it
 *   vayu_qmux_bsp=1    Apple's BSP service 0xE2 is allowed and answered here: SetAntennaPreference
 *                      and SetAudioLoggingEnabled succeed, anything else is NOT_SUPPORTED
 *                      (default 0: refused like every unknown service)
 *
 * Whitelist: CTL 0x00 (local), DMS 0x02, NAS 0x03, WMS 0x05, VOICE 0x09, UIM 0x0b, WDS 0x01
 * only with vayu_qmux_wds=1, 0xE1 with vayu_call_bb=1 and 0xE2 with vayu_qmux_bsp=1 (both
 * local). Everything else gets a QMI failure (not silence: silence is a timeout in Apple's
 * stack). This driver never sends a request the shim did not send,
 * and it logs no QMI payload, ever; DMS/UIM/VOICE/WMS frames are flagged REDACTED.
 *
 * The wire format (32-byte record header, one read/write per record) is sys/vayu_qmux.h.
 * The protocol core (framing, control service, client and transaction tables) is
 * bsd/dev/vayu_qmux_proto.c, which host/test_qmux.c compiles and tests on the host.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/hakios.qrtr.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_qmux.h>
#include <sys/vayu_voice_bb.h>

#include "vayu_qmux_proto.h"

/*
 * a6's WMS observer (bsd/dev/vayu_sms.c, vayu_sms=3): with CommCenter as the WMS client the kernel
 * only reads the WMS frames that cross this bridge. It does not sleep and takes only its own lock.
 * Its header comes with that branch; without it this file builds and observes nothing.
 */
#if __has_include(<sys/vayu_sms_wms.h>)
#include <sys/vayu_sms.h>
#define VQM_SMS_OBSERVE 1
#else
#define VQM_SMS_OBSERVE 0
#endif

extern void loader_log_putc(char c);

/* The 32-byte record is the ABI the shim compiles against; a padding surprise must fail the
 * build here, not on the wire. */
_Static_assert(sizeof(struct vayu_qmux_hdr) == 32, "struct vayu_qmux_hdr must be 32 bytes");

#define VQM_DEVICE              "vayu-qmux"
/* Only CommCenter (and abm-helper) run as _wireless, uid 25; the node lets any opener send any
 * whitelisted QMI request -- a VOICE Dial or a WMS Raw Send -- so it is theirs alone (was 0666). */
#define VQM_DEVICE_UID          25
#define VQM_QUEUE               64      /* records in each direction */
#define VQM_STOP_MS             3000    /* the longest wait for the thread to end */
#define VQM_TRACE_SIZE          8192
#define QMI_TYPE_MASK_LOCAL     0x06

#ifndef IO_NDELAY
#define IO_NDELAY               0x0010
#endif

#define QMI_SERVICE_CTL         0x00
#define QMI_SERVICE_WDS         0x01
#define QMI_SERVICE_DMS         0x02
#define QMI_SERVICE_NAS         0x03
#define QMI_SERVICE_WMS         0x05
#define QMI_SERVICE_VOICE       0x09
#define QMI_SERVICE_UIM         0x0b

/* The services whose QRTR ports are looked up when the bridge comes up. The first four go on
 * the data port (the router keeps four lookups per port); the rest on a second, lookup-only
 * port. A request is always sent from the data port, which is where the reply must arrive. */
static const uint8_t vqm_lookup_first[] = {
	QMI_SERVICE_DMS, QMI_SERVICE_NAS, QMI_SERVICE_UIM, QMI_SERVICE_WMS,
};
static const uint8_t vqm_lookup_second[] = {
	QMI_SERVICE_VOICE, QMI_SERVICE_WDS,
};

struct vqm_record {
	struct vqm_record *next;
	uint32_t len;                   /* the whole record: 32 + payload */
	uint32_t alloc;                 /* what kfree_data() has to be given */
	uint8_t data[1];                /* struct vayu_qmux_hdr + payload */
};

struct vqm_queue {
	struct vqm_record *head, *tail;
	uint32_t count;
};

/* vayu_qmux_per_client=1: the QRTR port of proto.clients[i] is cports[i] (same index). */
struct vqm_cport {
	struct hakios_qrtr_port *port;  /* NULL: not opened yet (or none left: the data port carries it) */
	uint8_t service, client;        /* the client it was opened for; a slot reused by another is stale */
	bool no_port;                   /* the open failed once: said, and the data port is used */
};

struct vqm_softc {
	lck_mtx_t lock;
	lck_grp_t *lock_group;
	struct vqm_state proto;
	struct vqm_queue rx, tx;
	struct selinfo rsel;
	struct hakios_qrtr_port *data_port;
	struct hakios_qrtr_port *lookup_port;
	const struct hakios_qrtr_ops *ops;
	struct vayu_qrtr *qrtr;
	thread_t thread;
	bool enabled, running, stop, open;
	bool lookups_done;
	bool settled;                  /* the 2 s grace for the modem's first announcements */
	bool reopen;
	bool trace;
	bool wds_bind;                 /* vayu_qmux_wds_bind=1: WDS Bind Mux Data Port gets the IPA port */
	uint32_t wds_binds;
	uint8_t wds_first_mux;
	uint32_t timeout_ms;
	uint32_t writes, reads_taken, queue_drops;
	bool per_client;               /* vayu_qmux_per_client=1 */
	bool cports_flush;             /* the device was closed or opened: every client port goes */
	struct vqm_cport cports[VQM_CLIENTS];
	uint32_t cport_opens, cport_closes, cport_fallbacks, cport_strays;
	uint32_t local_answers, local_indications;
};

static struct vqm_softc vqm;
static bool vqm_started;

/*
 * The data port CommCenter's WDS Bind is pointed at (vayu_qmux_wds_bind=1): the embedded IPA port
 * that bsd/dev/vayu_cell_data.c opens with DPM Open Port and formats with WDA, and the one QMAP mux
 * the kernel's packet path carries (sys/vayu_cell_data_qmi.h: VAYU_CELL_ENDPOINT_EMBEDDED 4,
 * VAYU_CELL_ENDPOINT_INTERFACE 1, VAYU_CELL_DATA_MUX 1; sys/vayu_cell_net.h VAYU_CELL_NET_MUX).
 */
#define VQM_DATA_EP_TYPE        4
#define VQM_DATA_EP_IFACE       1
#define VQM_DATA_MUX            1

static char vqm_wds_bind_text[200];
SYSCTL_STRING(_kern, OID_AUTO, vayu_qmux_wds_bind, CTLFLAG_RD, vqm_wds_bind_text,
    sizeof(vqm_wds_bind_text), "vayu-qmux: the last WDS Bind Mux Data Port as asked and as sent");

/* ---- logging (never a payload byte) ------------------------------------------------------ */

static LCK_GRP_DECLARE(vqm_trace_group, "vayu-qmux-trace");
static LCK_MTX_DECLARE(vqm_trace_lock, &vqm_trace_group);
static char vqm_trace_buf[VQM_TRACE_SIZE];
static size_t vqm_trace_used;

SYSCTL_STRING(_kern, OID_AUTO, vayu_qmux_trace, CTLFLAG_RD, vqm_trace_buf,
    sizeof(vqm_trace_buf), "vayu-qmux header trace (tail kept; no payload)");

static void
vqm_say(const char *format, ...) __printflike(1, 2);

static void
vqm_say(const char *format, ...)
{
	char line[320];
	va_list list;
	size_t length;
	int written;

	va_start(list, format);
	written = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (written <= 0) {
		return;
	}
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);

	length = strlen(line);
	lck_mtx_lock(&vqm_trace_lock);
	if (vqm_trace_used + length + 1 > sizeof(vqm_trace_buf)) {
		vqm_trace_used = 0;             /* the tail is what a run's PASS/FAIL is read from */
	}
	for (size_t i = 0; i < length; i++) {
		vqm_trace_buf[vqm_trace_used++] = line[i];
	}
	vqm_trace_buf[vqm_trace_used] = 0;
	lck_mtx_unlock(&vqm_trace_lock);
}

static const char *
vqm_service_name(uint8_t service)
{
	switch (service) {
	case QMI_SERVICE_CTL: return "CTL";
	case QMI_SERVICE_WDS: return "WDS";
	case QMI_SERVICE_DMS: return "DMS";
	case QMI_SERVICE_NAS: return "NAS";
	case QMI_SERVICE_WMS: return "WMS";
	case QMI_SERVICE_VOICE: return "VOICE";
	case QMI_SERVICE_UIM: return "UIM";
	case VQM_SERVICE_AUDIO: return "AUDIO";
	case VQM_SERVICE_BSP: return "BSP";
	default: return "?";
	}
}

static const char *
vqm_type_name(uint8_t type)
{
	switch (type & QMI_TYPE_MASK_LOCAL) {
	case 0x00: return "req";
	case 0x02: return "rsp";
	case 0x04: return "ind";
	default: return "?";
	}
}

/* DMS/UIM/VOICE/WMS carry identifiers in their TLVs; PBM is not whitelisted. The log never has a
 * payload, and for these services it says so, so no one is tempted to add one later. */
static bool
vqm_redacted(uint8_t service)
{
	/* WMS too: its TLVs carry the number and the text of a message. */
	return service == QMI_SERVICE_DMS || service == QMI_SERVICE_UIM ||
	    service == QMI_SERVICE_VOICE || service == QMI_SERVICE_WMS;
}

static void
vqm_log_frame(const char *direction, const struct vqm_frame *f, uint16_t flags)
{
	vqm_say("vayu-qmux: %s svc=0x%02x(%s) cli=0x%02x %s msg=0x%04x txn=0x%04x "
	    "qmux=%u flags=0x%04x%s\n", direction, f->service, vqm_service_name(f->service),
	    f->client, vqm_type_name(f->qmi_type), f->message, f->transaction, f->hdr_len, flags,
	    vqm_redacted(f->service) ? " payload-masked" : "");
}

/* ---- records and queues ------------------------------------------------------------------ */

static uint64_t
vqm_now_ms(void)
{
	uint64_t uptime, nanoseconds;

	clock_get_uptime(&uptime);
	absolutetime_to_nanoseconds(uptime, &nanoseconds);
	return nanoseconds / NSEC_PER_MSEC;
}

static struct vqm_record *
vqm_record_new(uint8_t kind, uint8_t direction, uint8_t service, uint8_t client, uint8_t qmi_type,
    uint16_t transaction, uint16_t message, uint16_t flags, uint32_t status, uint64_t cookie,
    size_t payload_len)
{
	size_t total = sizeof(struct vayu_qmux_hdr) + payload_len;
	size_t alloc = __builtin_offsetof(struct vqm_record, data) + total;
	struct vqm_record *rec;
	struct vayu_qmux_hdr *hdr;

	if (total > sizeof(struct vayu_qmux_hdr) + VAYU_QMUX_MAX_PAYLOAD) {
		return NULL;
	}
	/* Z_NOWAIT: this is called from the router's delivery thread as well, where the caller
	 * holds vqm.lock and must not block. A dropped record is logged; the modem retries. */
	rec = kalloc_data(alloc, Z_NOWAIT);
	if (rec == NULL) {
		return NULL;
	}
	rec->next = NULL;
	rec->len = (uint32_t)total;
	rec->alloc = (uint32_t)alloc;
	memset(rec->data, 0, sizeof(struct vayu_qmux_hdr));

	hdr = (struct vayu_qmux_hdr *)rec->data;
	hdr->vm_magic = VAYU_QMUX_MAGIC;
	hdr->vm_version = VAYU_QMUX_VERSION;
	hdr->vm_kind = kind;
	hdr->vm_direction = direction;
	hdr->vm_service = service;
	hdr->vm_client = client;
	hdr->vm_qmi_type = qmi_type;
	hdr->vm_transaction = transaction;
	hdr->vm_message = message;
	hdr->vm_length = (uint16_t)payload_len;
	hdr->vm_flags = flags;
	hdr->vm_status = status;
	hdr->vm_cookie = cookie;
	return rec;
}

/* After a builder wrote fewer bytes than the record reserved. */
static void
vqm_record_shorten(struct vqm_record *rec, size_t payload_len)
{
	struct vayu_qmux_hdr *hdr = (struct vayu_qmux_hdr *)rec->data;

	hdr->vm_length = (uint16_t)payload_len;
	rec->len = (uint32_t)(sizeof(struct vayu_qmux_hdr) + payload_len);
}

static void
vqm_record_free(struct vqm_record *rec)
{
	if (rec != NULL) {
		kfree_data(rec, rec->alloc);
	}
}

static void
vqm_queue_put(struct vqm_queue *q, struct vqm_record *rec, bool drop_oldest)
{
	if (rec == NULL) {
		return;
	}
	if (q->count >= VQM_QUEUE) {
		vqm.queue_drops++;
		if (!drop_oldest) {
			vqm_record_free(rec);
			return;
		}
		struct vqm_record *old = q->head;

		q->head = old->next;
		if (q->head == NULL) {
			q->tail = NULL;
		}
		q->count--;
		vqm_record_free(old);
	}
	if (q->tail != NULL) {
		q->tail->next = rec;
	} else {
		q->head = rec;
	}
	q->tail = rec;
	rec->next = NULL;
	q->count++;
}

static struct vqm_record *
vqm_queue_get(struct vqm_queue *q)
{
	struct vqm_record *rec = q->head;

	if (rec != NULL) {
		q->head = rec->next;
		if (q->head == NULL) {
			q->tail = NULL;
		}
		q->count--;
		rec->next = NULL;
	}
	return rec;
}

static void
vqm_queue_clear(struct vqm_queue *q)
{
	struct vqm_record *rec;

	while ((rec = vqm_queue_get(q)) != NULL) {
		vqm_record_free(rec);
	}
}

/* A status record (kind 2): the header only, never a payload. */
static void
vqm_status(uint16_t flags, uint32_t status, uint8_t service, uint8_t client, uint64_t cookie)
{
	struct vqm_record *rec = vqm_record_new(VAYU_QMUX_KIND_STATUS, VAYU_QMUX_MODEM_TO_AP,
	    service, client, VAYU_QMUX_QMI_TYPE_UNKNOWN, 0, 0, flags, status, cookie, 0);

	if (rec == NULL) {
		return;
	}
	vqm_queue_put(&vqm.rx, rec, true);
	wakeup(&vqm.rx);
	selwakeup(&vqm.rsel);
}

/* ---- QRTR -------------------------------------------------------------------------------- */

static void
vqm_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port);

static void
vqm_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length);

static int
vqm_ports_open_locked(void)
{
	int error;

	if (vqm.ops == NULL) {
		return ENXIO;
	}
	if (vqm.data_port == NULL) {
		vqm.data_port = vqm.ops->open(vqm_receive, vqm_server, &vqm);
		vqm.lookups_done = false;
		vqm.settled = false;
		if (vqm.data_port == NULL) {
			return ENXIO;
		}
		vqm_say("vayu-qmux: QRTR port is open; looking the services up\n");
	}
	if (!vqm.lookups_done) {
		for (size_t i = 0; i < sizeof(vqm_lookup_first) / sizeof(vqm_lookup_first[0]); i++) {
			error = vqm.ops->lookup(vqm.data_port, vqm_lookup_first[i], 1, 0);
			if (error != 0) {
				vqm_say("vayu-qmux: lookup 0x%02x on the data port: %d\n",
				    vqm_lookup_first[i], error);
			}
		}
		if (vqm.lookup_port == NULL) {
			vqm.lookup_port = vqm.ops->open(vqm_receive, vqm_server, &vqm);
		}
		if (vqm.lookup_port != NULL) {
			for (size_t i = 0; i < sizeof(vqm_lookup_second) / sizeof(vqm_lookup_second[0]); i++) {
				if (vqm_lookup_second[i] == QMI_SERVICE_WDS && !vqm.proto.wds) {
					continue;
				}
				error = vqm.ops->lookup(vqm.lookup_port, vqm_lookup_second[i], 1, 0);
				if (error != 0) {
					vqm_say("vayu-qmux: lookup 0x%02x on the second port: %d\n",
					    vqm_lookup_second[i], error);
				}
			}
		} else {
			vqm_say("vayu-qmux: no second QRTR port; VOICE/WDS not looked up\n");
		}
		vqm.lookups_done = true;
	}
	return 0;
}

static void
vqm_ports_close(void)
{
	struct hakios_qrtr_port *data = vqm.data_port;
	struct hakios_qrtr_port *second = vqm.lookup_port;

	vqm.data_port = NULL;
	vqm.lookup_port = NULL;
	vqm.lookups_done = false;
	if (vqm.ops != NULL) {
		if (second != NULL) {
			vqm.ops->close(second);
		}
		if (data != NULL) {
			vqm.ops->close(data);
		}
	}
}

static void
vqm_server(void *context __unused, bool arrived, uint32_t service, uint32_t version,
    uint32_t instance, uint32_t node, uint32_t port)
{
	lck_mtx_lock(&vqm.lock);
	if (arrived) {
		if (!vqm_allowed(&vqm.proto, (uint8_t)service)) {
			lck_mtx_unlock(&vqm.lock);
			return;
		}
		vqm_service_set(&vqm.proto, (uint8_t)service, node, port, version, instance);
		vqm_say("vayu-qmux: service 0x%02x(%s) is at node %u port %u, version %u instance %u\n",
		    service, vqm_service_name((uint8_t)service), node, port, version, instance);
	} else {
		vqm_service_clear(&vqm.proto, (uint8_t)service, port);
		vqm_say("vayu-qmux: service 0x%02x(%s) at node %u port %u is gone\n", service,
		    vqm_service_name((uint8_t)service), node, port);
	}
	lck_mtx_unlock(&vqm.lock);
}

/* Build a modem -> AP frame around `qmi` using the framing of `client` (or plain QMux 6 and
 * client 0 if there is none). Returns the record, not queued. The QMI transaction id is
 * rewritten to `transaction` after the copy, so the caller need not build a second buffer. */
static struct vqm_record *
vqm_frame_record(uint8_t service, uint8_t type, uint16_t transaction, const uint8_t *qmi,
    size_t qmi_len, const struct vqm_client *client, uint16_t flags)
{
	struct vqm_client fallback = { .style = 6, .len_mode = VQM_LEN_ALL_AFTER_VERSION };
	const struct vqm_client *shape = client != NULL ? client : &fallback;
	size_t cap = 7 + qmi_len;
	struct vqm_record *rec;
	struct vayu_qmux_hdr *hdr;
	size_t n;

	rec = vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_MODEM_TO_AP, service,
	    shape->client, type, transaction, qmi_len >= 7 ? (uint16_t)(qmi[3] | (qmi[4] << 8)) : 0,
	    flags, 0, 0, cap);
	if (rec == NULL) {
		return NULL;
	}
	n = vqm_build(rec->data + sizeof(struct vayu_qmux_hdr), cap,
	    shape->style, shape->len_mode, shape->ctl_flags, service, shape->client, type, qmi,
	    qmi_len);
	if (n == 0) {
		vqm_record_free(rec);
		return NULL;
	}
	hdr = (struct vayu_qmux_hdr *)rec->data;
	hdr->vm_transaction = transaction;
	hdr->vm_length = (uint16_t)n;
	rec->len = (uint32_t)(sizeof(struct vayu_qmux_hdr) + n);
	if (flags & VAYU_QMUX_F_REDACTED) {
		hdr->vm_flags |= VAYU_QMUX_F_REDACTED;
	}
	if (type == 0x02 && qmi_len >= 7 && n >= shape->style) {
		vqm_set_transaction(rec->data + sizeof(struct vayu_qmux_hdr) + shape->style,
		    transaction);
	}
	return rec;
}

static void
vqm_receive(void *context __unused, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *qmi = data;
	uint8_t service = 0xff;
	struct vqm_service *svc;
	uint8_t type;
	uint16_t transaction, message;

	if (length < 7 || length > VAYU_QMUX_MAX_PAYLOAD) {
		vqm_say("vayu-qmux: a datagram of %zu bytes from node %u port %u is not QMI\n",
		    length, node, port);
		return;
	}
	type = qmi[0] & QMI_TYPE_MASK_LOCAL;
	transaction = (uint16_t)(qmi[1] | (qmi[2] << 8));
	message = (uint16_t)(qmi[3] | (qmi[4] << 8));
	if ((size_t)(qmi[5] | (qmi[6] << 8)) != length - 7) {
		vqm_say("vayu-qmux: a QMI datagram from port %u has a length that does not add up\n",
		    port);
		return;
	}

	lck_mtx_lock(&vqm.lock);
	svc = vqm_service_by_port(&vqm.proto, &service, port);
	if (svc == NULL) {
		lck_mtx_unlock(&vqm.lock);
		vqm_say("vayu-qmux: a datagram from node %u port %u: no service of ours there\n", node,
		    port);
		return;
	}
#if VQM_SMS_OBSERVE
	if (service == QMI_SERVICE_WMS) {
		vayu_sms_wms_observe(1, qmi, length);
	}
#endif
	if (type == 0x02) {
		uint16_t ap = transaction;
		uint8_t client = 0;
		uint8_t style = 6, mode = VQM_LEN_ALL_AFTER_VERSION, ctl = 0;
		struct vqm_client shape;
		struct vqm_client *found = NULL;
		struct vqm_record *rec;
		uint16_t flags = 0;

		(void)vqm_txn_complete(&vqm.proto, service, transaction, &ap, &client, &style, &mode,
		    &ctl);
		memset(&shape, 0, sizeof(shape));
		shape.style = style;
		shape.len_mode = mode;
		shape.ctl_flags = ctl;
		shape.client = client;
		found = vqm_client_find(&vqm.proto, service, client);
		if (found == NULL) {
			/* No transaction of ours (a late reply or one after a restart): route it to a
			 * client of the service if there is one, else to client 0. */
			for (size_t i = 0; i < VQM_CLIENTS; i++) {
				if (vqm.proto.clients[i].used && vqm.proto.clients[i].service == service) {
					found = &vqm.proto.clients[i];
					break;
				}
			}
		}
		shape = found != NULL ? *found : shape;
		rec = vqm_frame_record(service, 0x02, ap, qmi, length, &shape,
		    flags | (vqm_redacted(service) ? VAYU_QMUX_F_REDACTED : 0));
		if (rec != NULL) {
			vqm_queue_put(&vqm.rx, rec, true);
			wakeup(&vqm.rx);
			selwakeup(&vqm.rsel);
			vqm.proto.counters.replies++;
		}
	} else if (type == 0x04) {
		unsigned delivered = 0, own_port = 0;

		for (size_t i = 0; i < VQM_CLIENTS; i++) {
			struct vqm_client *c = &vqm.proto.clients[i];
			struct vqm_record *rec;

			if (!c->used || c->service != service) {
				continue;
			}
			if (vqm.per_client && vqm.cports[i].port != NULL && vqm.cports[i].service == service &&
			    vqm.cports[i].client == c->client) {
				own_port++;
				continue;               /* (its own port brings its own indications) */
			}
			rec = vqm_frame_record(service, 0x04, transaction, qmi, length, c,
			    vqm_redacted(service) ? VAYU_QMUX_F_REDACTED : 0);
			if (rec != NULL) {
				vqm_queue_put(&vqm.rx, rec, true);
				delivered++;
			}
		}
		if (delivered == 0 && own_port == 0) {
			struct vqm_client fallback = { .style = 6, .len_mode = VQM_LEN_ALL_AFTER_VERSION,
				.client = 0 };
			struct vqm_record *rec = vqm_frame_record(service, 0x04, transaction, qmi,
			    length, &fallback, VAYU_QMUX_F_REDACTED);

			if (rec != NULL) {
				vqm_queue_put(&vqm.rx, rec, true);
				delivered = 1;
			}
		}
		if (delivered != 0) {
			wakeup(&vqm.rx);
			selwakeup(&vqm.rsel);
			vqm.proto.counters.indications++;
		}
	} else {
		vqm_say("vayu-qmux: a QMI %s (0x%04x) from the modem for service 0x%02x is not a "
		    "response or an indication; dropped\n", vqm_type_name(type), message, service);
	}
	lck_mtx_unlock(&vqm.lock);
}

/* ---- per-client QRTR ports (vayu_qmux_per_client=1) --------------------------------------- */

static void
vqm_cport_server(void *context __unused, bool arrived __unused, uint32_t service __unused,
    uint32_t version __unused, uint32_t instance __unused, uint32_t node __unused, uint32_t port __unused)
{
	/* (no lookup is made on a client's port: where the services are is the data port's business) */
}

/* A datagram on the QRTR port of one client: it is that client's (vqm_route_own()). */
static void
vqm_receive_client(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct vqm_cport *cp = context;
	size_t own = (size_t)(cp - vqm.cports);
	const uint8_t *qmi = data;
	struct vqm_service *svc;
	struct vqm_dest d;
	uint8_t service, type;
	uint16_t transaction;
	uint32_t strays;

	if (length < 7 || length > VAYU_QMUX_MAX_PAYLOAD || (size_t)(qmi[5] | (qmi[6] << 8)) != length - 7) {
		vqm_say("vayu-qmux: a datagram of %zu bytes from node %u port %u to a client's port is not QMI\n",
		    length, node, port);
		return;
	}
	type = qmi[0] & QMI_TYPE_MASK_LOCAL;
	transaction = (uint16_t)(qmi[1] | (qmi[2] << 8));

	lck_mtx_lock(&vqm.lock);
	if (own >= VQM_CLIENTS || cp->port == NULL || !vqm.proto.clients[own].used ||
	    vqm.proto.clients[own].service != cp->service || vqm.proto.clients[own].client != cp->client) {
		strays = ++vqm.cport_strays;
		lck_mtx_unlock(&vqm.lock);
		if (strays <= 8) {
			vqm_say("vayu-qmux: a datagram from node %u port %u for a client that is gone; dropped\n", node,
			    port);
		}
		return;
	}
	service = cp->service;
	svc = vqm_service_get(&vqm.proto, service);
	if (svc == NULL || svc->node != node || svc->port != port) {
		strays = ++vqm.cport_strays;
		lck_mtx_unlock(&vqm.lock);
		if (strays <= 8) {
			vqm_say("vayu-qmux: a datagram from node %u port %u on the port of a 0x%02x(%s) client is not "
			    "from that service; dropped\n", node, port, service, vqm_service_name(service));
		}
		return;
	}
#if VQM_SMS_OBSERVE
	if (service == QMI_SERVICE_WMS) {
		vayu_sms_wms_observe(1, qmi, length);
	}
#endif
	if (vqm_route_own(&vqm.proto, own, type, transaction, &d) == 1) {
		struct vqm_client shape = { .used = 1, .service = service, .client = d.client, .style = d.style,
			                    .len_mode = d.len_mode, .ctl_flags = d.ctl_flags };
		struct vqm_record *rec = vqm_frame_record(service, type, d.transaction, qmi, length, &shape,
		    vqm_redacted(service) ? VAYU_QMUX_F_REDACTED : 0);

		if (rec != NULL) {
			vqm_queue_put(&vqm.rx, rec, true);
			wakeup(&vqm.rx);
			selwakeup(&vqm.rsel);
			if (type == 0x02) {
				vqm.proto.counters.replies++;
			} else {
				vqm.proto.counters.indications++;
			}
		}
	} else {
		vqm_say("vayu-qmux: a QMI %s (0x%04x) from the modem for service 0x%02x is not a response or an "
		    "indication; dropped\n", vqm_type_name(type), (unsigned int)(qmi[3] | (qmi[4] << 8)), service);
	}
	lck_mtx_unlock(&vqm.lock);
}

static unsigned
vqm_cports_open_count_locked(void)
{
	unsigned n = 0;

	for (size_t i = 0; i < VQM_CLIENTS; i++) {
		n += vqm.cports[i].port != NULL ? 1 : 0;
	}
	return n;
}

/*
 * The port a request of (service, client) leaves from: the client's own, opened on its first request
 * (whether its id came from GET_CLIENT_ID or vqm_client_ensure), or the data port when none is left.
 * A port still open for the slot's previous client is handed back in *stale for the caller to close
 * outside the lock (qrtr_close() waits for the router's callbacks, which take this lock).
 */
static struct hakios_qrtr_port *
vqm_cport_port_locked(uint8_t service, uint8_t client, struct hakios_qrtr_port **stale)
{
	struct vqm_client *c = vqm_client_find(&vqm.proto, service, client);
	struct vqm_cport *cp;

	if (c == NULL) {
		return vqm.data_port;
	}
	cp = &vqm.cports[c - vqm.proto.clients];
	if (cp->service != service || cp->client != client) {
		if (cp->port != NULL) {
			*stale = cp->port;
			cp->port = NULL;
			vqm.cport_closes++;
		}
		cp->no_port = false;
		cp->service = service;
		cp->client = client;
	}
	if (cp->port != NULL) {
		return cp->port;
	}
	if (cp->no_port) {
		return vqm.data_port;
	}
	cp->port = vqm.ops->open(vqm_receive_client, vqm_cport_server, cp);
	if (cp->port == NULL) {
		cp->no_port = true;
		vqm.cport_fallbacks++;
		vqm_say("vayu-qmux: no QRTR port is left for svc=0x%02x(%s) cli=0x%02x; it shares the data port\n",
		    service, vqm_service_name(service), client);
		return vqm.data_port;
	}
	vqm.cport_opens++;
	vqm_say("vayu-qmux: per-client QRTR port for svc=0x%02x(%s) cli=0x%02x is open (%u client port(s))\n",
	    service, vqm_service_name(service), client, vqm_cports_open_count_locked());
	return cp->port;
}

/*
 * Closes the ports of clients that are gone (released, or their slot taken by another), or every one
 * (`all`, or the device was closed or opened since: a new CommCenter must not inherit the modem-side
 * state of the old one's ports). Only the worker thread and the stop path call this, and never with
 * the lock held; nothing else closes a client's port, so a send in flight never meets a closed one.
 */
static void
vqm_cports_reap(bool all)
{
	struct hakios_qrtr_port *gone[VQM_CLIENTS];
	unsigned n = 0;

	lck_mtx_lock(&vqm.lock);
	all = all || vqm.cports_flush;
	vqm.cports_flush = false;
	for (size_t i = 0; i < VQM_CLIENTS; i++) {
		struct vqm_cport *cp = &vqm.cports[i];
		const struct vqm_client *c = &vqm.proto.clients[i];
		bool mine = c->used && c->service == cp->service && c->client == cp->client;

		if (all || !mine) {
			if (cp->port != NULL) {
				gone[n++] = cp->port;
				cp->port = NULL;
			}
			cp->no_port = false;
			cp->service = 0;
			cp->client = 0;
		}
	}
	vqm.cport_closes += n;
	lck_mtx_unlock(&vqm.lock);
	for (unsigned i = 0; i < n; i++) {
		vqm.ops->close(gone[i]);
	}
	if (n != 0) {
		vqm_say("vayu-qmux: %u client QRTR port(s) closed (%s)\n", n,
		    all ? "all: the device was closed or reopened, or the router went" : "their clients were released");
	}
}

/* ---- the kernel's own services: 0xE1 (vayu_voice.c), 0xE2 (here) ------------------------------- */

/* One request of a local service, answered at once. Called with the lock held. */
static void
vqm_local_answer_locked(const struct vqm_frame *f, uint64_t cookie)
{
	struct vqm_client *c = vqm_client_ensure(&vqm.proto, f->service, f->client, f->style, f->len_mode,
	    f->ctl_flags);
	struct vqm_record *rec = NULL;
	uint16_t error = 0;
	size_t n = 0;

	if (f->service == VQM_SERVICE_AUDIO && c != NULL) {
		uint8_t sdu[VAYU_VOICE_BB_ANSWER_MAX];

		/* f->qmi is the SDU a7's core reads: flags, transaction, message, length and the TLVs */
		if (vayu_voice_bb_request(f->service, f->client, f->qmi, (size_t)f->qmi_len + 7, sdu, sizeof(sdu),
		    &n) == 0 && n >= 7) {
			rec = vqm_frame_record(f->service, 0x02, f->transaction, sdu, n, c, VAYU_QMUX_F_LOCAL);
		}
	}
	if (rec != NULL) {
		((struct vayu_qmux_hdr *)rec->data)->vm_cookie = cookie;
	} else {
		uint8_t out[128];

		if (f->service == VQM_SERVICE_BSP) {
			n = vqm_bsp_reply(out, sizeof(out), f, &error);
		} else {
			n = vqm_error(out, sizeof(out), f, 1, 1);       /* failure, MALFORMED */
			error = 1;
		}
		rec = n == 0 ? NULL : vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_MODEM_TO_AP, f->service,
		    f->client, 0x02, f->transaction, f->message, VAYU_QMUX_F_LOCAL, 0, cookie, n);
		if (rec != NULL) {
			memcpy(rec->data + sizeof(struct vayu_qmux_hdr), out, n);
			vqm_record_shorten(rec, n);
		}
	}
	if (rec == NULL) {
		vqm_status(VAYU_QMUX_F_LOCAL, ENOMEM, f->service, f->client, cookie);
		return;
	}
	vqm_queue_put(&vqm.rx, rec, true);
	wakeup(&vqm.rx);
	selwakeup(&vqm.rsel);
	vqm.local_answers++;
	if (f->service == VQM_SERVICE_BSP && (vqm.trace || error != 0)) {
		vqm_say("vayu-qmux: local 0xE2(BSP) cli=0x%02x msg 0x%04x -> %s\n", f->client, f->message,
		    error == 0 ? "success" : error == 94 ? "NOT_SUPPORTED" : "MALFORMED");
	}
}

/* An indication of a local service for one client (vayu_voice.c's thread call; no lock of ours held). */
static void
vqm_bb_deliver(uint8_t service, uint8_t client, const uint8_t *sdu, size_t length)
{
	struct vqm_client *c;
	struct vqm_record *rec = NULL;

	if (sdu == NULL || length < 7 || length > VAYU_QMUX_MAX_PAYLOAD) {
		return;
	}
	lck_mtx_lock(&vqm.lock);
	if (vqm.enabled && vqm.open && vqm_local(&vqm.proto, service) &&
	    (c = vqm_client_find(&vqm.proto, service, client)) != NULL) {
		rec = vqm_frame_record(service, 0x04, (uint16_t)(sdu[1] | (sdu[2] << 8)), sdu, length, c,
		    VAYU_QMUX_F_LOCAL);
	}
	if (rec != NULL) {
		vqm_queue_put(&vqm.rx, rec, true);
		wakeup(&vqm.rx);
		selwakeup(&vqm.rsel);
		vqm.local_indications++;
		vqm.proto.counters.indications++;
	}
	lck_mtx_unlock(&vqm.lock);
}

/* The device is closed or opened: a7's module forgets its 0xE1 clients (queued indications for them go nowhere). */
static void
vqm_local_release_all_locked(void)
{
	if (!vqm.proto.local_e1) {
		return;
	}
	for (size_t i = 0; i < VQM_CLIENTS; i++) {
		if (vqm.proto.clients[i].used && vqm.proto.clients[i].service == VQM_SERVICE_AUDIO) {
			vayu_voice_bb_release(VQM_SERVICE_AUDIO, vqm.proto.clients[i].client);
		}
	}
}

/* ---- the worker thread ------------------------------------------------------------------- */

/* One dequeued TX record. Called with the lock held; drops it around the send. */
static void
vqm_tx_one_locked(struct vqm_record *rec)
{
	struct vayu_qmux_hdr *hdr = (struct vayu_qmux_hdr *)rec->data;
	uint8_t *payload = rec->data + sizeof(struct vayu_qmux_hdr);
	size_t payload_len = hdr->vm_length;
	struct vqm_frame f;
	struct vqm_service *svc;
	uint32_t node, port;
	uint16_t modem_txn = 0;
	bool tracked = false;
	struct hakios_qrtr_port *from, *stale = NULL;
	int error;

	if (vqm_parse(payload, payload_len, &f) != 0) {
		vqm.proto.counters.unparsed++;
		vqm_status(VAYU_QMUX_F_UNPARSED, EINVAL, hdr->vm_service, hdr->vm_client,
		    hdr->vm_cookie);
		return;
	}
	svc = vqm_service_get(&vqm.proto, f.service);
	if (svc == NULL) {
		struct vqm_record *answer;
		uint8_t out[128];

		vqm.proto.counters.not_ready++;
		size_t n = vqm_error(out, sizeof(out), &f, 1, 3);      /* QMI failure, INTERNAL */
		answer = vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_MODEM_TO_AP, f.service,
		    f.client, 0x02, f.transaction, f.message, VAYU_QMUX_F_LOCAL, 0, hdr->vm_cookie,
		    n);
		if (answer != NULL && n != 0) {
			memcpy(answer->data + sizeof(struct vayu_qmux_hdr), out, n);
			vqm_record_shorten(answer, n);
			vqm_queue_put(&vqm.rx, answer, true);
			wakeup(&vqm.rx);
			selwakeup(&vqm.rsel);
		} else {
			vqm_record_free(answer);
		}
		vqm_status(VAYU_QMUX_F_LOCAL, VAYU_QMUX_ST_NO_SERVICE, f.service, f.client, hdr->vm_cookie);
		return;
	}
	node = svc->node;
	port = svc->port;

	if (f.qmi_type == 0x00 && f.service == QMI_SERVICE_WDS && vqm.wds_bind) {
		struct vqm_wds_bind was;
		int r = vqm_wds_bind_rewrite(payload + f.hdr_len, payload_len - f.hdr_len,
		    VQM_DATA_EP_TYPE, VQM_DATA_EP_IFACE, VQM_DATA_MUX, &was);

		if (r == 1) {
			vqm.wds_binds++;
			snprintf(vqm_wds_bind_text, sizeof(vqm_wds_bind_text),
			    "client %u asked ep_type %u iface %u mux %u%s%s; sent ep_type %u iface %u mux %u (%u bind(s))",
			    f.client, was.ep_type, was.iface_id, was.mux,
			    was.had_endpoint ? "" : " (no endpoint TLV)", was.had_mux ? "" : " (no mux TLV)",
			    VQM_DATA_EP_TYPE, VQM_DATA_EP_IFACE, VQM_DATA_MUX, vqm.wds_binds);
			vqm_say("vayu-qmux: WDS Bind Mux Data Port: %s\n", vqm_wds_bind_text);
			if (vqm.wds_binds > 1 && was.had_mux && was.mux != vqm.wds_first_mux) {
				vqm_say("vayu-qmux: WDS Bind: a second PDP (mux %u after %u) shares the one IPA mux; "
				    "its packets and the first one's are not told apart\n", was.mux, vqm.wds_first_mux);
			}
			if (vqm.wds_binds == 1) {
				vqm.wds_first_mux = was.mux;
			}
		} else if (r < 0) {
			vqm_say("vayu-qmux: WDS Bind Mux Data Port from client %u is malformed; sent as it came\n",
			    f.client);
		}
	}
	if (f.qmi_type == 0x00) {
		if (vqm_txn_begin(&vqm.proto, &f, vqm_now_ms() + vqm.timeout_ms, &modem_txn) != 0) {
			struct vqm_record *answer;
			uint8_t out[128];
			size_t n = vqm_error(out, sizeof(out), &f, 1, 3);

			answer = vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_MODEM_TO_AP,
			    f.service, f.client, 0x02, f.transaction, f.message, VAYU_QMUX_F_LOCAL,
			    0, hdr->vm_cookie, n);
			if (answer != NULL && n != 0) {
				memcpy(answer->data + sizeof(struct vayu_qmux_hdr), out, n);
				vqm_record_shorten(answer, n);
				vqm_queue_put(&vqm.rx, answer, true);
				wakeup(&vqm.rx);
				selwakeup(&vqm.rsel);
			} else {
				vqm_record_free(answer);
			}
			vqm_status(VAYU_QMUX_F_LOCAL, ENOBUFS, f.service, f.client, hdr->vm_cookie);
			return;
		}
		tracked = true;
		vqm_set_transaction(payload + f.hdr_len, modem_txn);
		hdr->vm_transaction = modem_txn;
	}

	from = vqm.per_client ? vqm_cport_port_locked(f.service, f.client, &stale) : vqm.data_port;
	lck_mtx_unlock(&vqm.lock);
	if (stale != NULL) {
		vqm.ops->close(stale);
	}
	error = vqm.ops->send(from, node, port, payload + f.hdr_len, payload_len - f.hdr_len);
	lck_mtx_lock(&vqm.lock);

	if (error != 0) {
		struct vqm_record *answer;
		uint8_t out[128];
		size_t n = vqm_error(out, sizeof(out), &f, 1, 3);
		uint16_t ap;
		uint8_t client, style, mode, ctl;

		vqm_say("vayu-qmux: send to service 0x%02x(%s) node %u port %u: %d\n", f.service,
		    vqm_service_name(f.service), node, port, error);
		if (tracked) {
			(void)vqm_txn_complete(&vqm.proto, f.service, modem_txn, &ap, &client, &style,
			    &mode, &ctl);
		}
		answer = vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_MODEM_TO_AP, f.service,
		    f.client, 0x02, f.transaction, f.message, VAYU_QMUX_F_LOCAL, 0, hdr->vm_cookie,
		    n);
		if (answer != NULL && n != 0) {
			memcpy(answer->data + sizeof(struct vayu_qmux_hdr), out, n);
			vqm_record_shorten(answer, n);
			vqm_queue_put(&vqm.rx, answer, true);
			wakeup(&vqm.rx);
			selwakeup(&vqm.rsel);
		} else {
			vqm_record_free(answer);
		}
		vqm_status(VAYU_QMUX_F_LOCAL, VAYU_QMUX_ST_NOT_READY, f.service, f.client,
		    hdr->vm_cookie);
		if (error == EINVAL || error == ENXIO) {
			/* The router went away: the port number is gone with it.  The close is
			 * done by the thread outside our lock (a router callback may need it). */
			vqm.reopen = true;
		}
	} else {
		vqm.proto.counters.forwarded++;
	}
}

/* Answers requests the modem did not answer in vqm.timeout_ms. Called with the lock held. */
static void
vqm_expire_locked(void)
{
	uint8_t service, client, style, mode, ctl;
	uint16_t ap, message;

	while (vqm_txn_expire(&vqm.proto, vqm_now_ms(), &service, &ap, &message, &client, &style,
	    &mode, &ctl) == 1) {
		struct vqm_frame request;
		uint8_t out[128];
		size_t n;
		struct vqm_record *rec;

		memset(&request, 0, sizeof(request));
		request.style = style;
		request.len_mode = mode;
		request.ctl_flags = ctl;
		request.service = service;
		request.client = client;
		request.transaction = ap;
		request.message = message;
		n = vqm_reply(out, sizeof(out), &request, 1, 3, NULL, 0); /* failure, INTERNAL */
		if (n == 0) {
			continue;
		}
		rec = vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_MODEM_TO_AP, service, client,
		    0x02, ap, message, VAYU_QMUX_F_LOCAL | VAYU_QMUX_F_TIMEOUT, ETIMEDOUT, 0, n);
		if (rec == NULL) {
			continue;
		}
		memcpy(rec->data + sizeof(struct vayu_qmux_hdr), out, n);
		vqm_record_shorten(rec, n);
		vqm_queue_put(&vqm.rx, rec, true);
		vqm.proto.counters.timeouts++;
		vqm_say("vayu-qmux: service 0x%02x(%s) did not answer msg 0x%04x in %u ms; "
		    "answered locally\n", service, vqm_service_name(service), message,
		    vqm.timeout_ms);
		wakeup(&vqm.rx);
		selwakeup(&vqm.rsel);
	}
}

static void
vqm_thread(void *argument __unused, wait_result_t wait __unused)
{
	struct timespec step = { .tv_sec = 0, .tv_nsec = 100 * NSEC_PER_MSEC };

	for (;;) {
		struct vqm_record *rec;

		lck_mtx_lock(&vqm.lock);
		if (vqm.stop) {
			lck_mtx_unlock(&vqm.lock);
			break;
		}
		if (vqm.data_port == NULL || !vqm.lookups_done) {
			if (vqm_ports_open_locked() != 0) {
				lck_mtx_unlock(&vqm.lock);
				delay_for_interval(200, NSEC_PER_MSEC);
				continue;
			}
		}
		if (!vqm.settled) {
			/* The modem announces DMS/NAS/UIM/WMS about 1.5 s after GLINK (run 1050);
			 * a request sent in that window would be answered INTERNAL and CommCenter
			 * might not retry it. Let the first frames queue for two seconds, then go. */
			lck_mtx_unlock(&vqm.lock);
			for (unsigned i = 0; i < 20 && !vqm.stop; i++) {
				delay_for_interval(100, NSEC_PER_MSEC);
			}
			lck_mtx_lock(&vqm.lock);
			vqm.settled = true;
			unsigned present = 0;
			for (size_t i = 0; i < VQM_SERVICES; i++) {
				present += vqm.proto.services[i].present ? 1 : 0;
			}
			vqm_say("vayu-qmux: 2 s settle done: %u service(s) announced; %u frame(s) queued\n",
			    present, vqm.tx.count);
		}
		/* (a device close or open stops the queue until the old clients' ports are gone) */
		while (!(vqm.per_client && vqm.cports_flush) && (rec = vqm_queue_get(&vqm.tx)) != NULL) {
			vqm_tx_one_locked(rec);
			vqm_record_free(rec);
			if (vqm.stop || vqm.reopen) {
				break;
			}
		}
		vqm_expire_locked();
		bool reopen = vqm.reopen;
		bool per_client = vqm.per_client;
		vqm.reopen = false;
		lck_mtx_unlock(&vqm.lock);
		if (per_client) {
			vqm_cports_reap(reopen);
		}
		if (reopen) {
			vqm_ports_close();
			delay_for_interval(200, NSEC_PER_MSEC);
		} else {
			lck_mtx_lock(&vqm.lock);
			if (!(vqm.per_client && vqm.cports_flush)) {
				(void)msleep(&vqm.tx, &vqm.lock, PRIBIO, "vayu-qmux-tx", &step);
			}
			lck_mtx_unlock(&vqm.lock);
		}
	}
	vqm.running = false;
	thread_terminate(current_thread());
}

/* ---- the character device ----------------------------------------------------------------- */

static int
vqm_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	int error = 0;

	lck_mtx_lock(&vqm.lock);
	if (!vqm.enabled) {
		error = ENXIO;
	} else if (vqm.open) {
		error = EBUSY;
	} else {
		vqm.open = true;
		vqm_local_release_all_locked();
		vqm_clients_clear(&vqm.proto);
		vqm.cports_flush = vqm.per_client;
		wakeup(&vqm.tx);
		vqm_queue_clear(&vqm.rx);
		vqm_queue_clear(&vqm.tx);
		vqm.writes = vqm.reads_taken = vqm.queue_drops = 0;
		vqm_say("vayu-qmux: /dev/%s is open\n", VQM_DEVICE);
	}
	lck_mtx_unlock(&vqm.lock);
	return error;
}

static int
vqm_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(&vqm.lock);
	vqm.open = false;
	vqm_local_release_all_locked();
	vqm_clients_clear(&vqm.proto);
	vqm.cports_flush = vqm.per_client;
	vqm_queue_clear(&vqm.rx);
	vqm_queue_clear(&vqm.tx);
	wakeup(&vqm.rx);
	wakeup(&vqm.tx);
	selwakeup(&vqm.rsel);
	lck_mtx_unlock(&vqm.lock);
	vqm_say("vayu-qmux: /dev/%s is closed; %u write(s), %u read(s), %u dropped record(s)\n",
	    VQM_DEVICE, vqm.writes, vqm.reads_taken, vqm.queue_drops);
	return 0;
}

static int
vqm_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	struct vqm_record *rec;
	struct timespec step = { .tv_sec = 1, .tv_nsec = 0 };
	int error = 0;

	lck_mtx_lock(&vqm.lock);
	for (;;) {
		rec = vqm.rx.head;
		if (rec != NULL) {
			break;
		}
		if (!vqm.open) {
			lck_mtx_unlock(&vqm.lock);
			return ENXIO;
		}
		if ((ioflag & IO_NDELAY) != 0) {
			lck_mtx_unlock(&vqm.lock);
			return EAGAIN;
		}
		if (msleep(&vqm.rx, &vqm.lock, PRIBIO, "vayu-qmux-rx", &step) == EWOULDBLOCK) {
			continue;
		}
	}
	if (uio_resid(uio) < (user_ssize_t)rec->len) {
		lck_mtx_unlock(&vqm.lock);
		return EMSGSIZE;                /* the record stays for the next read */
	}
	rec = vqm_queue_get(&vqm.rx);
	vqm.reads_taken++;
	lck_mtx_unlock(&vqm.lock);

	error = uiomove((caddr_t)rec->data, rec->len, uio);
	vqm_record_free(rec);
	return error;
}

static int
vqm_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct vayu_qmux_hdr hdr;
	uint8_t *payload;
	uint64_t cookie;
	int error = 0;

	if (uio_resid(uio) < (user_ssize_t)sizeof(hdr)) {
		return EINVAL;
	}
	error = uiomove((caddr_t)&hdr, sizeof(hdr), uio);
	if (error != 0) {
		return error;
	}
	if (hdr.vm_magic != VAYU_QMUX_MAGIC || hdr.vm_version != VAYU_QMUX_VERSION ||
	    hdr.vm_kind != VAYU_QMUX_KIND_FRAME || hdr.vm_direction != VAYU_QMUX_AP_TO_MODEM ||
	    hdr.vm_length > VAYU_QMUX_MAX_PAYLOAD ||
	    uio_resid(uio) != (user_ssize_t)hdr.vm_length) {
		return EINVAL;
	}
	payload = kalloc_data(hdr.vm_length == 0 ? 1 : hdr.vm_length, Z_WAITOK);
	if (payload == NULL) {
		return ENOMEM;
	}
	if (hdr.vm_length != 0) {
		error = uiomove((caddr_t)payload, hdr.vm_length, uio);
		if (error != 0) {
			kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
			return error;
		}
	}
	cookie = hdr.vm_cookie;

	lck_mtx_lock(&vqm.lock);
	if (!vqm.open) {
		lck_mtx_unlock(&vqm.lock);
		kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
		return ENXIO;
	}
	vqm.writes++;

	struct vqm_frame f;
	if (vqm_parse(payload, hdr.vm_length, &f) != 0) {
		vqm.proto.counters.unparsed++;
		vqm_status(VAYU_QMUX_F_UNPARSED, EINVAL, 0xff, 0xff, cookie);
		lck_mtx_unlock(&vqm.lock);
		kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
		return 0;
	}
	vqm.proto.counters.frames_ap++;
	if (vqm.trace) {
		vqm_log_frame("ap", &f, 0);
	}

	uint8_t answer[512];
	size_t answer_len = 0;
	int handled = vqm_control(&vqm.proto, &f, answer, sizeof(answer), &answer_len);

	if (handled == 1) {
		uint8_t released_service, released_client;

		if (vqm_control_release_target(&f, &released_service, &released_client) == 1) {
			if (released_service == VQM_SERVICE_AUDIO && vqm.proto.local_e1) {
				vayu_voice_bb_release(released_service, released_client);
			}
			if (vqm.per_client) {
				wakeup(&vqm.tx);        /* (the thread closes the client's port) */
			}
		}
		struct vqm_record *rec = vqm_record_new(VAYU_QMUX_KIND_FRAME,
		    VAYU_QMUX_MODEM_TO_AP, f.service, f.client, 0x02, f.transaction, f.message,
		    VAYU_QMUX_F_CONTROL | VAYU_QMUX_F_LOCAL, 0, cookie, answer_len);

		if (rec != NULL) {
			memcpy(rec->data + sizeof(struct vayu_qmux_hdr), answer, answer_len);
			vqm_record_shorten(rec, answer_len);
			vqm_queue_put(&vqm.rx, rec, true);
			wakeup(&vqm.rx);
			selwakeup(&vqm.rsel);
		}
		lck_mtx_unlock(&vqm.lock);
		kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
		return 0;
	}
	if (handled < 0) {
		vqm_status(VAYU_QMUX_F_CONTROL, (uint32_t)-handled, f.service, f.client, cookie);
		lck_mtx_unlock(&vqm.lock);
		kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
		return 0;
	}

	if (!vqm_allowed(&vqm.proto, f.service)) {
		struct vqm_record *rec;
		size_t n = vqm_error(answer, sizeof(answer), &f, 1, 94);  /* NOT_SUPPORTED */

		vqm.proto.counters.denied++;
		vqm_say("vayu-qmux: service 0x%02x(%s) is not whitelisted; answered NOT_SUPPORTED\n",
		    f.service, vqm_service_name(f.service));
		rec = vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_MODEM_TO_AP, f.service,
		    f.client, 0x02, f.transaction, f.message,
		    VAYU_QMUX_F_LOCAL | VAYU_QMUX_F_DENIED, 0, cookie, n);
		if (rec != NULL && n != 0) {
			memcpy(rec->data + sizeof(struct vayu_qmux_hdr), answer, n);
			vqm_record_shorten(rec, n);
			vqm_queue_put(&vqm.rx, rec, true);
			wakeup(&vqm.rx);
			selwakeup(&vqm.rsel);
		} else {
			vqm_record_free(rec);
		}
		lck_mtx_unlock(&vqm.lock);
		kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
		return 0;
	}

	if (vqm_local(&vqm.proto, f.service)) {
		vqm_local_answer_locked(&f, cookie);
		lck_mtx_unlock(&vqm.lock);
		kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
		return 0;
	}

#if VQM_SMS_OBSERVE
	if (f.service == QMI_SERVICE_WMS) {
		vayu_sms_wms_observe(0, f.qmi, (size_t)f.qmi_len + 7);
	}
#endif
	(void)vqm_client_ensure(&vqm.proto, f.service, f.client, f.style, f.len_mode, f.ctl_flags);

	struct vqm_record *rec = vqm_record_new(VAYU_QMUX_KIND_FRAME, VAYU_QMUX_AP_TO_MODEM,
	    f.service, f.client, f.qmi_type, f.transaction, f.message,
	    vqm_redacted(f.service) ? VAYU_QMUX_F_REDACTED : 0, 0, cookie, hdr.vm_length);
	if (rec == NULL) {
		lck_mtx_unlock(&vqm.lock);
		kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);
		return ENOMEM;
	}
	memcpy(rec->data + sizeof(struct vayu_qmux_hdr), payload, hdr.vm_length);
	lck_mtx_unlock(&vqm.lock);
	kfree_data(payload, hdr.vm_length == 0 ? 1 : hdr.vm_length);

	lck_mtx_lock(&vqm.lock);
	if (!vqm.open) {
		lck_mtx_unlock(&vqm.lock);
		vqm_record_free(rec);
		return ENXIO;
	}
	vqm_queue_put(&vqm.tx, rec, false);
	wakeup(&vqm.tx);
	lck_mtx_unlock(&vqm.lock);
	return 0;
}

static int
vqm_select(__unused dev_t dev, int rw, void *wql, proc_t p)
{
	int ready = 0;

	lck_mtx_lock(&vqm.lock);
	if ((rw & FREAD) != 0) {
		if (vqm.rx.head != NULL) {
			ready = 1;
		} else {
			selrecord(p, &vqm.rsel, wql);
		}
	}
	if ((rw & FWRITE) != 0) {
		ready = 1;                      /* the write queues, unless it is full */
	}
	lck_mtx_unlock(&vqm.lock);
	return ready;
}

static int
vqm_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flag,
    __unused struct proc *p)
{
	if (command != VAYU_QMUX_IOC_STATS) {
		return ENOTTY;
	}
	struct vayu_qmux_stats *stats = (struct vayu_qmux_stats *)data;
	uint32_t clients = 0, txns = 0;

	lck_mtx_lock(&vqm.lock);
	for (size_t i = 0; i < VQM_CLIENTS; i++) {
		clients += vqm.proto.clients[i].used ? 1 : 0;
	}
	for (size_t i = 0; i < VQM_TXNS; i++) {
		txns += vqm.proto.txns[i].used ? 1 : 0;
	}
	stats->writes = vqm.writes;
	stats->reads = vqm.reads_taken;
	stats->frames_ap = vqm.proto.counters.frames_ap;
	stats->frames_modem = vqm.proto.counters.replies + vqm.proto.counters.indications;
	stats->control = vqm.proto.counters.control;
	stats->forwarded = vqm.proto.counters.forwarded;
	stats->replies = vqm.proto.counters.replies;
	stats->indications = vqm.proto.counters.indications;
	stats->timeouts = vqm.proto.counters.timeouts;
	stats->denied = vqm.proto.counters.denied;
	stats->unparsed = vqm.proto.counters.unparsed;
	stats->not_ready = vqm.proto.counters.not_ready;
	stats->clients = clients;
	stats->txns = txns;
	lck_mtx_unlock(&vqm.lock);
	return 0;
}

static const struct cdevsw qmux_cdevsw = {
	.d_open = vqm_open, .d_close = vqm_close, .d_read = vqm_read, .d_write = vqm_write,
	.d_ioctl = vqm_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = vqm_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
vqm_make_node(void)
{
	int major = cdevsw_add(-1, &qmux_cdevsw);

	if (major < 0 ||
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, VQM_DEVICE_UID, GID_WHEEL, 0600,
	    VQM_DEVICE) == NULL) {
		vqm_say("vayu-qmux: /dev/%s cannot be made\n", VQM_DEVICE);
		return false;
	}
	vqm_say("vayu-qmux: /dev/%s (0600, uid %u) is made: record header %u bytes, payload at most %u\n",
	    VQM_DEVICE, VQM_DEVICE_UID, (unsigned int)sizeof(struct vayu_qmux_hdr), VAYU_QMUX_MAX_PAYLOAD);
	return true;
}

/* ---- start and stop ---------------------------------------------------------------------- */

void
vayu_qmux_start(struct vayu_qrtr *q)
{
	uint32_t enabled = 0, wds = 0, trace = 1, timeout_ms = 15000, per_client = 0, bsp = 0;
	thread_t thread;

	if (vqm_started) {
		return;
	}
	if (!PE_parse_boot_argn("vayu_qmux", &enabled, sizeof(enabled)) || enabled != 1) {
		return;                         /* the default: no node, no thread, no QRTR */
	}
	PE_parse_boot_argn("vayu_qmux_wds", &wds, sizeof(wds));
	PE_parse_boot_argn("vayu_qmux_trace", &trace, sizeof(trace));
	PE_parse_boot_argn("vayu_qmux_timeout_ms", &timeout_ms, sizeof(timeout_ms));
	PE_parse_boot_argn("vayu_qmux_per_client", &per_client, sizeof(per_client));
	PE_parse_boot_argn("vayu_qmux_bsp", &bsp, sizeof(bsp));
	if (timeout_ms < 1000) {
		timeout_ms = 1000;
	}
	if (timeout_ms > 60000) {
		timeout_ms = 60000;
	}

	memset(&vqm, 0, sizeof(vqm));
	vqm.lock_group = lck_grp_alloc_init("vayu-qmux", LCK_GRP_ATTR_NULL);
	if (vqm.lock_group == NULL) {
		return;
	}
	lck_mtx_init(&vqm.lock, vqm.lock_group, LCK_ATTR_NULL);
	vqm_state_init(&vqm.proto, wds == 1);
	{
		uint32_t bind = 0;

		PE_parse_boot_argn("vayu_qmux_wds_bind", &bind, sizeof(bind));
		vqm.wds_bind = wds == 1 && bind == 1;
		if (bind == 1 && wds != 1) {
			vqm_say("vayu-qmux: vayu_qmux_wds_bind=1 is ignored without vayu_qmux_wds=1\n");
		}
	}
	vqm.ops = &vayu_qrtr_ops;
	vqm.qrtr = q;
	vqm.enabled = true;
	vqm.trace = trace != 0;
	vqm.timeout_ms = timeout_ms;
	vqm.per_client = per_client == 1;
	vqm.proto.local_e1 = vayu_voice_bb_local(VQM_SERVICE_AUDIO);
	vqm.proto.local_bsp = bsp == 1;

	vqm_say("vayu-qmux: vayu_qmux=1 vayu_qmux_wds=%u vayu_qmux_trace=%u "
	    "vayu_qmux_timeout_ms=%u; control service 0x00 local, whitelist DMS/NAS/UIM/WMS/VOICE%s%s%s\n",
	    wds, trace, timeout_ms, wds == 1 ? "/WDS" : "", vqm.proto.local_e1 ? ", 0xE1 local" : "",
	    vqm.proto.local_bsp ? ", 0xE2 local" : "");
	if (vqm.per_client) {
		vqm_say("vayu-qmux: vayu_qmux_per_client=1: per-client QRTR ports: each QMux client gets a port of "
		    "its own on its first request (at most %u; past that the data port is shared)\n", VQM_CLIENTS);
	}
	if (q == NULL || q->glink == NULL || !q->glink->up) {
		vqm_say("vayu-qmux: the modem's GLINK is not up; the node will wait for it\n");
	}
	if (!vqm_make_node()) {
		vqm.enabled = false;
		return;
	}
	vqm.running = true;
	vqm.stop = false;
	vqm_started = true;
	if (kernel_thread_start(vqm_thread, &vqm, &thread) != KERN_SUCCESS) {
		vqm_say("vayu-qmux: no thread\n");
		vqm.enabled = false;
		vqm.running = false;
		vqm_started = false;
		return;
	}
	thread_deallocate(thread);
	if (vqm.proto.local_e1) {
		vayu_voice_bb_attach(vqm_bb_deliver);
		vqm_say("vayu-qmux: local 0xE1 attached: Apple's QMI Audio Service is answered by vayu-voice "
		    "(vayu_call_bb=1), its indications come back to the client that asked\n");
	}
	if (vqm.proto.local_bsp) {
		vqm_say("vayu-qmux: local 0xE2 (BSP) attached: SetAntennaPreference/SetAudioLoggingEnabled succeed, "
		    "the rest NOT_SUPPORTED\n");
	}
}

void
vayu_qmux_stop(void)
{
	if (!vqm_started) {
		return;
	}
	lck_mtx_lock(&vqm.lock);
	vqm.stop = true;
	wakeup(&vqm.tx);
	lck_mtx_unlock(&vqm.lock);
	for (uint32_t waited = 0; waited < VQM_STOP_MS && vqm.running; waited += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	if (vqm.proto.local_e1) {
		vayu_voice_bb_attach(NULL);
	}
	if (vqm.running) {
		vqm_say("vayu-qmux: the thread has not finished in %u ms; the ports are left to the "
		    "router's detach\n", VQM_STOP_MS);
	} else {
		if (vqm.per_client) {
			vqm_cports_reap(true);
		}
		lck_mtx_lock(&vqm.lock);
		vqm_ports_close();
		vqm_queue_clear(&vqm.tx);
		vqm_queue_clear(&vqm.rx);
		vqm.enabled = false;
		lck_mtx_unlock(&vqm.lock);
		vqm_started = false;
		vqm_say("vayu-qmux: done: %u write(s), %u read(s), %u forwarded, %u replies, "
		    "%u indications, %u timeout(s), %u denied, %u unparsed, %u dropped\n",
		    vqm.writes, vqm.reads_taken, vqm.proto.counters.forwarded,
		    vqm.proto.counters.replies, vqm.proto.counters.indications,
		    vqm.proto.counters.timeouts, vqm.proto.counters.denied,
		    vqm.proto.counters.unparsed, vqm.queue_drops);
		if (vqm.per_client || vqm.proto.local_e1 || vqm.proto.local_bsp) {
			vqm_say("vayu-qmux: client ports: %u opened, %u closed, %u shared the data port, %u stray "
			    "datagram(s); local: %u answer(s), %u indication(s)\n", vqm.cport_opens, vqm.cport_closes,
			    vqm.cport_fallbacks, vqm.cport_strays, vqm.local_answers, vqm.local_indications);
		}
	}
}
