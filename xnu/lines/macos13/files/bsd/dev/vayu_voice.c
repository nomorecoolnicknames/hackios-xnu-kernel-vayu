/*
 * Voice on vayu: a QMI VOICE client of the modem's service 0x09 (version 2), and
 * /dev/vayu-voice as its text protocol.
 *
 * What it asks, and why:
 *   VOICE 0x09  Indication Register  0x0003  the call-notification group (TLV 0x13)
 *                                            and the supplementary-service group
 *                                            (0x12), plus USSD (0x16): from then on the
 *                                            modem tells us about a call
 *   VOICE 0x09  Get All Call Info    0x002f  the calls the modem has, on demand and
 *                                            every vayu_call_poll seconds
 *   VOICE 0x09  Get Call Info        0x0023  one call by id
 *   VOICE 0x09  Dial Call            0x0020  only with vayu_call=2, vayu_call_dial=1
 *                                            and the number on the allow list
 *   VOICE 0x09  Answer Call          0x0022  only with vayu_call=2
 *   VOICE 0x09  End Call             0x0021  only with vayu_call=2
 *   VOICE 0x09  Burst DTMF           0x0028  one digit per request, vayu_call=2
 *   VOICE 0x09  Start/Stop Cont. DTMF 0x0029/0x002a, vayu_call=2
 *   VOICE 0x09  Send Flash           0x0024  vayu_call=2
 *
 * The default is read-only in the strongest sense the task allows: vayu_call=1
 * registers the indications and reads status, and every command that would put a
 * call on the network is refused by the client itself, not only by its absence
 * from a menu.  An incoming call is the one thing that arrives without being asked
 * for; it becomes one kernel log line ("vayu-voice: INCOMING CALL: ...") and an
 * EVENT line on the device.
 *
 * Why the ids and layouts are what they are: sys/vayu_voice_qmi.h's comment.  In
 * short, everything the client sends *and* the call-status indication it reads was
 * taken from the libqmi 1.32.2 library installed on this machine, offline: each
 * VOICE request is built inline in qmi_client_voice_*(), so the message and TLV ids
 * are immediates; the indication is 0x2e, whose handler in the same library reads a
 * one-byte count and 7-byte call entries from TLV 0x01 and emits the
 * "all-call-status" signal.  The handler here still takes *every* indication (the
 * handle's wildcard) and logs the id and TLV shape of each, so a firmware revision
 * that answers differently is seen in run A instead of being swallowed.
 *
 * Boot-args: sys/vayu_voice.h has the list with the meaning of each.  In short:
 *   vayu_call=0|1|2      off (default) | indications and status | + call commands
 *   vayu_call_wait=S     how long to wait for the VOICE service (30)
 *   vayu_call_dial=1     permit Dial Call; the number must equal vayu_call_allow
 *   vayu_call_allow=N    that number (default 0500, the owner-approved service number)
 *   vayu_call_poll=S     Get All Call Info every S seconds (0: indications only)
 *   vayu_call_ind_all=1  register every indication group, not just the four needed
 *   vayu_call_dump=1     print indication TLV bytes (a value may hold a number)
 *   vayu_call_audio=1    the q6voice path of bsd/dev/vayu_audio.c follows the call: up when a call
 *                        goes into CONVERSATION (vayu_voice=2 for an LTE/VoIP call, 3 for CS), down
 *                        when none is left.  Its "vayu_call" is the *call* client's; the audio
 *                        module's own flag for the path is still "vayu_voice".
 *   vayu_call_auto=1     dial vayu_call_allow by itself vayu_call_auto_delay s after READY, hold
 *                        CONVERSATION vayu_call_auto_end s, then End Call (no shell needed).
 *
 * The device: reads return whole lines (EVENT ..., OK ..., ERR ..., STATE ...); a
 * write is one or more '\n'-separated commands whose answers are queued for the
 * reader.  sys/vayu_voice.h has the grammar.  One STATE line is queued when the
 * device is opened, so a first read says where things are without a command.
 *
 * Privacy: the kernel log never prints a caller's or a dialled number -- only its
 * last four digits ("...1234") -- and never prints an IMSI, ICCID or IMEI, which
 * this client does not ask for.  The device carries the full number because a
 * phone UI needs it.  A dialled number is masked in every log line.
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
#include <sys/vayu_audio.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_voice.h>
#include <sys/vayu_voice_qmi.h>

extern void loader_log_putc(char c);

#define VOICE_HANDLE_MAX        4096    /* the largest datagram the QMI handle takes */
#define VOICE_REPLY_MS          5000    /* one request's reply */
#define VOICE_STOP_MS           8000    /* the longest wait for the worker to end */
#define VOICE_REGISTER_TRIES    3       /* Indication Register attempts per service */
#define VOICE_RAW_MAX           1024    /* one reply, kept whole for the TLV walk */
#define VOICE_NOTE_MAX          (VAYU_VOICE_CALLS * 2)
#define VOICE_DUMP_BYTES        40      /* bytes printed with vayu_call_dump=1 */

/* ---- state --------------------------------------------------------------------------------------- */

enum voice_job_kind {
	VOICE_JOB_NONE = 0,
	VOICE_JOB_HELP,
	VOICE_JOB_STATUS,
	VOICE_JOB_CALLS,
	VOICE_JOB_INFO,
	VOICE_JOB_ANSWER,
	VOICE_JOB_END,
	VOICE_JOB_DTMF,
	VOICE_JOB_DTMF_START,
	VOICE_JOB_DTMF_STOP,
	VOICE_JOB_FLASH,
	VOICE_JOB_DIAL,
};

struct voice_job {
	uint32_t kind;
	uint32_t call_id;
	uint32_t length;
	char text[VAYU_VOICE_DIGITS_MAX];
};

struct voice_line {
	char text[VAYU_VOICE_LINE_MAX];
};

/* One line to log after the lock is let go. */
struct voice_note {
	uint8_t kind;                   /* 1: a call changed, 2: it is incoming, 3: it is gone */
	uint8_t id, state, direction, mode, type;
	char tail[8];
};

struct voice_state {
	struct qmi_handle qmi;
	bool qmi_up;
	volatile bool present, stop, running, started;
	uint32_t node, port, version, instance, generation;
	uint32_t level, wait_s, poll_s, dial_arg, ind_all, ind_invert, dump_arg;
	bool call_audio;                /* vayu_call_audio=1: the q6voice path follows the call's state */
	bool call_audio_up;             /* ... and it was asked for (the path is the audio module's) */
	/* vayu_call_auto=1: the client dials and ends the call itself (no shell on the phone) */
	uint32_t auto_arg;              /* vayu_call_auto=1 */
	uint32_t auto_delay_s;          /* vayu_call_auto_delay: seconds after the indications are registered */
	uint32_t auto_hold_s;           /* vayu_call_auto_end: seconds in CONVERSATION before End Call */
	bool auto_dialled, auto_ended;
	uint64_t auto_dial_ms, auto_conversation_ms;
	char allow[VAYU_VOICE_NUMBER_MAX];
	uint32_t state;                 /* VAYU_VOICE_* */
	bool registered;
	uint32_t register_tries;
	uint64_t next_register_ms;
	uint64_t next_poll_ms;
	uint64_t started_ms;
	bool said_missing;

	/* what the modem last said about the calls */
	struct vayu_voice_calls calls;

	/* lines for the reader */
	struct voice_line line[VAYU_VOICE_LINES];
	uint32_t line_head, line_tail;
	bool work;

	/* commands from write(), executed by the worker */
	struct voice_job job[VAYU_VOICE_LINES];
	uint32_t job_head, job_tail;

	/* the reply of the request being made: only the worker touches it */
	uint8_t raw[VOICE_RAW_MAX];
	uint32_t raw_length;

	/* counters */
	uint32_t indications, unknown_indications, incoming;
	uint32_t requests, failures;
	uint32_t last_result, last_error, last_message;
};

static struct voice_state voice;

static LCK_GRP_DECLARE(vayu_voice_lck_grp, "vayu-voice");
static LCK_MTX_DECLARE(vayu_voice_lock, &vayu_voice_lck_grp);

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

/* ---- the lines the device hands out -------------------------------------------------------------- */

/*
 * One line, with the lock held.  A full queue drops the oldest line rather than
 * the newest: what just happened matters more than what was never read.
 */
static void
voice_line_locked(const char *format, ...) __printflike(1, 2);

static void
voice_line_locked(const char *format, ...)
{
	struct voice_line *line;
	va_list list;
	uint32_t next = (voice.line_head + 1) % VAYU_VOICE_LINES;

	if (next == voice.line_tail) {
		voice.line_tail = (voice.line_tail + 1) % VAYU_VOICE_LINES;
	}
	line = &voice.line[voice.line_head];
	va_start(list, format);
	vsnprintf(line->text, sizeof(line->text), format, list);
	va_end(list);
	voice.line_head = next;
	wakeup(&voice.line_head);
}

static void
voice_line(const char *format, ...) __printflike(1, 2);

static void
voice_line(const char *format, ...)
{
	char text[VAYU_VOICE_LINE_MAX];
	va_list list;

	va_start(list, format);
	vsnprintf(text, sizeof(text), format, list);
	va_end(list);
	lck_mtx_lock(&vayu_voice_lock);
	voice_line_locked("%s", text);
	lck_mtx_unlock(&vayu_voice_lock);
}

static bool
voice_line_take_locked(char *text, size_t size)
{
	if (voice.line_tail == voice.line_head) {
		return false;
	}
	strlcpy(text, voice.line[voice.line_tail].text, size);
	voice.line_tail = (voice.line_tail + 1) % VAYU_VOICE_LINES;
	return true;
}

/* ---- the QMI client ------------------------------------------------------------------------------ */

static int
voice_new_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (service->service != VAYU_VOICE_QMI_SERVICE) {
		return -1;                      /* not one we asked for: the helpers forget it again */
	}
	lck_mtx_lock(&vayu_voice_lock);
	voice.node = service->node;
	voice.port = service->port;
	voice.version = service->version;
	voice.instance = service->instance;
	voice.present = true;
	voice.generation++;
	voice.registered = false;
	voice.register_tries = 0;
	voice.next_register_ms = 0;         /* the worker registers on its next pass */
	voice.state = voice.state < VAYU_VOICE_CONFIGURING ? VAYU_VOICE_CONFIGURING : voice.state;
	voice.work = true;
	wakeup(&voice.work);
	voice_line_locked("EVENT SERVICE present node=%u port=%u version=%u",
	    (unsigned int)service->node, (unsigned int)service->port, (unsigned int)service->version);
	lck_mtx_unlock(&vayu_voice_lock);
	return 0;
}

static void
voice_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (service->service != VAYU_VOICE_QMI_SERVICE) {
		return;
	}
	lck_mtx_lock(&vayu_voice_lock);
	if (service->node == voice.node && service->port == voice.port) {
		voice.present = false;
		voice.registered = false;
		voice.register_tries = 0;
		voice.state = VAYU_VOICE_WAITING;
		memset(&voice.calls, 0, sizeof(voice.calls));
		voice_line_locked("EVENT SERVICE gone");
		voice.work = true;
		wakeup(&voice.work);
	}
	lck_mtx_unlock(&vayu_voice_lock);
}

static const struct qmi_ops voice_qmi_ops = {
	.new_server = voice_new_server,
	.del_server = voice_del_server,
};

/*
 * One request and its reply.  Called only from the worker: an indication handler
 * must never wait, because the reply is delivered by the same QRTR thread that
 * called it.  The QMI result is checked here (0 only when the modem answered
 * success).  `raw`, when not NULL, receives the whole reply datagram for a TLV
 * walk; `raw_length` is then set (0 when it did not fit or no reply came).
 */
static int
voice_request(const char *what, uint16_t message, const struct qmi_elem_info *req_ei, const void *req,
    struct vayu_voice_qmi_result *result, uint32_t *call_id, bool quiet)
{
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = voice.node, .sq_port = voice.port };
	struct vayu_voice_qmi_call_id_resp response;
	struct qmi_txn txn;
	int ret;

	memset(&response, 0, sizeof(response));
	if (voice.stop) {
		return -ECANCELED;
	}
	voice.requests++;
	ret = qmi_txn_init(&voice.qmi, &txn, vayu_voice_qmi_call_id_resp_ei, &response);
	if (ret < 0) {
		voice.failures++;
		if (!quiet) {
			say("vayu-voice: %s: no transaction: %d\n", what, ret);
		}
		return ret;
	}
	txn.raw = voice.raw;
	txn.raw_size = sizeof(voice.raw);
	txn.raw_length = 0;
	ret = qmi_send_request(&voice.qmi, &to, &txn, message, VAYU_VOICE_QMI_REQUEST_MAX, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		voice.failures++;
		if (!quiet) {
			say("vayu-voice: %s: cannot send: %d\n", what, ret);
		}
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, VOICE_REPLY_MS);
	voice.raw_length = ret < 0 ? 0 : (uint32_t)txn.raw_length;
	if (ret < 0) {
		voice.failures++;
		if (!quiet) {
			if (ret == -ETIMEDOUT) {
				say("vayu-voice: %s: no reply in %u ms\n", what, VOICE_REPLY_MS);
			} else {
				say("vayu-voice: %s: the reply cannot be decoded (%d)\n", what, ret);
			}
		}
		return ret;
	}
	if (result != NULL) {
		*result = response.result;
		voice.last_result = response.result.result;
		voice.last_error = response.result.error;
		voice.last_message = message;
		if (!vayu_voice_qmi_succeeded(&response.result)) {
			voice.failures++;
			if (!quiet) {
				say("vayu-voice: %s: the modem says result %u error %u\n", what,
				    (unsigned int)response.result.result, (unsigned int)response.result.error);
			}
			return -EIO;
		}
	}
	if (call_id != NULL && response.call_id_valid) {
		*call_id = response.call_id;
	}
	return 0;
}

/* ---- what the modem says about the calls --------------------------------------------------------- */

/* The TLV list as "0x01/14 0x10/5", and with vayu_call_dump=1 the bytes after it. */
static void
voice_describe_indication(uint32_t id, const uint8_t *message, size_t length, char *text, size_t size)
{
	struct vayu_voice_tlv tlv;
	size_t at = QMI_HEADER_SIZE, used = 0;
	uint32_t count = 0;

	used = (size_t)snprintf(text, size, "indication 0x%04x (%u bytes):", (unsigned int)id, (unsigned int)length);
	while (used < size && vayu_voice_tlv_next(message, length, &at, &tlv) && count < 12) {
		int wrote = snprintf(text + used, size - used, " 0x%02x/%u", tlv.type, (unsigned int)tlv.length);

		if (wrote < 0 || (size_t)wrote >= size - used) {
			break;
		}
		used += (size_t)wrote;
		count++;
	}
	if (used < size && count == 0) {
		snprintf(text + used, size - used, " (no TLV)");
	}
}

static void
voice_dump_indication(const uint8_t *message, size_t length, char *text, size_t size)
{
	size_t used = 0;
	size_t shown = length < VOICE_DUMP_BYTES ? length : VOICE_DUMP_BYTES;

	if (size < 3) {
		return;
	}
	used = (size_t)snprintf(text, size, "%s", " bytes:");
	for (size_t i = QMI_HEADER_SIZE; i < shown && used + 4 < size; i++) {
		int wrote = snprintf(text + used, size - used, " %02x", message[i]);

		if (wrote < 0) {
			break;
		}
		used += (size_t)wrote;
	}
	if (used + 5 < size && shown < length) {
		snprintf(text + used, size - used, " ...");
	}
}

/* The old report's call with that id, or NULL. */
static const struct vayu_voice_call *
voice_old_call_locked(uint8_t id)
{
	for (uint32_t i = 0; i < voice.calls.count; i++) {
		if (voice.calls.call[i].id == id) {
			return &voice.calls.call[i];
		}
	}
	return NULL;
}

/*
 * Take a call report: push an EVENT line for every call that is new or has
 * changed, note an incoming call, and remember the report.  Returns the number of
 * notes (lines) made; `notes` is filled for the caller to log after the lock is
 * let go.
 */
static uint32_t
voice_note_calls(struct vayu_voice_calls *calls, struct voice_note *notes, uint32_t max_notes, bool *seen_flag)
{
	uint32_t made = 0;
	uint32_t path = 0;                      /* vayu_voice=2 (VoLTE) or 3 (CS) of vayu_audio.c */
	bool conversation = false;

	lck_mtx_lock(&vayu_voice_lock);
	for (uint32_t i = 0; i < calls->count; i++) {
		if (calls->call[i].state == VAYU_VOICE_STATE_CONVERSATION) {
			conversation = true;
			path = (calls->call[i].mode == VAYU_VOICE_MODE_LTE ||
			    calls->call[i].type == VAYU_VOICE_TYPE_VOICE_IP) ? 2 : 3;
			break;
		}
	}
	for (uint32_t i = 0; i < calls->count && made < max_notes; i++) {
		const struct vayu_voice_call *call = &calls->call[i];
		const struct vayu_voice_call *old = voice_old_call_locked(call->id);
		bool changed = old == NULL || old->state != call->state || old->type != call->type ||
		    old->direction != call->direction || old->mode != call->mode ||
		    strcmp(old->number, call->number) != 0;
		bool incoming = call->direction == VAYU_VOICE_DIRECTION_MT &&
		    (call->state == VAYU_VOICE_STATE_INCOMING || call->state == VAYU_VOICE_STATE_WAITING ||
		    call->state == VAYU_VOICE_STATE_ALERTING);
		bool old_incoming = old != NULL && (old->state == VAYU_VOICE_STATE_INCOMING ||
		    old->state == VAYU_VOICE_STATE_WAITING || old->state == VAYU_VOICE_STATE_ALERTING);

		if (!changed) {
			continue;
		}
		voice_line_locked("EVENT CALL id=%u state=%s dir=%s type=%s mode=%s number=%s",
		    (unsigned int)call->id, vayu_voice_state_name(call->state),
		    vayu_voice_direction_name(call->direction), vayu_voice_type_name(call->type),
		    vayu_voice_mode_name(call->mode), call->number[0] != 0 ? call->number : "unknown");
		notes[made].kind = 1;
		notes[made].id = call->id;
		notes[made].state = call->state;
		notes[made].direction = call->direction;
		notes[made].mode = call->mode;
		notes[made].type = call->type;
		vayu_voice_mask_tail(call->number, notes[made].tail);
		made++;
		/*
		 * An MT call is "incoming" at its first state that is one of incoming,
		 * waiting or alerting: some networks report alerting first, without an
		 * incoming before it, and that is still the phone ringing.
		 */
		if (incoming && !old_incoming) {
			voice.incoming++;
			voice_line_locked("EVENT INCOMING id=%u number=%s mode=%s", (unsigned int)call->id,
			    call->number[0] != 0 ? call->number : "unknown", vayu_voice_mode_name(call->mode));
			if (made < max_notes) {
				notes[made] = notes[made - 1];
				notes[made].kind = 2;
				made++;
			}
		}
	}
	/* a call the modem no longer lists has ended */
	for (uint32_t i = 0; i < voice.calls.count && made < max_notes; i++) {
		const struct vayu_voice_call *old = &voice.calls.call[i];
		bool there = false;

		for (uint32_t c = 0; c < calls->count; c++) {
			if (calls->call[c].id == old->id) {
				there = true;
				break;
			}
		}
		if (there) {
			continue;
		}
		voice_line_locked("EVENT CALL id=%u state=gone", (unsigned int)old->id);
		notes[made].kind = 3;
		notes[made].id = old->id;
		notes[made].state = old->state;
		notes[made].direction = old->direction;
		notes[made].mode = old->mode;
		notes[made].type = old->type;
		vayu_voice_mask_tail(old->number, notes[made].tail);
		made++;
	}
	voice.calls = *calls;
	if (seen_flag != NULL) {
		*seen_flag = true;
	}
	lck_mtx_unlock(&vayu_voice_lock);
	/*
	 * vayu_call_audio=1: the q6voice path follows the call.  CONVERSATION is where the modem's voice
	 * stream (MVS) is attached and audio has to flow; the DSP path is the audio module's (it is
	 * brought up and taken down in its own thread, so this one never waits).  A call in HOLD keeps
	 * its path: only "no call in conversation at all" takes it down.
	 */
	if (voice.call_audio) {
		if (conversation) {
			if (!vayu_audio_call_audio_up()) {
				/* (the audio module ignores a request it already has; a failed one is
				 * retried here on the next report of the call) */
				vayu_audio_call_audio_start(path);
				if (!voice.call_audio_up) {
					voice.call_audio_up = true;
					say("vayu-voice: a call is in conversation: the q6voice path is asked for "
					    "(vayu_voice=%u, vayu_call_audio=1)\n", path);
				}
			} else {
				voice.call_audio_up = true;
			}
		} else if (voice.call_audio_up || vayu_audio_call_audio_up()) {
			voice.call_audio_up = false;
			vayu_audio_call_audio_stop();
			say("vayu-voice: no call is in conversation: the q6voice path goes down "
			    "(vayu_call_audio=1)\n");
		}
	}
	return made;
}

static void
voice_log_notes(const struct voice_note *notes, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		const struct voice_note *note = &notes[i];

		if (note->kind == 1) {
			say("vayu-voice: call id %u: %s, %s, %s, %s, number %s\n", (unsigned int)note->id,
			    vayu_voice_state_name(note->state), vayu_voice_direction_name(note->direction),
			    vayu_voice_type_name(note->type), vayu_voice_mode_name(note->mode), note->tail);
		} else if (note->kind == 2) {
			say("vayu-voice: INCOMING CALL: id %u, number %s, mode %s\n", (unsigned int)note->id,
			    note->tail, vayu_voice_mode_name(note->mode));
		} else {
			say("vayu-voice: call id %u: gone (the modem no longer lists it; last state %s)\n",
			    (unsigned int)note->id, vayu_voice_state_name(note->state));
		}
	}
}

/* A reply datagram into a call report: TLV 0x01 the list, 0x10/0x11 the numbers. */
static bool
voice_decode_calls(const uint8_t *message, size_t length, struct vayu_voice_calls *calls)
{
	struct vayu_voice_tlv list, tlv;

	if (!vayu_voice_tlv_find(message, length, 0x01, &list)) {
		return false;
	}
	/*
	 * vayu_call_dump=2: the call-list bytes only.  They are {id, state, type, direction, mode,
	 * multipart, als} (or 8-byte entries), never a number, so this is the one dump that is safe
	 * to leave in a run and that says whether this firmware's field order is the one the table
	 * names.  The numbers (TLV 0x10/0x11) are still not printed.
	 */
	if (voice.dump_arg == 2) {
		char hex[3 * 24 + 8];
		size_t used = 0;

		hex[0] = 0;
		for (size_t i = 0; i < list.length && i < 24 && used + 5 < sizeof(hex); i++) {
			int wrote = snprintf(hex + used, sizeof(hex) - used, " %02x", list.value[i]);

			if (wrote < 0) {
				break;
			}
			used += (size_t)wrote;
		}
		say("vayu-voice: call list TLV 0x01 (%u bytes):%s\n", (unsigned int)list.length, hex);
	}
	if (vayu_voice_call_list_decode(list.value, list.length, calls) < 0) {
		return false;
	}
	if (vayu_voice_tlv_find(message, length, 0x10, &tlv)) {
		(void)vayu_voice_call_numbers_decode(tlv.value, tlv.length, calls);
	}
	if (vayu_voice_tlv_find(message, length, 0x11, &tlv)) {
		(void)vayu_voice_call_numbers_decode(tlv.value, tlv.length, calls);
	}
	return true;
}

/* ---- the indications ----------------------------------------------------------------------------- */

/*
 * Every indication the modem sends reaches this: the handler is a wildcard (no
 * message id and no table), which is what makes the client honest about the one
 * id it does not know.  It runs on the QRTR thread: it must not wait, and it does
 * not -- it decodes, queues lines and returns.
 */
static void
voice_indication(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	const uint8_t *message = decoded;
	size_t length = txn != NULL ? txn->raw_length : 0;
	struct vayu_voice_calls calls;
	struct voice_note notes[VOICE_NOTE_MAX];
	uint32_t made = 0;
	uint32_t id;
	bool known = false;
	char text[VAYU_VOICE_LINE_MAX];

	if (message == NULL || length < QMI_HEADER_SIZE) {
		return;
	}
	if (voice.present && sq != NULL && (sq->sq_port != voice.port || sq->sq_node != voice.node)) {
		return;                         /* not the VOICE service: nothing of ours */
	}
	id = (uint32_t)(message[3] | (uint32_t)message[4] << 8);

	lck_mtx_lock(&vayu_voice_lock);
	voice.indications++;
	lck_mtx_unlock(&vayu_voice_lock);

	voice_describe_indication(id, message, length, text, sizeof(text));
	if (voice.dump_arg == 1) {
		size_t used = strlen(text);

		voice_dump_indication(message, length, text + used, sizeof(text) - used);
	}
	say("vayu-voice: %s\n", text);

	memset(&calls, 0, sizeof(calls));
	if (voice_decode_calls(message, length, &calls)) {
		known = true;
		made = voice_note_calls(&calls, notes, VOICE_NOTE_MAX, NULL);
	} else if (id == VAYU_VOICE_QMI_ALL_CALL_STATUS_IND) {
		lck_mtx_lock(&vayu_voice_lock);
		voice.unknown_indications++;
		lck_mtx_unlock(&vayu_voice_lock);
		say("vayu-voice: the all-call-status indication (0x2e) carried no call list this client can "
		    "read; the TLV list above says what it did carry (vayu_call_dump=1 prints the bytes)\n");
	} else if (id != VAYU_VOICE_QMI_SUPPLEMENTARY_SERVICE_IND && id != VAYU_VOICE_QMI_USSD_IND &&
	    id != VAYU_VOICE_QMI_RELEASE_USSD_IND) {
		lck_mtx_lock(&vayu_voice_lock);
		voice.unknown_indications++;
		lck_mtx_unlock(&vayu_voice_lock);
		say("vayu-voice: indication 0x%04x is not one this client knows; the TLV list above says what "
		    "it is (vayu_call_dump=1 prints the bytes)\n", (unsigned int)id);
	}
	voice_log_notes(notes, made);
	(void)known;
}

static const struct qmi_msg_handler voice_handlers[] = {
	{
		.type = QMI_INDICATION,
		.any = true,                    /* no id, no table: the client reads the datagram itself */
		.fn = voice_indication,
	},
	{ .fn = NULL },
};

/* ---- the one thing the client may send without being asked: the register ------------------------ */

static void
voice_register(void)
{
	struct vayu_voice_qmi_indication_register_req request;
	struct vayu_voice_qmi_result result;
	uint32_t generation = voice.generation;
	int ret;

	if (voice.register_tries == 0) {
		say("vayu-voice: the VOICE service (0x09) is there: node %u port %u version %u; registering the indications\n",
		    (unsigned int)voice.node, (unsigned int)voice.port, (unsigned int)voice.version);
	}
	vayu_voice_qmi_indication_register_fill(&request, voice.ind_all != 0, voice.ind_invert != 0);
	voice.register_tries++;
	ret = voice_request("indication register", VAYU_VOICE_QMI_INDICATION_REGISTER,
	    vayu_voice_qmi_indication_register_req_ei, &request, &result, NULL, false);
	if (voice.generation != generation) {
		return;                         /* the service went and came again: start over */
	}
	lck_mtx_lock(&vayu_voice_lock);
	if (ret == 0) {
		voice.registered = true;
		voice.state = VAYU_VOICE_READY;
		voice.next_poll_ms = now_ms() + (uint64_t)voice.poll_s * 1000;
		if (voice.auto_arg != 0) {
			voice.auto_dial_ms = now_ms() + (uint64_t)voice.auto_delay_s * 1000;
		}
		voice_line_locked("EVENT READY indications=registered");
		voice.work = true;
		wakeup(&voice.work);
	} else {
		voice.registered = false;
		voice.state = VAYU_VOICE_FAILED;
		voice.next_register_ms = now_ms() + 15000;
	}
	lck_mtx_unlock(&vayu_voice_lock);
	if (ret == 0) {
		say("vayu-voice: indication register: %s%s%s: accepted; the modem will report calls to this client\n",
		    "call notifications (0x13), supplementary service (0x12), USSD (0x16)",
		    voice.ind_all != 0 ? " and every other group" : "",
		    voice.ind_invert != 0 ? " (flags inverted)" : "");
	} else {
		say("vayu-voice: indication register: refused (%d); attempt %u of %u%s\n", ret,
		    (unsigned int)voice.register_tries, (unsigned int)VOICE_REGISTER_TRIES,
		    voice.register_tries >= VOICE_REGISTER_TRIES ? "; no more attempts until the service returns" : "");
	}
}

/* ---- the commands ------------------------------------------------------------------------------- */

static int
voice_get_calls(const char *what, bool quiet)
{
	struct vayu_voice_calls calls;
	struct voice_note notes[VOICE_NOTE_MAX];
	struct vayu_voice_qmi_result result;
	uint32_t made;
	int ret;

	ret = voice_request(what, VAYU_VOICE_QMI_GET_ALL_CALL_INFO, NULL, NULL, &result, NULL, quiet);
	if (ret != 0) {
		return ret;
	}
	if (voice.raw_length < QMI_HEADER_SIZE || voice.raw[0] != QMI_RESPONSE) {
		return -EIO;
	}
	memset(&calls, 0, sizeof(calls));
	if (!voice_decode_calls(voice.raw, voice.raw_length, &calls)) {
		lck_mtx_lock(&vayu_voice_lock);
		voice.unknown_indications++;
		lck_mtx_unlock(&vayu_voice_lock);
		say("vayu-voice: %s: the reply has no call list this client can read (TLVs are in the "
		    "indication log above)\n", what);
		return -EIO;
	}
	made = voice_note_calls(&calls, notes, VOICE_NOTE_MAX, NULL);
	voice_log_notes(notes, made);
	return (int)calls.count;
}

static void
voice_print_calls_locked(void)
{
	voice_line_locked("OK calls=%u", (unsigned int)voice.calls.count);
	for (uint32_t i = 0; i < voice.calls.count; i++) {
		const struct vayu_voice_call *call = &voice.calls.call[i];

		voice_line_locked("CALL id=%u state=%s dir=%s type=%s mode=%s multi=%u als=%u number=%s",
		    (unsigned int)call->id, vayu_voice_state_name(call->state),
		    vayu_voice_direction_name(call->direction), vayu_voice_type_name(call->type),
		    vayu_voice_mode_name(call->mode), (unsigned int)call->multipart,
		    (unsigned int)call->als, call->number[0] != 0 ? call->number : "unknown");
	}
}

static bool
voice_dial_ok(const char *number)
{
	if (voice.level < 2) {
		voice_line("ERR dial: vayu_call=1 is indications only (vayu_call=2 allows the call commands)");
		return false;
	}
	if (voice.dial_arg == 0) {
		voice_line("ERR dial: dialling is not allowed (vayu_call_dial=1 is not set)");
		return false;
	}
	if (!vayu_voice_digits_ok(number, VAYU_VOICE_NUMBER_MAX - 1)) {
		voice_line("ERR dial: the number is not a dialable number");
		return false;
	}
	if (strcmp(number, voice.allow) != 0) {
		voice_line("ERR dial: only the owner-approved number is allowed (one number, see vayu_call_allow)");
		return false;
	}
	return true;
}

static void
voice_run_job(struct voice_job *job)
{
	struct vayu_voice_qmi_result result;
	struct vayu_voice_qmi_call_id_req one;
	struct vayu_voice_qmi_dtmf_req dtmf;
	struct vayu_voice_qmi_dial_req dial;
	char tail[8];
	uint32_t call_id = 0;
	int ret;
	static const char help[] =
	    "commands: help, status, calls, info <id>, answer <id>, end <id>, dtmf <id> <digits>, "
	    "dtmfstart <id> <digit>, dtmfstop <id>, flash <id>, dial <number>";

	/* help and status are answered even before the service, so the device can be
	 * asked what is wrong when nothing else works */
	if (job->kind != VOICE_JOB_HELP && job->kind != VOICE_JOB_STATUS &&
	    (!voice.present || !voice.registered)) {
		voice_line("ERR %s: the VOICE service is %s",
		    job->kind == VOICE_JOB_DIAL ? "dial" : "command",
		    voice.present ? "there but not registered for indications" : "not there");
		return;
	}

	switch (job->kind) {
	case VOICE_JOB_HELP:
		voice_line("OK %s", help);
		return;
	case VOICE_JOB_STATUS: {
		struct vayu_voice_status status;

		memset(&status, 0, sizeof(status));
		lck_mtx_lock(&vayu_voice_lock);
		status.magic = VAYU_VOICE_MAGIC;
		status.version = 1;
		status.size = sizeof(status);
		status.state = voice.state;
		status.present = voice.present ? 1 : 0;
		status.registered = voice.registered ? 1 : 0;
		status.indications = voice.indications;
		status.unknown_indications = voice.unknown_indications;
		status.calls = voice.calls.count;
		status.incoming = voice.incoming;
		status.requests = voice.requests;
		status.failures = voice.failures;
		status.dial_allowed = voice.dial_arg;
		status.commands_allowed = voice.level >= 2 ? 1 : 0;
		status.last_result = voice.last_result;
		status.last_error = voice.last_error;
		status.last_message = voice.last_message;
		voice_line_locked("OK status: service=%s registered=%u indications=%u unknown=%u calls=%u "
		    "incoming=%u requests=%u failures=%u dial=%u commands=%u last=0x%04x/%u/%u",
		    voice.present ? "present" : "absent", (unsigned int)status.registered,
		    (unsigned int)status.indications, (unsigned int)status.unknown_indications,
		    (unsigned int)status.calls, (unsigned int)status.incoming, (unsigned int)status.requests,
		    (unsigned int)status.failures, (unsigned int)status.dial_allowed,
		    (unsigned int)status.commands_allowed, (unsigned int)status.last_message,
		    (unsigned int)status.last_result, (unsigned int)status.last_error);
		lck_mtx_unlock(&vayu_voice_lock);
		return;
	}
	case VOICE_JOB_CALLS:
		ret = voice_get_calls("get all call info", false);
		if (ret < 0) {
			voice_line("ERR calls: the modem did not answer with a call list (%d)", ret);
			return;
		}
		lck_mtx_lock(&vayu_voice_lock);
		voice_print_calls_locked();
		lck_mtx_unlock(&vayu_voice_lock);
		return;
	case VOICE_JOB_INFO:
		ret = voice_get_calls("get all call info", false);
		if (ret < 0) {
			voice_line("ERR info: the modem did not answer with a call list (%d)", ret);
			return;
		}
		one.call_id = (uint8_t)job->call_id;
		ret = voice_request("get call info", VAYU_VOICE_QMI_GET_CALL_INFO, vayu_voice_qmi_call_id_req_ei,
		    &one, &result, NULL, false);
		if (ret != 0) {
			voice_line("ERR info: call %u: the modem refused (%d)", (unsigned int)job->call_id, ret);
			return;
		}
		if (voice.raw[0] == QMI_RESPONSE && voice.raw_length > QMI_HEADER_SIZE) {
			struct vayu_voice_calls calls;
			struct vayu_voice_tlv tlv;

			memset(&calls, 0, sizeof(calls));
			if (vayu_voice_tlv_find(voice.raw, voice.raw_length, 0x01, &tlv) &&
			    vayu_voice_call_list_decode(tlv.value, tlv.length, &calls) >= 0) {
				if (vayu_voice_tlv_find(voice.raw, voice.raw_length, 0x10, &tlv)) {
					(void)vayu_voice_call_numbers_decode(tlv.value, tlv.length, &calls);
				}
				for (uint32_t i = 0; i < calls.count; i++) {
					const struct vayu_voice_call *call = &calls.call[i];

					voice_line("OK info id=%u state=%s dir=%s type=%s mode=%s number=%s",
					    (unsigned int)call->id, vayu_voice_state_name(call->state),
					    vayu_voice_direction_name(call->direction),
					    vayu_voice_type_name(call->type),
					    vayu_voice_mode_name(call->mode),
					    call->number[0] != 0 ? call->number : "unknown");
				}
				return;
			}
		}
		voice_line("OK info: call %u answered; the reply has no call list this client reads",
		    (unsigned int)job->call_id);
		return;
	case VOICE_JOB_DIAL:
		if (!voice_dial_ok(job->text)) {
			return;
		}
		memset(&dial, 0, sizeof(dial));
		strlcpy(dial.number, job->text, sizeof(dial.number));
		vayu_voice_mask_tail(dial.number, tail);
		say("vayu-voice: dialling %s (the one number the owner approved)\n", tail);
		ret = voice_request("dial call", VAYU_VOICE_QMI_DIAL_CALL, vayu_voice_qmi_dial_req_ei, &dial,
		    &result, &call_id, false);
		if (ret != 0) {
			voice_line("ERR dial %s: the modem refused (%d)", tail, ret);
			/*
			 * The modem may have listed the call in origination before it refused the
			 * Dial (that is what this firmware does on QMI_ERR_NO_SERVICE).  It has no
			 * call after the refusal -- End Call answers QMI_ERR_INVALID_ID -- so the
			 * stale entry is dropped here and no phantom call is reported.
			 */
			lck_mtx_lock(&vayu_voice_lock);
			memset(&voice.calls, 0, sizeof(voice.calls));
			lck_mtx_unlock(&vayu_voice_lock);
			say("vayu-voice: the dial was refused: the call list is dropped (the modem has no call)\n");
			return;
		}
		voice_line("OK dial %s: accepted, call id %u", tail, (unsigned int)call_id);
		say("vayu-voice: the dial was accepted: call id %u\n", (unsigned int)call_id);
		return;
	case VOICE_JOB_ANSWER:
	case VOICE_JOB_END:
	case VOICE_JOB_FLASH:
		if (voice.level < 2) {
			voice_line("ERR %s: vayu_call=1 is indications only (vayu_call=2 allows the call commands)",
			    job->kind == VOICE_JOB_ANSWER ? "answer" : job->kind == VOICE_JOB_END ? "end" : "flash");
			return;
		}
		one.call_id = (uint8_t)job->call_id;
		ret = voice_request(job->kind == VOICE_JOB_ANSWER ? "answer call" :
		    job->kind == VOICE_JOB_END ? "end call" : "send flash",
		    job->kind == VOICE_JOB_ANSWER ? VAYU_VOICE_QMI_ANSWER_CALL :
		    job->kind == VOICE_JOB_END ? VAYU_VOICE_QMI_END_CALL : VAYU_VOICE_QMI_SEND_FLASH,
		    vayu_voice_qmi_call_id_req_ei, &one, &result, NULL, false);
		if (ret != 0) {
			voice_line("ERR %s %u: the modem refused (%d)",
			    job->kind == VOICE_JOB_ANSWER ? "answer" : job->kind == VOICE_JOB_END ? "end" : "flash",
			    (unsigned int)job->call_id, ret);
			return;
		}
		voice_line("OK %s %u", job->kind == VOICE_JOB_ANSWER ? "answer" :
		    job->kind == VOICE_JOB_END ? "end" : "flash", (unsigned int)job->call_id);
		return;
	case VOICE_JOB_DTMF:
		if (voice.level < 2) {
			voice_line("ERR dtmf: vayu_call=1 is indications only (vayu_call=2 allows the call commands)");
			return;
		}
		if (job->length == 0) {
			voice_line("ERR dtmf: no digits");
			return;
		}
		for (uint32_t i = 0; i < job->length; i++) {
			memset(&dtmf, 0, sizeof(dtmf));
			dtmf.call_id = (uint8_t)job->call_id;
			dtmf.digit = (uint8_t)job->text[i];
			ret = voice_request("burst dtmf", VAYU_VOICE_QMI_BURST_DTMF,
			    vayu_voice_qmi_dtmf_req_ei, &dtmf, &result, NULL, false);
			if (ret != 0) {
				voice_line("ERR dtmf %u: digit %u refused (%d)", (unsigned int)job->call_id,
				    (unsigned int)(i + 1), ret);
				return;
			}
		}
		voice_line("OK dtmf %u: %u digit(s) sent", (unsigned int)job->call_id, (unsigned int)job->length);
		return;
	case VOICE_JOB_DTMF_START:
	case VOICE_JOB_DTMF_STOP:
		if (voice.level < 2) {
			voice_line("ERR dtmf: vayu_call=1 is indications only (vayu_call=2 allows the call commands)");
			return;
		}
		memset(&dtmf, 0, sizeof(dtmf));
		dtmf.call_id = (uint8_t)job->call_id;
		dtmf.digit = (uint8_t)(job->length > 0 ? job->text[0] : '0');
		ret = voice_request(job->kind == VOICE_JOB_DTMF_START ? "start continuous dtmf" : "stop continuous dtmf",
		    job->kind == VOICE_JOB_DTMF_START ? VAYU_VOICE_QMI_START_CONT_DTMF : VAYU_VOICE_QMI_STOP_CONT_DTMF,
		    vayu_voice_qmi_dtmf_req_ei, &dtmf, &result, NULL, false);
		if (ret != 0) {
			voice_line("ERR %s %u: the modem refused (%d)",
			    job->kind == VOICE_JOB_DTMF_START ? "dtmfstart" : "dtmfstop",
			    (unsigned int)job->call_id, ret);
			return;
		}
		voice_line("OK %s %u", job->kind == VOICE_JOB_DTMF_START ? "dtmfstart" : "dtmfstop",
		    (unsigned int)job->call_id);
		return;
	default:
		voice_line("ERR unknown command (help lists them)");
		return;
	}
}

/* ---- the text protocol's input ------------------------------------------------------------------- */

/* One word of a command: spaces skipped, copies until the next space and returns true if there was one. */
static bool
voice_word(const char **rest, char *word, size_t size)
{
	const char *p = *rest;
	size_t used = 0;

	while (*p == ' ' || *p == '\t' || *p == '\r') {
		p++;
	}
	if (*p == 0) {
		*rest = p;
		return false;
	}
	while (*p != 0 && *p != ' ' && *p != '\t' && *p != '\r' && used + 1 < size) {
		word[used++] = *p++;
	}
	word[used] = 0;
	while (*p != 0 && *p != ' ' && *p != '\t' && *p != '\r') {
		p++;                            /* an argument longer than the word is refused by its user */
	}
	*rest = p;
	return true;
}

static bool
voice_number_arg(const char *text, uint32_t *value)
{
	uint32_t result = 0;

	if (text == NULL || *text == 0) {
		return false;
	}
	for (const char *p = text; *p != 0; p++) {
		if (*p < '0' || *p > '9' || result > 409) {
			return false;
		}
		result = result * 10 + (uint32_t)(*p - '0');
	}
	*value = result;
	return true;
}

static void
voice_queue(struct voice_job *job)
{
	uint32_t next = (voice.job_head + 1) % VAYU_VOICE_LINES;

	lck_mtx_lock(&vayu_voice_lock);
	if (next == voice.job_tail) {
		voice_line_locked("ERR busy: the command queue is full");
		lck_mtx_unlock(&vayu_voice_lock);
		return;
	}
	voice.job[voice.job_head] = *job;
	voice.job_head = next;
	voice.work = true;
	wakeup(&voice.work);
	lck_mtx_unlock(&vayu_voice_lock);
}

static void
voice_command(const char *line)
{
	struct voice_job job;
	const char *rest = line;
	char verb[16];
	char word[VAYU_VOICE_DIGITS_MAX];

	memset(&job, 0, sizeof(job));
	if (!voice_word(&rest, verb, sizeof(verb))) {
		return;
	}
	if (strcmp(verb, "help") == 0) {
		job.kind = VOICE_JOB_HELP;
	} else if (strcmp(verb, "status") == 0) {
		job.kind = VOICE_JOB_STATUS;
	} else if (strcmp(verb, "calls") == 0) {
		job.kind = VOICE_JOB_CALLS;
	} else if (strcmp(verb, "dial") == 0) {
		if (!voice_word(&rest, word, sizeof(word))) {
			voice_line("ERR dial: no number");
			return;
		}
		job.kind = VOICE_JOB_DIAL;
		strlcpy(job.text, word, sizeof(job.text));
	} else if (strcmp(verb, "info") == 0 || strcmp(verb, "answer") == 0 || strcmp(verb, "end") == 0 ||
	    strcmp(verb, "flash") == 0 || strcmp(verb, "dtmfstart") == 0 || strcmp(verb, "dtmfstop") == 0) {
		uint32_t id;

		if (!voice_word(&rest, word, sizeof(word)) || !voice_number_arg(word, &id)) {
			voice_line("ERR %s: a call id is needed", verb);
			return;
		}
		job.call_id = id;
		if (strcmp(verb, "info") == 0) {
			job.kind = VOICE_JOB_INFO;
		} else if (strcmp(verb, "answer") == 0) {
			job.kind = VOICE_JOB_ANSWER;
		} else if (strcmp(verb, "end") == 0) {
			job.kind = VOICE_JOB_END;
		} else if (strcmp(verb, "flash") == 0) {
			job.kind = VOICE_JOB_FLASH;
		} else if (strcmp(verb, "dtmfstart") == 0) {
			job.kind = VOICE_JOB_DTMF_START;
			if (!voice_word(&rest, word, sizeof(word)) || !vayu_voice_digits_ok(word, 1)) {
				voice_line("ERR dtmfstart: one digit is needed");
				return;
			}
			job.length = 1;
			job.text[0] = word[0];
		} else {
			job.kind = VOICE_JOB_DTMF_STOP;
		}
	} else if (strcmp(verb, "dtmf") == 0) {
		uint32_t id;

		if (!voice_word(&rest, word, sizeof(word)) || !voice_number_arg(word, &id)) {
			voice_line("ERR dtmf: a call id is needed");
			return;
		}
		job.call_id = id;
		if (!voice_word(&rest, word, sizeof(word))) {
			voice_line("ERR dtmf: no digits");
			return;
		}
		if (!vayu_voice_digits_ok(word, VAYU_VOICE_DIGITS_MAX - 1)) {
			voice_line("ERR dtmf: the digits are not digits");
			return;
		}
		job.kind = VOICE_JOB_DTMF;
		job.length = (uint32_t)strlen(word);
		strlcpy(job.text, word, sizeof(job.text));
	} else {
		voice_line("ERR unknown command \"%s\" (help lists them)", verb);
		return;
	}
	voice_queue(&job);
}

/* ---- vayu_call_auto: the call without a shell --------------------------------------------------- */

/*
 * vayu_call_auto=1: once the indications are registered, wait vayu_call_auto_delay seconds (default
 * 30, by when the modem is normally registered too), dial the one allowed number, watch the call
 * reports for CONVERSATION, hold it vayu_call_auto_end seconds (default 45) and send End Call.  If
 * the call never reaches conversation, the End is sent vayu_call_auto_end seconds after the dial,
 * and a modem that lists no call at all is simply noted.  Every step is a log line with the same
 * masked number the manual `dial` prints; no number is ever written to the log.
 *
 * Runs in the worker, between the register/poll passes: it uses the same jobs the device uses, so
 * there is one implementation of dial and end, not two.
 */
static bool
voice_auto_step(void)
{
	struct voice_job job;
	uint64_t now = now_ms();
	uint64_t dial_at, conversation_at;
	uint8_t conversation = 0, first = 0;
	bool dialled, ended;
	uint32_t count;

	lck_mtx_lock(&vayu_voice_lock);
	count = voice.calls.count;
	for (uint32_t i = 0; i < count; i++) {
		if (first == 0) {
			first = voice.calls.call[i].id;
		}
		if (voice.calls.call[i].state == VAYU_VOICE_STATE_CONVERSATION) {
			conversation = voice.calls.call[i].id;
			break;
		}
	}
	dialled = voice.auto_dialled;
	ended = voice.auto_ended;
	dial_at = voice.auto_dial_ms;
	conversation_at = voice.auto_conversation_ms;
	lck_mtx_unlock(&vayu_voice_lock);

	if (ended || !voice.registered) {
		return false;
	}
	if (!dialled) {
		if (now < dial_at) {
			return false;
		}
		memset(&job, 0, sizeof(job));
		job.kind = VOICE_JOB_DIAL;
		strlcpy(job.text, voice.allow, sizeof(job.text));
		lck_mtx_lock(&vayu_voice_lock);
		voice.auto_dialled = true;
		voice.auto_dial_ms = now;
		lck_mtx_unlock(&vayu_voice_lock);
		say("vayu-voice: auto: %u s after READY: the call is dialled (vayu_call_auto=1, no shell)\n",
		    voice.auto_delay_s);
		voice_run_job(&job);
		return true;
	}
	if (conversation != 0) {
		if (conversation_at == 0) {
			lck_mtx_lock(&vayu_voice_lock);
			voice.auto_conversation_ms = now;
			lck_mtx_unlock(&vayu_voice_lock);
			say("vayu-voice: auto: call %u is in CONVERSATION: it is held %u s and then ended "
			    "(vayu_call_auto_end)\n", (unsigned int)conversation, voice.auto_hold_s);
			return false;
		}
		if (now >= conversation_at + (uint64_t)voice.auto_hold_s * 1000) {
			memset(&job, 0, sizeof(job));
			job.kind = VOICE_JOB_END;
			job.call_id = conversation;
			lck_mtx_lock(&vayu_voice_lock);
			voice.auto_ended = true;
			lck_mtx_unlock(&vayu_voice_lock);
			say("vayu-voice: auto: %u s of CONVERSATION: End Call for call %u is sent\n",
			    voice.auto_hold_s, (unsigned int)conversation);
			voice_run_job(&job);
			return true;
		}
		return false;
	}
	/*
	 * No conversation.  A call the modem still lists is ended after the hold window has run from
	 * the dial (so a slow ALERTING is not cut short); a modem that lists nothing is not asked.
	 */
	if (now >= dial_at + (uint64_t)voice.auto_hold_s * 1000) {
		lck_mtx_lock(&vayu_voice_lock);
		voice.auto_ended = true;
		lck_mtx_unlock(&vayu_voice_lock);
		if (first == 0) {
			say("vayu-voice: auto: no call is listed %u s after the dial: nothing to end "
			    "(the modem's refusal, if any, is above)\n", voice.auto_hold_s);
			return true;
		}
		memset(&job, 0, sizeof(job));
		job.kind = VOICE_JOB_END;
		job.call_id = first;
		say("vayu-voice: auto: no CONVERSATION %u s after the dial: End Call for call %u is sent\n",
		    voice.auto_hold_s, (unsigned int)first);
		voice_run_job(&job);
		return true;
	}
	return false;
}

/* ---- the worker ---------------------------------------------------------------------------------- */

static void
voice_worker_thread(void *argument __unused, wait_result_t wait __unused)
{
	struct timespec tick = { .tv_sec = 0, .tv_nsec = 200 * NSEC_PER_MSEC };
	int ret;

	ret = qmi_handle_init(&voice.qmi, VOICE_HANDLE_MAX, &voice_qmi_ops, voice_handlers);
	if (ret < 0) {
		say("vayu-voice: qmi_handle_init: %d (no QRTR transport?)\n", ret);
		goto out;
	}
	voice.qmi_up = true;
	ret = qmi_add_lookup(&voice.qmi, VAYU_VOICE_QMI_SERVICE, VAYU_VOICE_QMI_VERSION, 0);
	if (ret < 0) {
		say("vayu-voice: qmi_add_lookup(VOICE 0x09): %d\n", ret);
		goto out;
	}
	say("vayu-voice: the modem's VOICE service (0x09) is looked up; %u s to find it\n",
	    (unsigned int)voice.wait_s);

	while (!voice.stop) {
		struct voice_job job;
		uint64_t now = now_ms();
		bool have_job = false;
		bool register_now = false;
		bool poll_now = false;

		lck_mtx_lock(&vayu_voice_lock);
		if (!voice.present && !voice.said_missing &&
		    now >= voice.started_ms + (uint64_t)voice.wait_s * 1000) {
			voice.said_missing = true;
			lck_mtx_unlock(&vayu_voice_lock);
			say("vayu-voice: the VOICE service (0x09) has not appeared in %u s; the client keeps the "
			    "lookup open (a modem that does not serve voice would look like this)\n",
			    (unsigned int)voice.wait_s);
			continue;
		}
		if (voice.job_tail != voice.job_head) {
			job = voice.job[voice.job_tail];
			voice.job_tail = (voice.job_tail + 1) % VAYU_VOICE_LINES;
			have_job = true;
		}
		if (voice.present && !voice.registered && voice.register_tries < VOICE_REGISTER_TRIES &&
		    (voice.next_register_ms == 0 || now >= voice.next_register_ms)) {
			register_now = true;
		}
		if (voice.present && voice.registered && voice.poll_s > 0 && now >= voice.next_poll_ms) {
			poll_now = true;
		}
		voice.work = false;
		lck_mtx_unlock(&vayu_voice_lock);

		if (register_now) {
			voice_register();
			continue;
		}
		if (have_job) {
			voice_run_job(&job);
			continue;
		}
		if (poll_now) {
			lck_mtx_lock(&vayu_voice_lock);
			voice.next_poll_ms = now_ms() + (uint64_t)voice.poll_s * 1000;
			lck_mtx_unlock(&vayu_voice_lock);
			(void)voice_get_calls("poll: get all call info", true);
			continue;
		}
		if (voice.auto_arg != 0 && voice_auto_step()) {
			continue;
		}
		lck_mtx_lock(&vayu_voice_lock);
		if (!voice.work) {
			msleep(&voice.work, &vayu_voice_lock, PRIBIO, "vayu-voice", &tick);
		}
		lck_mtx_unlock(&vayu_voice_lock);
	}

out:
	lck_mtx_lock(&vayu_voice_lock);
	voice.running = false;
	voice.registered = false;
	if (voice.state != VAYU_VOICE_FAILED) {
		voice.state = VAYU_VOICE_OFF;
	}
	lck_mtx_unlock(&vayu_voice_lock);
	if (voice.qmi_up) {
		qmi_handle_release(&voice.qmi);
		voice.qmi_up = false;
	}
}

/* ---- /dev/vayu-voice ----------------------------------------------------------------------------- */

static int
voice_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&vayu_voice_lock);
	voice_line_locked("STATE service=%s registered=%u calls=%u indications=%u (%s)",
	    voice.present ? "present" : "absent", (unsigned int)(voice.registered ? 1 : 0),
	    (unsigned int)voice.calls.count, (unsigned int)voice.indications,
	    voice.state == VAYU_VOICE_READY ? "ready" : voice.state == VAYU_VOICE_WAITING ? "waiting" :
	    voice.state == VAYU_VOICE_FAILED ? "failed" : "off");
	lck_mtx_unlock(&vayu_voice_lock);
	return 0;
}

static int
voice_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
voice_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char out[VAYU_VOICE_LINE_MAX * 4];
	size_t used = 0, room = sizeof(out);
	uint64_t deadline;
	struct timespec tick = { .tv_sec = 0, .tv_nsec = 100 * NSEC_PER_MSEC };

	if (uio_resid(uio) < 8) {
		return EINVAL;                  /* a line does not fit: say so rather than split one */
	}
	if ((size_t)uio_resid(uio) < room) {
		room = (size_t)uio_resid(uio);  /* only whole lines that the caller's buffer holds */
	}
	deadline = now_ms() + VAYU_VOICE_READ_WAIT_MS;
	for (;;) {
		lck_mtx_lock(&vayu_voice_lock);
		while (used + VAYU_VOICE_LINE_MAX <= room && voice_line_take_locked(out + used, room - used)) {
			used += strlen(out + used);
			out[used++] = '\n';
		}
		if (used > 0) {
			lck_mtx_unlock(&vayu_voice_lock);
			break;
		}
		if (now_ms() >= deadline || voice.stop) {
			lck_mtx_unlock(&vayu_voice_lock);
			return EAGAIN;
		}
		msleep(&voice.line_head, &vayu_voice_lock, PRIBIO, "vayu-voice-read", &tick);
		lck_mtx_unlock(&vayu_voice_lock);
	}
	return uiomove(out, (int)used, uio);
}

static int
voice_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char buffer[VAYU_VOICE_COMMAND_MAX];
	size_t length = (size_t)uio_resid(uio);
	int ret;

	if (length >= sizeof(buffer)) {
		return EINVAL;
	}
	ret = uiomove(buffer, (int)length, uio);
	if (ret != 0) {
		return ret;
	}
	buffer[length] = 0;
	for (size_t at = 0; at < length;) {
		size_t end = at;

		while (end < length && buffer[end] != '\n' && buffer[end] != '\r') {
			end++;
		}
		buffer[end] = 0;
		if (end > at) {
			voice_command(buffer + at);
		}
		at = end + 1;
	}
	return 0;
}

static const struct cdevsw voice_cdevsw = {
	.d_open = voice_open, .d_close = voice_close, .d_read = voice_read, .d_write = voice_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
voice_make_node(void)
{
	int major = cdevsw_add(-1, &voice_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, VAYU_VOICE_DEVICE) == NULL) {
		say("vayu-voice: /dev/%s cannot be made\n", VAYU_VOICE_DEVICE);
		return false;
	}
	say("vayu-voice: /dev/%s (0666) is made: text lines, read() waits up to %u ms\n", VAYU_VOICE_DEVICE,
	    (unsigned int)VAYU_VOICE_READ_WAIT_MS);
	return true;
}

/* ---- start and stop ------------------------------------------------------------------------------ */

int
vayu_voice_start(struct vayu_qrtr *q)
{
	thread_t thread;
	uint32_t level = 0, wait_s = 30, poll_s = 0, dial_arg = 0, ind_all = 0, ind_invert = 0, dump_arg = 0;
	uint32_t call_audio_arg = 0;
	uint32_t auto_arg = 0, auto_delay = 30, auto_end = 45;

	if (!PE_parse_boot_argn("vayu_call", &level, sizeof(level)) || level == 0) {
		return 0;
	}
	if (level > 2) {
		level = 2;
	}
	if (voice.started) {
		say("vayu-voice: already started\n");
		return 0;
	}
	if (voice.running) {
		return -EBUSY;
	}
	if (q == NULL || q->glink == NULL || !q->glink->up) {
		say("vayu-voice: vayu_call=%u, but the modem's GLINK channel is not up: nothing is done\n", level);
		return 0;
	}
	PE_parse_boot_argn("vayu_call_wait", &wait_s, sizeof(wait_s));
	if (wait_s < 1) {
		wait_s = 1;
	}
	if (wait_s > 120) {
		wait_s = 120;
	}
	PE_parse_boot_argn("vayu_call_poll", &poll_s, sizeof(poll_s));
	if (poll_s != 0 && poll_s < 5) {
		poll_s = 5;
	}
	if (poll_s > 3600) {
		poll_s = 3600;
	}
	PE_parse_boot_argn("vayu_call_dial", &dial_arg, sizeof(dial_arg));
	if (dial_arg != 0 && dial_arg != 1) {
		say("vayu-voice: vayu_call_dial=%u is refused: 0 and 1 only\n", dial_arg);
		dial_arg = 0;
	}
	PE_parse_boot_argn("vayu_call_ind_all", &ind_all, sizeof(ind_all));
	PE_parse_boot_argn("vayu_call_ind_invert", &ind_invert, sizeof(ind_invert));
	PE_parse_boot_argn("vayu_call_dump", &dump_arg, sizeof(dump_arg));
	PE_parse_boot_argn("vayu_call_audio", &call_audio_arg, sizeof(call_audio_arg));
	PE_parse_boot_argn("vayu_call_auto", &auto_arg, sizeof(auto_arg));
	PE_parse_boot_argn("vayu_call_auto_delay", &auto_delay, sizeof(auto_delay));
	PE_parse_boot_argn("vayu_call_auto_end", &auto_end, sizeof(auto_end));
	if (auto_delay < 5) {
		auto_delay = 5;
	}
	if (auto_delay > 300) {
		auto_delay = 300;
	}
	if (auto_end < 10) {
		auto_end = 10;
	}
	if (auto_end > 600) {
		auto_end = 600;
	}
	if (auto_arg != 0 && (level < 2 || dial_arg == 0)) {
		say("vayu-voice: vayu_call_auto=1 needs vayu_call=2 and vayu_call_dial=1: the auto-dial is off\n");
		auto_arg = 0;
	}
	memset(&voice, 0, sizeof(voice));
	voice.call_audio = call_audio_arg != 0;
	strlcpy(voice.allow, "0500", sizeof(voice.allow));       /* the owner-approved service number */
	if (PE_parse_boot_argn("vayu_call_allow", voice.allow, sizeof(voice.allow))) {
		if (!vayu_voice_digits_ok(voice.allow, VAYU_VOICE_NUMBER_MAX - 1)) {
			say("vayu-voice: vayu_call_allow is refused (digits, +, * and # only); 0500 stays\n");
			strlcpy(voice.allow, "0500", sizeof(voice.allow));
		}
	}
	if (dial_arg != 0 && level < 2) {
		say("vayu-voice: vayu_call_dial=1 without vayu_call=2: dialling stays refused\n");
	}
	voice.level = level;
	voice.wait_s = wait_s;
	voice.poll_s = poll_s;
	voice.dial_arg = dial_arg;
	voice.ind_all = ind_all;
	voice.ind_invert = ind_invert;
	voice.dump_arg = dump_arg;
	voice.auto_arg = auto_arg;
	voice.auto_delay_s = auto_delay;
	voice.auto_hold_s = auto_end;
	voice.stop = false;
	voice.running = true;
	voice.state = VAYU_VOICE_WAITING;
	voice.started_ms = now_ms();
	say("vayu-voice: vayu_call=%u vayu_call_wait=%u vayu_call_poll=%u vayu_call_dial=%u "
	    "vayu_call_ind_all=%u vayu_call_ind_invert=%u vayu_call_dump=%u\n", level, wait_s, poll_s,
	    dial_arg, ind_all, ind_invert, dump_arg);
	if (voice.call_audio) {
		say("vayu-voice: vayu_call_audio=1: the q6voice path follows the call (CONVERSATION: up, "
		    "none: down); it needs vayu_audio>=3, vayu_jack>=2 and the audio module's hold window\n");
	}
	if (voice.auto_arg != 0) {
		char tail[8];

		vayu_voice_mask_tail(voice.allow, tail);
		say("vayu-voice: vayu_call_auto=1: %s is dialled %u s after the indications are registered, "
		    "held %u s in CONVERSATION, then ended (the phone needs no shell)\n", tail, auto_delay,
		    auto_end);
	}
	if (strcmp(voice.allow, "0500") == 0) {
		say("vayu-voice: dial allow list: 0500 (the operator's service number the owner approved)\n");
	} else {
		say("vayu-voice: dial allow list: one number of %u digits (not printed)\n",
		    (unsigned int)strlen(voice.allow));
	}
	say("vayu-voice: VOICE 0x09 (indication register, get all call info, get call info, answer, end, "
	    "burst/continuous DTMF, send flash%s); the log masks every number to its last four digits\n",
	    level >= 2 ? ", dial" : "");
	(void)voice_make_node();
	voice.started = true;
	if (kernel_thread_start(voice_worker_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-voice: no thread\n");
		voice.running = false;
		voice.started = false;
		voice.state = VAYU_VOICE_OFF;
		return -ENOMEM;
	}
	thread_deallocate(thread);
	return 0;
}

void
vayu_voice_stop(void)
{
	if (!voice.started) {
		return;
	}
	lck_mtx_lock(&vayu_voice_lock);
	voice.stop = true;
	wakeup(&voice.work);
	wakeup(&voice.line_head);
	lck_mtx_unlock(&vayu_voice_lock);
	if (voice.call_audio_up) {
		voice.call_audio_up = false;
		vayu_audio_call_audio_stop();
		say("vayu-voice: the client stops: the q6voice path is taken down (vayu_call_audio=1)\n");
	}
	for (uint32_t waited = 0; waited < VOICE_STOP_MS && voice.running; waited += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	if (voice.running) {
		say("vayu-voice: the client's thread has not finished in %u ms\n", VOICE_STOP_MS);
	} else {
		voice.started = false;
		say("vayu-voice: done: %u indication(s), %u request(s), %u failure(s); /dev/%s stays\n",
		    (unsigned int)voice.indications, (unsigned int)voice.requests, (unsigned int)voice.failures,
		    VAYU_VOICE_DEVICE);
	}
}
