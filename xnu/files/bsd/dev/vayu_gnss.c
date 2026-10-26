/*
 * GNSS for the POCO X3 Pro (vayu, SM8150-AC): the GNSS engine is in the modem (MPSS), and what a
 * location client on the application processor does is ask it over QMI's location service (LOC,
 * service 0x10) on the modem's QRTR. This file is that client: it opens a LOC session, registers
 * for the position, SV-info and NMEA indications, starts a periodic fix (one a second), logs what
 * comes back, and stops the session before the modem is taken down.
 *
 * It sends nothing but those four requests, and it changes no modem state: the ONLINE it waits for
 * is vayu_cell.c's to set (vayu_cell_online=1), and this client only reads DMS's operating mode.
 * No assistance data is written, no packet is sent, no storage is touched.
 *
 * Why it speaks QRTR directly instead of going through sys/hakios.qmi.h's handle: the handle hands
 * a message to a handler only after decoding it with a table of struct qmi_elem_info, and a table
 * that names a field the modem does not send in the size expected makes the whole message fail to
 * decode -- losing the very evidence (which TLVs did come) that a first run is for. The message
 * *coding* (hakios_qmi_encode) is still the helpers', and a request is still a transaction that
 * waits for its reply; what is here and not in the helpers is the reading of an indication's bytes,
 * which sys/vayu_gnss_qmi.h does and tools/gnss-test tests.
 *
 * Facts behind the numbers:
 *   - the service: the modem announces QMI LOC 0x10 instance 0x2 over QRTR -- FACT,
 *     artifacts/vayu/test1-20261026/base-run.log:59790 ("service 16 (0x10) instance 0x2 at node 0
 *     port 77"), t1-run.log:61567 (port 78), a test run-20261026/025023-run.log:56079 (port 79).
 *   - the message ids, the event bits, and which parts of the answer are a guess:
 *     sys/vayu_gnss_qmi.h, at its top, line by line.
 *   - NMEA 0183 is the path that cannot be misread: GGA and RMC carry the position, GGA the
 *     satellites used and the HDOP, GSV the satellites in view, GSA a second copy of the HDOP.
 *
 * Boot-args:
 *   vayu_gnss=0            nothing at all (the default)
 *   vayu_gnss=1            one session, as above
 *   vayu_gnss_wait=S       seconds to wait for the LOC service and for DMS to say ONLINE
 *                          (default 30, 1..120)
 *   vayu_gnss_run=S        seconds a session runs after LOC_START is accepted (default 120, 5..600)
 *   vayu_gnss_interval_ms=N  minimum interval between fixes (default 1000, 200..60000)
 *
 * Privacy: the log keeps two decimals of a coordinate (about a kilometre) and never a payload --
 * the TLV map of the first position report and of the first SV-info report is types and lengths
 * only, and an NMEA sentence is parsed, never printed. The record behind /dev/vayu-gnss keeps the
 * full 1e-7 degrees, because that is what userland is for.
 *
 * What is NOT known until it runs on the phone (HYPOTHESIS): that this modem accepts the three
 * Start forms in the order tried here; that its position report uses the field TLVs the reader
 * expects (and a double rather than a float); that it emits NMEA at all without a Set NMEA Types
 * request, and in which of its TLVs it puts the sentences (any TLV that begins with '$' is read).
 * Each of those fails visibly: one line in the log, and the record says what was not learned.
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
#include <sys/param.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vayu_gnss.h>
#include <sys/vayu_gnss_qmi.h>
#include <sys/vayu_pas.h>

extern void loader_log_putc(char c);

#define VAYU_GNSS_DEVICE         "vayu-gnss"
#define VAYU_GNSS_REPLY_MAX      512    /* the largest reply kept for its TLVs */
#define VAYU_GNSS_REPLY_MS       5000   /* one request's reply */
#define VAYU_GNSS_START_REPLY_MS 12000  /* the engine may take a few seconds to say it started */
#define VAYU_GNSS_STOP_MS        18000  /* the longest wait for the thread to end (one request) */
#define VAYU_GNSS_PROGRESS_MS    10000  /* a summary line this often while the session runs */
#define VAYU_GNSS_LOG_MS         1000   /* at most one fix line a second */
#define VAYU_GNSS_STALE_MS       5000   /* a position older than this is in the record, but marked */

/* ---- state --------------------------------------------------------------------------------------- */

struct vayu_gnss_state {
	struct hakios_qrtr_port *port;         /* our port of the modem's QRTR */
	volatile bool loc_there, dms_there;
	uint32_t loc_node, loc_port, loc_version, loc_instance;
	uint32_t dms_node, dms_port, dms_version, dms_instance;
	uint32_t wait_s, run_s, interval_ms;
	uint8_t session_id;
	bool online, online_known;
	uint8_t dms_mode;
	int dms_last_mode;
	bool events_done, start_done, stop_done;
	bool cleanup_request;                   /* a LOC_STOP that must go out even while stopping */
	uint8_t state;                          /* VAYU_GNSS_IDLE..FAILED */
	uint64_t started_ms, first_fix_ms, position_ms;
	uint32_t requests, failures;
	uint32_t fixes, nmea_count, nmea_bad, sv_reports;
	uint32_t last_fix_log_ms;
	uint16_t last_error;                    /* the QMI error of the last refused request */
	/* the transaction in flight: the reply is kept until the waiting thread takes it */
	uint32_t txn_token;
	uint16_t next_transaction, pending_transaction;
	bool pending, txn_done;
	uint8_t reply[VAYU_GNSS_REPLY_MAX];
	size_t reply_length;
	/* the latest fix, merged from the two readers */
	struct vayu_gnss_fix fix;
	uint8_t position_source;
	bool said_position_map, said_sv_map, said_nmea_layout, said_nmea_read, said_bad_checksum;
	bool running, started;
	struct vayu_gnss_record record;
};

static struct vayu_gnss_state gnss;
static volatile bool gnss_stop;

static LCK_GRP_DECLARE(vayu_gnss_lck_grp, "vayu-gnss");
static LCK_MTX_DECLARE(vayu_gnss_lock, &vayu_gnss_lck_grp);     /* the fix and the record */
static LCK_MTX_DECLARE(vayu_gnss_txn_lock, &vayu_gnss_lck_grp); /* the reply in flight */

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

static uint32_t
elapsed_ms(uint64_t since)
{
	uint64_t now = now_ms();

	return now > since ? (uint32_t)(now - since) : 0;
}

/* ---- the record and the log --------------------------------------------------------------------- */

/* "1.2" from hundredths; "?" when the value is not there. */
static void
gnss_hdop_text(char *out, size_t size, uint32_t centi)
{
	if (centi == 0xffffffffu) {
		snprintf(out, size, "?");
	} else {
		snprintf(out, size, "%u.%u", centi / 100u, (centi % 100u) / 10u);
	}
}

/* "156" metres from millimetres. */
static void
gnss_metres_text(char *out, size_t size, int32_t millimetres)
{
	int32_t metres = millimetres >= 0 ? (millimetres + 500) / 1000 : (millimetres - 500) / 1000;

	snprintf(out, size, "%d", (int)metres);
}

/* "9/14" satellites used/in view, each "?" when the engine has not said. */
static void
gnss_sats_text(char *out, size_t size, const struct vayu_gnss_fix *fix)
{
	char used[8] = "?", view[8] = "?";

	if (fix->has_sats_used) {
		snprintf(used, sizeof(used), "%u", (unsigned int)fix->sats_used);
	}
	if (fix->has_sats_view) {
		snprintf(view, sizeof(view), "%u", (unsigned int)fix->sats_view);
	}
	snprintf(out, size, "%s/%s", used, view);
}

/* "12.4" seconds from milliseconds. */
static void
gnss_seconds_text(char *out, size_t size, uint32_t milliseconds)
{
	snprintf(out, size, "%u.%u", milliseconds / 1000u, (milliseconds % 1000u) / 100u);
}

static const char *
gnss_source_text(uint8_t source)
{
	switch (source) {
	case VAYU_GNSS_FROM_QMI: return "position report";
	case VAYU_GNSS_FROM_NMEA: return "NMEA";
	case VAYU_GNSS_FROM_BOTH: return "position report + NMEA";
	default: return "nothing";
	}
}

static uint8_t
gnss_source_bits(uint8_t sources)
{
	bool qmi = (sources & VAYU_GNSS_SOURCE_QMI) != 0;
	bool nmea = (sources & VAYU_GNSS_SOURCE_NMEA) != 0;

	return qmi && nmea ? VAYU_GNSS_FROM_BOTH : qmi ? VAYU_GNSS_FROM_QMI :
	    nmea ? VAYU_GNSS_FROM_NMEA : VAYU_GNSS_FROM_NONE;
}

/*
 * The record, from the state there is; called with the lock held. A position older than
 * VAYU_GNSS_STALE_MS is left in the record but loses its flag and is marked stale: a reader must
 * not be handed a fresh-looking fix the engine stopped saying.
 */
static void
gnss_build_record(uint64_t now, struct vayu_gnss_record *record)
{
	bool fresh = gnss.position_ms != 0 && now - gnss.position_ms <= VAYU_GNSS_STALE_MS;
	uint32_t flags = 0;

	memset(record, 0, sizeof(*record));
	record->magic = VAYU_GNSS_MAGIC;
	record->version = VAYU_GNSS_VERSION;
	record->size = (uint32_t)sizeof(*record);
	record->seq = gnss.fixes;
	record->updated_ms = now;
	record->started_ms = gnss.started_ms;
	record->lat_e7 = gnss.fix.lat_e7;
	record->lon_e7 = gnss.fix.lon_e7;
	record->altitude_mm = gnss.fix.altitude_mm;
	record->hepe_cm = gnss.fix.hepe_cm;
	record->hdop_centi = gnss.fix.has_hdop ? gnss.fix.hdop_centi : 0xffffffffu;
	record->speed_mm_s = gnss.fix.speed_mm_s;
	record->heading_mdeg = gnss.fix.heading_mdeg;
	record->tod_ms = gnss.fix.has_tod ? gnss.fix.tod_ms : 0xffffffffu;
	record->ttff_ms = gnss.first_fix_ms != 0 && gnss.started_ms != 0 ?
	    (uint32_t)(gnss.first_fix_ms - gnss.started_ms) : 0xffffffffu;
	record->fixes = gnss.fixes;
	record->nmea = gnss.nmea_count;
	record->sv_reports = gnss.sv_reports;
	record->sats_used = gnss.fix.has_sats_used ? gnss.fix.sats_used : 0xffff;
	record->sats_view = gnss.fix.has_sats_view ? gnss.fix.sats_view : 0xffff;
	record->source = gnss.position_source;
	record->quality = gnss.fix.quality;
	record->session = gnss.state;
	record->online = gnss.online_known ? (gnss.online ? 0 : gnss.dms_mode) : 0xff;

	if (gnss.fix.position && fresh) {
		flags |= VAYU_GNSS_FLAG_POSITION;
	}
	if (gnss.fix.position && !fresh) {
		flags |= VAYU_GNSS_FLAG_STALE;
	}
	if (gnss.fix.has_altitude) {
		flags |= VAYU_GNSS_FLAG_ALTITUDE;
	}
	if (gnss.fix.has_hepe) {
		flags |= VAYU_GNSS_FLAG_HEPE;
	}
	if (gnss.fix.has_hdop) {
		flags |= VAYU_GNSS_FLAG_HDOP;
	}
	if (gnss.fix.has_speed) {
		flags |= VAYU_GNSS_FLAG_SPEED;
	}
	if (gnss.fix.has_heading) {
		flags |= VAYU_GNSS_FLAG_HEADING;
	}
	if (gnss.fix.has_tod) {
		flags |= VAYU_GNSS_FLAG_TIME_OF_DAY;
	}
	if (gnss.fix.has_sats_used) {
		flags |= VAYU_GNSS_FLAG_SATS_USED;
	}
	if (gnss.fix.has_sats_view) {
		flags |= VAYU_GNSS_FLAG_SATS_VIEW;
	}
	if (gnss.online) {
		flags |= VAYU_GNSS_FLAG_ONLINE;
	}
	if (gnss.events_done) {
		flags |= VAYU_GNSS_FLAG_EVENTS;
	}
	if (gnss.start_done) {
		flags |= VAYU_GNSS_FLAG_STARTED;
	}
	if (gnss.stop_done) {
		flags |= VAYU_GNSS_FLAG_STOPPED;
	}
	if (gnss.nmea_count != 0) {
		flags |= VAYU_GNSS_FLAG_NMEA_SEEN;
	}
	if (gnss.sv_reports != 0) {
		flags |= VAYU_GNSS_FLAG_SV_SEEN;
	}
	record->flags = flags;
}

/* Called with the lock held: the record, as it is now. */
static void
gnss_publish_locked(void)
{
	gnss_build_record(now_ms(), &gnss.record);
}

/*
 * A fix, or a part of one, has arrived: `incoming` holds what this message had, the counters what
 * its reader saw. True when the caller should write a fix line (a new position, at most one a
 * second); the values to log are copied out under the lock into `logged`.
 */
static bool
gnss_merge(const struct vayu_gnss_fix *incoming, uint32_t nmea_read, uint32_t nmea_bad, uint32_t sv_reports,
    struct vayu_gnss_fix *logged)
{
	uint64_t now = now_ms();
	bool log_now = false;

	if (gnss_stop) {
		return false;
	}
	lck_mtx_lock(&vayu_gnss_lock);
	if (incoming->position) {
		gnss.fix.position = true;
		gnss.fix.lat_e7 = incoming->lat_e7;
		gnss.fix.lon_e7 = incoming->lon_e7;
		gnss.position_ms = now;
		gnss.position_source = gnss_source_bits(incoming->source);
		gnss.fixes++;
		if (gnss.first_fix_ms == 0) {
			gnss.first_fix_ms = now;
		}
	}
	if (incoming->has_altitude) {
		gnss.fix.has_altitude = true;
		gnss.fix.altitude_mm = incoming->altitude_mm;
	}
	if (incoming->has_hepe) {
		gnss.fix.has_hepe = true;
		gnss.fix.hepe_cm = incoming->hepe_cm;
	}
	if (incoming->has_hdop) {
		gnss.fix.has_hdop = true;
		gnss.fix.hdop_centi = incoming->hdop_centi;
	}
	if (incoming->has_sats_used) {
		gnss.fix.has_sats_used = true;
		gnss.fix.sats_used = incoming->sats_used;
	}
	if (incoming->has_sats_view) {
		gnss.fix.has_sats_view = true;
		gnss.fix.sats_view = incoming->sats_view;
	}
	if (incoming->has_speed) {
		gnss.fix.has_speed = true;
		gnss.fix.speed_mm_s = incoming->speed_mm_s;
	}
	if (incoming->has_heading) {
		gnss.fix.has_heading = true;
		gnss.fix.heading_mdeg = incoming->heading_mdeg;
	}
	if (incoming->has_tod) {
		gnss.fix.has_tod = true;
		gnss.fix.tod_ms = incoming->tod_ms;
	}
	if (incoming->quality != VAYU_GNSS_QUALITY_UNKNOWN) {
		gnss.fix.quality = incoming->quality;
	}
	gnss.nmea_count += nmea_read;
	gnss.nmea_bad += nmea_bad;
	gnss.sv_reports += sv_reports;
	if (incoming->position && (gnss.fixes == 1 || now - gnss.last_fix_log_ms >= VAYU_GNSS_LOG_MS)) {
		gnss.last_fix_log_ms = (uint32_t)now;
		log_now = true;
		*logged = gnss.fix;
	}
	gnss_publish_locked();
	lck_mtx_unlock(&vayu_gnss_lock);
	return log_now;
}

static void
gnss_log_fix(const struct vayu_gnss_fix *fix, uint32_t fixes, uint32_t ttff)
{
	char lat[12], lon[12], sats[16], hdop[12] = "?";
	char altitude[16] = "?", hepe[16] = "?", ttff_text[16] = "?";
	uint32_t age = gnss.started_ms != 0 ? elapsed_ms(gnss.started_ms) : 0;

	vayu_gnss_coord_text(lat, sizeof(lat), fix->lat_e7, 'N', 'S');
	vayu_gnss_coord_text(lon, sizeof(lon), fix->lon_e7, 'E', 'W');
	gnss_sats_text(sats, sizeof(sats), fix);
	gnss_hdop_text(hdop, sizeof(hdop), fix->has_hdop ? fix->hdop_centi : 0xffffffffu);
	if (fix->has_altitude) {
		gnss_metres_text(altitude, sizeof(altitude), fix->altitude_mm);
	}
	if (fix->has_hepe) {
		gnss_metres_text(hepe, sizeof(hepe), (int32_t)(fix->hepe_cm * 10u));
	}
	if (ttff != 0xffffffffu) {
		gnss_seconds_text(ttff_text, sizeof(ttff_text), ttff);
	}
	say("vayu-gnss: fix %u (+%u.%u s): %s %s, satellites %s, HDOP %s, altitude %s m, error %s m, "
	    "time of day %s, from %s\n", (unsigned int)fixes, age / 1000u, (age % 1000u) / 100u, lat, lon,
	    sats, hdop, altitude, hepe, fix->has_tod ? "known" : "unknown", gnss_source_text(gnss.position_source));
	if (fixes == 1) {
		say("vayu-gnss: time to first fix: %s s (from LOC_START to the first fix with a position)\n",
		    ttff_text);
	}
}

/* ---- the transport: our port of the modem's QRTR, and one transaction at a time -------------------- */

static void
gnss_server(void *context __unused, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port)
{
	if (service == VAYU_GNSS_LOC_SERVICE) {
		if (arrived) {
			gnss.loc_node = node;
			gnss.loc_port = port;
			gnss.loc_version = version;
			gnss.loc_instance = instance;
			gnss.loc_there = true;
		} else if (node == gnss.loc_node && port == gnss.loc_port) {
			gnss.loc_there = false;
		}
	} else if (service == VAYU_GNSS_DMS_SERVICE) {
		if (arrived) {
			gnss.dms_node = node;
			gnss.dms_port = port;
			gnss.dms_version = version;
			gnss.dms_instance = instance;
			gnss.dms_there = true;
		} else if (node == gnss.dms_node && port == gnss.dms_port) {
			gnss.dms_there = false;
			gnss.online = false;
			gnss.online_known = false;
		}
	}
}

static void gnss_message(const void *data, size_t length);

/* A datagram for our port: the reply to the transaction in flight, or an indication. */
static void
gnss_receive(void *context __unused, uint32_t node __unused, uint32_t port __unused, const void *data, size_t length)
{
	const uint8_t *message = data;

	if (length < QMI_HEADER_SIZE) {
		return;
	}
	if (message[0] == QMI_RESPONSE) {
		uint16_t id = vayu_gnss_le16(message + 1);

		lck_mtx_lock(&vayu_gnss_txn_lock);
		if (gnss.pending && id == gnss.pending_transaction) {
			size_t copy = length < sizeof(gnss.reply) ? length : sizeof(gnss.reply);

			memcpy(gnss.reply, message, copy);
			gnss.reply_length = copy;
			gnss.txn_done = true;
			wakeup(&gnss.txn_token);
		}
		lck_mtx_unlock(&vayu_gnss_txn_lock);
		return;
	}
	if (message[0] == QMI_INDICATION) {
		gnss_message(data, length);
	}
}

/*
 * One request and its reply: encoded by the QMI helpers' hakios_qmi_encode(), sent on our port,
 * waited for as long as `timeout_ms`. The reply's Result TLV (0x02) is read here; the reply's bytes
 * stay in gnss.reply for the caller's optional reading of the rest.
 */
static int
gnss_transaction(const char *what, uint32_t node, uint32_t port, uint16_t message, const struct qmi_elem_info *req_ei,
    const void *request, uint32_t timeout_ms)
{
	uint8_t buffer[QMI_HEADER_SIZE + VAYU_GNSS_REQUEST_MAX];
	uint64_t until;
	uint16_t transaction, result = 0xffff, error = 0xffff;
	bool done;
	int length, sent;

	if ((gnss_stop && !gnss.cleanup_request) || gnss.port == NULL) {
		return -ECANCELED;
	}
	gnss.requests++;
	lck_mtx_lock(&vayu_gnss_txn_lock);
	transaction = gnss.next_transaction++;
	gnss.pending = true;
	gnss.pending_transaction = transaction;
	gnss.txn_done = false;
	gnss.reply_length = 0;
	lck_mtx_unlock(&vayu_gnss_txn_lock);
	length = hakios_qmi_encode(QMI_REQUEST, message, transaction, req_ei, request, buffer, sizeof(buffer));
	if (length < QMI_HEADER_SIZE) {
		lck_mtx_lock(&vayu_gnss_txn_lock);
		gnss.pending = false;
		lck_mtx_unlock(&vayu_gnss_txn_lock);
		say("vayu-gnss: %s: cannot encode the request: %d\n", what, length);
		gnss.failures++;
		return length < 0 ? length : -EINVAL;
	}
	sent = vayu_qrtr_ops.send(gnss.port, node, port, buffer, (size_t)length);
	if (sent != 0) {
		lck_mtx_lock(&vayu_gnss_txn_lock);
		gnss.pending = false;
		lck_mtx_unlock(&vayu_gnss_txn_lock);
		say("vayu-gnss: %s: cannot send: %d\n", what, sent);
		gnss.failures++;
		return sent < 0 ? sent : -EIO;
	}

	until = now_ms() + timeout_ms;
	lck_mtx_lock(&vayu_gnss_txn_lock);
	while (!gnss.txn_done && (!gnss_stop || gnss.cleanup_request)) {
		uint64_t now = now_ms();
		struct timespec ts;

		if (now >= until) {
			break;
		}
		ts.tv_sec = (time_t)((until - now) / 1000);
		ts.tv_nsec = (long)((until - now) % 1000) * NSEC_PER_MSEC;
		msleep(&gnss.txn_token, &vayu_gnss_txn_lock, PZERO, "vayu-gnss", &ts);
	}
	done = gnss.txn_done;
	gnss.pending = false;
	lck_mtx_unlock(&vayu_gnss_txn_lock);
	if (!done) {
		say("vayu-gnss: %s: no reply in %u ms\n", what, timeout_ms);
		gnss.failures++;
		return gnss_stop && !gnss.cleanup_request ? -ECANCELED : -ETIMEDOUT;
	}
	if (!vayu_gnss_reply_result(gnss.reply, gnss.reply_length, &result, &error)) {
		say("vayu-gnss: %s: the reply has no result TLV\n", what);
		gnss.failures++;
		return -EBADMSG;
	}
	if (result != QMI_RESULT_SUCCESS_V01 || error != QMI_ERR_NONE_V01) {
		say("vayu-gnss: %s: the modem says result %u error %u\n", what, (unsigned int)result,
		    (unsigned int)error);
		gnss.failures++;
		gnss.last_error = error;
		return -EIO;
	}
	gnss.last_error = 0;
	return 0;
}

/* ---- DMS: is the modem ONLINE? (read only; the ONLINE itself is vayu_cell.c's) --------------------- */

/* 0 when the modem is ONLINE, -1 when it is not or did not answer. */
static int
gnss_dms_round(void)
{
	uint8_t mode = 0xff;
	int ret;

	if (!gnss.dms_there) {
		return -1;
	}
	ret = gnss_transaction("get operating mode", gnss.dms_node, gnss.dms_port, VAYU_GNSS_DMS_GET_OPERATING_MODE,
	    NULL, NULL, VAYU_GNSS_REPLY_MS);
	if (ret != 0 || !vayu_gnss_reply_mode(gnss.reply, gnss.reply_length, &mode)) {
		return -1;
	}
	gnss.dms_mode = mode;
	gnss.online_known = true;
	gnss.online = mode == VAYU_GNSS_DMS_ONLINE;
	if (gnss.dms_last_mode != (int)mode) {
		gnss.dms_last_mode = (int)mode;
		say("vayu-gnss: DMS operating mode %u (%s)\n", (unsigned int)mode,
		    gnss.online ? "ONLINE" : "not online");
	}
	return gnss.online ? 0 : -1;
}

/* ---- what the engine says ---------------------------------------------------------------------- */

/*
 * A message that is not a reply: the three indications this client asked for are read here, from
 * their bytes, by sys/vayu_gnss_qmi.h's readers. The QMI header's length is checked first: a
 * datagram whose body is shorter than it says is not this client's message.
 */
static void
gnss_message(const void *data, size_t length)
{
	const uint8_t *message = data;
	struct vayu_gnss_fix incoming, logged;
	uint16_t id;
	uint32_t nmea_read = 0, nmea_bad = 0, sv = 0, fixes, ttff;
	bool position = false;

	if (length < QMI_HEADER_SIZE || message[0] != QMI_INDICATION) {
		return;
	}
	id = vayu_gnss_le16(message + 3);
	memset(&incoming, 0, sizeof(incoming));
	incoming.quality = VAYU_GNSS_QUALITY_UNKNOWN;

	switch (id) {
	case VAYU_GNSS_LOC_EVENT_POSITION_REPORT:
		position = vayu_gnss_parse_position_report(message, length, &incoming);
		if (!position && !gnss.said_position_map) {
			struct vayu_gnss_tlv_map map;
			char text[96];

			gnss.said_position_map = true;
			if (vayu_gnss_body(message, length, &message, &length)) {
				vayu_gnss_tlv_map(message, length, &map);
				vayu_gnss_tlv_map_text(&map, text, sizeof(text));
				say("vayu-gnss: a position report with no usable position: %u TLV(s): %s "
				    "(types/lengths; the field numbers are sys/vayu_gnss_qmi.h's guess)\n",
				    (unsigned int)map.count, text);
			}
		}
		break;
	case VAYU_GNSS_LOC_EVENT_NMEA:
		position = vayu_gnss_parse_nmea_message(message, length, &incoming, &nmea_read, &nmea_bad);
		if (nmea_read != 0 && !gnss.said_nmea_read) {
			gnss.said_nmea_read = true;
			say("vayu-gnss: NMEA sentences are being read (their contents are not logged: a sentence "
			    "carries the exact position)\n");
		}
		if (nmea_read == 0 && !gnss.said_nmea_layout) {
			struct vayu_gnss_tlv_map map;
			char text[96];

			gnss.said_nmea_layout = true;
			if (vayu_gnss_body(message, length, &message, &length)) {
				vayu_gnss_tlv_map(message, length, &map);
				vayu_gnss_tlv_map_text(&map, text, sizeof(text));
				/* The sentence itself carries the position: the map shows where it would be, and
				 * nothing of it is printed. A TLV of a few dozen bytes is a sentence; one of
				 * hundreds is a list of them. */
				say("vayu-gnss: an NMEA indication with no sentence this client reads: %u TLV(s): %s\n",
				    (unsigned int)map.count, text);
			}
		}
		if (nmea_bad != 0 && !gnss.said_bad_checksum) {
			gnss.said_bad_checksum = true;
			say("vayu-gnss: %u NMEA sentence(s) refused so far (a bad checksum, or not text)\n",
			    (unsigned int)nmea_bad);
		}
		break;
	case VAYU_GNSS_LOC_EVENT_SV_INFO:
		sv = 1;
		if (!gnss.said_sv_map) {
			struct vayu_gnss_tlv_map map;
			char text[96];

			gnss.said_sv_map = true;
			if (vayu_gnss_body(message, length, &message, &length)) {
				vayu_gnss_tlv_map(message, length, &map);
				vayu_gnss_tlv_map_text(&map, text, sizeof(text));
				/* The counts of satellites in view and used come from the sentences (GSV,
				 * GGA/GSA), whose meaning is not a guess; this map is what a later run needs
				 * to read the binary list. */
				say("vayu-gnss: SV-info report: %u TLV(s): %s (the binary list is not decoded yet; "
				    "the counts come from NMEA)\n", (unsigned int)map.count, text);
			}
		}
		break;
	default:
		return;
	}
	if (gnss_merge(&incoming, nmea_read, nmea_bad, sv, &logged)) {
		lck_mtx_lock(&vayu_gnss_lock);
		fixes = gnss.fixes;
		ttff = gnss.first_fix_ms != 0 && gnss.started_ms != 0 ?
		    (uint32_t)(gnss.first_fix_ms - gnss.started_ms) : 0xffffffffu;
		lck_mtx_unlock(&vayu_gnss_lock);
		gnss_log_fix(&logged, fixes, ttff);
	}
}

/* ---- the session ------------------------------------------------------------------------------- */

static int
gnss_register_events(void)
{
	struct vayu_gnss_reg_events_req request = {
		.mask = VAYU_GNSS_EVENT_POSITION | VAYU_GNSS_EVENT_SV_INFO | VAYU_GNSS_EVENT_NMEA,
	};

	say("vayu-gnss: Register Events: position (bit 0), SV info (bit 1), NMEA (bit 2), mask %#x\n",
	    (unsigned int)request.mask);
	return gnss_transaction("register events", gnss.loc_node, gnss.loc_port, VAYU_GNSS_LOC_REG_EVENTS,
	    vayu_gnss_reg_events_req_ei, &request, VAYU_GNSS_REPLY_MS);
}

/*
 * LOC_START, in the three forms sys/vayu_gnss_qmi.h describes. The next form is tried only when
 * the modem says the message itself is wrong (error 1 malformed, 58 encoding): a refusal for any
 * other reason is the modem's answer about the session, and a smaller request would not change it.
 */
static int
gnss_start_session(void)
{
	struct vayu_gnss_start_req full = {
		.session = gnss.session_id,
		.intermediate = 0,              /* only the fixes, not the reports between them */
		.min_interval_ms = gnss.interval_ms,
		.fix_mode = 0,                  /* the engine's default (GNSS) */
	};
	struct vayu_gnss_start_core_req core = {
		.session = gnss.session_id,
		.min_interval_ms = gnss.interval_ms,
	};
	struct vayu_gnss_start_min_req minimal = { .session = gnss.session_id };
	int ret;

	say("vayu-gnss: LOC_START: session %u, minimum interval %u ms\n", (unsigned int)gnss.session_id,
	    (unsigned int)gnss.interval_ms);
	ret = gnss_transaction("start (session, interval, intermediate, fix mode)", gnss.loc_node, gnss.loc_port,
	    VAYU_GNSS_LOC_START, vayu_gnss_start_req_ei, &full, VAYU_GNSS_START_REPLY_MS);
	if (ret == 0) {
		return 0;
	}
	if (ret != -EIO || (gnss.last_error != QMI_ERR_MALFORMED_MSG_V01 && gnss.last_error != QMI_ERR_ENCODING_V01)) {
		return ret;
	}
	say("vayu-gnss: that form was refused as malformed (error %u): LOC_START again with the session and "
	    "the interval only\n", (unsigned int)gnss.last_error);
	ret = gnss_transaction("start (session, interval)", gnss.loc_node, gnss.loc_port, VAYU_GNSS_LOC_START,
	    vayu_gnss_start_core_req_ei, &core, VAYU_GNSS_START_REPLY_MS);
	if (ret == 0) {
		return 0;
	}
	if (ret != -EIO || (gnss.last_error != QMI_ERR_MALFORMED_MSG_V01 && gnss.last_error != QMI_ERR_ENCODING_V01)) {
		return ret;
	}
	say("vayu-gnss: that form too was refused as malformed (error %u): LOC_START with the session alone\n",
	    (unsigned int)gnss.last_error);
	return gnss_transaction("start (session)", gnss.loc_node, gnss.loc_port, VAYU_GNSS_LOC_START,
	    vayu_gnss_start_min_req_ei, &minimal, VAYU_GNSS_START_REPLY_MS);
}

static void
gnss_stop_session(void)
{
	struct vayu_gnss_stop_req request = { .session = gnss.session_id };
	int ret;

	if (!gnss.start_done) {
		return;
	}
	say("vayu-gnss: LOC_STOP: session %u\n", (unsigned int)gnss.session_id);
	gnss.cleanup_request = true;
	ret = gnss_transaction("stop", gnss.loc_node, gnss.loc_port, VAYU_GNSS_LOC_STOP, vayu_gnss_stop_req_ei,
	    &request, VAYU_GNSS_REPLY_MS);
	gnss.cleanup_request = false;
	if (ret == 0) {
		gnss.stop_done = true;
		gnss.state = VAYU_GNSS_STOPPED;
		say("vayu-gnss: the engine stopped the session\n");
	} else {
		say("vayu-gnss: LOC_STOP was not accepted (%d); the session ends with the modem\n", ret);
	}
	lck_mtx_lock(&vayu_gnss_lock);
	gnss_publish_locked();
	lck_mtx_unlock(&vayu_gnss_lock);
}

/* ---- /dev/vayu-gnss ------------------------------------------------------------------------------ */

static int
gnss_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
gnss_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct vayu_gnss_record record;

	if (uio_resid(uio) < (user_ssize_t)sizeof(record)) {
		return EINVAL;                  /* the whole record, or nothing: a part of it says nothing */
	}
	lck_mtx_lock(&vayu_gnss_lock);
	record = gnss.record;
	lck_mtx_unlock(&vayu_gnss_lock);
	return uiomove((caddr_t)&record, sizeof(record), uio);
}

static const struct cdevsw gnss_cdevsw = {
	.d_open = gnss_open, .d_close = gnss_open, .d_read = gnss_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
gnss_make_node(void)
{
	int major = cdevsw_add(-1, &gnss_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, VAYU_GNSS_DEVICE) == NULL) {
		say("vayu-gnss: /dev/%s cannot be made\n", VAYU_GNSS_DEVICE);
		return false;
	}
	say("vayu-gnss: /dev/%s (0666) is made: the record is %u bytes\n", VAYU_GNSS_DEVICE,
	    (unsigned int)sizeof(struct vayu_gnss_record));
	return true;
}

/* ---- the thread ---------------------------------------------------------------------------------- */

/* Waits for the services to appear; true when the LOC service is there. */
static bool
gnss_wait_services(void)
{
	uint64_t waited = 0;

	while ((!gnss.loc_there || !gnss.dms_there) && !gnss_stop && waited < (uint64_t)gnss.wait_s * 1000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited += 100;
	}
	say("vayu-gnss: after %llu ms: LOC is %s, DMS is %s\n", (unsigned long long)waited,
	    gnss.loc_there ? "there" : "MISSING", gnss.dms_there ? "there" : "MISSING");
	if (gnss.loc_there) {
		say("vayu-gnss: LOC: node %u port %u, version %u instance %u\n", gnss.loc_node, gnss.loc_port,
		    gnss.loc_version, gnss.loc_instance);
	}
	if (gnss.dms_there) {
		say("vayu-gnss: DMS: node %u port %u, version %u instance %u (operating mode only)\n",
		    gnss.dms_node, gnss.dms_port, gnss.dms_version, gnss.dms_instance);
	}
	return gnss.loc_there;
}

/*
 * The modem must be ONLINE before the engine is asked to start: the location service answers on a
 * modem that is low-power or shutting down, but nothing comes of a session there, and a start on a
 * modem that is not up is another variable in the run. vayu_cell.c owns the transition
 * (vayu_cell_online=1); this client only waits for it.
 */
static bool
gnss_wait_online(void)
{
	uint64_t waited = 0;

	if (!gnss.dms_there) {
		say("vayu-gnss: DMS is not there: ONLINE cannot be confirmed; the session is tried anyway, "
		    "because the LOC service itself is there\n");
		return true;
	}
	while (!gnss_stop && waited < (uint64_t)gnss.wait_s * 1000) {
		if (gnss_dms_round() == 0) {
			return true;
		}
		delay_for_interval(500, NSEC_PER_MSEC);
		waited += 500;
	}
	if (!gnss.online && !gnss_stop) {
		say("vayu-gnss: the modem is not ONLINE after %llu ms (DMS mode %u): LOC_START is NOT sent; "
		    "vayu_cell_online=1 is what makes it online\n", (unsigned long long)waited,
		    (unsigned int)gnss.dms_mode);
	}
	return false;
}

static void
gnss_set_state(uint8_t state)
{
	gnss.state = state;
	lck_mtx_lock(&vayu_gnss_lock);
	gnss_publish_locked();
	lck_mtx_unlock(&vayu_gnss_lock);
}

static void
gnss_thread(void *argument __unused, wait_result_t wait __unused)
{
	uint64_t progress_ms;
	int ret;

	gnss.port = vayu_qrtr_ops.open(gnss_receive, gnss_server, NULL);
	if (gnss.port == NULL) {
		say("vayu-gnss: no port on the modem's QRTR (vayu_qrtr_detach()?)\n");
		gnss_set_state(VAYU_GNSS_FAILED);
		goto out;
	}
	ret = vayu_qrtr_ops.lookup(gnss.port, VAYU_GNSS_LOC_SERVICE, VAYU_GNSS_LOC_VERSION, 0);
	if (ret != 0) {
		say("vayu-gnss: the LOC service cannot be looked up: %d\n", ret);
		gnss_set_state(VAYU_GNSS_FAILED);
		goto out;
	}
	/* Version 1, instance 0: the instance is the router's wildcard, and the modem announces LOC as
	 * instance 2 (the run logs above), so the server is taken whatever instance it carries. */
	ret = vayu_qrtr_ops.lookup(gnss.port, VAYU_GNSS_DMS_SERVICE, 1, 0);
	if (ret != 0) {
		say("vayu-gnss: the DMS service cannot be looked up: %d\n", ret);
		gnss_set_state(VAYU_GNSS_FAILED);
		goto out;
	}
	if (!gnss_wait_services()) {
		say("vayu-gnss: the LOC service did not appear: nothing is sent\n");
		gnss_set_state(VAYU_GNSS_FAILED);
		goto out;
	}
	if (!gnss_wait_online()) {
		gnss_set_state(VAYU_GNSS_FAILED);
		goto out;
	}
	if (gnss_stop) {
		goto out;
	}
	gnss_set_state(VAYU_GNSS_WAITING);
	if (gnss_register_events() != 0) {
		say("vayu-gnss: the events were not registered: no indication would come, so LOC_START is "
		    "not sent\n");
		gnss_set_state(VAYU_GNSS_FAILED);
		goto out;
	}
	gnss.events_done = true;
	if (gnss_start_session() != 0) {
		say("vayu-gnss: LOC_START was not accepted: no session\n");
		gnss_set_state(VAYU_GNSS_FAILED);
		goto out;
	}
	gnss.start_done = true;
	gnss.started_ms = now_ms();
	gnss_set_state(VAYU_GNSS_RUNNING);
	say("vayu-gnss: the session is on: a fix every %u ms at most, %u s, then LOC_STOP\n",
	    (unsigned int)gnss.interval_ms, (unsigned int)gnss.run_s);

	progress_ms = gnss.started_ms;
	while (!gnss_stop) {
		uint64_t now = now_ms();

		if (now - gnss.started_ms >= (uint64_t)gnss.run_s * 1000) {
			say("vayu-gnss: %u s are up: the session ends\n", (unsigned int)gnss.run_s);
			break;
		}
		if (now - progress_ms >= VAYU_GNSS_PROGRESS_MS) {
			char last[16];

			progress_ms = now;
			lck_mtx_lock(&vayu_gnss_lock);
			if (gnss.position_ms != 0) {
				gnss_seconds_text(last, sizeof(last), elapsed_ms(gnss.position_ms));
			} else {
				snprintf(last, sizeof(last), "never");
			}
			lck_mtx_unlock(&vayu_gnss_lock);
			say("vayu-gnss: +%u s: %u fix(es), %u NMEA sentence(s) (%u refused), %u SV report(s), "
			    "last fix %s s ago\n", (unsigned int)elapsed_ms(gnss.started_ms) / 1000u,
			    (unsigned int)gnss.fixes, (unsigned int)gnss.nmea_count, (unsigned int)gnss.nmea_bad,
			    (unsigned int)gnss.sv_reports, last);
		}
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	gnss_stop_session();
out:
	gnss.online = false;
	if (gnss.port != NULL) {
		vayu_qrtr_ops.close(gnss.port);
		gnss.port = NULL;
	}
	gnss.running = false;
	if (gnss.state != VAYU_GNSS_STOPPED && gnss.state != VAYU_GNSS_FAILED) {
		gnss_set_state(VAYU_GNSS_STOPPED);
	}
	say("vayu-gnss: the client's thread is done: %u request(s), %u failure(s), %u fix(es)%s\n",
	    (unsigned int)gnss.requests, (unsigned int)gnss.failures, (unsigned int)gnss.fixes,
	    gnss.first_fix_ms == 0 ? ", no fix" : "");
	thread_terminate(current_thread());
}

/* ---- start and stop ------------------------------------------------------------------------------ */

void
vayu_gnss_start(struct vayu_qrtr *q)
{
	thread_t thread;
	uint32_t level = 0, wait_s = 30, run_s = 120, interval_ms = 1000;

	if (gnss.started) {
		say("vayu-gnss: already started\n");
		return;
	}
	if (!PE_parse_boot_argn("vayu_gnss", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (q == NULL || q->glink == NULL || !q->glink->up) {
		say("vayu-gnss: vayu_gnss=%u, but the modem's GLINK channel is not up: nothing is done\n", level);
		return;
	}
	if (level != 1) {
		say("vayu-gnss: vayu_gnss=%u is refused: only 1 starts a session\n", level);
	}
	PE_parse_boot_argn("vayu_gnss_wait", &wait_s, sizeof(wait_s));
	if (wait_s < 1) {
		wait_s = 1;
	}
	if (wait_s > 120) {
		wait_s = 120;
	}
	PE_parse_boot_argn("vayu_gnss_run", &run_s, sizeof(run_s));
	if (run_s < 5) {
		run_s = 5;
	}
	if (run_s > 600) {
		run_s = 600;
	}
	PE_parse_boot_argn("vayu_gnss_interval_ms", &interval_ms, sizeof(interval_ms));
	if (interval_ms < 200) {
		interval_ms = 200;
	}
	if (interval_ms > 60000) {
		interval_ms = 60000;
	}
	memset(&gnss, 0, sizeof(gnss));
	gnss.fix.quality = VAYU_GNSS_QUALITY_UNKNOWN;
	gnss.dms_last_mode = -1;
	gnss.dms_mode = 0xff;
	gnss.state = VAYU_GNSS_IDLE;
	gnss.wait_s = wait_s;
	gnss.run_s = run_s;
	gnss.interval_ms = interval_ms;
	gnss.session_id = 0;
	gnss.running = true;
	gnss.started = true;
	gnss_stop = false;
	say("vayu-gnss: vayu_gnss=1 vayu_gnss_wait=%u vayu_gnss_run=%u vayu_gnss_interval_ms=%u\n",
	    wait_s, run_s, interval_ms);
	say("vayu-gnss: LOC 0x%x (register events, start, stop), DMS 0x%x (operating mode only); "
	    "the fix is logged with two decimals, /dev/%s holds all of it\n",
	    VAYU_GNSS_LOC_SERVICE, VAYU_GNSS_DMS_SERVICE, VAYU_GNSS_DEVICE);
	(void)gnss_make_node();
	lck_mtx_lock(&vayu_gnss_lock);
	gnss_publish_locked();
	lck_mtx_unlock(&vayu_gnss_lock);
	if (kernel_thread_start(gnss_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-gnss: no thread\n");
		gnss.running = false;
		gnss.started = false;
		return;
	}
	thread_deallocate(thread);
}

void
vayu_gnss_stop(void)
{
	uint32_t requests, failures, fixes;

	if (!gnss.started) {
		return;
	}
	gnss_stop = true;
	for (uint32_t waited = 0; waited < VAYU_GNSS_STOP_MS && gnss.running; waited += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	requests = gnss.requests;
	failures = gnss.failures;
	fixes = gnss.fixes;
	if (gnss.running) {
		say("vayu-gnss: the client's thread has not finished in %u ms; its record is left as it is\n",
		    VAYU_GNSS_STOP_MS);
	} else {
		gnss.started = false;
		say("vayu-gnss: done: %u request(s), %u failure(s), %u fix(es); /dev/%s still holds the last "
		    "record (%u bytes, seq %u)\n", (unsigned int)requests, (unsigned int)failures,
		    (unsigned int)fixes, VAYU_GNSS_DEVICE, (unsigned int)sizeof(struct vayu_gnss_record),
		    (unsigned int)gnss.record.seq);
	}
}
