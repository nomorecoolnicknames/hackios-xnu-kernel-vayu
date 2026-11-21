/*
 * Host tests for bsd/sys/vayu_sms_qmi.h: the GSM 03.40/03.38 PDU codec and the QMI
 * WMS message tables of vayu.
 *
 * Everything is synthetic: made-up numbers, made-up bodies, hand-written PDUs.  A
 * failure prints lengths and offsets, never a number, a body or a PDU byte.  No
 * modem is contacted and nothing is sent.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/hakios.qmi.h>
#include <sys/vayu_sms_qmi.h>
#include <sys/vayu_sms_wms.h>

static int checks, failures;

static void
check(bool ok, const char *format, ...)
{
	va_list list;

	checks++;
	if (ok) {
		return;
	}
	failures++;
	printf("FAIL: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

static void
check_status(int got, int want, const char *what)
{
	check(got == want, "%s: got %d, want %d", what, got, want);
}

/* hakios_qmi_decode returns the bytes decoded (positive), not 0. */
static void
check_decoded(int got, int want, const char *what)
{
	check(got == want, "%s: decoded %d bytes, want %d", what, got, want);
}

static void
check_bytes(const uint8_t *got, size_t got_length, const uint8_t *want, size_t want_length, const char *what)
{
	size_t at = 0;
	bool same = got_length == want_length;

	if (same) {
		while (at < want_length && got[at] == want[at]) {
			at++;
		}
		same = at == want_length;
	}
	check(same, "%s: %zu bytes, %zu wanted, first difference at %zu", what, got_length, want_length, at);
}

static void
check_text(const char *got, const char *want, const char *what)
{
	check(strcmp(got, want) == 0, "%s: text differs (%zu vs %zu bytes)", what, strlen(got), strlen(want));
}

/* ---- GSM 7-bit ------------------------------------------------------------------------------- */

static void
test_gsm7(void)
{
	static const uint8_t hello[] = { 'h', 'e', 'l', 'l', 'o' };
	static const uint8_t hello_packed[] = { 0xe8, 0x32, 0x9b, 0xfd, 0x06 };
	uint8_t packed[32], septets[32];
	size_t packed_length = 0, septet_count = 0;
	char text[128];
	size_t text_length = 0;

	check_status(vayu_sms_gsm7_pack(hello, sizeof(hello), packed, sizeof(packed), &packed_length),
	    VAYU_SMS_OK, "pack hello");
	check_bytes(packed, packed_length, hello_packed, sizeof(hello_packed), "packed hello");

	check_status(vayu_sms_gsm7_unpack_at(hello_packed, sizeof(hello_packed), 0, 5, septets, sizeof(septets)),
	    VAYU_SMS_OK, "unpack hello");
	check_bytes(septets, 5, hello, sizeof(hello), "unpacked hello");

	/* the extension table: euro is ESC 0x65, brackets are ESC 0x3c / ESC 0x3e */
	check_status(vayu_sms_utf8_to_gsm7("a\xe2\x82\xac[", 5, septets, sizeof(septets), &septet_count),
	    VAYU_SMS_OK, "utf8 to septets");
	{
		static const uint8_t want[] = { 0x61, 0x1b, 0x65, 0x1b, 0x3c };

		check_bytes(septets, septet_count, want, sizeof(want), "escaped septets");
	}

	/* Cyrillic has no GSM 7-bit form: the caller must fall back to UCS-2 */
	check_status(vayu_sms_utf8_to_gsm7("\xd0\x9f\xd1\x80\xd0\xb8", 6, septets, sizeof(septets), &septet_count),
	    VAYU_SMS_ERR_UNSUPPORTED, "cyrillic is not GSM7");

	/* 160 septets fit, 161 do not */
	{
		char many[200];
		uint8_t many_septets[VAYU_SMS_SEPTETS_MAX];

		memset(many, 'a', sizeof(many));
		check_status(vayu_sms_utf8_to_gsm7(many, 160, many_septets, sizeof(many_septets), &septet_count),
		    VAYU_SMS_OK, "160 septets");
		check_status(vayu_sms_utf8_to_gsm7(many, 161, many_septets, sizeof(many_septets), &septet_count),
		    VAYU_SMS_ERR_TOO_LONG, "161 septets");
	}

	/* text back from a septet stream, including a non-ASCII default-alphabet character */
	{
		static const uint8_t gsm[] = { 0x20, 0x01, 0x20 };    /* space, pound, space */

		check_status(vayu_sms_gsm7_to_utf8(gsm, 3, text, sizeof(text), &text_length), VAYU_SMS_OK, "gsm7 to utf8");
		check(text_length == 4 && text[1] == (char)0xc2 && text[2] == (char)0xa3, "pound is UTF-8 C2 A3");
	}
}

/* ---- SMS-DELIVER ----------------------------------------------------------------------------- */

/*
 * A published SMS-DELIVER: SMSC +31624000000, sender +31641600986, 2002-08-26
 * 19:37:41, GSM 7-bit, "How are you?".
 */
static const uint8_t deliver_hello[] = {
	0x07, 0x91, 0x13, 0x26, 0x04, 0x00, 0x00, 0xf0,
	0x04, 0x0b, 0x91, 0x13, 0x46, 0x61, 0x00, 0x89, 0xf6,
	0x00, 0x00, 0x20, 0x80, 0x62, 0x91, 0x73, 0x14, 0x08,
	0x0c, 0xc8, 0xf7, 0x1d, 0x14, 0x96, 0x97, 0x41, 0xf9, 0x77, 0xfd, 0x07,
};

static void
test_deliver_gsm7(void)
{
	struct vayu_sms_deliver deliver;

	check_status(vayu_sms_pdu_decode_deliver(deliver_hello, sizeof(deliver_hello), &deliver), VAYU_SMS_OK,
	    "decode hello");
	check_text(deliver.originator.text, "+31641600986", "hello sender");
	check_text(deliver.text, "How are you?", "hello body");
	check(deliver.alphabet == VAYU_SMS_ALPHABET_GSM7, "hello alphabet");
	check_text(deliver.timestamp, "2002-08-26 19:37:41 -0000", "hello timestamp");
	check(!deliver.concatenated, "hello is not concatenated");

	/* a truncated PDU is refused, not read past its end */
	for (size_t cut = 1; cut < sizeof(deliver_hello); cut++) {
		int status = vayu_sms_pdu_decode_deliver(deliver_hello, cut, &deliver);

		check(status != VAYU_SMS_OK, "a %zu-byte prefix must not decode", cut);
	}
}

/* A UCS-2 SMS-DELIVER with a Cyrillic body. */
static void
test_deliver_ucs2(void)
{
	static const uint8_t deliver[] = {
		0x00,                                           /* no SMSC */
		0x04, 0x0b, 0x91, 0x13, 0x46, 0x61, 0x00, 0x89, 0xf6,
		0x00, 0x08, 0x62, 0x10, 0x20, 0x10, 0x30, 0x00, 0x00,
		0x0c, 0x04, 0x1f, 0x04, 0x40, 0x04, 0x38, 0x04, 0x32, 0x04, 0x35, 0x04, 0x42,
	};
	struct vayu_sms_deliver deliver_out;

	check_status(vayu_sms_pdu_decode_deliver(deliver, sizeof(deliver), &deliver_out), VAYU_SMS_OK, "decode ucs2");
	check_text(deliver_out.text, "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "ucs2 body");
	check(deliver_out.alphabet == VAYU_SMS_ALPHABET_UCS2, "ucs2 alphabet");
	check_text(deliver_out.originator.text, "+31641600986", "ucs2 sender");
	check_text(deliver_out.timestamp, "2026-01-02 01:03:00 +0000", "ucs2 timestamp");
}

/* A UCS-2 segment with a UDH: TP-UDL counts the header octets too. */
static void
test_deliver_ucs2_udh(void)
{
	static const uint8_t deliver[] = {
		0x00, 0x44,
		0x0b, 0x91, 0x13, 0x46, 0x61, 0x00, 0x89, 0xf6,
		0x00, 0x08, 0x62, 0x10, 0x20, 0x10, 0x30, 0x00, 0x00,
		0x12,
		0x05, 0x00, 0x03, 0x01, 0x02, 0x01,
		0x04, 0x1f, 0x04, 0x40, 0x04, 0x38, 0x04, 0x32, 0x04, 0x35, 0x04, 0x42,
	};
	struct vayu_sms_deliver decoded;

	check_status(vayu_sms_pdu_decode_deliver(deliver, sizeof(deliver), &decoded), VAYU_SMS_OK, "decode ucs2 udh");
	check_text(decoded.text, "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "ucs2 udh body");
	check(decoded.concatenated && decoded.concat_total == 2 && decoded.concat_sequence == 1, "ucs2 udh header");
}

/*
 * A UDH concatenation header shifts the 7-bit text to the next septet boundary.
 * The test packs "Hello" at that boundary itself, then expects the decoder to
 * find it there and to report the header.
 */
static void
test_deliver_udh(void)
{
	uint8_t pdu[64];
	uint8_t septets[8] = { 'H', 'e', 'l', 'l', 'o' }, packed[8];
	size_t packed_length = 0, at = 0, skip, relative;
	struct vayu_sms_deliver deliver;

	/* 05 00 03 01 02 01: concat, 2 parts, this is part 1 */
	skip = ((6u * 8u) + 6u) / 7u;                   /* 7 septets of padding */
	relative = skip * 7u - 6u * 8u;                 /* 1 bit */
	check_status(vayu_sms_gsm7_pack(septets, 5, packed, sizeof(packed), &packed_length), VAYU_SMS_OK, "pack udh text");
	pdu[at++] = 0x00;
	pdu[at++] = 0x44;                               /* SMS-DELIVER with TP-UDHI */
	pdu[at++] = 0x0b;
	pdu[at++] = 0x91;
	pdu[at++] = 0x13; pdu[at++] = 0x46; pdu[at++] = 0x61; pdu[at++] = 0x00;
	pdu[at++] = 0x89; pdu[at++] = 0xf6;
	pdu[at++] = 0x00;
	pdu[at++] = 0x00;
	pdu[at++] = 0x62; pdu[at++] = 0x10; pdu[at++] = 0x20; pdu[at++] = 0x10;
	pdu[at++] = 0x30; pdu[at++] = 0x00; pdu[at++] = 0x00;
	pdu[at++] = (uint8_t)(skip + 5);                /* UDL: header padding + 5 text septets */
	pdu[at++] = 0x05; pdu[at++] = 0x00; pdu[at++] = 0x03; pdu[at++] = 0x01;
	pdu[at++] = 0x02; pdu[at++] = 0x01;
	/* the 5 text septets, shifted by `relative` bits inside the next octets */
	{
		size_t bits = relative, out = at;
		memset(pdu + out, 0, 6);

		for (size_t i = 0; i < 5; i++) {
			size_t bit = bits + i * 7u;

			pdu[out + bit / 8u] |= (uint8_t)(septets[i] << (bit % 8u));
			if ((bit % 8u) > 1u) {
				pdu[out + bit / 8u + 1u] |= (uint8_t)(septets[i] >> (8u - (bit % 8u)));
			}
		}
		at += (bits + 5 * 7u + 7u) / 8u;
	}
	check_status(vayu_sms_pdu_decode_deliver(pdu, at, &deliver), VAYU_SMS_OK, "decode udh");
	check_text(deliver.text, "Hello", "udh body");
	check(deliver.concatenated, "udh is reported");
	check(deliver.concat_total == 2 && deliver.concat_sequence == 1, "udh sequence");
	check(deliver.user_data_header_length == 5, "udh length");
}

/* ---- SMS-SUBMIT ------------------------------------------------------------------------------ */

static void
test_submit_gsm7(void)
{
	uint8_t pdu[VAYU_SMS_PDU_MAX];
	static const uint8_t want[] = {
		0x00, 0x01, 0x00, 0x0b, 0x91,
		0x97, 0x00, 0x21, 0x43, 0x65, 0xf7,
		0x00, 0x00, 0x05,
		0xe8, 0x32, 0x9b, 0xfd, 0x06,
	};
	size_t length = 0;
	uint32_t dcs = 0xffff;

	check_status(vayu_sms_pdu_encode_submit("+79001234567", "hello", 5, 0, pdu, sizeof(pdu), &length, &dcs),
	    VAYU_SMS_OK, "encode submit");
	check_bytes(pdu, length, want, sizeof(want), "submit pdu");
	check(dcs == 0x00, "submit dcs");
}

static void
test_submit_ucs2(void)
{
	uint8_t pdu[VAYU_SMS_PDU_MAX];
	static const uint8_t want[] = {
		0x00, 0x01, 0x07, 0x0b, 0x91,
		0x97, 0x00, 0x21, 0x43, 0x65, 0xf7,
		0x00, 0x08, 0x0c,
		0x04, 0x1f, 0x04, 0x40, 0x04, 0x38, 0x04, 0x32, 0x04, 0x35, 0x04, 0x42,
	};
	size_t length = 0;
	uint32_t dcs = 0xffff;

	check_status(vayu_sms_pdu_encode_submit("+79001234567", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", 12,
	    7, pdu, sizeof(pdu), &length, &dcs), VAYU_SMS_OK, "encode ucs2 submit");
	check_bytes(pdu, length, want, sizeof(want), "ucs2 submit pdu");
	check(dcs == 0x08, "ucs2 submit dcs");

	/* and the PDU it made decodes as the same text when read as a DELIVER?  No: a
	 * SUBMIT is not a DELIVER.  Encode/decode agreement is tested on DELIVER above. */
}

static void
test_submit_limits(void)
{
	uint8_t pdu[VAYU_SMS_PDU_MAX];
	char many[200];
	size_t length = 0;
	uint32_t dcs;

	memset(many, 'a', sizeof(many));
	check_status(vayu_sms_pdu_encode_submit("12345", many, 160, 0, pdu, sizeof(pdu), &length, &dcs), VAYU_SMS_OK,
	    "160 characters fit");
	check_status(vayu_sms_pdu_encode_submit("12345", many, 161, 0, pdu, sizeof(pdu), &length, &dcs),
	    VAYU_SMS_ERR_TOO_LONG, "161 characters do not fit");
	check_status(vayu_sms_pdu_encode_submit("+", "hello", 5, 0, pdu, sizeof(pdu), &length, &dcs),
	    VAYU_SMS_ERR_ADDRESS, "a lone plus is not an address");
	check_status(vayu_sms_pdu_encode_submit("12a45", "hello", 5, 0, pdu, sizeof(pdu), &length, &dcs),
	    VAYU_SMS_ERR_ADDRESS, "letters are not digits");
}

/* ---- QMI WMS messages ------------------------------------------------------------------------ */

static void
test_qmi_set_event(void)
{
	struct vayu_sms_qmi_set_event_req request = { .report = 1 };
	static const uint8_t want[] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x10, 0x01, 0x00, 0x01 };
	uint8_t message[64];
	int length;

	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_SET_EVENT_REPORT, 0, vayu_sms_qmi_set_event_report_req_ei,
	    &request, message, sizeof(message));
	check(length == (int)sizeof(want), "set event encode length: %d", length);
	if (length > 0) {
		check_bytes(message, (size_t)length, want, sizeof(want), "set event message");
	}
}

static void
test_qmi_raw_send(void)
{
	uint8_t pdu[VAYU_SMS_PDU_MAX], message[512], legacy[512], omitted[512];
	struct vayu_sms_qmi_raw_send_req request;
	size_t pdu_length = 0;
	uint32_t dcs;
	int length;

	check_status(vayu_sms_pdu_encode_submit("+79001234567", "hello", 5, 0, pdu, sizeof(pdu), &pdu_length, &dcs),
	    VAYU_SMS_OK, "send pdu");
	check_status(vayu_sms_qmi_raw_send_fill(&request, pdu, pdu_length), VAYU_SMS_OK, "fill raw send");
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_RAW_SEND, 1, vayu_sms_qmi_raw_send_req_ei, &request,
	    message, sizeof(message));
	check(length > 7, "raw send encode length: %d", length);
	if (length > 7) {
		/* header, then TLV 0x01, length 1 + 2 + pdu, format 0x06, u16 length */
		check(message[7] == 0x01, "raw send TLV type");
		check((size_t)(message[8] | message[9] << 8) == 1 + 2 + pdu_length, "raw send TLV length");
		check(message[10] == 0x06, "raw send format");
		check((size_t)(message[11] | message[12] << 8) == pdu_length, "raw send pdu length");
		check(memcmp(message + 13, pdu, pdu_length) == 0, "raw send pdu bytes");
	}
	check(length == (int)(17 + pdu_length), "raw send total length: %d", length);
	check(memcmp(message + 13 + pdu_length, (uint8_t[]){ 0x02, 0x01, 0x00, 0x00 }, 4) == 0,
	    "raw send SMS-on-IMS TLV 0x02");
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_RAW_SEND, 1, vayu_sms_qmi_raw_send_req_legacy_ei,
	    &request, legacy, sizeof(legacy));
	check(length == (int)(17 + pdu_length), "legacy raw send length: %d", length);
	message[13 + pdu_length] = 0x13;
	check_bytes(legacy, length > 0 ? (size_t)length : 0, message, 17 + pdu_length, "legacy raw send TLV 0x13");
	request.sms_on_ims_valid = 0;
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_RAW_SEND, 1, vayu_sms_qmi_raw_send_req_ei,
	    &request, omitted, sizeof(omitted));
	message[5] -= 4;
	check_bytes(omitted, length > 0 ? (size_t)length : 0, message, 13 + pdu_length, "raw send without IMS TLV");
	check_status(vayu_sms_qmi_raw_send_fill_format(&request, 1, pdu, pdu_length), VAYU_SMS_OK,
	    "fill raw send format 1");
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_RAW_SEND, 1, vayu_sms_qmi_raw_send_req_ei,
	    &request, message, sizeof(message));
	check(length == (int)(17 + pdu_length) && message[10] == 1, "raw send format 1 on wire");
}

/* A RAW_SEND answer with no Result TLV: a zeroed result must not read as the modem's success. */
static void
test_qmi_missing_result(void)
{
	static const uint8_t empty[] = { 0x02, 1, 0, 0x20, 0, 0, 0 };
	static const uint8_t id_only[] = { 0x02, 1, 0, 0x20, 0, 5, 0, 0x01, 2, 0, 42, 0 };
	struct vayu_sms_qmi_raw_send_resp response = { 0 };

	check(hakios_qmi_decode(empty, sizeof(empty), vayu_sms_qmi_raw_send_resp_ei, &response) < 0,
	    "RAW_SEND without Result cannot succeed");
	check(hakios_qmi_decode(id_only, sizeof(id_only), vayu_sms_qmi_raw_send_resp_ei, &response) < 0,
	    "RAW_SEND message id without Result cannot succeed");
}

static void
test_qmi_raw_read(void)
{
	struct vayu_sms_qmi_raw_read_req request = { .id = { .storage = 0, .index = 42 } };
	uint8_t message[128];
	int length;

	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_RAW_READ, 2, vayu_sms_qmi_raw_read_req_ei, &request,
	    message, sizeof(message));
	check(length > 7, "raw read encode length: %d", length);
	if (length > 7) {
		check(message[7] == 0x01, "raw read TLV");
		check(message[10] == 0x00, "raw read storage");
		check((uint32_t)(message[11] | message[12] << 8 | message[13] << 16 | (uint32_t)message[14] << 24) == 42,
		    "raw read index");
	}
}

static void
test_qmi_list_request(void)
{
	uint8_t message[64];
	uint8_t want[] = {
		0x00, 0x04, 0x00, 0x31, 0x00, 0x0c, 0x00,
		0x01, 0x01, 0x00, 0x00, 0x11, 0x01, 0x00, 0x01, 0x12, 0x01, 0x00, 0x01,
	};
	struct vayu_sms_qmi_list_req request = { .storage = 0, .tag = 1, .mode = VAYU_SMS_QMI_MODE_GSM };
	int length;

	/* SMS4_REPORT: the exact 19-byte List request captured on the phone. */
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_LIST_MESSAGES, 4, vayu_sms_qmi_list_req_ei,
	    &request, message, sizeof(message));
	check(length == (int)sizeof(want), "list tag 1 length: %d", length);
	check_bytes(message, length > 0 ? (size_t)length : 0, want, sizeof(want), "list tag 1 wire bytes");
	request.tag = 0;
	want[14] = 0;
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_LIST_MESSAGES, 4, vayu_sms_qmi_list_req_ei,
	    &request, message, sizeof(message));
	check(length == (int)sizeof(want), "list tag 0 length: %d", length);
	check_bytes(message, length > 0 ? (size_t)length : 0, want, sizeof(want), "list tag 0 wire bytes");
}

static void
test_qmi_raw_send_resp(void)
{
	static const uint8_t refused[] = {
		0x02, 0x01, 0x00, 0x20, 0x00, 0x07, 0x00,
		0x02, 0x04, 0x00, 0x01, 0x00, 0x30, 0x00,
	};
	static const uint8_t success[] = {
		0x02, 0x01, 0x00, 0x20, 0x00, 0x12, 0x00,
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x02, 0x00, 0x34, 0x12,
		0x12, 0x03, 0x00, 0x00, 0x00, 0x00,
	};
	struct vayu_sms_qmi_raw_send_resp decoded;

	memset(&decoded, 0, sizeof(decoded));
	check_decoded(hakios_qmi_decode(refused, sizeof(refused), vayu_sms_qmi_raw_send_resp_ei, &decoded),
	    (int)sizeof(refused), "decode raw send refusal");
	check(decoded.result.result == 1 && decoded.result.error == 48, "raw send refusal result/error");
	check(!decoded.message_id_valid, "raw send refusal without message id");
	memset(&decoded, 0, sizeof(decoded));
	check_decoded(hakios_qmi_decode(success, sizeof(success), vayu_sms_qmi_raw_send_resp_ei, &decoded),
	    (int)sizeof(success), "decode raw send success");
	check(decoded.result.result == 0 && decoded.result.error == 0 && decoded.message_id_valid &&
	    decoded.message_id == 0x1234, "raw send success and message id");
	check(decoded.cause_valid && decoded.cause.rp_cause == 0 && decoded.cause.tp_cause == 0,
	    "raw send GW cause");
}

static void
test_qmi_list_decode(void)
{
	static const uint8_t message[] = {
		0x02, 0x04, 0x00, 0x31, 0x00, 0x18, 0x00,
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x0e, 0x00,
		0x02, 0x00, 0x00, 0x00,
		0x2a, 0x00, 0x00, 0x00, 0x01,
		0x2b, 0x00, 0x00, 0x00, 0x00,
	};
	struct vayu_sms_qmi_list_resp decoded;
	struct vayu_sms_qmi_message_list list;

	/* a reply with result success and a two-entry message list */
	check_decoded(hakios_qmi_decode(message, sizeof(message), vayu_sms_qmi_list_resp_ei, &decoded),
	    (int)sizeof(message), "decode list");
	check(decoded.list_valid == 1, "list TLV present");
	check(decoded.result.result == 0 && decoded.result.error == 0, "list result");
	check_status(vayu_sms_qmi_list_parse(decoded.list, 4 + 2 * 5, &list), VAYU_SMS_OK, "parse list");
	check(list.count == 2, "list count: %u", list.count);
	check(list.entry[0].index == 42 && list.entry[0].tag == 1, "list entry 0");
	check(list.entry[1].index == 43 && list.entry[1].tag == 0, "list entry 1");

	/* a count that overruns the payload is refused */
	check_status(vayu_sms_qmi_list_parse(decoded.list, 9, &list), VAYU_SMS_ERR_MALFORMED, "short list");

	{
		uint8_t message10[sizeof(message)];

		memcpy(message10, message, sizeof(message));
		message10[14] = 0x10;
		memset(&decoded, 0, sizeof(decoded));
		check_decoded(hakios_qmi_decode(message10, sizeof(message10), vayu_sms_qmi_list_resp_ei, &decoded),
		    (int)sizeof(message10), "decode list TLV 0x10");
		check(decoded.list10_valid && !decoded.list_valid, "list TLV 0x10 present without 0x01");
		check_status(vayu_sms_qmi_list_parse(decoded.list10, 4 + 2 * 5, &list), VAYU_SMS_OK, "parse list 0x10");
		check(list.count == 2, "list 0x10 count: %u", list.count);
		check(list.entry[0].index == 42 && list.entry[0].tag == 1, "list 0x10 entry 0");
		check(list.entry[1].index == 43 && list.entry[1].tag == 0, "list 0x10 entry 1");
	}

}

static void
test_qmi_event_decode(void)
{
	uint8_t message[256];
	struct vayu_sms_qmi_event_report decoded;
	static const uint8_t header[] = { 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
	size_t at = 0, pdu_length = sizeof(deliver_hello);

	memcpy(message, header, sizeof(header));
	at = sizeof(header);
	/* TLV 0x10: storage UIM, index 3 */
	message[at++] = 0x10; message[at++] = 0x05; message[at++] = 0x00;
	message[at++] = 0x00;
	message[at++] = 0x03; message[at++] = 0x00; message[at++] = 0x00; message[at++] = 0x00;
	/* TLV 0x11: ack needed, transaction 9, format 6, PDU */
	message[at++] = 0x11;
	message[at++] = (uint8_t)(4 + 1 + 1 + 2 + pdu_length);
	message[at++] = (uint8_t)((4 + 1 + 1 + 2 + pdu_length) >> 8);
	message[at++] = 0x00;
	message[at++] = 0x09; message[at++] = 0x00; message[at++] = 0x00; message[at++] = 0x00;
	message[at++] = 0x06;
	message[at++] = (uint8_t)pdu_length; message[at++] = (uint8_t)(pdu_length >> 8);
	memcpy(message + at, deliver_hello, pdu_length);
	at += pdu_length;
	/* fix the QMI payload length */
	message[5] = (uint8_t)(at - 7);
	message[6] = (uint8_t)((at - 7) >> 8);

	memset(&decoded, 0, sizeof(decoded));
	check_decoded(hakios_qmi_decode(message, at, vayu_sms_qmi_event_report_ei, &decoded), (int)at,
	    "decode event report");
	check(decoded.mt_valid == 1 && decoded.mt.storage == 0 && decoded.mt.index == 3, "event storage/index");
	check(decoded.route_valid == 1 && decoded.route.ack == 0 && decoded.route.transaction == 9, "event route");
	check(decoded.route.format == VAYU_SMS_QMI_FORMAT_GSM_P2P, "event format");
	check(decoded.route.raw_length == pdu_length, "event pdu length");
	check(memcmp(decoded.route.raw, deliver_hello, pdu_length) == 0, "event pdu bytes");
}

static void
test_qmi_delete(void)
{
	struct vayu_sms_qmi_delete_req request = { .storage = 0, .index = 7, .tag = 1 };
	uint8_t message[64];
	int length;

	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_DELETE, 3, vayu_sms_qmi_delete_req_ei, &request,
	    message, sizeof(message));
	check(length > 7, "delete encode length: %d", length);
	if (length > 7) {
		check(message[7] == 0x01 && message[10] == 0x00, "delete storage TLV");
		check(message[11] == 0x10 && message[14] == 7, "delete index TLV");
	}
}

static void
test_qmi_send_ack(void)
{
	struct vayu_sms_qmi_send_ack_req request = { .info = { .transaction = 9, .protocol = 1, .success = 1 } };
	uint8_t message[64];
	int length;

	length = hakios_qmi_encode(QMI_REQUEST, VAYU_SMS_QMI_SEND_ACK, 4, vayu_sms_qmi_send_ack_req_ei, &request,
	    message, sizeof(message));
	check(length == 7 + 3 + 6, "send ack encode length: %d", length);
	if (length > 7) {
		check(message[7] == 0x01, "send ack TLV type");
		check((message[8] | message[9] << 8) == 6, "send ack is one six-byte TLV");
		check((uint32_t)(message[10] | message[11] << 8 | message[12] << 16 | (uint32_t)message[13] << 24) == 9,
		    "send ack transaction");
		check(message[14] == 1 && message[15] == 1, "send ack protocol/success");
	}
}

static void
test_qmi_routes(void)
{
	struct vayu_sms_qmi_get_routes_resp decoded;
	uint8_t message[128];
	static const uint8_t body[] = {
		0x02, 0x05, 0x00, 0x33, 0x00, 0x14, 0x00,
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x0a, 0x00,
		0x02, 0x00,
		0x00, 0x01, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x02,
	};

	memcpy(message, body, sizeof(body));
	memset(&decoded, 0, sizeof(decoded));
	check_decoded(hakios_qmi_decode(message, sizeof(body), vayu_sms_qmi_get_routes_resp_ei, &decoded),
	    (int)sizeof(body), "decode routes");
	check(decoded.routes_valid == 1 && decoded.routes.count == 2, "routes count");
	check(decoded.routes.route[0].message_class == 1 && decoded.routes.route[0].storage == 0 &&
	    decoded.routes.route[0].receipt_action == 1, "route 0");
	check(decoded.routes.route[1].message_class == 0 && decoded.routes.route[1].receipt_action == 2, "route 1");
}

/* ---- bsd/sys/vayu_sms_wms.h: CommCenter's frames as the observer reads them ---------------- */

/* A QMI message: the 7-byte header, then the TLVs as given. */
static size_t
wms_frame(uint8_t *out, uint8_t type, uint16_t txn, uint16_t message, const uint8_t *tlvs, uint16_t length)
{
	out[0] = type;
	out[1] = (uint8_t)txn; out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)message; out[4] = (uint8_t)(message >> 8);
	out[5] = (uint8_t)length; out[6] = (uint8_t)(length >> 8);
	memcpy(out + 7, tlvs, length);
	return 7 + (size_t)length;
}

static void
test_wms_mt_transfer(void)
{
	static const uint8_t tlvs[] = {
		0x11, 0x0b, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01, 0x06, 0x03, 0x00, 0xaa, 0xbb, 0xcc,
		0x12, 0x01, 0x00, 0x01,
		0x16, 0x01, 0x00, 0x01,
	};
	uint8_t frame[64];
	struct vayu_sms_wms_note n;
	size_t length = wms_frame(frame, 0x04, 7, 0x0001, tlvs, sizeof(tlvs));

	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms mt: parse");
	check(n.qmi_type == VAYU_SMS_WMS_INDICATION && n.message == 0x0001, "wms mt: type %u message 0x%04x",
	    n.qmi_type, n.message);
	check(n.mt_transfer && !n.mt_stored, "wms mt: transfer route seen");
	check(n.ack_indicator == 0 && n.wms_transaction == 0x01020304u, "wms mt: ack %u transaction 0x%08x",
	    n.ack_indicator, n.wms_transaction);
	check(n.format == 6 && n.pdu_length == 3, "wms mt: format %u, %u bytes", n.format, n.pdu_length);
	check(n.mode_valid && n.mode == 1 && n.sms_on_ims_valid && n.sms_on_ims == 1, "wms mt: mode and IMS");

	/* a PDU length that runs past its TLV is not a transfer route */
	{
		uint8_t bad[sizeof(tlvs)];

		memcpy(bad, tlvs, sizeof(tlvs));
		bad[9] = 0x09;
		length = wms_frame(frame, 0x04, 7, 0x0001, bad, sizeof(bad));
		check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms mt long: parse");
		check(!n.mt_transfer, "wms mt long: refused");
	}
	/* the same TLVs in a request from CommCenter are not an MT */
	length = wms_frame(frame, 0x00, 7, 0x0001, tlvs, sizeof(tlvs));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_AP_TO_MODEM, frame, length, &n), 0, "wms mt req: parse");
	check(!n.mt_transfer && !n.mt_stored, "wms mt req: not an MT");
}

static void
test_wms_mt_stored(void)
{
	static const uint8_t tlvs[] = { 0x10, 0x05, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00 };
	uint8_t frame[32];
	struct vayu_sms_wms_note n;
	size_t length = wms_frame(frame, 0x04, 1, 0x0001, tlvs, sizeof(tlvs));

	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms stored: parse");
	check(n.mt_stored && n.storage == 0 && n.index == 5 && !n.mt_transfer, "wms stored: storage %u index %u",
	    n.storage, n.index);
}

static void
test_wms_send_ack(void)
{
	static const uint8_t tlvs[] = { 0x01, 0x06, 0x00, 0x04, 0x03, 0x02, 0x01, 0x01, 0x01 };
	static const uint8_t reply[] = { 0x02, 0x04, 0x00, 0x01, 0x00, 0x30, 0x00, 0x10, 0x01, 0x00, 0x02 };
	uint8_t frame[32];
	struct vayu_sms_wms_note n;
	size_t length = wms_frame(frame, 0x00, 3, 0x0037, tlvs, sizeof(tlvs));

	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_AP_TO_MODEM, frame, length, &n), 0, "wms ack: parse");
	check(n.ack_valid && n.ack_transaction == 0x01020304u && n.ack_protocol == 1 && n.ack_success == 1,
	    "wms ack: fields");
	length = wms_frame(frame, 0x02, 3, 0x0037, reply, sizeof(reply));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms ack reply: parse");
	check(n.result_valid && n.result == 1 && n.error == 0x30, "wms ack reply: result %u error %u", n.result,
	    n.error);
	check(n.ack_failure_valid && n.ack_failure == 2, "wms ack reply: failure cause");
}

static void
test_wms_raw_send(void)
{
	static const uint8_t request[] = { 0x01, 0x07, 0x00, 0x06, 0x04, 0x00, 0x11, 0x22, 0x33, 0x44 };
	static const uint8_t ok[] = { 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x34, 0x12 };
	static const uint8_t refused[] = {
		0x02, 0x04, 0x00, 0x01, 0x00, 0x24, 0x00,
		0x10, 0x02, 0x00, 0x05, 0x00,
		0x11, 0x01, 0x00, 0x01,
		0x12, 0x03, 0x00, 0x29, 0x00, 0x00,
	};
	static const uint8_t async[] = {
		0x01, 0x02, 0x00, 0x00, 0x00,
		0x10, 0x02, 0x00, 0x07, 0x00,
		0x13, 0x03, 0x00, 0x2a, 0x00, 0x01,
	};
	uint8_t frame[64];
	struct vayu_sms_wms_note n;
	size_t length = wms_frame(frame, 0x00, 9, 0x0020, request, sizeof(request));

	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_AP_TO_MODEM, frame, length, &n), 0, "wms send: parse");
	check(n.send_valid && n.send_format == 6 && n.send_length == 4, "wms send: format %u, %u bytes",
	    n.send_format, n.send_length);
	length = wms_frame(frame, 0x02, 9, 0x0020, ok, sizeof(ok));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms send ok: parse");
	check(n.result_valid && n.result == 0 && n.message_id_valid && n.message_id == 0x1234,
	    "wms send ok: message id 0x%04x", n.message_id);
	length = wms_frame(frame, 0x02, 9, 0x0020, refused, sizeof(refused));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms send no: parse");
	check(n.result == 1 && n.error == 0x24 && n.cause_valid && n.cause == 5 && n.error_class_valid &&
	    n.error_class == 1, "wms send no: cause %u class %u", n.cause, n.error_class);
	check(n.gw_cause_valid && n.rp_cause == 41 && n.tp_cause == 0, "wms send no: RP cause %u", n.rp_cause);
	length = wms_frame(frame, 0x04, 0, 0x0059, async, sizeof(async));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms async: parse");
	check(n.status_valid && n.status == 0 && n.message_id_valid && n.message_id == 7, "wms async: status");
	check(n.gw_cause_valid && n.rp_cause == 42 && n.tp_cause == 1 && !n.cause_valid,
	    "wms async: causes one TLV later than the reply's");
}

static void
test_wms_routes(void)
{
	static const uint8_t request[] = {
		0x01, 0x0a, 0x00, 0x02, 0x00, 0x00, 0x00, 0xff, 0x02, 0x00, 0x01, 0xff, 0x03,
		0x10, 0x01, 0x00, 0x01,
	};
	uint8_t frame[64], bad[sizeof(request)];
	struct vayu_sms_wms_note n;
	size_t length = wms_frame(frame, 0x00, 2, 0x0032, request, sizeof(request));

	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_AP_TO_MODEM, frame, length, &n), 0, "wms routes: parse");
	check(n.routes_valid && n.route_count == 2, "wms routes: %u routes", n.route_count);
	check(n.route[0].message_class == 0 && n.route[0].storage == 0xff && n.route[0].action == 2 &&
	    n.route[1].message_class == 1 && n.route[1].action == 3, "wms routes: tuples");
	check(n.transfer_ind_valid && n.transfer_ind == 1, "wms routes: transfer status reports");
	memcpy(bad, request, sizeof(request));
	bad[3] = 3;                                     /* three routes claimed, two there */
	length = wms_frame(frame, 0x00, 2, 0x0032, bad, sizeof(bad));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_AP_TO_MODEM, frame, length, &n), 0, "wms routes bad: parse");
	check(!n.routes_valid, "wms routes bad: refused");
}

static void
test_wms_misc(void)
{
	static const uint8_t list[] = {
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x0e, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t ready[] = { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 };
	static const uint8_t ready_reply[] = {
		0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x01, 0x11, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00,
	};
	static const uint8_t bind[] = { 0x01, 0x01, 0x00, 0x00 };
	static const uint8_t truncated[] = { 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x09, 0x00, 0x01 };
	uint8_t frame[64];
	struct vayu_sms_wms_note n;
	size_t length = wms_frame(frame, 0x02, 4, 0x0031, list, sizeof(list));

	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms list: parse");
	check(n.list_valid && n.list_count == 2, "wms list: %u stored", n.list_count);
	length = wms_frame(frame, 0x04, 0, 0x005D, ready, sizeof(ready));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms ready: parse");
	check(n.ready_valid && n.ready_status == 1, "wms ready: %u", n.ready_status);
	check(strcmp(vayu_sms_wms_ready_name(n.ready_status), "3GPP") == 0, "wms ready: name");
	length = wms_frame(frame, 0x02, 5, 0x005C, ready_reply, sizeof(ready_reply));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms ready reply: parse");
	check(n.ready_valid && n.ready_status == 3, "wms ready reply: %u", n.ready_status);
	length = wms_frame(frame, 0x00, 6, 0x004C, bind, sizeof(bind));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_AP_TO_MODEM, frame, length, &n), 0, "wms bind: parse");
	check(n.subscription_valid && n.subscription == 0, "wms bind: subscription");
	/* a TLV past the end stops the walk, the result before it stays */
	length = wms_frame(frame, 0x02, 7, 0x0035, truncated, sizeof(truncated));
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length, &n), 0, "wms truncated: parse");
	check(n.result_valid && n.result == 0, "wms truncated: result kept");
	/* a header longer than the frame, or no header */
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, length - 1, &n), -1, "wms short frame");
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, frame, 6, &n), -1, "wms no header");
	check_status(vayu_sms_wms_parse(VAYU_SMS_WMS_MODEM_TO_AP, NULL, 0, &n), -1, "wms null");
	check(vayu_sms_wms_name(0x0037) != NULL && strcmp(vayu_sms_wms_name(0x0037), "send ack") == 0 &&
	    vayu_sms_wms_name(0x7777) == NULL, "wms names");
}

int
main(void)
{
	test_gsm7();
	test_deliver_gsm7();
	test_deliver_ucs2();
	test_deliver_ucs2_udh();
	test_deliver_udh();
	test_submit_gsm7();
	test_submit_ucs2();
	test_submit_limits();
	test_qmi_set_event();
	test_qmi_raw_send();
	test_qmi_missing_result();
	test_qmi_raw_send_resp();
	test_qmi_list_request();
	test_qmi_raw_read();
	test_qmi_list_decode();
	test_qmi_event_decode();
	test_qmi_delete();
	test_qmi_send_ack();
	test_qmi_routes();
	test_wms_mt_transfer();
	test_wms_mt_stored();
	test_wms_send_ack();
	test_wms_raw_send();
	test_wms_routes();
	test_wms_misc();

	printf("sms-test: %d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
