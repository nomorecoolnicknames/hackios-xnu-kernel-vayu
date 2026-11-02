/*
 * SMS on vayu, the first stage: a QMI WMS client of the modem's service 0x05 over
 * QRTR, a small inbox in the kernel, and /dev/vayu-sms for a userland tool.
 *
 * What it asks, and why:
 *   WMS 0x05  Get Routes       0x0033   read-only; the table is logged
 *   WMS 0x05  Set Routes       0x0032   only under vayu_sms_routes=1, and only when
 *                                      Get Routes returned an empty table: class 0
 *                                      and 1, UIM storage, STORE_AND_NOTIFY
 *   WMS 0x05  Set Event Report 0x0001   new-MT-message indications on
 *   WMS 0x05  List Messages    0x0031   the stored messages, for the initial sweep
 *                                      and `sms refresh`
 *   WMS 0x05  Raw Read         0x0022   one stored message by storage/index
 *   WMS 0x05  Raw Send         0x0020   one SMS-SUBMIT PDU built here
 *   WMS 0x05  Delete           0x0024   one stored message the user asked to remove
 *   WMS 0x05  Send Ack         0x0037   only when a transfer route (TLV 0x11) says
 *                                      an acknowledgement is owed
 *
 * The event report and Raw Read carry a complete GSM 03.40 PDU; bsd/sys/
 * vayu_sms_qmi.h decodes SMS-DELIVER (GSM 7-bit with the extension table, UCS-2,
 * 8-bit) and builds SMS-SUBMIT (GSM 7-bit or UCS-2, one segment).  A UDH
 * concatenation header is reported, but the segments are stored one by one: no
 * reassembly queue exists in this stage.
 *
 * The userland interface is /dev/vayu-sms (0666): read() returns the inbox count
 * record, ioctl() COUNT/GET reads it message by message, SEND sends one short
 * message, DELETE removes one stored message, REFRESH sweeps one storage, STATUS
 * reports the counters.  bsd/sys/vayu_sms.h has the ABI.
 *
 * Privacy: the kernel log masks a sender to its last four digits and never prints
 * text, a PDU byte, SMSC, IMSI, ICCID or IMEI.  The inbox has the full number and
 * text because that is what the device is for.
 *
 * Boot-args: vayu_sms=0/1 (default 0), vayu_sms_wait=S (30), vayu_sms_routes=1,
 * vayu_sms_store=0|1 (UIM|NV), vayu_sms_refresh=S (60, 0 off), vayu_sms_scan=N
 * (20, the largest index Raw Read sweeps when List Messages is refused).
 *
 * Facts behind the numbers: service 0x05, the message ids and the TLV layouts are
 * libqmi's data/qmi-service-wms.json (master, read 2026-11-02); the enums are
 * src/libqmi-glib/qmi-enums-wms.h of the same.  The QMI coding and QRTR transport
 * are sys/hakios.qmi.h and sys/hakios.qrtr.h (the same ones vayu_cell.c uses).
 *
 * What is NOT known until it runs on the phone (HYPOTHESIS): that this modem's WMS
 * accepts List Messages with only a storage TLV; how many slots it reports; whether
 * its existing route table delivers class 1 through a store route or a transfer
 * route; and that its SIM has room for the message being received.  Each failure is
 * one log line and nothing is guessed in its place.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_sms.h>
#include <sys/vayu_sms_qmi.h>

extern void loader_log_putc(char c);

#define SMS_DEVICE              "vayu-sms"
#define SMS_HANDLE_MAX          4096    /* the largest datagram the QMI handle takes */
#define SMS_REPLY_MS            5000    /* one WMS request's reply */
#define SMS_SEND_REPLY_MS       15000   /* Raw Send waits for the network's answer */
#define SMS_STOP_MS             20000   /* one Raw Send may take 15 s; let it finish */
#define SMS_EVENT_QUEUE         4
#define SMS_NUMBER_TAIL         8

/* ---- state --------------------------------------------------------------------------------------- */

struct sms_event {
	uint8_t storage;
	uint32_t index;
	uint8_t tag;
	bool have_mt;
	bool have_route;
	uint8_t ack;
	uint32_t transaction;
	uint8_t format;
	uint32_t raw_length;
	uint8_t raw[VAYU_SMS_PDU_MAX];
};

enum sms_job_kind {
	SMS_JOB_NONE = 0,
	SMS_JOB_SEND,
	SMS_JOB_DELETE,
	SMS_JOB_REFRESH,
};

struct sms_job {
	uint32_t kind;
	char number[VAYU_SMS_NUMBER_MAX];
	uint32_t text_length;
	char text[VAYU_SMS_TEXT_MAX];
	uint32_t seq;
	uint32_t storage;
};

struct sms_state {
	struct qmi_handle qmi;
	bool qmi_up;
	volatile bool wms_there, stop, running, started;
	uint32_t node, port, version, instance, generation;
	uint32_t level, wait_s, routes_arg, store, refresh_s, scan_max;
	uint64_t next_refresh_ms, next_config_ms;
	/* the worker's inbox and the counters the device reports */
	struct vayu_sms_message inbox[VAYU_SMS_INBOX];
	uint32_t inbox_count, inbox_unread, next_seq;
	uint32_t state;
	bool event_report, routes_known, routes_written;
	uint32_t route_count;
	uint32_t received, sent, deleted, dropped;
	uint32_t requests, failures;
	uint32_t last_result, last_error, last_message, last_gsm_cause, last_failure;
	uint8_t mr;
	/* one control job at a time; the worker is its only executor */
	bool job_pending, job_busy, job_done;
	struct sms_job job, job_running;
	int job_result;
	uint32_t job_message_id, job_mr, job_pdu_length;
	/* the indications, copied off the QRTR thread */
	struct sms_event event[SMS_EVENT_QUEUE];
	uint32_t event_head, event_tail;
	bool work;
};

static struct sms_state sms;

static LCK_GRP_DECLARE(vayu_sms_lck_grp, "vayu-sms");
static LCK_MTX_DECLARE(vayu_sms_lock, &vayu_sms_lck_grp);

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

static uint64_t
now_ms(void)
{
	uint64_t uptime, nanoseconds;

	clock_get_uptime(&uptime);
	absolutetime_to_nanoseconds(uptime, &nanoseconds);
	return nanoseconds / NSEC_PER_MSEC;
}

/* ---- the QMI client ------------------------------------------------------------------------------ */

static int
sms_new_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (service->service != VAYU_SMS_QMI_WMS_SERVICE) {
		return -1;
	}
	lck_mtx_lock(&vayu_sms_lock);
	sms.node = service->node;
	sms.port = service->port;
	sms.version = service->version;
	sms.instance = service->instance;
	sms.wms_there = true;
	sms.generation++;
	if (sms.state < VAYU_SMS_STATE_CONFIGURING) {
		sms.state = VAYU_SMS_STATE_CONFIGURING;
	}
	sms.work = true;
	wakeup(&sms.work);
	lck_mtx_unlock(&vayu_sms_lock);
	return 0;
}

static void
sms_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	lck_mtx_lock(&vayu_sms_lock);
	if (service->service == VAYU_SMS_QMI_WMS_SERVICE && service->node == sms.node && service->port == sms.port) {
		sms.wms_there = false;
		sms.event_report = false;
		sms.routes_known = false;
		sms.state = VAYU_SMS_STATE_WAITING;
	}
	lck_mtx_unlock(&vayu_sms_lock);
}

static const struct qmi_ops sms_qmi_ops = {
	.new_server = sms_new_server,
	.del_server = sms_del_server,
};

/*
 * One request and its reply, waited `timeout` ms.  Called only from the worker
 * thread: an indication callback must never wait (its reply is delivered by the
 * same QRTR thread).  `quiet` suppresses the log and the failure counter for an
 * expected refusal (an empty Raw Read slot, for instance).
 */
static int
sms_request(const char *what, uint16_t message, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, struct vayu_sms_qmi_result *result, uint32_t timeout, bool quiet)
{
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = sms.node, .sq_port = sms.port };
	struct qmi_txn txn;
	int ret;

	if (sms.stop) {
		return -ECANCELED;
	}
	sms.requests++;
	ret = qmi_txn_init(&sms.qmi, &txn, resp_ei, resp);
	if (ret < 0) {
		sms.failures++;
		if (!quiet) {
			say("vayu-sms: %s: no transaction: %d\n", what, ret);
		}
		return ret;
	}
	ret = qmi_send_request(&sms.qmi, &to, &txn, message, VAYU_SMS_QMI_REQUEST_MAX, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		sms.failures++;
		if (!quiet) {
			say("vayu-sms: %s: cannot send: %d\n", what, ret);
		}
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, timeout);
	if (ret < 0) {
		sms.failures++;
		if (!quiet) {
			if (ret == -ETIMEDOUT) {
				say("vayu-sms: %s: no reply in %u ms\n", what, timeout);
			} else {
				say("vayu-sms: %s: the reply cannot be decoded (%d)\n", what, ret);
			}
		}
		return ret;
	}
	if (result != NULL) {
		if (!quiet) {
			sms.last_result = result->result;
			sms.last_error = result->error;
			sms.last_message = message;
		}
		if (result->result != QMI_RESULT_SUCCESS_V01 || result->error != QMI_ERR_NONE_V01) {
			if (!quiet) {
				sms.failures++;         /* an empty Raw Read slot is expected, not a failure */
			}
			if (!quiet) {
				say("vayu-sms: %s: the modem says result %u error %u\n", what,
				    (unsigned int)result->result, (unsigned int)result->error);
			}
			return -EIO;
		}
	}
	return 0;
}

/* ---- the log's view of a number ------------------------------------------------------------------ */

/* Only the last four digits: "...0986".  A short or alphanumeric sender is "...". */
static void
sms_mask_tail(const char *number, char out[SMS_NUMBER_TAIL])
{
	size_t length = strlen(number);
	size_t kept = 0;

	out[0] = '.'; out[1] = '.'; out[2] = '.';
	if (length >= 5) {
		for (size_t i = length; i > 0 && kept < 4; i--) {
			if (number[i - 1] < '0' || number[i - 1] > '9') {
				break;
			}
			kept++;
		}
	}
	for (size_t i = 0; i < kept; i++) {
		out[3 + i] = number[length - kept + i];
	}
	out[3 + kept] = 0;
}

/* ---- the inbox ----------------------------------------------------------------------------------- */

static struct vayu_sms_message *
sms_find_locked(uint8_t storage, uint32_t index)
{
	for (uint32_t i = 0; i < VAYU_SMS_INBOX; i++) {
		if (sms.inbox[i].seq != 0 && sms.inbox[i].storage == storage && sms.inbox[i].index == index) {
			return &sms.inbox[i];
		}
	}
	return NULL;
}

/* A free slot, or the oldest message's (the inbox is a 16-message window). */
static struct vayu_sms_message *
sms_slot_locked(void)
{
	struct vayu_sms_message *oldest = &sms.inbox[0];

	for (uint32_t i = 0; i < VAYU_SMS_INBOX; i++) {
		if (sms.inbox[i].seq == 0) {
			return &sms.inbox[i];
		}
		if (sms.inbox[i].seq < oldest->seq) {
			oldest = &sms.inbox[i];
		}
	}
	return oldest;
}

/*
 * Decode one deliver PDU and put it in the inbox.  A stored message (a real
 * storage and index) replaces the earlier copy of that slot, keeping its seq; a
 * transferred one (storage 0xff) always gets a new seq.  No text or number is
 * logged.
 */
static void
sms_store_pdu(uint8_t storage, uint32_t index, uint8_t tag, const uint8_t *pdu, uint32_t length)
{
	struct vayu_sms_deliver deliver;
	struct vayu_sms_message message, *slot;
	char tail[SMS_NUMBER_TAIL];
	int ret;

	ret = vayu_sms_pdu_decode_deliver(pdu, length, &deliver);
	if (ret != VAYU_SMS_OK) {
		sms.dropped++;
		say("vayu-sms: one PDU from storage %u index %u is not decoded: %d; dropped\n",
		    (unsigned int)storage, (unsigned int)index, ret);
		return;
	}
	memset(&message, 0, sizeof(message));
	message.storage = storage;
	message.index = index;
	message.tag = tag;
	message.alphabet = deliver.alphabet;
	if (deliver.originator.alphanumeric) {
		message.flags |= VAYU_SMS_F_ALPHANUMERIC;
	}
	if (deliver.concatenated) {
		message.flags |= VAYU_SMS_F_MULTIPART;
	}
	if (deliver.alphabet == VAYU_SMS_ALPHABET_UCS2) {
		message.flags |= VAYU_SMS_F_UCS2;
	} else if (deliver.alphabet == VAYU_SMS_ALPHABET_8BIT) {
		message.flags |= VAYU_SMS_F_8BIT;
	}
	message.number_length = deliver.originator.length;
	if (message.number_length >= sizeof(message.number)) {
		message.number_length = sizeof(message.number) - 1;
	}
	memcpy(message.number, deliver.originator.text, message.number_length);
	message.number[message.number_length] = 0;
	memcpy(message.timestamp, deliver.timestamp, sizeof(message.timestamp));
	message.timestamp[sizeof(message.timestamp) - 1] = 0;
	message.text_length = deliver.text_length;
	if (message.text_length >= sizeof(message.text)) {
		message.text_length = sizeof(message.text) - 1;
	}
	memcpy(message.text, deliver.text, message.text_length);
	message.text[message.text_length] = 0;
	message.received_ms = now_ms();

	lck_mtx_lock(&vayu_sms_lock);
	slot = storage != VAYU_SMS_QMI_STORAGE_NONE ? sms_find_locked(storage, index) : NULL;
	if (slot != NULL && slot->text_length == message.text_length &&
	    slot->number_length == message.number_length &&
	    memcmp(slot->text, message.text, message.text_length) == 0 &&
	    memcmp(slot->number, message.number, message.number_length) == 0 &&
	    memcmp(slot->timestamp, message.timestamp, sizeof(message.timestamp)) == 0) {
		/* the periodic sweep read the same slot again: not a new message */
		if (slot->tag == VAYU_SMS_QMI_TAG_MT_NOT_READ && message.tag != VAYU_SMS_QMI_TAG_MT_NOT_READ) {
			sms.inbox_unread--;
		} else if (slot->tag != VAYU_SMS_QMI_TAG_MT_NOT_READ && message.tag == VAYU_SMS_QMI_TAG_MT_NOT_READ) {
			sms.inbox_unread++;
		}
		slot->received_ms = message.received_ms;
		slot->tag = message.tag;
		lck_mtx_unlock(&vayu_sms_lock);
		return;
	}
	if (slot != NULL) {
		if (slot->tag == VAYU_SMS_QMI_TAG_MT_NOT_READ) {
			sms.inbox_unread--;
		}
		message.seq = slot->seq;
		*slot = message;
	} else {
		slot = sms_slot_locked();
		if (slot->seq != 0) {
			sms.inbox_count--;
			if (slot->tag == VAYU_SMS_QMI_TAG_MT_NOT_READ) {
				sms.inbox_unread--;
			}
		}
		message.seq = sms.next_seq++;
		*slot = message;
		sms.inbox_count++;
	}
	if (message.tag == VAYU_SMS_QMI_TAG_MT_NOT_READ) {
		sms.inbox_unread++;
	}
	sms.received++;
	lck_mtx_unlock(&vayu_sms_lock);

	if (message.flags & VAYU_SMS_F_ALPHANUMERIC) {
		say("vayu-sms: message %u from an alphanumeric sender, %u bytes, %s, at %s%s\n",
		    (unsigned int)message.seq, (unsigned int)message.text_length,
		    message.alphabet == VAYU_SMS_ALPHABET_UCS2 ? "UCS-2" : "GSM 7-bit",
		    message.timestamp[0] != 0 ? message.timestamp : "(no time)",
		    (message.flags & VAYU_SMS_F_MULTIPART) ? ", multipart segment" : "");
	} else {
		sms_mask_tail(message.number, tail);
		say("vayu-sms: message %u from %s, %u bytes, %s, at %s%s\n",
		    (unsigned int)message.seq, tail, (unsigned int)message.text_length,
		    message.alphabet == VAYU_SMS_ALPHABET_UCS2 ? "UCS-2" :
		    message.alphabet == VAYU_SMS_ALPHABET_8BIT ? "8-bit" : "GSM 7-bit",
		    message.timestamp[0] != 0 ? message.timestamp : "(no time)",
		    (message.flags & VAYU_SMS_F_MULTIPART) ? ", multipart segment" : "");
	}
}

/* ---- WMS conversations (worker thread only) ------------------------------------------------------- */

static int
sms_raw_read(uint8_t storage, uint32_t index, struct vayu_sms_qmi_raw_read_resp *response, bool quiet)
{
	struct vayu_sms_qmi_raw_read_req request;

	memset(&request, 0, sizeof(request));
	request.id.storage = storage;
	request.id.index = index;
	request.mode_valid = 1;
	request.mode = VAYU_SMS_QMI_MODE_GSM;
	memset(response, 0, sizeof(*response));
	return sms_request("raw read", VAYU_SMS_QMI_RAW_READ, vayu_sms_qmi_raw_read_req_ei, &request,
	    vayu_sms_qmi_raw_read_resp_ei, response, &response->result, quiet ? 2000 : SMS_REPLY_MS, quiet);
}

/* One stored message: read it and decode it.  Returns 1 when a message was stored. */
static int
sms_read_index(uint8_t storage, uint32_t index, bool quiet)
{
	struct vayu_sms_qmi_raw_read_resp response;
	int ret;

	ret = sms_raw_read(storage, index, &response, quiet);
	if (ret != 0 || !response.raw_valid) {
		return ret == -ETIMEDOUT ? -ETIMEDOUT : 0;
	}
	if (response.raw.format != VAYU_SMS_QMI_FORMAT_GSM_P2P) {
		if (!quiet) {
			say("vayu-sms: storage %u index %u holds format %u, not a GSM point-to-point message; skipped\n",
			    (unsigned int)storage, (unsigned int)index, (unsigned int)response.raw.format);
		}
		return 0;
	}
	if (response.raw.tag != VAYU_SMS_QMI_TAG_MT_READ && response.raw.tag != VAYU_SMS_QMI_TAG_MT_NOT_READ) {
		return 0;                       /* an MO message: not the inbox's business */
	}
	if (response.raw.raw_length == 0 || response.raw.raw_length > VAYU_SMS_PDU_MAX) {
		sms.dropped++;
		return 0;
	}
	sms_store_pdu(storage, index, response.raw.tag, response.raw.raw, response.raw.raw_length);
	return 1;
}

/*
 * List Messages, then Raw Read every MT entry.  When the modem refuses List
 * (some firmware does without a tag), sweep the first vayu_sms_scan indices
 * instead.  The List reply's count prefix is 32 bits, so its TLV was captured
 * opaque by sys/vayu_sms_qmi.h and is parsed here.
 */
static uint32_t
sms_refresh_store(uint8_t storage, bool quiet)
{
	struct vayu_sms_qmi_list_req request = { .storage = storage };
	struct vayu_sms_qmi_list_resp response;
	struct vayu_sms_qmi_message_list list;
	uint32_t found = 0;
	int ret;

	memset(&response, 0, sizeof(response));
	ret = sms_request("list messages", VAYU_SMS_QMI_LIST_MESSAGES, vayu_sms_qmi_list_req_ei, &request,
	    vayu_sms_qmi_list_resp_ei, &response, &response.result, SMS_REPLY_MS, quiet);
	if (ret == 0 && response.list_valid) {
		uint32_t count = vayu_sms_le32((const uint8_t *)response.list);
		size_t payload = 4u + (size_t)count * 5u;

		if (payload > sizeof(response.list) ||
		    vayu_sms_qmi_list_parse(response.list, payload, &list) != VAYU_SMS_OK) {
			say("vayu-sms: list messages: the list TLV does not parse; nothing read from it\n");
		} else {
			if (!quiet) {
				say("vayu-sms: storage %u lists %u message(s)\n", (unsigned int)storage,
				    (unsigned int)list.count);
			}
			for (uint32_t i = 0; i < list.count; i++) {
				if (sms.stop) {
					break;
				}
				if (list.entry[i].tag != VAYU_SMS_QMI_TAG_MT_READ &&
				    list.entry[i].tag != VAYU_SMS_QMI_TAG_MT_NOT_READ) {
					continue;
				}
				if (sms_read_index(storage, list.entry[i].index, true) > 0) {
					found++;
				}
			}
			return found;
		}
	} else if (!quiet) {
		say("vayu-sms: list messages is not answered; sweeping the first %u indices of storage %u\n",
		    (unsigned int)sms.scan_max, (unsigned int)storage);
	}
	{
		uint32_t timeouts = 0;

		for (uint32_t index = 0; index < sms.scan_max && !sms.stop; index++) {
			int result = sms_read_index(storage, index, true);

			if (result == -ETIMEDOUT) {
				if (++timeouts >= 3) {
					say("vayu-sms: storage %u: three Raw Reads in a row timed out at index %u; sweep stops\n",
					    (unsigned int)storage, (unsigned int)index);
					break;
				}
			} else {
				timeouts = 0;
			}
			if (result > 0) {
				found++;
			}
		}
	}
	if (!quiet) {
		say("vayu-sms: storage %u: %u message(s) read\n", (unsigned int)storage, (unsigned int)found);
	}
	return found;
}

static const char *
sms_action_name(uint8_t action)
{
	switch (action) {
	case VAYU_SMS_QMI_RECEIPT_DISCARD: return "discard";
	case VAYU_SMS_QMI_RECEIPT_STORE: return "store-and-notify";
	case VAYU_SMS_QMI_RECEIPT_TRANSFER: return "transfer-only";
	case VAYU_SMS_QMI_RECEIPT_TRANSFER_ACK: return "transfer-and-ack";
	default: return "?";
	}
}

/* Get Routes; install a store route only when the table is empty and explicitly allowed. */
static void
sms_configure_routes(void)
{
	struct vayu_sms_qmi_get_routes_resp routes;
	int ret;

	memset(&routes, 0, sizeof(routes));
	ret = sms_request("get routes", VAYU_SMS_QMI_GET_ROUTES, NULL, NULL, vayu_sms_qmi_get_routes_resp_ei,
	    &routes, &routes.result, SMS_REPLY_MS, false);
	if (ret != 0 || !routes.routes_valid) {
		say("vayu-sms: get routes gave no table; incoming messages depend on the modem's own table\n");
		return;
	}
	sms.routes_known = true;
	sms.route_count = routes.routes.count;
	say("vayu-sms: %u route(s)%s\n", (unsigned int)routes.routes.count,
	    routes.routes.count == 0 ? " (empty)" : "");
	for (uint32_t i = 0; i < routes.routes.count && i < VAYU_SMS_ROUTE_MAX; i++) {
		say("vayu-sms: route %u: type %u, class %u, storage %u, action %s\n", (unsigned int)i + 1,
		    (unsigned int)routes.routes.route[i].message_type,
		    (unsigned int)routes.routes.route[i].message_class,
		    (unsigned int)routes.routes.route[i].storage,
		    sms_action_name(routes.routes.route[i].receipt_action));
	}
	if (routes.routes.count != 0) {
		return;                         /* never rewrite a table that exists */
	}
	if (sms.routes_arg != 1) {
		say("vayu-sms: the route table is empty and vayu_sms_routes=0: Set Routes is not sent\n");
		return;
	}
	{
		struct vayu_sms_qmi_set_routes_req request;
		struct vayu_sms_qmi_result_resp response;

		memset(&request, 0, sizeof(request));
		request.routes.count = 2;
		request.routes.route[0].message_type = VAYU_SMS_QMI_TYPE_P2P;
		request.routes.route[0].message_class = VAYU_SMS_QMI_CLASS_0;
		request.routes.route[0].storage = VAYU_SMS_QMI_STORAGE_UIM;
		request.routes.route[0].receipt_action = VAYU_SMS_QMI_RECEIPT_STORE;
		request.routes.route[1] = request.routes.route[0];
		request.routes.route[1].message_class = VAYU_SMS_QMI_CLASS_1;
		memset(&response, 0, sizeof(response));
		say("vayu-sms: vayu_sms_routes=1: Set Routes class 0 and 1, UIM, store-and-notify\n");
		ret = sms_request("set routes", VAYU_SMS_QMI_SET_ROUTES, vayu_sms_qmi_set_routes_req_ei, &request,
		    vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, false);
		if (ret != 0) {
			say("vayu-sms: set routes was refused; incoming messages depend on the modem's table\n");
			return;
		}
		sms.routes_written = true;
		/* verify what the modem now says, without trusting the write's echo */
		memset(&routes, 0, sizeof(routes));
		ret = sms_request("get routes after set", VAYU_SMS_QMI_GET_ROUTES, NULL, NULL,
		    vayu_sms_qmi_get_routes_resp_ei, &routes, &routes.result, SMS_REPLY_MS, false);
		if (ret == 0 && routes.routes_valid) {
			sms.route_count = routes.routes.count;
			say("vayu-sms: set routes: %u route(s) now\n", (unsigned int)routes.routes.count);
		}
	}
}

static void
sms_configure_events(void)
{
	struct vayu_sms_qmi_set_event_req request = { .report = 1 };
	struct vayu_sms_qmi_result_resp response;
	int ret;

	memset(&response, 0, sizeof(response));
	ret = sms_request("set event report", VAYU_SMS_QMI_SET_EVENT_REPORT, vayu_sms_qmi_set_event_report_req_ei,
	    &request, vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, false);
	lck_mtx_lock(&vayu_sms_lock);
	sms.event_report = ret == 0;
	sms.state = ret == 0 ? VAYU_SMS_STATE_READY : VAYU_SMS_STATE_FAILED;
	lck_mtx_unlock(&vayu_sms_lock);
	say("vayu-sms: new-MT-message event report: %s\n", ret == 0 ? "accepted" : "refused");
}

/* ---- indications --------------------------------------------------------------------------------- */

static void
sms_event_indication(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused, struct qmi_txn *txn __unused,
    const void *decoded)
{
	const struct vayu_sms_qmi_event_report *report = decoded;
	struct sms_event *event;
	uint32_t next;

	if (!report->mt_valid && !report->route_valid) {
		return;
	}
	lck_mtx_lock(&vayu_sms_lock);
	if (sms.stop || !sms.wms_there) {
		lck_mtx_unlock(&vayu_sms_lock);
		return;
	}
	next = (sms.event_head + 1) % SMS_EVENT_QUEUE;
	if (next == sms.event_tail) {
		sms.dropped++;
		lck_mtx_unlock(&vayu_sms_lock);
		return;
	}
	event = &sms.event[sms.event_head];
	memset(event, 0, sizeof(*event));
	event->storage = VAYU_SMS_QMI_STORAGE_NONE;
	if (report->mt_valid) {
		event->have_mt = true;
		event->storage = report->mt.storage;
		event->index = report->mt.index;
	}
	if (report->route_valid) {
		event->have_route = true;
		event->ack = report->route.ack;
		event->transaction = report->route.transaction;
		event->format = report->route.format;
		event->raw_length = report->route.raw_length;
		if (event->raw_length > sizeof(event->raw)) {
			event->raw_length = sizeof(event->raw);
		}
		memcpy(event->raw, report->route.raw, event->raw_length);
	}
	event->tag = VAYU_SMS_QMI_TAG_MT_NOT_READ;
	sms.event_head = next;
	sms.work = true;
	wakeup(&sms.work);
	lck_mtx_unlock(&vayu_sms_lock);
}

static const struct qmi_msg_handler sms_handlers[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_SMS_QMI_EVENT_REPORT,
		.ei = vayu_sms_qmi_event_report_ei,
		.decoded_size = sizeof(struct vayu_sms_qmi_event_report),
		.fn = sms_event_indication,
	},
	{ .fn = NULL },
};

/* ---- sending and deleting (worker thread) --------------------------------------------------------- */

static void
sms_send_ack(uint32_t transaction, bool success)
{
	struct vayu_sms_qmi_send_ack_req request;
	struct vayu_sms_qmi_result_resp response;

	memset(&request, 0, sizeof(request));
	request.info.transaction = transaction;
	request.info.protocol = 1;              /* QMI_WMS_MESSAGE_PROTOCOL_WCDMA */
	request.info.success = success ? 1 : 0;
	memset(&response, 0, sizeof(response));
	(void)sms_request("send ack", VAYU_SMS_QMI_SEND_ACK, vayu_sms_qmi_send_ack_req_ei, &request,
	    vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, true);
}

static int
sms_do_send(const char *number, const char *text, uint32_t text_length, uint32_t *message_id, uint32_t *reference,
    uint32_t *pdu_length)
{
	uint8_t pdu[VAYU_SMS_PDU_MAX];
	struct vayu_sms_qmi_raw_send_req request;
	struct vayu_sms_qmi_raw_send_resp response;
	size_t length = 0;
	uint32_t dcs = 0;
	uint8_t mr = sms.mr++;
	int ret;

	ret = vayu_sms_pdu_encode_submit(number, text, text_length, mr, pdu, sizeof(pdu), &length, &dcs);
	if (ret != VAYU_SMS_OK) {
		say("vayu-sms: send: the PDU encoder refused the message (%d) for %u bytes of %s text\n", ret,
		    (unsigned int)text_length, dcs == 0x08 ? "UCS-2" : "GSM 7-bit");
		return ret == VAYU_SMS_ERR_TOO_LONG ? -EMSGSIZE : -EINVAL;
	}
	if (vayu_sms_qmi_raw_send_fill(&request, pdu, length) != VAYU_SMS_OK) {
		return -EMSGSIZE;
	}
	memset(&response, 0, sizeof(response));
	ret = sms_request("raw send", VAYU_SMS_QMI_RAW_SEND, vayu_sms_qmi_raw_send_req_ei, &request,
	    vayu_sms_qmi_raw_send_resp_ei, &response, &response.result, SMS_SEND_REPLY_MS, false);
	if (ret != 0) {
		if (response.cause_valid) {
			sms.last_gsm_cause = response.cause.rp_cause;
			say("vayu-sms: raw send refused: GSM cause %u, TP cause %u\n",
			    (unsigned int)response.cause.rp_cause, (unsigned int)response.cause.tp_cause);
		}
		return -EIO;
	}
	*message_id = response.message_id_valid ? response.message_id : 0;
	*reference = mr;
	*pdu_length = (uint32_t)length;
	sms.sent++;
	say("vayu-sms: one message sent: PDU %u bytes, %s, message id %u, MR %u\n", (unsigned int)length,
	    dcs == 0x08 ? "UCS-2" : "GSM 7-bit", (unsigned int)*message_id, (unsigned int)mr);
	return 0;
}

static int
sms_do_delete(uint32_t seq)
{
	struct vayu_sms_qmi_delete_req request;
	struct vayu_sms_qmi_result_resp response;
	struct vayu_sms_message *message = NULL;
	uint32_t slot = 0;
	int ret;

	lck_mtx_lock(&vayu_sms_lock);
	for (uint32_t i = 0; i < VAYU_SMS_INBOX; i++) {
		if (sms.inbox[i].seq == seq) {
			message = &sms.inbox[i];
			slot = i;
			break;
		}
	}
	if (message == NULL) {
		lck_mtx_unlock(&vayu_sms_lock);
		return -ENOENT;
	}
	if (message->storage == VAYU_SMS_QMI_STORAGE_NONE) {
		lck_mtx_unlock(&vayu_sms_lock);
		return -EOPNOTSUPP;             /* a transferred message has no storage slot */
	}
	memset(&request, 0, sizeof(request));
	request.storage = (uint8_t)message->storage;
	request.index = message->index;
	request.tag = (uint8_t)message->tag;
	lck_mtx_unlock(&vayu_sms_lock);

	memset(&response, 0, sizeof(response));
	ret = sms_request("delete", VAYU_SMS_QMI_DELETE, vayu_sms_qmi_delete_req_ei, &request,
	    vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, false);
	if (ret != 0) {
		return -EIO;
	}
	lck_mtx_lock(&vayu_sms_lock);
	if (sms.inbox[slot].seq == seq) {
		if (sms.inbox[slot].tag == VAYU_SMS_QMI_TAG_MT_NOT_READ) {
			sms.inbox_unread--;
		}
		memset(&sms.inbox[slot], 0, sizeof(sms.inbox[slot]));
		sms.inbox_count--;
		sms.deleted++;
	}
	lck_mtx_unlock(&vayu_sms_lock);
	say("vayu-sms: message %u deleted from storage %u index %u\n", (unsigned int)seq,
	    (unsigned int)request.storage, (unsigned int)request.index);
	return 0;
}

/* ---- the worker thread --------------------------------------------------------------------------- */

static void
sms_handle_event(struct sms_event *event)
{
	struct vayu_sms_deliver deliver;
	int ret = VAYU_SMS_ERR_UNSUPPORTED;

	if (event->have_route && event->format == VAYU_SMS_QMI_FORMAT_GSM_P2P && event->raw_length > 0) {
		ret = vayu_sms_pdu_decode_deliver(event->raw, event->raw_length, &deliver);
		if (ret == VAYU_SMS_OK) {
			sms_store_pdu(event->storage, event->index, event->tag, event->raw, event->raw_length);
		} else {
			sms.dropped++;
			say("vayu-sms: a transferred PDU is not decoded: %d; %s\n", ret,
			    event->ack == VAYU_SMS_QMI_ACK_NEEDED ? "acknowledged anyway" : "no ack was owed");
		}
		if (event->ack == VAYU_SMS_QMI_ACK_NEEDED) {
			sms_send_ack(event->transaction, ret == VAYU_SMS_OK);
		}
		return;
	}
	if (event->have_mt) {
		(void)sms_read_index(event->storage, event->index, false);
		return;
	}
	sms.dropped++;
}

static void
sms_run_job(struct sms_job *job)
{
	int result = 0;

	switch (job->kind) {
	case SMS_JOB_SEND:
		result = sms_do_send(job->number, job->text, job->text_length, &sms.job_message_id, &sms.job_mr,
		    &sms.job_pdu_length);
		break;
	case SMS_JOB_DELETE:
		result = sms_do_delete(job->seq);
		break;
	case SMS_JOB_REFRESH:
		sms.job_pdu_length = sms_refresh_store((uint8_t)job->storage, false);
		break;
	default:
		result = -EINVAL;
		break;
	}
	lck_mtx_lock(&vayu_sms_lock);
	sms.job_result = result;
	sms.job_busy = false;
	wakeup(&sms.job_done);
	lck_mtx_unlock(&vayu_sms_lock);
}

static void
sms_thread(void *argument __unused, wait_result_t wait __unused)
{
	uint64_t waited_ms = 0;
	int ret;

	ret = qmi_handle_init(&sms.qmi, SMS_HANDLE_MAX, &sms_qmi_ops, sms_handlers);
	if (ret < 0) {
		say("vayu-sms: qmi_handle_init: %d (no QRTR transport?)\n", ret);
		goto out;
	}
	sms.qmi_up = true;
	ret = qmi_add_lookup(&sms.qmi, VAYU_SMS_QMI_WMS_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-sms: qmi_add_lookup(WMS): %d\n", ret);
		goto out;
	}
	while (!sms.wms_there && !sms.stop && waited_ms < (uint64_t)sms.wait_s * 1000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited_ms += 100;
	}
	say("vayu-sms: after %llu ms: WMS is %s\n", (unsigned long long)waited_ms,
	    sms.wms_there ? "there" : "MISSING");
	if (!sms.wms_there) {
		lck_mtx_lock(&vayu_sms_lock);
		sms.state = VAYU_SMS_STATE_FAILED;
		lck_mtx_unlock(&vayu_sms_lock);
		goto out;
	}
	say("vayu-sms: WMS: node %u port %u, version %u instance %u\n", sms.node, sms.port, sms.version,
	    sms.instance);

	sms_configure_routes();
	sms_configure_events();
	/* stored messages can be read whether or not the event report was accepted */
	(void)sms_refresh_store((uint8_t)sms.store, false);
	sms.next_refresh_ms = now_ms() + (uint64_t)sms.refresh_s * 1000;
	sms.next_config_ms = now_ms() + 20000;

	for (;;) {
		struct sms_event event;
		bool have_event = false;
		bool have_job = false;

		lck_mtx_lock(&vayu_sms_lock);
		if (sms.stop) {
			lck_mtx_unlock(&vayu_sms_lock);
			break;
		}
		if (sms.job_pending) {
			sms.job_running = sms.job;
			sms.job_pending = false;
			have_job = true;
		} else if (sms.event_tail != sms.event_head) {
			event = sms.event[sms.event_tail];
			sms.event_tail = (sms.event_tail + 1) % SMS_EVENT_QUEUE;
			have_event = true;
		}
		lck_mtx_unlock(&vayu_sms_lock);

		if (have_job) {
			sms_run_job(&sms.job_running);
			continue;
		}
		if (have_event) {
			sms_handle_event(&event);
			continue;
		}
		if (!sms.event_report && now_ms() >= sms.next_config_ms && sms.wms_there) {
			sms.next_config_ms = now_ms() + 20000;
			sms_configure_events();
			if (sms.event_report) {
				sms.next_refresh_ms = now_ms() + (uint64_t)sms.refresh_s * 1000;
			}
			continue;
		}
		if (sms.refresh_s != 0 && now_ms() >= sms.next_refresh_ms) {
			sms.next_refresh_ms = now_ms() + (uint64_t)sms.refresh_s * 1000;
			(void)sms_refresh_store((uint8_t)sms.store, true);
			continue;
		}
		delay_for_interval(200, NSEC_PER_MSEC);
	}
out:
	if (sms.qmi_up) {
		qmi_handle_release(&sms.qmi);
		sms.qmi_up = false;
	}
	lck_mtx_lock(&vayu_sms_lock);
	sms.running = false;
	sms.wms_there = false;
	lck_mtx_unlock(&vayu_sms_lock);
	say("vayu-sms: the client's thread is done\n");
	thread_terminate(current_thread());
}

/* ---- the device ---------------------------------------------------------------------------------- */

static int
sms_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
sms_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static void
sms_count_locked(struct vayu_sms_count *count)
{
	count->count = sms.inbox_count;
	count->capacity = VAYU_SMS_INBOX;
	count->unread = sms.inbox_unread;
	count->next_seq = sms.next_seq;
}

static int
sms_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct vayu_sms_count count;

	if (uio_resid(uio) < (user_ssize_t)sizeof(count)) {
		return EINVAL;
	}
	lck_mtx_lock(&vayu_sms_lock);
	sms_count_locked(&count);
	lck_mtx_unlock(&vayu_sms_lock);
	return uiomove((caddr_t)&count, sizeof(count), uio);
}

/* Send one job to the worker and wait for its answer.  A Raw Send takes at most 15 s;
 * a refresh that has to sweep 30 slots may take a minute. */
static int
sms_submit(struct sms_job *job)
{
	struct timespec step = { .tv_sec = 5, .tv_nsec = 0 };
	struct timespec limit = { .tv_sec = 90, .tv_nsec = 0 };
	int result;

	lck_mtx_lock(&vayu_sms_lock);
	if (!sms.wms_there || !sms.running || sms.state == VAYU_SMS_STATE_OFF) {
		lck_mtx_unlock(&vayu_sms_lock);
		return ENOTCONN;
	}
	while (sms.job_busy && !sms.stop) {
		if (msleep(&sms.job_done, &vayu_sms_lock, PRIBIO, "vayu-sms-queue", &step) == EWOULDBLOCK) {
			break;
		}
	}
	if (sms.stop) {
		lck_mtx_unlock(&vayu_sms_lock);
		return ECANCELED;
	}
	if (sms.job_busy) {
		lck_mtx_unlock(&vayu_sms_lock);
		return EBUSY;
	}
	sms.job = *job;
	sms.job_pending = true;
	sms.job_busy = true;
	sms.job_result = -EINPROGRESS;
	sms.work = true;
	wakeup(&sms.work);
	while (sms.job_busy && !sms.stop) {
		if (msleep(&sms.job_done, &vayu_sms_lock, PRIBIO, "vayu-sms-job", &limit) == EWOULDBLOCK) {
			break;
		}
	}
	/*
	 * On a timeout the worker may still be inside a WMS transaction; leave
	 * job_busy set so that the next caller waits for it instead of overwriting
	 * the job it is running.
	 */
	result = sms.job_busy ? -ETIMEDOUT : sms.job_result;
	lck_mtx_unlock(&vayu_sms_lock);
	return result;
}

static int
sms_ioctl_count(caddr_t data)
{
	struct vayu_sms_count *count = (struct vayu_sms_count *)data;

	lck_mtx_lock(&vayu_sms_lock);
	sms_count_locked(count);
	lck_mtx_unlock(&vayu_sms_lock);
	return 0;
}

static int
sms_ioctl_get(caddr_t data)
{
	struct vayu_sms_get *get = (struct vayu_sms_get *)data;
	bool found = false;

	lck_mtx_lock(&vayu_sms_lock);
	for (uint32_t i = 0; i < VAYU_SMS_INBOX; i++) {
		if (sms.inbox[i].seq == get->seq) {
			get->message = sms.inbox[i];
			found = true;
			break;
		}
	}
	lck_mtx_unlock(&vayu_sms_lock);
	return found ? 0 : ENOENT;
}

static int
sms_ioctl_send(caddr_t data)
{
	struct vayu_sms_send *send = (struct vayu_sms_send *)data;
	struct sms_job job;
	size_t length;
	int ret;

	length = strnlen(send->number, sizeof(send->number));
	if (length == 0 || length >= sizeof(send->number)) {
		return EINVAL;
	}
	if (send->text_length == 0 || send->text_length >= sizeof(send->text)) {
		return EINVAL;
	}
	memset(&job, 0, sizeof(job));
	job.kind = SMS_JOB_SEND;
	memcpy(job.number, send->number, length);
	job.number[length] = 0;
	job.text_length = send->text_length;
	memcpy(job.text, send->text, send->text_length);
	job.text[send->text_length] = 0;
	ret = sms_submit(&job);
	if (ret == 0) {
		send->message_id = sms.job_message_id;
		send->message_reference = sms.job_mr;
		send->pdu_length = sms.job_pdu_length;
	}
	return ret < 0 ? -ret : ret;
}

static int
sms_ioctl_delete(caddr_t data)
{
	struct vayu_sms_delete *delete = (struct vayu_sms_delete *)data;
	struct sms_job job;

	memset(&job, 0, sizeof(job));
	job.kind = SMS_JOB_DELETE;
	job.seq = delete->seq;
	{
		int ret = sms_submit(&job);

		return ret < 0 ? -ret : ret;
	}
}

static int
sms_ioctl_refresh(caddr_t data)
{
	uint32_t storage = *(uint32_t *)data;
	struct sms_job job;
	int ret;

	if (storage != VAYU_SMS_QMI_STORAGE_UIM && storage != VAYU_SMS_QMI_STORAGE_NV) {
		return EINVAL;
	}
	memset(&job, 0, sizeof(job));
	job.kind = SMS_JOB_REFRESH;
	job.storage = storage;
	ret = sms_submit(&job);
	if (ret != 0) {
		return ret < 0 ? -ret : ret;
	}
	*(uint32_t *)data = sms.job_pdu_length;
	return 0;
}

static int
sms_ioctl_status(caddr_t data)
{
	struct vayu_sms_status *status = (struct vayu_sms_status *)data;

	memset(status, 0, sizeof(*status));
	lck_mtx_lock(&vayu_sms_lock);
	status->magic = VAYU_SMS_MAGIC;
	status->version = VAYU_SMS_VERSION;
	status->size = sizeof(*status);
	status->state = sms.state;
	status->wms_present = sms.wms_there ? 1 : 0;
	status->event_report = sms.event_report ? 1 : 0;
	status->routes_known = sms.routes_known ? 1 : 0;
	status->routes_written = sms.routes_written ? 1 : 0;
	status->routes_allowed = sms.routes_arg;
	status->route_count = sms.route_count;
	status->store = sms.store;
	status->inbox_count = sms.inbox_count;
	status->received = sms.received;
	status->sent = sms.sent;
	status->deleted = sms.deleted;
	status->dropped = sms.dropped;
	status->requests = sms.requests;
	status->failures = sms.failures;
	status->last_result = sms.last_result;
	status->last_error = sms.last_error;
	status->last_message = sms.last_message;
	status->last_gsm_cause = sms.last_gsm_cause;
	lck_mtx_unlock(&vayu_sms_lock);
	return 0;
}

static int
sms_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (command) {
	case VAYU_SMS_IOC_COUNT:
		return sms_ioctl_count(data);
	case VAYU_SMS_IOC_GET:
		return sms_ioctl_get(data);
	case VAYU_SMS_IOC_SEND:
		return sms_ioctl_send(data);
	case VAYU_SMS_IOC_DELETE:
		return sms_ioctl_delete(data);
	case VAYU_SMS_IOC_REFRESH:
		return sms_ioctl_refresh(data);
	case VAYU_SMS_IOC_STATUS:
		return sms_ioctl_status(data);
	default:
		return ENOTTY;
	}
}

static const struct cdevsw sms_cdevsw = {
	.d_open = sms_open, .d_close = sms_close, .d_read = sms_read, .d_write = eno_rdwrt,
	.d_ioctl = sms_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
sms_make_node(void)
{
	int major = cdevsw_add(-1, &sms_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, SMS_DEVICE) == NULL) {
		say("vayu-sms: /dev/%s cannot be made\n", SMS_DEVICE);
		return false;
	}
	say("vayu-sms: /dev/%s (0666) is made: the count record is %u bytes\n", SMS_DEVICE,
	    (unsigned int)sizeof(struct vayu_sms_count));
	return true;
}

/* ---- start and stop ------------------------------------------------------------------------------ */

int
vayu_sms_start(struct vayu_qrtr *q)
{
	thread_t thread;
	uint32_t level = 0, wait_s = 30, routes_arg = 0, store = VAYU_SMS_QMI_STORAGE_UIM, refresh_s = 60;
	uint32_t scan_max = 20;

	if (!PE_parse_boot_argn("vayu_sms", &level, sizeof(level)) || level == 0) {
		return 0;
	}
	if (sms.started) {
		say("vayu-sms: already started\n");
		return 0;
	}
	if (sms.running) {
		return -EBUSY;
	}
	if (q == NULL || q->glink == NULL || !q->glink->up) {
		say("vayu-sms: vayu_sms=%u, but the modem's GLINK channel is not up: nothing is done\n", level);
		return 0;
	}
	PE_parse_boot_argn("vayu_sms_wait", &wait_s, sizeof(wait_s));
	if (wait_s < 1) {
		wait_s = 1;
	}
	if (wait_s > 120) {
		wait_s = 120;
	}
	PE_parse_boot_argn("vayu_sms_routes", &routes_arg, sizeof(routes_arg));
	if (routes_arg != 0 && routes_arg != 1) {
		say("vayu-sms: vayu_sms_routes=%u is refused: only 1 allows Set Routes\n", routes_arg);
		routes_arg = 0;
	}
	PE_parse_boot_argn("vayu_sms_store", &store, sizeof(store));
	if (store != VAYU_SMS_QMI_STORAGE_UIM && store != VAYU_SMS_QMI_STORAGE_NV) {
		say("vayu-sms: vayu_sms_store=%u is refused: 0 (UIM) and 1 (NV) only\n", store);
		store = VAYU_SMS_QMI_STORAGE_UIM;
	}
	PE_parse_boot_argn("vayu_sms_refresh", &refresh_s, sizeof(refresh_s));
	if (refresh_s != 0 && refresh_s < 15) {
		refresh_s = 15;
	}
	if (refresh_s > 3600) {
		refresh_s = 3600;
	}
	PE_parse_boot_argn("vayu_sms_scan", &scan_max, sizeof(scan_max));
	if (scan_max < 1) {
		scan_max = 1;
	}
	if (scan_max > 40) {
		scan_max = 40;
	}
	memset(&sms.inbox, 0, sizeof(sms.inbox));
	sms.inbox_count = sms.inbox_unread = 0;
	sms.next_seq = 1;
	sms.mr = 0;
	sms.level = level;
	sms.wait_s = wait_s;
	sms.routes_arg = routes_arg;
	sms.store = store;
	sms.refresh_s = refresh_s;
	sms.scan_max = scan_max;
	sms.stop = false;
	sms.running = true;
	sms.state = VAYU_SMS_STATE_WAITING;
	sms.event_report = false;
	sms.routes_known = false;
	sms.routes_written = false;
	sms.route_count = 0;
	sms.received = sms.sent = sms.deleted = sms.dropped = 0;
	sms.requests = sms.failures = 0;
	sms.last_result = sms.last_error = 0xffff;
	sms.last_message = 0;
	sms.last_gsm_cause = 0xffff;
	sms.last_failure = 0;
	sms.job_pending = sms.job_busy = sms.job_done = false;
	sms.job.kind = SMS_JOB_NONE;
	sms.event_head = sms.event_tail = 0;
	sms.work = false;
	sms.next_refresh_ms = 0;
	sms.next_config_ms = now_ms() + 20000;
	say("vayu-sms: vayu_sms=%u vayu_sms_wait=%u vayu_sms_routes=%u vayu_sms_store=%u vayu_sms_refresh=%u "
	    "vayu_sms_scan=%u\n", level, wait_s, routes_arg, store, refresh_s, scan_max);
	say("vayu-sms: WMS 0x05 (routes, event report, list, raw read, raw send, delete, ack); "
	    "the log masks numbers and never prints text\n");
	(void)sms_make_node();
	sms.started = true;
	if (kernel_thread_start(sms_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-sms: no thread\n");
		sms.running = false;
		sms.started = false;
		sms.state = VAYU_SMS_STATE_OFF;
		return -ENOMEM;
	}
	thread_deallocate(thread);
	return 0;
}

void
vayu_sms_stop(void)
{
	if (!sms.started) {
		return;
	}
	lck_mtx_lock(&vayu_sms_lock);
	sms.stop = true;
	wakeup(&sms.work);
	wakeup(&sms.job_done);
	lck_mtx_unlock(&vayu_sms_lock);
	for (uint32_t waited = 0; waited < SMS_STOP_MS && sms.running; waited += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	if (sms.running) {
		say("vayu-sms: the client's thread has not finished in %u ms; /dev/%s still holds the inbox\n",
		    SMS_STOP_MS, SMS_DEVICE);
	} else {
		sms.started = false;
		sms.state = VAYU_SMS_STATE_OFF;
		say("vayu-sms: done: %u received, %u sent, %u deleted, %u dropped; /dev/%s still holds %u message(s)\n",
		    (unsigned int)sms.received, (unsigned int)sms.sent, (unsigned int)sms.deleted,
		    (unsigned int)sms.dropped, SMS_DEVICE, (unsigned int)sms.inbox_count);
	}
}
