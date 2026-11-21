/*
 * The protocol core of /dev/vayu-qmux. See vayu_qmux_proto.h and bsd/sys/vayu_qmux.h.
 *
 * This file is deliberately free of kernel headers: host/test_qmux.c compiles it with the
 * host compiler and feeds it the same byte vectors the driver will see.
 */

#include "vayu_qmux_proto.h"

#define QMI_CTL_GET_VERSION_INFO        0x0021
#define QMI_CTL_ALLOCATE_CID            0x0022
#define QMI_CTL_RELEASE_CID             0x0023
#define QMI_CTL_SET_DATA_FORMAT         0x0026
#define QMI_CTL_SYNC                    0x0027

#define QMI_SERVICE_CTL                 0x00
#define QMI_SERVICE_WDS                 0x01
#define QMI_SERVICE_DMS                 0x02
#define QMI_SERVICE_NAS                 0x03
#define QMI_SERVICE_WMS                 0x05
#define QMI_SERVICE_VOICE               0x09
#define QMI_SERVICE_UIM                 0x0b

#define QMI_RESULT_SUCCESS              0
#define QMI_RESULT_FAILURE              1
#define QMI_ERR_NONE                    0
#define QMI_ERR_MALFORMED               1
#define QMI_ERR_CLIENT_IDS_EXHAUSTED    5
#define QMI_ERR_INVALID_ID              41
#define QMI_ERR_NOT_SUPPORTED           94

#define QMI_TLV_RESULT                  0x02
#define QMI_TLV_CTL_SERVICE             0x01
#define QMI_TLV_CTL_ALLOCATION          0x01
#define QMI_TLV_CTL_RELEASE             0x01
#define QMI_TLV_CTL_DATA_FORMAT         0x01

#define QMI_TYPE_REQUEST                0x00
#define QMI_TYPE_RESPONSE               0x02
#define QMI_TYPE_INDICATION             0x04
#define QMI_TYPE_MASK                   0x06

#define QMUX_CONTROL_FLAG               0x80

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

static void
copy_bytes(uint8_t *to, const uint8_t *from, size_t n)
{
	while (n-- != 0) {
		*to++ = *from++;
	}
}

static void
zero_bytes(uint8_t *to, size_t n)
{
	while (n-- != 0) {
		*to++ = 0;
	}
}

/* ---- QMux framing ------------------------------------------------------------------------ */

/*
 * The length field is checked against the actual datagram for every candidate, so a frame
 * that validates is unambiguous in practice; the QMI header at the candidate offset has to
 * validate too (type, and its own length exactly the rest).
 */
static int
parse_at(const uint8_t *buf, size_t len, uint8_t style, uint8_t len_mode, struct vqm_frame *out)
{
	size_t hdr = style;
	uint16_t field = style == 7 ? get16(buf + 2) : get16(buf + 1);
	size_t expected;

	switch (style) {
	case 6:
		expected = (size_t)field + (len_mode == VQM_LEN_ALL_AFTER_VERSION ? 1 : 3);
		break;
	case 5:
		expected = (size_t)field + (len_mode == VQM_LEN_ALL_AFTER_VERSION ? 1 : 2);
		break;
	case 7:
		expected = (size_t)field + 4;
		break;
	default:
		return -22;
	}
	if (expected != len || len < hdr + 7) {
		return -22;
	}

	const uint8_t *qmi = buf + hdr;
	uint8_t type = qmi[0];

	if ((type & (uint8_t)~QMI_TYPE_MASK) != 0) {
		return -22;
	}
	if (style == 7 && type != buf[1]) {
		return -22;
	}
	if ((size_t)get16(qmi + 5) != len - hdr - 7) {
		return -22;
	}

	out->frame_len = (uint16_t)len;
	out->hdr_len = (uint16_t)hdr;
	out->style = style;
	out->len_mode = len_mode;
	out->qmi = qmi;
	out->qmi_type = type;
	out->transaction = get16(qmi + 1);
	out->message = get16(qmi + 3);
	out->qmi_len = get16(qmi + 5);
	out->ctl_flags = (style == 7) ? buf[4] : buf[3];
	out->service = (style == 6) ? buf[4] : (style == 7) ? buf[5] : QMI_SERVICE_CTL;
	out->client = (style == 6) ? buf[5] : (style == 7) ? buf[6] : buf[4];
	return 0;
}

int
vqm_parse(const uint8_t *buf, size_t len, struct vqm_frame *out)
{
	/* The order is the one that matters when two candidates could fit: the 6-byte form the
	 * shim and libqmi use first, then its 5-byte control form, then canonical QMux. */
	static const struct {
		uint8_t style;
		uint8_t mode;
	} candidates[] = {
		{ 6, VQM_LEN_ALL_AFTER_VERSION },
		{ 6, VQM_LEN_PAYLOAD },
		{ 5, VQM_LEN_ALL_AFTER_VERSION },
		{ 5, VQM_LEN_PAYLOAD },
		{ 7, VQM_LEN_CANONICAL },
	};

	if (buf == NULL || out == NULL || len < 5 || buf[0] != 0x01) {
		return -22;
	}
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (parse_at(buf, len, candidates[i].style, candidates[i].mode, out) == 0) {
			return 0;
		}
	}
	return -22;
}

size_t
vqm_build(uint8_t *out, size_t cap, uint8_t style, uint8_t len_mode, uint8_t ctl_flags,
    uint8_t service, uint8_t client, uint8_t qmi_type, const uint8_t *qmi, size_t qmi_len)
{
	size_t hdr, total, field;

	if (out == NULL || qmi == NULL || qmi_len > VAYU_QMUX_MAX_PAYLOAD) {
		return 0;
	}
	switch (style) {
	case 6:
		hdr = 6;
		break;
	case 5:
		hdr = 5;
		break;
	case 7:
		hdr = 7;
		break;
	default:
		return 0;
	}
	total = hdr + qmi_len;
	if (total > VAYU_QMUX_MAX_PAYLOAD || cap < total || total > 0xffff) {
		return 0;
	}

	switch (style) {
	case 6:
		field = len_mode == VQM_LEN_ALL_AFTER_VERSION ? total - 1 : total - 3;
		break;
	case 5:
		field = len_mode == VQM_LEN_ALL_AFTER_VERSION ? total - 1 : total - 2;
		break;
	default:
		field = total - 4;
		break;
	}
	if (field > 0xffff) {
		return 0;
	}

	out[0] = 0x01;
	if (style == 7) {
		out[1] = qmi_type;
		put16(out + 2, (uint16_t)field);
		out[4] = ctl_flags;
		out[5] = service;
		out[6] = client;
	} else if (style == 6) {
		put16(out + 1, (uint16_t)field);
		out[3] = ctl_flags;
		out[4] = service;
		out[5] = client;
	} else {
		put16(out + 1, (uint16_t)field);
		out[3] = ctl_flags;
		out[4] = client;
	}
	copy_bytes(out + hdr, qmi, qmi_len);
	return total;
}

/* ---- TLVs -------------------------------------------------------------------------------- */

int
vqm_tlv_find(const uint8_t *tlvs, size_t len, uint8_t type, const uint8_t **value,
    uint16_t *value_len)
{
	size_t at = 0;

	if (tlvs == NULL) {
		return 0;
	}
	while (at + 3 <= len) {
		uint16_t l = get16(tlvs + at + 1);

		if (at + 3 + (size_t)l > len) {
			return 0;
		}
		if (tlvs[at] == type) {
			if (value != NULL) {
				*value = tlvs + at + 3;
			}
			if (value_len != NULL) {
				*value_len = l;
			}
			return 1;
		}
		at += 3 + (size_t)l;
	}
	return 0;
}

static size_t
put_tlv(uint8_t *out, size_t cap, uint8_t type, const uint8_t *value, size_t value_len)
{
	if (cap < 3 + value_len || value_len > 0xffff) {
		return 0;
	}
	out[0] = type;
	put16(out + 1, (uint16_t)value_len);
	if (value_len != 0) {
		copy_bytes(out + 3, value, value_len);
	}
	return 3 + value_len;
}

void
vqm_set_transaction(uint8_t *qmi, uint16_t transaction)
{
	if (qmi != NULL) {
		put16(qmi + 1, transaction);
	}
}

/* A QMI response in `qmi`: header, result TLV, then the caller's TLVs. */
static size_t
qmi_response(uint8_t *qmi, size_t cap, uint16_t transaction, uint16_t message, uint16_t result,
    uint16_t error, const uint8_t *tlvs, size_t tlvs_len)
{
	size_t body, total;
	uint8_t *at;

	if (cap < 14 + tlvs_len || tlvs_len > 0xffff - 7) {
		return 0;
	}
	body = 7 + tlvs_len;
	total = 7 + body;
	qmi[0] = QMI_TYPE_RESPONSE;
	put16(qmi + 1, transaction);
	put16(qmi + 3, message);
	put16(qmi + 5, (uint16_t)body);

	at = qmi + 7;
	at[0] = QMI_TLV_RESULT;
	put16(at + 1, 4);
	put16(at + 3, result);
	put16(at + 5, error);
	if (tlvs_len != 0) {
		copy_bytes(at + 7, tlvs, tlvs_len);
	}
	return total;
}

size_t
vqm_reply(uint8_t *out, size_t cap, const struct vqm_frame *request, uint16_t result,
    uint16_t error, const uint8_t *tlvs, size_t tlvs_len)
{
	uint8_t qmi[VQM_TLV_MAX + 16];
	uint8_t ctl = request->ctl_flags;
	size_t qmi_len;

	if (request == NULL || tlvs_len > VQM_TLV_MAX) {
		return 0;
	}
	if (request->service == QMI_SERVICE_CTL) {
		ctl |= QMUX_CONTROL_FLAG;
	}
	qmi_len = qmi_response(qmi, sizeof(qmi), request->transaction, request->message, result,
	    error, tlvs, tlvs_len);
	if (qmi_len == 0) {
		return 0;
	}
	return vqm_build(out, cap, request->style, request->len_mode, ctl, request->service,
	    request->client, QMI_TYPE_RESPONSE, qmi, qmi_len);
}

size_t
vqm_error(uint8_t *out, size_t cap, const struct vqm_frame *request, uint16_t result,
    uint16_t error)
{
	return vqm_reply(out, cap, request, result, error, NULL, 0);
}

/* ---- whitelist --------------------------------------------------------------------------- */

bool
vqm_allowed(const struct vqm_state *s, uint8_t service)
{
	switch (service) {
	case QMI_SERVICE_CTL:
	case QMI_SERVICE_DMS:
	case QMI_SERVICE_NAS:
	case QMI_SERVICE_WMS:
	case QMI_SERVICE_VOICE:
	case QMI_SERVICE_UIM:
		return true;
	case QMI_SERVICE_WDS:
		return s != NULL && s->wds;
	case VQM_SERVICE_AUDIO:
	case VQM_SERVICE_BSP:
		return vqm_local(s, service);
	default:
		return false;
	}
}

bool
vqm_local(const struct vqm_state *s, uint8_t service)
{
	return s != NULL && ((service == VQM_SERVICE_AUDIO && s->local_e1) ||
	    (service == VQM_SERVICE_BSP && s->local_bsp));
}

size_t
vqm_bsp_reply(uint8_t *out, size_t cap, const struct vqm_frame *request, uint16_t *error)
{
	uint16_t e = QMI_ERR_NONE;

	if (request == NULL || request->qmi_type != QMI_TYPE_REQUEST) {
		e = QMI_ERR_MALFORMED;
	} else if (request->message != VQM_BSP_SET_ANTENNA_PREFERENCE &&
	    request->message != VQM_BSP_SET_AUDIO_LOGGING) {
		e = QMI_ERR_NOT_SUPPORTED;
	}
	if (error != NULL) {
		*error = e;
	}
	if (request == NULL) {
		return 0;
	}
	return vqm_error(out, cap, request, e == QMI_ERR_NONE ? QMI_RESULT_SUCCESS : QMI_RESULT_FAILURE, e);
}

/* ---- client ids -------------------------------------------------------------------------- */

struct vqm_client *
vqm_client_find(struct vqm_state *s, uint8_t service, uint8_t client)
{
	for (size_t i = 0; i < VQM_CLIENTS; i++) {
		if (s->clients[i].used && s->clients[i].service == service &&
		    s->clients[i].client == client) {
			return &s->clients[i];
		}
	}
	return NULL;
}

struct vqm_client *
vqm_client_ensure(struct vqm_state *s, uint8_t service, uint8_t client, uint8_t style,
    uint8_t len_mode, uint8_t ctl_flags)
{
	struct vqm_client *existing = vqm_client_find(s, service, client);

	if (existing != NULL) {
		existing->style = style;
		existing->len_mode = len_mode;
		existing->ctl_flags = ctl_flags;
		return existing;
	}
	for (size_t i = 0; i < VQM_CLIENTS; i++) {
		if (!s->clients[i].used) {
			s->clients[i].used = 1;
			s->clients[i].service = service;
			s->clients[i].client = client;
			s->clients[i].style = style;
			s->clients[i].len_mode = len_mode;
			s->clients[i].ctl_flags = ctl_flags;
			s->clients[i].requests = 0;
			return &s->clients[i];
		}
	}
	return NULL;
}

static bool
client_id_in_use(struct vqm_state *s, uint8_t id)
{
	for (size_t i = 0; i < VQM_CLIENTS; i++) {
		if (s->clients[i].used && s->clients[i].client == id) {
			return true;
		}
	}
	return false;
}

int
vqm_client_alloc(struct vqm_state *s, uint8_t service, uint8_t style, uint8_t len_mode,
    uint8_t ctl_flags, uint8_t *client)
{
	struct vqm_client *slot;

	/* The control service's own client is id 0, as on a real QMux. */
	if (service == QMI_SERVICE_CTL) {
		slot = vqm_client_find(s, service, 0);
		if (slot == NULL) {
			slot = vqm_client_ensure(s, service, 0, style, len_mode, ctl_flags);
		}
		if (slot == NULL) {
			return -5;
		}
		*client = 0;
		return 0;
	}
	for (unsigned tries = 0; tries < 255; tries++) {
		uint16_t id = (uint16_t)(s->next_client + 1);

		if (id == 0 || id > 255) {
			id = 1;
		}
		s->next_client = id;
		if (client_id_in_use(s, (uint8_t)id)) {
			continue;
		}
		slot = vqm_client_ensure(s, service, (uint8_t)id, style, len_mode, ctl_flags);
		if (slot == NULL) {
			return -5;
		}
		*client = (uint8_t)id;
		return 0;
	}
	return -5;
}

int
vqm_client_release(struct vqm_state *s, uint8_t service, uint8_t client)
{
	struct vqm_client *slot = vqm_client_find(s, service, client);

	if (slot == NULL) {
		return -41;                     /* QMI_ERR_INVALID_ID, reported as success by vqm_control */
	}
	(void)vqm_txn_cancel_client(s, service, client);
	zero_bytes((uint8_t *)slot, sizeof(*slot));
	return 0;
}

void
vqm_clients_clear(struct vqm_state *s)
{
	zero_bytes((uint8_t *)s->clients, sizeof(s->clients));
	zero_bytes((uint8_t *)s->txns, sizeof(s->txns));
}

/* ---- transactions ------------------------------------------------------------------------ */

static bool
modem_txn_in_use(struct vqm_state *s, uint16_t id)
{
	for (size_t i = 0; i < VQM_TXNS; i++) {
		if (s->txns[i].used && s->txns[i].modem_txn == id) {
			return true;
		}
	}
	return false;
}

int
vqm_txn_begin(struct vqm_state *s, const struct vqm_frame *f, uint64_t deadline_ms,
    uint16_t *modem_txn)
{
	struct vqm_txn *slot = NULL;
	uint16_t id = 0;

	for (size_t i = 0; i < VQM_TXNS; i++) {
		if (!s->txns[i].used) {
			slot = &s->txns[i];
			break;
		}
	}
	if (slot == NULL) {
		return -28;                     /* ENOSPC */
	}
	for (unsigned tries = 0; tries < 65535; tries++) {
		s->next_txn = (uint16_t)(s->next_txn + 1);
		if (s->next_txn == 0) {
			s->next_txn = 1;
		}
		if (!modem_txn_in_use(s, s->next_txn)) {
			id = s->next_txn;
			break;
		}
	}
	if (id == 0) {
		return -28;
	}
	slot->used = 1;
	slot->service = f->service;
	slot->client = f->client;
	slot->style = f->style;
	slot->len_mode = f->len_mode;
	slot->ctl_flags = f->ctl_flags;
	slot->ap_txn = f->transaction;
	slot->modem_txn = id;
	slot->message = f->message;
	slot->deadline_ms = deadline_ms;
	*modem_txn = id;
	return 0;
}

int
vqm_txn_complete(struct vqm_state *s, uint8_t service, uint16_t modem_txn, uint16_t *ap_txn,
    uint8_t *client, uint8_t *style, uint8_t *len_mode, uint8_t *ctl_flags)
{
	for (size_t i = 0; i < VQM_TXNS; i++) {
		struct vqm_txn *t = &s->txns[i];

		if (!t->used || t->modem_txn != modem_txn) {
			continue;
		}
		if (service != 0xff && t->service != service) {
			continue;
		}
		*ap_txn = t->ap_txn;
		*client = t->client;
		*style = t->style;
		*len_mode = t->len_mode;
		*ctl_flags = t->ctl_flags;
		zero_bytes((uint8_t *)t, sizeof(*t));
		return 1;
	}
	return 0;
}

int
vqm_txn_expire(struct vqm_state *s, uint64_t now_ms, uint8_t *service, uint16_t *ap_txn,
    uint16_t *message, uint8_t *client, uint8_t *style, uint8_t *len_mode, uint8_t *ctl_flags)
{
	struct vqm_txn *oldest = NULL;

	for (size_t i = 0; i < VQM_TXNS; i++) {
		struct vqm_txn *t = &s->txns[i];

		if (!t->used || t->deadline_ms == 0 || t->deadline_ms > now_ms) {
			continue;
		}
		if (oldest == NULL || t->deadline_ms < oldest->deadline_ms) {
			oldest = t;
		}
	}
	if (oldest == NULL) {
		return 0;
	}
	*service = oldest->service;
	*ap_txn = oldest->ap_txn;
	*message = oldest->message;
	*client = oldest->client;
	*style = oldest->style;
	*len_mode = oldest->len_mode;
	*ctl_flags = oldest->ctl_flags;
	zero_bytes((uint8_t *)oldest, sizeof(*oldest));
	return 1;
}

unsigned
vqm_txn_cancel_client(struct vqm_state *s, uint8_t service, uint8_t client)
{
	unsigned count = 0;

	for (size_t i = 0; i < VQM_TXNS; i++) {
		if (s->txns[i].used && s->txns[i].service == service &&
		    s->txns[i].client == client) {
			zero_bytes((uint8_t *)&s->txns[i], sizeof(s->txns[i]));
			count++;
		}
	}
	return count;
}

/* ---- control service --------------------------------------------------------------------- */

static int
control_reply(uint8_t *out, size_t cap, size_t *out_len, const struct vqm_frame *f,
    uint16_t result, uint16_t error, const uint8_t *tlvs, size_t tlvs_len)
{
	size_t n = vqm_reply(out, cap, f, result, error, tlvs, tlvs_len);

	if (n == 0) {
		return -7;                      /* EMSGSIZE: the driver turns this into a status record */
	}
	*out_len = n;
	return 1;
}

static int
control_version_info(struct vqm_state *s, const struct vqm_frame *f, uint8_t *out, size_t cap,
    size_t *out_len)
{
	static const uint8_t base[] = {
		QMI_SERVICE_CTL, QMI_SERVICE_DMS, QMI_SERVICE_NAS,
		QMI_SERVICE_WMS, QMI_SERVICE_VOICE, QMI_SERVICE_UIM,
	};
	uint8_t service_tlv[1 + 5 * 12];
	uint8_t tlvs[80];
	size_t list = 0, n = 0, count = 0;

	for (size_t i = 0; i < sizeof(base); i++) {
		uint8_t svc = base[i];
		uint16_t major = 1, minor = 0;

		if (!vqm_allowed(s, svc)) {
			continue;
		}
		if (svc == QMI_SERVICE_CTL) {
			minor = 5;              /* QMI CTL 1.5, as libqmi's qmi-service-ctl declares */
		}
		service_tlv[1 + list++] = svc;
		put16(service_tlv + 1 + list, major);
		list += 2;
		put16(service_tlv + 1 + list, minor);
		list += 2;
		count++;
	}
	if (s->wds) {
		service_tlv[1 + list++] = QMI_SERVICE_WDS;
		put16(service_tlv + 1 + list, 1);
		list += 2;
		put16(service_tlv + 1 + list, 0);
		list += 2;
		count++;
	}
	/* The kernel's own services are listed like the modem's, so a client that checks the list first finds them. */
	for (uint8_t svc = VQM_SERVICE_AUDIO; svc <= VQM_SERVICE_BSP; svc++) {
		if (!vqm_local(s, svc)) {
			continue;
		}
		service_tlv[1 + list++] = svc;
		put16(service_tlv + 1 + list, 1);
		list += 2;
		put16(service_tlv + 1 + list, 0);
		list += 2;
		count++;
	}
	service_tlv[0] = (uint8_t)count;

	n += put_tlv(tlvs + n, sizeof(tlvs) - n, QMI_TLV_CTL_SERVICE, service_tlv, 1 + list);
	if (n == 0) {
		return -7;
	}
	s->counters.control++;
	return control_reply(out, cap, out_len, f, QMI_RESULT_SUCCESS, QMI_ERR_NONE, tlvs, n);
}

static int
control_allocate(struct vqm_state *s, const struct vqm_frame *f, uint8_t *out, size_t cap,
    size_t *out_len)
{
	const uint8_t *tlvs = f->qmi + 7;
	size_t tlvs_len = f->qmi_len;
	const uint8_t *value;
	uint16_t value_len;
	uint8_t allocation[2];
	uint8_t reply[8];
	uint8_t service;
	uint8_t client;
	size_t n = 0;
	int ret;

	if (!vqm_tlv_find(tlvs, tlvs_len, QMI_TLV_CTL_SERVICE, &value, &value_len) ||
	    value_len < 1) {
		return control_reply(out, cap, out_len, f, QMI_RESULT_FAILURE, QMI_ERR_MALFORMED,
		    NULL, 0);
	}
	service = value[0];
	if (!vqm_allowed(s, service)) {
		s->counters.denied++;
		return control_reply(out, cap, out_len, f, QMI_RESULT_FAILURE,
		    QMI_ERR_NOT_SUPPORTED, NULL, 0);
	}
	ret = vqm_client_alloc(s, service, f->style, f->len_mode, f->ctl_flags, &client);
	if (ret < 0) {
		return control_reply(out, cap, out_len, f, QMI_RESULT_FAILURE,
		    QMI_ERR_CLIENT_IDS_EXHAUSTED, NULL, 0);
	}
	allocation[0] = client;
	allocation[1] = service;
	n = put_tlv(reply, sizeof(reply), QMI_TLV_CTL_ALLOCATION, allocation, sizeof(allocation));
	if (n == 0) {
		return -7;
	}
	s->counters.control++;
	return control_reply(out, cap, out_len, f, QMI_RESULT_SUCCESS, QMI_ERR_NONE, reply, n);
}

static int
control_release(struct vqm_state *s, const struct vqm_frame *f, uint8_t *out, size_t cap,
    size_t *out_len)
{
	const uint8_t *tlvs = f->qmi + 7;
	size_t tlvs_len = f->qmi_len;
	const uint8_t *value;
	uint16_t value_len;
	uint8_t service;

	if (!vqm_tlv_find(tlvs, tlvs_len, QMI_TLV_CTL_RELEASE, &value, &value_len) ||
	    value_len < 1) {
		return control_reply(out, cap, out_len, f, QMI_RESULT_FAILURE, QMI_ERR_MALFORMED,
		    NULL, 0);
	}
	service = value_len >= 2 ? value[1] : QMI_SERVICE_CTL;
	/* Releasing an id that is not there is not an error: the caller wants it gone. */
	(void)vqm_client_release(s, service, value[0]);
	s->counters.control++;
	return control_reply(out, cap, out_len, f, QMI_RESULT_SUCCESS, QMI_ERR_NONE, NULL, 0);
}

static int
control_plain(struct vqm_state *s, const struct vqm_frame *f, uint8_t *out, size_t cap,
    size_t *out_len)
{
	s->counters.control++;
	return control_reply(out, cap, out_len, f, QMI_RESULT_SUCCESS, QMI_ERR_NONE, NULL, 0);
}

int
vqm_control_release_target(const struct vqm_frame *f, uint8_t *service, uint8_t *client)
{
	const uint8_t *value;
	uint16_t value_len;

	if (f == NULL || f->service != QMI_SERVICE_CTL || f->qmi_type != QMI_TYPE_REQUEST ||
	    f->message != QMI_CTL_RELEASE_CID ||
	    !vqm_tlv_find(f->qmi + 7, f->qmi_len, QMI_TLV_CTL_RELEASE, &value, &value_len) ||
	    value_len < 1) {
		return 0;
	}
	*client = value[0];
	*service = value_len >= 2 ? value[1] : QMI_SERVICE_CTL;      /* as control_release() reads it */
	return 1;
}

int
vqm_control(struct vqm_state *s, const struct vqm_frame *f, uint8_t *out, size_t cap,
    size_t *out_len)
{
	/* The service byte is the authoritative sign: the control flag alone is not, since a
	 * caller may set bit 7 for reasons of its own. */
	bool control = f->service == QMI_SERVICE_CTL;

	if (!control) {
		return 0;
	}
	if (f->qmi_type != QMI_TYPE_REQUEST) {
		return control_reply(out, cap, out_len, f, QMI_RESULT_FAILURE, QMI_ERR_MALFORMED,
		    NULL, 0);
	}
	switch (f->message) {
	case QMI_CTL_GET_VERSION_INFO:
		return control_version_info(s, f, out, cap, out_len);
	case QMI_CTL_ALLOCATE_CID:
		return control_allocate(s, f, out, cap, out_len);
	case QMI_CTL_RELEASE_CID:
		return control_release(s, f, out, cap, out_len);
	case QMI_CTL_SYNC:
	case QMI_CTL_SET_DATA_FORMAT:
		return control_plain(s, f, out, cap, out_len);
	default:
		/* Never silence: an unknown request gets a failure the caller can act on. */
		return control_reply(out, cap, out_len, f, QMI_RESULT_FAILURE,
		    QMI_ERR_NOT_SUPPORTED, NULL, 0);
	}
}

/* ---- per-client QRTR ports ---------------------------------------------------------------- */

int
vqm_route_own(struct vqm_state *s, size_t own, uint8_t qmi_type, uint16_t modem_txn,
    struct vqm_dest *d)
{
	const struct vqm_client *c;

	if (s == NULL || d == NULL || own >= VQM_CLIENTS || !s->clients[own].used) {
		return 0;
	}
	c = &s->clients[own];
	zero_bytes((uint8_t *)d, sizeof(*d));
	d->client = c->client;
	d->style = c->style;
	d->len_mode = c->len_mode;
	d->ctl_flags = c->ctl_flags;
	d->transaction = modem_txn;
	switch (qmi_type & QMI_TYPE_MASK) {
	case QMI_TYPE_RESPONSE: {
		uint16_t ap;
		uint8_t client, style, mode, ctl;

		if (vqm_txn_complete(s, c->service, modem_txn, &ap, &client, &style, &mode, &ctl) == 1) {
			d->client = client;
			d->style = style;
			d->len_mode = mode;
			d->ctl_flags = ctl;
			d->transaction = ap;
			d->matched = true;
		}
		return 1;
	}
	case QMI_TYPE_INDICATION:
		return 1;
	default:
		return 0;
	}
}

/* ---- services ---------------------------------------------------------------------------- */

struct vqm_service *
vqm_service_get(struct vqm_state *s, uint8_t service)
{
	for (size_t i = 0; i < VQM_SERVICES; i++) {
		if (s->services[i].present && s->services[i].service == service) {
			return &s->services[i];
		}
	}
	return NULL;
}

struct vqm_service *
vqm_service_by_port(struct vqm_state *s, uint8_t *service, uint32_t port)
{
	for (size_t i = 0; i < VQM_SERVICES; i++) {
		if (s->services[i].present && s->services[i].port == port) {
			if (service != NULL) {
				*service = s->services[i].service;
			}
			return &s->services[i];
		}
	}
	return NULL;
}

void
vqm_service_set(struct vqm_state *s, uint8_t service, uint32_t node, uint32_t port,
    uint32_t version, uint32_t instance)
{
	struct vqm_service *slot = NULL;

	for (size_t i = 0; i < VQM_SERVICES; i++) {
		if (s->services[i].present && s->services[i].service == service) {
			slot = &s->services[i];
			break;
		}
		if (!s->services[i].present && slot == NULL) {
			slot = &s->services[i];
		}
	}
	if (slot == NULL) {
		return;
	}
	slot->present = 1;
	slot->service = service;
	slot->node = node;
	slot->port = port;
	slot->version = version;
	slot->instance = (uint16_t)instance;
}

void
vqm_service_clear(struct vqm_state *s, uint8_t service, uint32_t port)
{
	for (size_t i = 0; i < VQM_SERVICES; i++) {
		if (s->services[i].present && s->services[i].service == service &&
		    (port == 0 || s->services[i].port == port)) {
			s->services[i].present = 0;
			s->services[i].port = 0;
			s->services[i].node = 0;
		}
	}
}

void
vqm_state_init(struct vqm_state *s, bool wds)
{
	zero_bytes((uint8_t *)s, sizeof(*s));
	s->wds = wds;
}

/* ---- WDS Bind Mux Data Port --------------------------------------------------------------- */

static void
put32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t
get32le(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int
vqm_wds_bind_rewrite(uint8_t *qmi, size_t qmi_len, uint32_t ep_type, uint32_t iface_id,
    uint8_t mux, struct vqm_wds_bind *was)
{
	uint8_t *ep = NULL, *mux_at = NULL;
	size_t tlvs_len, at = 0;

	if (was != NULL) {
		*was = (struct vqm_wds_bind){ 0 };
	}
	if (qmi == NULL || qmi_len < 7 || qmi[0] != QMI_TYPE_REQUEST ||
	    get16(qmi + 3) != VQM_WDS_BIND_MUX_DATA_PORT) {
		return 0;
	}
	tlvs_len = get16(qmi + 5);
	if (7 + tlvs_len > qmi_len) {
		return -22;              /* EINVAL */
	}
	/* Find both first: a malformed request is left exactly as it came. */
	while (at + 3 <= tlvs_len) {
		uint8_t type = qmi[7 + at];
		uint16_t l = get16(qmi + 7 + at + 1);

		if (at + 3 + (size_t)l > tlvs_len) {
			return -22;              /* EINVAL */
		}
		if (type == VQM_WDS_TLV_ENDPOINT) {
			if (l != 8) {
				return -22;              /* EINVAL */
			}
			ep = qmi + 7 + at + 3;
		} else if (type == VQM_WDS_TLV_MUX_ID) {
			if (l != 1) {
				return -22;              /* EINVAL */
			}
			mux_at = qmi + 7 + at + 3;
		}
		at += 3 + (size_t)l;
	}
	if (at != tlvs_len) {
		return -22;              /* EINVAL */
	}
	if (ep != NULL) {
		if (was != NULL) {
			was->had_endpoint = true;
			was->ep_type = get32le(ep);
			was->iface_id = get32le(ep + 4);
		}
		put32le(ep, ep_type);
		put32le(ep + 4, iface_id);
	}
	if (mux_at != NULL) {
		if (was != NULL) {
			was->had_mux = true;
			was->mux = *mux_at;
		}
		*mux_at = mux;
	}
	return 1;
}
