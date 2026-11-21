/*
 * The WMS frames CommCenter exchanges with the modem, read as they pass: a parser with
 * no side effects for bsd/dev/vayu_sms.c's observer (vayu_sms=3) and for the host tests
 * (tools/sms-test).
 *
 * On the stock path the kernel is not a WMS client at all: iOS's CommCenter is.  Its
 * QMISMSCommandDriver (libCommCenter's sms::, 20F66) speaks QMI WMS 0x05 itself, through
 * the baseband transport (AppleBaseband / QMux, then the QMux<->QRTR bridge of
 * bsd/dev/vayu_qmux.c).  What that driver sends, as the symbols of the 20F66 shared cache
 * name it: bindSubscription_sync, sendIndRegisterRequest_sync, getServiceReadyStatus and
 * registerForServiceReadyInd, setEventReport, setRoutes, setRetryPeriod/Interval,
 * setDCDisconnectTimer, getSmsc/setSmsc, requestStoredMessages and readStoredMessages
 * (List + Raw Read), modifyStoredMessage (Modify Tag), handleMTMessage(TransferRoute
 * MTMessage) and sendAck, sendSegment (Raw Send), broadcast config and voicemail (MWI).
 * Its MT handler takes a TransferRouteMTMessage and it has a sendAck, so the incoming
 * message is expected on a transfer route that CommCenter acknowledges; it also reads
 * stored messages (a store route, or what the SIM held).  Which one it asks for is
 * not read out of the binary: the observer logs the route table CommCenter sets.
 *
 * This file reads those frames for the log and the counters only: which request, which
 * result, and for the SMS path's checkpoints (an MT on a transfer route and its Send
 * Ack, a Raw Send and its cause) the numbers that say whether it worked.  It never
 * copies an address, a PDU byte, an SMSC or a message body out of a frame; the only
 * sizes it reports are a PDU's length.
 *
 * Message ids and TLV numbers: Qualcomm's QMI IDL wireless_messaging_service_v01.{h,c}
 * (as published in os-vector/wire-os, qmi/services/, read 2026-11-18), checked against
 * libqmi's data/qmi-service-wms.json for the ones libqmi has.  Aggregate layouts on the
 * wire (byte enums, little-endian) are libqmi's:
 *   Event Report ind 0x0001: 0x10 {storage u8, index u32}; 0x11 {ack indicator u8
 *       (0 send ack, 1 do not), transaction u32, format u8, u16 length, data};
 *       0x12 mode u8; 0x16 SMS-on-IMS u8
 *   Set Event Report 0x0001: 0x10 report MT u8
 *   Raw Send 0x0020: 0x01 {format u8, u16 length, data}; reply 0x01 message id u16,
 *       0x10 cause u16, 0x11 error class u8, 0x12 {RP cause u16, TP cause u8}
 *   List Messages 0x0031 reply: 0x01 {u32 count, {index u32, tag u8}...}
 *   Set Routes 0x0032: 0x01 {u16 count, {type, class, storage, action}...},
 *       0x10 transfer status report u8
 *   Send Ack 0x0037: 0x01 {transaction u32, protocol u8, success u8}; reply 0x10
 *       failure cause u8
 *   Bind Subscription 0x004C: 0x01 subscription u8
 *   Transport NW Reg ind 0x004B: 0x01 status u8
 *   Async Raw Send ind 0x0059: 0x01 status u16, 0x10 message id u16, 0x11 cause u16,
 *       0x12 error class u8, 0x13 {RP cause u16, TP cause u8}
 *   Async Send Ack ind 0x005A: 0x01 status u16, 0x10 failure cause u8
 *   Get Service Ready Status 0x005C reply: 0x11 ready status u32
 *   Service Ready ind 0x005D: 0x01 ready status u32 (0 none, 1 3GPP, 2 3GPP2, 3 both)
 */
#ifndef _SYS_VAYU_SMS_WMS_H_
#define _SYS_VAYU_SMS_WMS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VAYU_SMS_WMS_AP_TO_MODEM        0
#define VAYU_SMS_WMS_MODEM_TO_AP        1

#define VAYU_SMS_WMS_REQUEST            0x00
#define VAYU_SMS_WMS_RESPONSE           0x02
#define VAYU_SMS_WMS_INDICATION         0x04

/* the ids this file names (wireless_messaging_service_v01.h) */
#define VAYU_SMS_WMS_RESET              0x0000
#define VAYU_SMS_WMS_EVENT_REPORT       0x0001  /* Set Event Report and its indication */
#define VAYU_SMS_WMS_GET_SUPPORTED      0x001E
#define VAYU_SMS_WMS_RAW_SEND           0x0020
#define VAYU_SMS_WMS_RAW_WRITE          0x0021
#define VAYU_SMS_WMS_RAW_READ           0x0022
#define VAYU_SMS_WMS_MODIFY_TAG         0x0023
#define VAYU_SMS_WMS_DELETE             0x0024
#define VAYU_SMS_WMS_GET_PROTOCOL       0x0030
#define VAYU_SMS_WMS_LIST               0x0031
#define VAYU_SMS_WMS_SET_ROUTES         0x0032
#define VAYU_SMS_WMS_GET_ROUTES         0x0033
#define VAYU_SMS_WMS_GET_SMSC           0x0034
#define VAYU_SMS_WMS_SET_SMSC           0x0035
#define VAYU_SMS_WMS_GET_STORE_MAX      0x0036
#define VAYU_SMS_WMS_SEND_ACK           0x0037
#define VAYU_SMS_WMS_SET_RETRY_PERIOD   0x0038
#define VAYU_SMS_WMS_SET_RETRY_INTERVAL 0x0039
#define VAYU_SMS_WMS_SET_DC_TIMER       0x003A
#define VAYU_SMS_WMS_SET_MEMORY_STATUS  0x003B
#define VAYU_SMS_WMS_SET_BC_ACTIVATION  0x003C
#define VAYU_SMS_WMS_SET_BC_CONFIG      0x003D
#define VAYU_SMS_WMS_GET_BC_CONFIG      0x003E
#define VAYU_SMS_WMS_MEMORY_FULL        0x003F  /* indication */
#define VAYU_SMS_WMS_GET_DOMAIN_PREF    0x0040
#define VAYU_SMS_WMS_SET_DOMAIN_PREF    0x0041
#define VAYU_SMS_WMS_SEND_FROM_STORE    0x0042
#define VAYU_SMS_WMS_GET_MSG_WAITING    0x0043
#define VAYU_SMS_WMS_MSG_WAITING        0x0044  /* indication */
#define VAYU_SMS_WMS_SET_PRIMARY        0x0045
#define VAYU_SMS_WMS_SMSC_ADDRESS       0x0046  /* indication */
#define VAYU_SMS_WMS_IND_REGISTER       0x0047
#define VAYU_SMS_WMS_GET_TL_INFO        0x0048
#define VAYU_SMS_WMS_TL_INFO            0x0049  /* indication */
#define VAYU_SMS_WMS_GET_NW_REG         0x004A
#define VAYU_SMS_WMS_NW_REG             0x004B  /* indication */
#define VAYU_SMS_WMS_BIND_SUBSCRIPTION  0x004C
#define VAYU_SMS_WMS_GET_IND_REGISTER   0x004D
#define VAYU_SMS_WMS_CALL_STATUS        0x0050  /* indication */
#define VAYU_SMS_WMS_ASYNC_RAW_SEND     0x0059  /* request and indication */
#define VAYU_SMS_WMS_ASYNC_SEND_ACK     0x005A  /* request and indication */
#define VAYU_SMS_WMS_GET_READY_STATUS   0x005C
#define VAYU_SMS_WMS_SERVICE_READY      0x005D  /* indication */
#define VAYU_SMS_WMS_BC_CONFIG          0x005E  /* indication */
#define VAYU_SMS_WMS_SET_MSG_WAITING    0x005F
#define VAYU_SMS_WMS_TL_MWI             0x0060  /* indication */

#define VAYU_SMS_WMS_ROUTES_MAX         8       /* the routes a note keeps; the count is kept whole */

struct vayu_sms_wms_route {
	uint8_t type, message_class, storage, action;
};

/* What one frame said, without its content.  Every *_valid is "that TLV was there and whole". */
struct vayu_sms_wms_note {
	uint8_t direction;              /* VAYU_SMS_WMS_AP_TO_MODEM / _MODEM_TO_AP */
	uint8_t qmi_type;               /* VAYU_SMS_WMS_REQUEST / _RESPONSE / _INDICATION */
	uint16_t transaction;           /* the QMI header's (the bridge's, on the modem side) */
	uint16_t message;
	uint16_t tlv_length;
	bool result_valid;
	uint16_t result, error;
	/* Event Report indication */
	bool mt_stored;
	uint8_t storage;
	uint32_t index;
	bool mt_transfer;
	uint8_t ack_indicator;          /* 0: an ack is owed */
	uint32_t wms_transaction;
	uint8_t format;
	uint16_t pdu_length;
	bool mode_valid;
	uint8_t mode;
	bool sms_on_ims_valid;
	uint8_t sms_on_ims;
	/* Set Event Report */
	bool report_mt_valid;
	uint8_t report_mt;
	/* Send Ack request and its reply / the async indication */
	bool ack_valid;
	uint32_t ack_transaction;
	uint8_t ack_protocol, ack_success;
	bool ack_failure_valid;
	uint8_t ack_failure;
	/* Raw Send request, its reply / the async indication */
	bool send_valid;
	uint8_t send_format;
	uint16_t send_length;
	bool message_id_valid;
	uint16_t message_id;
	bool cause_valid;
	uint16_t cause;
	bool error_class_valid;
	uint8_t error_class;
	bool gw_cause_valid;
	uint16_t rp_cause;
	uint8_t tp_cause;
	bool status_valid;              /* async indications' TLV 0x01 */
	uint16_t status;
	/* Set Routes */
	bool routes_valid;
	uint16_t route_count;
	struct vayu_sms_wms_route route[VAYU_SMS_WMS_ROUTES_MAX];
	bool transfer_ind_valid;
	uint8_t transfer_ind;
	/* List Messages reply */
	bool list_valid;
	uint32_t list_count;
	/* Bind Subscription */
	bool subscription_valid;
	uint8_t subscription;
	/* Service Ready indication, Get Service Ready Status reply */
	bool ready_valid;
	uint32_t ready_status;
	/* Transport NW Reg indication */
	bool nw_reg_valid;
	uint8_t nw_reg;
};

static __inline__ uint16_t
vayu_sms_wms_get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static __inline__ uint32_t
vayu_sms_wms_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static __inline__ const char *
vayu_sms_wms_name(uint16_t message)
{
	switch (message) {
	case VAYU_SMS_WMS_RESET: return "reset";
	case VAYU_SMS_WMS_EVENT_REPORT: return "event report";
	case VAYU_SMS_WMS_GET_SUPPORTED: return "get supported messages";
	case VAYU_SMS_WMS_RAW_SEND: return "raw send";
	case VAYU_SMS_WMS_RAW_WRITE: return "raw write";
	case VAYU_SMS_WMS_RAW_READ: return "raw read";
	case VAYU_SMS_WMS_MODIFY_TAG: return "modify tag";
	case VAYU_SMS_WMS_DELETE: return "delete";
	case VAYU_SMS_WMS_GET_PROTOCOL: return "get message protocol";
	case VAYU_SMS_WMS_LIST: return "list messages";
	case VAYU_SMS_WMS_SET_ROUTES: return "set routes";
	case VAYU_SMS_WMS_GET_ROUTES: return "get routes";
	case VAYU_SMS_WMS_GET_SMSC: return "get SMSC";
	case VAYU_SMS_WMS_SET_SMSC: return "set SMSC";
	case VAYU_SMS_WMS_GET_STORE_MAX: return "get store max size";
	case VAYU_SMS_WMS_SEND_ACK: return "send ack";
	case VAYU_SMS_WMS_SET_RETRY_PERIOD: return "set retry period";
	case VAYU_SMS_WMS_SET_RETRY_INTERVAL: return "set retry interval";
	case VAYU_SMS_WMS_SET_DC_TIMER: return "set DC disconnect timer";
	case VAYU_SMS_WMS_SET_MEMORY_STATUS: return "set memory status";
	case VAYU_SMS_WMS_SET_BC_ACTIVATION: return "set broadcast activation";
	case VAYU_SMS_WMS_SET_BC_CONFIG: return "set broadcast config";
	case VAYU_SMS_WMS_GET_BC_CONFIG: return "get broadcast config";
	case VAYU_SMS_WMS_MEMORY_FULL: return "memory full";
	case VAYU_SMS_WMS_GET_DOMAIN_PREF: return "get domain pref";
	case VAYU_SMS_WMS_SET_DOMAIN_PREF: return "set domain pref";
	case VAYU_SMS_WMS_SEND_FROM_STORE: return "send from memory store";
	case VAYU_SMS_WMS_GET_MSG_WAITING: return "get message waiting";
	case VAYU_SMS_WMS_MSG_WAITING: return "message waiting";
	case VAYU_SMS_WMS_SET_PRIMARY: return "set primary client";
	case VAYU_SMS_WMS_SMSC_ADDRESS: return "SMSC address";
	case VAYU_SMS_WMS_IND_REGISTER: return "indication register";
	case VAYU_SMS_WMS_GET_TL_INFO: return "get transport layer info";
	case VAYU_SMS_WMS_TL_INFO: return "transport layer info";
	case VAYU_SMS_WMS_GET_NW_REG: return "get transport NW reg";
	case VAYU_SMS_WMS_NW_REG: return "transport NW reg";
	case VAYU_SMS_WMS_BIND_SUBSCRIPTION: return "bind subscription";
	case VAYU_SMS_WMS_GET_IND_REGISTER: return "get indication register";
	case VAYU_SMS_WMS_CALL_STATUS: return "call status";
	case VAYU_SMS_WMS_ASYNC_RAW_SEND: return "async raw send";
	case VAYU_SMS_WMS_ASYNC_SEND_ACK: return "async send ack";
	case VAYU_SMS_WMS_GET_READY_STATUS: return "get service ready status";
	case VAYU_SMS_WMS_SERVICE_READY: return "service ready";
	case VAYU_SMS_WMS_BC_CONFIG: return "broadcast config";
	case VAYU_SMS_WMS_SET_MSG_WAITING: return "set message waiting";
	case VAYU_SMS_WMS_TL_MWI: return "transport layer MWI";
	default: return NULL;
	}
}

static __inline__ const char *
vayu_sms_wms_ready_name(uint32_t status)
{
	switch (status) {
	case 0: return "none";
	case 1: return "3GPP";
	case 2: return "3GPP2";
	case 3: return "3GPP and 3GPP2";
	default: return "?";
	}
}

/* Raw Send's reply and the async raw send indication share their cause TLVs, one number apart. */
static __inline__ void
vayu_sms_wms_send_causes(struct vayu_sms_wms_note *n, uint8_t type, const uint8_t *v, uint16_t length,
    uint8_t first)
{
	if (type == first && length >= 2) {
		n->cause_valid = true;
		n->cause = vayu_sms_wms_get16(v);
	} else if (type == first + 1 && length >= 1) {
		n->error_class_valid = true;
		n->error_class = v[0];
	} else if (type == first + 2 && length >= 3) {
		n->gw_cause_valid = true;
		n->rp_cause = vayu_sms_wms_get16(v);
		n->tp_cause = v[2];
	}
}

static __inline__ void
vayu_sms_wms_tlv(struct vayu_sms_wms_note *n, uint8_t type, const uint8_t *v, uint16_t length)
{
	bool from_modem = n->direction == VAYU_SMS_WMS_MODEM_TO_AP;
	bool request = n->qmi_type == VAYU_SMS_WMS_REQUEST;
	bool response = n->qmi_type == VAYU_SMS_WMS_RESPONSE;
	bool indication = n->qmi_type == VAYU_SMS_WMS_INDICATION;

	if (response && type == 0x02 && length >= 4) {
		n->result_valid = true;
		n->result = vayu_sms_wms_get16(v);
		n->error = vayu_sms_wms_get16(v + 2);
		return;
	}
	switch (n->message) {
	case VAYU_SMS_WMS_EVENT_REPORT:
		if (request && type == 0x10 && length >= 1) {
			n->report_mt_valid = true;
			n->report_mt = v[0];
		} else if (indication && from_modem) {
			if (type == 0x10 && length >= 5) {
				n->mt_stored = true;
				n->storage = v[0];
				n->index = vayu_sms_wms_get32(v + 1);
			} else if (type == 0x11 && length >= 8) {
				uint16_t pdu = vayu_sms_wms_get16(v + 6);

				if ((uint32_t)pdu + 8 <= length) {
					n->mt_transfer = true;
					n->ack_indicator = v[0];
					n->wms_transaction = vayu_sms_wms_get32(v + 1);
					n->format = v[5];
					n->pdu_length = pdu;
				}
			} else if (type == 0x12 && length >= 1) {
				n->mode_valid = true;
				n->mode = v[0];
			} else if (type == 0x16 && length >= 1) {
				n->sms_on_ims_valid = true;
				n->sms_on_ims = v[0];
			}
		}
		break;
	case VAYU_SMS_WMS_RAW_SEND:
	case VAYU_SMS_WMS_ASYNC_RAW_SEND:
		if (request && type == 0x01 && length >= 3) {
			uint16_t pdu = vayu_sms_wms_get16(v + 1);

			if ((uint32_t)pdu + 3 <= length) {
				n->send_valid = true;
				n->send_format = v[0];
				n->send_length = pdu;
			}
		} else if (response && n->message == VAYU_SMS_WMS_RAW_SEND) {
			if (type == 0x01 && length >= 2) {
				n->message_id_valid = true;
				n->message_id = vayu_sms_wms_get16(v);
			} else {
				vayu_sms_wms_send_causes(n, type, v, length, 0x10);
			}
		} else if (indication && n->message == VAYU_SMS_WMS_ASYNC_RAW_SEND) {
			if (type == 0x01 && length >= 2) {
				n->status_valid = true;
				n->status = vayu_sms_wms_get16(v);
			} else if (type == 0x10 && length >= 2) {
				n->message_id_valid = true;
				n->message_id = vayu_sms_wms_get16(v);
			} else {
				vayu_sms_wms_send_causes(n, type, v, length, 0x11);
			}
		}
		break;
	case VAYU_SMS_WMS_SEND_ACK:
	case VAYU_SMS_WMS_ASYNC_SEND_ACK:
		if (request && type == 0x01 && length >= 6) {
			n->ack_valid = true;
			n->ack_transaction = vayu_sms_wms_get32(v);
			n->ack_protocol = v[4];
			n->ack_success = v[5];
		} else if ((response || indication) && type == 0x10 && length >= 1) {
			n->ack_failure_valid = true;
			n->ack_failure = v[0];
		} else if (indication && type == 0x01 && length >= 2) {
			n->status_valid = true;
			n->status = vayu_sms_wms_get16(v);
		}
		break;
	case VAYU_SMS_WMS_SET_ROUTES:
		if (request && type == 0x01 && length >= 2) {
			uint16_t count = vayu_sms_wms_get16(v);

			if ((uint32_t)count * 4 + 2 <= length) {
				n->routes_valid = true;
				n->route_count = count;
				for (uint16_t i = 0; i < count && i < VAYU_SMS_WMS_ROUTES_MAX; i++) {
					n->route[i].type = v[2 + 4 * i];
					n->route[i].message_class = v[3 + 4 * i];
					n->route[i].storage = v[4 + 4 * i];
					n->route[i].action = v[5 + 4 * i];
				}
			}
		} else if (request && type == 0x10 && length >= 1) {
			n->transfer_ind_valid = true;
			n->transfer_ind = v[0];
		}
		break;
	case VAYU_SMS_WMS_LIST:
		/* 0x01 per the IDL; 0x10 is what an older firmware used (bsd/dev/vayu_sms.c accepts both) */
		if (response && (type == 0x01 || type == 0x10) && length >= 4) {
			uint32_t count = vayu_sms_wms_get32(v);

			if ((uint64_t)count * 5 + 4 <= length) {
				n->list_valid = true;
				n->list_count = count;
			}
		}
		break;
	case VAYU_SMS_WMS_BIND_SUBSCRIPTION:
		if (request && type == 0x01 && length >= 1) {
			n->subscription_valid = true;
			n->subscription = v[0];
		}
		break;
	case VAYU_SMS_WMS_SERVICE_READY:
		if (indication && type == 0x01 && length >= 4) {
			n->ready_valid = true;
			n->ready_status = vayu_sms_wms_get32(v);
		}
		break;
	case VAYU_SMS_WMS_GET_READY_STATUS:
		if (response && type == 0x11 && length >= 4) {
			n->ready_valid = true;
			n->ready_status = vayu_sms_wms_get32(v);
		}
		break;
	case VAYU_SMS_WMS_NW_REG:
		if (indication && type == 0x01 && length >= 1) {
			n->nw_reg_valid = true;
			n->nw_reg = v[0];
		}
		break;
	default:
		break;
	}
}

/*
 * Read one QMI message (the 7-byte header: type, u16 transaction, u16 message, u16
 * length, then the TLVs) as it went in `direction`.  Returns 0, or -1 when the frame is
 * not a whole QMI message; a TLV that runs past the end stops the walk, and what was read
 * before it stays in the note.
 */
static __inline__ int
vayu_sms_wms_parse(uint8_t direction, const uint8_t *qmi, size_t length, struct vayu_sms_wms_note *n)
{
	size_t at, end;

	memset(n, 0, sizeof(*n));
	if (qmi == NULL || length < 7) {
		return -1;
	}
	n->direction = direction;
	n->qmi_type = qmi[0] & 0x06;
	n->transaction = vayu_sms_wms_get16(qmi + 1);
	n->message = vayu_sms_wms_get16(qmi + 3);
	n->tlv_length = vayu_sms_wms_get16(qmi + 5);
	if ((size_t)n->tlv_length + 7 > length) {
		return -1;
	}
	end = 7 + (size_t)n->tlv_length;
	for (at = 7; at + 3 <= end;) {
		uint8_t type = qmi[at];
		uint16_t tlv = vayu_sms_wms_get16(qmi + at + 1);

		if (at + 3 + tlv > end) {
			break;
		}
		vayu_sms_wms_tlv(n, type, qmi + at + 3, tlv);
		at += 3 + (size_t)tlv;
	}
	return 0;
}

#endif /* _SYS_VAYU_SMS_WMS_H_ */
