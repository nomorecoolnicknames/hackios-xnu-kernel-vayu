/*
 * Host tests for the /dev/vayu-qmux protocol core. No kernel, no modem: the same
 * bsd/dev/vayu_qmux_proto.c the kernel links is fed byte vectors, and every response is
 * parsed back rather than compared to a string.
 *
 *   tools/qmux-test/run.sh (CC=..., XNU_SRC=<kernel tree>)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vayu_qmux_proto.h"
#include <sys/vayu_voice_bb.h>

static unsigned checks;
static unsigned failures;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- byte vectors ------------------------------------------------------------------------ */

struct bytes {
	uint8_t b[512];
	size_t n;
};

static void
put16le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* A QMI request: header + TLVs. */
static struct bytes
qmi_request(uint16_t txn, uint16_t msg, const uint8_t *tlvs, size_t tlvs_len)
{
	struct bytes q = { { 0 }, 0 };

	q.b[0] = 0x00;
	put16le(q.b + 1, txn);
	put16le(q.b + 3, msg);
	put16le(q.b + 5, (uint16_t)tlvs_len);
	if (tlvs_len != 0) {
		memcpy(q.b + 7, tlvs, tlvs_len);
	}
	q.n = 7 + tlvs_len;
	return q;
}

/* A 6-byte QMux frame (the form the shim and libqmi speak) around a QMI message. */
static struct bytes
qmux6(uint8_t service, uint8_t client, uint8_t ctl, const struct bytes *qmi)
{
	struct bytes f = { { 0 }, 0 };
	size_t total = 6 + qmi->n;

	f.b[0] = 0x01;
	put16le(f.b + 1, (uint16_t)(total - 1));
	f.b[3] = ctl;
	f.b[4] = service;
	f.b[5] = client;
	memcpy(f.b + 6, qmi->b, qmi->n);
	f.n = total;
	return f;
}

static struct bytes
qmux5(uint8_t client, uint8_t ctl, const struct bytes *qmi)
{
	struct bytes f = { { 0 }, 0 };
	size_t total = 5 + qmi->n;

	f.b[0] = 0x01;
	put16le(f.b + 1, (uint16_t)(total - 1));
	f.b[3] = ctl;
	f.b[4] = client;
	memcpy(f.b + 5, qmi->b, qmi->n);
	f.n = total;
	return f;
}

static struct bytes
qmux7(uint8_t type, uint8_t service, uint8_t client, uint8_t ctl, const struct bytes *qmi)
{
	struct bytes f = { { 0 }, 0 };
	size_t total = 7 + qmi->n;

	f.b[0] = 0x01;
	f.b[1] = type;
	put16le(f.b + 2, (uint16_t)(total - 4));
	f.b[4] = ctl;
	f.b[5] = service;
	f.b[6] = client;
	memcpy(f.b + 7, qmi->b, qmi->n);
	f.n = total;
	return f;
}

/* ---- framing ----------------------------------------------------------------------------- */

static void
test_header_layout(void)
{
	CHECK(sizeof(struct vayu_qmux_hdr) == 32);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_magic) == 0);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_version) == 4);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_kind) == 6);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_direction) == 8);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_service) == 9);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_client) == 10);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_qmi_type) == 11);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_transaction) == 12);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_message) == 14);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_length) == 16);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_flags) == 18);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_status) == 20);
	CHECK(offsetof(struct vayu_qmux_hdr, vm_cookie) == 24);
	CHECK(VAYU_QMUX_MAGIC == 0x58554d51u);   /* 'Q','M','U','X' in memory */
}

static void
test_parse_shapes(void)
{
	uint8_t tlv[4] = { 0x01, 0x01, 0x00, 0x03 };   /* allocate CID for NAS */
	struct bytes qmi = qmi_request(0x1234, 0x0022, tlv, sizeof(tlv));
	struct bytes f6 = qmux6(0x00, 0x01, 0x80, &qmi);
	struct bytes f5 = qmux5(0x01, 0x80, &qmi);
	struct bytes f7 = qmux7(0x00, 0x00, 0x01, 0x80, &qmi);
	struct vqm_frame parsed;

	CHECK(vqm_parse(f6.b, f6.n, &parsed) == 0);
	CHECK(parsed.style == 6);
	CHECK(parsed.service == 0x00);
	CHECK(parsed.client == 0x01);
	CHECK(parsed.ctl_flags == 0x80);
	CHECK(parsed.qmi_type == 0x00);
	CHECK(parsed.transaction == 0x1234);
	CHECK(parsed.message == 0x0022);
	CHECK(parsed.qmi_len == qmi.n - 7);
	CHECK(parsed.hdr_len == 6);

	CHECK(vqm_parse(f5.b, f5.n, &parsed) == 0);
	CHECK(parsed.style == 5);
	CHECK(parsed.service == 0x00);
	CHECK(parsed.client == 0x01);
	CHECK(parsed.transaction == 0x1234);

	CHECK(vqm_parse(f7.b, f7.n, &parsed) == 0);
	CHECK(parsed.style == 7);
	CHECK(parsed.service == 0x00);
	CHECK(parsed.client == 0x01);
	CHECK(parsed.transaction == 0x1234);
	CHECK(parsed.qmi_type == 0x00);

	/* The QMI body length must be exactly the rest of the datagram. */
	f6.b[6 + 5] ^= 0xff;
	CHECK(vqm_parse(f6.b, f6.n, &parsed) != 0);
	f6.b[6 + 5] ^= 0xff;

	/* A short frame, a wrong version byte, a random buffer. */
	CHECK(vqm_parse(f6.b, 4, &parsed) != 0);
	CHECK(vqm_parse(f6.b, f6.n - 1, &parsed) != 0);
	uint8_t bad = 0x02;
	uint8_t copy[32];
	memcpy(copy, f6.b, f6.n);
	copy[0] = bad;
	CHECK(vqm_parse(copy, f6.n, &parsed) != 0);
}

static void
test_build_roundtrip(void)
{
	uint8_t qmi[32];
	uint8_t out[128];
	size_t n;
	struct vqm_frame p;

	for (size_t i = 0; i < (size_t)sizeof(qmi); i++) {
		qmi[i] = (uint8_t)(i * 7 + 1);
	}
	qmi[0] = 0x00;
	put16le(qmi + 5, (uint16_t)(sizeof(qmi) - 7));

	n = vqm_build(out, sizeof(out), 6, VQM_LEN_ALL_AFTER_VERSION, 0x00, 0x03, 0x07, 0x00,
	    qmi, sizeof(qmi));
	CHECK(n == 6 + sizeof(qmi));
	CHECK(vqm_parse(out, n, &p) == 0);
	CHECK(p.style == 6 && p.service == 0x03 && p.client == 0x07);
	CHECK(p.qmi_len == sizeof(qmi) - 7);

	n = vqm_build(out, sizeof(out), 6, VQM_LEN_PAYLOAD, 0x00, 0x03, 0x07, 0x00, qmi,
	    sizeof(qmi));
	CHECK(vqm_parse(out, n, &p) == 0);
	CHECK(p.len_mode == VQM_LEN_PAYLOAD);

	n = vqm_build(out, sizeof(out), 5, VQM_LEN_ALL_AFTER_VERSION, 0x80, 0x00, 0x07, 0x00,
	    qmi, sizeof(qmi));
	CHECK(vqm_parse(out, n, &p) == 0);
	CHECK(p.style == 5 && p.client == 0x07);

	n = vqm_build(out, sizeof(out), 7, VQM_LEN_CANONICAL, 0x00, 0x03, 0x07, 0x00, qmi,
	    sizeof(qmi));
	CHECK(vqm_parse(out, n, &p) == 0);
	CHECK(p.style == 7 && p.service == 0x03 && p.client == 0x07);

	/* 0 cap must refuse, never write. */
	CHECK(vqm_build(out, 3, 6, VQM_LEN_ALL_AFTER_VERSION, 0, 0, 0, 0, qmi, sizeof(qmi)) == 0);
}

static void
test_fuzz(void)
{
	uint8_t *buf = malloc(256);
	struct vqm_frame f;

	srand(0x51u);
	for (unsigned round = 0; round < 20000; round++) {
		size_t n = (size_t)(rand() % 200);

		for (size_t i = 0; i < n; i++) {
			buf[i] = (uint8_t)rand();
		}
		if (n != 0 && (round & 1) != 0) {
			buf[0] = 0x01;          /* half the inputs look like QMux at byte 0 */
		}
		(void)vqm_parse(buf, n, &f);
	}
	free(buf);
}

/* ---- control service --------------------------------------------------------------------- */

static struct bytes
control_request(struct vqm_state *s, uint16_t txn, uint16_t msg, const uint8_t *value,
    size_t value_len)
{
	uint8_t tlv[64];
	size_t tlvs_len = 0;
	struct bytes qmi, frame;

	(void)s;
	if (value_len != 0) {
		tlv[0] = 0x01;
		put16le(tlv + 1, (uint16_t)value_len);
		memcpy(tlv + 3, value, value_len);
		tlvs_len = 3 + value_len;
	}
	qmi = qmi_request(txn, msg, tlv, tlvs_len);
	frame = qmux6(0x00, 0x00, 0x80, &qmi);
	return frame;
}

static int
control_call(struct vqm_state *s, const struct bytes *request, struct vqm_frame *response,
    struct bytes *response_bytes)
{
	uint8_t out[512];
	size_t out_len = 0;
	struct vqm_frame f;
	int ret;

	if (vqm_parse(request->b, request->n, &f) != 0) {
		return -22;
	}
	ret = vqm_control(s, &f, out, sizeof(out), &out_len);
	if (ret != 1) {
		return ret;
	}
	memcpy(response_bytes->b, out, out_len);
	response_bytes->n = out_len;
	if (vqm_parse(response_bytes->b, response_bytes->n, response) != 0) {
		return -22;
	}
	return 0;
}

static uint16_t
response_result(const struct vqm_frame *f, uint16_t *error)
{
	const uint8_t *v;
	uint16_t len;

	if (!vqm_tlv_find(f->qmi + 7, f->qmi_len, 0x02, &v, &len) || len < 4) {
		return 0xffff;
	}
	*error = (uint16_t)(v[2] | (v[3] << 8));
	return (uint16_t)(v[0] | (v[1] << 8));
}

static void
test_control(void)
{
	struct vqm_state s;
	struct bytes req, rsp;
	struct vqm_frame rf, pf;
	uint16_t error = 0;
	uint8_t service = 0x03;
	uint8_t client1 = 0, client2 = 0;

	vqm_state_init(&s, false);

	/* GET_VERSION_INFO: success, a service list with NAS/DMS/UIM/WMS/VOICE. */
	req = control_request(&s, 0x0101, 0x0021, NULL, 0);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(rf.qmi_type == 0x02);
	CHECK(rf.transaction == 0x0101);
	CHECK(rf.message == 0x0021);
	CHECK(rf.client == 0x00);
	CHECK(response_result(&rf, &error) == 0);
	CHECK(error == 0);
	const uint8_t *list;
	uint16_t list_len;
	CHECK(vqm_tlv_find(rf.qmi + 7, rf.qmi_len, 0x01, &list, &list_len) == 1);
	CHECK(list_len >= 1);
	unsigned count = list[0];
	CHECK(count == 6);
	CHECK(1 + count * 5 == list_len);
	bool saw_nas = false, saw_dms = false, saw_uim = false, saw_ctl = false;
	for (unsigned i = 0; i < count; i++) {
		uint8_t svc = list[1 + i * 5];
		saw_nas |= svc == 0x03;
		saw_dms |= svc == 0x02;
		saw_uim |= svc == 0x0b;
		saw_ctl |= svc == 0x00;
	}
	CHECK(saw_nas && saw_dms && saw_uim && saw_ctl);

	/* ALLOCATE CID for NAS: a client id and the service type come back. */
	req = control_request(&s, 0x0102, 0x0022, &service, 1);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 0);
	CHECK(error == 0);
	const uint8_t *alloc;
	uint16_t alloc_len;
	CHECK(vqm_tlv_find(rf.qmi + 7, rf.qmi_len, 0x01, &alloc, &alloc_len) == 1);
	CHECK(alloc_len == 2);
	CHECK(alloc[1] == service);
	client1 = alloc[0];
	CHECK(client1 != 0);
	CHECK(vqm_client_find(&s, service, client1) != NULL);

	/* A second allocation gets another id. */
	req = control_request(&s, 0x0103, 0x0022, &service, 1);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(vqm_tlv_find(rf.qmi + 7, rf.qmi_len, 0x01, &alloc, &alloc_len) == 1);
	client2 = alloc[0];
	CHECK(client2 != 0 && client2 != client1);

	/* WDS is refused until vayu_qmux_wds=1. */
	uint8_t wds = 0x01;
	req = control_request(&s, 0x0104, 0x0022, &wds, 1);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 1);
	CHECK(error == 94);
	s.wds = true;
	req = control_request(&s, 0x0105, 0x0022, &wds, 1);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 0);
	CHECK(vqm_tlv_find(rf.qmi + 7, rf.qmi_len, 0x01, &alloc, &alloc_len) == 1);
	CHECK(alloc[1] == 0x01);
	s.wds = false;

	/* RELEASE CID: {client, service}, success, and the table forgets it. */
	uint8_t release[2] = { client2, service };
	req = control_request(&s, 0x0106, 0x0023, release, 2);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 0);
	CHECK(vqm_client_find(&s, service, client2) == NULL);

	/* SYNC: success, no extra TLV required. */
	req = control_request(&s, 0x0107, 0x0027, NULL, 0);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 0);
	CHECK(rf.message == 0x0027);

	/* An unknown control message gets failure, not silence. */
	req = control_request(&s, 0x0108, 0x0099, NULL, 0);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 1);
	CHECK(error == 94);

	/* Malformed ALLOCATE (no TLV): malformed message. */
	req = control_request(&s, 0x0109, 0x0022, NULL, 0);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 1);
	CHECK(error == 1);

	/* A data frame is not the control service's. */
	uint8_t tlv[4] = { 0x01, 0x01, 0x00, 0x02 };
	struct bytes qmi = qmi_request(1, 0x0001, tlv, sizeof(tlv));
	struct bytes data = qmux6(0x02, client1, 0x00, &qmi);
	CHECK(vqm_parse(data.b, data.n, &pf) == 0);
	uint8_t out[128];
	size_t out_len = 0;
	CHECK(vqm_control(&s, &pf, out, sizeof(out), &out_len) == 0);
	CHECK(pf.service == 0x02);
}

static void
test_transactions(void)
{
	struct vqm_state s;
	struct bytes qmi, frame, rsp;
	struct vqm_frame f, p;
	uint16_t modem1 = 0, modem2 = 0, ap = 0;
	uint8_t client = 0, style = 0, mode = 0, ctl = 0;
	uint8_t tlv[4] = { 0x01, 0x01, 0x00, 0x02 };

	vqm_state_init(&s, false);
	qmi = qmi_request(0x1111, 0x0021, tlv, sizeof(tlv));      /* same AP txn twice */
	frame = qmux6(0x02, 0x05, 0x00, &qmi);

	CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
	CHECK(vqm_txn_begin(&s, &f, 1000, &modem1) == 0);
	CHECK(modem1 != 0);
	CHECK(vqm_txn_begin(&s, &f, 2000, &modem2) == 0);
	CHECK(modem2 != 0 && modem2 != modem1);

	/* The modem's ids are what the wire sees; the AP's id is restored on the way back. */
	CHECK(vqm_txn_complete(&s, 0x02, modem2, &ap, &client, &style, &mode, &ctl) == 1);
	CHECK(ap == 0x1111 && client == 0x05 && style == 6 && mode == VQM_LEN_ALL_AFTER_VERSION);
	CHECK(vqm_txn_complete(&s, 0x02, modem1, &ap, &client, &style, &mode, &ctl) == 1);
	CHECK(ap == 0x1111 && client == 0x05);
	CHECK(vqm_txn_complete(&s, 0x02, modem1, &ap, &client, &style, &mode, &ctl) == 0);

	/* Expiry returns the fields needed to answer the caller locally. */
	CHECK(vqm_txn_begin(&s, &f, 500, &modem1) == 0);
	uint8_t svc = 0, st = 0, md = 0, cl = 0, cf = 0;
	uint16_t atxn = 0, msg = 0;
	CHECK(vqm_txn_expire(&s, 499, &svc, &atxn, &msg, &cl, &st, &md, &cf) == 0);
	CHECK(vqm_txn_expire(&s, 500, &svc, &atxn, &msg, &cl, &st, &md, &cf) == 1);
	CHECK(svc == 0x02 && atxn == 0x1111 && msg == 0x0021 && cl == 0x05);

	/* Releasing a client drops its in-flight requests. */
	CHECK(vqm_txn_begin(&s, &f, 9000, &modem1) == 0);
	CHECK(vqm_txn_cancel_client(&s, 0x02, 0x05) == 1);
	CHECK(vqm_txn_complete(&s, 0x02, modem1, &ap, &client, &style, &mode, &ctl) == 0);

	/* A response from the modem is wrapped with the client's framing and txn. */
	uint8_t qmi_rsp[14] = { 0x02, 0x22, 0x33, 0x01, 0x00, 0x07, 0x00,
	    0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
	size_t n = vqm_build(rsp.b, sizeof(rsp.b), 6, VQM_LEN_ALL_AFTER_VERSION, 0x00, 0x02, 0x05,
	    qmi_rsp[0], qmi_rsp, sizeof(qmi_rsp));
	CHECK(n > 0);
	CHECK(vqm_parse(rsp.b, n, &p) == 0);
	CHECK(p.transaction == 0x3322 && p.message == 0x0001 && p.service == 0x02 &&
	    p.client == 0x05);

	/* A frame with no room is refused, not truncated. */
	CHECK(vqm_reply(rsp.b, 10, &f, 0, 0, NULL, 0) == 0);
}

static void
test_styles_and_error(void)
{
	struct vqm_state s;
	struct bytes qmi, frame, rsp;
	struct vqm_frame f, p;

	vqm_state_init(&s, false);
	uint8_t tlv[4] = { 0x01, 0x01, 0x00, 0x02 };
	qmi = qmi_request(0x4242, 0x0022, tlv, sizeof(tlv));
	frame = qmux7(0x00, 0x00, 0x00, 0x80, &qmi);
	CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
	uint8_t out[128];
	size_t out_len = 0;
	CHECK(vqm_control(&s, &f, out, sizeof(out), &out_len) == 1);
	CHECK(vqm_parse(out, out_len, &p) == 0);
	CHECK(p.style == 7);
	CHECK(p.qmi_type == 0x02);
	CHECK(p.transaction == 0x4242);
	CHECK(p.ctl_flags == 0x80);

	/* An error response keeps the caller's txn/message and carries result 1/94. */
	frame = qmux6(0x01, 0x00, 0x00, &qmi);    /* WDS, not whitelisted */
	CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
	rsp.n = vqm_error(rsp.b, sizeof(rsp.b), &f, 1, 94);
	CHECK(rsp.n > 0);
	CHECK(vqm_parse(rsp.b, rsp.n, &p) == 0);
	CHECK(p.service == 0x01 && p.client == 0x00 && p.transaction == 0x4242 && p.message == 0x0022);
	uint16_t error = 0;
	CHECK(response_result(&p, &error) == 1 && error == 94);

	/* Service table: set, find by port, clear. */
	vqm_service_set(&s, 0x03, 0, 50, 1, 0);
	uint8_t svc = 0;
	CHECK(vqm_service_get(&s, 0x03) != NULL);
	CHECK(vqm_service_by_port(&s, &svc, 50) != NULL && svc == 0x03);
	vqm_service_clear(&s, 0x03, 51);
	CHECK(vqm_service_get(&s, 0x03) != NULL);
	vqm_service_clear(&s, 0x03, 50);
	CHECK(vqm_service_get(&s, 0x03) == NULL);
}

static void
test_session(void)
{
	/* The whole sequence the shim will drive, without a device: version info, allocate a NAS
	 * client, forward a request (with the modem's own transaction), route the reply back,
	 * deliver an indication, and answer a silent request locally. */
	struct vqm_state s;
	struct bytes req, rsp;
	struct vqm_frame f, rf, out_frame;
	uint8_t value = 0x03;
	uint8_t client = 0;
	uint16_t error = 0;

	vqm_state_init(&s, false);
	vqm_service_set(&s, 0x03, 0, 50, 1, 0);

	req = control_request(&s, 0x2001, 0x0021, NULL, 0);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(response_result(&rf, &error) == 0);

	req = control_request(&s, 0x2002, 0x0022, &value, 1);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	const uint8_t *alloc;
	uint16_t alloc_len;
	CHECK(vqm_tlv_find(rf.qmi + 7, rf.qmi_len, 0x01, &alloc, &alloc_len) == 1);
	client = alloc[0];

	/* The client sends NAS Get Serving System (0x0024) with transaction 7. */
	uint8_t tlv[4] = { 0x01, 0x01, 0x00, 0x03 };
	struct bytes qmi = qmi_request(0x0007, 0x0024, tlv, sizeof(tlv));
	struct bytes frame = qmux6(0x03, client, 0x00, &qmi);

	CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
	uint16_t modem_txn = 0;
	CHECK(vqm_txn_begin(&s, &f, 5000, &modem_txn) == 0);
	CHECK(modem_txn != 0x0007);

	/* The modem answers on the service port, transaction modem_txn. */
	uint8_t modem_reply[14] = { 0x02, 0, 0, 0x24, 0x00, 0x07, 0x00,
	    0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
	put16le(modem_reply + 1, modem_txn);
	uint16_t ap = 0;
	uint8_t back_client = 0, style = 0, mode = 0, ctl = 0;
	CHECK(vqm_txn_complete(&s, 0x03, modem_txn, &ap, &back_client, &style, &mode, &ctl) == 1);
	CHECK(ap == 0x0007 && back_client == client);

	rsp.n = vqm_build(rsp.b, sizeof(rsp.b), style, mode, ctl, 0x03, back_client,
	    modem_reply[0], modem_reply, sizeof(modem_reply));
	CHECK(rsp.n > 0);
	/* The kernel's vqm_frame_record does this rewrite after the copy. */
	vqm_set_transaction(rsp.b + style, 0x0007);
	CHECK(vqm_parse(rsp.b, rsp.n, &out_frame) == 0);
	CHECK(out_frame.transaction == 0x0007 && out_frame.client == client);

	/* An indication for the service is wrapped for the client with the same framing. */
	struct vqm_client *c = vqm_client_find(&s, 0x03, client);
	CHECK(c != NULL);
	uint8_t indication[11] = { 0x04, 0x00, 0x00, 0x35, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x01 };
	rsp.n = vqm_build(rsp.b, sizeof(rsp.b), c->style, c->len_mode, c->ctl_flags, 0x03,
	    c->client, 0x04, indication, sizeof(indication));
	CHECK(rsp.n > 0);
	CHECK(vqm_parse(rsp.b, rsp.n, &out_frame) == 0);
	CHECK(out_frame.qmi_type == 0x04 && out_frame.client == client);

	/* A request the modem never answers: the thread builds this with vqm_reply. */
	struct vqm_frame silent;
	memset(&silent, 0, sizeof(silent));
	silent.style = 6;
	silent.len_mode = VQM_LEN_ALL_AFTER_VERSION;
	silent.service = 0x03;
	silent.client = client;
	silent.transaction = 0x0009;
	silent.message = 0x0024;
	uint8_t local[128];
	size_t local_len = vqm_reply(local, sizeof(local), &silent, 1, 3, NULL, 0);
	CHECK(local_len > 0);
	CHECK(vqm_parse(local, local_len, &out_frame) == 0);
	CHECK(out_frame.transaction == 0x0009);
	CHECK(response_result(&out_frame, &error) == 1 && error == 3);
}

/* ---- the kernel's own services 0xE1/0xE2 ------------------------------------------------- */

static uint8_t
allocate(struct vqm_state *s, uint16_t txn, uint8_t service, uint16_t *error)
{
	struct bytes req = control_request(s, txn, 0x0022, &service, 1);
	struct bytes rsp;
	struct vqm_frame rf;
	const uint8_t *alloc;
	uint16_t alloc_len;

	*error = 0xffff;
	if (control_call(s, &req, &rf, &rsp) != 0 || response_result(&rf, error) != 0) {
		return 0;
	}
	if (!vqm_tlv_find(rf.qmi + 7, rf.qmi_len, 0x01, &alloc, &alloc_len) || alloc_len != 2 || alloc[1] != service) {
		return 0;
	}
	return alloc[0];
}

static void
test_local_services(void)
{
	struct vqm_state s;
	struct bytes req, rsp;
	struct vqm_frame rf, f;
	uint16_t error = 0;
	const uint8_t *list;
	uint16_t list_len;

	/* Off (the default): 0xE1 and 0xE2 are refused like any unknown service and not listed. */
	vqm_state_init(&s, false);
	CHECK(!vqm_allowed(&s, 0xe1) && !vqm_allowed(&s, 0xe2) && !vqm_local(&s, 0xe1) && !vqm_local(&s, 0xe2));
	CHECK(allocate(&s, 0x0201, 0xe1, &error) == 0 && error == 94);
	CHECK(allocate(&s, 0x0202, 0xe2, &error) == 0 && error == 94);

	/* On: allowed, local, and listed in GET_VERSION_INFO after the six (and WDS). */
	s.local_e1 = true;
	s.local_bsp = true;
	s.wds = true;
	CHECK(vqm_allowed(&s, 0xe1) && vqm_local(&s, 0xe1) && vqm_allowed(&s, 0xe2) && vqm_local(&s, 0xe2));
	CHECK(!vqm_local(&s, 0x09) && !vqm_local(&s, 0x01));
	req = control_request(&s, 0x0203, 0x0021, NULL, 0);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(vqm_tlv_find(rf.qmi + 7, rf.qmi_len, 0x01, &list, &list_len) == 1);
	CHECK(list[0] == 9 && list_len == 1 + 9 * 5);
	bool saw_e1 = false, saw_e2 = false;
	for (unsigned i = 0; i < list[0]; i++) {
		saw_e1 |= list[1 + i * 5] == 0xe1 && list[2 + i * 5] == 1 && list[4 + i * 5] == 0;
		saw_e2 |= list[1 + i * 5] == 0xe2;
	}
	CHECK(saw_e1 && saw_e2);
	s.wds = false;

	uint8_t audio = allocate(&s, 0x0204, 0xe1, &error);
	uint8_t bsp = allocate(&s, 0x0205, 0xe2, &error);
	CHECK(audio != 0 && bsp != 0 && audio != bsp);

	/* 0xE1 SetAudioPath {downlink 0x10, uplink 0x11}: the kernel's path. f.qmi with qmi_len + 7 is the
	 * SDU a7's core reads; its answer is framed for the client and parses back as its response. */
	uint8_t path[10] = { 0x10, 0x02, 0x00, 0x03, 0x00, 0x11, 0x02, 0x00, 0x01, 0x00 };
	struct bytes qmi = qmi_request(0x0042, 0x0020, path, sizeof(path));
	struct bytes frame = qmux6(0xe1, audio, 0x00, &qmi);
	struct vayu_voice_bb_out out;
	struct vayu_voice_bb_effect effect;
	struct vqm_client *c;
	struct vqm_frame back;
	uint8_t built[256];
	size_t n;

	CHECK(vqm_parse(frame.b, frame.n, &f) == 0 && f.service == 0xe1 && f.client == audio);
	CHECK(vayu_voice_bb_audio_handle(f.qmi, (size_t)f.qmi_len + 7, &out, &effect) == 0);
	CHECK(effect.has_downlink && effect.downlink == 3 && effect.has_uplink && effect.uplink == 1);
	c = vqm_client_ensure(&s, f.service, f.client, f.style, f.len_mode, f.ctl_flags);
	CHECK(c != NULL && c->client == audio);
	n = vqm_build(built, sizeof(built), c->style, c->len_mode, c->ctl_flags, 0xe1, c->client, 0x02, out.answer,
	    out.answer_length);
	CHECK(n != 0 && vqm_parse(built, n, &back) == 0);
	CHECK(back.service == 0xe1 && back.client == audio && back.qmi_type == 0x02);
	CHECK(back.transaction == 0x0042 && back.message == 0x0020);
	CHECK(response_result(&back, &error) == 0 && error == 0);

	/* BurstDtmfTone: success, then the 0x2e indication for the same client. */
	qmi = qmi_request(0x0043, 0x0029, NULL, 0);
	frame = qmux6(0xe1, audio, 0x00, &qmi);
	CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
	CHECK(vayu_voice_bb_audio_handle(f.qmi, (size_t)f.qmi_len + 7, &out, &effect) == 0);
	CHECK(out.indication_length >= 7);
	n = vqm_build(built, sizeof(built), c->style, c->len_mode, c->ctl_flags, 0xe1, c->client, 0x04,
	    out.indication, out.indication_length);
	CHECK(n != 0 && vqm_parse(built, n, &back) == 0);
	CHECK(back.qmi_type == 0x04 && back.client == audio && back.message == 0x002e);

	/* An unknown 0xE1 request: answered, INVALID_QMI_CMD (never silence). */
	qmi = qmi_request(0x0044, 0x0077, NULL, 0);
	frame = qmux6(0xe1, audio, 0x00, &qmi);
	CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
	CHECK(vayu_voice_bb_audio_handle(f.qmi, (size_t)f.qmi_len + 7, &out, &effect) == 0);
	CHECK(out.error == 0x47);

	/* 0xE2: the two messages CommCenter sends succeed, the rest NOT_SUPPORTED, a non-request MALFORMED. */
	static const struct { uint16_t msg; uint16_t result, error; } bsp_cases[] = {
		{ 0x000b, 0, 0 }, { 0xb009, 0, 0 }, { 0x0001, 1, 94 }, { 0x000c, 1, 94 },
	};
	for (size_t i = 0; i < sizeof(bsp_cases) / sizeof(bsp_cases[0]); i++) {
		uint8_t answer[128];
		uint16_t said = 0xffff;
		uint8_t onoff[4] = { 0x01, 0x01, 0x00, 0x01 };

		qmi = qmi_request((uint16_t)(0x0300 + i), bsp_cases[i].msg, onoff, sizeof(onoff));
		frame = qmux6(0xe2, bsp, 0x00, &qmi);
		CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
		n = vqm_bsp_reply(answer, sizeof(answer), &f, &said);
		CHECK(n != 0 && vqm_parse(answer, n, &back) == 0);
		CHECK(back.service == 0xe2 && back.client == bsp && back.qmi_type == 0x02);
		CHECK(back.transaction == 0x0300 + i && back.message == bsp_cases[i].msg);
		CHECK(response_result(&back, &error) == bsp_cases[i].result && error == bsp_cases[i].error);
		CHECK(said == bsp_cases[i].error);
	}
	{
		uint8_t answer[128];
		uint16_t said = 0;
		struct bytes ind = qmi_request(0x0001, 0x000b, NULL, 0);

		ind.b[0] = 0x04;
		frame = qmux6(0xe2, bsp, 0x00, &ind);
		CHECK(vqm_parse(frame.b, frame.n, &f) == 0);
		n = vqm_bsp_reply(answer, sizeof(answer), &f, &said);
		CHECK(n != 0 && said == 1);
		CHECK(vqm_bsp_reply(answer, 8, &f, &said) == 0);       /* too small: nothing, not a short frame */
	}

	/* RELEASE_CLIENT_ID names the (service, client) the driver hands to vayu_voice_bb_release(). */
	uint8_t release[2] = { audio, 0xe1 };
	uint8_t rs = 0, rc = 0;
	req = control_request(&s, 0x0206, 0x0023, release, 2);
	CHECK(vqm_parse(req.b, req.n, &f) == 0);
	CHECK(vqm_control_release_target(&f, &rs, &rc) == 1 && rs == 0xe1 && rc == audio);
	CHECK(control_call(&s, &req, &rf, &rsp) == 0 && response_result(&rf, &error) == 0);
	CHECK(vqm_client_find(&s, 0xe1, audio) == NULL);
	req = control_request(&s, 0x0207, 0x0022, release, 2);  /* an allocation is not a release */
	CHECK(vqm_parse(req.b, req.n, &f) == 0 && vqm_control_release_target(&f, &rs, &rc) == 0);
	req = control_request(&s, 0x0208, 0x0023, NULL, 0);     /* no TLV: not a target */
	CHECK(vqm_parse(req.b, req.n, &f) == 0 && vqm_control_release_target(&f, &rs, &rc) == 0);
	qmi = qmi_request(0x0001, 0x0023, release, 2);           /* the same message id on a data service */
	frame = qmux6(0x09, 0x01, 0x00, &qmi);
	CHECK(vqm_parse(frame.b, frame.n, &f) == 0 && vqm_control_release_target(&f, &rs, &rc) == 0);
}

/* ---- per-client QRTR ports ------------------------------------------------------------------ */

static size_t
slot_of(struct vqm_state *s, uint8_t service, uint8_t client)
{
	struct vqm_client *c = vqm_client_find(s, service, client);

	return c == NULL ? VQM_CLIENTS : (size_t)(c - s->clients);
}

static void
test_per_client(void)
{
	struct vqm_state s;
	struct vqm_frame fa, fb, fw;
	struct vqm_dest d;
	uint16_t error = 0, ma = 0, mb = 0, mw = 0;
	uint8_t reg[4] = { 0x13, 0x01, 0x00, 0x01 };

	vqm_state_init(&s, true);
	/* CommCenter's two VOICE clients (QMICallAudioDriver's and QMIAudioRoutingCommandDriver's) and a WDS one. */
	uint8_t a = allocate(&s, 0x0401, 0x09, &error);
	uint8_t b = allocate(&s, 0x0402, 0x09, &error);
	uint8_t w = allocate(&s, 0x0403, 0x01, &error);
	CHECK(a != 0 && b != 0 && w != 0 && a != b);
	size_t sa = slot_of(&s, 0x09, a), sb = slot_of(&s, 0x09, b), sw = slot_of(&s, 0x01, w);
	CHECK(sa < VQM_CLIENTS && sb < VQM_CLIENTS && sw < VQM_CLIENTS && sa != sb);

	/* Both send Indication Register with the same AP transaction id 5; each gets its own modem id. */
	struct bytes qa = qmi_request(5, 0x0003, reg, sizeof(reg)), ra = qmux6(0x09, a, 0x00, &qa);
	struct bytes qb = qmi_request(5, 0x0003, reg, sizeof(reg)), rb = qmux6(0x09, b, 0x00, &qb);
	struct bytes qw = qmi_request(5, 0x00a2, NULL, 0), rw = qmux6(0x01, w, 0x00, &qw);
	CHECK(vqm_parse(ra.b, ra.n, &fa) == 0 && vqm_parse(rb.b, rb.n, &fb) == 0 && vqm_parse(rw.b, rw.n, &fw) == 0);
	CHECK(vqm_txn_begin(&s, &fa, 1000, &ma) == 0 && vqm_txn_begin(&s, &fb, 1000, &mb) == 0 &&
	    vqm_txn_begin(&s, &fw, 1000, &mw) == 0);
	CHECK(ma != mb && ma != mw && mb != mw);

	/* B's answer arrives first, on B's port: it is B's, with B's AP id back. */
	CHECK(vqm_route_own(&s, sb, 0x02, mb, &d) == 1);
	CHECK(d.matched && d.client == b && d.transaction == 5 && d.style == 6);
	/* The same id again (a duplicate): no transaction any more, still B's port, so B's, unchanged id. */
	CHECK(vqm_route_own(&s, sb, 0x02, mb, &d) == 1 && !d.matched && d.client == b && d.transaction == mb);
	/* A's answer on A's port. */
	CHECK(vqm_route_own(&s, sa, 0x02, ma, &d) == 1 && d.matched && d.client == a && d.transaction == 5);
	/* WDS's transaction is not completed by a VOICE port's answer that happens to carry its id. */
	CHECK(vqm_route_own(&s, sa, 0x02, mw, &d) == 1 && !d.matched && d.client == a);
	CHECK(vqm_route_own(&s, sw, 0x02, mw, &d) == 1 && d.matched && d.client == w && d.transaction == 5);

	/* An indication on A's port is A's alone (the shared port fanned it out to A and B). */
	CHECK(vqm_route_own(&s, sa, 0x04, 0x0000, &d) == 1 && d.client == a && !d.matched && d.transaction == 0);
	CHECK(vqm_route_own(&s, sb, 0x04, 0x0007, &d) == 1 && d.client == b && d.transaction == 7);

	/* Neither a response nor an indication: dropped. A request from the modem, too. */
	CHECK(vqm_route_own(&s, sa, 0x00, 1, &d) == 0);
	CHECK(vqm_route_own(&s, sa, 0x06, 1, &d) == 0);

	/* After B's release its slot routes nothing; out of range, nothing. A's is untouched. */
	uint8_t release[2] = { b, 0x09 };
	struct bytes req = control_request(&s, 0x0404, 0x0023, release, 2), rsp;
	struct vqm_frame rf;
	CHECK(control_call(&s, &req, &rf, &rsp) == 0);
	CHECK(vqm_route_own(&s, sb, 0x04, 0, &d) == 0);
	CHECK(vqm_route_own(&s, VQM_CLIENTS, 0x04, 0, &d) == 0);
	CHECK(vqm_route_own(&s, sa, 0x04, 0, &d) == 1 && d.client == a);

	/* A transaction still open when its client is released is gone with it. */
	struct bytes qc = qmi_request(9, 0x0020, NULL, 0), rc = qmux6(0x09, a, 0x00, &qc);
	struct vqm_frame fc;
	uint16_t mc = 0;
	CHECK(vqm_parse(rc.b, rc.n, &fc) == 0 && vqm_txn_begin(&s, &fc, 1000, &mc) == 0);
	CHECK(vqm_client_release(&s, 0x09, a) == 0);
	CHECK(vqm_route_own(&s, sa, 0x02, mc, &d) == 0);
	uint16_t ap;
	uint8_t client, style, mode, ctl;
	CHECK(vqm_txn_complete(&s, 0x09, mc, &ap, &client, &style, &mode, &ctl) == 0);

	/* A 32nd client fits (a port per client: up to VQM_CLIENTS), the 33rd does not. */
	vqm_state_init(&s, false);
	unsigned got = 0;
	for (unsigned i = 0; i < VQM_CLIENTS + 1; i++) {
		got += allocate(&s, (uint16_t)(0x0500 + i), 0x03, &error) != 0 ? 1 : 0;
	}
	CHECK(got == VQM_CLIENTS && error == 5);
}

int
main(void)
{
	test_header_layout();
	test_parse_shapes();
	test_build_roundtrip();
	test_fuzz();
	test_control();
	test_transactions();
	test_styles_and_error();
	test_session();
	test_local_services();
	test_per_client();

	printf("host/test_qmux: %u checks, %u failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
