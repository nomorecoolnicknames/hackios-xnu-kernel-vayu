/*
 * The QMI messages bsd/dev/vayu_cell.c speaks, as tables of struct qmi_elem_info for the helpers
 * of sys/hakios.qmi.h. The structures are here, apart from the client, so that a program on the
 * machine that builds the kernel can code and decode the same messages (tools/cell-test).
 *
 * Where every number comes from: libqmi's message descriptions, data/qmi-service-uim.json,
 * qmi-service-nas.json and qmi-service-dms.json of linux-mobile-broadband/libqmi (master, read
 * 2026-10-25). Each service, message and TLV below names the entry it was taken from; nothing
 * here is guessed. The wire format itself (a 7-byte header, then TLVs) is sys/hakios.qmi.h's.
 *
 *   UIM  0x0b  Get Card Status       0x002f   qmi-service-uim.json "Get Card Status"
 *   UIM  0x0b  Read Record           0x0021   qmi-service-uim.json "Read Record"
 *   UIM  0x0b  Read Transparent      0x0020   qmi-service-uim.json "Read Transparent"
 *   NAS  0x03  Get Serving System    0x0024   qmi-service-nas.json "Get Serving System"
 *   NAS  0x03  Get Signal Info       0x004f   qmi-service-nas.json "Get Signal Info"
 *   NAS  0x03  Initiate Network Reg. 0x0022   qmi-service-nas.json "Initiate Network Register"
 *   DMS  0x02  -- nothing is asked: its useful messages are identifiers (Get IDs / Get IMEI),
 *              and this client has no business with an identifier it does not need.
 *
 * What is deliberately absent: UIM Verify PIN / Unblock PIN / any write, NAS Set Operating Mode,
 * NAS Initiate Network Register (present but sent only with vayu_cell_register=1), WDS (no data
 * call). The client is read-only.
 *
 * Two notes on the element types, because the JSON's types are finer than the coding has:
 *   - a gint8 (RSSI, RSRQ, a radio interface) is a QMI_UNSIGNED_1_BYTE here, and the client reads
 *     it back through int8_t; the byte on the wire is the same, and 0xff (NO_CHANGE) stays 0xff.
 *   - a gint16 (RSRP, SNR) is a QMI_SIGNED_2_BYTE_ENUM: the coding has no plain signed type, and
 *     this one is the two's-complement 16-bit quantity the JSON asks for (Linux's tables do the
 *     same for int16).
 */
#ifndef _SYS_VAYU_CELL_QMI_H_
#define _SYS_VAYU_CELL_QMI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/hakios.qmi.h>

/* ---- what is asked, and of whom -------------------------------------------------------------- */

#define VAYU_CELL_UIM_SERVICE   0x0b
#define VAYU_CELL_NAS_SERVICE   0x03
#define VAYU_CELL_DMS_SERVICE   0x02            /* named for the log; never looked up */

#define VAYU_CELL_UIM_GET_CARD_STATUS           0x002f
#define VAYU_CELL_UIM_READ_RECORD               0x0021
#define VAYU_CELL_UIM_READ_TRANSPARENT          0x0020
#define VAYU_CELL_NAS_GET_SERVING_SYSTEM        0x0024
#define VAYU_CELL_NAS_GET_SIGNAL_INFO           0x004f
#define VAYU_CELL_NAS_INITIATE_NETWORK_REGISTER 0x0022

/* The largest request this client builds (a read record: three TLVs, under 64 bytes). */
#define VAYU_CELL_REQUEST_MAX                   128

/* ---- the sizes the modem's answers are taken into -------------------------------------------- */

#define VAYU_CELL_CARDS         4               /* one slot on vayu; a dual-SIM modem still fits */
#define VAYU_CELL_APPLICATIONS  8               /* USIM, ISIM, CSIM... a handful */
#define VAYU_CELL_AID           32              /* an AID is at most 16 bytes (TS 31.102) */
#define VAYU_CELL_RADIOS        8               /* QMI's list of radio interfaces is short */
#define VAYU_CELL_OPERATOR      128             /* the PLMN description, with room to spare */
#define VAYU_CELL_FILE_PATH     8               /* two or three file ids, two bytes each */
#define VAYU_CELL_FILE_DATA     64              /* EF_IMSI is 9 bytes, EF_ICCID 10; room to spare */

/* QMI UIM session types (libqmi qmi-enums-uim.h: QmiUimSessionType). */
#define VAYU_CELL_UIM_SESSION_PRIMARY_GW        0

/* UIM card states (QmiUimCardState). */
#define VAYU_CELL_CARD_ABSENT   0
#define VAYU_CELL_CARD_PRESENT  1
#define VAYU_CELL_CARD_ERROR    2

/* UIM card application states (QmiUimCardApplicationState). */
#define VAYU_CELL_UIM_APP_READY                     7
#define VAYU_CELL_UIM_APP_PIN1_REQUIRED             2
#define VAYU_CELL_UIM_APP_PUK1_REQUIRED             3
#define VAYU_CELL_UIM_APP_PIN1_BLOCKED              5

/* UIM card application types (QmiUimCardApplicationType). */
#define VAYU_CELL_UIM_APP_TYPE_SIM      1
#define VAYU_CELL_UIM_APP_TYPE_USIM     2
#define VAYU_CELL_UIM_APP_TYPE_RUIM     3
#define VAYU_CELL_UIM_APP_TYPE_CSIM     4
#define VAYU_CELL_UIM_APP_TYPE_ISIM     5

/* UIM PIN states (QmiUimPinState). */
#define VAYU_CELL_PIN_ENABLED_NOT_VERIFIED  1
#define VAYU_CELL_PIN_ENABLED_VERIFIED      2
#define VAYU_CELL_PIN_DISABLED              3
#define VAYU_CELL_PIN_BLOCKED               4
#define VAYU_CELL_PIN_PERMANENTLY_BLOCKED   5

/* The two files read, by their TS 31.102 identifiers. */
#define VAYU_CELL_UIM_EF_ICCID  0x2fe2          /* under the MF (3f00) */
#define VAYU_CELL_UIM_EF_IMSI   0x6f07          /* under ADF USIM, which the implicit fid 7fff names */

/* NAS registration states (QmiNasRegistrationState), the record's reg_state. */
#define VAYU_CELL_NAS_REG_NOT_REGISTERED    0
#define VAYU_CELL_NAS_REG_REGISTERED        1
#define VAYU_CELL_NAS_REG_SEARCHING         2
#define VAYU_CELL_NAS_REG_DENIED            3
#define VAYU_CELL_NAS_REG_UNKNOWN           4

/* NAS radio interfaces (QmiNasRadioInterface). */
#define VAYU_CELL_NAS_RADIO_NONE    0
#define VAYU_CELL_NAS_RADIO_GSM     4
#define VAYU_CELL_NAS_RADIO_UMTS    5
#define VAYU_CELL_NAS_RADIO_LTE     8
#define VAYU_CELL_NAS_RADIO_NR5G    0x0c

/* NAS Initiate Network Register's action (QmiNasNetworkRegisterType). */
#define VAYU_CELL_NAS_REGISTER_AUTOMATIC    1

/* NAS roaming indicator (QmiNasRoamingIndicatorStatus): ON is 0, OFF is 1. */
#define VAYU_CELL_NAS_ROAM_ON       0
#define VAYU_CELL_NAS_ROAM_OFF      1

/* ---- common ---------------------------------------------------------------------------------- */

/* TLV 0x02 of every reply (qmi-common.json "Result": error status, then error code). */
struct vayu_cell_result {
	uint16_t result;                /* QMI_RESULT_* */
	uint16_t error;                 /* QMI_ERR_* */
};

static const struct qmi_elem_info vayu_cell_result_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_result, result) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_result, error) },
	{ .data_type = QMI_EOTI },
};

/*
 * The digits of a transparent UIM file: TS 51.011's semi-octets, the low nibble of a byte first.
 * EF_IMSI has one byte of length first (the number of digits); EF_ICCID has none. A 0xf nibble
 * is skipped (EF_IMSI's parity; the padding of an odd count). A nibble above 9 that is not 0xf
 * is not a digit: nothing is decoded then and 0 is returned. `length` is the number of bytes of
 * the file; `digits` gets at most `max` of them.
 */
static __inline__ uint32_t
vayu_cell_uim_digits(const uint8_t *data, uint32_t length, bool length_byte, char *digits, uint32_t max)
{
	uint32_t wanted, out = 0;
	const uint8_t *at = data;

	if (length_byte) {
		if (length == 0 || data[0] == 0) {
			return 0;
		}
		wanted = data[0];               /* the digits the file says it has */
		at = data + 1;
		length--;
		if ((wanted + 1) / 2 > length) {
			return 0;               /* shorter than its own length byte says */
		}
	} else {
		wanted = length * 2;
	}
	for (uint32_t i = 0; i < wanted && out < max; i++) {
		uint8_t nibble = (i & 1) != 0 ? (uint8_t)(at[i / 2] >> 4) : (uint8_t)(at[i / 2] & 0x0f);

		if (nibble == 0x0f) {
			continue;
		}
		if (nibble > 9) {
			return 0;
		}
		digits[out++] = (char)('0' + nibble);
	}
	return out;
}

/* ---- UIM: Get Card Status (0x002f) ------------------------------------------------------------ */

struct vayu_cell_uim_app {
	uint8_t type;
	uint8_t state;
	uint8_t personalization_state;
	uint8_t personalization_feature;
	uint8_t personalization_retries;
	uint8_t personalization_unblock_retries;
	uint32_t aid_len;
	uint8_t aid[VAYU_CELL_AID];
	uint8_t upin_replaces_pin1;
	uint8_t pin1_state;
	uint8_t pin1_retries;
	uint8_t puk1_retries;
	uint8_t pin2_state;
	uint8_t pin2_retries;
	uint8_t puk2_retries;
};

struct vayu_cell_uim_card {
	uint8_t card_state;
	uint8_t upin_state;
	uint8_t upin_retries;
	uint8_t upuk_retries;
	uint8_t error_code;
	uint32_t app_len;
	struct vayu_cell_uim_app app[VAYU_CELL_APPLICATIONS];
};

/* TLV 0x10 "UIM Card Status" (qmi-service-uim.json, common-ref of that name). */
struct vayu_cell_uim_card_status {
	uint16_t index_gw_primary;
	uint16_t index_1x_primary;
	uint16_t index_gw_secondary;
	uint16_t index_1x_secondary;
	uint32_t cards_len;
	struct vayu_cell_uim_card cards[VAYU_CELL_CARDS];
};

static const struct qmi_elem_info vayu_cell_uim_app_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, type) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, personalization_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, personalization_feature) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, personalization_retries) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, personalization_unblock_retries) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, aid_len) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_CELL_AID, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, aid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, upin_replaces_pin1) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, pin1_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, pin1_retries) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, puk1_retries) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, pin2_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, pin2_retries) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_app, puk2_retries) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_card_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card, card_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card, upin_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card, upin_retries) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card, upuk_retries) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card, error_code) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card, app_len) },
	{ .data_type = QMI_STRUCT, .elem_len = VAYU_CELL_APPLICATIONS, .elem_size = sizeof(struct vayu_cell_uim_app),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card, app),
	  .ei_array = vayu_cell_uim_app_ei },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_card_status_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_status, index_gw_primary) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_status, index_1x_primary) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_status, index_gw_secondary) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_status, index_1x_secondary) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_status, cards_len) },
	{ .data_type = QMI_STRUCT, .elem_len = VAYU_CELL_CARDS, .elem_size = sizeof(struct vayu_cell_uim_card),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_status, cards),
	  .ei_array = vayu_cell_uim_card_ei },
	{ .data_type = QMI_EOTI },
};

struct vayu_cell_uim_get_card_status_resp {
	struct vayu_cell_result result;         /* TLV 0x02 */
	uint8_t card_status_valid;              /* TLV 0x10 */
	struct vayu_cell_uim_card_status card_status;
};

static const struct qmi_elem_info vayu_cell_uim_get_card_status_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_result), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct vayu_cell_uim_get_card_status_resp, result),
	  .ei_array = vayu_cell_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_cell_uim_get_card_status_resp, card_status_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_card_status), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_cell_uim_get_card_status_resp, card_status),
	  .ei_array = vayu_cell_uim_card_status_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- UIM: Read Record (0x0021) and Read Transparent (0x0020) --------------------------------- */

/* TLV 0x01 "Session" (qmi-service-uim.json): the type and an application id, which a
 * provisioning session's modem ignores (libqmi's qmicli passes an empty one). */
struct vayu_cell_uim_session {
	uint8_t session_type;           /* VAYU_CELL_UIM_SESSION_* */
	uint32_t aid_len;               /* a uint32_t in C, one byte on the wire */
	uint8_t aid[VAYU_CELL_AID];
};

/* TLV 0x02 "File": the file id (0x6f07), then its path's ids, two little-endian bytes each. */
struct vayu_cell_uim_file {
	uint16_t file_id;
	uint32_t path_len;
	uint8_t path[VAYU_CELL_FILE_PATH];
};

/* TLV 0x03 "Record" of Read Record. */
struct vayu_cell_uim_record_info {
	uint16_t record_number;
	uint16_t record_length;
};

/* TLV 0x03 "Read Information" of Read Transparent. */
struct vayu_cell_uim_read_info {
	uint16_t offset;
	uint16_t length;                /* 0: to the end of the file */
};

struct vayu_cell_uim_read_record_req {
	struct vayu_cell_uim_session session;
	struct vayu_cell_uim_file file;
	struct vayu_cell_uim_record_info record;
};

struct vayu_cell_uim_read_transparent_req {
	struct vayu_cell_uim_session session;
	struct vayu_cell_uim_file file;
	struct vayu_cell_uim_read_info read;
};

static const struct qmi_elem_info vayu_cell_uim_session_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_session, session_type) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_session, aid_len) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_CELL_AID, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_session, aid) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_file_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_file, file_id) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_file, path_len) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_CELL_FILE_PATH, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_file, path) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_record_info_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_record_info, record_number) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_record_info, record_length) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_read_info_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_read_info, offset) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_read_info, length) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_read_record_req_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_session), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_cell_uim_read_record_req, session),
	  .ei_array = vayu_cell_uim_session_ei },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_file), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct vayu_cell_uim_read_record_req, file),
	  .ei_array = vayu_cell_uim_file_ei },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_record_info), .array_type = NO_ARRAY,
	  .tlv_type = 0x03, .offset = offsetof(struct vayu_cell_uim_read_record_req, record),
	  .ei_array = vayu_cell_uim_record_info_ei },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_read_transparent_req_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_session), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_cell_uim_read_transparent_req, session),
	  .ei_array = vayu_cell_uim_session_ei },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_file), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct vayu_cell_uim_read_transparent_req, file),
	  .ei_array = vayu_cell_uim_file_ei },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_read_info), .array_type = NO_ARRAY,
	  .tlv_type = 0x03, .offset = offsetof(struct vayu_cell_uim_read_transparent_req, read),
	  .ei_array = vayu_cell_uim_read_info_ei },
	{ .data_type = QMI_EOTI },
};

/* TLV 0x10 "Card Result": the card's SW1/SW2 answer. */
struct vayu_cell_uim_card_result {
	uint8_t sw1;
	uint8_t sw2;
};

struct vayu_cell_uim_read_resp {
	struct vayu_cell_result result;         /* TLV 0x02 */
	uint8_t card_result_valid;              /* TLV 0x10 */
	struct vayu_cell_uim_card_result card_result;
	uint8_t read_valid;                     /* TLV 0x11; "Additional Read Result" (0x12) is not taken */
	uint32_t read_len;
	uint8_t read[VAYU_CELL_FILE_DATA];
};

static const struct qmi_elem_info vayu_cell_uim_card_result_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_result, sw1) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_uim_card_result, sw2) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_uim_read_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_result), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct vayu_cell_uim_read_resp, result),
	  .ei_array = vayu_cell_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_cell_uim_read_resp, card_result_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_uim_card_result), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_cell_uim_read_resp, card_result),
	  .ei_array = vayu_cell_uim_card_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_cell_uim_read_resp, read_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_cell_uim_read_resp, read_len) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_CELL_FILE_DATA, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x11, .offset = offsetof(struct vayu_cell_uim_read_resp, read) },
	{ .data_type = QMI_EOTI },
};

/* ---- NAS: Get Serving System (0x0024) -------------------------------------------------------- */

/* TLV 0x01 "Serving System" (qmi-service-nas.json). */
struct vayu_cell_nas_serving_system {
	uint8_t registration_state;     /* VAYU_CELL_NAS_REG_* */
	uint8_t cs_attach_state;        /* QmiNasAttachState */
	uint8_t ps_attach_state;
	uint8_t selected_network;       /* QmiNasNetworkType */
	uint32_t radio_len;
	uint8_t radio[VAYU_CELL_RADIOS];        /* QmiNasRadioInterface, one byte each */
};

/* TLV 0x12 "Current PLMN". */
struct vayu_cell_nas_plmn {
	uint16_t mcc;
	uint16_t mnc;
	char description[VAYU_CELL_OPERATOR];
};

/* TLV 0x21 "Detailed Service Status". */
struct vayu_cell_nas_detailed_status {
	uint8_t status;                 /* QmiNasServiceStatus */
	uint8_t capability;             /* QmiNasNetworkServiceDomain */
	uint8_t hdr_status;
	uint8_t hdr_hybrid;
	uint8_t forbidden;
};

struct vayu_cell_nas_get_serving_system_resp {
	struct vayu_cell_result result;         /* TLV 0x02 */
	uint8_t serving_valid;                  /* TLV 0x01 */
	struct vayu_cell_nas_serving_system serving;
	uint8_t roam_valid;                     /* TLV 0x10 */
	uint8_t roam;
	uint8_t plmn_valid;                     /* TLV 0x12 */
	struct vayu_cell_nas_plmn plmn;
	uint8_t lac_valid;                      /* TLV 0x1c */
	uint16_t lac;
	uint8_t cid_valid;                      /* TLV 0x1d */
	uint32_t cid;
	uint8_t detailed_valid;                 /* TLV 0x21 */
	struct vayu_cell_nas_detailed_status detailed;
};

static const struct qmi_elem_info vayu_cell_nas_serving_system_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_serving_system, registration_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_serving_system, cs_attach_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_serving_system, ps_attach_state) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_serving_system, selected_network) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_serving_system, radio_len) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_CELL_RADIOS, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_serving_system, radio) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_nas_plmn_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_plmn, mcc) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_plmn, mnc) },
	{ .data_type = QMI_STRING, .elem_len = VAYU_CELL_OPERATOR, .elem_size = sizeof(char), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_plmn, description) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_nas_detailed_status_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_detailed_status, status) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_detailed_status, capability) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_detailed_status, hdr_status) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_detailed_status, hdr_hybrid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_detailed_status, forbidden) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_nas_get_serving_system_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_result), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, result),
	  .ei_array = vayu_cell_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, serving_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_nas_serving_system), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, serving),
	  .ei_array = vayu_cell_nas_serving_system_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, roam_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, roam) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, plmn_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_nas_plmn), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, plmn),
	  .ei_array = vayu_cell_nas_plmn_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1c, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, lac_valid) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1c, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, lac) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1d, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, cid_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1d, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, cid) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x21, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, detailed_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_nas_detailed_status), .array_type = NO_ARRAY,
	  .tlv_type = 0x21, .offset = offsetof(struct vayu_cell_nas_get_serving_system_resp, detailed),
	  .ei_array = vayu_cell_nas_detailed_status_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- NAS: Get Signal Info (0x004f) ----------------------------------------------------------- */

/* TLV 0x14 "LTE Signal Strength": RSSI and RSRQ in dB(m), RSRP in dBm, SNR in 0.1 dB. */
struct vayu_cell_nas_lte_signal {
	uint8_t rssi;                   /* gint8 */
	uint8_t rsrq;                   /* gint8 */
	int16_t rsrp;
	int16_t snr;
};

/* TLV 0x17 "5G Signal Strength". */
struct vayu_cell_nas_nr5g_signal {
	int16_t rsrp;
	int16_t snr;
};

struct vayu_cell_nas_get_signal_info_resp {
	struct vayu_cell_result result;         /* TLV 0x02 */
	uint8_t gsm_valid;                      /* TLV 0x12: RSSI, gint8 */
	uint8_t gsm_rssi;
	uint8_t lte_valid;                      /* TLV 0x14 */
	struct vayu_cell_nas_lte_signal lte;
	uint8_t nr5g_valid;                     /* TLV 0x17 */
	struct vayu_cell_nas_nr5g_signal nr5g;
};

static const struct qmi_elem_info vayu_cell_nas_lte_signal_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_lte_signal, rssi) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_lte_signal, rsrq) },
	{ .data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(int16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_lte_signal, rsrp) },
	{ .data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(int16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_lte_signal, snr) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_nas_nr5g_signal_ei[] = {
	{ .data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(int16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_nr5g_signal, rsrp) },
	{ .data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(int16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_cell_nas_nr5g_signal, snr) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_nas_get_signal_info_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_result), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct vayu_cell_nas_get_signal_info_resp, result),
	  .ei_array = vayu_cell_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_cell_nas_get_signal_info_resp, gsm_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_cell_nas_get_signal_info_resp, gsm_rssi) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_cell_nas_get_signal_info_resp, lte_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_nas_lte_signal), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_cell_nas_get_signal_info_resp, lte),
	  .ei_array = vayu_cell_nas_lte_signal_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_cell_nas_get_signal_info_resp, nr5g_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_nas_nr5g_signal), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_cell_nas_get_signal_info_resp, nr5g),
	  .ei_array = vayu_cell_nas_nr5g_signal_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- NAS: Initiate Network Register (0x0022) -------------------------------------------------- */

/* TLV 0x01 "Action": QMI_NAS_NETWORK_REGISTER_TYPE_AUTOMATIC (1) or MANUAL (2). Only this
 * client's vayu_cell_register=1 sends it, and only automatic: a manual one needs a PLMN. */
struct vayu_cell_nas_initiate_register_req {
	uint8_t action;
};

static const struct qmi_elem_info vayu_cell_nas_initiate_register_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_cell_nas_initiate_register_req, action) },
	{ .data_type = QMI_EOTI },
};

/* The reply to it: the Operation Result alone. */
struct vayu_cell_nas_register_resp {
	struct vayu_cell_result result;         /* TLV 0x02 */
};

static const struct qmi_elem_info vayu_cell_nas_register_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_result), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct vayu_cell_nas_register_resp, result),
	  .ei_array = vayu_cell_result_ei },
	{ .data_type = QMI_EOTI },
};

#endif /* _SYS_VAYU_CELL_QMI_H_ */
