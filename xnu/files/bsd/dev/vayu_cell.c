/*
 * Cellular, the first real look at the SIM and the network of the POCO X3 Pro (vayu, SM8150-AC,
 * X24): a QMI client of the modem's UIM and NAS services, over QRTR on the modem's GLINK channel
 * (the transport vayu_pas.c gives the QMI helpers of hakios.qmi.c). It is read-only, and it is
 * off unless boot-arg vayu_cell=N asks for it.
 *
 * What it asks, and why only that:
 *   UIM 0x0b  Get Card Status (0x002f)         is there a card, is an application ready, is a PIN
 *                                              wanted (asked only, never sent)
 *   UIM 0x0b  Read Record (0x0021)             EF_ICCID (0x2fe2) and EF_IMSI (0x6f07), as the
 *   UIM 0x0b  Read Transparent (0x0020)        messages were specified; a transparent file is
 *                                              not read by Read Record, so the record read that
 *                                              the card refuses is followed by a transparent one
 *   NAS 0x03  Get Serving System (0x0024)      registration, PLMN and its name, radio, LAC/CID
 *   NAS 0x03  Get Signal Info (0x004f)         LTE RSRP/RSRQ/RSSI/SNR in dB(m), or 5G's, or GSM's
 *   NAS 0x03  Initiate Network Register (0x0022)  ONLY with vayu_cell_register=1: asking the
 *                                              modem to register may make it transmit, so it is
 *                                              never done by default
 *
 * What it never does: DMS (0x02) is not even looked up -- its useful messages are identifiers
 * (Get IDs, IMEI) and no identifier is needed here; no UIM write, no Verify/Unblock PIN, no
 * storage write, no WDS (there is no data call), no power or mode change. Every byte a reply
 * carries that is an identifier is dropped before anything is logged: only the number of digits
 * and a masked tail ("...1234") of the ICCID and IMSI are kept, in the log and in the record.
 *
 * The answer, for the iOS userland shim: /dev/vayu-cell, a fixed record of 152 bytes
 * (sys/vayu_cell.h has every field and its offset). One is always readable; seq is 0 until the
 * first round has filled it, and rises by one per round. The node is made when vayu_cell is not 0,
 * and it stays after vayu_cell_stop(): what was last learned is still there.
 *
 * Boot-args:
 *   vayu_cell=0        nothing at all (the default)
 *   vayu_cell=1        one round after the UIM/NAS services appear, then the QMI handle is let go
 *   vayu_cell=2        one round, and another every vayu_cell_poll seconds
 *   vayu_cell_wait=S   how long to wait for UIM and NAS (default 20, 1..120)
 *   vayu_cell_poll=S   seconds between rounds at vayu_cell=2 (default 30, 5..3600)
 *   vayu_cell_register=1  may send NAS Initiate Network Register (automatic); 0, or anything but
 *                      exactly 1, refuses it
 *
 * Facts behind the numbers: the QMI service ids (UIM 0x0b, NAS 0x03, DMS 0x02), the message ids
 * and the TLV layouts are libqmi's data/qmi-service-uim.json and qmi-service-nas.json (master,
 * read 2026-10-25; sys/vayu_cell_qmi.h names each). The card's and network's enum values are
 * libqmi's qmi-enums-uim.h and qmi-enums-nas.h. The file ids: EF_ICCID is 2fe2 under the MF
 * (3f00), EF_IMSI is 6f07 under the USIM's application file, which the implicit fid 7fff names
 * (TS 31.102). The BCD order of a UIM file is TS 51.011's: the low nibble of a byte is the first
 * digit, and 0xf is padding or the IMSI's parity nibble.
 *
 * What is NOT known until it runs on the phone (HYPOTHESIS): that this modem's UIM service reads
 * EF_IMSI through a provisioning session with an empty AID (that is what libqmi's qmicli does);
 * that its Get Signal Info has the LTE TLV; that its PLMN description is a string this long; and
 * that its Read Record refuses a transparent file the way the specification says. Each of those
 * fails visibly, one log line, and the record says what was not learned.
 */
#include <kern/clock.h>
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
#include <sys/vayu_cell.h>
#include <sys/vayu_cell_qmi.h>
#include <sys/vayu_pas.h>

extern void loader_log_putc(char c);

#define VAYU_CELL_DEVICE        "vayu-cell"
#define VAYU_CELL_HANDLE_MAX    4096    /* the largest datagram the QMI handle takes */
#define VAYU_CELL_REPLY_MS      5000    /* one request's reply: longer than a card read takes */
#define VAYU_CELL_STOP_MS       8000    /* the longest wait for the thread to end (one reply) */
#define VAYU_CELL_DIGITS        32      /* an IMSI is 15 digits, an ICCID 20 */

/* ---- state --------------------------------------------------------------------------------------- */

struct vayu_cell_state {
	struct qmi_handle qmi;
	bool qmi_up;
	volatile bool uim_there, nas_there;
	uint32_t uim_node, uim_port, uim_version, uim_instance;
	uint32_t nas_node, nas_port, nas_version, nas_instance;
	volatile bool stop, running;
	bool started;
	uint32_t level, wait_s, poll_s, register_arg;
	bool register_sent;
	uint32_t rounds, requests, failures;
	uint32_t seq;
	struct vayu_cell_record record;
	/* the answers, one at a time: the thread is their only user */
	struct vayu_cell_uim_get_card_status_resp card_status;
	struct vayu_cell_uim_read_resp read;
	struct vayu_cell_nas_get_serving_system_resp serving;
	struct vayu_cell_nas_get_signal_info_resp signal;
};

static struct vayu_cell_state cell;

static LCK_GRP_DECLARE(vayu_cell_lck_grp, "vayu-cell");
static LCK_MTX_DECLARE(vayu_cell_lock, &vayu_cell_lck_grp);

/* Every line to the loader's log as well: what was done to the modem should be known even if the kernel dies. */
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
cell_new_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (service->service == VAYU_CELL_UIM_SERVICE) {
		cell.uim_node = service->node;
		cell.uim_port = service->port;
		cell.uim_version = service->version;
		cell.uim_instance = service->instance;
		cell.uim_there = true;
	} else if (service->service == VAYU_CELL_NAS_SERVICE) {
		cell.nas_node = service->node;
		cell.nas_port = service->port;
		cell.nas_version = service->version;
		cell.nas_instance = service->instance;
		cell.nas_there = true;
	} else {
		return -1;                      /* not one we asked for: the helpers forget it again */
	}
	return 0;
}

static void
cell_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (service->service == VAYU_CELL_UIM_SERVICE && cell.uim_node == service->node && cell.uim_port == service->port) {
		cell.uim_there = false;
	}
	if (service->service == VAYU_CELL_NAS_SERVICE && cell.nas_node == service->node && cell.nas_port == service->port) {
		cell.nas_there = false;
	}
}

static const struct qmi_ops cell_qmi_ops = {
	.new_server = cell_new_server,
	.del_server = cell_del_server,
};

/*
 * One request and its reply, waited VAYU_CELL_REPLY_MS. The QMI result is checked here: 0 only if
 * the modem answered success. `result` is the reply's first TLV (0x02) of the caller's structure.
 * A negative return is either the transport's or the modem's refusal; both are in the log.
 */
static int
cell_request(const char *what, uint32_t node, uint32_t port, uint16_t message,
    const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, struct vayu_cell_result *result)
{
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = node, .sq_port = port };
	struct qmi_txn txn;
	int ret;

	if (cell.stop) {
		return -ECANCELED;
	}
	cell.requests++;
	ret = qmi_txn_init(&cell.qmi, &txn, resp_ei, resp);
	if (ret < 0) {
		say("vayu-cell: %s: no transaction: %d\n", what, ret);
		cell.failures++;
		return ret;
	}
	ret = qmi_send_request(&cell.qmi, &to, &txn, message, VAYU_CELL_REQUEST_MAX, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-cell: %s: cannot send: %d\n", what, ret);
		cell.failures++;
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, VAYU_CELL_REPLY_MS);
	if (ret < 0) {
		if (ret == -ETIMEDOUT) {
			say("vayu-cell: %s: no reply in %u ms\n", what, VAYU_CELL_REPLY_MS);
		} else {
			say("vayu-cell: %s: the reply cannot be decoded (%d)\n", what, ret);
		}
		cell.failures++;
		return ret;
	}
	if (result->result != QMI_RESULT_SUCCESS_V01 || result->error != QMI_ERR_NONE_V01) {
		say("vayu-cell: %s: the modem says result %u error %u\n", what, (unsigned int)result->result,
		    (unsigned int)result->error);
		cell.failures++;
		return -EIO;
	}
	return 0;
}

/* ---- the SIM ------------------------------------------------------------------------------------- */

static const struct vayu_cell_uim_card *
cell_card(const struct vayu_cell_uim_card_status *status)
{
	uint32_t count = status->cards_len < VAYU_CELL_CARDS ? status->cards_len : VAYU_CELL_CARDS;

	for (uint32_t i = 0; i < count; i++) {
		if (status->cards[i].card_state == VAYU_CELL_CARD_PRESENT) {
			return &status->cards[i];
		}
	}
	return count != 0 ? &status->cards[0] : NULL;
}

/* The USIM's application if there is one, else the SIM's, else the first the card lists. */
static const struct vayu_cell_uim_app *
cell_app(const struct vayu_cell_uim_card *card)
{
	uint32_t count = card->app_len < VAYU_CELL_APPLICATIONS ? card->app_len : VAYU_CELL_APPLICATIONS;
	const struct vayu_cell_uim_app *sim = NULL;

	for (uint32_t i = 0; i < count; i++) {
		if (card->app[i].type == VAYU_CELL_UIM_APP_TYPE_USIM) {
			return &card->app[i];
		}
		if (card->app[i].type == VAYU_CELL_UIM_APP_TYPE_SIM && sim == NULL) {
			sim = &card->app[i];
		}
	}
	return sim != NULL ? sim : count != 0 ? &card->app[0] : NULL;
}

/* The PIN the card is waiting for: a USIM's UPIN if it replaces PIN1, else the application's PIN1. */
static uint8_t
cell_pin_state(const struct vayu_cell_uim_card *card, const struct vayu_cell_uim_app *app)
{
	if (app == NULL) {
		return card->upin_state;
	}
	if (app->upin_replaces_pin1 != 0) {
		return card->upin_state;
	}
	return app->pin1_state != 0 ? app->pin1_state : card->upin_state;
}

static uint8_t
cell_pin_retries(const struct vayu_cell_uim_card *card, const struct vayu_cell_uim_app *app)
{
	if (app == NULL) {
		return card->upin_retries;
	}
	if (app->upin_replaces_pin1 != 0) {
		return card->upin_retries;
	}
	return app->pin1_state != 0 ? app->pin1_retries : card->upin_retries;
}

/*
 * What the card is doing, from the card's state and its application's. A PIN that is enabled but
 * not verified is "PIN required": this client asks for nothing and sends no PIN, so the record
 * saying so is the whole of what it can do about it.
 */
static uint8_t
cell_sim_state(const struct vayu_cell_uim_card *card, const struct vayu_cell_uim_app *app, uint8_t pin)
{
	if (card->card_state == VAYU_CELL_CARD_ABSENT) {
		return VAYU_CELL_SIM_ABSENT;
	}
	if (card->card_state == VAYU_CELL_CARD_ERROR) {
		return VAYU_CELL_SIM_ERROR;
	}
	if (card->card_state != VAYU_CELL_CARD_PRESENT) {
		return VAYU_CELL_SIM_UNKNOWN;
	}
	if (app != NULL && (app->state == VAYU_CELL_UIM_APP_PIN1_REQUIRED || app->state == VAYU_CELL_UIM_APP_PUK1_REQUIRED ||
	    app->state == VAYU_CELL_UIM_APP_PIN1_BLOCKED)) {
		return VAYU_CELL_SIM_PIN_REQUIRED;
	}
	if (pin == VAYU_CELL_PIN_ENABLED_NOT_VERIFIED || pin == VAYU_CELL_PIN_BLOCKED ||
	    pin == VAYU_CELL_PIN_PERMANENTLY_BLOCKED) {
		return VAYU_CELL_SIM_PIN_REQUIRED;
	}
	if (app != NULL && app->state == VAYU_CELL_UIM_APP_READY) {
		return VAYU_CELL_SIM_READY;
	}
	return VAYU_CELL_SIM_PRESENT;
}

static const char *
cell_sim_name(uint8_t state)
{
	switch (state) {
	case VAYU_CELL_SIM_ABSENT: return "ABSENT";
	case VAYU_CELL_SIM_PRESENT: return "PRESENT";
	case VAYU_CELL_SIM_PIN_REQUIRED: return "PIN REQUIRED (never sent)";
	case VAYU_CELL_SIM_READY: return "READY";
	case VAYU_CELL_SIM_ERROR: return "ERROR";
	default: return "UNKNOWN";
	}
}

static const char *
cell_app_name(uint8_t type)
{
	switch (type) {
	case VAYU_CELL_UIM_APP_TYPE_SIM: return "SIM";
	case VAYU_CELL_UIM_APP_TYPE_USIM: return "USIM";
	case VAYU_CELL_UIM_APP_TYPE_RUIM: return "RUIM";
	case VAYU_CELL_UIM_APP_TYPE_CSIM: return "CSIM";
	case VAYU_CELL_UIM_APP_TYPE_ISIM: return "ISIM";
	default: return "?";
	}
}

/*
 * One transparent file of the card, by the two messages there are for it: Read Record first (the
 * message this client was specified around), and when the card refuses it -- a transparent file
 * has no records -- Read Transparent. On success *data_length is the bytes and true is returned.
 * Nothing is logged of the bytes.
 */
static bool
cell_read_file(const char *name, uint16_t file_id, const uint8_t *path, uint32_t path_length,
    uint8_t *data, uint32_t *data_length)
{
	struct vayu_cell_uim_read_record_req record = { 0 };
	struct vayu_cell_uim_read_transparent_req transparent = { 0 };
	const char *how = "record";
	int ret;

	if (cell.stop) {
		return false;
	}
	record.session.session_type = VAYU_CELL_UIM_SESSION_PRIMARY_GW;
	record.session.aid_len = 0;             /* a provisioning session's AID is ignored */
	record.file.file_id = file_id;
	record.file.path_len = path_length;
	memcpy(record.file.path, path, path_length);
	record.record.record_number = 0;
	record.record.record_length = 0;        /* (libqmi's qmicli passes 0, 0 too) */
	memset(&cell.read, 0, sizeof(cell.read));
	ret = cell_request(name, cell.uim_node, cell.uim_port, VAYU_CELL_UIM_READ_RECORD,
	    vayu_cell_uim_read_record_req_ei, &record, vayu_cell_uim_read_resp_ei, &cell.read, &cell.read.result);
	if (ret != 0 || !cell.read.read_valid || cell.read.read_len == 0) {
		if (ret == 0) {
			say("vayu-cell: %s: the record read has no data\n", name);
		}
		if (cell.stop) {
			return false;
		}
		how = "transparent";
		transparent.session.session_type = VAYU_CELL_UIM_SESSION_PRIMARY_GW;
		transparent.session.aid_len = 0;
		transparent.file.file_id = file_id;
		transparent.file.path_len = path_length;
		memcpy(transparent.file.path, path, path_length);
		transparent.read.offset = 0;
		transparent.read.length = 0;    /* to the end of the file */
		memset(&cell.read, 0, sizeof(cell.read));
		ret = cell_request(name, cell.uim_node, cell.uim_port, VAYU_CELL_UIM_READ_TRANSPARENT,
		    vayu_cell_uim_read_transparent_req_ei, &transparent, vayu_cell_uim_read_resp_ei, &cell.read,
		    &cell.read.result);
		if (ret != 0 || !cell.read.read_valid || cell.read.read_len == 0) {
			say("vayu-cell: %s: not read (record and transparent)\n", name);
			return false;
		}
	}
	if (cell.read.card_result_valid && !(cell.read.card_result.sw1 == 0x90 && cell.read.card_result.sw2 == 0x00)) {
		say("vayu-cell: %s: the card answers SW %02x%02x\n", name, cell.read.card_result.sw1, cell.read.card_result.sw2);
	}
	*data_length = cell.read.read_len;
	memcpy(data, cell.read.read, *data_length);
	say("vayu-cell: %s: %u bytes, read %s\n", name, (unsigned int)*data_length, how);
	return true;
}

/* The card's state, then its ICCID and IMSI (masked; only their digit counts are kept whole). */
static void
cell_uim_round(struct vayu_cell_record *record)
{
	char digits[VAYU_CELL_DIGITS];
	uint8_t data[VAYU_CELL_FILE_DATA];
	uint32_t length;
	const struct vayu_cell_uim_card *card;
	const struct vayu_cell_uim_app *app;
	uint8_t pin;
	/* EF_ICCID under the MF (3f00): the path's ids, two little-endian bytes each. */
	static const uint8_t iccid_path[] = { 0x00, 0x3f };
	/* EF_IMSI under the ADF USIM, which the implicit fid 7fff names: 3f00, 7fff. */
	static const uint8_t imsi_path[] = { 0x00, 0x3f, 0xff, 0x7f };
	int ret;

	record->card_state = 0xff;
	record->app_state = 0xff;
	record->pin_state = 0xff;
	if (!cell.uim_there) {
		say("vayu-cell: UIM is not there: no SIM state\n");
		return;
	}
	memset(&cell.card_status, 0, sizeof(cell.card_status));
	ret = cell_request("card status", cell.uim_node, cell.uim_port, VAYU_CELL_UIM_GET_CARD_STATUS, NULL, NULL,
	    vayu_cell_uim_get_card_status_resp_ei, &cell.card_status, &cell.card_status.result);
	if (ret != 0) {
		record->sim_state = VAYU_CELL_SIM_UNKNOWN;
		return;
	}
	if (!cell.card_status.card_status_valid) {
		say("vayu-cell: card status: the reply has no card status TLV\n");
		record->sim_state = VAYU_CELL_SIM_UNKNOWN;
		return;
	}
	record->flags |= VAYU_CELL_FLAG_CARD_STATUS;
	card = cell_card(&cell.card_status.card_status);
	if (card == NULL) {
		say("vayu-cell: card status: no card slot in the reply\n");
		record->sim_state = VAYU_CELL_SIM_ABSENT;
		return;
	}
	record->card_state = card->card_state;
	app = cell_app(card);
	if (app != NULL) {
		record->app_state = app->state;
	}
	pin = cell_pin_state(card, app);
	record->pin_state = pin;
	record->pin_retries = cell_pin_retries(card, app);
	record->sim_state = cell_sim_state(card, app, pin);
	say("vayu-cell: SIM: %s; card state %u, %u application(s)%s%s, PIN state %u with %u attempt(s) left%s\n",
	    cell_sim_name(record->sim_state), (unsigned int)card->card_state, (unsigned int)card->app_len,
	    app != NULL ? ", " : "", app != NULL ? cell_app_name(app->type) : "",
	    (unsigned int)pin, (unsigned int)record->pin_retries,
	    record->sim_state == VAYU_CELL_SIM_PIN_REQUIRED ? " (no PIN is ever sent)" : "");
	if (card->card_state != VAYU_CELL_CARD_PRESENT) {
		return;
	}

	if (cell_read_file("ICCID", VAYU_CELL_UIM_EF_ICCID, iccid_path, sizeof(iccid_path), data, &length) &&
	    (length = vayu_cell_uim_digits(data, length, false, digits, sizeof(digits))) != 0) {
		vayu_cell_mask_tail(digits, length, record->iccid_tail);
		record->iccid_length = length;
		record->flags |= VAYU_CELL_FLAG_ICCID;
		say("vayu-cell: ICCID: %u digits, tail \"%s\"\n", (unsigned int)length, record->iccid_tail);
	}
	if (cell_read_file("IMSI", VAYU_CELL_UIM_EF_IMSI, imsi_path, sizeof(imsi_path), data, &length) &&
	    (length = vayu_cell_uim_digits(data, length, true, digits, sizeof(digits))) != 0) {
		vayu_cell_mask_tail(digits, length, record->imsi_tail);
		record->imsi_length = length;
		record->flags |= VAYU_CELL_FLAG_IMSI;
		say("vayu-cell: IMSI: %u digits, tail \"%s\"\n", (unsigned int)length, record->imsi_tail);
	}
}

/* ---- the network ---------------------------------------------------------------------------------- */

/* The radio interface of the serving system: LTE if it is one of them, else 5G NR, else the first. */
static uint8_t
cell_radio(const struct vayu_cell_nas_serving_system *serving)
{
	uint32_t count = serving->radio_len < VAYU_CELL_RADIOS ? serving->radio_len : VAYU_CELL_RADIOS;
	uint8_t first = 0xff;

	for (uint32_t i = 0; i < count; i++) {
		if (serving->radio[i] == VAYU_CELL_NAS_RADIO_LTE) {
			return VAYU_CELL_NAS_RADIO_LTE;
		}
		if (serving->radio[i] == VAYU_CELL_NAS_RADIO_NR5G) {
			first = VAYU_CELL_NAS_RADIO_NR5G;
		} else if (first == 0xff) {
			first = serving->radio[i];
		}
	}
	return first;
}

static const char *
cell_radio_name(uint8_t radio)
{
	switch (radio) {
	case VAYU_CELL_NAS_RADIO_GSM: return "GSM";
	case VAYU_CELL_NAS_RADIO_UMTS: return "UMTS";
	case VAYU_CELL_NAS_RADIO_LTE: return "LTE";
	case VAYU_CELL_NAS_RADIO_NR5G: return "5G NR";
	case VAYU_CELL_NAS_RADIO_NONE: return "none";
	default: return "?";
	}
}

static const char *
cell_reg_name(uint8_t state)
{
	switch (state) {
	case VAYU_CELL_REG_NOT_REGISTERED: return "NOT REGISTERED";
	case VAYU_CELL_REG_REGISTERED: return "REGISTERED";
	case VAYU_CELL_REG_SEARCHING: return "SEARCHING";
	case VAYU_CELL_REG_DENIED: return "DENIED";
	case VAYU_CELL_REG_QMI_UNKNOWN: return "UNKNOWN";
	default: return "NO ANSWER";
	}
}

/*
 * A value the specification's range does not hold is not a measurement: the modem's "not there"
 * (0x8000 for a 16-bit one) and a wrong one are left out of the record rather than shown as bars.
 * The ranges: RSRP -140..-44 dBm for LTE (3GPP 36.133; ModemManager's RSRP_MIN/MAX) and -156..-44
 * for 5G NR, RSSI -120..-20 dBm, RSRQ -30..5 dB, SNR -20..50 dB.
 */
static bool
cell_in_range(int value, int lowest, int highest)
{
	return value >= lowest && value <= highest;
}

/*
 * Registration and the operator: Get Serving System's TLVs. Then the signal, Get Signal Info's:
 * RSRP is dBm as it comes (QMI NAS's LTE RSRP is -140..-44 dBm), RSRQ is dB, SNR is 0.1 dB.
 */
static void
cell_nas_round(struct vayu_cell_record *record)
{
	const struct vayu_cell_nas_serving_system *serving;
	uint8_t radio;
	int ret;

	if (!cell.nas_there) {
		say("vayu-cell: NAS is not there: no network state\n");
		return;
	}
	if (cell.register_arg == 1 && !cell.register_sent) {
		struct vayu_cell_nas_initiate_register_req request = { .action = VAYU_CELL_NAS_REGISTER_AUTOMATIC };
		struct vayu_cell_nas_register_resp response = { 0 };

		cell.register_sent = true;
		say("vayu-cell: vayu_cell_register=1: NAS Initiate Network Register (automatic) is sent; "
		    "the modem may transmit because of it\n");
		ret = cell_request("initiate register", cell.nas_node, cell.nas_port, VAYU_CELL_NAS_INITIATE_NETWORK_REGISTER,
		    vayu_cell_nas_initiate_register_req_ei, &request, vayu_cell_nas_register_resp_ei, &response,
		    &response.result);
		say("vayu-cell: initiate register: %s\n", ret == 0 ? "the modem agreed" : "refused (see above)");
	}
	if (cell.stop) {
		return;
	}
	memset(&cell.serving, 0, sizeof(cell.serving));
	ret = cell_request("serving system", cell.nas_node, cell.nas_port, VAYU_CELL_NAS_GET_SERVING_SYSTEM, NULL, NULL,
	    vayu_cell_nas_get_serving_system_resp_ei, &cell.serving, &cell.serving.result);
	if (ret == 0 && cell.serving.serving_valid) {
		serving = &cell.serving.serving;
		record->flags |= VAYU_CELL_FLAG_SERVING;
		record->reg_state = serving->registration_state <= VAYU_CELL_REG_QMI_UNKNOWN ?
		    serving->registration_state : VAYU_CELL_REG_UNKNOWN;
		radio = cell_radio(serving);
		record->radio = radio;
		if (cell.serving.plmn_valid) {
			record->mcc = cell.serving.plmn.mcc;
			record->mnc = cell.serving.plmn.mnc;
			cell.serving.plmn.description[VAYU_CELL_OPERATOR - 1] = 0;
			if (cell.serving.plmn.description[0] != 0) {
				strlcpy(record->operator_name, cell.serving.plmn.description, sizeof(record->operator_name));
				record->flags |= VAYU_CELL_FLAG_OPERATOR;
			}
		}
		if (cell.serving.lac_valid) {
			record->lac = cell.serving.lac;
		}
		if (cell.serving.cid_valid) {
			record->cid = cell.serving.cid;
		}
		if (cell.serving.lac_valid && cell.serving.cid_valid) {
			record->flags |= VAYU_CELL_FLAG_LAC_CID;
		}
		if (cell.serving.roam_valid) {
			/* QmiNasRoamingIndicatorStatus: ON is 0, OFF is 1. */
			record->roam = cell.serving.roam == VAYU_CELL_NAS_ROAM_ON ? 1 :
			    cell.serving.roam == VAYU_CELL_NAS_ROAM_OFF ? 0 : 0xff;
		}
		say("vayu-cell: network: %s (%u), radio %s (%u), MCC %u MNC %u%s%s, LAC %#x CID %#x, roaming %s\n",
		    cell_reg_name(record->reg_state), (unsigned int)record->reg_state, cell_radio_name(radio),
		    (unsigned int)radio, (unsigned int)record->mcc, (unsigned int)record->mnc,
		    record->flags & VAYU_CELL_FLAG_OPERATOR ? " " : "",
		    record->flags & VAYU_CELL_FLAG_OPERATOR ? record->operator_name : "",
		    (unsigned int)record->lac, (unsigned int)record->cid,
		    record->roam == 0 ? "no" : record->roam == 1 ? "YES" : "unknown");
		if (cell.serving.detailed_valid && cell.serving.detailed.forbidden != 0) {
			say("vayu-cell: network: the modem says this network is forbidden\n");
		}
	} else if (ret == 0) {
		say("vayu-cell: serving system: the reply has no serving system TLV\n");
	}
	if (cell.stop) {
		return;
	}
	memset(&cell.signal, 0, sizeof(cell.signal));
	ret = cell_request("signal info", cell.nas_node, cell.nas_port, VAYU_CELL_NAS_GET_SIGNAL_INFO, NULL, NULL,
	    vayu_cell_nas_get_signal_info_resp_ei, &cell.signal, &cell.signal.result);
	if (ret != 0) {
		return;
	}
	if (cell.signal.lte_valid && cell.signal.lte.rsrp != (int16_t)0x8000) {
		if (!cell_in_range(cell.signal.lte.rsrp, -140, -44)) {
			say("vayu-cell: signal: LTE RSRP %d dBm is not in -140..-44 dBm: not used\n",
			    (int)cell.signal.lte.rsrp);
		} else {
			record->signal_dbm = cell.signal.lte.rsrp;
			if (cell_in_range((int8_t)cell.signal.lte.rsrq, -30, 5)) {
				record->rsrq_db = (int8_t)cell.signal.lte.rsrq;
			}
			if (cell_in_range(cell.signal.lte.snr, -200, 500)) {
				record->snr_tenth_db = cell.signal.lte.snr;
			}
			record->flags |= VAYU_CELL_FLAG_SIGNAL;
			if (record->snr_tenth_db != INT32_MIN) {
				say("vayu-cell: signal: LTE RSRP %d dBm, RSRQ %d dB, RSSI %d dBm, SNR %d/10 dB\n",
				    (int)record->signal_dbm, (int)record->rsrq_db, (int)(int8_t)cell.signal.lte.rssi,
				    (int)record->snr_tenth_db);
			} else {
				say("vayu-cell: signal: LTE RSRP %d dBm, RSRQ %d dB, RSSI %d dBm\n",
				    (int)record->signal_dbm, (int)record->rsrq_db, (int)(int8_t)cell.signal.lte.rssi);
			}
		}
	}
	if ((record->flags & VAYU_CELL_FLAG_SIGNAL) == 0 && cell.signal.nr5g_valid &&
	    cell.signal.nr5g.rsrp != (int16_t)0x8000) {
		if (!cell_in_range(cell.signal.nr5g.rsrp, -156, -44)) {
			say("vayu-cell: signal: 5G NR RSRP %d dBm is not in -156..-44 dBm: not used\n",
			    (int)cell.signal.nr5g.rsrp);
		} else {
			record->signal_dbm = cell.signal.nr5g.rsrp;
			if (cell_in_range(cell.signal.nr5g.snr, -200, 500)) {
				record->snr_tenth_db = cell.signal.nr5g.snr;
			}
			record->flags |= VAYU_CELL_FLAG_SIGNAL;
			if (record->snr_tenth_db != INT32_MIN) {
				say("vayu-cell: signal: 5G NR RSRP %d dBm, SNR %d/10 dB\n", (int)record->signal_dbm,
				    (int)record->snr_tenth_db);
			} else {
				say("vayu-cell: signal: 5G NR RSRP %d dBm\n", (int)record->signal_dbm);
			}
		}
	}
	if ((record->flags & VAYU_CELL_FLAG_SIGNAL) == 0 && cell.signal.gsm_valid &&
	    cell_in_range((int8_t)cell.signal.gsm_rssi, -120, -20)) {
		record->signal_dbm = (int8_t)cell.signal.gsm_rssi;
		record->flags |= VAYU_CELL_FLAG_SIGNAL;
		say("vayu-cell: signal: GSM RSSI %d dBm\n", (int)record->signal_dbm);
	}
	if ((record->flags & VAYU_CELL_FLAG_SIGNAL) == 0) {
		say("vayu-cell: signal: no LTE, 5G or GSM strength to use in the reply\n");
	}
}

/* ---- one round, and the record ------------------------------------------------------------------- */

static void
cell_record_reset(struct vayu_cell_record *record)
{
	memset(record, 0, sizeof(*record));
	record->magic = VAYU_CELL_MAGIC;
	record->version = VAYU_CELL_VERSION;
	record->size = (uint32_t)sizeof(*record);
	record->sim_state = VAYU_CELL_SIM_UNKNOWN;
	record->card_state = 0xff;
	record->app_state = 0xff;
	record->pin_state = 0xff;
	record->reg_state = VAYU_CELL_REG_UNKNOWN;
	record->radio = 0xff;
	record->roam = 0xff;
	record->signal_dbm = INT32_MIN;
	record->rsrq_db = INT32_MIN;
	record->snr_tenth_db = INT32_MIN;
	record->lac = 0xffffffffu;
	record->cid = 0xffffffffu;
}

/*
 * One round is filled from scratch: a part the modem does not answer this time is not carried
 * over from an earlier round, so the record never claims something the modem has stopped saying.
 */
static void
cell_round(void)
{
	struct vayu_cell_record record;

	cell_record_reset(&record);
	cell_uim_round(&record);
	if (cell.stop) {
		return;
	}
	cell_nas_round(&record);
	if (cell.stop) {
		return;
	}
	record.updated_ms = now_ms();
	cell.rounds++;
	lck_mtx_lock(&vayu_cell_lock);
	cell.seq++;
	record.seq = cell.seq;
	cell.record = record;
	lck_mtx_unlock(&vayu_cell_lock);
	say("vayu-cell: round %u: SIM %s, ICCID %u digits \"%s\", IMSI %u digits \"%s\", network %s, "
	    "signal %d dBm; /dev/%s holds %u bytes\n", (unsigned int)cell.rounds, cell_sim_name(record.sim_state),
	    (unsigned int)record.iccid_length, record.iccid_tail, (unsigned int)record.imsi_length, record.imsi_tail,
	    cell_reg_name(record.reg_state), (int)record.signal_dbm, VAYU_CELL_DEVICE, (unsigned int)record.size);
}

/* ---- /dev/vayu-cell ------------------------------------------------------------------------------ */

static int
cell_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
cell_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct vayu_cell_record record;

	if (uio_resid(uio) < (user_ssize_t)sizeof(record)) {
		return EINVAL;                  /* the whole record, or nothing: a part of it says nothing */
	}
	lck_mtx_lock(&vayu_cell_lock);
	record = cell.record;
	lck_mtx_unlock(&vayu_cell_lock);
	return uiomove((caddr_t)&record, sizeof(record), uio);
}

static const struct cdevsw cell_cdevsw = {
	.d_open = cell_open, .d_close = cell_open, .d_read = cell_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
cell_make_node(void)
{
	int major = cdevsw_add(-1, &cell_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, VAYU_CELL_DEVICE) == NULL) {
		say("vayu-cell: /dev/%s cannot be made\n", VAYU_CELL_DEVICE);
		return false;
	}
	say("vayu-cell: /dev/%s (0666) is made: the record is %u bytes\n", VAYU_CELL_DEVICE,
	    (unsigned int)sizeof(struct vayu_cell_record));
	return true;
}

/* ---- the thread ---------------------------------------------------------------------------------- */

static void
cell_thread(void *argument __unused, wait_result_t wait __unused)
{
	uint64_t waited_ms = 0;
	int ret;

	ret = qmi_handle_init(&cell.qmi, VAYU_CELL_HANDLE_MAX, &cell_qmi_ops, NULL);
	if (ret < 0) {
		say("vayu-cell: qmi_handle_init: %d (no QRTR transport?)\n", ret);
		goto out;
	}
	cell.qmi_up = true;
	/*
	 * Version 1 and instance 0: the instance is the wildcard the router's lookup takes (0), so
	 * the UIM and NAS servers are the ones the modem announces whatever version they carry.
	 */
	ret = qmi_add_lookup(&cell.qmi, VAYU_CELL_UIM_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-cell: qmi_add_lookup(UIM): %d\n", ret);
		goto out;
	}
	ret = qmi_add_lookup(&cell.qmi, VAYU_CELL_NAS_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-cell: qmi_add_lookup(NAS): %d\n", ret);
		goto out;
	}
	while ((!cell.uim_there || !cell.nas_there) && !cell.stop && waited_ms < (uint64_t)cell.wait_s * 1000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited_ms += 100;
	}
	say("vayu-cell: after %llu ms: UIM is %s, NAS is %s\n", (unsigned long long)waited_ms,
	    cell.uim_there ? "there" : "MISSING", cell.nas_there ? "there" : "MISSING");
	if (cell.uim_there) {
		say("vayu-cell: UIM: node %u port %u, version %u instance %u\n", cell.uim_node, cell.uim_port,
		    cell.uim_version, cell.uim_instance);
	}
	if (cell.nas_there) {
		say("vayu-cell: NAS: node %u port %u, version %u instance %u\n", cell.nas_node, cell.nas_port,
		    cell.nas_version, cell.nas_instance);
	}
	if (!cell.uim_there && !cell.nas_there) {
		say("vayu-cell: neither service appeared\n");
		if (cell.level < 2) {
			say("vayu-cell: no request is sent\n");
			goto out;
		}
	} else {
		say("vayu-cell: DMS (0x02) is not asked: no identifier is needed here, and none is kept\n");
	}

	for (;;) {
		cell_round();
		if (cell.stop || cell.level < 2 || cell.poll_s == 0) {
			break;
		}
		for (uint32_t slept = 0; slept < cell.poll_s * 1000 && !cell.stop; slept += 100) {
			delay_for_interval(100, NSEC_PER_MSEC);
		}
		if (cell.stop) {
			break;
		}
	}
out:
	if (cell.qmi_up) {
		qmi_handle_release(&cell.qmi);
		cell.qmi_up = false;
	}
	cell.running = false;
	say("vayu-cell: the client's thread is done\n");
	thread_terminate(current_thread());
}

/* ---- start and stop ------------------------------------------------------------------------------ */

void
vayu_cell_start(struct vayu_qrtr *q)
{
	thread_t thread;
	uint32_t level = 0, wait_s = 20, poll_s = 30, register_arg = 0;

	if (cell.started) {
		say("vayu-cell: already started\n");
		return;
	}
	if (!PE_parse_boot_argn("vayu_cell", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (q == NULL || q->glink == NULL || !q->glink->up) {
		say("vayu-cell: vayu_cell=%u, but the modem's GLINK channel is not up: nothing is done\n", level);
		return;
	}
	if (level > 2) {
		level = 2;
	}
	PE_parse_boot_argn("vayu_cell_wait", &wait_s, sizeof(wait_s));
	if (wait_s < 1) {
		wait_s = 1;
	}
	if (wait_s > 120) {
		wait_s = 120;
	}
	PE_parse_boot_argn("vayu_cell_poll", &poll_s, sizeof(poll_s));
	if (poll_s < 5) {
		poll_s = 5;
	}
	if (poll_s > 3600) {
		poll_s = 3600;
	}
	PE_parse_boot_argn("vayu_cell_register", &register_arg, sizeof(register_arg));
	if (register_arg != 0 && register_arg != 1) {
		say("vayu-cell: vayu_cell_register=%u is refused: only 1 allows NAS Initiate Network Register\n",
		    register_arg);
		register_arg = 0;
	}
	cell.level = level;
	cell.wait_s = wait_s;
	cell.poll_s = poll_s;
	cell.register_arg = register_arg;
	cell.stop = false;
	cell.running = true;
	cell.register_sent = false;
	cell.rounds = cell.requests = cell.failures = 0;
	cell.seq = 0;
	cell_record_reset(&cell.record);
	say("vayu-cell: vayu_cell=%u vayu_cell_wait=%u vayu_cell_poll=%u vayu_cell_register=%u\n", level, wait_s,
	    poll_s, register_arg);
	say("vayu-cell: read-only: UIM 0x%x (card status, EF_ICCID, EF_IMSI), NAS 0x%x (serving system, signal); "
	    "no DMS, no PIN, no write, no data call\n", VAYU_CELL_UIM_SERVICE, VAYU_CELL_NAS_SERVICE);
	(void)cell_make_node();
	cell.started = true;
	if (kernel_thread_start(cell_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-cell: no thread\n");
		cell.running = false;
		cell.started = false;
		return;
	}
	thread_deallocate(thread);
}

void
vayu_cell_stop(void)
{
	uint32_t rounds, requests, failures;

	if (!cell.started) {
		return;
	}
	cell.stop = true;
	for (uint32_t waited = 0; waited < VAYU_CELL_STOP_MS && cell.running; waited += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	rounds = cell.rounds;
	requests = cell.requests;
	failures = cell.failures;
	if (cell.running) {
		say("vayu-cell: the client's thread has not finished in %u ms; its record is left as it is\n",
		    VAYU_CELL_STOP_MS);
	} else {
		cell.started = false;
		say("vayu-cell: done: %u round(s), %u QMI request(s), %u failure(s); /dev/%s still holds the last "
		    "record (%u bytes, seq %u)\n", rounds, requests, failures, VAYU_CELL_DEVICE,
		    (unsigned int)sizeof(struct vayu_cell_record), cell.seq);
	}
}
