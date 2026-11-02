/*
 * The QMI WMS messages and the GSM 03.40/03.38 PDU codec of bsd/dev/vayu_sms.c.
 *
 * The structures and tables are here, apart from the client, so that a program on the
 * machine that builds the kernel codes and decodes the same messages and PDUs
 * (tools/sms-test).  The wire format (a 7-byte header, then TLVs) and the element
 * tables are sys/hakios.qmi.h's.
 *
 * WMS message ids and TLV layouts are libqmi's data/qmi-service-wms.json (master,
 * read 2026-11-02); enum values are src/libqmi-glib/qmi-enums-wms.h of the same:
 *   WMS 0x05  Set Event Report  0x0001  TLV 0x10 {new MT message report u8}
 *   WMS 0x05  Event Report      0x0001  indication: 0x10 {storage,index},
 *                                       0x11 {ack,transaction,format,raw}, 0x12 mode,
 *                                       0x16 SMS-on-IMS
 *   WMS 0x05  Raw Send          0x0020  TLV 0x01 {format, u16 length, PDU};
 *                                       reply 0x01 message id, 0x12 GSM cause
 *   WMS 0x05  Raw Read          0x0022  TLV 0x01 {storage,index}, 0x10 mode;
 *                                       reply 0x01 {tag,format,u16 length,PDU}
 *   WMS 0x05  List Messages     0x0031  input 0x01 storage; reply 0x01 array of
 *                                       {index u32, tag u8}, u32 count prefix
 *   WMS 0x05  Delete            0x0024  TLV 0x01 storage, 0x10 index, 0x11 tag
 *   WMS 0x05  Set/Get Routes    0x0032/0x0033  array of
 *                                       {type,class,storage,receipt action}, u16 count
 *   WMS 0x05  Send Ack          0x0037  TLV 0x01 {transaction,protocol,success}
 *
 * PDU: TS 23.040 SMS-DELIVER and SMS-SUBMIT, TS 23.038 GSM 7-bit default alphabet
 * (with the extension table) and UCS-2.  A message longer than one segment is
 * refused for sending and decoded one segment at a time for receiving; the UDH
 * concatenation header, when present, is reported in the deliver structure but the
 * segments are not joined.  That limit is deliberate: joining would need a
 * reassembly queue with its own timeouts, and this stage does not have one.
 *
 * None of these functions logs or stores anything by itself.
 */
#ifndef _SYS_VAYU_SMS_QMI_H_
#define _SYS_VAYU_SMS_QMI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/hakios.qmi.h>
#include <sys/vayu_sms.h>

/* ---- limits ---------------------------------------------------------------------------------- */

#define VAYU_SMS_SEPTETS_MAX            160     /* one GSM 7-bit segment */
#define VAYU_SMS_USER_OCTETS_MAX        140     /* TP-UD octets in one segment */
#define VAYU_SMS_TPDU_MAX               164     /* worst-case SMS-DELIVER TPDU */
#define VAYU_SMS_PDU_MAX                176     /* TPDU plus the SMSC prefix */
#define VAYU_SMS_TEXT_MAX               640     /* UTF-8 bytes of the decoded body */
#define VAYU_SMS_ADDRESS_MAX            32      /* a sender as text, plus NUL */
#define VAYU_SMS_TIMESTAMP_MAX          26      /* "2026-11-02 06:30:00 +0300" and NUL */
#define VAYU_SMS_ROUTE_MAX              16
#define VAYU_SMS_LIST_MAX               32
#define VAYU_SMS_QMI_REQUEST_MAX        512

#define VAYU_SMS_QMI_WMS_SERVICE        0x05
#define VAYU_SMS_QMI_SET_EVENT_REPORT   0x0001
#define VAYU_SMS_QMI_EVENT_REPORT       0x0001
#define VAYU_SMS_QMI_RAW_SEND           0x0020
#define VAYU_SMS_QMI_RAW_READ           0x0022
#define VAYU_SMS_QMI_DELETE             0x0024
#define VAYU_SMS_QMI_LIST_MESSAGES      0x0031
#define VAYU_SMS_QMI_SET_ROUTES         0x0032
#define VAYU_SMS_QMI_GET_ROUTES         0x0033
#define VAYU_SMS_QMI_SEND_ACK           0x0037

/* QmiWmsStorageType, QmiWmsMessageMode, QmiWmsMessageFormat, QmiWmsMessageTagType. */
#define VAYU_SMS_QMI_STORAGE_UIM        0x00
#define VAYU_SMS_QMI_STORAGE_NV         0x01
#define VAYU_SMS_QMI_STORAGE_NONE       0xff
#define VAYU_SMS_QMI_MODE_GSM           0x01
#define VAYU_SMS_QMI_FORMAT_GSM_P2P     0x06
#define VAYU_SMS_QMI_TAG_MT_READ        0x00
#define VAYU_SMS_QMI_TAG_MT_NOT_READ    0x01
#define VAYU_SMS_QMI_TAG_MO_SENT        0x02
#define VAYU_SMS_QMI_TAG_MO_NOT_SENT    0x03

/* QmiWmsMessageType, QmiWmsMessageClass, QmiWmsReceiptAction. */
#define VAYU_SMS_QMI_TYPE_P2P           0x00
#define VAYU_SMS_QMI_CLASS_0            0x00
#define VAYU_SMS_QMI_CLASS_1            0x01
#define VAYU_SMS_QMI_RECEIPT_DISCARD    0x00
#define VAYU_SMS_QMI_RECEIPT_STORE      0x01
#define VAYU_SMS_QMI_RECEIPT_TRANSFER   0x02
#define VAYU_SMS_QMI_RECEIPT_TRANSFER_ACK 0x03
#define VAYU_SMS_QMI_ACK_NEEDED         0x00

/* ---- status codes ---------------------------------------------------------------------------- */

enum {
	VAYU_SMS_OK = 0,
	VAYU_SMS_ERR_ARGUMENT = -1,
	VAYU_SMS_ERR_TRUNCATED = -2,
	VAYU_SMS_ERR_MALFORMED = -3,
	VAYU_SMS_ERR_UNSUPPORTED = -4,
	VAYU_SMS_ERR_TOO_LONG = -5,
	VAYU_SMS_ERR_BUFFER = -6,
	VAYU_SMS_ERR_ADDRESS = -7,
	VAYU_SMS_ERR_TIMESTAMP = -8,
};

/* VAYU_SMS_ALPHABET_* names the alphabet the DCS names; sys/vayu_sms.h has them. */

/* ---- UTF-8 ----------------------------------------------------------------------------------- */

/* Decode one UTF-8 sequence at *at; returns 0 and advances, or -1.  No overlongs, no surrogates. */
static __inline__ int
vayu_sms_utf8_next(const char *text, size_t length, size_t *at, uint32_t *codepoint)
{
	const uint8_t *s = (const uint8_t *)text;
	size_t i = *at;
	uint32_t cp;
	int extra;

	if (i >= length) {
		return -1;
	}
	if (s[i] < 0x80) {
		*codepoint = s[i];
		*at = i + 1;
		return 0;
	}
	if ((s[i] & 0xe0) == 0xc0) {
		cp = s[i] & 0x1f;
		extra = 1;
	} else if ((s[i] & 0xf0) == 0xe0) {
		cp = s[i] & 0x0f;
		extra = 2;
	} else if ((s[i] & 0xf8) == 0xf0) {
		cp = s[i] & 0x07;
		extra = 3;
	} else {
		return -1;
	}
	if (i + (size_t)extra >= length) {
		return -1;
	}
	for (int k = 1; k <= extra; k++) {
		if (i + (size_t)k >= length || (s[i + (size_t)k] & 0xc0) != 0x80) {
			return -1;
		}
		cp = (cp << 6) | (s[i + (size_t)k] & 0x3f);
	}
	if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
	    (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
		return -1;
	}
	*codepoint = cp;
	*at = i + 1 + (size_t)extra;
	return 0;
}

/* Append one codepoint as UTF-8; returns 0 or -1 when it does not fit. */
static __inline__ int
vayu_sms_utf8_put(char *text, size_t capacity, size_t *at, uint32_t cp)
{
	size_t i = *at;

	if (cp < 0x80) {
		if (i + 1 >= capacity) {
			return -1;
		}
		text[i++] = (char)cp;
	} else if (cp < 0x800) {
		if (i + 2 >= capacity) {
			return -1;
		}
		text[i++] = (char)(0xc0 | (cp >> 6));
		text[i++] = (char)(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		if (i + 3 >= capacity) {
			return -1;
		}
		text[i++] = (char)(0xe0 | (cp >> 12));
		text[i++] = (char)(0x80 | ((cp >> 6) & 0x3f));
		text[i++] = (char)(0x80 | (cp & 0x3f));
	} else {
		if (i + 4 >= capacity) {
			return -1;
		}
		text[i++] = (char)(0xf0 | (cp >> 18));
		text[i++] = (char)(0x80 | ((cp >> 12) & 0x3f));
		text[i++] = (char)(0x80 | ((cp >> 6) & 0x3f));
		text[i++] = (char)(0x80 | (cp & 0x3f));
	}
	text[i] = 0;
	*at = i;
	return 0;
}

/* ---- GSM 7-bit default alphabet (TS 23.038) -------------------------------------------------- */

/*
 * Septet to Unicode.  0xffff means the septet has no assigned character (0x1b is
 * handled as the escape and never looked up here).
 */
static const uint16_t vayu_sms_gsm7_unicode[128] = {
	0x0040, 0x00a3, 0x0024, 0x00a5, 0x00e8, 0x00e9, 0x00f9, 0x00ec,
	0x00f2, 0x00c7, 0x000a, 0x00d8, 0x00f8, 0x000d, 0x00c5, 0x00e5,
	0x0394, 0x005f, 0x03a6, 0x0393, 0x039b, 0x03a9, 0x03a0, 0x03a8,
	0x03a3, 0x0398, 0x039e, 0xffff, 0x00c6, 0x00e6, 0x00df, 0x00c9,
	0x0020, 0x0021, 0x0022, 0x0023, 0x00a4, 0x0025, 0x0026, 0x0027,
	0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
	0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
	0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
	0x00a1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
	0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f,
	0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
	0x0058, 0x0059, 0x005a, 0x00c4, 0x00d6, 0x00d1, 0x00dc, 0x00a7,
	0x00bf, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
	0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f,
	0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
	0x0078, 0x0079, 0x007a, 0x00e4, 0x00f6, 0x00f1, 0x00fc, 0x00e0,
};

/* The extension table after 0x1b: code to Unicode, 0xffff unassigned. */
static __inline__ uint16_t
vayu_sms_gsm7_extension_unicode(uint8_t code)
{
	switch (code) {
	case 0x0a: return 0x000c;       /* form feed */
	case 0x14: return 0x005e;       /* ^ */
	case 0x28: return 0x007b;       /* { */
	case 0x29: return 0x007d;       /* } */
	case 0x2f: return 0x005c;       /* backslash */
	case 0x3c: return 0x005b;       /* [ */
	case 0x3d: return 0x007e;       /* ~ */
	case 0x3e: return 0x005d;       /* ] */
	case 0x40: return 0x007c;       /* | */
	case 0x65: return 0x20ac;       /* euro */
	default: return 0xffff;
	}
}

/* Unicode to a GSM code: 0x000-0x07f basic septet, 0x100|code escaped, 0xffff none. */
static __inline__ uint16_t
vayu_sms_unicode_to_gsm7(uint32_t cp)
{
	for (uint16_t i = 0; i < 128; i++) {
		if (i != 0x1b && vayu_sms_gsm7_unicode[i] == cp) {
			return i;
		}
	}
	switch (cp) {
	case 0x000c: return 0x100u | 0x0a;
	case 0x005e: return 0x100u | 0x14;
	case 0x007b: return 0x100u | 0x28;
	case 0x007d: return 0x100u | 0x29;
	case 0x005c: return 0x100u | 0x2f;
	case 0x005b: return 0x100u | 0x3c;
	case 0x007e: return 0x100u | 0x3d;
	case 0x005d: return 0x100u | 0x3e;
	case 0x007c: return 0x100u | 0x40;
	case 0x20ac: return 0x100u | 0x65;
	default: return 0xffff;
	}
}

/* Pack septets, least significant bit first. */
static __inline__ int
vayu_sms_gsm7_pack(const uint8_t *septets, size_t count, uint8_t *out, size_t out_size, size_t *out_length)
{
	size_t octets;

	if (septets == NULL || out == NULL || out_length == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	if (count > VAYU_SMS_SEPTETS_MAX) {
		return VAYU_SMS_ERR_TOO_LONG;
	}
	octets = (count * 7u + 7u) / 8u;
	if (out_size < octets) {
		return VAYU_SMS_ERR_BUFFER;
	}
	memset(out, 0, octets);
	for (size_t i = 0; i < count; i++) {
		size_t bit = i * 7u;

		if (septets[i] > 0x7fu) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		out[bit / 8u] |= (uint8_t)(septets[i] << (bit % 8u));
		if ((bit % 8u) > 1u) {
			out[bit / 8u + 1u] |= (uint8_t)(septets[i] >> (8u - (bit % 8u)));
		}
	}
	*out_length = octets;
	return VAYU_SMS_OK;
}

/* Unpack septets starting at an arbitrary bit offset (a UDH may shift the text). */
static __inline__ int
vayu_sms_gsm7_unpack_at(const uint8_t *data, size_t data_length, size_t bit_offset, size_t septets,
    uint8_t *out, size_t out_size)
{
	size_t needed;

	if (data == NULL || out == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	if (septets > VAYU_SMS_SEPTETS_MAX) {
		return VAYU_SMS_ERR_TOO_LONG;
	}
	if (out_size < septets) {
		return VAYU_SMS_ERR_BUFFER;
	}
	needed = (bit_offset + septets * 7u + 7u) / 8u;
	if (data_length < needed) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	for (size_t i = 0; i < septets; i++) {
		size_t bit = bit_offset + i * 7u;
		uint16_t value = (uint16_t)(data[bit / 8u] >> (bit % 8u));

		if ((bit % 8u) > 1u) {
			value |= (uint16_t)(data[bit / 8u + 1u] << (8u - (bit % 8u)));
		}
		out[i] = (uint8_t)(value & 0x7f);
	}
	return VAYU_SMS_OK;
}

/* Septets (with 0x1b escapes) to UTF-8 text. */
static __inline__ int
vayu_sms_gsm7_to_utf8(const uint8_t *septets, size_t count, char *text, size_t text_size, size_t *text_length)
{
	size_t at = 0;

	if (septets == NULL || text == NULL || text_length == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	text[0] = 0;
	for (size_t i = 0; i < count; i++) {
		uint32_t cp = 0xffff;

		if (septets[i] == 0x1b && i + 1 < count) {
			cp = vayu_sms_gsm7_extension_unicode(septets[++i]);
			if (cp == 0xffff) {
				cp = 0x0020;            /* an unknown escape: one space, never a guess */
			}
		} else if (septets[i] < 128) {
			cp = vayu_sms_gsm7_unicode[septets[i]];
			if (cp == 0xffff) {
				cp = 0x0020;
			}
		}
		if (vayu_sms_utf8_put(text, text_size, &at, cp) < 0) {
			return VAYU_SMS_ERR_BUFFER;
		}
	}
	*text_length = at;
	return VAYU_SMS_OK;
}

/* UTF-8 text to septets; -VAYU_SMS_ERR_UNSUPPORTED if a character has no GSM 7-bit form. */
static __inline__ int
vayu_sms_utf8_to_gsm7(const char *text, size_t text_length, uint8_t *septets, size_t septets_size,
    size_t *septet_count)
{
	size_t at = 0, out = 0;

	if (text == NULL || septets == NULL || septet_count == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	while (at < text_length) {
		uint32_t cp;
		uint16_t code;

		if (vayu_sms_utf8_next(text, text_length, &at, &cp) < 0) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		code = vayu_sms_unicode_to_gsm7(cp);
		if (code == 0xffff) {
			return VAYU_SMS_ERR_UNSUPPORTED;
		}
		if (code & 0x100u) {
			if (out + 2 > septets_size) {
				return VAYU_SMS_ERR_TOO_LONG;
			}
			septets[out++] = 0x1b;
			septets[out++] = (uint8_t)(code & 0x7f);
		} else {
			if (out + 1 > septets_size) {
				return VAYU_SMS_ERR_TOO_LONG;
			}
			septets[out++] = (uint8_t)code;
		}
	}
	*septet_count = out;
	return VAYU_SMS_OK;
}

/* ---- UCS-2 ----------------------------------------------------------------------------------- */

/* UTF-8 to UTF-16BE octets. */
static __inline__ int
vayu_sms_utf8_to_ucs2(const char *text, size_t text_length, uint8_t *out, size_t out_size, size_t *out_length)
{
	size_t at = 0, octets = 0;

	if (text == NULL || out == NULL || out_length == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	while (at < text_length) {
		uint32_t cp;

		if (vayu_sms_utf8_next(text, text_length, &at, &cp) < 0) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		if (cp < 0x10000) {
			if (octets + 2 > out_size) {
				return VAYU_SMS_ERR_TOO_LONG;
			}
			out[octets++] = (uint8_t)(cp >> 8);
			out[octets++] = (uint8_t)cp;
		} else {
			uint32_t v = cp - 0x10000;

			if (octets + 4 > out_size) {
				return VAYU_SMS_ERR_TOO_LONG;
			}
			out[octets++] = (uint8_t)(0xd8 | (v >> 18));
			out[octets++] = (uint8_t)((v >> 10) & 0xff);
			out[octets++] = (uint8_t)(0xdc | ((v >> 8) & 0x03));
			out[octets++] = (uint8_t)(v & 0xff);
		}
	}
	*out_length = octets;
	return VAYU_SMS_OK;
}

/* UTF-16BE octets (with surrogate pairs) to UTF-8. */
static __inline__ int
vayu_sms_ucs2_to_utf8(const uint8_t *data, size_t length, char *text, size_t text_size, size_t *text_length)
{
	size_t at = 0, out = 0;

	if (data == NULL || text == NULL || text_length == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	if ((length & 1u) != 0) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	text[0] = 0;
	while (at < length) {
		uint32_t cp = (uint32_t)data[at] << 8 | data[at + 1];

		at += 2;
		if (cp >= 0xd800 && cp <= 0xdbff) {
			uint32_t low;

			if (at + 2 > length) {
				return VAYU_SMS_ERR_TRUNCATED;
			}
			low = (uint32_t)data[at] << 8 | data[at + 1];
			if (low < 0xdc00 || low > 0xdfff) {
				return VAYU_SMS_ERR_MALFORMED;
			}
			at += 2;
			cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
		} else if (cp >= 0xdc00 && cp <= 0xdfff) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		if (vayu_sms_utf8_put(text, text_size, &out, cp) < 0) {
			return VAYU_SMS_ERR_BUFFER;
		}
	}
	*text_length = out;
	return VAYU_SMS_OK;
}

/* ---- SMS-DELIVER ----------------------------------------------------------------------------- */

struct vayu_sms_address {
	uint32_t length;                /* bytes in text */
	uint8_t type_of_address;        /* TP-OA's type-of-address octet */
	bool alphanumeric;
	char text[VAYU_SMS_ADDRESS_MAX];
};

struct vayu_sms_deliver {
	struct vayu_sms_address originator;
	uint8_t first_octet;
	uint8_t pid;
	uint8_t dcs;
	uint8_t alphabet;               /* VAYU_SMS_ALPHABET_* */
	uint8_t scts[7];
	char timestamp[VAYU_SMS_TIMESTAMP_MAX];
	uint8_t user_data_header_length;
	bool concatenated;
	uint8_t concat_reference;
	uint8_t concat_total;
	uint8_t concat_sequence;
	uint32_t text_length;
	char text[VAYU_SMS_TEXT_MAX];
};

/* A semi-octet pair: the low nibble is the first (tens) digit, the high one the units. */
static __inline__ int
vayu_sms_bcd_value(uint8_t octet)
{
	return (octet & 0x0f) * 10 + (octet >> 4);
}

/* "2026-11-02 06:30:00 +0300"; the TP-SCTS timezone is in quarter hours. */
static __inline__ void
vayu_sms_format_scts(const uint8_t raw[7], char out[VAYU_SMS_TIMESTAMP_MAX])
{
	static const char digits[] = "0123456789";
	int year, month, day, hour, minute, second, quarters, sign;
	char local[6];

	year = vayu_sms_bcd_value(raw[0]);
	month = vayu_sms_bcd_value(raw[1]);
	day = vayu_sms_bcd_value(raw[2]);
	hour = vayu_sms_bcd_value(raw[3]);
	minute = vayu_sms_bcd_value(raw[4]);
	second = vayu_sms_bcd_value(raw[5]);
	sign = (raw[6] & 0x08) != 0;
	quarters = (raw[6] & 0x07) * 10 + (raw[6] >> 4);
	if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59 || quarters > 79) {
		out[0] = 0;
		return;
	}
	/* 20xx, two digits of the year as the PDU carries them */
	local[0] = '2';
	local[1] = '0';
	local[2] = digits[(year / 10) % 10];
	local[3] = digits[year % 10];
	local[4] = 0;
	out[0] = local[0]; out[1] = local[1]; out[2] = local[2]; out[3] = local[3];
	out[4] = '-';
	out[5] = digits[month / 10]; out[6] = digits[month % 10];
	out[7] = '-';
	out[8] = digits[day / 10]; out[9] = digits[day % 10];
	out[10] = ' ';
	out[11] = digits[hour / 10]; out[12] = digits[hour % 10];
	out[13] = ':';
	out[14] = digits[minute / 10]; out[15] = digits[minute % 10];
	out[16] = ':';
	out[17] = digits[second / 10]; out[18] = digits[second % 10];
	out[19] = ' ';
	out[20] = sign ? '-' : '+';
	out[21] = digits[(quarters * 15 / 60) / 10];
	out[22] = digits[(quarters * 15 / 60) % 10];
	out[23] = digits[(quarters * 15 % 60) / 10];
	out[24] = digits[(quarters * 15 % 60) % 10];
	out[25] = 0;
}

static __inline__ int
vayu_sms_parse_address(const uint8_t *pdu, size_t length, size_t *at, struct vayu_sms_address *address)
{
	size_t octets;
	uint8_t address_length, type_of_address;

	if (pdu == NULL || at == NULL || address == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	if (*at + 2 > length) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	address_length = pdu[(*at)++];
	type_of_address = pdu[(*at)++];
	address->type_of_address = type_of_address;
	address->alphanumeric = false;
	if ((type_of_address & 0x70) == 0x50) {
		uint8_t septets[VAYU_SMS_ADDRESS_MAX];
		size_t text_length;

		octets = ((size_t)address_length * 7u + 7u) / 8u;
		if (*at + octets > length) {
			return VAYU_SMS_ERR_TRUNCATED;
		}
		if (address_length > VAYU_SMS_ADDRESS_MAX - 1) {
			return VAYU_SMS_ERR_TOO_LONG;
		}
		if (vayu_sms_gsm7_unpack_at(pdu + *at, octets, 0, address_length, septets, sizeof(septets)) != VAYU_SMS_OK) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		if (vayu_sms_gsm7_to_utf8(septets, address_length, address->text, sizeof(address->text), &text_length) != VAYU_SMS_OK) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		address->length = (uint32_t)text_length;
		address->alphanumeric = true;
		*at += octets;
		return VAYU_SMS_OK;
	}
	octets = ((size_t)address_length + 1u) / 2u;
	if (*at + octets > length) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	if (address_length > VAYU_SMS_ADDRESS_MAX - 2) {
		return VAYU_SMS_ERR_TOO_LONG;
	}
	{
		size_t out = 0;

		if ((type_of_address & 0x70) == 0x10) {
			address->text[out++] = '+';
		}
		for (size_t i = 0; i < address_length; i++) {
			uint8_t nibble = (i & 1u) != 0 ? (uint8_t)(pdu[*at + i / 2u] >> 4) : (uint8_t)(pdu[*at + i / 2u] & 0x0f);

			if (nibble == 0x0f) {
				continue;
			}
			if (nibble > 9) {
				return VAYU_SMS_ERR_MALFORMED;
			}
			address->text[out++] = (char)('0' + nibble);
		}
		address->text[out] = 0;
		address->length = (uint32_t)out;
	}
	*at += octets;
	return VAYU_SMS_OK;
}

static __inline__ uint8_t
vayu_sms_dcs_alphabet(uint8_t dcs)
{
	uint8_t group = dcs & 0xf0;
	uint8_t bits;

	if (group == 0x00 || group == 0x10 || group == 0x20 || group == 0x30) {
		bits = (uint8_t)((dcs >> 2) & 0x03);
	} else if (group == 0x40 || group == 0x50 || group == 0x60 || group == 0x70 || group == 0xe0) {
		return VAYU_SMS_ALPHABET_GSM7;          /* message-waiting groups: always GSM 7-bit */
	} else if (group == 0xf0) {
		bits = (uint8_t)((dcs >> 2) & 0x03);
	} else {
		return VAYU_SMS_ALPHABET_NONE;
	}
	switch (bits) {
	case 0: return VAYU_SMS_ALPHABET_GSM7;
	case 1: return VAYU_SMS_ALPHABET_8BIT;
	case 2: return VAYU_SMS_ALPHABET_UCS2;
	default: return VAYU_SMS_ALPHABET_NONE;
	}
}

/*
 * Decode one complete SMS-DELIVER PDU (SMSC prefix included).  out is zeroed first.
 * A UDH is parsed for concatenation metadata; the text of this one segment is
 * decoded.  DCS alphabets this stage does not know are VAYU_SMS_ERR_UNSUPPORTED.
 */
static __inline__ int
vayu_sms_pdu_decode_deliver(const uint8_t *pdu, size_t length, struct vayu_sms_deliver *out)
{
	size_t at, prefix, udh_octets = 0, ud_octets;
	uint8_t first, udl, alphabet;

	if (pdu == NULL || out == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	memset(out, 0, sizeof(*out));
	out->alphabet = VAYU_SMS_ALPHABET_NONE;
	if (length < 2) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	prefix = 1u + (size_t)pdu[0];
	if (prefix >= length) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	at = prefix;
	first = pdu[at++];
	out->first_octet = first;
	if ((first & 0x03u) != 0) {
		return VAYU_SMS_ERR_UNSUPPORTED;         /* not SMS-DELIVER: SMS-SUBMIT, STATUS-REPORT... */
	}
	if (vayu_sms_parse_address(pdu, length, &at, &out->originator) != VAYU_SMS_OK) {
		return VAYU_SMS_ERR_MALFORMED;
	}
	if (at + 2 > length) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	out->pid = pdu[at++];
	out->dcs = pdu[at++];
	alphabet = vayu_sms_dcs_alphabet(out->dcs);
	if (alphabet == VAYU_SMS_ALPHABET_NONE) {
		return VAYU_SMS_ERR_UNSUPPORTED;
	}
	out->alphabet = alphabet;
	if (at + 7 > length) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	memcpy(out->scts, pdu + at, 7);
	vayu_sms_format_scts(out->scts, out->timestamp);
	at += 7;
	if (at + 1 > length) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	udl = pdu[at++];
	if ((first & 0x40u) != 0) {                     /* TP-UDHI */
		if (at + 1 > length) {
			return VAYU_SMS_ERR_TRUNCATED;
		}
		out->user_data_header_length = pdu[at];
		udh_octets = (size_t)out->user_data_header_length + 1u;
		if (at + udh_octets > length) {
			return VAYU_SMS_ERR_TRUNCATED;
		}
		for (size_t i = 1; i + 1 < udh_octets;) {
			uint8_t iei = pdu[at + i];
			uint8_t ie_length = pdu[at + i + 1];

			if (i + 2u + ie_length > udh_octets) {
				return VAYU_SMS_ERR_MALFORMED;
			}
			if (iei == 0x00 && ie_length == 3) {
				out->concatenated = true;
				out->concat_reference = pdu[at + i + 2];
				out->concat_total = pdu[at + i + 3];
				out->concat_sequence = pdu[at + i + 4];
			} else if (iei == 0x08 && ie_length == 4) {
				out->concatenated = true;
				out->concat_reference = (uint8_t)(pdu[at + i + 3] & 0xff);
				out->concat_total = pdu[at + i + 4];
				out->concat_sequence = pdu[at + i + 5];
			}
			i += 2u + ie_length;
		}
		at += udh_octets;
	}
	if (alphabet == VAYU_SMS_ALPHABET_GSM7) {
		size_t septets = udl;
		size_t skip = 0, relative_bits = 0;

		/*
		 * The UDH occupies whole octets before the septets.  The text's first
		 * septet starts at the next septet boundary; relative_bits is the 0..6-bit
		 * shift of that boundary inside the first octet after the UDH.
		 */
		if (udh_octets != 0) {
			skip = (udh_octets * 8u + 6u) / 7u;
			relative_bits = skip * 7u - udh_octets * 8u;
			if (skip >= septets) {
				out->text_length = 0;
				return VAYU_SMS_OK;
			}
			septets -= skip;
		}
		ud_octets = (relative_bits + septets * 7u + 7u) / 8u;
		if (at + ud_octets > length) {
			return VAYU_SMS_ERR_TRUNCATED;
		}
		{
			uint8_t septet_data[VAYU_SMS_SEPTETS_MAX];
			size_t text_length;

			if (vayu_sms_gsm7_unpack_at(pdu + at, ud_octets, relative_bits, septets, septet_data, sizeof(septet_data)) != VAYU_SMS_OK) {
				return VAYU_SMS_ERR_MALFORMED;
			}
			if (vayu_sms_gsm7_to_utf8(septet_data, septets, out->text, sizeof(out->text), &text_length) != VAYU_SMS_OK) {
				return VAYU_SMS_ERR_MALFORMED;
			}
			out->text_length = (uint32_t)text_length;
		}
		return VAYU_SMS_OK;
	}
	/*
	 * 8-bit and UCS-2: UDL is the number of octets in TP-UD, the UDH included;
	 * at points after the UDH already, so this segment's text is the rest.
	 */
	if (udl > VAYU_SMS_USER_OCTETS_MAX || udl < udh_octets) {
		return VAYU_SMS_ERR_MALFORMED;
	}
	ud_octets = udl - udh_octets;
	if (at + ud_octets > length) {
		return VAYU_SMS_ERR_TRUNCATED;
	}
	if (alphabet == VAYU_SMS_ALPHABET_UCS2) {
		size_t text_length;

		if ((ud_octets & 1u) != 0) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		if (vayu_sms_ucs2_to_utf8(pdu + at, ud_octets, out->text, sizeof(out->text), &text_length) != VAYU_SMS_OK) {
			return VAYU_SMS_ERR_MALFORMED;
		}
		out->text_length = (uint32_t)text_length;
	} else {
		size_t copy = ud_octets;        /* at most 140 octets, the buffer is larger */

		memcpy(out->text, pdu + at, copy);
		out->text[copy] = 0;
		out->text_length = (uint32_t)copy;
	}
	return VAYU_SMS_OK;
}

/* ---- SMS-SUBMIT ------------------------------------------------------------------------------ */

/*
 * Build a complete SMS-SUBMIT PDU (with the SMSC prefix byte 0x00: "use the
 * modem's default SMSC").  The text is GSM 7-bit when every character has a form
 * there, UCS-2 otherwise.  More than one segment is VAYU_SMS_ERR_TOO_LONG; no
 * truncation and no hidden concatenation.
 */
static __inline__ int
vayu_sms_pdu_encode_submit(const char *number, const char *text, size_t text_length, uint8_t message_reference,
    uint8_t *out, size_t out_size, size_t *out_length, uint32_t *pdu_dcs)
{
	uint8_t digits[24];
	size_t digit_count = 0, at = 0, user_length = 0, septet_count = 0, address_octets;
	bool international = false, gsm7 = true;
	uint8_t user[VAYU_SMS_USER_OCTETS_MAX];
	uint8_t septets[VAYU_SMS_SEPTETS_MAX];

	if (number == NULL || text == NULL || out == NULL || out_length == NULL || pdu_dcs == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	if (text_length == 0) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	for (const char *c = number; *c != 0; c++) {
		if (*c == '+' && digit_count == 0) {
			international = true;
			continue;
		}
		if (*c < '0' || *c > '9' || digit_count >= 20) {
			return VAYU_SMS_ERR_ADDRESS;
		}
		digits[digit_count++] = (uint8_t)(*c - '0');
	}
	if (digit_count == 0) {
		return VAYU_SMS_ERR_ADDRESS;
	}
	/* a leading '+' and then no digits is not an address */
	if (international && digit_count == 0) {
		return VAYU_SMS_ERR_ADDRESS;
	}
	if (vayu_sms_utf8_to_gsm7(text, text_length, septets, sizeof(septets), &septet_count) != VAYU_SMS_OK) {
		gsm7 = false;
	}
	if (gsm7) {
		if (septet_count > VAYU_SMS_SEPTETS_MAX) {
			return VAYU_SMS_ERR_TOO_LONG;
		}
		if (vayu_sms_gsm7_pack(septets, septet_count, user, sizeof(user), &user_length) != VAYU_SMS_OK) {
			return VAYU_SMS_ERR_TOO_LONG;
		}
	} else {
		size_t octets;

		if (vayu_sms_utf8_to_ucs2(text, text_length, user, sizeof(user), &octets) != VAYU_SMS_OK) {
			return VAYU_SMS_ERR_TOO_LONG;
		}
		if (octets > VAYU_SMS_USER_OCTETS_MAX) {
			return VAYU_SMS_ERR_TOO_LONG;
		}
		user_length = octets;
	}
	address_octets = (digit_count + 1u) / 2u;
	if (out_size < 1u + 1u + 1u + 1u + 1u + address_octets + 1u + 1u + 1u + user_length) {
		return VAYU_SMS_ERR_BUFFER;
	}
	out[at++] = 0x00;                               /* SMSC: the modem's default */
	out[at++] = 0x01;                               /* SMS-SUBMIT, no VP, no UDH, no SR */
	out[at++] = message_reference;
	out[at++] = (uint8_t)digit_count;
	out[at++] = international ? 0x91 : 0x81;
	for (size_t i = 0; i < digit_count; i += 2) {
		uint8_t low = digits[i];
		uint8_t high = i + 1 < digit_count ? digits[i + 1] : 0x0f;

		out[at++] = (uint8_t)((high << 4) | low);
	}
	out[at++] = 0x00;                               /* TP-PID */
	if (gsm7) {
		out[at++] = 0x00;                       /* TP-DCS: GSM 7-bit */
		out[at++] = (uint8_t)septet_count;      /* TP-UDL in septets */
		memcpy(out + at, user, user_length);    /* user_length: packed octets */
		at += user_length;
		*pdu_dcs = 0x00;
	} else {
		out[at++] = 0x08;                       /* TP-DCS: UCS-2 */
		out[at++] = (uint8_t)user_length;       /* TP-UDL in octets */
		memcpy(out + at, user, user_length);
		at += user_length;
		*pdu_dcs = 0x08;
	}
	*out_length = at;
	return VAYU_SMS_OK;
}

/* ---- QMI WMS: common ------------------------------------------------------------------------- */

struct vayu_sms_qmi_result {
	uint16_t result;
	uint16_t error;
};

static const struct qmi_elem_info vayu_sms_qmi_result_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_result, result) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_result, error) },
	{ .data_type = QMI_EOTI },
};

static __inline__ bool
vayu_sms_qmi_succeeded(const struct vayu_sms_qmi_result *result)
{
	return result != NULL && result->result == QMI_RESULT_SUCCESS_V01 && result->error == QMI_ERR_NONE_V01;
}

/* ---- Set Event Report (0x0001) ---------------------------------------------------------------- */

struct vayu_sms_qmi_set_event_req {
	uint8_t report;                 /* TLV 0x10: report new MT messages */
};

static const struct qmi_elem_info vayu_sms_qmi_set_event_report_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_set_event_req, report) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_result_resp {
	struct vayu_sms_qmi_result result;      /* TLV 0x02 */
};

static const struct qmi_elem_info vayu_sms_qmi_result_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_result),
	  .array_type = NO_ARRAY, .tlv_type = 0x02, .offset = offsetof(struct vayu_sms_qmi_result_resp, result),
	  .ei_array = vayu_sms_qmi_result_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- Event Report indication (0x0001) --------------------------------------------------------- */

struct vayu_sms_qmi_mt_message {
	uint8_t storage;                /* QmiWmsStorageType */
	uint32_t index;                 /* memory index */
};

static const struct qmi_elem_info vayu_sms_qmi_mt_message_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_mt_message, storage) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_mt_message, index) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_transfer_route {
	uint8_t ack;                    /* QmiWmsAckIndicator */
	uint32_t transaction;
	uint8_t format;                 /* QmiWmsMessageFormat */
	uint32_t raw_length;            /* guint16 on the wire */
	uint8_t raw[VAYU_SMS_PDU_MAX];
};

static const struct qmi_elem_info vayu_sms_qmi_transfer_route_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_transfer_route, ack) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_transfer_route, transaction) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_transfer_route, format) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_transfer_route, raw_length) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_SMS_PDU_MAX, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE,
	  .offset = offsetof(struct vayu_sms_qmi_transfer_route, raw) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_event_report {
	struct vayu_sms_qmi_result result;      /* not in an indication; left zero */
	uint8_t mt_valid;                       /* TLV 0x10 */
	struct vayu_sms_qmi_mt_message mt;
	uint8_t route_valid;                    /* TLV 0x11 */
	struct vayu_sms_qmi_transfer_route route;
	uint8_t mode_valid;                     /* TLV 0x12 */
	uint8_t mode;
	uint8_t ims_valid;                      /* TLV 0x16 */
	uint8_t ims;
};

static const struct qmi_elem_info vayu_sms_qmi_event_report_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_event_report, mt_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_mt_message),
	  .array_type = NO_ARRAY, .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_event_report, mt),
	  .ei_array = vayu_sms_qmi_mt_message_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_sms_qmi_event_report, route_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_transfer_route),
	  .array_type = NO_ARRAY, .tlv_type = 0x11, .offset = offsetof(struct vayu_sms_qmi_event_report, route),
	  .ei_array = vayu_sms_qmi_transfer_route_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_sms_qmi_event_report, mode_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_sms_qmi_event_report, mode) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_sms_qmi_event_report, ims_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_sms_qmi_event_report, ims) },
	{ .data_type = QMI_EOTI },
};

/* ---- Raw Send (0x0020) ------------------------------------------------------------------------ */

struct vayu_sms_qmi_raw_message {
	uint8_t format;                 /* QmiWmsMessageFormat */
	uint32_t raw_length;            /* guint16 on the wire */
	uint8_t raw[VAYU_SMS_PDU_MAX];
};

static const struct qmi_elem_info vayu_sms_qmi_raw_message_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_raw_message, format) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_raw_message, raw_length) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_SMS_PDU_MAX, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE,
	  .offset = offsetof(struct vayu_sms_qmi_raw_message, raw) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_raw_send_req {
	struct vayu_sms_qmi_raw_message message;        /* TLV 0x01 */
};

static const struct qmi_elem_info vayu_sms_qmi_raw_send_req_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_raw_message),
	  .array_type = NO_ARRAY, .tlv_type = 0x01,
	  .offset = offsetof(struct vayu_sms_qmi_raw_send_req, message),
	  .ei_array = vayu_sms_qmi_raw_message_ei },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_gsm_cause {
	uint16_t rp_cause;
	uint8_t tp_cause;
};

static const struct qmi_elem_info vayu_sms_qmi_gsm_cause_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_gsm_cause, rp_cause) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_gsm_cause, tp_cause) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_raw_send_resp {
	struct vayu_sms_qmi_result result;      /* TLV 0x02 */
	uint8_t message_id_valid;               /* TLV 0x01 */
	uint16_t message_id;
	uint8_t cause_valid;                    /* TLV 0x12 */
	struct vayu_sms_qmi_gsm_cause cause;
};

static const struct qmi_elem_info vayu_sms_qmi_raw_send_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_result),
	  .array_type = NO_ARRAY, .tlv_type = 0x02, .offset = offsetof(struct vayu_sms_qmi_raw_send_resp, result),
	  .ei_array = vayu_sms_qmi_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_raw_send_resp, message_id_valid) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_raw_send_resp, message_id) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_sms_qmi_raw_send_resp, cause_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_gsm_cause),
	  .array_type = NO_ARRAY, .tlv_type = 0x12, .offset = offsetof(struct vayu_sms_qmi_raw_send_resp, cause),
	  .ei_array = vayu_sms_qmi_gsm_cause_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- Raw Read (0x0022) ------------------------------------------------------------------------ */

struct vayu_sms_qmi_memory_id {
	uint8_t storage;                /* QmiWmsStorageType */
	uint32_t index;
};

static const struct qmi_elem_info vayu_sms_qmi_memory_id_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_memory_id, storage) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_memory_id, index) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_raw_read_req {
	struct vayu_sms_qmi_memory_id id;       /* TLV 0x01 */
	uint8_t mode_valid;                     /* TLV 0x10 */
	uint8_t mode;
};

static const struct qmi_elem_info vayu_sms_qmi_raw_read_req_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_memory_id),
	  .array_type = NO_ARRAY, .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_raw_read_req, id),
	  .ei_array = vayu_sms_qmi_memory_id_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_raw_read_req, mode_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_raw_read_req, mode) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_raw_read_data {
	uint8_t tag;                    /* QmiWmsMessageTagType */
	uint8_t format;                 /* QmiWmsMessageFormat */
	uint32_t raw_length;            /* guint16 on the wire */
	uint8_t raw[VAYU_SMS_PDU_MAX];
};

static const struct qmi_elem_info vayu_sms_qmi_raw_read_data_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_raw_read_data, tag) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_raw_read_data, format) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_raw_read_data, raw_length) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_SMS_PDU_MAX, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE,
	  .offset = offsetof(struct vayu_sms_qmi_raw_read_data, raw) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_raw_read_resp {
	struct vayu_sms_qmi_result result;      /* TLV 0x02 */
	uint8_t raw_valid;                      /* TLV 0x01 */
	struct vayu_sms_qmi_raw_read_data raw;
};

static const struct qmi_elem_info vayu_sms_qmi_raw_read_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_result),
	  .array_type = NO_ARRAY, .tlv_type = 0x02, .offset = offsetof(struct vayu_sms_qmi_raw_read_resp, result),
	  .ei_array = vayu_sms_qmi_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_raw_read_resp, raw_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_raw_read_data),
	  .array_type = NO_ARRAY, .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_raw_read_resp, raw),
	  .ei_array = vayu_sms_qmi_raw_read_data_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- List Messages (0x0031) ------------------------------------------------------------------ */

struct vayu_sms_qmi_list_req {
	uint8_t storage;                /* TLV 0x01; a value, not a wildcard */
};

static const struct qmi_elem_info vayu_sms_qmi_list_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_list_req, storage) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_list_entry {
	uint32_t index;
	uint8_t tag;
};

struct vayu_sms_qmi_message_list {
	uint32_t count;
	struct vayu_sms_qmi_list_entry entry[VAYU_SMS_LIST_MAX];
};

#define VAYU_SMS_QMI_LIST_RAW   512     /* a 32-entry list is 164 bytes; room to spare */

/*
 * List Messages is the one WMS reply whose count prefix is 32 bits (guint32 in
 * qmi-service-wms.json), while sys/hakios.qmi.h's QMI_DATA_LEN supports only 8- and
 * 16-bit prefixes.  A top-level QMI_STRING element copies a whole TLV value, so the
 * 0x01 TLV is taken as 512 opaque bytes and parsed by vayu_sms_qmi_list_parse().
 */
struct vayu_sms_qmi_list_resp {
	struct vayu_sms_qmi_result result;      /* TLV 0x02 */
	uint8_t list_valid;                     /* TLV 0x01 */
	char list[VAYU_SMS_QMI_LIST_RAW];
};

static const struct qmi_elem_info vayu_sms_qmi_list_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_result),
	  .array_type = NO_ARRAY, .tlv_type = 0x02, .offset = offsetof(struct vayu_sms_qmi_list_resp, result),
	  .ei_array = vayu_sms_qmi_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_list_resp, list_valid) },
	{ .data_type = QMI_STRING, .elem_len = VAYU_SMS_QMI_LIST_RAW, .elem_size = sizeof(char),
	  .array_type = NO_ARRAY, .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_list_resp, list) },
	{ .data_type = QMI_EOTI },
};

static __inline__ uint16_t
vayu_sms_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static __inline__ uint32_t
vayu_sms_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The 0x01 TLV body: u32 count, then {u32 index, u8 tag}. */
static __inline__ int
vayu_sms_qmi_list_parse(const char *payload, size_t payload_length, struct vayu_sms_qmi_message_list *out)
{
	const uint8_t *data = (const uint8_t *)payload;
	uint32_t count;

	if (payload == NULL || out == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	memset(out, 0, sizeof(*out));
	if (payload_length < 4) {
		return VAYU_SMS_ERR_MALFORMED;
	}
	count = vayu_sms_le32(data);
	if (count > (payload_length - 4u) / 5u) {
		return VAYU_SMS_ERR_MALFORMED;
	}
	if (count > VAYU_SMS_LIST_MAX) {
		count = VAYU_SMS_LIST_MAX;              /* more than the inbox can show */
	}
	out->count = count;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *entry = data + 4u + (size_t)i * 5u;

		out->entry[i].index = vayu_sms_le32(entry);
		out->entry[i].tag = entry[4];
	}
	return VAYU_SMS_OK;
}

/* ---- Delete (0x0024) ------------------------------------------------------------------------- */

struct vayu_sms_qmi_delete_req {
	uint8_t storage;                /* TLV 0x01 */
	uint32_t index;                 /* TLV 0x10 */
	uint8_t tag;                    /* TLV 0x11 */
};

static const struct qmi_elem_info vayu_sms_qmi_delete_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_delete_req, storage) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_delete_req, index) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_sms_qmi_delete_req, tag) },
	{ .data_type = QMI_EOTI },
};

/* ---- Send Ack (0x0037) ----------------------------------------------------------------------- */

struct vayu_sms_qmi_send_ack_info {
	uint32_t transaction;
	uint8_t protocol;               /* QmiWmsMessageProtocol: WCDMA = 1 */
	uint8_t success;
};

static const struct qmi_elem_info vayu_sms_qmi_send_ack_info_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_send_ack_info, transaction) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_send_ack_info, protocol) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_send_ack_info, success) },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_send_ack_req {
	struct vayu_sms_qmi_send_ack_info info; /* TLV 0x01 */
};

static const struct qmi_elem_info vayu_sms_qmi_send_ack_req_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_send_ack_info),
	  .array_type = NO_ARRAY, .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_send_ack_req, info),
	  .ei_array = vayu_sms_qmi_send_ack_info_ei },
	{ .data_type = QMI_EOTI },
};

/* ---- Set/Get Routes (0x0032/0x0033) ---------------------------------------------------------- */

struct vayu_sms_qmi_route {
	uint8_t message_type;
	uint8_t message_class;
	uint8_t storage;
	uint8_t receipt_action;
};

struct vayu_sms_qmi_routes {
	uint32_t count;                 /* guint16 on the wire for routes */
	struct vayu_sms_qmi_route route[VAYU_SMS_ROUTE_MAX];
};

static const struct qmi_elem_info vayu_sms_qmi_route_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_route, message_type) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_route, message_class) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_route, storage) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_route, receipt_action) },
	{ .data_type = QMI_EOTI },
};

/* The route list's wire count is 16 bits, unlike List Messages' 32 (qmi-service-wms.json). */
static const struct qmi_elem_info vayu_sms_qmi_routes_ei[] = {
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct vayu_sms_qmi_routes, count) },
	{ .data_type = QMI_STRUCT, .elem_len = VAYU_SMS_ROUTE_MAX, .elem_size = sizeof(struct vayu_sms_qmi_route),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = QMI_COMMON_TLV_TYPE,
	  .offset = offsetof(struct vayu_sms_qmi_routes, route), .ei_array = vayu_sms_qmi_route_ei },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_set_routes_req {
	struct vayu_sms_qmi_routes routes;      /* TLV 0x01 */
};

static const struct qmi_elem_info vayu_sms_qmi_set_routes_req_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_routes),
	  .array_type = NO_ARRAY, .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_set_routes_req, routes),
	  .ei_array = vayu_sms_qmi_routes_ei },
	{ .data_type = QMI_EOTI },
};

struct vayu_sms_qmi_get_routes_resp {
	struct vayu_sms_qmi_result result;      /* TLV 0x02 */
	uint8_t routes_valid;                   /* TLV 0x01 */
	struct vayu_sms_qmi_routes routes;
	uint8_t transfer_valid;                 /* TLV 0x10 */
	uint8_t transfer;
};

static const struct qmi_elem_info vayu_sms_qmi_get_routes_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_result),
	  .array_type = NO_ARRAY, .tlv_type = 0x02, .offset = offsetof(struct vayu_sms_qmi_get_routes_resp, result),
	  .ei_array = vayu_sms_qmi_result_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_get_routes_resp, routes_valid) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_sms_qmi_routes),
	  .array_type = NO_ARRAY, .tlv_type = 0x01, .offset = offsetof(struct vayu_sms_qmi_get_routes_resp, routes),
	  .ei_array = vayu_sms_qmi_routes_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_get_routes_resp, transfer_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_sms_qmi_get_routes_resp, transfer) },
	{ .data_type = QMI_EOTI },
};

/*
 * Fill a Raw Send request from a complete PDU (the form vayu_sms_pdu_encode_submit()
 * produces).  No bytes leave the machine here.
 */
static __inline__ int
vayu_sms_qmi_raw_send_fill(struct vayu_sms_qmi_raw_send_req *request, const uint8_t *pdu, size_t pdu_length)
{
	if (request == NULL || pdu == NULL) {
		return VAYU_SMS_ERR_ARGUMENT;
	}
	if (pdu_length == 0 || pdu_length > VAYU_SMS_PDU_MAX) {
		return VAYU_SMS_ERR_TOO_LONG;
	}
	memset(request, 0, sizeof(*request));
	request->message.format = VAYU_SMS_QMI_FORMAT_GSM_P2P;
	request->message.raw_length = (uint32_t)pdu_length;
	memcpy(request->message.raw, pdu, pdu_length);
	return VAYU_SMS_OK;
}

#endif /* _SYS_VAYU_SMS_QMI_H_ */
