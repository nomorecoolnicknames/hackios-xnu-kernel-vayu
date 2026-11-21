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
 * (20, the largest index Raw Read sweeps when List Messages is refused),
 * vayu_sms_smsc=0|1 (0: Raw Send gets the TPDU and the modem uses EF_SMSC;
 * 1: the full PDU with a leading 0x00 "default SMSC" byte),
 * vayu_sms_list=0|1|2 (0: List with tag 1, then tag 0 when that is refused;
 * 1: tag 1 only; 2: tag 0 only), vayu_sms_ims_tlv=2|19|0 (Raw Send's SMS-on-IMS
 * TLV type: 2 per qmi-service-wms.json, 19 = 0x13 the k25 form, 0 omit),
 * vayu_sms_format=N (Raw Send's message format byte, default 6),
 * vayu_sms_send_timeout=S (30, how long Raw Send waits for the network's answer),
 * vayu_sms=3 (CommCenter is the WMS client through the baseband bridge, vayu_qmux=1:
 * no kernel client, only content-free log lines of the frames that pass -- see the
 * "CommCenter is the WMS client" section; vayu_sms=1/2 with vayu_qmux=1 is taken as 3
 * unless vayu_sms_coexist=1), vayu_sms_strict=1 (/dev/vayu-sms 0600 instead of 0666).
 *
 * The two WMS requests that matter here, as qmi-service-wms.json has them:
 *   List Messages 0x0031: 0x01 storage, 0x11 message tag, 0x12 message mode
 *                         (all three are sent: this modem answers error 17 without
 *                         any one of them); reply 0x02 result and 0x10 message list.
 *   Raw Send      0x0020: 0x01 {format, u16 length, PDU}, 0x02 SMS on IMS.
 * The list TLV of the reply is accepted under 0x10 and under k25's 0x01.
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
#include <kern/thread_call.h>
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
#include <sys/vayu_sms_wms.h>

extern void loader_log_putc(char c);

#define SMS_DEVICE              "vayu-sms"
#define SMS_HANDLE_MAX          4096    /* the largest datagram the QMI handle takes */
#define SMS_REPLY_MS            5000    /* one WMS request's reply */
#define SMS_SEND_REPLY_MS       30000   /* the default Raw Send wait (vayu_sms_send_timeout) */
#define SMS_STOP_MS             45000   /* one Raw Send may take 30 s; let it finish */
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
	uint32_t message_id, mr, pdu_length;    /* the job's own results, copied out under the lock */
};

struct sms_state {
	struct qmi_handle qmi;
	bool qmi_up;
	volatile bool wms_there, stop, running, started;
	uint32_t node, port, version, instance, generation;
	uint32_t level, wait_s, routes_arg, store, refresh_s, scan_max, smsc_prefix;
	bool observer;                          /* vayu_sms=3: CommCenter is the WMS client */
	bool strict;                            /* vayu_sms_strict=1: the node is 0600 */
	uint32_t list_mode, ims_tlv, send_format, send_timeout_s;
	/* what the last List Messages attempt did, so a quiet sweep can say when it changes */
	uint32_t list_last_tag, list_last_result, list_last_error, list_last_id;
	uint32_t list_ok_tag;                   /* the tag value that last listed, 0xffff: none yet */
	uint32_t last_read_error;               /* the last Raw Read's modem error, 0xffff: no answer */
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
	bool job_pending, job_busy, job_done, job_waiting;      /* job_waiting: its caller has not taken the results yet */
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

/* ---- the wire dump ------------------------------------------------------------------------------- */

/* The reply's metadata only: its header, its TLVs' types and sizes, Result and the cause TLVs -- no payload bytes
 * either way (a request carries the recipient and the text, a reply the stored messages). */
struct sms_dump {
	bool response;
};

/*
 * The QMI errors bsd/sys/hakios.qmi.h names, plus the three numbers this modem has
 * returned that the common table does not name (47, 48, 71).  Their names are a
 * hypothesis, not a fact: the numbers are what the log proves, so the question mark
 * stays next to the guess.
 */
static const char *
sms_error_name(uint16_t error)
{
	switch (error) {
	case 0: return "NONE";
	case 1: return "MALFORMED_MSG";
	case 2: return "NO_MEMORY";
	case 3: return "INTERNAL";
	case 4: return "ABORTED";
	case 5: return "CLIENT_IDS_EXHAUSTED";
	case 17: return "MISSING_ARGUMENT";
	case 18: return "ARG_TOO_LONG";
	case 41: return "INVALID_ID";
	case 58: return "ENCODING";
	case 69: return "DISABLED";
	case 90: return "INCOMPATIBLE_STATE";
	case 94: return "NOT_SUPPORTED";
	case 47: return "47? storage/device not ready?";
	case 48: return "48? no SMSC or network not ready?";
	case 71: return "71? command not implemented?";
	default: return "?";
	}
}

/* The reply's header, its TLVs and, when present, result/error and the cause TLVs. */
static void
sms_dump_response(const char *what, const uint8_t *raw, size_t length)
{
	char tlvs[256];
	size_t at, end, used = 0;
	uint16_t result = 0xffff, error = 0xffff;
	uint16_t cdma_cause = 0xffff;
	uint16_t gw_rp = 0xffff;
	uint8_t cdma_class = 0xff, gw_tp = 0xff;
	bool causes;

	if (length < QMI_HEADER_SIZE) {
		say("vayu-sms: %s response: none (%u bytes)\n", what, (unsigned int)length);
		return;
	}
	/*
	 * A List Messages reply reuses 0x10/0x11/0x12 for the list, the tag and the
	 * mode; the CDMA/GW cause TLVs only exist in the other replies.
	 */
	causes = vayu_sms_le16(raw + 3) != VAYU_SMS_QMI_LIST_MESSAGES;
	say("vayu-sms: %s response: type %u txn %u msg 0x%04x payload %u\n", what, raw[0],
	    (unsigned int)vayu_sms_le16(raw + 1), (unsigned int)vayu_sms_le16(raw + 3),
	    (unsigned int)vayu_sms_le16(raw + 5));
	tlvs[0] = 0;
	end = QMI_HEADER_SIZE + (size_t)vayu_sms_le16(raw + 5);
	if (end > length) {
		end = length;
	}
	for (at = QMI_HEADER_SIZE; at + 3 <= end;) {
		uint8_t type = raw[at];
		uint16_t size = vayu_sms_le16(raw + at + 1);
		const uint8_t *value = raw + at + 3;
		int wrote;

		if (at + 3u + size > end) {
			break;
		}
		wrote = snprintf(tlvs + used, sizeof(tlvs) - used, "0x%02x/%u%s", type, (unsigned int)size,
		    at + 3u + size < end ? " " : "");
		if (wrote < 0 || (size_t)wrote >= sizeof(tlvs) - used) {
			break;
		}
		used += (size_t)wrote;
		if (type == 0x02 && size >= 4) {
			result = vayu_sms_le16(value);
			error = vayu_sms_le16(value + 2);
		} else if (causes && type == 0x10 && size >= 2) {
			cdma_cause = vayu_sms_le16(value);
		} else if (causes && type == 0x11 && size >= 1) {
			cdma_class = value[0];
		} else if (causes && type == 0x12 && size >= 3) {
			gw_rp = vayu_sms_le16(value);
			gw_tp = value[2];
		}
		at += 3u + size;
	}
	say("vayu-sms: %s TLVs: %s\n", what, tlvs[0] != 0 ? tlvs : "(none)");
	if (result != 0xffff) {
		say("vayu-sms: %s result %u error %u (%s)\n", what, (unsigned int)result, (unsigned int)error,
		    sms_error_name(error));
	}
	if (cdma_cause != 0xffff || cdma_class != 0xff || gw_rp != 0xffff) {
		say("vayu-sms: %s cause: cdma-code %u class %u, gw-rp %u tp %u\n", what, (unsigned int)cdma_cause,
		    (unsigned int)cdma_class, (unsigned int)gw_rp, (unsigned int)gw_tp);
	}
}

/*
 * One request and its reply, waited `timeout` ms.  Called only from the worker
 * thread: an indication callback must never wait (its reply is delivered by the
 * same QRTR thread).  `quiet` suppresses the log and the failure counter for an
 * expected refusal (an empty Raw Read slot, for instance).  `dump`, when set,
 * prints only the reply's metadata, Result and cause fields: no payload bytes.
 */
static int
sms_request(const char *what, uint16_t message, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, struct vayu_sms_qmi_result *result, uint32_t timeout, bool quiet,
    const struct sms_dump *dump)
{
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = sms.node, .sq_port = sms.port };
	struct qmi_txn txn;
	uint8_t raw[512];
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
	if (dump != NULL && dump->response) {
		txn.raw = raw;
		txn.raw_size = sizeof(raw);
		txn.raw_length = 0;
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
	if (ret >= 0 && !txn.result_present) {
		ret = -EINVAL;          /* (a zeroed result is not a success the modem gave) */
	}
	if (dump != NULL && dump->response) {
		sms_dump_response(what, raw, ret < 0 ? 0 : txn.raw_length);
	}
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
				say("vayu-sms: %s: the modem says result %u error %u (%s)\n", what,
				    (unsigned int)result->result, (unsigned int)result->error,
				    sms_error_name(result->error));
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
	int ret;

	memset(&request, 0, sizeof(request));
	request.id.storage = storage;
	request.id.index = index;
	request.mode_valid = 1;
	request.mode = VAYU_SMS_QMI_MODE_GSM;
	memset(response, 0, sizeof(*response));
	ret = sms_request("raw read", VAYU_SMS_QMI_RAW_READ, vayu_sms_qmi_raw_read_req_ei, &request,
	    vayu_sms_qmi_raw_read_resp_ei, response, &response->result, quiet ? 2000 : SMS_REPLY_MS, quiet, NULL);
	/*
	 * A quiet Raw Read does not log, but its answer is what tells a sweep apart
	 * "empty slot" from "storage refused"; keep the last one for the sweep line.
	 */
	sms.last_read_error = (ret == 0 || ret == -EIO) ? response->result.error : 0xffff;
	return ret;
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
 * List Messages, then Raw Read every MT entry.
 *
 * List's tag TLV 0x11 is optional in qmi-service-wms.json ("every tag" when it is
 * absent), but this modem answers error 17 without it, so k25 sent tag 1
 * (MT_NOT_READ).  It then answered error 47.  Whether the tag value matters is
 * exactly what the ladder answers: tag 1 first, then tag 0 (MT_READ), and the run's
 * log shows both answers.  vayu_sms_list pins one value for a clean A/B.
 *
 * The reply's list TLV is 0x10 in qmi-service-wms.json and was 0x01 in k25; both are
 * decoded here, and the log says which one the modem used.
 *
 * When both attempts are refused, sweep the first vayu_sms_scan indices instead.
 */
static uint32_t
sms_refresh_store(uint8_t storage, bool quiet)
{
	struct vayu_sms_qmi_message_list list;
	static const struct sms_dump list_dump = { .response = true };
	uint8_t tags[2];
	uint32_t tag_count = 0, found = 0;

	if (sms.list_mode == 1) {
		tags[tag_count++] = VAYU_SMS_QMI_TAG_MT_NOT_READ;
	} else if (sms.list_mode == 2) {
		tags[tag_count++] = VAYU_SMS_QMI_TAG_MT_READ;
	} else {
		tags[tag_count++] = VAYU_SMS_QMI_TAG_MT_NOT_READ;
		tags[tag_count++] = VAYU_SMS_QMI_TAG_MT_READ;
	}
	if (tag_count == 2 && sms.list_ok_tag == VAYU_SMS_QMI_TAG_MT_READ) {
		tags[0] = VAYU_SMS_QMI_TAG_MT_READ;
		tags[1] = VAYU_SMS_QMI_TAG_MT_NOT_READ;
	}

	for (uint32_t attempt = 0; attempt < tag_count && !sms.stop; attempt++) {
		struct vayu_sms_qmi_list_req request = {
			.storage = storage,
			.tag = tags[attempt],
			.mode = VAYU_SMS_QMI_MODE_GSM,
		};
		struct vayu_sms_qmi_list_resp response;
		const char *payload = NULL;
		uint8_t list_id = 0;
		uint32_t count, listed_now = 0;
		bool dump = !quiet || sms.level >= 2;
		int ret;

		memset(&response, 0, sizeof(response));
		ret = sms_request("list messages", VAYU_SMS_QMI_LIST_MESSAGES, vayu_sms_qmi_list_req_ei, &request,
		    vayu_sms_qmi_list_resp_ei, &response, &response.result, SMS_REPLY_MS, quiet,
		    dump ? &list_dump : NULL);
		{
			uint32_t error = response.result.error;

			/*
			 * A quiet sweep repeats every vayu_sms_refresh seconds; say the result
			 * only when it changes, so a refused List cannot fill the log.
			 */
			if (quiet && dump == false &&
			    (sms.list_last_tag != tags[attempt] || sms.list_last_result != ret ||
			    sms.list_last_error != error)) {
				say("vayu-sms: list messages (tag %u): %s, result %u error %u (%s)\n",
				    (unsigned int)tags[attempt], ret == 0 ? "answered" : "not answered",
				    (unsigned int)response.result.result, (unsigned int)error, sms_error_name((uint16_t)error));
				sms.list_last_tag = tags[attempt];
				sms.list_last_result = (uint32_t)ret;
				sms.list_last_error = error;
			}
		}
		if (ret == 0) {
			if (response.list10_valid) {
				payload = response.list10;
				list_id = 0x10;
			} else if (response.list_valid) {
				payload = response.list;
				list_id = 0x01;
			}
			if (payload == NULL) {
				say("vayu-sms: list messages: answered without a list TLV (0x01 or 0x10)\n");
				continue;
			}
			count = vayu_sms_le32((const uint8_t *)payload);
			if (4u + (size_t)count * 5u > VAYU_SMS_QMI_LIST_RAW ||
			    vayu_sms_qmi_list_parse(payload, 4u + (size_t)count * 5u, &list) != VAYU_SMS_OK) {
				say("vayu-sms: list messages: the list TLV 0x%02x does not parse; nothing read from it\n",
				    (unsigned int)list_id);
				continue;
			}
			sms.list_ok_tag = tags[attempt];
			if (sms.list_last_id != list_id) {
				sms.list_last_id = list_id;
				say("vayu-sms: list messages: the modem uses TLV 0x%02x for the list\n",
				    (unsigned int)list_id);
			}
			say("vayu-sms: list messages: tag %u answered; storage %u lists %u message(s)\n",
			    (unsigned int)tags[attempt], (unsigned int)storage, (unsigned int)list.count);
			for (uint32_t i = 0; i < list.count; i++) {
				if (sms.stop) {
					break;
				}
				if (list.entry[i].tag != VAYU_SMS_QMI_TAG_MT_READ &&
				    list.entry[i].tag != VAYU_SMS_QMI_TAG_MT_NOT_READ) {
					continue;
				}
				if (sms_read_index(storage, list.entry[i].index, true) > 0) {
					listed_now++;
				}
			}
			return listed_now;
		}
	}
	if (!quiet) {
		say("vayu-sms: list messages is not answered; sweeping the first %u indices of storage %u\n",
		    (unsigned int)sms.scan_max, (unsigned int)storage);
	}
	{
		uint32_t timeouts = 0, refused = 0, first_error = 0xffff;

		for (uint32_t index = 0; index < sms.scan_max && !sms.stop; index++) {
			int result = sms_read_index(storage, index, true);

			if (sms.last_read_error != 0 && sms.last_read_error != 0xffff) {
				refused++;
				if (first_error == 0xffff) {
					first_error = sms.last_read_error;
				}
			}
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
		if (!quiet && refused != 0) {
			say("vayu-sms: storage %u: %u Raw Read(s) refused, first error %u (%s)\n", (unsigned int)storage,
			    (unsigned int)refused, (unsigned int)first_error, sms_error_name((uint16_t)first_error));
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

/* Get Supported Messages (0x001E): read-only, says whether the six ids exist at all. */
static void
sms_query_supported(void)
{
	struct vayu_sms_qmi_supported supported;
	bool raw_send = false, raw_read = false, list = false, delete = false, set_event = false;
	int ret;

	memset(&supported, 0, sizeof(supported));
	ret = sms_request("get supported messages", VAYU_SMS_QMI_GET_SUPPORTED, NULL, NULL,
	    vayu_sms_qmi_supported_ei, &supported, &supported.result, SMS_REPLY_MS, false, NULL);
	if (ret != 0 || !supported.list_valid) {
		say("vayu-sms: get supported messages gave no list (last result %u error %u (%s)); the six ids are "
		    "tried anyway\n", (unsigned int)sms.last_result, (unsigned int)sms.last_error,
		    sms_error_name((uint16_t)sms.last_error));
		return;
	}
	for (uint32_t i = 0; i < supported.count && i < VAYU_SMS_SUPPORTED_MAX; i++) {
		switch (supported.ids[i]) {
		case VAYU_SMS_QMI_RAW_SEND: raw_send = true; break;
		case VAYU_SMS_QMI_RAW_READ: raw_read = true; break;
		case VAYU_SMS_QMI_LIST_MESSAGES: list = true; break;
		case VAYU_SMS_QMI_DELETE: delete = true; break;
		case VAYU_SMS_QMI_SET_EVENT_REPORT: set_event = true; break;
		default: break;
		}
	}
	say("vayu-sms: supported: raw-send=%s raw-read=%s list=%s delete=%s set-event=%s (%u ids)\n",
	    raw_send ? "yes" : "no", raw_read ? "yes" : "no", list ? "yes" : "no", delete ? "yes" : "no",
	    set_event ? "yes" : "no", (unsigned int)supported.count);
}

/* Get Routes; install a store route only when the table is empty and explicitly allowed. */
static void
sms_configure_routes(void)
{
	struct vayu_sms_qmi_get_routes_resp routes;
	int ret;

	memset(&routes, 0, sizeof(routes));
	ret = sms_request("get routes", VAYU_SMS_QMI_GET_ROUTES, NULL, NULL, vayu_sms_qmi_get_routes_resp_ei,
	    &routes, &routes.result, SMS_REPLY_MS, false, NULL);
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
		    vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, false, NULL);
		if (ret != 0) {
			say("vayu-sms: set routes was refused; incoming messages depend on the modem's table\n");
			return;
		}
		sms.routes_written = true;
		/* verify what the modem now says, without trusting the write's echo */
		memset(&routes, 0, sizeof(routes));
		ret = sms_request("get routes after set", VAYU_SMS_QMI_GET_ROUTES, NULL, NULL,
		    vayu_sms_qmi_get_routes_resp_ei, &routes, &routes.result, SMS_REPLY_MS, false, NULL);
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
	    &request, vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, false, NULL);
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
	    vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, true, NULL);
}

static int
sms_do_send(const char *number, const char *text, uint32_t text_length, uint32_t *message_id, uint32_t *reference,
    uint32_t *pdu_length)
{
	uint8_t pdu[VAYU_SMS_PDU_MAX];
	struct vayu_sms_qmi_raw_send_req request;
	struct vayu_sms_qmi_raw_send_resp response;
	struct sms_dump dump = { .response = true };
	size_t length = 0;
	bool full_pdu = sms.smsc_prefix != 0;
	uint32_t dcs = 0;
	uint8_t mr = sms.mr++;
	int ret;

	ret = vayu_sms_pdu_encode_submit(number, text, text_length, mr, pdu, sizeof(pdu), &length, &dcs);
	if (ret != VAYU_SMS_OK) {
		say("vayu-sms: send: the PDU encoder refused the message (%d) for %u bytes of %s text\n", ret,
		    (unsigned int)text_length, dcs == 0x08 ? "UCS-2" : "GSM 7-bit");
		return ret == VAYU_SMS_ERR_TOO_LONG ? -EMSGSIZE : -EINVAL;
	}
	if (!full_pdu) {
		const uint8_t *tpdu;
		size_t tpdu_length;

		ret = vayu_sms_pdu_submit_tpdu(pdu, length, &tpdu, &tpdu_length);
		if (ret != VAYU_SMS_OK) {
			say("vayu-sms: send: the SMSC prefix cannot be removed: %d\n", ret);
			return -EINVAL;
		}
		memmove(pdu, tpdu, tpdu_length);
		length = tpdu_length;
	}
	if (vayu_sms_qmi_raw_send_fill_format(&request, (uint8_t)sms.send_format, pdu, length) != VAYU_SMS_OK) {
		return -EMSGSIZE;
	}
	if (sms.ims_tlv == 0) {
		request.sms_on_ims_valid = 0;           /* no SMS-on-IMS TLV at all */
	}
	say("vayu-sms: raw send: %u-byte %s, SMSC %s, format 0x%02x, SMS-on-IMS TLV %s\n", (unsigned int)length,
	    full_pdu ? "PDU (with SMSC prefix)" : "TPDU (SMSC from EF_SMSC)", full_pdu ? "prefix 0x00" : "modem's",
	    (unsigned int)sms.send_format,
	    sms.ims_tlv == 0 ? "omitted" : (sms.ims_tlv == 0x13 ? "0x13 (the k25 form)" : "0x02"));
	memset(&response, 0, sizeof(response));
	ret = sms_request("raw send", VAYU_SMS_QMI_RAW_SEND,
	    sms.ims_tlv == 0x13 ? vayu_sms_qmi_raw_send_req_legacy_ei : vayu_sms_qmi_raw_send_req_ei, &request,
	    vayu_sms_qmi_raw_send_resp_ei, &response, &response.result, sms.send_timeout_s * 1000, false, &dump);
	if (ret != 0) {
		if (response.cdma_cause_valid || response.cdma_class_valid) {
			say("vayu-sms: raw send refused: CDMA cause %u, class %u\n",
			    (unsigned int)response.cdma_cause, (unsigned int)response.cdma_class);
		}
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
	say("vayu-sms: one message sent: %u bytes, %s, message id %u, MR %u\n", (unsigned int)length,
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
	request.mode = VAYU_SMS_QMI_MODE_GSM;
	lck_mtx_unlock(&vayu_sms_lock);

	memset(&response, 0, sizeof(response));
	ret = sms_request("delete", VAYU_SMS_QMI_DELETE, vayu_sms_qmi_delete_req_ei, &request,
	    vayu_sms_qmi_result_resp_ei, &response, &response.result, SMS_REPLY_MS, false, NULL);
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

	sms_query_supported();
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

/* ---- vayu_sms=3: CommCenter is the WMS client, the kernel watches ------------------------------------ */

/*
 * On the stock path CommCenter's QMISMSCommandDriver is the modem's WMS client (bsd/sys/
 * vayu_sms_wms.h has what it sends), and its frames pass through the baseband bridge.  A
 * second, kernel client would be wrong there: both would get the transfer route's MT and
 * both would answer its ack, and the kernel would keep a copy of a message that belongs to
 * Messages.  So the kernel starts none, and reads what passes instead, for three things
 * the run needs to tell apart -- the modem never delivered, CommCenter never acked, the
 * network refused the send -- with no address, body, PDU byte or SMSC in the log.
 *
 * Checkpoints, as log lines (grep "vayu-sms: cc"):
 *   cc> bind subscription / indication register / set event report / set routes ...
 *   cc< <request>: result R error E           (a failure is always printed)
 *   cc< service ready: 3GPP                    (WMS is ready for CommCenter)
 *   cc< MT #n: transfer route, ack owed, ...   (the modem gave CommCenter a message)
 *   cc> MT #n: CommCenter acks it (success 1) after T ms
 *   cc: MT #n: no ack from CommCenter in 35 s  (CommCenter got it and did nothing)
 *   cc> raw send: format F, L-byte PDU / cc< raw send: OK, message id M | failed ... RP cause C
 */
#define SMS_CC_ACK_MS           35000   /* how long an owed ack may take before it is reported */
#define SMS_CC_PENDING          8
#define SMS_CC_QUIET_AFTER      6       /* routine requests of one id logged this many times, failures always */

struct sms_cc_pending {
	bool used;
	uint32_t number;                /* the MT's ordinal in this boot, the log's #n */
	uint32_t transaction;
	uint64_t at_ms;
};

struct sms_cc_state {
	bool on;
	uint32_t frames, mt, mt_acked, mt_nacked, mt_unacked, mt_stored, mo, mo_ok, mo_failed;
	uint32_t ack_failed, failures, deletes;
	uint32_t ready_status;
	bool ready_known, event_report, routes_set;
	uint32_t route_count;
	uint32_t last_message, last_result, last_error, last_rp_cause;
	uint8_t seen[2][0x80];          /* per direction and id, how often it was logged */
	struct sms_cc_pending pending[SMS_CC_PENDING];
	thread_call_t ack_call;
	bool ack_armed;
};

static struct sms_cc_state cc;

static LCK_MTX_DECLARE(vayu_sms_cc_lock, &vayu_sms_lck_grp);

static const char *
sms_cc_name(uint16_t message, char buffer[8])
{
	const char *name = vayu_sms_wms_name(message);

	if (name != NULL) {
		return name;
	}
	snprintf(buffer, 8, "0x%04x", (unsigned int)message);
	return buffer;
}

/* Routine traffic is logged the first few times per id; a failure or a checkpoint always. */
static bool
sms_cc_routine_locked(uint8_t direction, uint16_t message)
{
	uint8_t *seen;

	if (message >= 0x80) {
		return true;
	}
	seen = &cc.seen[direction ? 1 : 0][message];
	if (*seen >= SMS_CC_QUIET_AFTER) {
		return false;
	}
	(*seen)++;
	return true;
}

static void
sms_cc_arm_locked(void)
{
	uint64_t deadline;

	if (cc.ack_call == NULL || cc.ack_armed) {
		return;
	}
	clock_interval_to_deadline(SMS_CC_ACK_MS / 1000 + 1, NSEC_PER_SEC, &deadline);
	cc.ack_armed = true;
	thread_call_enter_delayed(cc.ack_call, deadline);
}

static void
sms_cc_ack_check(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t now = now_ms();
	bool left = false;

	lck_mtx_lock(&vayu_sms_cc_lock);
	cc.ack_armed = false;
	for (uint32_t i = 0; i < SMS_CC_PENDING; i++) {
		struct sms_cc_pending *p = &cc.pending[i];

		if (!p->used) {
			continue;
		}
		if (now - p->at_ms >= SMS_CC_ACK_MS) {
			cc.mt_unacked++;
			say("vayu-sms: cc: MT #%u: no ack from CommCenter in %u s (the network will repeat it); "
			    "%u of %u MT unacknowledged\n", (unsigned int)p->number, SMS_CC_ACK_MS / 1000,
			    (unsigned int)cc.mt_unacked, (unsigned int)cc.mt);
			p->used = false;
		} else {
			left = true;
		}
	}
	if (left) {
		sms_cc_arm_locked();
	}
	lck_mtx_unlock(&vayu_sms_cc_lock);
}

static void
sms_cc_summary_locked(void)
{
	say("vayu-sms: cc: MT %u (acked %u, refused %u, unacked %u, stored %u), MO %u (ok %u, failed %u), "
	    "%u failed WMS answer(s)\n", (unsigned int)cc.mt, (unsigned int)cc.mt_acked,
	    (unsigned int)cc.mt_nacked, (unsigned int)cc.mt_unacked, (unsigned int)cc.mt_stored,
	    (unsigned int)cc.mo, (unsigned int)cc.mo_ok, (unsigned int)cc.mo_failed, (unsigned int)cc.failures);
}

static void
sms_cc_request_locked(const struct vayu_sms_wms_note *n)
{
	char buffer[8];
	const char *name = sms_cc_name(n->message, buffer);

	switch (n->message) {
	case VAYU_SMS_WMS_SEND_ACK:
	case VAYU_SMS_WMS_ASYNC_SEND_ACK:
		if (n->ack_valid) {
			uint32_t number = 0;
			uint64_t waited = 0;

			for (uint32_t i = 0; i < SMS_CC_PENDING; i++) {
				if (cc.pending[i].used && cc.pending[i].transaction == n->ack_transaction) {
					number = cc.pending[i].number;
					waited = now_ms() - cc.pending[i].at_ms;
					cc.pending[i].used = false;
					break;
				}
			}
			if (n->ack_success) {
				cc.mt_acked++;
			} else {
				cc.mt_nacked++;
			}
			if (number != 0) {
				say("vayu-sms: cc> MT #%u: CommCenter acks it (%s, success %u, protocol %u) after %llu ms\n",
				    (unsigned int)number, name, (unsigned int)n->ack_success, (unsigned int)n->ack_protocol,
				    (unsigned long long)waited);
			} else {
				say("vayu-sms: cc> %s (success %u) for a transaction this observer did not see\n", name,
				    (unsigned int)n->ack_success);
			}
			return;
		}
		break;
	case VAYU_SMS_WMS_RAW_SEND:
	case VAYU_SMS_WMS_ASYNC_RAW_SEND:
		cc.mo++;
		if (n->send_valid) {
			say("vayu-sms: cc> MO #%u: %s, format %u, %u-byte PDU\n", (unsigned int)cc.mo, name,
			    (unsigned int)n->send_format, (unsigned int)n->send_length);
		} else {
			say("vayu-sms: cc> MO #%u: %s without a whole raw message TLV\n", (unsigned int)cc.mo, name);
		}
		return;
	case VAYU_SMS_WMS_SET_ROUTES:
		if (n->routes_valid) {
			say("vayu-sms: cc> set routes: %u route(s)%s\n", (unsigned int)n->route_count,
			    n->transfer_ind_valid ? (n->transfer_ind ? ", transfer status reports" :
			    ", status reports stored") : "");
			for (uint32_t i = 0; i < n->route_count && i < VAYU_SMS_WMS_ROUTES_MAX; i++) {
				say("vayu-sms: cc> route %u: type %u, class %u, storage %u, action %s\n",
				    (unsigned int)i + 1, (unsigned int)n->route[i].type,
				    (unsigned int)n->route[i].message_class, (unsigned int)n->route[i].storage,
				    sms_action_name(n->route[i].action));
			}
			cc.route_count = n->route_count;
			return;
		}
		break;
	case VAYU_SMS_WMS_EVENT_REPORT:
		if (n->report_mt_valid) {
			say("vayu-sms: cc> set event report: new MT messages %s\n", n->report_mt ? "on" : "off");
			return;
		}
		break;
	case VAYU_SMS_WMS_BIND_SUBSCRIPTION:
		if (n->subscription_valid) {
			say("vayu-sms: cc> bind subscription %u\n", (unsigned int)n->subscription);
			return;
		}
		break;
	case VAYU_SMS_WMS_DELETE:
		cc.deletes++;
		break;
	default:
		break;
	}
	if (sms_cc_routine_locked(n->direction, n->message)) {
		say("vayu-sms: cc> %s (txn %u)\n", name, (unsigned int)n->transaction);
	}
}

static void
sms_cc_response_locked(const struct vayu_sms_wms_note *n)
{
	char buffer[8];
	const char *name = sms_cc_name(n->message, buffer);
	bool failed = !n->result_valid || n->result != 0;

	if (failed) {
		cc.failures++;
		cc.last_message = n->message;
		cc.last_result = n->result_valid ? n->result : 0xffff;
		cc.last_error = n->result_valid ? n->error : 0xffff;
	}
	switch (n->message) {
	case VAYU_SMS_WMS_RAW_SEND:
		if (!failed) {
			cc.mo_ok++;
			say("vayu-sms: cc< raw send: OK%s%u\n", n->message_id_valid ? ", message id " : "",
			    n->message_id_valid ? (unsigned int)n->message_id : 0);
		} else {
			cc.mo_failed++;
			if (n->gw_cause_valid) {
				cc.last_rp_cause = n->rp_cause;
			}
			say("vayu-sms: cc< raw send: failed, result %u error %u (%s); cause %d, class %d, RP cause %d, "
			    "TP cause %d\n", (unsigned int)cc.last_result, (unsigned int)cc.last_error,
			    sms_error_name((uint16_t)cc.last_error), n->cause_valid ? (int)n->cause : -1,
			    n->error_class_valid ? (int)n->error_class : -1, n->gw_cause_valid ? (int)n->rp_cause : -1,
			    n->gw_cause_valid ? (int)n->tp_cause : -1);
		}
		sms_cc_summary_locked();
		return;
	case VAYU_SMS_WMS_SEND_ACK:
		if (failed) {
			cc.ack_failed++;
			say("vayu-sms: cc< send ack: failed, result %u error %u (%s), failure cause %d\n",
			    (unsigned int)cc.last_result, (unsigned int)cc.last_error,
			    sms_error_name((uint16_t)cc.last_error), n->ack_failure_valid ? (int)n->ack_failure : -1);
		} else {
			say("vayu-sms: cc< send ack: OK\n");
		}
		sms_cc_summary_locked();
		return;
	case VAYU_SMS_WMS_EVENT_REPORT:
		cc.event_report = !failed;
		break;
	case VAYU_SMS_WMS_SET_ROUTES:
		cc.routes_set = !failed;
		break;
	case VAYU_SMS_WMS_GET_READY_STATUS:
		if (n->ready_valid) {
			cc.ready_known = true;
			cc.ready_status = n->ready_status;
			say("vayu-sms: cc< get service ready status: %s\n", vayu_sms_wms_ready_name(n->ready_status));
			if (!failed) {
				return;
			}
		}
		break;
	case VAYU_SMS_WMS_LIST:
		if (!failed && n->list_valid && sms_cc_routine_locked(n->direction, n->message)) {
			say("vayu-sms: cc< list messages: %u stored\n", (unsigned int)n->list_count);
			return;
		}
		break;
	default:
		break;
	}
	if (failed) {
		say("vayu-sms: cc< %s: result %u error %u (%s)\n", name, (unsigned int)cc.last_result,
		    (unsigned int)cc.last_error, sms_error_name((uint16_t)cc.last_error));
	} else if (sms_cc_routine_locked(n->direction, n->message)) {
		say("vayu-sms: cc< %s: OK\n", name);
	}
}

static void
sms_cc_indication_locked(const struct vayu_sms_wms_note *n)
{
	char buffer[8];

	switch (n->message) {
	case VAYU_SMS_WMS_EVENT_REPORT:
		if (n->mt_transfer) {
			cc.mt++;
			say("vayu-sms: cc< MT #%u: transfer route, %s, format %u, %u-byte PDU%s%s\n",
			    (unsigned int)cc.mt, n->ack_indicator == 0 ? "ack owed" : "no ack owed",
			    (unsigned int)n->format, (unsigned int)n->pdu_length,
			    n->sms_on_ims_valid && n->sms_on_ims ? ", over IMS" : "",
			    n->mt_stored ? ", also stored" : "");
			if (n->ack_indicator == 0) {
				struct sms_cc_pending *slot = NULL;

				for (uint32_t i = 0; i < SMS_CC_PENDING && slot == NULL; i++) {
					if (!cc.pending[i].used) {
						slot = &cc.pending[i];
					}
				}
				if (slot == NULL) {
					slot = &cc.pending[0];          /* the oldest is reported lost */
					for (uint32_t i = 1; i < SMS_CC_PENDING; i++) {
						if (cc.pending[i].at_ms < slot->at_ms) {
							slot = &cc.pending[i];
						}
					}
					cc.mt_unacked++;
				}
				slot->used = true;
				slot->number = cc.mt;
				slot->transaction = n->wms_transaction;
				slot->at_ms = now_ms();
				sms_cc_arm_locked();
			}
		} else if (n->mt_stored) {
			cc.mt++;
			cc.mt_stored++;
			say("vayu-sms: cc< MT #%u: stored in %s, index %u (a store route: CommCenter reads it)\n",
			    (unsigned int)cc.mt, n->storage == VAYU_SMS_QMI_STORAGE_UIM ? "UIM" :
			    n->storage == VAYU_SMS_QMI_STORAGE_NV ? "NV" : "?", (unsigned int)n->index);
		} else {
			say("vayu-sms: cc< event report without an MT message (other TLVs only)\n");
		}
		return;
	case VAYU_SMS_WMS_SERVICE_READY:
		if (n->ready_valid) {
			cc.ready_known = true;
			cc.ready_status = n->ready_status;
			say("vayu-sms: cc< service ready: %s\n", vayu_sms_wms_ready_name(n->ready_status));
			return;
		}
		break;
	case VAYU_SMS_WMS_ASYNC_RAW_SEND:
		if (n->status_valid) {
			if (n->status == 0) {
				cc.mo_ok++;
			} else {
				cc.mo_failed++;
			}
			say("vayu-sms: cc< async raw send: status %u; cause %d, RP cause %d\n", (unsigned int)n->status,
			    n->cause_valid ? (int)n->cause : -1, n->gw_cause_valid ? (int)n->rp_cause : -1);
			sms_cc_summary_locked();
			return;
		}
		break;
	case VAYU_SMS_WMS_MEMORY_FULL:
		say("vayu-sms: cc< memory full\n");
		return;
	case VAYU_SMS_WMS_NW_REG:
		if (n->nw_reg_valid) {
			say("vayu-sms: cc< transport NW reg: %u\n", (unsigned int)n->nw_reg);
			return;
		}
		break;
	default:
		break;
	}
	if (sms_cc_routine_locked(n->direction, n->message)) {
		say("vayu-sms: cc< indication %s\n", sms_cc_name(n->message, buffer));
	}
}

void
vayu_sms_wms_observe(uint32_t direction, const void *qmi, size_t length)
{
	struct vayu_sms_wms_note note;

	if (!cc.on) {
		return;
	}
	if (vayu_sms_wms_parse(direction == VAYU_SMS_WMS_MODEM_TO_AP ? VAYU_SMS_WMS_MODEM_TO_AP :
	    VAYU_SMS_WMS_AP_TO_MODEM, qmi, length, &note) != 0) {
		return;
	}
	lck_mtx_lock(&vayu_sms_cc_lock);
	cc.frames++;
	if (note.direction == VAYU_SMS_WMS_AP_TO_MODEM && note.qmi_type == VAYU_SMS_WMS_REQUEST) {
		sms_cc_request_locked(&note);
	} else if (note.direction == VAYU_SMS_WMS_MODEM_TO_AP && note.qmi_type == VAYU_SMS_WMS_RESPONSE) {
		sms_cc_response_locked(&note);
	} else if (note.direction == VAYU_SMS_WMS_MODEM_TO_AP && note.qmi_type == VAYU_SMS_WMS_INDICATION) {
		sms_cc_indication_locked(&note);
	}
	lck_mtx_unlock(&vayu_sms_cc_lock);
}

static void
sms_cc_start(void)
{
	lck_mtx_lock(&vayu_sms_cc_lock);
	if (!cc.on) {
		thread_call_t call = cc.ack_call;

		memset(&cc, 0, sizeof(cc));
		cc.ack_call = call != NULL ? call : thread_call_allocate(sms_cc_ack_check, NULL);
		cc.last_result = cc.last_error = cc.last_rp_cause = 0xffff;
		cc.on = true;
	}
	lck_mtx_unlock(&vayu_sms_cc_lock);
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

/* Send one job to the worker and wait for its answer.  A Raw Send waits up to
 * vayu_sms_send_timeout (120 s at most); a refresh that has to sweep 30 slots may
 * take a minute. */
static int
sms_submit(struct sms_job *job)
{
	struct timespec step = { .tv_sec = 5, .tv_nsec = 0 };
	struct timespec limit = { .tv_sec = 150, .tv_nsec = 0 };
	int result;

	lck_mtx_lock(&vayu_sms_lock);
	if (!sms.wms_there || !sms.running || sms.state == VAYU_SMS_STATE_OFF) {
		lck_mtx_unlock(&vayu_sms_lock);
		return ENOTCONN;
	}
	while ((sms.job_busy || sms.job_waiting) && !sms.stop) {
		if (msleep(&sms.job_done, &vayu_sms_lock, PRIBIO, "vayu-sms-queue", &step) == EWOULDBLOCK) {
			break;
		}
	}
	if (sms.stop) {
		lck_mtx_unlock(&vayu_sms_lock);
		return ECANCELED;
	}
	if (sms.job_busy || sms.job_waiting) {
		lck_mtx_unlock(&vayu_sms_lock);
		return EBUSY;
	}
	sms.job = *job;
	sms.job_pending = true;
	sms.job_busy = true;
	sms.job_waiting = true;
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
	if (!sms.job_busy) {
		/* (copied before the next job may start: the shared fields were read after the unlock, and a second
		 * caller's job could finish in between and hand this one its message id -- R3.10) */
		job->message_id = sms.job_message_id;
		job->mr = sms.job_mr;
		job->pdu_length = sms.job_pdu_length;
	}
	sms.job_waiting = false;
	wakeup(&sms.job_done);
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
		send->message_id = job.message_id;
		send->message_reference = job.mr;
		send->pdu_length = job.pdu_length;
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
	*(uint32_t *)data = job.pdu_length;
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
	if (sms.observer) {
		/* The same record, read as CommCenter's traffic: see sms status in userland/src/sms.c. */
		lck_mtx_lock(&vayu_sms_cc_lock);
		status->wms_present = cc.frames != 0 ? 1 : 0;
		status->event_report = cc.event_report ? 1 : 0;
		status->routes_known = cc.ready_known ? 1 : 0;
		status->routes_written = cc.routes_set ? 1 : 0;
		status->route_count = cc.route_count;
		status->received = cc.mt;
		status->sent = cc.mo_ok;
		status->deleted = cc.deletes;
		status->dropped = cc.mt_unacked;
		status->requests = cc.frames;
		status->failures = cc.failures;
		status->last_result = cc.last_result;
		status->last_error = cc.last_error;
		status->last_message = cc.last_message;
		status->last_gsm_cause = cc.last_rp_cause;
		status->last_failure = cc.ready_known ? cc.ready_status : 0xffffffffu;
		lck_mtx_unlock(&vayu_sms_cc_lock);
	}
	return 0;
}

static int
sms_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flag, __unused struct proc *p)
{
	if (sms.observer && (command == VAYU_SMS_IOC_SEND || command == VAYU_SMS_IOC_DELETE ||
	    command == VAYU_SMS_IOC_REFRESH)) {
		return ENOTSUP;                 /* CommCenter owns WMS: Messages sends and deletes */
	}
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

	int mode = sms.strict ? 0600 : 0666;

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, mode, SMS_DEVICE) == NULL) {
		say("vayu-sms: /dev/%s cannot be made\n", SMS_DEVICE);
		return false;
	}
	say("vayu-sms: /dev/%s (0%o) is made: the count record is %u bytes\n", SMS_DEVICE, mode,
	    (unsigned int)sizeof(struct vayu_sms_count));
	return true;
}

/* ---- start and stop ------------------------------------------------------------------------------ */

int
vayu_sms_start(struct vayu_qrtr *q)
{
	thread_t thread;
	uint32_t level = 0, wait_s = 30, routes_arg = 0, store = VAYU_SMS_QMI_STORAGE_UIM, refresh_s = 60;
	uint32_t scan_max = 20, smsc_prefix = 0, list_mode = 0, ims_tlv = 0x02, send_format = VAYU_SMS_QMI_FORMAT_GSM_P2P;
	uint32_t send_timeout_s = SMS_SEND_REPLY_MS / 1000;

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
	{
		uint32_t qmux = 0, coexist = 0, strict = 0;

		PE_parse_boot_argn("vayu_qmux", &qmux, sizeof(qmux));
		PE_parse_boot_argn("vayu_sms_coexist", &coexist, sizeof(coexist));
		PE_parse_boot_argn("vayu_sms_strict", &strict, sizeof(strict));
		sms.strict = strict == 1;
		if ((level == 1 || level == 2) && qmux != 0 && coexist != 1) {
			say("vayu-sms: vayu_sms=%u with vayu_qmux=%u: CommCenter is the WMS client through the bridge, so "
			    "the kernel's is not started (taken as vayu_sms=3; vayu_sms_coexist=1 keeps both)\n", level, qmux);
			level = 3;
		}
		if (level == 3) {
			sms.level = level;
			sms.observer = true;
			sms.state = VAYU_SMS_STATE_COMMCENTER;
			sms_cc_start();
			say("vayu-sms: vayu_sms=3: CommCenter owns WMS 0x05; the kernel starts no client (no event report, "
			    "route, ack, delete or inbox) and logs the bridge's WMS frames without content%s\n",
			    qmux != 0 ? "" : " -- but vayu_qmux=0: no bridge, nothing will pass");
			(void)sms_make_node();
			sms.started = true;
			return 0;
		}
		if (level > 3) {
			say("vayu-sms: vayu_sms=%u is refused: 0, 1, 2 or 3\n", level);
			return 0;
		}
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
	PE_parse_boot_argn("vayu_sms_smsc", &smsc_prefix, sizeof(smsc_prefix));
	if (smsc_prefix != 0 && smsc_prefix != 1) {
		say("vayu-sms: vayu_sms_smsc=%u is refused: 0 (TPDU) and 1 (full PDU) only\n", smsc_prefix);
		smsc_prefix = 0;
	}
	PE_parse_boot_argn("vayu_sms_list", &list_mode, sizeof(list_mode));
	if (list_mode > 2) {
		say("vayu-sms: vayu_sms_list=%u is refused: 0 (tag 1 then 0), 1 (tag 1), 2 (tag 0)\n", list_mode);
		list_mode = 0;
	}
	PE_parse_boot_argn("vayu_sms_ims_tlv", &ims_tlv, sizeof(ims_tlv));
	if (ims_tlv != 0 && ims_tlv != 0x02 && ims_tlv != 0x13) {
		say("vayu-sms: vayu_sms_ims_tlv=%u is refused: 2 (default), 19 (0x13, k25) and 0 (omit)\n", ims_tlv);
		ims_tlv = 0x02;
	}
	PE_parse_boot_argn("vayu_sms_format", &send_format, sizeof(send_format));
	if (send_format == 0 || send_format > 0x08) {
		say("vayu-sms: vayu_sms_format=%u is refused: 1..8 (6 is GSM point-to-point)\n", send_format);
		send_format = VAYU_SMS_QMI_FORMAT_GSM_P2P;
	}
	PE_parse_boot_argn("vayu_sms_send_timeout", &send_timeout_s, sizeof(send_timeout_s));
	if (send_timeout_s < 5) {
		send_timeout_s = 5;
	}
	if (send_timeout_s > 120) {
		send_timeout_s = 120;
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
	sms.smsc_prefix = smsc_prefix;
	sms.list_mode = list_mode;
	sms.ims_tlv = ims_tlv;
	sms.send_format = send_format;
	sms.send_timeout_s = send_timeout_s;
	sms.list_last_tag = 0xffffffff;
	sms.list_last_result = 0xffffffff;
	sms.list_last_error = 0xffffffff;
	sms.list_last_id = 0;
	sms.list_ok_tag = 0xffff;
	sms.last_read_error = 0xffff;
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
	sms.job_pending = sms.job_busy = sms.job_done = sms.job_waiting = false;
	sms.job.kind = SMS_JOB_NONE;
	sms.event_head = sms.event_tail = 0;
	sms.work = false;
	sms.next_refresh_ms = 0;
	sms.next_config_ms = now_ms() + 20000;
	say("vayu-sms: vayu_sms=%u vayu_sms_wait=%u vayu_sms_routes=%u vayu_sms_store=%u vayu_sms_refresh=%u "
	    "vayu_sms_scan=%u vayu_sms_smsc=%u vayu_sms_list=%u vayu_sms_ims_tlv=%u vayu_sms_format=%u "
	    "vayu_sms_send_timeout=%u\n", level, wait_s, routes_arg, store, refresh_s, scan_max,
	    smsc_prefix, list_mode, ims_tlv, send_format, send_timeout_s);
	say("vayu-sms: WMS 0x05 (routes, event report, list, raw read, raw send, delete, ack); "
	    "the log masks numbers and never prints text\n");
	if (level >= 2) {
		say("vayu-sms: vayu_sms=%u: every List/Raw Read sweep is dumped, not only the first\n", level);
	}
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
	if (sms.observer) {
		/* nothing of ours runs; the frames stop when the bridge stops */
		lck_mtx_lock(&vayu_sms_cc_lock);
		sms_cc_summary_locked();
		lck_mtx_unlock(&vayu_sms_cc_lock);
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
