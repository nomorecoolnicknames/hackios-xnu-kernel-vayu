/*
 * The QMI VOICE messages of bsd/dev/vayu_voice.c (modem service 0x09), and the
 * decoders for the call reports it sends.
 *
 * The structures and tables live here, apart from the client, so that a program on
 * the machine that builds the kernel codes and decodes the same messages
 * (tools/voice-test).  The wire format (7-byte header, then TLVs) and the element
 * tables are sys/hakios.qmi.h's.
 *
 * Where the numbers come from -- this matters, because a QMI table that is guessed
 * is a request the modem refuses:
 *
 *   FACT, read out of libqmi 1.32.2 as installed here
 *   (/usr/lib/x86_64-linux-gnu/libqmi-glib.so.5.9.0, offline disassembly of
 *   qmi_client_voice_*(), which builds every VOICE request inline, so the message
 *   id and each TLV id are immediates; see work/voice_requests.py and
 *   work/voice_indreg.py):
 *
 *     VOICE 0x09  Indication Register   0x0003  TLVs 0x10..0x1d, each a u8
 *     VOICE 0x09  Dial Call             0x0020  TLV 0x01 = the number, a string
 *                                                (no length byte, no NUL)
 *     VOICE 0x09  End Call              0x0021  TLV 0x01 = call id, u8
 *     VOICE 0x09  Answer Call           0x0022  TLV 0x01 = call id, u8
 *     VOICE 0x09  Burst DTMF            0x0028  TLV 0x01 = {call id u8, digits}
 *     VOICE 0x09  Start Cont. DTMF      0x0029  TLV 0x01 = {call id u8, u8}
 *     VOICE 0x09  Stop Cont. DTMF       0x002a  TLV 0x01 = {call id u8, u8}
 *     VOICE 0x09  Get All Call Info     0x002f  no TLVs in the request
 *
 *   The Indication Register TLV map is the one work/voice_indreg.py recovers by
 *   joining each setter's struct offset with the offset the builder reads:
 *
 *     0x10 dtmf_events              0x16 ussd_notification_events
 *     0x11 voice_privacy_events     0x18 modification_events
 *     0x12 supplementary_service_notification_events
 *                                   0x19 uus_events
 *     0x13 call_notification_events 0x1a aoc_events
 *     0x14 handover_events          0x1b conference_events
 *     0x15 speech_codec_events      0x1c extended_burst_type_international_information_events
 *                                   0x1d mt_page_miss_information_events
 *
 *   `call_notification_events` (TLV 0x13) is the group that carries call status:
 *   it is the one a client sets to be told about a call at all.
 *
 *   The enum values (state, type, direction, mode) are libqmi's GEnum tables, read
 *   from the same library; they match QmiVoiceCallState & co.
 *
 *   HYPOTHESIS (checked by run A of JOB-DELTA, not by anything local):
 *     - Get Call Info is 0x0023 with TLV 0x01 = call id.  Its position between
 *       Answer Call 0x0022 and Send Flash 0x0024 is the whole of the evidence;
 *       libqmi 1.32.2 has no such client call to disassemble.
 *     - that this modem's firmware accepts v2 of the service and reports the
 *       call status with 0x2e once registered.  The id itself is a fact (above);
 *       what only a run can say is what *this* firmware sends.
 *     - the meaning of the last two bytes of a call entry: the first five are
 *       the ones libqmi names in order (id, state, type, direction, mode); the
 *       sixth and seventh are read by the handler but not exported, so this
 *       client calls them multipart and als after Qualcomm's
 *       qmi_voice_call_info_v02 and does not rely on them.
 *
 * None of these functions logs or stores anything by itself.
 */
#ifndef _SYS_VAYU_VOICE_QMI_H_
#define _SYS_VAYU_VOICE_QMI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/hakios.qmi.h>
#include <sys/vayu_voice.h>

/* ---- the service and its messages ------------------------------------------------------------ */

#define VAYU_VOICE_QMI_SERVICE                  0x09
#define VAYU_VOICE_QMI_VERSION                  2

#define VAYU_VOICE_QMI_INDICATION_REGISTER      0x0003
#define VAYU_VOICE_QMI_DIAL_CALL                0x0020
#define VAYU_VOICE_QMI_END_CALL                 0x0021
#define VAYU_VOICE_QMI_ANSWER_CALL              0x0022
#define VAYU_VOICE_QMI_GET_CALL_INFO            0x0023
#define VAYU_VOICE_QMI_SEND_FLASH               0x0024
#define VAYU_VOICE_QMI_BURST_DTMF               0x0028
#define VAYU_VOICE_QMI_START_CONT_DTMF          0x0029
#define VAYU_VOICE_QMI_STOP_CONT_DTMF           0x002a
#define VAYU_VOICE_QMI_GET_ALL_CALL_INFO        0x002f

/*
 * The indication ids libqmi's own VOICE client handles, so they are facts.  Its
 * process_indication() switches over 0x2e..0x43 (a jump table at 0x2ce38c); each
 * handler ends in g_signal_emit of one of the class's signals:
 *
 *   0x2e -> qmi_indication_voice_all_call_status_output  "all-call-status"
 *   0x32 ->                                                "supplementary-service"
 *   0x3d ->                                                "release-ussd"
 *   0x3e ->                                                "ussd"
 *   0x43 ->                                                "originate-ussd-no-wait"
 *
 * The 0x2e handler is the one that matters here: it reads TLV 0x01 as a one-byte
 * count and then that many 7-byte entries, one qmi_message_tlv_read_guint8 per
 * field, and TLV 0x10 as a single byte.  It also carries an assertion that the
 * message id is 0x2e (cmp $0x2e,%ax), which is where the id comes from twice.
 */
#define VAYU_VOICE_QMI_ALL_CALL_STATUS_IND       0x002e
#define VAYU_VOICE_QMI_SUPPLEMENTARY_SERVICE_IND 0x0032
#define VAYU_VOICE_QMI_RELEASE_USSD_IND          0x003d
#define VAYU_VOICE_QMI_USSD_IND                  0x003e

#define VAYU_VOICE_QMI_REQUEST_MAX              512     /* largest request we encode */

/* ---- the answer every request starts with (TLV 0x02) ----------------------------------------- */

struct vayu_voice_qmi_result {
	uint16_t result;
	uint16_t error;
};

static const struct qmi_elem_info vayu_voice_qmi_result_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_voice_qmi_result, result) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_voice_qmi_result, error) },
	{ .data_type = QMI_EOTI },
};

static __inline__ bool
vayu_voice_qmi_succeeded(const struct vayu_voice_qmi_result *result)
{
	return result != NULL && result->result == QMI_RESULT_SUCCESS_V01 && result->error == QMI_ERR_NONE_V01;
}

struct vayu_voice_qmi_result_resp {
	struct vayu_voice_qmi_result result;    /* TLV 0x02 */
};

static const struct qmi_elem_info vayu_voice_qmi_result_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_voice_qmi_result),
	  .array_type = NO_ARRAY, .tlv_type = 0x02, .offset = offsetof(struct vayu_voice_qmi_result_resp, result),
	  .ei_array = vayu_voice_qmi_result_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- Indication Register (0x0003) ------------------------------------------------------------ */

struct vayu_voice_qmi_indication_register_req {
	uint8_t dtmf_events;                            /* 0x10 */
	uint8_t voice_privacy_events;                   /* 0x11 */
	uint8_t supplementary_service_notification_events; /* 0x12 */
	uint8_t call_notification_events;               /* 0x13 */
	uint8_t handover_events;                        /* 0x14 */
	uint8_t speech_codec_events;                    /* 0x15 */
	uint8_t ussd_notification_events;               /* 0x16 */
	uint8_t modification_events;                    /* 0x18 */
	uint8_t uus_events;                             /* 0x19 */
	uint8_t aoc_events;                             /* 0x1a */
	uint8_t conference_events;                      /* 0x1b */
	uint8_t extended_burst_type_international_information_events; /* 0x1c */
	uint8_t mt_page_miss_information_events;        /* 0x1d */
};

static const struct qmi_elem_info vayu_voice_qmi_indication_register_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, dtmf_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, voice_privacy_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_voice_qmi_indication_register_req, supplementary_service_notification_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, call_notification_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, handover_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, speech_codec_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, ussd_notification_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, modification_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, uus_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1a, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, aoc_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1b, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, conference_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1c,
	  .offset = offsetof(struct vayu_voice_qmi_indication_register_req, extended_burst_type_international_information_events) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1d, .offset = offsetof(struct vayu_voice_qmi_indication_register_req, mt_page_miss_information_events) },
	{ .data_type = QMI_EOTI },
};

/*
 * What the client enables by default: the call notifications (0x13) and the
 * supplementary-service notifications (0x12), which is where a call's progress
 * comes from; USSD (0x16) and DTMF (0x10) are off until asked for, because they
 * are not needed to see a call and each adds traffic.  vayu_call_ind_all=1 turns
 * every group on.
 *
 * `invert` is the hedge for one unverifiable detail: whether 1 means "report" or,
 * as some firmware revisions have it, "do not report".  With no indication after a
 * call (run A), vayu_call_ind_invert=1 turns the register inside out and the
 * second run says which reading this modem uses.
 */
static __inline__ void
vayu_voice_qmi_indication_register_fill(struct vayu_voice_qmi_indication_register_req *request, bool all, bool invert)
{
	memset(request, 0, sizeof(*request));
	request->call_notification_events = 1;
	request->supplementary_service_notification_events = 1;
	request->ussd_notification_events = 1;
	if (all) {
		request->dtmf_events = 1;
		request->voice_privacy_events = 1;
		request->handover_events = 1;
		request->speech_codec_events = 1;
		request->modification_events = 1;
		request->uus_events = 1;
		request->aoc_events = 1;
		request->conference_events = 1;
		request->extended_burst_type_international_information_events = 1;
		request->mt_page_miss_information_events = 1;
	}
	if (invert) {
		request->dtmf_events ^= 1;
		request->voice_privacy_events ^= 1;
		request->supplementary_service_notification_events ^= 1;
		request->call_notification_events ^= 1;
		request->handover_events ^= 1;
		request->speech_codec_events ^= 1;
		request->ussd_notification_events ^= 1;
		request->modification_events ^= 1;
		request->uus_events ^= 1;
		request->aoc_events ^= 1;
		request->conference_events ^= 1;
		request->extended_burst_type_international_information_events ^= 1;
		request->mt_page_miss_information_events ^= 1;
	}
}

/* ---- Dial Call (0x0020) ---------------------------------------------------------------------- */

/* TLV 0x01 is the number as a string: the TLV's length is the digits, no NUL (libqmi). */
struct vayu_voice_qmi_dial_req {
	char number[VAYU_VOICE_NUMBER_MAX];
};

static const struct qmi_elem_info vayu_voice_qmi_dial_req_ei[] = {
	{ .data_type = QMI_STRING, .elem_len = VAYU_VOICE_NUMBER_MAX, .elem_size = sizeof(char), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_voice_qmi_dial_req, number) },
	{ .data_type = QMI_EOTI },
};

/* ---- End Call (0x0021), Answer Call (0x0022), Get Call Info (0x0023) ------------------------- */

struct vayu_voice_qmi_call_id_req {
	uint8_t call_id;                        /* TLV 0x01 */
};

static const struct qmi_elem_info vayu_voice_qmi_call_id_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_voice_qmi_call_id_req, call_id) },
	{ .data_type = QMI_EOTI },
};

/* Dial Call's answer carries the call id the modem gave the call. */
struct vayu_voice_qmi_call_id_resp {
	uint8_t call_id_valid;                  /* TLV 0x01 */
	uint8_t call_id;
	struct vayu_voice_qmi_result result;    /* TLV 0x02 */
};

static const struct qmi_elem_info vayu_voice_qmi_call_id_resp_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_voice_qmi_call_id_resp, call_id_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_voice_qmi_call_id_resp, call_id) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_voice_qmi_result),
	  .array_type = NO_ARRAY, .tlv_type = 0x02, .offset = offsetof(struct vayu_voice_qmi_call_id_resp, result),
	  .ei_array = vayu_voice_qmi_result_ei },
	{ .data_type = QMI_EOTI },
};

/*
 * End Call and Answer Call answer with the call id too (libqmi's
 * qmi_message_voice_{end,answer}_call_output_get_call_id), so the same response
 * structure decodes them.
 */

/* ---- Burst DTMF (0x0028), Start/Stop Continuous DTMF (0x0029/0x002a) ------------------------- */

/*
 * TLV 0x01 is one structure of two fields, so it is one TLV: two top-level
 * elements of the same tlv_type would be two TLVs, which is not what the wire
 * wants.  One digit per request: the digits field is a plain byte, so no
 * length-prefix question arises (libqmi writes the digits without a prefix, a
 * table's string inside a structure would carry one).
 */
struct vayu_voice_qmi_dtmf_req {
	uint8_t call_id;
	uint8_t digit;
};

static const struct qmi_elem_info vayu_voice_qmi_dtmf_fields_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_voice_qmi_dtmf_req, call_id) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_voice_qmi_dtmf_req, digit) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_voice_qmi_dtmf_req_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_voice_qmi_dtmf_req),
	  .array_type = NO_ARRAY, .tlv_type = 0x01, .offset = 0, .ei_array = vayu_voice_qmi_dtmf_fields_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- a TLV walker, for the messages that are not described by a table ------------------------ */

struct vayu_voice_tlv {
	uint8_t type;
	uint16_t length;
	const uint8_t *value;
};

/*
 * The next TLV of a QMI message (the buffer includes the 7-byte header).  `at` is
 * the cursor: start it at QMI_HEADER_SIZE.  False when the message ends or the
 * next TLV would reach past it (a truncated datagram is not half decoded).
 */
static __inline__ bool
vayu_voice_tlv_next(const uint8_t *message, size_t length, size_t *at, struct vayu_voice_tlv *tlv)
{
	size_t header = QMI_HEADER_SIZE;
	size_t end = length;

	if (message == NULL || at == NULL || tlv == NULL) {
		return false;
	}
	if (length < header) {
		return false;
	}
	if (*at < header) {
		*at = header;
	}
	/* the header's own length: a TLV past it is not part of this message */
	if ((size_t)(message[5] | (size_t)message[6] << 8) + header < end) {
		end = (size_t)(message[5] | (size_t)message[6] << 8) + header;
	}
	if (*at + 3 > end) {
		return false;
	}
	tlv->type = message[*at];
	tlv->length = (uint16_t)(message[*at + 1] | (uint16_t)message[*at + 2] << 8);
	tlv->value = message + *at + 3;
	if (*at + 3 + tlv->length > end) {
		return false;
	}
	*at += 3 + tlv->length;
	return true;
}

/* The first TLV of that type, or false. */
static __inline__ bool
vayu_voice_tlv_find(const uint8_t *message, size_t length, uint8_t type, struct vayu_voice_tlv *tlv)
{
	size_t at = QMI_HEADER_SIZE;

	while (vayu_voice_tlv_next(message, length, &at, tlv)) {
		if (tlv->type == type) {
			return true;
		}
	}
	return false;
}

/* ---- the call report ------------------------------------------------------------------------- */

/* A state/type/direction/mode as the log and the device print it. */
static __inline__ const char *
vayu_voice_state_name(uint8_t state)
{
	switch (state) {
	case VAYU_VOICE_STATE_UNKNOWN: return "unknown";
	case VAYU_VOICE_STATE_ORIGINATION: return "origination";
	case VAYU_VOICE_STATE_INCOMING: return "incoming";
	case VAYU_VOICE_STATE_CONVERSATION: return "conversation";
	case VAYU_VOICE_STATE_CC_IN_PROGRESS: return "cc-in-progress";
	case VAYU_VOICE_STATE_ALERTING: return "alerting";
	case VAYU_VOICE_STATE_HOLD: return "hold";
	case VAYU_VOICE_STATE_WAITING: return "waiting";
	case VAYU_VOICE_STATE_DISCONNECTING: return "disconnecting";
	case VAYU_VOICE_STATE_END: return "end";
	case VAYU_VOICE_STATE_SETUP: return "setup";
	default: return "?";
	}
}

static __inline__ const char *
vayu_voice_type_name(uint8_t type)
{
	switch (type) {
	case VAYU_VOICE_TYPE_VOICE: return "voice";
	case VAYU_VOICE_TYPE_VOICE_IP: return "voice-ip";
	case VAYU_VOICE_TYPE_OTAPA: return "otapa";
	case VAYU_VOICE_TYPE_NON_STD_OTASP: return "non-std-otasp";
	case VAYU_VOICE_TYPE_EMERGENCY: return "emergency";
	case VAYU_VOICE_TYPE_SUPS: return "sups";
	default: return "?";
	}
}

static __inline__ const char *
vayu_voice_direction_name(uint8_t direction)
{
	switch (direction) {
	case VAYU_VOICE_DIRECTION_MO: return "MO";
	case VAYU_VOICE_DIRECTION_MT: return "MT";
	default: return "?";
	}
}

static __inline__ const char *
vayu_voice_mode_name(uint8_t mode)
{
	switch (mode) {
	case VAYU_VOICE_MODE_CDMA: return "cdma";
	case VAYU_VOICE_MODE_GSM: return "gsm";
	case VAYU_VOICE_MODE_UMTS: return "umts";
	case VAYU_VOICE_MODE_LTE: return "lte";
	default: return "?";
	}
}

/* Only the last four digits: "...1234".  A short or non-numeric number is "...". */
static __inline__ void
vayu_voice_mask_tail(const char *number, char out[8])
{
	size_t length = number != NULL ? strlen(number) : 0;
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

/* A dialable number or digit string: digits and + * #, at most `max` of them. */
static __inline__ bool
vayu_voice_digits_ok(const char *text, size_t max)
{
	size_t length = 0;

	if (text == NULL) {
		return false;
	}
	for (; text[length] != 0; length++) {
		char c = text[length];

		if (length >= max) {
			return false;
		}
		if ((c < '0' || c > '9') && c != '+' && c != '*' && c != '#') {
			return false;
		}
	}
	return length > 0;
}

/* The same, for a run of bytes that is not zero-terminated (a TLV's value). */
static __inline__ bool
vayu_voice_digits_span_ok(const uint8_t *text, size_t length)
{
	if (text == NULL || length == 0 || length >= VAYU_VOICE_NUMBER_MAX) {
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		uint8_t c = text[i];

		if ((c < '0' || c > '9') && c != '+' && c != '*' && c != '#') {
			return false;
		}
	}
	return true;
}

static __inline__ bool
vayu_voice_call_fields_sane(uint8_t state, uint8_t type, uint8_t direction, uint8_t mode, uint8_t id)
{
	if (state > VAYU_VOICE_STATE_SETUP) {
		return false;
	}
	if (direction > VAYU_VOICE_DIRECTION_MT) {
		return false;
	}
	if (mode > VAYU_VOICE_MODE_LTE) {
		return false;
	}
	if (id > 0x3f) {
		return false;                   /* call ids are small; a stray byte is not one */
	}
	(void)type;
	return true;
}

/*
 * Decode the call list TLV (0x01) into `calls`.  On the wire it is a one-byte
 * count and then that many entries of 7 bytes {id, state, type, direction, mode,
 * multipart, als} -- that is what libqmi's all-call-status handler reads, one
 * qmi_message_tlv_read_guint8 per field, count included (FACT, see the file's
 * comment).  An entry of 8 bytes is also tried, and a value with no count byte is
 * accepted, so that a firmware that omits the count is still read; whichever
 * shape fits is taken, and every entry must look like a call, so a wrong guess is
 * refused rather than printed as a call with nonsense fields.
 * Returns the number of calls, or -1 when no shape parses.
 */
static __inline__ int
vayu_voice_call_list_decode(const uint8_t *value, uint16_t length, struct vayu_voice_calls *calls)
{
	static const uint8_t sizes[] = { 7, 8 };

	if (value == NULL || calls == NULL) {
		return -1;
	}
	/* the count byte first (the shape libqmi reads), then a value without one */
	for (unsigned counted = 1; counted <= 2; counted++) {
		const uint8_t *entries = value;
		uint16_t bytes = length;
		uint32_t count;

		if (counted == 1) {
			if (length < 1 || value[0] > VAYU_VOICE_CALLS) {
				continue;
			}
			count = value[0];
			entries = value + 1;
			bytes = (uint16_t)(length - 1);
			if (count == 0) {
				if (bytes == 0) {
					memset(calls, 0, sizeof(*calls));
					return 0;
				}
				continue;
			}
		}
		for (size_t s = 0; s < sizeof(sizes); s++) {
			uint8_t size = sizes[s];
			bool sane = true;

			if (bytes < size || bytes % size != 0) {
				continue;
			}
			if (counted == 1 && (uint32_t)(bytes / size) != count) {
				continue;
			}
			if (bytes / size > VAYU_VOICE_CALLS) {
				continue;
			}
			for (uint32_t i = 0; i < bytes / size && sane; i++) {
				const uint8_t *p = entries + (size_t)i * size;

				sane = vayu_voice_call_fields_sane(p[1], p[2], p[3], p[4], p[0]);
			}
			if (!sane) {
				continue;
			}
			memset(calls, 0, sizeof(*calls));
			calls->count = bytes / size;
			for (uint32_t i = 0; i < calls->count; i++) {
				const uint8_t *p = entries + (size_t)i * size;
				struct vayu_voice_call *call = &calls->call[i];

				call->id = p[0];
				call->state = p[1];
				call->type = p[2];
				call->direction = p[3];
				call->mode = p[4];
				call->multipart = p[5];
				call->als = p[6];
				call->entry_size = size;
			}
			return (int)calls->count;
		}
	}
	return -1;
}

/*
 * Decode a party-number TLV into the calls it names.  Two shapes are seen in the
 * QMI voice service: a count byte then {call id, length, digits} (the string
 * inside a structure carries a one-byte length when the table describes it), and
 * the same without the length byte.  The first shape that consumes the TLV
 * exactly and leaves only digits wins.
 * Returns the number of numbers filled in, or -1.
 */
static __inline__ int
vayu_voice_call_numbers_decode(const uint8_t *value, uint16_t length, struct vayu_voice_calls *calls)
{
	if (value == NULL || calls == NULL || length < 1) {
		return -1;
	}
	for (unsigned shape = 0; shape < 2; shape++) {
		uint32_t count = value[0];
		size_t at = 1;
		uint32_t filled = 0;
		bool ok = true;

		if (count == 0 || count > VAYU_VOICE_CALLS) {
			continue;
		}
		if (shape == 1 && count != 1) {
			continue;               /* without a length byte, only one entry is unambiguous */
		}
		for (uint32_t i = 0; i < count && ok; i++) {
			uint8_t id, digits;
			struct vayu_voice_call *call = NULL;

			if (at >= length) {
				ok = false;
				break;
			}
			id = value[at++];
			digits = shape == 0 && at < length ? value[at++] : (uint8_t)(length - at);
			if (digits == 0 || digits >= VAYU_VOICE_NUMBER_MAX || at + digits > length) {
				ok = false;
				break;
			}
			if (!vayu_voice_digits_span_ok(value + at, digits)) {
				ok = false;
				break;
			}
			for (uint32_t c = 0; c < calls->count; c++) {
				if (calls->call[c].id == id) {
					call = &calls->call[c];
					break;
				}
			}
			if (call != NULL) {
				memcpy(call->number, value + at, digits);
				call->number[digits] = 0;
				filled++;
			}
			at += digits;
		}
		if (ok && at == length) {
			return (int)filled;
		}
	}
	return -1;
}

#endif /* _SYS_VAYU_VOICE_QMI_H_ */
