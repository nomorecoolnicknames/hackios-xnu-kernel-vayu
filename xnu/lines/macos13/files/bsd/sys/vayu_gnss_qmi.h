/*
 * The QMI messages bsd/dev/vayu_gnss.c speaks to the modem's location service, and the reading of
 * what the service says back. They are here, apart from the client, so that a program on the
 * machine that builds the kernel can code, decode and test the same messages (tools/gnss-test),
 * as sys/vayu_cell_qmi.h is for the cellular client.
 *
 * The service: QMI LOC, service id 0x10 (16), version 2, instance 0, on the modem, over QRTR -- FACT:
 * artifacts/vayu/test5-20261027/run/test5q-110918.log:448427 ("service 16 (0x10), version 2,
 * instance 0, at node 0 port 78"). The "instance 0x2" of the older run logs is that same 32-bit
 * word read without its packing: the router puts the version in its low byte and the instance
 * above it, so vayu_pas.c's handler prints version 2 / instance 0 (vayu_pas.c:3335, 3339-3340) while
 * vayu_modem.c:290-292 prints the whole word as the instance. The service is LOC v02, not v01;
 * the instance is 0 and a request is routed by the port (78), never by that field.
 *
 * The message ids are the QMI LOC v02 IDL's, the header Android's location HAL is generated from
 * (qmi_loc_v02.h): Register Events 0x0021, Start 0x0022, Stop 0x0023, and the indications this
 * client asks for: Position Report 0x0024, GNSS SV Info 0x0025, NMEA 0x0026.
 *
 * What is a FACT and what is a HYPOTHESIS here, because the difference matters when a run
 * produces nothing:
 *
 *   FACT   the service id 0x10 and the six message ids above: named in the reference this work was
 *          specified from (QMI_LOC_REG_EVENTS 0x0021, QMI_LOC_START 0x0022, QMI_LOC_STOP,
 *          QMI_LOC_EVENT_POSITION_REPORT_IND 0x0024, QMI_LOC_EVENT_NMEA_IND 0x0026, GNSS_SV_INFO),
 *          and consistent with the order of the enum those names come from.
 *   FACT   Register Events' TLV 0x01 is the event bitmask, and its bits are in the same order as
 *          the indications: position 0x0024 is bit 0, SV info 0x0025 bit 1, NMEA 0x0026 bit 2.
 *   FACT   that mask is EIGHT bytes wide in the LOC v02 IDL this modem runs, not four. The
 *          downstream library the phone's own Android uses for exactly this message,
 *          libloc_api_v02.so of the vayu vendor image
 *          (artifacts/vayu/shm-rescue-20261026/vayu-sens-tasks/vendor/ex/lib64/libloc_api_v02.so,
 *          build.prop: ro.product.vendor.device=vayu):
 *            - locClientRegisterEventMask (0x4d578) sends message 0x21 with the mask stored into
 *              the request's offset 0 by one 8-byte store ("str x1, [sp, #24]") and the message
 *              id in "mov w1, #0x21";
 *            - LocApiV02::convertMask(uint64_t) (0x218a4), whose result goes to that call, keeps
 *              event bits above 31 in the QMI mask ("and x4, x10, #0x10000000000" bit 40,
 *              "and x20, x10, #0x8000000000" bit 39, "and x2, x2, #0x800000000000" bit 47), which
 *              a 4-byte field cannot hold;
 *            - the service object it registers with is LOC v02, IDL revision 134
 *              (loc_get_service_object_internal_v02: major 2, minor 134, idl_version 6).
 *          A four-byte TLV 0x01 is therefore a different length from the one that IDL declares,
 *          and the modem refuses it with result 1, error 1 (malformed): TEST5Q,
 *          test5q-110918.log:449265. An older IDL (LOC v01, and v02 revisions before the mask
 *          grew) takes four bytes; the client falls back to that form, and says so, when the
 *          eight-byte one is refused as malformed.
 *   HYPOTHESIS  Start's TLV 0x01 session id, 0x03 "intermediate report", 0x04 minimum interval in
 *          milliseconds, 0x06 fix mode (bitmask, 0 = the engine's default). The client sends
 *          these in a fixed order of attempts -- all of them, then session+interval, then the
 *          session alone -- so a modem that refuses one form is still started by the next, and the
 *          log says which form it took. Nothing is silently guessed.
 *   HYPOTHESIS  the position report's field TLVs (0x06 latitude, 0x07 longitude, 0x08 HEPE,
 *          0x09 altitude), and whether a coordinate is binary64 or binary32. The reader takes
 *          either length and refuses a value outside the range a coordinate has instead of showing
 *          it as a position; the first reports' TLV map (types and lengths only, never payload) is
 *          logged, so a wrong guess is visible in one run instead of being presented as a fix.
 *   FACT   NMEA 0183 needs no guessing: a sentence is text, its checksum is the XOR of the bytes
 *          between '$' and '*', and GGA/RMC/GSA/GSV carry the position, the satellites used and in
 *          view, the HDOP and the altitude. This is the path that fills a fix when the binary
 *          report cannot be read. Sentences are parsed, never logged: one carries the exact
 *          position, and the log keeps two decimals.
 *
 * There is deliberately no Inject Time / Inject Position / Set Server here: this client opens no
 * assistance session and sends the engine no data of its own. Those messages' ids are not in the
 * reference this file was written from, and a request with a guessed id is what the project's
 * rules forbid.
 */
#ifndef _SYS_VAYU_GNSS_QMI_H_
#define _SYS_VAYU_GNSS_QMI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/hakios.qmi.h>

/* ---- what is asked, and of whom -------------------------------------------------------------- */

#define VAYU_GNSS_LOC_SERVICE       0x10        /* QMI LOC: the GNSS engine in the modem */
#define VAYU_GNSS_DMS_SERVICE       0x02        /* only Get Operating Mode: is the modem ONLINE? */
#define VAYU_GNSS_LOC_VERSION       1           /* asked for; the server's own version is logged */

#define VAYU_GNSS_LOC_REG_EVENTS                0x0021
#define VAYU_GNSS_LOC_START                     0x0022
#define VAYU_GNSS_LOC_STOP                      0x0023
#define VAYU_GNSS_LOC_EVENT_POSITION_REPORT     0x0024
#define VAYU_GNSS_LOC_EVENT_SV_INFO             0x0025
#define VAYU_GNSS_LOC_EVENT_NMEA                0x0026

#define VAYU_GNSS_DMS_GET_OPERATING_MODE        0x002d    /* qmi-service-dms.json "Get Operating Mode" */

/* The event bitmask of Register Events: bit n is the indication 0x0024 + n. */
#define VAYU_GNSS_EVENT_POSITION     0x00000001u
#define VAYU_GNSS_EVENT_SV_INFO      0x00000002u
#define VAYU_GNSS_EVENT_NMEA         0x00000004u

/* DMS operating modes (QmiDmsOperatingMode): 0 is ONLINE, the one this client waits for. */
#define VAYU_GNSS_DMS_ONLINE         0

/* The largest request built here is Start with four TLVs (under 32 bytes). */
#define VAYU_GNSS_REQUEST_MAX        64

/* ---- the fix, as the two readers of a message fill it --------------------------------------- */

/* Which reader put something into a fix. OR-ed: one fix may come from both. */
#define VAYU_GNSS_SOURCE_QMI         0x01        /* the binary position report */
#define VAYU_GNSS_SOURCE_NMEA        0x02        /* a GGA or RMC sentence */
#define VAYU_GNSS_QUALITY_UNKNOWN    0xff

struct vayu_gnss_fix {
	bool     position;              /* lat_e7 and lon_e7 are sane and set */
	int32_t  lat_e7;                /* degrees * 1e7, north positive */
	int32_t  lon_e7;                /* degrees * 1e7, east positive */
	bool     has_altitude;
	int32_t  altitude_mm;
	bool     has_hepe;              /* horizontal estimated position error */
	uint32_t hepe_cm;
	bool     has_hdop;
	uint32_t hdop_centi;            /* HDOP * 100 */
	bool     has_sats_used;
	uint16_t sats_used;
	bool     has_sats_view;
	uint16_t sats_view;
	bool     has_speed;
	uint32_t speed_mm_s;
	bool     has_heading;
	uint32_t heading_mdeg;
	bool     has_tod;               /* time of day of the fix, from the sentences' UTC */
	uint32_t tod_ms;
	uint8_t  quality;               /* NMEA GGA fix quality: 0 none, 1 GPS, 2 DGPS, 4 RTK; 0xff unknown */
	uint8_t  source;                /* VAYU_GNSS_SOURCE_* */
};

/* Sets a position, refusing one the earth does not have. */
static inline bool
vayu_gnss_fix_position(struct vayu_gnss_fix *fix, int32_t lat_e7, int32_t lon_e7, uint8_t source)
{
	if (lat_e7 < -900000000 || lat_e7 > 900000000 || lon_e7 < -1800000000 || lon_e7 > 1800000000) {
		return false;
	}
	fix->position = true;
	fix->lat_e7 = lat_e7;
	fix->lon_e7 = lon_e7;
	fix->source |= source;
	return true;
}

/* ---- little-endian, as QMI is ---------------------------------------------------------------- */

static inline uint16_t
vayu_gnss_le16(const uint8_t *at)
{
	return (uint16_t)(at[0] | (at[1] << 8));
}

static inline uint32_t
vayu_gnss_le32(const uint8_t *at)
{
	return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static inline uint64_t
vayu_gnss_le64(const uint8_t *at)
{
	return (uint64_t)vayu_gnss_le32(at) | ((uint64_t)vayu_gnss_le32(at + 4) << 32);
}

/* A coordinate as the IDL may send it: binary64 or binary32, both little-endian. */
static inline double
vayu_gnss_real(const uint8_t *at, uint16_t length)
{
	if (length == 8) {
		union { uint64_t bits; double value; } real;

		real.bits = vayu_gnss_le64(at);
		return real.value;
	}
	if (length == 4) {
		union { uint32_t bits; float value; } real;

		real.bits = vayu_gnss_le32(at);
		return (double)real.value;
	}
	return 0.0;
}

/* Degrees to 1e-7 degrees, rounded away from zero. The range is the caller's to check. */
static inline int32_t
vayu_gnss_e7(double degrees)
{
	double scaled = degrees * 10000000.0;

	return (int32_t)(int64_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

/*
 * A coordinate for the log, rounded to two decimals -- about a kilometre: the log says where in
 * the world a fix was, not where the person holding the phone is. 55.7520 north -> "55.75N".
 * The last character is the hemisphere; `size` must be at least 8.
 */
static inline void
vayu_gnss_coord_text(char *out, size_t size, int32_t e7, char positive, char negative)
{
	static const char digits[] = "0123456789";
	uint32_t magnitude = e7 < 0 ? (uint32_t)-(int64_t)e7 : (uint32_t)e7;
	uint32_t hundredths = (magnitude + 50000u) / 100000u;    /* round to 0.01 degree */
	uint32_t limit = (positive == 'N' || positive == 'S') ? 9000u : 18000u;
	uint32_t whole;
	int at = 0;

	if (size < 8) {
		if (size != 0) {
			out[0] = 0;
		}
		return;
	}
	if (hundredths > limit) {
		hundredths = limit;
	}
	whole = hundredths / 100u;
	if (whole >= 100u) {
		out[at++] = digits[(whole / 100u) % 10u];
	}
	if (whole >= 10u) {
		out[at++] = digits[(whole / 10u) % 10u];
	}
	out[at++] = digits[whole % 10u];
	out[at++] = '.';
	out[at++] = digits[(hundredths % 100u) / 10u];
	out[at++] = digits[hundredths % 10u];
	out[at++] = e7 < 0 ? negative : positive;
	out[at] = 0;
}

/* ---- TLVs, walked by hand both ways --------------------------------------------------------- */

/*
 * One TLV of a message's body (the seven-byte QMI header is not part of it). `offset` is where its
 * header is; `length` is what its length field says, checked against the body.
 */
struct vayu_gnss_tlv {
	uint8_t        type;
	uint16_t       length;
	const uint8_t *value;
	size_t         offset;
};

static inline bool
vayu_gnss_tlv_at(const uint8_t *body, size_t body_length, size_t offset, struct vayu_gnss_tlv *tlv)
{
	uint16_t length;

	if (offset > body_length || body_length - offset < 3) {
		return false;
	}
	length = vayu_gnss_le16(body + offset + 1);
	if (body_length - offset - 3 < length) {
		return false;                   /* longer than the body: not a TLV of this message */
	}
	tlv->type = body[offset];
	tlv->length = length;
	tlv->value = body + offset + 3;
	tlv->offset = offset;
	return true;
}

static inline bool
vayu_gnss_tlv_first(const uint8_t *body, size_t body_length, struct vayu_gnss_tlv *tlv)
{
	return vayu_gnss_tlv_at(body, body_length, 0, tlv);
}

static inline bool
vayu_gnss_tlv_next(const uint8_t *body, size_t body_length, struct vayu_gnss_tlv *tlv)
{
	return vayu_gnss_tlv_at(body, body_length, tlv->offset + 3 + tlv->length, tlv);
}

/* The first TLV of a type, copied into `found`, or NULL. */
static inline const struct vayu_gnss_tlv *
vayu_gnss_tlv_find(const uint8_t *body, size_t body_length, uint8_t type, struct vayu_gnss_tlv *found)
{
	struct vayu_gnss_tlv tlv;

	for (bool any = vayu_gnss_tlv_first(body, body_length, &tlv); any; any = vayu_gnss_tlv_next(body, body_length, &tlv)) {
		if (tlv.type == type) {
			*found = tlv;
			return found;
		}
	}
	return NULL;
}

/* What a message carries, for the log: type and length of its first few TLVs, never their bytes. */
#define VAYU_GNSS_TLV_MAP_MAX   12

struct vayu_gnss_tlv_map {
	uint8_t  count;
	uint8_t  type[VAYU_GNSS_TLV_MAP_MAX];
	uint16_t length[VAYU_GNSS_TLV_MAP_MAX];
};

static inline void
vayu_gnss_tlv_map(const uint8_t *body, size_t body_length, struct vayu_gnss_tlv_map *map)
{
	struct vayu_gnss_tlv tlv;

	map->count = 0;
	for (bool any = vayu_gnss_tlv_first(body, body_length, &tlv);
	    any && map->count < VAYU_GNSS_TLV_MAP_MAX; any = vayu_gnss_tlv_next(body, body_length, &tlv)) {
		map->type[map->count] = tlv.type;
		map->length[map->count] = tlv.length;
		map->count++;
	}
}

/* "01/1 02/8 06/8", for one line of the log; the payload is never in it. */
static inline void
vayu_gnss_tlv_map_text(const struct vayu_gnss_tlv_map *map, char *out, size_t size)
{
	static const char hex[] = "0123456789abcdef";
	size_t used = 0;

	if (size == 0) {
		return;
	}
	out[0] = 0;
	for (uint8_t i = 0; i < map->count; i++) {
		char one[16];
		int length = 0;
		uint16_t value = map->length[i];

		one[length++] = hex[(map->type[i] >> 4) & 0x0f];
		one[length++] = hex[map->type[i] & 0x0f];
		one[length++] = '/';
		if (value >= 1000) {
			one[length++] = (char)('0' + (value / 1000) % 10);
		}
		if (value >= 100) {
			one[length++] = (char)('0' + (value / 100) % 10);
		}
		if (value >= 10) {
			one[length++] = (char)('0' + (value / 10) % 10);
		}
		one[length++] = (char)('0' + value % 10);
		if (i != 0) {
			if (used + 1 >= size) {
				break;
			}
			out[used++] = ' ';
		}
		for (int j = 0; j < length; j++) {
			if (used + 1 >= size) {
				break;
			}
			out[used++] = one[j];
		}
		out[used] = 0;
	}
}

/* ---- a message's body ------------------------------------------------------------------------ */

/*
 * The body of a QMI message: past the seven-byte header, as long as the header says (a datagram
 * may carry more than its message). False when the header is short or says more than the datagram
 * has.
 */
static inline bool
vayu_gnss_body(const uint8_t *message, size_t length, const uint8_t **body, size_t *body_length)
{
	uint16_t said;

	if (length < QMI_HEADER_SIZE) {
		return false;
	}
	said = vayu_gnss_le16(message + 5);
	if ((size_t)said > length - QMI_HEADER_SIZE) {
		return false;
	}
	*body = message + QMI_HEADER_SIZE;
	*body_length = said;
	return true;
}

/* ---- the position report (indication 0x0024) ------------------------------------------------ */

#define VAYU_GNSS_POS_SESSION   0x01
#define VAYU_GNSS_POS_LATITUDE  0x06
#define VAYU_GNSS_POS_LONGITUDE 0x07
#define VAYU_GNSS_POS_HEPE      0x08
#define VAYU_GNSS_POS_ALTITUDE  0x09

/*
 * Reads a position report into `fix` (only what the report has; nothing is cleared). True when a
 * sane latitude and longitude are in it. A value outside the range a coordinate has is not a
 * position: it is refused, and the caller logs the TLV map once so the layout can be corrected
 * from evidence rather than by guessing again.
 */
static inline bool
vayu_gnss_parse_position_report(const uint8_t *message, size_t length, struct vayu_gnss_fix *fix)
{
	const uint8_t *body;
	size_t body_length;
	struct vayu_gnss_tlv found;
	double latitude, longitude;
	bool position;

	if (!vayu_gnss_body(message, length, &body, &body_length)) {
		return false;
	}

	if (vayu_gnss_tlv_find(body, body_length, VAYU_GNSS_POS_SESSION, &found) != NULL && found.length == 1 &&
	    found.value[0] != 0) {
		return false;                   /* another client's session, not the one this client starts */
	}
	if (vayu_gnss_tlv_find(body, body_length, VAYU_GNSS_POS_LATITUDE, &found) == NULL ||
	    (found.length != 8 && found.length != 4)) {
		return false;
	}
	latitude = vayu_gnss_real(found.value, found.length);
	if (vayu_gnss_tlv_find(body, body_length, VAYU_GNSS_POS_LONGITUDE, &found) == NULL ||
	    (found.length != 8 && found.length != 4)) {
		return false;
	}
	longitude = vayu_gnss_real(found.value, found.length);
	position = vayu_gnss_fix_position(fix, vayu_gnss_e7(latitude), vayu_gnss_e7(longitude), VAYU_GNSS_SOURCE_QMI);

	if (vayu_gnss_tlv_find(body, body_length, VAYU_GNSS_POS_HEPE, &found) != NULL &&
	    (found.length == 8 || found.length == 4)) {
		double hepe = vayu_gnss_real(found.value, found.length);

		if (hepe >= 0.0 && hepe < 100000.0) {
			fix->has_hepe = true;
			fix->hepe_cm = (uint32_t)(hepe * 100.0 + 0.5);
		}
	}
	if (vayu_gnss_tlv_find(body, body_length, VAYU_GNSS_POS_ALTITUDE, &found) != NULL &&
	    (found.length == 8 || found.length == 4)) {
		double altitude = vayu_gnss_real(found.value, found.length);

		if (altitude > -1000.0 && altitude < 30000.0) {
			fix->has_altitude = true;
			fix->altitude_mm = (int32_t)(int64_t)(altitude * 1000.0 + (altitude >= 0.0 ? 0.5 : -0.5));
		}
	}
	return position;
}

/* ---- NMEA 0183 ------------------------------------------------------------------------------ */

#define VAYU_GNSS_NMEA_FIELDS   20
#define VAYU_GNSS_NMEA_MAX      128     /* the standard allows 82 characters; modems pack a few more */
#define VAYU_GNSS_NMEA_SENTENCES 4      /* at most this many in one QMI message */

/* What one sentence gave. */
#define VAYU_GNSS_NMEA_BAD      (-1)    /* malformed, or its checksum is wrong */
#define VAYU_GNSS_NMEA_IGNORED  0       /* a sentence this client does not read (VTG, ZDA, ...) */
#define VAYU_GNSS_NMEA_READ     1       /* read, no position in it (GSV, GSA, a GGA without a fix) */
#define VAYU_GNSS_NMEA_POSITION 2       /* read, and it had a position */

/* Splits the body of a sentence ("GPGGA,123519,...", between '$' and '*') at its commas. */
static inline int
vayu_gnss_nmea_fields(const char *sentence, size_t length, const char *field[VAYU_GNSS_NMEA_FIELDS],
    uint16_t field_length[VAYU_GNSS_NMEA_FIELDS])
{
	int count = 0;
	size_t start = 0;

	for (size_t i = 0; i <= length; i++) {
		if (i == length || sentence[i] == ',') {
			if (count < VAYU_GNSS_NMEA_FIELDS) {
				field[count] = sentence + start;
				field_length[count] = (uint16_t)(i - start);
				count++;
			}
			start = i + 1;
		}
	}
	return count;
}

/* An unsigned field: digits only, at most nine of them. */
static inline bool
vayu_gnss_nmea_uint(const char *s, uint16_t length, uint32_t *out)
{
	uint32_t value = 0;

	if (length == 0 || length > 9) {
		return false;
	}
	for (uint16_t i = 0; i < length; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
		value = value * 10u + (uint32_t)(s[i] - '0');
	}
	*out = value;
	return true;
}

/*
 * A decimal field scaled by 10^decimals: "156.4" with 3 -> 156400, "-12.35" with 3 -> -12350;
 * digits past `decimals` after the point are dropped. False when there is no digit at all.
 */
static inline bool
vayu_gnss_decimal(const char *s, uint16_t length, uint32_t decimals, int64_t *out)
{
	bool negative = false;
	bool seen_digit = false;
	bool seen_point = false;
	uint32_t kept = 0;
	int64_t value = 0;

	if (length == 0 || length > 16) {
		return false;
	}
	if (s[0] == '-' || s[0] == '+') {
		negative = s[0] == '-';
		s++;
		length--;
	}
	for (uint16_t i = 0; i < length; i++) {
		if (s[i] == '.') {
			if (seen_point) {
				return false;
			}
			seen_point = true;
			continue;
		}
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
		seen_digit = true;
		if (!seen_point) {
			value = value * 10 + (s[i] - '0');      /* the whole part, every digit of it */
		} else if (kept < decimals) {
			value = value * 10 + (s[i] - '0');
			kept++;
		}
	}
	if (!seen_digit) {
		return false;
	}
	while (kept < decimals) {
		value *= 10;
		kept++;
	}
	*out = negative ? -value : value;
	return true;
}

/* ddmm.mmmm with a hemisphere, to degrees * 1e7: "5545.1234" N -> 557520556. */
static inline bool
vayu_gnss_nmea_coord(const char *s, uint16_t length, char hemisphere, int32_t *e7)
{
	int64_t scaled = 0;
	uint32_t degrees, minutes_1e4;

	if (hemisphere != 'N' && hemisphere != 'S' && hemisphere != 'E' && hemisphere != 'W') {
		return false;
	}
	if (!vayu_gnss_decimal(s, length, 4, &scaled) || scaled < 0) {
		return false;
	}
	degrees = (uint32_t)(scaled / 1000000);                 /* the degree digits */
	minutes_1e4 = (uint32_t)(scaled % 1000000);             /* mm.mmmm * 1e4 */
	if (minutes_1e4 >= 600000u) {                           /* a minute is 60 of them */
		return false;
	}
	*e7 = (int32_t)(degrees * 10000000u + (minutes_1e4 * 50u) / 3u);
	if (hemisphere == 'S' || hemisphere == 'W') {
		*e7 = -*e7;
	}
	return true;
}

/* hhmmss.ss -> milliseconds of the day */
static inline bool
vayu_gnss_nmea_time(const char *s, uint16_t length, uint32_t *ms)
{
	uint32_t hours, minutes, seconds, thousandths = 0;

	if (length < 6) {
		return false;
	}
	if (!vayu_gnss_nmea_uint(s, 2, &hours) || !vayu_gnss_nmea_uint(s + 2, 2, &minutes) ||
	    !vayu_gnss_nmea_uint(s + 4, 2, &seconds)) {
		return false;
	}
	if (length > 7 && s[6] == '.') {
		int64_t fraction = 0;

		if (vayu_gnss_decimal(s + 7, (uint16_t)(length - 7), 3, &fraction)) {
			thousandths = (uint32_t)fraction;
		}
	}
	if (hours > 23 || minutes > 59 || seconds > 60) {
		return false;
	}
	*ms = hours * 3600000u + minutes * 60000u + seconds * 1000u + thousandths;
	return true;
}

static inline char
vayu_gnss_upper(char c)
{
	return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

/* Is the sentence's type field ("GPGGA", "GNGGA", "GLGSV") the three letters given? */
static inline bool
vayu_gnss_nmea_type(const char *field, uint16_t length, const char *type)
{
	return length >= 3 &&
	    vayu_gnss_upper(field[length - 3]) == type[0] &&
	    vayu_gnss_upper(field[length - 2]) == type[1] &&
	    vayu_gnss_upper(field[length - 1]) == type[2];
}

/*
 * One sentence, from its '$' to the end of its buffer. Its checksum, if it has one, is checked.
 * What it carries goes into `fix`; the return is one of VAYU_GNSS_NMEA_*.
 */
static inline int
vayu_gnss_parse_nmea_sentence(const char *sentence, size_t length, struct vayu_gnss_fix *fix)
{
	const char *field[VAYU_GNSS_NMEA_FIELDS];
	uint16_t field_length[VAYU_GNSS_NMEA_FIELDS];
	size_t end = length;
	const char *star = NULL;
	int fields;

	if (length < 7 || length > VAYU_GNSS_NMEA_MAX || sentence[0] != '$') {
		return VAYU_GNSS_NMEA_BAD;
	}
	for (size_t i = 1; i < length; i++) {
		if (sentence[i] == '*') {
			star = sentence + i;
			end = i;
			break;
		}
	}
	/* the checksum is the XOR of everything between the '$' and the '*', as two hex digits */
	if (star != NULL) {
		uint8_t checksum = 0, wanted = 0;

		if (length - end < 3) {
			return VAYU_GNSS_NMEA_BAD;
		}
		for (size_t i = 1; i < end; i++) {
			checksum ^= (uint8_t)sentence[i];
		}
		for (int i = 1; i <= 2; i++) {
			char c = vayu_gnss_upper(star[i]);
			uint8_t nibble;

			if (c >= '0' && c <= '9') {
				nibble = (uint8_t)(c - '0');
			} else if (c >= 'A' && c <= 'F') {
				nibble = (uint8_t)(c - 'A' + 10);
			} else {
				return VAYU_GNSS_NMEA_BAD;
			}
			wanted = (uint8_t)((wanted << 4) | nibble);
		}
		if (wanted != checksum) {
			return VAYU_GNSS_NMEA_BAD;
		}
	}
	fields = vayu_gnss_nmea_fields(sentence + 1, end - 1, field, field_length);
	if (fields < 2) {
		return VAYU_GNSS_NMEA_BAD;
	}

	if (vayu_gnss_nmea_type(field[0], field_length[0], "GGA")) {
		uint32_t quality = 0, sats = 0, tod = 0;
		int64_t hdop = 0, altitude = 0;
		int32_t lat_e7, lon_e7;

		if (fields < 10) {
			return VAYU_GNSS_NMEA_IGNORED;
		}
		if (vayu_gnss_nmea_uint(field[6], field_length[6], &quality)) {
			fix->quality = (uint8_t)(quality > 8 ? VAYU_GNSS_QUALITY_UNKNOWN : quality);
		}
		if (vayu_gnss_nmea_uint(field[7], field_length[7], &sats) && sats > 0 && sats < 1000) {
			fix->has_sats_used = true;
			fix->sats_used = (uint16_t)sats;
		}
		if (vayu_gnss_decimal(field[8], field_length[8], 2, &hdop) && hdop > 0 && hdop < 10000) {
			fix->has_hdop = true;
			fix->hdop_centi = (uint32_t)hdop;
		}
		if (vayu_gnss_nmea_time(field[1], field_length[1], &tod)) {
			fix->has_tod = true;
			fix->tod_ms = tod;
		}
		/* the altitude is field 9, in metres; a session with no fix has it empty, not zero */
		if (vayu_gnss_decimal(field[9], field_length[9], 3, &altitude) &&
		    altitude > -1000000 && altitude < 30000000) {
			fix->has_altitude = true;
			fix->altitude_mm = (int32_t)altitude;
		}
		if (quality == 0 || field_length[2] == 0 || field_length[4] == 0) {
			return VAYU_GNSS_NMEA_READ;             /* no fix yet: the rest of the fields still count */
		}
		if (!vayu_gnss_nmea_coord(field[2], field_length[2], field_length[3] != 0 ? field[3][0] : 0, &lat_e7) ||
		    !vayu_gnss_nmea_coord(field[4], field_length[4], field_length[5] != 0 ? field[5][0] : 0, &lon_e7)) {
			return VAYU_GNSS_NMEA_READ;
		}
		return vayu_gnss_fix_position(fix, lat_e7, lon_e7, VAYU_GNSS_SOURCE_NMEA) ?
		    VAYU_GNSS_NMEA_POSITION : VAYU_GNSS_NMEA_READ;
	}

	if (vayu_gnss_nmea_type(field[0], field_length[0], "RMC")) {
		int32_t lat_e7, lon_e7;
		uint32_t tod = 0;
		int64_t knots = 0, course = 0;

		if (fields < 9) {
			return VAYU_GNSS_NMEA_IGNORED;
		}
		if (vayu_gnss_nmea_time(field[1], field_length[1], &tod)) {
			fix->has_tod = true;
			fix->tod_ms = tod;
		}
		if (vayu_gnss_decimal(field[7], field_length[7], 3, &knots) && knots >= 0 && knots < 2000000) {
			fix->has_speed = true;
			fix->speed_mm_s = (uint32_t)((uint64_t)knots * 514444u / 1000000u);
		}
		if (vayu_gnss_decimal(field[8], field_length[8], 3, &course) && course >= 0 && course < 360000) {
			fix->has_heading = true;
			fix->heading_mdeg = (uint32_t)course;
		}
		if (field_length[2] == 0 || vayu_gnss_upper(field[2][0]) != 'A') {
			return VAYU_GNSS_NMEA_READ;             /* V: the receiver's own warning, no valid fix */
		}
		if (!vayu_gnss_nmea_coord(field[3], field_length[3], field_length[4] != 0 ? field[4][0] : 0, &lat_e7) ||
		    !vayu_gnss_nmea_coord(field[5], field_length[5], field_length[6] != 0 ? field[6][0] : 0, &lon_e7)) {
			return VAYU_GNSS_NMEA_READ;
		}
		return vayu_gnss_fix_position(fix, lat_e7, lon_e7, VAYU_GNSS_SOURCE_NMEA) ?
		    VAYU_GNSS_NMEA_POSITION : VAYU_GNSS_NMEA_READ;
	}

	if (vayu_gnss_nmea_type(field[0], field_length[0], "GSV")) {
		uint32_t in_view = 0;

		if (fields < 4) {
			return VAYU_GNSS_NMEA_IGNORED;
		}
		if (vayu_gnss_nmea_uint(field[3], field_length[3], &in_view) && in_view > 0 && in_view < 1000) {
			fix->has_sats_view = true;
			fix->sats_view = (uint16_t)in_view;
		}
		return VAYU_GNSS_NMEA_READ;
	}

	if (vayu_gnss_nmea_type(field[0], field_length[0], "GSA")) {
		uint32_t used = 0;
		int64_t hdop = 0;

		/* fields 3..14 are the satellites used; 16 is the HDOP (NMEA 2.3 and later) */
		for (int i = 3; i <= 14 && i < fields; i++) {
			if (field_length[i] != 0) {
				used++;
			}
		}
		if (used != 0) {
			fix->has_sats_used = true;
			fix->sats_used = (uint16_t)used;
		}
		if (fields > 16 && vayu_gnss_decimal(field[16], field_length[16], 2, &hdop) && hdop > 0 && hdop < 10000) {
			fix->has_hdop = true;
			fix->hdop_centi = (uint32_t)hdop;
		}
		return VAYU_GNSS_NMEA_READ;
	}
	return VAYU_GNSS_NMEA_IGNORED;
}

/*
 * Every sentence in `data` (a NMEA indication's string, or a run of them), up to four of them. The
 * number of sentences read and of those refused are added to the counters; true when a position
 * was among them.
 */
static inline bool
vayu_gnss_parse_nmea(const uint8_t *data, size_t length, struct vayu_gnss_fix *fix, uint32_t *sentences,
    uint32_t *bad)
{
	bool position = false;
	uint32_t read = 0;
	size_t i = 0;

	while (i < length && read < VAYU_GNSS_NMEA_SENTENCES) {
		size_t start, end;

		if (data[i] != '$') {
			i++;
			continue;
		}
		start = i;
		end = start + 1;                /* the '$' itself is not the end of the sentence */
		while (end < length && data[end] != '$' && data[end] != 0 && data[end] != '\r' && data[end] != '\n' &&
		    end - start < VAYU_GNSS_NMEA_MAX) {
			end++;
		}
		if (end - start >= 7) {
			char sentence[VAYU_GNSS_NMEA_MAX];
			size_t sentence_length = end - start;
			int result;

			for (size_t c = 0; c < sentence_length; c++) {
				sentence[c] = (char)data[start + c];
			}
			result = vayu_gnss_parse_nmea_sentence(sentence, sentence_length, fix);
			if (result == VAYU_GNSS_NMEA_BAD) {
				(*bad)++;
			} else {
				read++;
				(*sentences)++;
				if (result == VAYU_GNSS_NMEA_POSITION) {
					position = true;
				}
			}
		}
		i = end;
	}
	return position;
}

/*
 * The NMEA indication (0x0026): the sentences are in a string TLV whose number need not be guessed
 * -- any TLV that begins with '$' is one, and so is a message with no TLV at all.
 */
static inline bool
vayu_gnss_parse_nmea_message(const uint8_t *message, size_t length, struct vayu_gnss_fix *fix,
    uint32_t *sentences, uint32_t *bad)
{
	const uint8_t *body;
	size_t body_length;
	struct vayu_gnss_tlv tlv;
	bool position = false;
	bool any_tlv = false;

	if (!vayu_gnss_body(message, length, &body, &body_length)) {
		return false;
	}
	for (bool any = vayu_gnss_tlv_first(body, body_length, &tlv); any;
	    any = vayu_gnss_tlv_next(body, body_length, &tlv)) {
		any_tlv = true;
		if (tlv.length != 0 && tlv.value[0] == '$') {
			position |= vayu_gnss_parse_nmea(tlv.value, tlv.length, fix, sentences, bad);
		}
	}
	if (!any_tlv && body[0] == '$') {
		position |= vayu_gnss_parse_nmea(body, body_length, fix, sentences, bad);
	}
	return position;
}

/* ---- a reply's two TLVs ---------------------------------------------------------------------- */

/* TLV 0x02 of a reply: the result and the error the modem gives. False when it has none. */
static inline bool
vayu_gnss_reply_result(const uint8_t *message, size_t length, uint16_t *result, uint16_t *error)
{
	const uint8_t *body;
	size_t body_length;
	struct vayu_gnss_tlv found;

	if (!vayu_gnss_body(message, length, &body, &body_length)) {
		return false;
	}
	if (vayu_gnss_tlv_find(body, body_length, 0x02, &found) == NULL || found.length < 4) {
		return false;
	}
	*result = vayu_gnss_le16(found.value);
	*error = vayu_gnss_le16(found.value + 2);
	return true;
}

/* TLV 0x01 of a reply: one byte, which is what DMS Get Operating Mode's mode is. */
static inline bool
vayu_gnss_reply_mode(const uint8_t *message, size_t length, uint8_t *mode)
{
	const uint8_t *body;
	size_t body_length;
	struct vayu_gnss_tlv found;

	if (!vayu_gnss_body(message, length, &body, &body_length)) {
		return false;
	}
	if (vayu_gnss_tlv_find(body, body_length, 0x01, &found) == NULL || found.length < 1) {
		return false;
	}
	*mode = found.value[0];
	return true;
}

/* ---- the requests, as tables of struct qmi_elem_info (sys/hakios.qmi.h) ---------------------- */

/* Register Events (0x0021): TLV 0x01, the mask of the indications that are wanted. Eight bytes:
 * the LOC v02 IDL of this modem's generation widened the mask to 64 bits (see the FACT above). */
struct vayu_gnss_reg_events_req {
	uint64_t mask;
};

static const struct qmi_elem_info vayu_gnss_reg_events_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_gnss_reg_events_req, mask) },
	{ .data_type = QMI_EOTI },
};

/* The same request with a four-byte mask: the LOC v01 layout, and what the first LOC v02 revisions
 * had. Sent only when the modem calls the eight-byte one malformed. */
struct vayu_gnss_reg_events_req32 {
	uint32_t mask;
};

static const struct qmi_elem_info vayu_gnss_reg_events_req32_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_gnss_reg_events_req32, mask) },
	{ .data_type = QMI_EOTI },
};

/*
 * Start (0x0022), in the three forms the client tries in order (see the top of this file). The
 * fixed point: the session id (0x01) and the minimum interval (0x04) are the two this file is
 * confident of; the rest is added when the modem takes it.
 */
struct vayu_gnss_start_req {
	uint8_t  session;               /* TLV 0x01 */
	uint8_t  intermediate;          /* TLV 0x03: reports between fixes (0: only the fixes) */
	uint32_t min_interval_ms;       /* TLV 0x04: 1000 for one fix a second */
	uint32_t fix_mode;              /* TLV 0x06: a bitmask; 0 is the engine's default */
};

struct vayu_gnss_start_core_req {
	uint8_t  session;               /* TLV 0x01 */
	uint32_t min_interval_ms;       /* TLV 0x04 */
};

struct vayu_gnss_start_min_req {
	uint8_t  session;               /* TLV 0x01 */
};

static const struct qmi_elem_info vayu_gnss_start_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_gnss_start_req, session) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x03, .offset = offsetof(struct vayu_gnss_start_req, intermediate) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x04, .offset = offsetof(struct vayu_gnss_start_req, min_interval_ms) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x06, .offset = offsetof(struct vayu_gnss_start_req, fix_mode) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_gnss_start_core_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_gnss_start_core_req, session) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x04, .offset = offsetof(struct vayu_gnss_start_core_req, min_interval_ms) },
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_gnss_start_min_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_gnss_start_min_req, session) },
	{ .data_type = QMI_EOTI },
};

/* Stop (0x0023): the session id alone. */
struct vayu_gnss_stop_req {
	uint8_t session;                /* TLV 0x01 */
};

static const struct qmi_elem_info vayu_gnss_stop_req_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct vayu_gnss_stop_req, session) },
	{ .data_type = QMI_EOTI },
};

/* DMS Get Operating Mode (0x002d): no request TLVs; the answer's TLV 0x01 is the mode (above). */

#endif /* _SYS_VAYU_GNSS_QMI_H_ */
