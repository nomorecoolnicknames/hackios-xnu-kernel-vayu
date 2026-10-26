/*
 * Cellular, the first real look at the SIM and the network of the POCO X3 Pro (vayu, SM8150-AC,
 * X24): a QMI client of the modem's UIM and NAS services, over QRTR on the modem's GLINK channel
 * (the transport vayu_pas.c gives the QMI helpers of hakios.qmi.c). It is read-only by default;
 * the optional modem-state changes below are individually gated and it is off unless vayu_cell=N.
 *
 * What it asks, and why:
 *   UIM 0x0b  Get Card Status (0x002f)         card slot, application readiness, PIN state, AID
 *   UIM 0x0b  Read Record/Transparent          EF_ICCID and EF_IMSI, masked before logging
 *   DMS 0x02  Get Operating Mode (0x002d)      online/offline only, never an identifier
 *   NAS 0x03  Get Serving System (0x0024)      registration, PLMN/name, radio, LAC/CID
 *   NAS 0x03  Get Signal Info (0x004f)         LTE/5G RSRP and GSM/UMTS RSSI in dBm
 *
 * Optional modem-side changes are all OFF by default and independently gated:
 *   vayu_cell_online=1      DMS Set Operating Mode ONLINE (0x002e)
 *   vayu_cell_provision=1   UIM Switch Slot (0x0046) when the usable card is not in logical slot 1,
 *                           then Change Provisioning Session (0x0038) if primary GW is unassigned
 *   vayu_cell_register=1    NAS Initiate Network Register automatic (0x0022), retried with backoff
 *                           while unregistered (UIM initialization and PS attach are asynchronous)
 *   vayu_cell_data=1        after REGISTERED, a separate DPM/WDA/WDS client starts an IPv4 session
 *                           and reads its settings (bsd/dev/vayu_cell_data.c); no packets yet
 *
 * What it never does: no DMS identifier request, no SIM PIN, no APDU, no EFS/storage write, no
 * WDS/data call and no IPA register access. The host writes no storage; rmtfs keeps any modem
 * NV/EFS writes in the RAM overlay, including anything a session change may persist. Every identifier
 * byte is dropped before logging: only the digit count and a masked
 * tail ("...1234") of ICCID/IMSI are kept in the log and in the record.
 *
 * The answer, for the iOS userland shim: /dev/vayu-cell, a fixed record of 152 bytes
 * (sys/vayu_cell.h has every field and its offset). One is always readable; seq is 0 until the
 * first round has filled it, and rises by one per round. The node is made when vayu_cell is not 0,
 * and it stays after vayu_cell_stop(): what was last learned is still there.
 *
 * Boot-args:
 *   vayu_cell=0        nothing at all (the default)
 *   vayu_cell=1        one round after the UIM/NAS services appear, then the QMI handle is let go
 *                      (use vayu_cell=2 for slot/provisioning followed by registration retries)
 *   vayu_cell=2        one round, and another every vayu_cell_poll seconds
 *   vayu_cell_wait=S   how long to wait for UIM and NAS (default 20, 1..120)
 *   vayu_cell_poll=S   seconds between rounds at vayu_cell=2 (default 30, 5..3600; while a
 *                      subscription/registration change is pending, re-read after 1 s)
 *   vayu_cell_online=1 may send DMS Set Operating Mode ONLINE; default 0: only read the mode
 *   vayu_cell_provision=1 may point logical slot 1 at the slot with a usable USIM and make that
 *                      application primary GW; default 0: no UIM changes
 *   vayu_cell_register=1 may send NAS Initiate Network Register automatic and retry it; default 0
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
#include <sys/vayu_cell_data.h>
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
	volatile bool uim_there, nas_there, dms_there;
	uint32_t uim_node, uim_port, uim_version, uim_instance;
	uint32_t nas_node, nas_port, nas_version, nas_instance;
	uint32_t dms_node, dms_port, dms_version, dms_instance;
	volatile bool stop, running;
	bool started;
	uint32_t level, wait_s, poll_s, register_arg, online_arg, provision_arg;
	bool online_sent, slot_switch_sent, provision_sent;
	uint32_t online_attempts;
	uint8_t online_stage;
	bool online_low_power_tried;
	uint16_t online_qmi_error;
	uint64_t online_next_ms;
	bool dms_mode_valid, dms_missing_logged, card_dumped;
	uint8_t dms_mode;
	int dms_last_mode;
	uint64_t next_register_ms;
	uint32_t recheck_delay_ms;
	uint32_t register_attempts;
	bool registration_blocked, registration_denied, slot_switch_done;
	bool last_registered;
	bool last_signal_valid;
	int32_t last_signal_dbm;
	uint8_t last_signal_radio;
	uint64_t last_signal_ms;
	uint32_t rounds, requests, failures;
	uint32_t seq;
	struct vayu_cell_record record;
	/* the answers, one at a time: the thread is their only user */
	struct vayu_cell_uim_get_card_status_resp card_status;
	struct vayu_cell_uim_read_resp read;
	struct vayu_cell_nas_get_serving_system_resp serving;
	struct vayu_cell_nas_get_signal_info_resp signal;
	struct vayu_cell_dms_get_operating_mode_resp operating;
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
	} else if (service->service == VAYU_CELL_DMS_SERVICE) {
		cell.dms_node = service->node;
		cell.dms_port = service->port;
		cell.dms_version = service->version;
		cell.dms_instance = service->instance;
		cell.dms_there = true;
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
		cell.last_registered = false;
		vayu_cell_data_registered(false);
		cell.register_attempts = 0;
		cell.registration_denied = false;
		cell.registration_blocked = false;
		cell.next_register_ms = 0;
		cell.last_signal_valid = false;
		cell.last_signal_ms = 0;
	}
	if (service->service == VAYU_CELL_DMS_SERVICE && cell.dms_node == service->node && cell.dms_port == service->port) {
		cell.dms_there = false;
		cell.dms_mode_valid = false;
		cell.online_sent = false;              /* allow a fresh bounded sequence after a DMS restart */
		cell.online_attempts = 0;
		cell.online_stage = 0;
		cell.online_low_power_tried = false;
		cell.online_qmi_error = 0;
		cell.online_next_ms = 0;
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


/* ---- DMS operating mode: no identifiers, and ONLINE only when explicitly allowed --------------- */

/* libqmi/ModemManager's observed offline -> low-power -> online fallback is error 52;
 * upstream HackiOS also treats it that way. Keep 60 visible as a possible invalid transition:
 * try ONLINE first, and use LOW_POWER only as a bounded fallback, never RESET/SHUTDOWN.
 */
static void
cell_dms_set(uint8_t mode, bool low_power_step)
{
	struct vayu_cell_dms_set_operating_mode_req request = { .mode = mode };
	struct vayu_cell_result_resp response = { 0 };
	char what[64];
	int ret;

	snprintf(what, sizeof(what), "set operating mode %s", low_power_step ? "LOW_POWER" : "ONLINE");
	if (low_power_step) {
		cell.online_low_power_tried = true;     /* at most one LOW_POWER write; ONLINE has its own budget */
	} else {
		cell.online_attempts++;
	}
	say("vayu-cell: vayu_cell_online=1: DMS Set Operating Mode %s (ONLINE attempts %u/3, from mode %u); "
	    "the modem may transmit\n", low_power_step ? "LOW_POWER" : "ONLINE",
	    (unsigned int)cell.online_attempts, (unsigned int)cell.dms_mode);
	ret = cell_request(what, cell.dms_node, cell.dms_port, VAYU_CELL_DMS_SET_OPERATING_MODE,
	    vayu_cell_dms_set_operating_mode_req_ei, &request, vayu_cell_result_resp_ei, &response,
	    &response.result);
	cell.online_qmi_error = response.result.error;
	if (ret == 0) {
		cell.dms_mode = mode;
		cell.online_stage = low_power_step ? 2 : 0;
		cell.online_next_ms = now_ms() + (low_power_step ? 2000 : 5000);
		/* A later read-back decides whether ONLINE still needs sending. The success
		 * latch must not suppress the final ONLINE after LOW_POWER or an accepted-but-
		 * not-persisted ONLINE (the mode-5 case). ONLINE attempts remain capped at 3. */
		cell.online_sent = false;
		say("vayu-cell: DMS %s accepted; read-back on the next round%s\n",
		    low_power_step ? "LOW_POWER" : "ONLINE", low_power_step ? " before ONLINE" : "");
		return;
	}
	if (ret != -EIO && cell.online_attempts < 3) {
		/* Transport failures may be retried; a QMI refusal advances only to the documented fallback. */
		cell.online_sent = false;
		say("vayu-cell: DMS %s transport failed; one bounded retry is allowed\n",
		    low_power_step ? "LOW_POWER" : "ONLINE");
		return;
	}
	if (low_power_step) {
		cell.online_stage = 2;          /* the detour failed: try ONLINE directly, budget permitting */
		cell.online_next_ms = now_ms() + 1000;
		cell.online_sent = false;
		say("vayu-cell: LOW_POWER was not accepted; ONLINE is still tried directly\n");
		return;
	}
	if ((response.result.error == 52 || response.result.error == 60) && cell.online_attempts < 3) {
		cell.online_stage = 1;
		cell.online_next_ms = now_ms() + 1000;
		cell.online_sent = false;
		say("vayu-cell: ONLINE refused with QMI error %u; next: LOW_POWER, wait 2 s, then ONLINE\n",
		    (unsigned int)response.result.error);
		return;
	}
	say("vayu-cell: DMS %s refused (result %u error %u); no reset/shutdown command is sent\n",
	    low_power_step ? "LOW_POWER" : "ONLINE", (unsigned int)response.result.result,
	    (unsigned int)response.result.error);
}

static void
cell_dms_round(void)
{
	int ret;

	if (!cell.dms_there) {
		if (cell.online_arg == 1 && !cell.dms_missing_logged) {
			cell.dms_missing_logged = true;
			say("vayu-cell: DMS is not there: cannot check/set ONLINE\n");
		}
		return;
	}
	memset(&cell.operating, 0, sizeof(cell.operating));
	ret = cell_request("get operating mode", cell.dms_node, cell.dms_port, VAYU_CELL_DMS_GET_OPERATING_MODE,
	    NULL, NULL, vayu_cell_dms_get_operating_mode_resp_ei, &cell.operating, &cell.operating.result);
	if (ret != 0 || !cell.operating.mode_valid) {
		return;
	}
	cell.dms_mode = cell.operating.mode;
	cell.dms_mode_valid = true;
	if (cell.dms_last_mode != cell.dms_mode) {
		cell.dms_last_mode = cell.dms_mode;
		say("vayu-cell: DMS operating mode %u (%s), offline reason %u, hardware restricted %u\n",
		    (unsigned int)cell.dms_mode, cell.dms_mode == VAYU_CELL_DMS_MODE_ONLINE ? "ONLINE" : "not online",
		    cell.operating.offline_reason_valid ? (unsigned int)cell.operating.offline_reason : 0xffffU,
		    cell.operating.hw_restricted_valid ? (unsigned int)cell.operating.hw_restricted : 0xffU);
	}
	if (cell.dms_mode != VAYU_CELL_DMS_MODE_ONLINE && !cell.dms_missing_logged &&
	    (cell.online_arg == 1 || cell.register_arg == 1)) {
		cell.dms_missing_logged = true;
		say("vayu-cell: DMS is not ONLINE; %s\n", cell.online_arg == 1 ?
		    "try ONLINE once even from SHUTTING_DOWN; LOW_POWER is a bounded fallback on error 52/60, never a reset" :
		    "NAS automatic registration waits for ONLINE (set vayu_cell_online=1 to allow the transition)");
	}
	if (cell.online_arg == 1 && cell.dms_mode != VAYU_CELL_DMS_MODE_ONLINE && !cell.online_sent &&
	    now_ms() >= cell.online_next_ms) {
		if (cell.online_stage == 1 && (cell.dms_mode == 1 || cell.online_low_power_tried)) {
			cell.online_stage = 2;      /* low power already accepted or already tried once */
			cell.online_next_ms = now_ms() + 2000;
			return;
		}
		if (cell.dms_mode == 4 || cell.dms_mode == 2 || cell.dms_mode == 0xff) {
			say("vayu-cell: DMS mode %u is not a safe ONLINE source; no reset/shutdown command\n",
			    (unsigned int)cell.dms_mode);
			return;                     /* do not latch: a later read may show a recoverable mode */
		}
		if (cell.online_attempts >= 3) {
			return;                     /* at most 3 ONLINE writes, plus at most one LOW_POWER */
		}
		cell.online_sent = true;
		if (cell.online_stage == 1) {
			cell_dms_set(1, true);      /* low power, then 2 s and ONLINE on the next round */
		} else {
			/* 0610b: mode 5 persisted while QMI answered. ONLINE is the first probe,
			 * not an unconditional low-power/reset cycle. The modem's response decides. */
			cell_dms_set(VAYU_CELL_DMS_MODE_ONLINE, false);
		}
	}
}

/* ---- the SIM ------------------------------------------------------------------------------------- */

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

/* The USIM/SIM application states that a provisioning session may name. */
static bool
cell_app_provisionable(const struct vayu_cell_uim_app *app)
{
	return app != NULL && (app->type == VAYU_CELL_UIM_APP_TYPE_USIM || app->type == VAYU_CELL_UIM_APP_TYPE_SIM) &&
	    (app->state == VAYU_CELL_UIM_APP_READY || app->state == VAYU_CELL_UIM_APP_DETECTED);
}

/* Is primary GW already mapped to a present slot with a usable USIM/SIM application?
 * The packed index is (slot_index << 8 | application_index); 0xffff means unassigned.
 * Do not switch a working subscription merely because the best card sits in slot 2.
 */
static bool
cell_primary_usable(const struct vayu_cell_uim_card_status *status)
{
	uint32_t index = status->index_gw_primary;
	uint32_t slot = index >> 8;
	const struct vayu_cell_uim_card *card;

	if (index == VAYU_CELL_UIM_INDEX_NONE || slot >= status->cards_len || slot >= VAYU_CELL_CARDS) {
		return false;
	}
	card = &status->cards[slot];
	return card->card_state == VAYU_CELL_CARD_PRESENT && cell_app_provisionable(cell_app(card));
}

/* Pick the card with an application NAS can use, not merely the first slot. */
static const struct vayu_cell_uim_card *
cell_select_card(const struct vayu_cell_uim_card_status *status, uint32_t *slot)
{
	const struct vayu_cell_uim_card *best = NULL;
	unsigned best_score = 0;
	uint32_t count = status->cards_len < VAYU_CELL_CARDS ? status->cards_len : VAYU_CELL_CARDS;

	*slot = 1;
	for (uint32_t i = 0; i < count; i++) {
		const struct vayu_cell_uim_card *card = &status->cards[i];
		const struct vayu_cell_uim_app *app = cell_app(card);
		unsigned score = 0;

		if (card->card_state == VAYU_CELL_CARD_PRESENT) {
			score = 2;
			if (app != NULL && app->state == VAYU_CELL_UIM_APP_DETECTED) {
				score = 3;
			}
			if (app != NULL && app->state == VAYU_CELL_UIM_APP_READY) {
				score = 4;
			}
		} else if (best == NULL) {
			score = 1;
		}
		if (score > best_score) {
			best = card;
			best_score = score;
			*slot = i + 1;
		}
	}
	return best;
}

static void
cell_dump_cards(const struct vayu_cell_uim_card_status *status)
{
	uint32_t count = status->cards_len < VAYU_CELL_CARDS ? status->cards_len : VAYU_CELL_CARDS;

	say("vayu-cell: UIM primary GW index %#x (0xffff: none), %u slot(s)\n",
	    (unsigned int)status->index_gw_primary, (unsigned int)status->cards_len);
	for (uint32_t i = 0; i < count; i++) {
		const struct vayu_cell_uim_card *card = &status->cards[i];
		uint32_t apps = card->app_len < VAYU_CELL_APPLICATIONS ? card->app_len : VAYU_CELL_APPLICATIONS;

		say("vayu-cell: UIM slot %u: card state %u, error %u, %u application(s)\n", i + 1,
		    (unsigned int)card->card_state, (unsigned int)card->error_code, (unsigned int)card->app_len);
		for (uint32_t a = 0; a < apps; a++) {
			const struct vayu_cell_uim_app *app = &card->app[a];
			say("vayu-cell: UIM slot %u app %u: %s, state %u, AID %u bytes\n", i + 1, a,
			    cell_app_name(app->type), (unsigned int)app->state, (unsigned int)app->aid_len);
		}
	}
}

static void
cell_switch_slot(uint32_t physical_slot)
{
	struct vayu_cell_uim_switch_slot_req request = {
		.logical_slot = VAYU_CELL_UIM_SLOT_1,
		.physical_slot = physical_slot,
	};
	struct vayu_cell_result_resp response = { 0 };
	int ret;

	cell.slot_switch_sent = true;           /* one attempt: never toggle mappings in a loop */
	say("vayu-cell: vayu_cell_provision=1: UIM Switch Slot logical 1 -> physical %u\n", physical_slot);
	ret = cell_request("switch slot", cell.uim_node, cell.uim_port, VAYU_CELL_UIM_SWITCH_SLOT,
	    vayu_cell_uim_switch_slot_req_ei, &request, vayu_cell_result_resp_ei, &response, &response.result);
	cell.card_dumped = false;
	cell.last_registered = false;
	cell.next_register_ms = now_ms() + 2000;
	cell.slot_switch_done = ret == 0;
	cell.recheck_delay_ms = ret == 0 ? 1000 : 0;
	say("vayu-cell: switch slot: %s; card status will be read again\n", ret == 0 ? "accepted" : "refused");
}

static void
cell_change_provisioning(const struct vayu_cell_uim_app *app, uint32_t slot)
{
	struct vayu_cell_uim_change_provisioning_req request = { 0 };
	struct vayu_cell_result_resp response = { 0 };
	int ret;

	if (app == NULL || app->aid_len == 0 || app->aid_len > VAYU_CELL_AID) {
		say("vayu-cell: provisioning: the selected application has no usable AID; not sent\n");
		cell.provision_sent = true;
		return;
	}
	request.change.session_type = VAYU_CELL_UIM_SESSION_PRIMARY_GW;
	request.change.activate = 1;
	request.application.slot = (uint8_t)slot;
	request.application.aid_len = app->aid_len;
	memcpy(request.application.aid, app->aid, app->aid_len);
	cell.provision_sent = true;             /* one attempt per boot, success or refusal */
	say("vayu-cell: vayu_cell_provision=1: make the USIM/SIM of slot %u the primary GW application "
	    "(AID %u bytes)\n", slot, (unsigned int)app->aid_len);
	ret = cell_request("change provisioning session", cell.uim_node, cell.uim_port,
	    VAYU_CELL_UIM_CHANGE_PROVISIONING, vayu_cell_uim_change_provisioning_req_ei, &request,
	    vayu_cell_result_resp_ei, &response, &response.result);
	cell.card_dumped = false;
	cell.last_registered = false;
	cell.next_register_ms = now_ms() + 2000;
	cell.recheck_delay_ms = ret == 0 ? 1000 : 0;
	say("vayu-cell: provisioning change: %s; NAS registration may be asked again after re-read\n",
	    ret == 0 ? "accepted" : "refused");
}

/* The card's state, then its ICCID and IMSI (masked; only their digit counts are kept whole). */
static void
cell_uim_round(struct vayu_cell_record *record)
{
	char digits[VAYU_CELL_DIGITS];
	uint8_t data[VAYU_CELL_FILE_DATA];
	uint32_t length, slot = 1;
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
	if (!cell.card_dumped) {
		cell.card_dumped = true;
		cell_dump_cards(&cell.card_status.card_status);
	}
	card = cell_select_card(&cell.card_status.card_status, &slot);
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
	say("vayu-cell: SIM slot %u: %s; card state %u, %u application(s)%s%s, PIN state %u with %u attempt(s) left%s\n",
	    slot, cell_sim_name(record->sim_state), (unsigned int)card->card_state, (unsigned int)card->app_len,
	    app != NULL ? ", " : "", app != NULL ? cell_app_name(app->type) : "",
	    (unsigned int)pin, (unsigned int)record->pin_retries,
	    record->sim_state == VAYU_CELL_SIM_PIN_REQUIRED ? " (no PIN is ever sent)" : "");

	/* Never switch an already usable primary GW. After one attempt, successful or not,
	 * continue ordinary UIM/NAS reads instead of hiding the network behind a retry loop.
	 */
	if (cell.provision_arg == 1 && slot > 1 && !cell_primary_usable(&cell.card_status.card_status) &&
	    !cell.slot_switch_done && !cell.slot_switch_sent) {
		cell_switch_slot(slot);
		return;
	}
	if (card->card_state != VAYU_CELL_CARD_PRESENT) {
		return;
	}
	if (cell.provision_arg == 1 && !cell_primary_usable(&cell.card_status.card_status) &&
	    cell_app_provisionable(app) && !cell.provision_sent) {
		/* The selected slot is physical; after Switch Slot the primary may already be usable,
		 * otherwise bind that card's application directly (also works after a switch refusal). */
		cell_change_provisioning(app, slot);
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

#define VAYU_CELL_REGISTER_MAX_ATTEMPTS 12

/* Stop modem signalling on a terminal condition, and back off while UIM/PS attach settles.
 * SIM_ABSENT/PIN/ERROR follows the latest card state and can clear when the card changes;
 * DENIED and the attempt budget stop retries until registration/searching or a new session.
 */
static void
cell_update_registration_blocked(const struct vayu_cell_record *record)
{
	bool sim_blocked = record->sim_state == VAYU_CELL_SIM_ABSENT ||
	    record->sim_state == VAYU_CELL_SIM_ERROR || record->sim_state == VAYU_CELL_SIM_PIN_REQUIRED;
	bool radio_blocked = cell.dms_mode_valid && cell.dms_mode != VAYU_CELL_DMS_MODE_ONLINE;
	bool blocked = sim_blocked || radio_blocked || cell.registration_denied ||
	    cell.register_attempts >= VAYU_CELL_REGISTER_MAX_ATTEMPTS;

	if (blocked != cell.registration_blocked) {
		say("vayu-cell: NAS automatic registration %s%s%s%s\n", blocked ? "paused" : "resumed",
		    sim_blocked ? " (SIM absent/error/PIN)" : "",
		    radio_blocked ? " (DMS not ONLINE)" : "",
		    cell.registration_denied ? " (network denied)" : "",
		    cell.register_attempts >= VAYU_CELL_REGISTER_MAX_ATTEMPTS ? " (retry budget exhausted)" : "");
	}
	cell.registration_blocked = blocked;
}

static void
cell_schedule_registration_retry(const struct vayu_cell_record *record)
{
	uint32_t delay_ms = cell.register_attempts < 4 ? 6000 : cell.register_attempts < 8 ? 15000 : 60000;

	cell.register_attempts++;
	cell.next_register_ms = now_ms() + delay_ms;
	if (cell.register_attempts == VAYU_CELL_REGISTER_MAX_ATTEMPTS) {
		say("vayu-cell: NAS registration retry budget (%u attempts) exhausted; stopping automatic attempts\n",
		    VAYU_CELL_REGISTER_MAX_ATTEMPTS);
		cell_update_registration_blocked(record);
	}
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
		vayu_cell_data_registered(false);
		if (vayu_cell_data_pending()) {
			cell.recheck_delay_ms = 1000;
		}
		return;
	}
	cell_update_registration_blocked(record);
	if (cell.register_arg == 1 && !cell.registration_blocked && !cell.last_registered &&
	    now_ms() >= cell.next_register_ms && (!cell.dms_mode_valid || cell.dms_mode == VAYU_CELL_DMS_MODE_ONLINE)) {
		struct vayu_cell_nas_initiate_register_req request = { .action = VAYU_CELL_NAS_REGISTER_AUTOMATIC };
		struct vayu_cell_nas_register_resp response = { 0 };

		/* Retry while unregistered: UIM initialization and packet-domain attach are asynchronous. */
		cell_schedule_registration_retry(record);
		say("vayu-cell: vayu_cell_register=1: NAS Initiate Network Register (automatic) is sent; "
		    "the modem may transmit because of it\n");
		ret = cell_request("initiate register", cell.nas_node, cell.nas_port, VAYU_CELL_NAS_INITIATE_NETWORK_REGISTER,
		    vayu_cell_nas_initiate_register_req_ei, &request, vayu_cell_nas_register_resp_ei, &response,
		    &response.result);
		say("vayu-cell: initiate register: %s%s\n", ret == 0 ? "the modem agreed" : "refused (see above)",
		    ret == 0 ? "" : "; will retry after the next UIM/NAS poll");
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
		cell.last_registered = record->reg_state == VAYU_CELL_REG_REGISTERED;
		vayu_cell_data_registered(cell.last_registered);
		if (record->reg_state == VAYU_CELL_REG_DENIED) {
			cell.registration_denied = true;
		} else if (record->reg_state == VAYU_CELL_REG_SEARCHING || cell.last_registered) {
			cell.registration_denied = false;
			if (record->reg_state == VAYU_CELL_REG_SEARCHING &&
			    cell.record.reg_state != VAYU_CELL_REG_SEARCHING) {
				/* One reset on entry; a persistent SEARCHING state must not postpone retries forever. */
				cell.register_attempts = 0;
				cell.next_register_ms = now_ms() + 6000;
			}
		}
		if (cell.last_registered) {
			cell.register_attempts = 0;
			cell.next_register_ms = 0;
		}
		cell_update_registration_blocked(record);
		if (!cell.last_registered &&
		    (vayu_cell_data_pending() || (cell.register_arg == 1 && !cell.registration_blocked))) {
			cell.recheck_delay_ms = cell.register_attempts < 4 ? 1000 : 5000;
		}
	} else {
		if (ret == 0) {
			say("vayu-cell: serving system: the reply has no serving system TLV\n");
		}
		/* A failed/missing serving response is not proof of continued registration. */
		cell.last_registered = false;
		vayu_cell_data_registered(false);
		if (vayu_cell_data_pending()) {
			cell.recheck_delay_ms = 1000;
		}
	}
	if (cell.stop) {
		return;
	}
	memset(&cell.signal, 0, sizeof(cell.signal));
	ret = cell_request("signal info", cell.nas_node, cell.nas_port, VAYU_CELL_NAS_GET_SIGNAL_INFO, NULL, NULL,
	    vayu_cell_nas_get_signal_info_resp_ei, &cell.signal, &cell.signal.result);
	if (ret == 0 && cell.signal.lte_valid && cell.signal.lte.rsrp != (int16_t)0x8000) {
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
	if ((record->flags & VAYU_CELL_FLAG_SIGNAL) == 0 && ret == 0 && cell.signal.nr5g_valid &&
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
	if ((record->flags & VAYU_CELL_FLAG_SIGNAL) == 0 && ret == 0 && cell.signal.wcdma_valid &&
	    cell_in_range((int8_t)cell.signal.wcdma.rssi, -120, -20)) {
		record->signal_dbm = (int8_t)cell.signal.wcdma.rssi;
		record->flags |= VAYU_CELL_FLAG_SIGNAL;
		say("vayu-cell: signal: UMTS RSSI %d dBm, ECIO raw %d\n", (int)record->signal_dbm,
		    (int)cell.signal.wcdma.ecio);
	}
	if ((record->flags & VAYU_CELL_FLAG_SIGNAL) == 0 && ret == 0 && cell.signal.gsm_valid &&
	    cell_in_range((int8_t)cell.signal.gsm_rssi, -120, -20)) {
		record->signal_dbm = (int8_t)cell.signal.gsm_rssi;
		record->flags |= VAYU_CELL_FLAG_SIGNAL;
		say("vayu-cell: signal: GSM RSSI %d dBm\n", (int)record->signal_dbm);
	}
	if ((record->flags & VAYU_CELL_FLAG_SIGNAL) != 0) {
		cell.last_signal_valid = true;
		cell.last_signal_dbm = record->signal_dbm;
		cell.last_signal_radio = record->radio;
		cell.last_signal_ms = now_ms();
	} else if (cell.last_signal_valid && now_ms() - cell.last_signal_ms <= 60000) {
		/* NAS can alternate registration and signal answers. Keep only a fresh measurement,
		 * and mark it as cached; never invent a level when there was none. */
		record->signal_dbm = cell.last_signal_dbm;
		record->flags |= VAYU_CELL_FLAG_SIGNAL | VAYU_CELL_FLAG_SIGNAL_CACHED;
		if (record->radio == 0xff) {           /* only when serving gave no radio, never replace NONE */
			record->radio = cell.last_signal_radio;
		}
		say("vayu-cell: signal: cached %d dBm (%llu ms old)\n", (int)record->signal_dbm,
		    (unsigned long long)(now_ms() - cell.last_signal_ms));
	} else {
		say("vayu-cell: signal: no fresh or recent LTE, 5G, UMTS or GSM strength in the reply\n");
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
	cell.recheck_delay_ms = 0;
	cell_uim_round(&record);
	if (cell.stop) {
		return;
	}
	/* Subscription first, radio mode second: never attach with the wrong slot. */
	if (cell.recheck_delay_ms == 0) {
		cell_dms_round();
	}
	if (cell.recheck_delay_ms == 0) {
		cell_nas_round(&record);
	} else {
		say("vayu-cell: subscription changed: NAS is polled after the UIM re-read\n");
	}
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
	ret = qmi_add_lookup(&cell.qmi, VAYU_CELL_DMS_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-cell: qmi_add_lookup(DMS): %d\n", ret);
		goto out;
	}
	while ((!cell.uim_there || !cell.nas_there || (cell.online_arg == 1 && !cell.dms_there)) && !cell.stop &&
	    waited_ms < (uint64_t)cell.wait_s * 1000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited_ms += 100;
	}
	say("vayu-cell: after %llu ms: UIM is %s, NAS is %s, DMS is %s\n", (unsigned long long)waited_ms,
	    cell.uim_there ? "there" : "MISSING", cell.nas_there ? "there" : "MISSING",
	    cell.dms_there ? "there" : "MISSING");
	if (cell.uim_there) {
		say("vayu-cell: UIM: node %u port %u, version %u instance %u\n", cell.uim_node, cell.uim_port,
		    cell.uim_version, cell.uim_instance);
	}
	if (cell.nas_there) {
		say("vayu-cell: NAS: node %u port %u, version %u instance %u\n", cell.nas_node, cell.nas_port,
		    cell.nas_version, cell.nas_instance);
	}
	if (cell.dms_there) {
		say("vayu-cell: DMS: node %u port %u, version %u instance %u (operating mode only)\n",
		    cell.dms_node, cell.dms_port, cell.dms_version, cell.dms_instance);
	}
	if (!cell.uim_there && !cell.nas_there) {
		say("vayu-cell: neither service appeared\n");
		if (cell.level < 2) {
			say("vayu-cell: no request is sent\n");
			goto out;
		}
	} else {
		say("vayu-cell: DMS is used for operating mode only: no device identifiers are asked or kept\n");
	}

	for (;;) {
		cell_round();
		if (cell.stop || cell.level < 2 || cell.poll_s == 0) {
			break;
		}
		uint32_t delay_ms = cell.recheck_delay_ms != 0 ? cell.recheck_delay_ms : cell.poll_s * 1000;
		for (uint32_t slept = 0; slept < delay_ms && !cell.stop; slept += 100) {
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
	uint32_t level = 0, wait_s = 20, poll_s = 30, register_arg = 0, online_arg = 0, provision_arg = 0;

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
	PE_parse_boot_argn("vayu_cell_online", &online_arg, sizeof(online_arg));
	PE_parse_boot_argn("vayu_cell_provision", &provision_arg, sizeof(provision_arg));
	PE_parse_boot_argn("vayu_cell_register", &register_arg, sizeof(register_arg));
	if (online_arg != 0 && online_arg != 1) {
		say("vayu-cell: vayu_cell_online=%u is refused: only 1 allows DMS ONLINE\n", online_arg);
		online_arg = 0;
	}
	if (provision_arg != 0 && provision_arg != 1) {
		say("vayu-cell: vayu_cell_provision=%u is refused: only 1 allows UIM slot/provisioning changes\n",
		    provision_arg);
		provision_arg = 0;
	}
	if (register_arg != 0 && register_arg != 1) {
		say("vayu-cell: vayu_cell_register=%u is refused: only 1 allows NAS Initiate Network Register\n",
		    register_arg);
		register_arg = 0;
	}
	cell.uim_there = cell.nas_there = cell.dms_there = false;
	cell.level = level;
	cell.wait_s = wait_s;
	cell.poll_s = poll_s;
	cell.register_arg = register_arg;
	cell.online_arg = online_arg;
	cell.provision_arg = provision_arg;
	cell.stop = false;
	cell.running = true;
	cell.online_sent = false;
	cell.online_attempts = 0;
	cell.online_stage = 0;
	cell.online_low_power_tried = false;
	cell.online_qmi_error = 0;
	cell.online_next_ms = 0;
	cell.slot_switch_sent = false;
	cell.provision_sent = false;
	cell.dms_mode_valid = false;
	cell.dms_missing_logged = false;
	cell.card_dumped = false;
	cell.dms_last_mode = -1;
	cell.next_register_ms = 0;
	cell.recheck_delay_ms = 0;
	cell.register_attempts = 0;
	cell.registration_blocked = false;
	cell.registration_denied = false;
	cell.slot_switch_done = false;
	cell.last_registered = false;
	cell.last_signal_valid = false;
	cell.last_signal_dbm = INT32_MIN;
	cell.last_signal_radio = 0xff;
	cell.last_signal_ms = 0;
	cell.rounds = cell.requests = cell.failures = 0;
	cell.seq = 0;
	cell_record_reset(&cell.record);
	say("vayu-cell: vayu_cell=%u vayu_cell_wait=%u vayu_cell_poll=%u vayu_cell_online=%u "
	    "vayu_cell_provision=%u vayu_cell_register=%u\n", level, wait_s, poll_s, online_arg, provision_arg,
	    register_arg);
	say("vayu-cell: UIM 0x%x (card status, EF_ICCID, EF_IMSI), NAS 0x%x (serving system, signal), "
	    "DMS 0x%x (operating mode); no PIN, no host storage write; data only under vayu_cell_data=1\n",
	    VAYU_CELL_UIM_SERVICE, VAYU_CELL_NAS_SERVICE, VAYU_CELL_DMS_SERVICE);
	(void)cell_make_node();
	cell.started = true;
	vayu_cell_data_start(q);
	if (kernel_thread_start(cell_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-cell: no thread\n");
		vayu_cell_data_stop();
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
	vayu_cell_data_stop();
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
