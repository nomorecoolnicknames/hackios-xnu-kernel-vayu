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
	uint8_t pdu[VAYU_SMS_PDU_MAX], message[512];
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
	test_qmi_raw_read();
	test_qmi_list_decode();
	test_qmi_event_decode();
	test_qmi_delete();
	test_qmi_send_ack();
	test_qmi_routes();

	printf("sms-test: %d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
