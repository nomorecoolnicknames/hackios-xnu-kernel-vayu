/*
 * Tests of the QMI LOC messages of bsd/dev/vayu_gnss.c (sys/vayu_gnss_qmi.h) on the machine that
 * builds the kernel, against the QMI coding as it is in the kernel (bsd/dev/hakios.qmi.c), under
 * the address and undefined-behaviour sanitizers. tools/gnss-test/run.sh compiles and runs it.
 *
 * What is here: the requests this client sends, byte for byte; the reading of a reply's result;
 * the binary position report (with a binary64 and with a binary32 coordinate, and with values that
 * are not coordinates); and NMEA 0183, whose sentences are written out here by hand -- GGA, RMC,
 * GSV, GSA, a bad checksum, and the same sentence inside a QMI indication.
 *
 * What is NOT tested here: the modem. Every byte below is this repository's reading of the QMI LOC
 * v02 IDL (sys/vayu_gnss_qmi.h names what is a fact and what is a guess); whether an X24 answers
 * like this is a HYPOTHESIS until vayu_gnss runs on the phone.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/hakios.qmi.h>
#include <sys/vayu_gnss.h>
#include <sys/vayu_gnss_qmi.h>

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
check_bytes(const uint8_t *got, int got_length, const uint8_t *want, size_t want_length, const char *what)
{
	bool same = got_length == (int)want_length && memcmp(got, want, want_length) == 0;

	check(same, "%s: the bytes are not the expected ones", what);
	if (!same) {
		printf("  got  (%d):", got_length);
		for (int i = 0; i < got_length && i < 96; i++) {
			printf(" %02x", got[i]);
		}
		printf("\n  want (%zu):", want_length);
		for (size_t i = 0; i < want_length && i < 96; i++) {
			printf(" %02x", want[i]);
		}
		printf("\n");
	}
}

/* ---- building a message and its TLVs, as the wire has them ----------------------------------- */

static size_t
put_tlv(uint8_t *at, uint8_t type, const uint8_t *value, uint16_t length)
{
	at[0] = type;
	at[1] = (uint8_t)(length & 0xff);
	at[2] = (uint8_t)(length >> 8);
	if (length != 0) {
		memcpy(at + 3, value, length);
	}
	return 3u + length;
}

/* A whole message: the seven-byte header and a body already made. */
static size_t
put_message(uint8_t *out, uint8_t type, uint16_t id, uint16_t transaction, const uint8_t *body, size_t body_length)
{
	out[0] = type;
	out[1] = (uint8_t)(transaction & 0xff);
	out[2] = (uint8_t)(transaction >> 8);
	out[3] = (uint8_t)(id & 0xff);
	out[4] = (uint8_t)(id >> 8);
	out[5] = (uint8_t)(body_length & 0xff);
	out[6] = (uint8_t)(body_length >> 8);
	memcpy(out + QMI_HEADER_SIZE, body, body_length);
	return QMI_HEADER_SIZE + body_length;
}

static void
put_u16(uint8_t *at, uint16_t value)
{
	at[0] = (uint8_t)(value & 0xff);
	at[1] = (uint8_t)(value >> 8);
}

static void
put_u32(uint8_t *at, uint32_t value)
{
	put_u16(at, (uint16_t)(value & 0xffff));
	put_u16(at + 2, (uint16_t)(value >> 16));
}

static void
put_u64(uint8_t *at, uint64_t value)
{
	put_u32(at, (uint32_t)(value & 0xffffffffu));
	put_u32(at + 4, (uint32_t)(value >> 32));
}

static void
put_double(uint8_t *at, double value)
{
	union { double value; uint64_t bits; } real;

	real.value = value;
	put_u64(at, real.bits);
}

static void
put_float(uint8_t *at, float value)
{
	union { float value; uint32_t bits; } real;

	real.value = value;
	put_u32(at, real.bits);
}

/* ---- the requests, byte for byte -------------------------------------------------------------- */

static void
test_reg_events(void)
{
	struct vayu_gnss_reg_events_req request = {
		.mask = VAYU_GNSS_EVENT_POSITION | VAYU_GNSS_EVENT_SV_INFO | VAYU_GNSS_EVENT_NMEA,
	};
	struct vayu_gnss_reg_events_req32 narrow = { .mask = VAYU_GNSS_EVENT_POSITION | VAYU_GNSS_EVENT_SV_INFO |
	    VAYU_GNSS_EVENT_NMEA };
	static const uint8_t want[] = {
		0x00, 0x00, 0x00, 0x21, 0x00, 0x0b, 0x00,       /* request, transaction 0, id 0x0021, 11 bytes */
		0x01, 0x08, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, /* TLV 0x01, 8 bytes: bits 0, 1 and 2 */
		0x00, 0x00, 0x00,
	};
	static const uint8_t want_narrow[] = {
		0x00, 0x05, 0x00, 0x21, 0x00, 0x07, 0x00,       /* the four-byte form: LOC v01's layout */
		0x01, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
	};
	uint8_t out[64];
	int length;

	length = hakios_qmi_encode(QMI_REQUEST, VAYU_GNSS_LOC_REG_EVENTS, 0, vayu_gnss_reg_events_req_ei, &request,
	    out, sizeof(out));
	check_bytes(out, length, want, sizeof(want), "Register Events");
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_GNSS_LOC_REG_EVENTS, 5, vayu_gnss_reg_events_req32_ei, &narrow,
	    out, sizeof(out));
	check_bytes(out, length, want_narrow, sizeof(want_narrow), "Register Events, four-byte fallback");
}

static void
test_start_forms(void)
{
	struct vayu_gnss_start_req full = {
		.session = 0, .intermediate = 0, .min_interval_ms = 1000, .fix_mode = 0,
	};
	struct vayu_gnss_start_core_req core = { .session = 0, .min_interval_ms = 1000 };
	struct vayu_gnss_start_min_req minimal = { .session = 0 };
	struct vayu_gnss_stop_req stop = { .session = 0 };
	static const uint8_t want_full[] = {
		0x00, 0x01, 0x00, 0x22, 0x00, 0x16, 0x00,       /* transaction 1, id 0x0022, 22 bytes */
		0x01, 0x01, 0x00, 0x00,                         /* TLV 0x01: session 0 */
		0x03, 0x01, 0x00, 0x00,                         /* TLV 0x03: intermediate report 0 */
		0x04, 0x04, 0x00, 0xe8, 0x03, 0x00, 0x00,       /* TLV 0x04: 1000 ms */
		0x06, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,       /* TLV 0x06: fix mode 0, the default */
	};
	static const uint8_t want_core[] = {
		0x00, 0x02, 0x00, 0x22, 0x00, 0x0b, 0x00,       /* transaction 2, 11 bytes */
		0x01, 0x01, 0x00, 0x00,
		0x04, 0x04, 0x00, 0xe8, 0x03, 0x00, 0x00,
	};
	static const uint8_t want_min[] = {
		0x00, 0x03, 0x00, 0x22, 0x00, 0x04, 0x00,       /* transaction 3, 4 bytes */
		0x01, 0x01, 0x00, 0x00,
	};
	static const uint8_t want_stop[] = {
		0x00, 0x04, 0x00, 0x23, 0x00, 0x04, 0x00,       /* transaction 4, id 0x0023 */
		0x01, 0x01, 0x00, 0x00,
	};
	uint8_t out[64];
	int length;

	length = hakios_qmi_encode(QMI_REQUEST, VAYU_GNSS_LOC_START, 1, vayu_gnss_start_req_ei, &full, out, sizeof(out));
	check_bytes(out, length, want_full, sizeof(want_full), "Start, full form");
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_GNSS_LOC_START, 2, vayu_gnss_start_core_req_ei, &core, out,
	    sizeof(out));
	check_bytes(out, length, want_core, sizeof(want_core), "Start, session and interval");
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_GNSS_LOC_START, 3, vayu_gnss_start_min_req_ei, &minimal, out,
	    sizeof(out));
	check_bytes(out, length, want_min, sizeof(want_min), "Start, session alone");
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_GNSS_LOC_STOP, 4, vayu_gnss_stop_req_ei, &stop, out, sizeof(out));
	check_bytes(out, length, want_stop, sizeof(want_stop), "Stop");
}

static void
test_reply_result(void)
{
	uint8_t message[32];
	uint8_t body[16];
	size_t body_length, length;
	uint16_t result = 0xffff, error = 0xffff;
	uint8_t mode = 0xff;

	/* a success: TLV 0x02, result 0 error 0, then the HEPE-ish TLV nobody reads */
	body_length = put_tlv(body, 0x02, (const uint8_t *)"\x00\x00\x00\x00", 4);
	body_length += put_tlv(body + body_length, 0x99, (const uint8_t *)"\x01\x02", 2);
	length = put_message(message, QMI_RESPONSE, VAYU_GNSS_LOC_START, 7, body, body_length);
	check(vayu_gnss_reply_result(message, length, &result, &error), "reply: no result TLV found");
	check(result == 0 && error == 0, "reply: result %u error %u, not 0/0", (unsigned int)result,
	    (unsigned int)error);

	/* a refusal: result 1, error 58 (ENCODING) */
	body_length = put_tlv(body, 0x02, (const uint8_t *)"\x01\x00\x3a\x00", 4);
	length = put_message(message, QMI_RESPONSE, VAYU_GNSS_LOC_START, 8, body, body_length);
	check(vayu_gnss_reply_result(message, length, &result, &error), "reply: the refusal's TLV is not read");
	check(result == 1 && error == QMI_ERR_ENCODING_V01, "reply: result %u error %u, not 1/58",
	    (unsigned int)result, (unsigned int)error);

	/* DMS Get Operating Mode: TLV 0x02 then TLV 0x01, mode 0 (online) */
	body_length = put_tlv(body, 0x02, (const uint8_t *)"\x00\x00\x00\x00", 4);
	body_length += put_tlv(body + body_length, 0x01, (const uint8_t *)"\x00", 1);
	length = put_message(message, QMI_RESPONSE, VAYU_GNSS_DMS_GET_OPERATING_MODE, 9, body, body_length);
	check(vayu_gnss_reply_mode(message, length, &mode), "DMS mode: TLV 0x01 is not read");
	check(mode == 0, "DMS mode: %u, not 0", (unsigned int)mode);

	/* a message whose header says more than the datagram has */
	length = put_message(message, QMI_RESPONSE, VAYU_GNSS_LOC_START, 10, body, body_length);
	check(!vayu_gnss_reply_result(message, length - 1, &result, &error),
	    "a short datagram was read as a reply");
}

/* ---- the position report --------------------------------------------------------------------- */

/* One report: TLV 0x01 session, 0x02 timestamp, 0x06 latitude, 0x07 longitude, 0x08 HEPE, 0x09 altitude. */
static size_t
position_report(uint8_t *message, bool as_float, double latitude, double longitude, double hepe, double altitude)
{
	uint8_t body[128], value[8];
	size_t body_length = 0;

	body_length += put_tlv(body + body_length, VAYU_GNSS_POS_SESSION, (const uint8_t *)"\x00", 1);
	body_length += put_tlv(body + body_length, 0x02, (const uint8_t *)"\x30\x39\x00\x00", 4);
	if (as_float) {
		put_float(value, (float)latitude);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_LATITUDE, value, 4);
		put_float(value, (float)longitude);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_LONGITUDE, value, 4);
		put_float(value, (float)hepe);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_HEPE, value, 4);
		put_float(value, (float)altitude);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_ALTITUDE, value, 4);
	} else {
		put_double(value, latitude);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_LATITUDE, value, 8);
		put_double(value, longitude);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_LONGITUDE, value, 8);
		put_double(value, hepe);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_HEPE, value, 8);
		put_double(value, altitude);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_ALTITUDE, value, 8);
	}
	return put_message(message, QMI_INDICATION, VAYU_GNSS_LOC_EVENT_POSITION_REPORT, 0, body, body_length);
}

static void
test_position_report(void)
{
	struct vayu_gnss_fix fix;
	uint8_t message[160];
	size_t length;

	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	length = position_report(message, false, 55.7520566, 37.6187233, 8.5, 156.75);
	check(vayu_gnss_parse_position_report(message, length, &fix), "position report (double): not read");
	check(fix.position, "position report (double): no position");
	check(fix.lat_e7 == 557520566, "position report (double): latitude %d, not 557520566", (int)fix.lat_e7);
	check(fix.lon_e7 == 376187233, "position report (double): longitude %d, not 376187233", (int)fix.lon_e7);
	check(fix.has_hepe && fix.hepe_cm == 850, "position report (double): HEPE %u cm, not 850",
	    (unsigned int)fix.hepe_cm);
	check(fix.has_altitude && fix.altitude_mm == 156750, "position report (double): altitude %d mm, not 156750",
	    (int)fix.altitude_mm);
	check(fix.source == VAYU_GNSS_SOURCE_QMI, "position report: source %u, not QMI", (unsigned int)fix.source);

	/* the same report with 4-byte reals, in case the IDL's field is a binary32 */
	memset(&fix, 0, sizeof(fix));
	length = position_report(message, true, 55.75206, 37.61872, 8.5, 156.75);
	check(vayu_gnss_parse_position_report(message, length, &fix), "position report (float): not read");
	check(fix.lat_e7 > 557520500 && fix.lat_e7 < 557520650, "position report (float): latitude %d is far from "
	    "55.75206", (int)fix.lat_e7);
	check(fix.lon_e7 > 376187100 && fix.lon_e7 < 376187300, "position report (float): longitude %d is far from "
	    "37.61872", (int)fix.lon_e7);

	/* a latitude that is not one: refused, not shown as a position */
	memset(&fix, 0, sizeof(fix));
	length = position_report(message, false, 555.75, 37.61, 8.0, 150.0);
	check(!vayu_gnss_parse_position_report(message, length, &fix), "latitude 555.75 was accepted");

	/* another session's report: refused */
	memset(&fix, 0, sizeof(fix));
	length = position_report(message, false, 55.75, 37.61, 8.0, 150.0);
	message[QMI_HEADER_SIZE + 3] = 2;               /* the session-id TLV's value */
	check(!vayu_gnss_parse_position_report(message, length, &fix), "another session's report was accepted");

	/* no longitude at all: refused */
	memset(&fix, 0, sizeof(fix));
	{
		uint8_t body[64], value[8];
		size_t body_length = 0;

		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_SESSION, (const uint8_t *)"\x00", 1);
		put_double(value, 55.75);
		body_length += put_tlv(body + body_length, VAYU_GNSS_POS_LATITUDE, value, 8);
		length = put_message(message, QMI_INDICATION, VAYU_GNSS_LOC_EVENT_POSITION_REPORT, 0, body, body_length);
	}
	check(!vayu_gnss_parse_position_report(message, length, &fix), "a report with no longitude was accepted");
}

/* ---- NMEA 0183 -------------------------------------------------------------------------------- */

/* A sentence with its checksum, from the body between '$' and '*' ("GPGGA,..."). */
static size_t
nmea(char *out, size_t size, const char *body)
{
	uint8_t checksum = 0;
	size_t length = 0;

	for (const char *c = body; *c != 0; c++) {
		checksum ^= (uint8_t)*c;
	}
	length = (size_t)snprintf(out, size, "$%s*%02X\r\n", body, checksum);
	return length;
}

static void
test_nmea_gga(void)
{
	struct vayu_gnss_fix fix;
	char sentence[160];
	size_t length;
	uint32_t sentences = 0, bad = 0;

	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	length = nmea(sentence, sizeof(sentence),
	    "GPGGA,123519.00,5545.1234,N,03737.1234,E,1,09,1.2,156.4,M,14.3,M,,");
	check(vayu_gnss_parse_nmea((const uint8_t *)sentence, length, &fix, &sentences, &bad),
	    "GGA: no position");
	check(sentences == 1 && bad == 0, "GGA: %u sentence(s), %u bad, not 1/0", (unsigned int)sentences,
	    (unsigned int)bad);
	check(fix.lat_e7 == 557520566, "GGA: latitude %d, not 557520566", (int)fix.lat_e7);
	check(fix.lon_e7 == 376187233, "GGA: longitude %d, not 376187233", (int)fix.lon_e7);
	check(fix.has_sats_used && fix.sats_used == 9, "GGA: satellites used %u, not 9",
	    (unsigned int)fix.sats_used);
	check(fix.has_hdop && fix.hdop_centi == 120, "GGA: HDOP %u, not 120", (unsigned int)fix.hdop_centi);
	check(fix.has_altitude && fix.altitude_mm == 156400, "GGA: altitude %d mm, not 156400", (int)fix.altitude_mm);
	check(fix.has_tod && fix.tod_ms == 45319000u, "GGA: time of day %u ms, not 45319000",
	    (unsigned int)fix.tod_ms);
	check(fix.quality == 1, "GGA: quality %u, not 1", (unsigned int)fix.quality);

	/* a GGA with no fix (quality 0): no position, but the fields it has are still read */
	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	sentences = bad = 0;
	length = nmea(sentence, sizeof(sentence), "GPGGA,123519.00,,,,,0,00,99.9,,,,,,");
	check(!vayu_gnss_parse_nmea((const uint8_t *)sentence, length, &fix, &sentences, &bad),
	    "GGA with quality 0: a position came out of it");
	check(sentences == 1 && bad == 0, "GGA with quality 0: %u/%u, not 1/0", (unsigned int)sentences,
	    (unsigned int)bad);
	check(!fix.position, "GGA with quality 0: position is set");

	/* a bad checksum: refused, counted */
	memset(&fix, 0, sizeof(fix));
	sentences = bad = 0;
	length = nmea(sentence, sizeof(sentence), "GPGGA,123519.00,5545.1234,N,03737.1234,E,1,09,1.2,156.4,M,14.3,M,,");
	sentence[length - 3] = sentence[length - 3] == '0' ? '1' : '0';        /* the checksum's first digit */
	check(!vayu_gnss_parse_nmea((const uint8_t *)sentence, length, &fix, &sentences, &bad),
	    "a sentence with a wrong checksum gave a position");
	check(sentences == 0 && bad == 1, "bad checksum: %u read, %u refused, not 0/1", (unsigned int)sentences,
	    (unsigned int)bad);
}

static void
test_nmea_rmc_gsv_gsa(void)
{
	struct vayu_gnss_fix fix;
	char sentence[160];
	size_t length;
	uint32_t sentences = 0, bad = 0;

	/* RMC: a valid fix, 12.3 knots, course 84.4 degrees */
	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	length = nmea(sentence, sizeof(sentence), "GPRMC,123519.00,A,5545.1234,N,03737.1234,E,12.3,84.4,230394,,");
	check(vayu_gnss_parse_nmea((const uint8_t *)sentence, length, &fix, &sentences, &bad), "RMC: no position");
	check(fix.lat_e7 == 557520566 && fix.lon_e7 == 376187233, "RMC: %d/%d, not 557520566/376187233",
	    (int)fix.lat_e7, (int)fix.lon_e7);
	check(fix.has_speed && fix.speed_mm_s > 6300 && fix.speed_mm_s < 6400, "RMC: speed %u mm/s, not about 6328",
	    (unsigned int)fix.speed_mm_s);
	check(fix.has_heading && fix.heading_mdeg == 84400, "RMC: heading %u mdeg, not 84400",
	    (unsigned int)fix.heading_mdeg);

	/* RMC with the receiver's warning (V): no position, the rest still read */
	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	sentences = bad = 0;
	length = nmea(sentence, sizeof(sentence), "GPRMC,123519.00,V,,,,,,,230394,,");
	check(!vayu_gnss_parse_nmea((const uint8_t *)sentence, length, &fix, &sentences, &bad),
	    "RMC with V gave a position");

	/* GSV: 3 messages in the group, 14 satellites in view */
	memset(&fix, 0, sizeof(fix));
	sentences = bad = 0;
	length = nmea(sentence, sizeof(sentence), "GPGSV,3,1,14,01,40,083,46,02,17,308,41,12,07,344,39");
	(void)vayu_gnss_parse_nmea((const uint8_t *)sentence, length, &fix, &sentences, &bad);
	check(fix.has_sats_view && fix.sats_view == 14, "GSV: satellites in view %u, not 14",
	    (unsigned int)fix.sats_view);

	/* GSA: 4 satellites used, HDOP 1.3 */
	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	sentences = bad = 0;
	length = nmea(sentence, sizeof(sentence), "GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1");
	(void)vayu_gnss_parse_nmea((const uint8_t *)sentence, length, &fix, &sentences, &bad);
	check(fix.has_sats_used && fix.sats_used == 5, "GSA: satellites used %u, not 5", (unsigned int)fix.sats_used);
	check(fix.has_hdop && fix.hdop_centi == 130, "GSA: HDOP %u, not 130", (unsigned int)fix.hdop_centi);
}

static void
test_nmea_in_a_message(void)
{
	struct vayu_gnss_fix fix;
	char sentence[160];
	uint8_t message[256], body[192];
	size_t sentence_length, body_length, length;
	uint32_t sentences = 0, bad = 0;

	sentence_length = nmea(sentence, sizeof(sentence),
	    "GPGGA,123519.00,5545.1234,N,03737.1234,E,1,09,1.2,156.4,M,14.3,M,,");

	/* the sentences in a string TLV of the number this client does not know (0x10): it must still
	 * find them, because it looks for the '$' and not for the TLV's number */
	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	body_length = put_tlv(body, 0x10, (const uint8_t *)sentence, (uint16_t)sentence_length);
	length = put_message(message, QMI_INDICATION, VAYU_GNSS_LOC_EVENT_NMEA, 0, body, body_length);
	check(vayu_gnss_parse_nmea_message(message, length, &fix, &sentences, &bad), "NMEA in TLV 0x10: no position");
	check(fix.lat_e7 == 557520566 && fix.lon_e7 == 376187233, "NMEA in TLV 0x10: %d/%d",
	    (int)fix.lat_e7, (int)fix.lon_e7);

	/* ... and with no TLV at all: the body itself is the sentence */
	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	sentences = bad = 0;
	length = put_message(message, QMI_INDICATION, VAYU_GNSS_LOC_EVENT_NMEA, 0, (const uint8_t *)sentence,
	    sentence_length);
	check(vayu_gnss_parse_nmea_message(message, length, &fix, &sentences, &bad), "NMEA as the body: no position");

	/* two sentences back to back inside one string TLV, as a modem that packs them sends them */
	memset(&fix, 0, sizeof(fix));
	fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	sentences = bad = 0;
	memcpy(body + 3, sentence, sentence_length);
	memcpy(body + 3 + sentence_length, sentence, sentence_length);
	body_length = 3u + 2u * sentence_length;
	body[0] = 0x01;
	body[1] = (uint8_t)((2u * sentence_length) & 0xff);
	body[2] = (uint8_t)((2u * sentence_length) >> 8);
	length = put_message(message, QMI_INDICATION, VAYU_GNSS_LOC_EVENT_NMEA, 0, body, body_length);
	(void)vayu_gnss_parse_nmea_message(message, length, &fix, &sentences, &bad);
	check(sentences == 2, "two sentences in one message: %u read, not 2", (unsigned int)sentences);
}

/* ---- the map of a message's TLVs, and the log's rounding --------------------------------------- */

static void
test_tlv_map_and_privacy(void)
{
	uint8_t body[64];
	size_t body_length = 0;
	struct vayu_gnss_tlv_map map;
	char text[128], coord[12];

	body_length += put_tlv(body, 0x01, (const uint8_t *)"\x00", 1);
	body_length += put_tlv(body + body_length, 0x02, (const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00\x00", 8);
	vayu_gnss_tlv_map(body, body_length, &map);
	check(map.count == 2, "TLV map: %u entries, not 2", (unsigned int)map.count);
	check(map.type[0] == 0x01 && map.length[0] == 1 && map.type[1] == 0x02 && map.length[1] == 8,
	    "TLV map: not 01/1 02/8");
	vayu_gnss_tlv_map_text(&map, text, sizeof(text));
	check(strcmp(text, "01/1 02/8") == 0, "TLV map text: \"%s\", not \"01/1 02/8\"", text);

	/* the log keeps two decimals -- and rounds, not truncates */
	vayu_gnss_coord_text(coord, sizeof(coord), 557520566, 'N', 'S');
	check(strcmp(coord, "55.75N") == 0, "55.7520566 north for the log: \"%s\", not \"55.75N\"", coord);
	vayu_gnss_coord_text(coord, sizeof(coord), 557550000, 'N', 'S');
	check(strcmp(coord, "55.76N") == 0, "55.755 north for the log: \"%s\", not \"55.76N\"", coord);
	vayu_gnss_coord_text(coord, sizeof(coord), -376187233, 'E', 'W');
	check(strcmp(coord, "37.62W") == 0, "37.6187233 west for the log: \"%s\", not \"37.62W\"", coord);
	vayu_gnss_coord_text(coord, sizeof(coord), 0, 'N', 'S');
	check(strcmp(coord, "0.00N") == 0, "0 north for the log: \"%s\"", coord);
}

/* ---- the record's ABI ------------------------------------------------------------------------ */

static void
test_record_layout(void)
{
	check(sizeof(struct vayu_gnss_record) == 96, "record: %u bytes, not 96",
	    (unsigned int)sizeof(struct vayu_gnss_record));
	check(offsetof(struct vayu_gnss_record, magic) == 0 && offsetof(struct vayu_gnss_record, lat_e7) == 32 &&
	    offsetof(struct vayu_gnss_record, ttff_ms) == 64 && offsetof(struct vayu_gnss_record, sats_view) == 82 &&
	    offsetof(struct vayu_gnss_record, flags) == 88, "record: the offsets are not the documented ones");
	check(VAYU_GNSS_VERSION == 1, "record: version %u", (unsigned int)VAYU_GNSS_VERSION);
}

int
main(void)
{
	test_reg_events();
	test_start_forms();
	test_reply_result();
	test_position_report();
	test_nmea_gga();
	test_nmea_rmc_gsv_gsa();
	test_nmea_in_a_message();
	test_tlv_map_and_privacy();
	test_record_layout();
	printf("gnss-test: %d check(s), %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
