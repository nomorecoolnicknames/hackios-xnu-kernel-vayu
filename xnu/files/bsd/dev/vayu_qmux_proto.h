/*
 * The protocol core of /dev/vayu-qmux, with no kernel dependency at all: QMux frame
 * parsing and building, the QMux control service (0x00) answered locally, the client-id
 * table and the transaction rewrite table. bsd/dev/vayu_qmux.c is the device and QRTR
 * plumbing around it; host/test_qmux.c drives this same code with a fake modem.
 *
 * All buffers are the caller's; nothing here allocates or sleeps. The tables are fixed
 * size (the driver is a bridge for one CommCenter, not a general QMux server).
 */
#ifndef _VAYU_QMUX_PROTO_H_
#define _VAYU_QMUX_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/vayu_qmux.h>

#define VQM_CLIENTS             32
#define VQM_TXNS                96
#define VQM_SERVICES            8
#define VQM_TLV_MAX             512

/* The QMux header style a client used. The three forms seen in Apple's stack and libqmi:
 *   6: version, length16, control, service, client, QMI at 6
 *   5: version, length16, control, client,          QMI at 5 (control service only)
 *   7: version, iftype, length16, control, service, client, QMI at 7
 * `len_mode` records what the length field counted, because implementations differ:
 *   VQM_LEN_ALL_AFTER_VERSION: the two length bytes and everything after them (total - 1)
 *   VQM_LEN_PAYLOAD: control + service + client + QMI (total - header)
 *   VQM_LEN_CANONICAL: control + service + client + QMI for the 7-byte form (total - 4)
 */
enum { VQM_LEN_ALL_AFTER_VERSION = 0, VQM_LEN_PAYLOAD = 1, VQM_LEN_CANONICAL = 2 };

struct vqm_frame {
	uint16_t frame_len;             /* the whole QMux frame */
	uint16_t hdr_len;               /* 5, 6 or 7 */
	uint8_t  style;
	uint8_t  len_mode;
	uint8_t  ctl_flags;             /* the QMux control byte (0x80 on control frames) */
	uint8_t  service;
	uint8_t  client;
	uint8_t  qmi_type;
	uint16_t transaction;
	uint16_t message;
	uint16_t qmi_len;
	const uint8_t *qmi;             /* qmi_type .. TLVs */
};

struct vqm_client {
	uint8_t  used;
	uint8_t  service;
	uint8_t  client;
	uint8_t  style;
	uint8_t  len_mode;
	uint8_t  ctl_flags;
	uint32_t requests;
};

struct vqm_txn {
	uint8_t  used;
	uint8_t  service;
	uint8_t  client;
	uint8_t  style;
	uint8_t  len_mode;
	uint8_t  ctl_flags;
	uint16_t ap_txn;
	uint16_t modem_txn;
	uint16_t message;
	uint64_t deadline_ms;
};

struct vqm_service {
	uint8_t  service;
	uint8_t  present;
	uint16_t instance;
	uint32_t version;
	uint32_t node;
	uint32_t port;
};

struct vqm_counters {
	uint32_t frames_ap, frames_modem, control, forwarded, replies, indications;
	uint32_t timeouts, denied, unparsed, not_ready;
};

struct vqm_state {
	struct vqm_client clients[VQM_CLIENTS];
	struct vqm_txn txns[VQM_TXNS];
	struct vqm_service services[VQM_SERVICES];
	struct vqm_counters counters;
	uint16_t next_client;           /* the last client id handed out */
	uint16_t next_txn;              /* the last modem transaction id used */
	bool wds;                       /* vayu_qmux_wds=1 */
	bool local_e1;                  /* Apple's QMI Audio Service 0xE1 answered by the kernel (vayu_call_bb=1) */
	bool local_bsp;                 /* Apple's BSP service 0xE2 answered here (vayu_qmux_bsp=1) */
};

/* ---- framing ----------------------------------------------------------------------------- */

/* 0 or a negative errno. Nothing is read past `len`. */
int vqm_parse(const uint8_t *buf, size_t len, struct vqm_frame *out);
/* The whole QMux frame, or 0 if `cap` is too small. */
size_t vqm_build(uint8_t *out, size_t cap, uint8_t style, uint8_t len_mode, uint8_t ctl_flags,
    uint8_t service, uint8_t client, uint8_t qmi_type, const uint8_t *qmi, size_t qmi_len);
/* A QMI response (header + result TLV + caller's TLVs) wrapped in the request's QMux framing. */
size_t vqm_error(uint8_t *out, size_t cap, const struct vqm_frame *request, uint16_t result,
    uint16_t error);
/* A QMI response with extra TLVs already encoded (type,length,value...). */
size_t vqm_reply(uint8_t *out, size_t cap, const struct vqm_frame *request, uint16_t result,
    uint16_t error, const uint8_t *tlvs, size_t tlvs_len);

/* TLV lookup in a QMI message (the 7-byte header's payload). 1 found, 0 not. */
int vqm_tlv_find(const uint8_t *tlvs, size_t len, uint8_t type, const uint8_t **value,
    uint16_t *value_len);

/* Rewrites the transaction id of an encoded QMI message in place (bytes 1..2). */
void vqm_set_transaction(uint8_t *qmi, uint16_t transaction);

/* ---- whitelist --------------------------------------------------------------------------- */

#define VQM_SERVICE_AUDIO       0xe1    /* Apple's QMI Audio Service (sys/vayu_voice_bb.h) */
#define VQM_SERVICE_BSP         0xe2    /* Apple's BSP service: antennas, thermal, logging */

bool vqm_allowed(const struct vqm_state *s, uint8_t service);
/* A service that no modem port carries: its requests are answered in the kernel (0xE1, 0xE2). */
bool vqm_local(const struct vqm_state *s, uint8_t service);

/*
 * BSP 0xE2 (vayu_qmux_bsp=1). CommCenter's QMIAudioCommandDriver opens a client of it and sends
 * bsp::SetAntennaPreference 0x000b and bsp::SetAudioLoggingEnabled 0xb009 (20F66); this modem has no such service. Those two get success, anything else NOT_SUPPORTED,
 * a frame that is not a request MALFORMED. The whole QMux response is written to `out`; its
 * length is returned (0: `cap` too small).
 */
#define VQM_BSP_SET_ANTENNA_PREFERENCE  0x000b
#define VQM_BSP_SET_AUDIO_LOGGING       0xb009
size_t vqm_bsp_reply(uint8_t *out, size_t cap, const struct vqm_frame *request, uint16_t *error);

/* ---- client ids -------------------------------------------------------------------------- */

struct vqm_client *vqm_client_find(struct vqm_state *s, uint8_t service, uint8_t client);
struct vqm_client *vqm_client_ensure(struct vqm_state *s, uint8_t service, uint8_t client,
    uint8_t style, uint8_t len_mode, uint8_t ctl_flags);
int vqm_client_alloc(struct vqm_state *s, uint8_t service, uint8_t style, uint8_t len_mode,
    uint8_t ctl_flags, uint8_t *client);
int vqm_client_release(struct vqm_state *s, uint8_t service, uint8_t client);
void vqm_clients_clear(struct vqm_state *s);

/* ---- transactions ------------------------------------------------------------------------ */

/* Remembers one AP request and gives it a modem-unique transaction id. 0 or -ENOSPC. */
int vqm_txn_begin(struct vqm_state *s, const struct vqm_frame *f, uint64_t deadline_ms,
    uint16_t *modem_txn);
/* A modem response for (service, modem_txn): the AP transaction and client to restore. 1 or 0. */
int vqm_txn_complete(struct vqm_state *s, uint8_t service, uint16_t modem_txn, uint16_t *ap_txn,
    uint8_t *client, uint8_t *style, uint8_t *len_mode, uint8_t *ctl_flags);
/* The oldest expired request, if one is due: 1 and its fields, else 0. */
int vqm_txn_expire(struct vqm_state *s, uint64_t now_ms, uint8_t *service, uint16_t *ap_txn,
    uint16_t *message, uint8_t *client, uint8_t *style, uint8_t *len_mode, uint8_t *ctl_flags);
/* Drops every request of one client (release or close). Returns how many. */
unsigned vqm_txn_cancel_client(struct vqm_state *s, uint8_t service, uint8_t client);

/* ---- control service 0x00 ---------------------------------------------------------------- */

/* Handles a control frame. 1 handled (out/out_len are a whole QMux response), 0 not control,
 * a negative errno if the frame is control but malformed. */
int vqm_control(struct vqm_state *s, const struct vqm_frame *f, uint8_t *out, size_t cap,
    size_t *out_len);
/* 1 and the (service, client) it names if f is a well-formed RELEASE_CLIENT_ID request, else 0. */
int vqm_control_release_target(const struct vqm_frame *f, uint8_t *service, uint8_t *client);

/* ---- per-client QRTR ports (vayu_qmux_per_client=1) --------------------------------------- */

/*
 * QMI over QRTR tells clients apart by their QRTR port, not by a QMux client id: Bind
 * Subscription, Bind Mux Data Port and Indication Register are the state of the port they came
 * from. With one port per QMux client (the driver keeps it beside clients[i]), a datagram that
 * comes in on clients[own]'s port is for that client only: a response goes to the client whose
 * transaction it completes (its AP transaction id restored), else to the port's own client with
 * the modem's id; an indication goes to the port's client and to no other. 1 and *d, or 0 if the
 * client is gone or the type is neither (the datagram is dropped).
 */
struct vqm_dest {
	uint8_t  client, style, len_mode, ctl_flags;
	uint16_t transaction;
	bool     matched;               /* a response that completed a transaction of ours */
};

int vqm_route_own(struct vqm_state *s, size_t own, uint8_t qmi_type, uint16_t modem_txn,
    struct vqm_dest *d);

/* ---- services ---------------------------------------------------------------------------- */

struct vqm_service *vqm_service_get(struct vqm_state *s, uint8_t service);
struct vqm_service *vqm_service_by_port(struct vqm_state *s, uint8_t *service, uint32_t port);
void vqm_service_set(struct vqm_state *s, uint8_t service, uint32_t node, uint32_t port,
    uint32_t version, uint32_t instance);
void vqm_service_clear(struct vqm_state *s, uint8_t service, uint32_t port);
void vqm_state_init(struct vqm_state *s, bool wds);

/* ---- WDS Bind Mux Data Port: the data port of this SoC ----------------------------------- */

/*
 * CommCenter binds its WDS client to the data port of an iPhone's modem (Apple's QMI library sends
 * WDS Bind Mux Data Port 0x00a2 with TLV 0x10 PeripheralEndPoint {ep_type u32, iface_id u32} and
 * TLV 0x11 MuxId u8). This modem's data port is the IPA's embedded one, opened by the kernel
 * (vayu_cell_data.c, DPM Open Port + WDA Set Data Format), and the kernel's packet path carries
 * one mux. vqm_wds_bind_rewrite() puts that port and mux into the request in place, so the
 * session CommCenter starts on that client is carried by the IPA; what CommCenter asked for is
 * returned in *was. QMI is the 7-byte QMI header and its TLVs. 1 rewritten, 0 not a bind request,
 * a negative errno if it is one but malformed (nothing is changed then).
 */
#define VQM_WDS_BIND_MUX_DATA_PORT      0x00a2
#define VQM_WDS_TLV_ENDPOINT            0x10
#define VQM_WDS_TLV_MUX_ID              0x11

struct vqm_wds_bind {
	bool     had_endpoint, had_mux;
	uint32_t ep_type, iface_id;     /* as CommCenter sent them */
	uint8_t  mux;
};

int vqm_wds_bind_rewrite(uint8_t *qmi, size_t qmi_len, uint32_t ep_type, uint32_t iface_id,
    uint8_t mux, struct vqm_wds_bind *was);

#endif /* _VAYU_QMUX_PROTO_H_ */
