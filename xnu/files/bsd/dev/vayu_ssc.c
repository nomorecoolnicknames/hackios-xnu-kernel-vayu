/*
 * The sensors' QMI service (SSC, service 0x190) on the sensors DSP (SLPI) of the POCO X3 Pro
 * (vayu, SM8150-AC): SUID lookups, sensor attributes, and an accelerometer/gyroscope stream.
 *
 * This file only speaks the wire protocol. vayu_slpi.c loads and starts the SLPI, opens GLINK's
 * IPCRTR channel and, when the QRTR service 0x190 is there, calls vayu_ssc_run() with its node
 * and port. Nothing here is sent unless that call is made.
 *
 * Facts and where they come from:
 *
 *   - QMI framing (7-byte little-endian header: type u8, transaction u16, message id u16,
 *     length u16; TLV: type u8, length u16, value) and the Control request 0x0020 whose TLV
 *     0x01 is a u16 length plus a protobuf SscClientRequest and whose TLV 0x10 is one report
 *     type byte (1 = LARGE): libqmi data/qmi-service-ssc.json:24-90, summarized in
 *     the bring-up notes; the same shape appears in libssc's
 *     utils/strace.txt capture.
 *   - Protobuf message shapes, field numbers and wire types: libssc data/ssc-common.proto,
 *     data/ssc-sensor-suid.proto, data/ssc-sensor-accelerometer.proto and
 *     data/ssc-sensor-gyroscope.proto (proto2). The codec below is written here; no libssc code
 *     is copied.
 *   - Message ids 512 (SUID request), 768 (SUID/config answer), 513 (enable config),
 *     514, 10 (disable), 1 (attributes request), 128 (attributes answer) and 1025 (sensor
 *     sample): libssc src/libssc-common-private.h:30-37 and src/libssc-client-private.h:51-54.
 *   - A response to Control 0x0020 carries TLV 0x02 (result u16, error u16), TLV 0x10 (client
 *     id, 8 bytes seen; 4 accepted too) and TLV 0x11 (u32). Report Small 0x0021 and Report
 *     Large 0x0022 carry TLV 0x01 (client id u64) and TLV 0x02 (u16 length plus a protobuf
 *     SscClientResponse).
 *   - The SUID pseudo-sensor's uid is low = high = 0xabababababababab.
 *   - The accel/gyro payloads are repeated IEEE-754 floats: acceleration in m/s^2, gyro
 *     velocity in whatever unit the DSP uses (libssc's comment says m/s; rad/s is common for
 *     a gyroscope). This kernel must not handle them as C floats: every float here is a
 *     uint32_t of its bits and f32_milli() converts to thousandths with integer arithmetic only.
 *
 * HYPOTHESES, not facts yet:
 *
 *   - The body of msg_id 513 is SscEnableConfigRequest{sample_rate float = 1}; it has not been
 *     seen on this hardware yet.
 *   - Whether physical sensors show up without the FastRPC registry is unknown. If the
 *     "registry" SUID lookup comes back empty, H-noreg (the FastRPC file server needed by the
 *     SLPI's sensor registry) is the likely reason, but sensors may still answer directly.
 *   - The SLPI may answer 768 to 513 with SscConfigResponse{sample_rate = 1, mode = 5}; the
 *     fields are taken from libssc's .proto, not from a capture here.
 *
 * Boot-arg: vayu_ssc_hz=N requests N Hz (integer, default 50, clamped to 1..400). It is turned
 * into IEEE-754 bits by u32_to_f32_bits(); no floating point is used anywhere in this file.
 *
 * All buffers are static and at most 8 KB; kernel stacks are 16 KB, so nothing large is put on
 * one. Decoding happens on GLINK's thread: it only records and prints, and the run thread waits
 * by polling every 10 ms with explicit timeouts.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/hakios.qrtr.h>
#include <sys/vayu_ssc.h>

extern void loader_log_putc(char c);
extern void IOSleep(unsigned int);

/* QMI message types. */
#define SSC_QMI_REQUEST		0
#define SSC_QMI_RESPONSE	2
#define SSC_QMI_INDICATION	4

/* QMI message ids on the Control service and the two Report indications. */
#define SSC_MSG_CONTROL		0x0020
#define SSC_MSG_REPORT_SMALL	0x0021
#define SSC_MSG_REPORT_LARGE	0x0022

/* Sensor Core message ids (libssc src/libssc-common-private.h:30-37). */
#define SSC_MSG_SUID_REQUEST	512
#define SSC_MSG_SUID_RESPONSE	768
#define SSC_MSG_ENABLE_CONFIG	513
#define SSC_MSG_514		514
#define SSC_MSG_CONFIG_RESPONSE	768
#define SSC_MSG_DISABLE		10
#define SSC_MSG_ATTR_REQUEST	1
#define SSC_MSG_ATTR_RESPONSE	128
#define SSC_MSG_SENSOR_SAMPLE	1025

/* The SUID pseudo-sensor: low = high = 0xabababababababab. */
#define SSC_SUID_LOW		0xababababababababULL
#define SSC_SUID_HIGH		0xababababababababULL

/* Our QRTR client port (the user picks a number >= 0x4000). */

#define SSC_TXN_SLOTS		16
#define SSC_REPLY_SLOTS		16
#define SSC_MAX_UIDS		16
#define SSC_UNKNOWN_MSGS	8
#define SSC_ATTR_STRS		4
#define SSC_ATTR_STR		48
#define SSC_ATTR_FLOATS		9
#define SSC_ATTR_INTS		4

/*
 * The data types asked for, in the order they are asked for. The registry is the SUID pseudo
 * sensor itself; the rest are real sensor data types.
 */
enum ssc_type {
	SSC_REGISTRY,
	SSC_ACCEL,
	SSC_GYRO,
	SSC_MAG,
	SSC_LIGHT,
	SSC_PROXIMITY,
	SSC_TEMPERATURE,
	SSC_TYPES
};

static const char *const ssc_type_name[SSC_TYPES] = {
	"registry",
	"accel",
	"gyro",
	"mag",
	"ambient_light",
	"proximity",
	"sensor_temperature",
};

struct ssc_suid_slot {
	uint32_t seq;			/* bumped for every answer; the run thread waits on it */
	uint32_t total;			/* how many uids the DSP returned */
	uint32_t stored;		/* how many fit in low[]/high[] */
	uint64_t low[SSC_MAX_UIDS];
	uint64_t high[SSC_MAX_UIDS];
};

struct ssc_state {
	uint32_t responses;
	uint32_t indications;
	uint32_t large;
	uint32_t small;
	uint32_t undecodable;
	uint32_t suid_answers;
	uint32_t unknown_uids;

	uint16_t next_txn;
	uint16_t txn_msg[SSC_TXN_SLOTS];
	uint16_t txn_num[SSC_TXN_SLOTS];
	bool txn_used[SSC_TXN_SLOTS];
	unsigned txn_next;

	uint16_t reply_txn[SSC_REPLY_SLOTS];
	uint16_t reply_result[SSC_REPLY_SLOTS];
	uint16_t reply_error[SSC_REPLY_SLOTS];
	bool reply_used[SSC_REPLY_SLOTS];
	unsigned reply_next;

	struct ssc_suid_slot suid[SSC_TYPES];

	struct {
		uint32_t seq;
		uint64_t low, high;
		uint32_t rate;
		bool has_rate;
		char mode[32];
		bool has_mode;
	} config;

	struct {
		bool waiting;
		uint32_t seq;
		uint64_t low, high;
	} attr;

	struct {
		int type;		/* ssc_type or -1 */
		uint32_t count;
		bool has;
		int32_t last[3];
		int32_t accuracy;
		uint64_t timestamp;
	} stream;

	int32_t accel_mm[3];
	uint64_t accel_ns;
	bool accel_valid;

	uint32_t unknown_msg[SSC_UNKNOWN_MSGS];
	unsigned unknown_msg_count;
};

static lck_grp_t *ssc_lock_group;
static lck_spin_t ssc_lock;
static bool ssc_lock_ready;
static struct ssc_state ssc;
static volatile bool ssc_leaving;       /* vayu_ssc_quiesce(): level 6's endless stream ends */
static uint32_t accel_seq;              /* accelerometer samples so far (under ssc_lock), for /dev/vayu-motion */

/* Static protocol buffers: the run thread owns all of them. */
static uint8_t enc_uid[32];
static uint8_t enc_cfg[16];
static uint8_t enc_leaf[512];
static uint8_t enc_body[768];
static uint8_t enc_req[1024];
static uint8_t enc_qmi[2048];

/* Every line to the loader's log as well: what is done to SSC should be known even if the kernel dies. */
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
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		printf("%s", line);
	}
	return length;
}

static bool
ssc_lock_init(void)
{
	if (ssc_lock_ready) {
		return true;
	}
	ssc_lock_group = lck_grp_alloc_init("vayu-ssc", LCK_GRP_ATTR_NULL);
	if (ssc_lock_group == NULL) {
		return false;
	}
	lck_spin_init(&ssc_lock, ssc_lock_group, LCK_ATTR_NULL);
	ssc_lock_ready = true;
	return true;
}

static void
ssc_reset(void)
{
	lck_spin_lock(&ssc_lock);
	memset(&ssc, 0, sizeof(ssc));
	ssc.next_txn = 1;
	ssc.stream.type = -1;
	lck_spin_unlock(&ssc_lock);
}

static void
ssc_bad(void)
{
	lck_spin_lock(&ssc_lock);
	ssc.undecodable++;
	lck_spin_unlock(&ssc_lock);
}

/* Nanoseconds from the same monotonic clock as mach_absolute_time(). */
static uint64_t
ssc_now_ns(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

/*
 * IEEE-754 binary32 bits to thousandths, rounded toward zero, by integer arithmetic only.
 * The result is clamped to +/-2,000,000,000; NaN and infinity return INT32_MIN, which the
 * printers show as "nan".
 */
static int32_t
f32_milli(uint32_t bits)
{
	uint32_t exp = (bits >> 23) & 0xff;
	uint32_t mant = bits & 0x7fffff;
	bool neg = (bits >> 31) != 0;
	uint64_t v;
	int shift;

	if (exp == 0xff) {
		return INT32_MIN;		/* NaN or infinity */
	}
	if (exp == 0) {
		return 0;			/* subnormal: less than one thousandth */
	}
	v = ((uint64_t)mant + 0x800000ULL) * 1000ULL;
	shift = (int)exp - 150;			/* value * 1000 = v * 2^shift */
	if (shift >= 0) {
		if (shift > 30) {
			v = 2000000000ULL;
		} else {
			v <<= shift;
			if (v > 2000000000ULL) {
				v = 2000000000ULL;
			}
		}
	} else {
		unsigned right = (unsigned)(-shift);

		if (right >= 64) {
			v = 0;
		} else {
			v >>= right;
		}
	}
	if (v > 2000000000ULL) {
		v = 2000000000ULL;
	}
	if (neg && v != 0) {
		return -(int32_t)v;
	}
	return (int32_t)v;
}

/* A non-negative integer as IEEE-754 binary32 bits (used for the requested sample rate). */
static uint32_t
u32_to_f32_bits(uint32_t v)
{
	unsigned e = 0;
	uint32_t m;

	if (v == 0) {
		return 0;
	}
	while (e < 31 && (v >> (e + 1)) != 0) {
		e++;
	}
	if (e <= 23) {
		m = (v << (23 - e)) & 0x7fffff;
	} else {
		m = (v >> (e - 23)) & 0x7fffff;
	}
	return ((uint32_t)(e + 127) << 23) | m;
}

static void
fmt_milli(char *to, size_t size, int32_t v)
{
	uint32_t a;

	if (v == INT32_MIN) {
		snprintf(to, size, "nan");
		return;
	}
	if (v < 0) {
		a = (uint32_t)(-(v + 1)) + 1;
		snprintf(to, size, "-%d.%03u", (int)(a / 1000), a % 1000);
	} else {
		a = (uint32_t)v;
		snprintf(to, size, "%d.%03u", (int)(a / 1000), a % 1000);
	}
}

/* --------------------------------------------------------------------------------------- */
/* Protobuf encoder: only what the requests below need.                                    */
/* --------------------------------------------------------------------------------------- */

struct pbuf {
	uint8_t *data;
	uint32_t cap;
	uint32_t len;
	bool bad;
};

static void
pb_init(struct pbuf *b, uint8_t *data, uint32_t cap)
{
	b->data = data;
	b->cap = cap;
	b->len = 0;
	b->bad = false;
}

static void
pb_raw(struct pbuf *b, const void *p, uint32_t n)
{
	if (b->bad) {
		return;
	}
	if (n > b->cap - b->len) {
		b->bad = true;
		return;
	}
	if (n != 0) {
		memcpy(b->data + b->len, p, n);
	}
	b->len += n;
}

static void
pb_varint(struct pbuf *b, uint64_t v)
{
	uint8_t tmp[10];
	unsigned n = 0;

	do {
		tmp[n] = (uint8_t)(v & 0x7f);
		v >>= 7;
		if (v != 0) {
			tmp[n] |= 0x80;
		}
		n++;
	} while (v != 0);
	pb_raw(b, tmp, n);
}

static void
pb_tag(struct pbuf *b, uint32_t field, unsigned wire)
{
	pb_varint(b, ((uint64_t)field << 3) | wire);
}

static void
pb_uint(struct pbuf *b, uint32_t field, uint64_t v)
{
	pb_tag(b, field, 0);
	pb_varint(b, v);
}

static void
pb_fixed32(struct pbuf *b, uint32_t field, uint32_t v)
{
	uint8_t t[4];

	pb_tag(b, field, 5);
	t[0] = (uint8_t)(v & 0xff);
	t[1] = (uint8_t)((v >> 8) & 0xff);
	t[2] = (uint8_t)((v >> 16) & 0xff);
	t[3] = (uint8_t)((v >> 24) & 0xff);
	pb_raw(b, t, sizeof(t));
}

static void
pb_fixed64(struct pbuf *b, uint32_t field, uint64_t v)
{
	uint8_t t[8];

	pb_tag(b, field, 1);
	t[0] = (uint8_t)(v & 0xff);
	t[1] = (uint8_t)((v >> 8) & 0xff);
	t[2] = (uint8_t)((v >> 16) & 0xff);
	t[3] = (uint8_t)((v >> 24) & 0xff);
	t[4] = (uint8_t)((v >> 32) & 0xff);
	t[5] = (uint8_t)((v >> 40) & 0xff);
	t[6] = (uint8_t)((v >> 48) & 0xff);
	t[7] = (uint8_t)((v >> 56) & 0xff);
	pb_raw(b, t, sizeof(t));
}

static void
pb_bytes(struct pbuf *b, uint32_t field, const void *p, uint32_t n)
{
	pb_tag(b, field, 2);
	pb_varint(b, n);
	if (n != 0) {
		pb_raw(b, p, n);
	}
}

static void
pb_string(struct pbuf *b, uint32_t field, const char *s)
{
	pb_bytes(b, field, s, (uint32_t)strlen(s));
}

/* --------------------------------------------------------------------------------------- */
/* Protobuf decoder: wire types 0, 1, 2 and 5; unknown fields are skipped.                 */
/* --------------------------------------------------------------------------------------- */

struct dbuf {
	const uint8_t *data;
	uint32_t len;
	uint32_t off;
	bool bad;
};

static void
db_init(struct dbuf *d, const uint8_t *data, uint32_t len)
{
	d->data = data;
	d->len = len;
	d->off = 0;
	d->bad = false;
}

static bool
db_varint(struct dbuf *d, uint64_t *v)
{
	uint64_t r = 0;
	unsigned shift = 0;

	while (shift < 64) {
		uint8_t c;

		if (d->off >= d->len) {
			d->bad = true;
			return false;
		}
		c = d->data[d->off++];
		r |= (uint64_t)(c & 0x7f) << shift;
		if ((c & 0x80) == 0) {
			*v = r;
			return true;
		}
		shift += 7;
	}
	d->bad = true;
	return false;
}

static bool
db_tag(struct dbuf *d, uint32_t *field, unsigned *wire)
{
	uint64_t t;

	if (!db_varint(d, &t)) {
		return false;
	}
	if ((t >> 3) == 0 || (t >> 3) > 0x1fffffffULL) {
		d->bad = true;
		return false;
	}
	*field = (uint32_t)(t >> 3);
	*wire = (unsigned)(t & 7);
	return true;
}

static bool
db_fixed32(struct dbuf *d, uint32_t *v)
{
	if (d->len - d->off < 4) {
		d->bad = true;
		return false;
	}
	*v = (uint32_t)d->data[d->off] |
	    ((uint32_t)d->data[d->off + 1] << 8) |
	    ((uint32_t)d->data[d->off + 2] << 16) |
	    ((uint32_t)d->data[d->off + 3] << 24);
	d->off += 4;
	return true;
}

static bool
db_fixed64(struct dbuf *d, uint64_t *v)
{
	uint64_t lo, hi;

	if (d->len - d->off < 8) {
		d->bad = true;
		return false;
	}
	lo = (uint32_t)d->data[d->off] |
	    ((uint32_t)d->data[d->off + 1] << 8) |
	    ((uint32_t)d->data[d->off + 2] << 16) |
	    ((uint32_t)d->data[d->off + 3] << 24);
	hi = (uint32_t)d->data[d->off + 4] |
	    ((uint32_t)d->data[d->off + 5] << 8) |
	    ((uint32_t)d->data[d->off + 6] << 16) |
	    ((uint32_t)d->data[d->off + 7] << 24);
	d->off += 8;
	*v = lo | (hi << 32);
	return true;
}

static bool
db_bytes(struct dbuf *d, const uint8_t **p, uint32_t *n)
{
	uint64_t v;

	if (!db_varint(d, &v)) {
		return false;
	}
	if (v > 0xffffffffULL || v > d->len - d->off) {
		d->bad = true;
		return false;
	}
	*p = d->data + d->off;
	*n = (uint32_t)v;
	d->off += (uint32_t)v;
	return true;
}

static bool
db_skip(struct dbuf *d, unsigned wire)
{
	uint64_t v;
	const uint8_t *p;
	uint32_t n;

	switch (wire) {
	case 0:
		return db_varint(d, &v);
	case 1:
		if (d->len - d->off < 8) {
			d->bad = true;
			return false;
		}
		d->off += 8;
		return true;
	case 2:
		return db_bytes(d, &p, &n);
	case 5:
		if (d->len - d->off < 4) {
			d->bad = true;
			return false;
		}
		d->off += 4;
		return true;
	}
	d->bad = true;
	return false;
}

/* A packed or unpacked repeated float field. */
static bool
db_float_list(struct dbuf *d, unsigned wire, uint32_t *out, unsigned max, unsigned *count)
{
	if (wire == 5) {
		uint32_t v;

		if (!db_fixed32(d, &v)) {
			return false;
		}
		if (*count < max) {
			out[*count] = v;
		}
		(*count)++;
		return true;
	}
	if (wire == 2) {
		const uint8_t *p;
		uint32_t n;

		if (!db_bytes(d, &p, &n)) {
			return false;
		}
		if ((n & 3) != 0) {
			d->bad = true;
			return false;
		}
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t v = (uint32_t)p[i] |
			    ((uint32_t)p[i + 1] << 8) |
			    ((uint32_t)p[i + 2] << 16) |
			    ((uint32_t)p[i + 3] << 24);

			if (*count < max) {
				out[*count] = v;
			}
			(*count)++;
		}
		return true;
	}
	return db_skip(d, wire);
}

/* SscUid{low fixed64 = 1; high fixed64 = 2}. */
static bool
uid_parse(const uint8_t *p, uint32_t len, uint64_t *low, uint64_t *high)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;

	*low = 0;
	*high = 0;
	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			return false;
		}
		if (field == 1 && wire == 1) {
			if (!db_fixed64(&d, low)) {
				return false;
			}
		} else if (field == 2 && wire == 1) {
			if (!db_fixed64(&d, high)) {
				return false;
			}
		} else if (!db_skip(&d, wire)) {
			return false;
		}
	}
	return !d.bad;
}

/* --------------------------------------------------------------------------------------- */
/* Names and attributes                                                                    */
/* --------------------------------------------------------------------------------------- */

static int
ssc_type_index(const char *s, uint32_t len)
{
	unsigned i;

	for (i = 0; i < SSC_TYPES; i++) {
		size_t n = strlen(ssc_type_name[i]);

		if (n == len && memcmp(ssc_type_name[i], s, len) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* Called with ssc_lock held. */
static int
ssc_type_of_uid(uint64_t low, uint64_t high)
{
	unsigned t, i;

	for (t = 0; t < SSC_TYPES; t++) {
		for (i = 0; i < ssc.suid[t].stored; i++) {
			if (ssc.suid[t].low[i] == low && ssc.suid[t].high[i] == high) {
				return (int)t;
			}
		}
	}
	return -1;
}

struct ssc_attr_values {
	unsigned count;
	unsigned nstr;
	char str[SSC_ATTR_STRS][SSC_ATTR_STR];
	unsigned nf;
	uint32_t f[SSC_ATTR_FLOATS];
	unsigned ni;
	uint64_t i[SSC_ATTR_INTS];
};

static bool attr_value(struct dbuf *d, struct ssc_attr_values *v, unsigned depth);

/* SscAttrValueArray{element SscAttrValue = 1}. */
static bool
attr_value_array(struct dbuf *d, struct ssc_attr_values *v, unsigned depth)
{
	uint32_t field;
	unsigned wire;

	while (d->off < d->len) {
		if (!db_tag(d, &field, &wire)) {
			return false;
		}
		if (field == 1 && wire == 2) {
			const uint8_t *p;
			uint32_t n;
			struct dbuf s;

			if (!db_bytes(d, &p, &n)) {
				return false;
			}
			db_init(&s, p, n);
			if (!attr_value(&s, v, depth + 1)) {
				return false;
			}
		} else if (!db_skip(d, wire)) {
			return false;
		}
	}
	return !d->bad;
}

/* SscAttrValue{a = 1; s = 2; f = 3; i = 4; b = 5}. */
static bool
attr_value(struct dbuf *d, struct ssc_attr_values *v, unsigned depth)
{
	uint32_t field;
	unsigned wire;

	if (depth > 8) {
		d->bad = true;
		return false;
	}
	while (d->off < d->len) {
		if (!db_tag(d, &field, &wire)) {
			return false;
		}
		if (field == 1 && wire == 2) {
			const uint8_t *p;
			uint32_t n;
			struct dbuf s;

			if (!db_bytes(d, &p, &n)) {
				return false;
			}
			db_init(&s, p, n);
			if (!attr_value_array(&s, v, depth + 1)) {
				return false;
			}
		} else if (field == 2 && wire == 2) {
			const uint8_t *p;
			uint32_t n;

			if (!db_bytes(d, &p, &n)) {
				return false;
			}
			if (v->nstr < SSC_ATTR_STRS) {
				uint32_t c = n < SSC_ATTR_STR - 1 ? n : SSC_ATTR_STR - 1;

				memcpy(v->str[v->nstr], p, c);
				v->str[v->nstr][c] = 0;
				v->nstr++;
			}
			v->count++;
		} else if (field == 3 && wire == 5) {
			uint32_t x;

			if (!db_fixed32(d, &x)) {
				return false;
			}
			if (v->nf < SSC_ATTR_FLOATS) {
				v->f[v->nf++] = x;
			}
			v->count++;
		} else if (field == 4 && wire == 1) {
			uint64_t x;

			if (!db_fixed64(d, &x)) {
				return false;
			}
			if (v->ni < SSC_ATTR_INTS) {
				v->i[v->ni++] = x;
			}
			v->count++;
		} else if (field == 5 && wire == 0) {
			uint64_t x;

			if (!db_varint(d, &x)) {
				return false;
			}
			if (v->ni < SSC_ATTR_INTS) {
				v->i[v->ni++] = x;
			}
			v->count++;
		} else if (!db_skip(d, wire)) {
			return false;
		}
	}
	return !d->bad;
}

/* Attribute ids from libssc data/ssc-common.proto (SscAttr.id is an enum in the DSP). */
static void
attr_print(int32_t id, const struct ssc_attr_values *v)
{
	unsigned i;
	char m[16];

	switch (id) {
	case 0:
		if (v->nstr != 0) {
			say("vayu-ssc: attr 0 NAME: \"%s\"\n", v->str[0]);
		} else {
			say("vayu-ssc: attr 0 NAME: (none)\n");
		}
		break;
	case 1:
		if (v->nstr != 0) {
			say("vayu-ssc: attr 1 VENDOR: \"%s\"\n", v->str[0]);
		} else {
			say("vayu-ssc: attr 1 VENDOR: (none)\n");
		}
		break;
	case 6:
		say("vayu-ssc: attr 6 SAMPLE_RATE:");
		for (i = 0; i < v->nf && i < 8; i++) {
			fmt_milli(m, sizeof(m), f32_milli(v->f[i]));
			say(" %s", m);
		}
		say("\n");
		break;
	case 16:
		if (v->ni != 0) {
			say("vayu-ssc: attr 16 STREAM_TYPE: %lld\n", (long long)v->i[0]);
		} else {
			say("vayu-ssc: attr 16 STREAM_TYPE: (none)\n");
		}
		break;
	case 18:
		if (v->ni != 0) {
			say("vayu-ssc: attr 18 HARDWARE_ID: %llu\n", (unsigned long long)v->i[0]);
		} else {
			say("vayu-ssc: attr 18 HARDWARE_ID: (none)\n");
		}
		break;
	case 20:
		say("vayu-ssc: attr 20 MOUNT_MATRIX:");
		for (i = 0; i < v->nf && i < 9; i++) {
			fmt_milli(m, sizeof(m), f32_milli(v->f[i]));
			say(" %s", m);
		}
		say("\n");
		break;
	default:
		say("vayu-ssc: attr %d: %u values\n", id, v->count);
		break;
	}
}

/* SscAttrResponse{attr SscAttr = 1}; SscAttr{id int32 = 1; value_array = 2}. */
static void
ssc_attr_response(const uint8_t *p, uint32_t len)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;

	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			ssc_bad();
			return;
		}
		if (field == 1 && wire == 2) {
			const uint8_t *q;
			uint32_t n;
			struct dbuf a;
			struct ssc_attr_values v;
			int32_t id = 0;

			if (!db_bytes(&d, &q, &n)) {
				ssc_bad();
				return;
			}
			memset(&v, 0, sizeof(v));
			db_init(&a, q, n);
			while (a.off < a.len) {
				uint32_t af;
				unsigned aw;

				if (!db_tag(&a, &af, &aw)) {
					ssc_bad();
					return;
				}
				if (af == 1 && aw == 0) {
					uint64_t x;

					if (!db_varint(&a, &x)) {
						ssc_bad();
						return;
					}
					id = (int32_t)x;
				} else if (af == 2 && aw == 2) {
					const uint8_t *q2;
					uint32_t n2;
					struct dbuf arr;

					if (!db_bytes(&a, &q2, &n2)) {
						ssc_bad();
						return;
					}
					db_init(&arr, q2, n2);
					if (!attr_value_array(&arr, &v, 0)) {
						ssc_bad();
						return;
					}
				} else if (!db_skip(&a, aw)) {
					ssc_bad();
					return;
				}
			}
			attr_print(id, &v);
		} else if (!db_skip(&d, wire)) {
			ssc_bad();
			return;
		}
	}
}

/* --------------------------------------------------------------------------------------- */
/* Sensor Core answers                                                                     */
/* --------------------------------------------------------------------------------------- */

/* SscSuidResponse{data_type string = 1; uid SscUid = 2}. */
static void
ssc_suid_response(const uint8_t *p, uint32_t len)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;
	const uint8_t *name = NULL;
	uint32_t name_len = 0;
	uint64_t low[SSC_MAX_UIDS];
	uint64_t high[SSC_MAX_UIDS];
	uint32_t total = 0;
	uint32_t stored;
	int type;

	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			ssc_bad();
			return;
		}
		if (field == 1 && wire == 2) {
			if (!db_bytes(&d, &name, &name_len)) {
				ssc_bad();
				return;
			}
		} else if (field == 2 && wire == 2) {
			const uint8_t *q;
			uint32_t n;
			uint64_t lo, hi;

			if (!db_bytes(&d, &q, &n) || !uid_parse(q, n, &lo, &hi)) {
				ssc_bad();
				return;
			}
			if (total < SSC_MAX_UIDS) {
				low[total] = lo;
				high[total] = hi;
			}
			total++;
		} else if (!db_skip(&d, wire)) {
			ssc_bad();
			return;
		}
	}
	if (name == NULL) {
		ssc_bad();
		return;
	}
	type = ssc_type_index((const char *)name, name_len);
	if (type < 0) {
		say("vayu-ssc: suid answer for unknown data type \"%.*s\"\n",
		    (int)(name_len > 31 ? 31 : name_len), (const char *)name);
		return;
	}
	stored = total < SSC_MAX_UIDS ? total : SSC_MAX_UIDS;
	lck_spin_lock(&ssc_lock);
	ssc.suid[type].seq++;
	ssc.suid[type].total = total;
	ssc.suid[type].stored = stored;
	for (uint32_t i = 0; i < stored; i++) {
		ssc.suid[type].low[i] = low[i];
		ssc.suid[type].high[i] = high[i];
	}
	lck_spin_unlock(&ssc_lock);
}

/* SscConfigResponse{sample_rate float = 1; mode string = 5}. */
static void
ssc_config_response(uint64_t low, uint64_t high, const uint8_t *p, uint32_t len)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;
	uint32_t rate = 0;
	bool has_rate = false;
	char mode[32];
	bool has_mode = false;

	memset(mode, 0, sizeof(mode));
	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			ssc_bad();
			return;
		}
		if (field == 1 && wire == 5) {
			if (!db_fixed32(&d, &rate)) {
				ssc_bad();
				return;
			}
			has_rate = true;
		} else if (field == 5 && wire == 2) {
			const uint8_t *q;
			uint32_t n;
			uint32_t c;

			if (!db_bytes(&d, &q, &n)) {
				ssc_bad();
				return;
			}
			c = n < sizeof(mode) - 1 ? n : sizeof(mode) - 1;
			memcpy(mode, q, c);
			mode[c] = 0;
			has_mode = true;
		} else if (!db_skip(&d, wire)) {
			ssc_bad();
			return;
		}
	}
	lck_spin_lock(&ssc_lock);
	ssc.config.seq++;
	ssc.config.low = low;
	ssc.config.high = high;
	ssc.config.rate = rate;
	ssc.config.has_rate = has_rate;
	memcpy(ssc.config.mode, mode, sizeof(mode));
	ssc.config.has_mode = has_mode;
	lck_spin_unlock(&ssc_lock);
}

/* SscAccelerometerResponse / SscGyroscopeResponse: repeated float = 1, accuracy int32 = 2. */
static void
ssc_sample_response(int type, const uint8_t *p, uint32_t len, uint64_t timestamp)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;
	uint32_t bits[3] = { 0, 0, 0 };
	unsigned nf = 0;
	int32_t accuracy = 0;
	int32_t mm[3];
	uint32_t no;
	uint64_t arrived;

	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			ssc_bad();
			return;
		}
		if (field == 1) {
			if (!db_float_list(&d, wire, bits, 3, &nf)) {
				ssc_bad();
				return;
			}
		} else if (field == 2 && wire == 0) {
			uint64_t x;

			if (!db_varint(&d, &x)) {
				ssc_bad();
				return;
			}
			accuracy = (int32_t)x;
		} else if (!db_skip(&d, wire)) {
			ssc_bad();
			return;
		}
	}
	if (nf < 3) {
		ssc_bad();
		return;
	}
	mm[0] = f32_milli(bits[0]);
	mm[1] = f32_milli(bits[1]);
	mm[2] = f32_milli(bits[2]);
	arrived = ssc_now_ns();

	lck_spin_lock(&ssc_lock);
	if (ssc.stream.type != type) {
		lck_spin_unlock(&ssc_lock);
		return;
	}
	ssc.stream.count++;
	ssc.stream.has = true;
	ssc.stream.last[0] = mm[0];
	ssc.stream.last[1] = mm[1];
	ssc.stream.last[2] = mm[2];
	ssc.stream.accuracy = accuracy;
	ssc.stream.timestamp = timestamp;
	no = ssc.stream.count;
	if (type == SSC_ACCEL) {
		ssc.accel_mm[0] = mm[0];
		ssc.accel_mm[1] = mm[1];
		ssc.accel_mm[2] = mm[2];
		ssc.accel_ns = arrived;
		ssc.accel_valid = true;
		accel_seq++;
	}
	lck_spin_unlock(&ssc_lock);

	if (no <= 10) {
		char x[16], y[16], z[16];

		fmt_milli(x, sizeof(x), mm[0]);
		fmt_milli(y, sizeof(y), mm[1]);
		fmt_milli(z, sizeof(z), mm[2]);
		say("vayu-ssc: %s #%u: %s %s %s (%s), accuracy %d, t %llu\n",
		    ssc_type_name[type], no, x, y, z,
		    type == SSC_ACCEL ? "m/s^2" : "m/s", accuracy,
		    (unsigned long long)timestamp);
	}
}

static bool
ssc_note_unknown_msg(uint32_t msg_id)
{
	bool fresh = false;
	unsigned i;

	lck_spin_lock(&ssc_lock);
	for (i = 0; i < ssc.unknown_msg_count; i++) {
		if (ssc.unknown_msg[i] == msg_id) {
			break;
		}
	}
	if (i == ssc.unknown_msg_count && ssc.unknown_msg_count < SSC_UNKNOWN_MSGS) {
		ssc.unknown_msg[ssc.unknown_msg_count++] = msg_id;
		fresh = true;
	}
	lck_spin_unlock(&ssc_lock);
	return fresh;
}

static void
ssc_unknown_uid(uint64_t low, uint64_t high, uint32_t msg_id, uint32_t msg_len)
{
	bool print;
	uint32_t n;

	lck_spin_lock(&ssc_lock);
	ssc.unknown_uids++;
	n = ssc.unknown_uids;
	print = n <= 4;
	lck_spin_unlock(&ssc_lock);
	if (print) {
		say("vayu-ssc: indication from unknown uid %016llx%016llx: msg %u, %u bytes\n",
		    (unsigned long long)high, (unsigned long long)low, msg_id, msg_len);
	}
}

static bool
ssc_msg_known(uint32_t msg_id)
{
	return msg_id == SSC_MSG_SUID_RESPONSE ||
	    msg_id == SSC_MSG_ATTR_RESPONSE ||
	    msg_id == SSC_MSG_DISABLE ||
	    msg_id == SSC_MSG_514 ||
	    msg_id == SSC_MSG_SENSOR_SAMPLE;
}

/* SscClientResponseBody{msg_id fixed32 = 1; timestamp fixed64 = 2; msg bytes = 3}. */
static void
ssc_handle_body(uint64_t low, uint64_t high, const uint8_t *p, uint32_t len)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;
	uint32_t msg_id = 0;
	uint64_t timestamp = 0;
	const uint8_t *msg = NULL;
	uint32_t msg_len = 0;
	bool is_suid;
	bool waiting;
	int type;

	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			ssc_bad();
			return;
		}
		if (field == 1 && wire == 5) {
			if (!db_fixed32(&d, &msg_id)) {
				ssc_bad();
				return;
			}
		} else if (field == 2 && wire == 1) {
			if (!db_fixed64(&d, &timestamp)) {
				ssc_bad();
				return;
			}
		} else if (field == 3 && wire == 2) {
			if (!db_bytes(&d, &msg, &msg_len)) {
				ssc_bad();
				return;
			}
		} else if (!db_skip(&d, wire)) {
			ssc_bad();
			return;
		}
	}
	if (msg_id == 0) {
		ssc_bad();
		return;
	}
	if (!ssc_msg_known(msg_id)) {
		if (ssc_note_unknown_msg(msg_id)) {
			say("vayu-ssc: unknown msg %u from uid %016llx%016llx (%u bytes)\n",
			    msg_id, (unsigned long long)high, (unsigned long long)low, msg_len);
		}
		return;
	}

	is_suid = low == SSC_SUID_LOW && high == SSC_SUID_HIGH;
	lck_spin_lock(&ssc_lock);
	type = ssc_type_of_uid(low, high);
	lck_spin_unlock(&ssc_lock);

	if (msg_id == SSC_MSG_SUID_RESPONSE) {
		if (is_suid) {
			lck_spin_lock(&ssc_lock);
			ssc.suid_answers++;
			lck_spin_unlock(&ssc_lock);
			ssc_suid_response(msg, msg_len);
		} else if (type >= 0) {
			ssc_config_response(low, high, msg, msg_len);
		} else {
			ssc_unknown_uid(low, high, msg_id, msg_len);
		}
		return;
	}
	if (type < 0) {
		ssc_unknown_uid(low, high, msg_id, msg_len);
		return;
	}
	if (msg_id == SSC_MSG_ATTR_RESPONSE) {
		lck_spin_lock(&ssc_lock);
		waiting = ssc.attr.waiting && ssc.attr.low == low && ssc.attr.high == high;
		lck_spin_unlock(&ssc_lock);
		if (waiting) {
			ssc_attr_response(msg, msg_len);
			lck_spin_lock(&ssc_lock);
			if (ssc.attr.waiting && ssc.attr.low == low && ssc.attr.high == high) {
				ssc.attr.seq++;
			}
			lck_spin_unlock(&ssc_lock);
		}
		return;
	}
	if (msg_id == SSC_MSG_SENSOR_SAMPLE) {
		lck_spin_lock(&ssc_lock);
		waiting = ssc.stream.type == type;
		lck_spin_unlock(&ssc_lock);
		if (waiting) {
			ssc_sample_response(type, msg, msg_len, timestamp);
		}
		return;
	}
}

/* SscClientResponse{uid SscUid = 1; response SscClientResponseBody = 2}. */
static bool
client_response_uid(const uint8_t *p, uint32_t len, uint64_t *low, uint64_t *high)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;

	*low = 0;
	*high = 0;
	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			return false;
		}
		if (field == 1 && wire == 2) {
			const uint8_t *q;
			uint32_t n;

			if (!db_bytes(&d, &q, &n) || !uid_parse(q, n, low, high)) {
				return false;
			}
		} else if (!db_skip(&d, wire)) {
			return false;
		}
	}
	return !d.bad;
}

static void
ssc_client_response(const uint8_t *p, uint32_t len)
{
	struct dbuf d;
	uint32_t field;
	unsigned wire;
	uint64_t low, high;

	if (!client_response_uid(p, len, &low, &high)) {
		ssc_bad();
		return;
	}
	db_init(&d, p, len);
	while (d.off < d.len) {
		if (!db_tag(&d, &field, &wire)) {
			ssc_bad();
			return;
		}
		if (field == 2 && wire == 2) {
			const uint8_t *q;
			uint32_t n;

			if (!db_bytes(&d, &q, &n)) {
				ssc_bad();
				return;
			}
			ssc_handle_body(low, high, q, n);
		} else if (!db_skip(&d, wire)) {
			ssc_bad();
			return;
		}
	}
}

/* Report Small / Report Large: TLV 0x01 client id, TLV 0x02 length + SscClientResponse. */
static void
ssc_indication(uint16_t msg, const uint8_t *p, uint32_t plen)
{
	uint32_t off = 0;
	const uint8_t *body = NULL;
	uint32_t body_len = 0;
	bool has_body = false;
	bool bad = false;

	while (off + 3 <= plen) {
		uint8_t type = p[off];
		uint16_t n = (uint16_t)(p[off + 1] | (p[off + 2] << 8));

		off += 3;
		if (n > plen - off) {
			bad = true;
			break;
		}
		if (type == 0x01) {
			if (n != 4 && n != 8) {
				bad = true;
			}
		} else if (type == 0x02) {
			uint16_t blen;

			if (n < 2) {
				bad = true;
				break;
			}
			blen = (uint16_t)(p[off] | (p[off + 1] << 8));
			if (blen > n - 2) {
				bad = true;
				break;
			}
			body = p + off + 2;
			body_len = blen;
			has_body = true;
		}
		off += n;
	}
	if (off != plen) {
		bad = true;
	}

	lck_spin_lock(&ssc_lock);
	ssc.indications++;
	if (msg == SSC_MSG_REPORT_SMALL) {
		ssc.small++;
	} else {
		ssc.large++;
	}
	lck_spin_unlock(&ssc_lock);

	if (bad || !has_body) {
		ssc_bad();
		return;
	}
	ssc_client_response(body, body_len);
}

/* --------------------------------------------------------------------------------------- */
/* QMI                                                                                     */
/* --------------------------------------------------------------------------------------- */

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* Control 0x0020 answer: result/error, client id and the optional u32. */
static void
ssc_qmi_response(const uint8_t *data, uint32_t length)
{
	uint16_t txn = get16(data + 1);
	uint16_t msg = get16(data + 3);
	uint32_t plen = get16(data + 5);
	const uint8_t *p = data + 7;
	uint32_t off = 0;
	uint16_t result = 0xffff;
	uint16_t error = 0xffff;
	bool has_result = false;
	uint64_t client = 0;
	bool has_client = false;
	uint32_t extra = 0;
	bool has_extra = false;
	bool bad = false;
	bool print;
	uint32_t response_no;
	uint16_t request_msg = 0;

	if (plen > length - 7) {
		ssc_bad();
		return;
	}
	while (off + 3 <= plen) {
		uint8_t type = p[off];
		uint16_t n = (uint16_t)(p[off + 1] | (p[off + 2] << 8));

		off += 3;
		if (n > plen - off) {
			bad = true;
			break;
		}
		if (type == 0x02) {
			if (n < 4) {
				bad = true;
			} else {
				result = get16(p + off);
				error = get16(p + off + 2);
				has_result = true;
			}
		} else if (type == 0x10) {
			if (n == 8) {
				client = get64(p + off);
				has_client = true;
			} else if (n == 4) {
				client = get32(p + off);
				has_client = true;
			} else {
				bad = true;
			}
		} else if (type == 0x11) {
			if (n >= 4) {
				extra = get32(p + off);
				has_extra = true;
			}
		}
		off += n;
	}
	if (off != plen) {
		bad = true;
	}

	lck_spin_lock(&ssc_lock);
	ssc.responses++;
	response_no = ssc.responses;
	print = response_no <= 8;
	for (unsigned i = 0; i < SSC_TXN_SLOTS; i++) {
		if (ssc.txn_used[i] && ssc.txn_num[i] == txn) {
			request_msg = ssc.txn_msg[i];
			break;
		}
	}
	{
		unsigned slot = ssc.reply_next++ % SSC_REPLY_SLOTS;

		ssc.reply_used[slot] = true;
		ssc.reply_txn[slot] = txn;
		ssc.reply_result[slot] = result;
		ssc.reply_error[slot] = error;
	}
	lck_spin_unlock(&ssc_lock);

	if (print) {
		say("vayu-ssc: response %u: txn %u msg 0x%04x: result %u error %u client 0x%llx u32 %u\n",
		    response_no, txn, msg, has_result ? result : 0xffff, has_result ? error : 0xffff,
		    has_client ? (unsigned long long)client : 0ULL, has_extra ? extra : 0);
	}
	if (has_result && result != 0) {
		say("vayu-ssc: request %u REFUSED: result %u error %u\n",
		    request_msg, result, error);
	}
	if (bad) {
		ssc_bad();
	}
}

/* Our side of QRTR: the router's calls (sys/hakios.qrtr.h) and the port they gave us. */
struct ssc_link {
	const struct hakios_qrtr_ops *ops;
	struct hakios_qrtr_port *port;
};
static struct ssc_link ssc_conn;

/* (servers are not looked up through this port: vayu_slpi.c already knows where SSC is) */
static void
ssc_server(void *context __unused, bool arrived __unused, uint32_t service __unused, uint32_t version __unused,
    uint32_t instance __unused, uint32_t node __unused, uint32_t port __unused)
{
}

static void
ssc_receive(void *context __unused, uint32_t from_node __unused, uint32_t from_port __unused,
    const void *bytes, size_t size)
{
	const uint8_t *data = bytes;
	uint32_t length = size > 0xffffffffu ? 0xffffffffu : (uint32_t)size;

	uint8_t type;
	uint16_t msg;
	uint32_t plen;

	if (length < 7) {
		ssc_bad();
		return;
	}
	type = data[0];
	msg = get16(data + 3);
	plen = get16(data + 5);
	if (plen > length - 7) {
		ssc_bad();
		return;
	}

	if (type == SSC_QMI_RESPONSE && msg == SSC_MSG_CONTROL) {
		ssc_qmi_response(data, length);
	} else if (type == SSC_QMI_INDICATION &&
	    (msg == SSC_MSG_REPORT_SMALL || msg == SSC_MSG_REPORT_LARGE)) {
		ssc_indication(msg, data + 7, plen);
	} else {
		ssc_bad();
	}
}

/* --------------------------------------------------------------------------------------- */
/* Requests                                                                                */
/* --------------------------------------------------------------------------------------- */

static uint16_t
ssc_alloc_txn(uint16_t msg_id)
{
	uint16_t txn;
	unsigned slot;

	lck_spin_lock(&ssc_lock);
	txn = ssc.next_txn++;
	if (ssc.next_txn == 0) {
		ssc.next_txn = 1;
	}
	slot = ssc.txn_next++ % SSC_TXN_SLOTS;
	ssc.txn_used[slot] = true;
	ssc.txn_num[slot] = txn;
	ssc.txn_msg[slot] = msg_id;
	lck_spin_unlock(&ssc_lock);
	return txn;
}

/*
 * One Control 0x0020 request with a SscClientRequest. `body_present' false omits the optional
 * SscClientRequestBody.msg field entirely (the disable request wants no body).
 */
static int
ssc_send(struct ssc_link *q, uint32_t node, uint32_t port, uint64_t low, uint64_t high,
    uint32_t msg_id, const uint8_t *body, uint32_t body_len, bool body_present, uint16_t *txn_out)
{
	struct pbuf uid, cfg, bodyb, req, qmi;
	uint16_t txn;
	uint32_t plen;
	int err;

	pb_init(&uid, enc_uid, sizeof(enc_uid));
	pb_fixed64(&uid, 1, low);
	pb_fixed64(&uid, 2, high);

	pb_init(&cfg, enc_cfg, sizeof(enc_cfg));
	pb_uint(&cfg, 1, 1);			/* SscProcessorType APPS */
	pb_uint(&cfg, 2, 0);			/* suspend mode WAKEUP */

	pb_init(&bodyb, enc_body, sizeof(enc_body));
	if (body_present) {
		pb_bytes(&bodyb, 2, body, body_len);
	}

	pb_init(&req, enc_req, sizeof(enc_req));
	pb_bytes(&req, 1, uid.data, uid.len);
	pb_fixed32(&req, 2, msg_id);
	pb_bytes(&req, 3, cfg.data, cfg.len);
	pb_bytes(&req, 4, bodyb.data, bodyb.len);
	if (uid.bad || cfg.bad || bodyb.bad || req.bad) {
		return -1;
	}

	txn = ssc_alloc_txn((uint16_t)msg_id);

	pb_init(&qmi, enc_qmi, sizeof(enc_qmi));
	{
		uint8_t hdr[7] = { 0, 0, 0, 0, 0, 0, 0 };

		pb_raw(&qmi, hdr, sizeof(hdr));
	}
	{
		uint8_t t[5];

		/*
		 * TLV 0x01: the TLV's length, then the payload's own u16 length, then the payload (an
		 * Android capture: `01 33 00 31 00 0a 12 ...`, libssc utils/strace.txt:1832). Run 195412
		 * sent it without the inner length: SSC read 0x120a as the length and answered
		 * result 1 error 19 (QMI_ERR_ARG_TOO_LONG) to every request.
		 */
		t[0] = 0x01;			/* SscClientRequest */
		t[1] = (uint8_t)((req.len + 2) & 0xff);
		t[2] = (uint8_t)((req.len + 2) >> 8);
		t[3] = (uint8_t)(req.len & 0xff);
		t[4] = (uint8_t)(req.len >> 8);
		pb_raw(&qmi, t, sizeof(t));
		pb_raw(&qmi, req.data, req.len);
	}
	{
		uint8_t t[4] = { 0x10, 1, 0, 0x01 };	/* report type: LARGE */

		pb_raw(&qmi, t, sizeof(t));
	}
	if (qmi.bad || qmi.len < 7 || qmi.len - 7 > 0xffff) {
		return -1;
	}

	plen = qmi.len - 7;
	qmi.data[0] = SSC_QMI_REQUEST;
	qmi.data[1] = (uint8_t)(txn & 0xff);
	qmi.data[2] = (uint8_t)(txn >> 8);
	qmi.data[3] = (uint8_t)(SSC_MSG_CONTROL & 0xff);
	qmi.data[4] = (uint8_t)(SSC_MSG_CONTROL >> 8);
	qmi.data[5] = (uint8_t)(plen & 0xff);
	qmi.data[6] = (uint8_t)(plen >> 8);

	if (txn_out != NULL) {
		*txn_out = txn;
	}
	err = q->ops->send(q->port, node, port, qmi.data, qmi.len);
	return err;
}

/* --------------------------------------------------------------------------------------- */
/* The run thread                                                                          */
/* --------------------------------------------------------------------------------------- */

static bool
ssc_wait_reply(uint16_t txn, uint32_t timeout_ms, uint16_t *result, uint16_t *error)
{
	for (uint32_t waited = 0; ; waited += 10) {
		bool found = false;

		lck_spin_lock(&ssc_lock);
		for (unsigned i = 0; i < SSC_REPLY_SLOTS; i++) {
			if (ssc.reply_used[i] && ssc.reply_txn[i] == txn) {
				found = true;
				if (result != NULL) {
					*result = ssc.reply_result[i];
				}
				if (error != NULL) {
					*error = ssc.reply_error[i];
				}
				break;
			}
		}
		lck_spin_unlock(&ssc_lock);
		if (found) {
			return true;
		}
		if (waited >= timeout_ms) {
			return false;
		}
		IOSleep(10);
	}
}

static bool
ssc_lookup(struct ssc_link *q, uint32_t node, uint32_t port, unsigned type)
{
	struct pbuf b;
	uint32_t seen;
	uint32_t total = 0;
	uint32_t stored = 0;
	uint32_t i;
	uint64_t low[SSC_MAX_UIDS];
	uint64_t high[SSC_MAX_UIDS];
	uint16_t txn;
	bool answered = false;
	int err;

	pb_init(&b, enc_leaf, sizeof(enc_leaf));
	pb_string(&b, 1, ssc_type_name[type]);
	/* as Android's HAL sends it (libssc utils/strace.txt:1832: `0a 08 registry 10 01 18 00`) */
	pb_uint(&b, 2, 1);			/* enable_updates = true */
	pb_uint(&b, 3, 0);			/* only_default_values = false */
	if (b.bad) {
		say("vayu-ssc: suid %s: request too large\n", ssc_type_name[type]);
		return false;
	}

	lck_spin_lock(&ssc_lock);
	seen = ssc.suid[type].seq;
	lck_spin_unlock(&ssc_lock);

	err = ssc_send(q, node, port, SSC_SUID_LOW, SSC_SUID_HIGH, SSC_MSG_SUID_REQUEST,
	    b.data, b.len, true, &txn);
	if (err != 0) {
		say("vayu-ssc: suid %s: send failed (%d)\n", ssc_type_name[type], err);
		return false;
	}

	for (uint32_t waited = 0; ; waited += 10) {
		lck_spin_lock(&ssc_lock);
		if (ssc.suid[type].seq != seen) {
			answered = true;
			total = ssc.suid[type].total;
			stored = ssc.suid[type].stored;
			for (i = 0; i < stored; i++) {
				low[i] = ssc.suid[type].low[i];
				high[i] = ssc.suid[type].high[i];
			}
		}
		lck_spin_unlock(&ssc_lock);
		if (answered || waited >= 3000) {
			break;
		}
		IOSleep(10);
	}

	if (!answered || total == 0) {
		say("vayu-ssc: suid %s: NONE\n", ssc_type_name[type]);
		return false;
	}
	say("vayu-ssc: suid %s: %u uid(s)\n", ssc_type_name[type], total);
	for (i = 0; i < stored; i++) {
		say("vayu-ssc: suid %s uid %u: %016llx%016llx\n", ssc_type_name[type], i,
		    (unsigned long long)high[i], (unsigned long long)low[i]);
	}
	return total != 0;
}

static void
ssc_attributes(struct ssc_link *q, uint32_t node, uint32_t port, unsigned type,
    uint64_t low, uint64_t high)
{
	struct pbuf b;
	uint32_t seen;
	uint16_t txn;
	bool answered = false;
	int err;

	pb_init(&b, enc_leaf, sizeof(enc_leaf));
	pb_uint(&b, 2, 0);			/* SscAttrRequest.enable_updates = false */
	say("vayu-ssc: %s attributes (uid %016llx%016llx):\n", ssc_type_name[type],
	    (unsigned long long)high, (unsigned long long)low);

	lck_spin_lock(&ssc_lock);
	ssc.attr.waiting = true;
	ssc.attr.low = low;
	ssc.attr.high = high;
	seen = ssc.attr.seq;
	lck_spin_unlock(&ssc_lock);

	/* SscAttrRequest{enable_updates = 2 (false)}: an empty message is enough. */
	err = ssc_send(q, node, port, low, high, SSC_MSG_ATTR_REQUEST,
	    b.data, b.len, true, &txn);
	if (err != 0) {
		say("vayu-ssc: %s attributes: send failed (%d)\n", ssc_type_name[type], err);
		lck_spin_lock(&ssc_lock);
		ssc.attr.waiting = false;
		lck_spin_unlock(&ssc_lock);
		return;
	}

	for (uint32_t waited = 0; ; waited += 10) {
		lck_spin_lock(&ssc_lock);
		if (ssc.attr.seq != seen) {
			answered = true;
		}
		lck_spin_unlock(&ssc_lock);
		if (answered || waited >= 2000) {
			break;
		}
		IOSleep(10);
	}
	lck_spin_lock(&ssc_lock);
	ssc.attr.waiting = false;
	lck_spin_unlock(&ssc_lock);
	if (!answered) {
		say("vayu-ssc: %s: no attribute answer\n", ssc_type_name[type]);
	}
}

static void
ssc_stream(struct ssc_link *q, uint32_t node, uint32_t port, unsigned type,
    uint64_t low, uint64_t high, uint32_t hz, uint32_t secs, bool forever)
{
	struct pbuf b;
	uint32_t seen;
	uint32_t rate = 0;
	bool has_rate = false;
	char mode[32];
	bool has_mode = false;
	bool configured = false;
	uint16_t txn, result, error;
	uint32_t count;
	int32_t last[3] = { 0, 0, 0 };
	int32_t accuracy = 0;
	uint64_t timestamp = 0;
	bool has_latest = false;
	int err;

	memset(mode, 0, sizeof(mode));

	lck_spin_lock(&ssc_lock);
	ssc.stream.type = (int)type;
	ssc.stream.count = 0;
	ssc.stream.has = false;
	seen = ssc.config.seq;
	lck_spin_unlock(&ssc_lock);

	pb_init(&b, enc_leaf, sizeof(enc_leaf));
	pb_fixed32(&b, 1, u32_to_f32_bits(hz));	/* SscEnableConfigRequest{sample_rate = 1} */
	if (b.bad) {
		say("vayu-ssc: %s: enable request too large\n", ssc_type_name[type]);
		lck_spin_lock(&ssc_lock);
		ssc.stream.type = -1;
		lck_spin_unlock(&ssc_lock);
		return;
	}
	err = ssc_send(q, node, port, low, high, SSC_MSG_ENABLE_CONFIG,
	    b.data, b.len, true, &txn);
	if (err != 0) {
		say("vayu-ssc: %s: enable send failed (%d)\n", ssc_type_name[type], err);
		lck_spin_lock(&ssc_lock);
		ssc.stream.type = -1;
		lck_spin_unlock(&ssc_lock);
		return;
	}

	for (uint32_t waited = 0; ; waited += 10) {
		lck_spin_lock(&ssc_lock);
		if (ssc.config.seq != seen && ssc.config.low == low && ssc.config.high == high) {
			configured = true;
			rate = ssc.config.rate;
			has_rate = ssc.config.has_rate;
			memcpy(mode, ssc.config.mode, sizeof(mode));
			has_mode = ssc.config.has_mode;
		}
		lck_spin_unlock(&ssc_lock);
		if (configured || waited >= 2000) {
			break;
		}
		IOSleep(10);
	}
	if (configured) {
		char m[16];

		fmt_milli(m, sizeof(m), f32_milli(rate));
		say("vayu-ssc: %s config: sample rate %s Hz", ssc_type_name[type],
		    has_rate ? m : "unknown");
		if (has_mode) {
			say(", mode \"%s\"", mode);
		}
		say("\n");
	} else {
		say("vayu-ssc: %s: no config answer\n", ssc_type_name[type]);
	}

	if (forever) {
		/* level 6: the stream stays on for iOS (/dev/vayu-motion) until the run ends */
		uint32_t said = 0;

		say("vayu-ssc: %s: STREAMING for iOS (/dev/vayu-motion) until the run ends\n", ssc_type_name[type]);
		for (uint32_t s = 1; !ssc_leaving; s++) {
			IOSleep(1000);
			if (s % 30 == 0 && said++ < 40) {
				lck_spin_lock(&ssc_lock);
				count = ssc.stream.count;
				last[0] = ssc.stream.last[0];
				last[1] = ssc.stream.last[1];
				last[2] = ssc.stream.last[2];
				lck_spin_unlock(&ssc_lock);
				say("vayu-ssc: %s: +%u s: %u samples, latest %d %d %d mm/s^2\n", ssc_type_name[type], s, count,
				    last[0], last[1], last[2]);
			}
		}
		return;         /* (no disable: the SLPI is being stopped) */
	}
	{
		uint64_t t0 = ssc_now_ns();

		for (uint32_t s = 1; s <= secs; s++) {
			uint64_t until = t0 + (uint64_t)s * 1000000000ULL;

			for (;;) {
				uint64_t now = ssc_now_ns();

				if (now >= until) {
					break;
				}
				IOSleep(10);
			}
			lck_spin_lock(&ssc_lock);
			count = ssc.stream.count;
			has_latest = ssc.stream.has;
			last[0] = ssc.stream.last[0];
			last[1] = ssc.stream.last[1];
			last[2] = ssc.stream.last[2];
			accuracy = ssc.stream.accuracy;
			timestamp = ssc.stream.timestamp;
			lck_spin_unlock(&ssc_lock);
			if (has_latest) {
				char x[16], y[16], z[16];

				fmt_milli(x, sizeof(x), last[0]);
				fmt_milli(y, sizeof(y), last[1]);
				fmt_milli(z, sizeof(z), last[2]);
				say("vayu-ssc: %s: %u samples, latest %s %s %s (%s), accuracy %d, t %llu\n",
				    ssc_type_name[type], count, x, y, z,
				    type == SSC_ACCEL ? "m/s^2" : "m/s", accuracy,
				    (unsigned long long)timestamp);
			} else {
				say("vayu-ssc: %s: %u samples, no sample yet\n",
				    ssc_type_name[type], count);
			}
		}
	}

	lck_spin_lock(&ssc_lock);
	count = ssc.stream.count;
	lck_spin_unlock(&ssc_lock);
	say("vayu-ssc: %s: %u samples in %u s (≈%u Hz)\n", ssc_type_name[type], count, secs,
	    secs != 0 ? (count + secs / 2) / secs : 0);

	/* Disable: msg_id 10 with no SscClientRequestBody.msg. */
	err = ssc_send(q, node, port, low, high, SSC_MSG_DISABLE, NULL, 0, false, &txn);
	if (err != 0) {
		say("vayu-ssc: %s disable: send failed (%d)\n", ssc_type_name[type], err);
	} else if (ssc_wait_reply(txn, 1000, &result, &error)) {
		say("vayu-ssc: %s disable: result %u error %u\n", ssc_type_name[type],
		    result, error);
	} else {
		say("vayu-ssc: %s disable: no response\n", ssc_type_name[type]);
	}

	lck_spin_lock(&ssc_lock);
	ssc.stream.type = -1;
	lck_spin_unlock(&ssc_lock);
}

/*
 * /dev/vayu-motion (level 6): a read gives the latest accelerometer sample as struct vayu_motion
 * (sys/vayu_ssc.h); seq 0 means none yet. Never blocks: the reader polls at its own rate.
 */
static int
motion_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
motion_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct vayu_motion m = { .version = VAYU_MOTION_VERSION };
	uint64_t now = ssc_now_ns();

	lck_spin_lock(&ssc_lock);
	if (ssc.accel_valid) {
		m.seq = accel_seq;
		m.accel_mm_s2[0] = ssc.accel_mm[0];
		m.accel_mm_s2[1] = ssc.accel_mm[1];
		m.accel_mm_s2[2] = ssc.accel_mm[2];
		m.age_ms = (uint32_t)((now > ssc.accel_ns ? now - ssc.accel_ns : 0) / 1000000ULL);
	}
	lck_spin_unlock(&ssc_lock);
	if (uio_resid(uio) < (user_ssize_t)sizeof(m)) {
		return EINVAL;
	}
	return uiomove((caddr_t)&m, sizeof(m), uio);
}

static const struct cdevsw motion_cdevsw = {
	.d_open = motion_open, .d_close = motion_open, .d_read = motion_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
motion_node(void)
{
	static bool made;
	int major;

	if (made) {
		return true;
	}
	major = cdevsw_add(-1, &motion_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0444, "vayu-motion") == NULL) {
		say("vayu-ssc: /dev/vayu-motion cannot be made\n");
		return false;
	}
	made = true;
	say("vayu-ssc: /dev/vayu-motion (0444) is made: the latest accelerometer sample, struct vayu_motion\n");
	return true;
}

void
vayu_ssc_quiesce(void)
{
	ssc_leaving = true;
}

bool
vayu_ssc_run(const struct hakios_qrtr_ops *ops, uint32_t node, uint32_t port, uint32_t level, uint32_t secs)
{
	struct ssc_link *q = &ssc_conn;
	uint32_t hz = 50;
	uint32_t responses, indications, large, small, undecodable;
	bool answered;
	unsigned t;

	if (ops == NULL) {
		return false;
	}
	if (!ssc_lock_init()) {
		say("vayu-ssc: no spin lock; nothing is done\n");
		return false;
	}
	ssc_reset();

	PE_parse_boot_argn("vayu_ssc_hz", &hz, sizeof(hz));
	if (hz < 1) {
		hz = 1;
	}
	if (hz > 400) {
		hz = 400;
	}

	say("vayu-ssc: SSC at node %u port %u: level %u, %u s, %u Hz\n",
	    node, port, level, secs, hz);

	ssc_conn.ops = ops;
	ssc_conn.port = ops->open(ssc_receive, ssc_server, &ssc);
	if (ssc_conn.port == NULL) {
		say("vayu-ssc: no QRTR port of our own to be had; nothing is sent\n");
		return false;
	}

	for (t = 0; t < SSC_TYPES; t++) {
		bool found = ssc_lookup(q, node, port, t);

		if (t == SSC_REGISTRY && !found) {
			say("vayu-ssc: no registry sensor: H-noreg (FastRPC file server needed) is the likely reason if nothing physical shows up below\n");
		}
	}

	/*
	 * Late indications: the SLPI can take a while to read its registry (through the FastRPC
	 * file server vayu_fastrpc.c answers) and only then announce its sensors. If no SUID
	 * answered, keep asking for the two that matter for up to vayu_ssc_late seconds (default
	 * 60), a round every 10 s, and print what arrived. Everything below is unchanged.
	 */
	{
		uint32_t answered;
		uint32_t late = 60;
		uint64_t began;

		lck_spin_lock(&ssc_lock);
		answered = ssc.suid_answers;
		lck_spin_unlock(&ssc_lock);
		if (answered == 0) {
			PE_parse_boot_argn("vayu_ssc_late", &late, sizeof(late));
			if (late > 600) {
				late = 600;		/* ten minutes at the most */
			}
			say("vayu-ssc: no SUID answer yet: %u s more, registry and accel every 10 s\n", late);
			began = ssc_now_ns();
			while (!ssc_leaving && ssc_now_ns() - began < (uint64_t)late * 1000000000ULL) {
				uint32_t indications;

				IOSleep(10000);
				if (ssc_leaving) {
					break;
				}
				ssc_lookup(q, node, port, SSC_REGISTRY);
				ssc_lookup(q, node, port, SSC_ACCEL);
				lck_spin_lock(&ssc_lock);
				indications = ssc.indications;
				answered = ssc.suid_answers;
				lck_spin_unlock(&ssc_lock);
				say("vayu-ssc: late +%llu s: %u indication(s), %u SUID answer(s)\n",
				    (unsigned long long)((ssc_now_ns() - began) / 1000000000ULL),
				    indications, answered);
				if (answered != 0) {
					break;
				}
			}
		}
	}

	if (level >= 4) {
		for (t = 0; t < SSC_TYPES; t++) {
			uint64_t low = 0, high = 0;
			bool have = false;

			lck_spin_lock(&ssc_lock);
			if (ssc.suid[t].stored != 0) {
				low = ssc.suid[t].low[0];
				high = ssc.suid[t].high[0];
				have = true;
			}
			lck_spin_unlock(&ssc_lock);
			if (have) {
				ssc_attributes(q, node, port, t, low, high);
			}
		}
	}

	if (level >= 5) {
		unsigned order[2] = { SSC_ACCEL, SSC_GYRO };

		for (unsigned k = 0; k < 2; k++) {
			uint64_t low = 0, high = 0;
			bool have = false;

			t = order[k];
			lck_spin_lock(&ssc_lock);
			if (ssc.suid[t].stored != 0) {
				low = ssc.suid[t].low[0];
				high = ssc.suid[t].high[0];
				have = true;
			}
			lck_spin_unlock(&ssc_lock);
			if (have) {
				ssc_stream(q, node, port, t, low, high, hz, secs, false);
			}
		}
	}

	if (level >= 6) {
		uint64_t low = 0, high = 0;
		bool have = false;

		lck_spin_lock(&ssc_lock);
		if (ssc.suid[SSC_ACCEL].stored != 0) {
			low = ssc.suid[SSC_ACCEL].low[0];
			high = ssc.suid[SSC_ACCEL].high[0];
			have = true;
		}
		lck_spin_unlock(&ssc_lock);
		if (have && motion_node()) {
			ssc_stream(q, node, port, SSC_ACCEL, low, high, hz, 0, true);
		} else if (!have) {
			say("vayu-ssc: 6: no accelerometer: nothing to stream for iOS\n");
		}
	}

	lck_spin_lock(&ssc_lock);
	responses = ssc.responses;
	indications = ssc.indications;
	large = ssc.large;
	small = ssc.small;
	undecodable = ssc.undecodable;
	answered = ssc.suid_answers != 0;
	lck_spin_unlock(&ssc_lock);

	say("vayu-ssc: done: %u responses, %u indications (%u large, %u small), %u undecodable\n",
	    responses, indications, large, small, undecodable);

	ops->close(ssc_conn.port);       /* no callback after this */
	ssc_conn.port = NULL;

	return answered;
}

bool
vayu_ssc_accel(int32_t mm_s2[3], uint64_t *age_ns)
{
	uint64_t now;
	bool valid;

	if (!ssc_lock_ready || mm_s2 == NULL) {
		return false;
	}
	now = ssc_now_ns();
	lck_spin_lock(&ssc_lock);
	valid = ssc.accel_valid;
	if (valid) {
		mm_s2[0] = ssc.accel_mm[0];
		mm_s2[1] = ssc.accel_mm[1];
		mm_s2[2] = ssc.accel_mm[2];
		if (age_ns != NULL) {
			*age_ns = now > ssc.accel_ns ? now - ssc.accel_ns : 0;
		}
	}
	lck_spin_unlock(&ssc_lock);
	return valid;
}
