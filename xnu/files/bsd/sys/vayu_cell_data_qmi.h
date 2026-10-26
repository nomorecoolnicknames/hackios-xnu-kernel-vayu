/*
 * QMI messages for the vayu cellular data-session control plane.
 * Wire layouts: libqmi data/qmi-service-{dpm,wda,wds}.json (master, read 2026-10-26).
 * vayu IPA endpoint mapping: downstream vayu-r-oss ipa3_ep_mapping[IPA_4_1], not SM7325.
 */
#ifndef _SYS_VAYU_CELL_DATA_QMI_H_
#define _SYS_VAYU_CELL_DATA_QMI_H_

#include <sys/vayu_cell_qmi.h>

#define VAYU_CELL_WDS_SERVICE       0x01
#define VAYU_CELL_WDA_SERVICE       0x1a
#define VAYU_CELL_DPM_SERVICE       0x2f

#define VAYU_CELL_DPM_OPEN_PORT                 0x0020
#define VAYU_CELL_WDA_SET_DATA_FORMAT           0x0020
#define VAYU_CELL_WDS_START_NETWORK_INTERFACE   0x0020
#define VAYU_CELL_WDS_STOP_NETWORK_INTERFACE    0x0021
#define VAYU_CELL_WDS_GET_CURRENT_SETTINGS      0x002d
#define VAYU_CELL_WDS_SET_IP_FAMILY             0x004d
#define VAYU_CELL_WDS_BIND_MUX_DATA_PORT        0x00a2

#define VAYU_CELL_ENDPOINT_EMBEDDED     4
#define VAYU_CELL_ENDPOINT_INTERFACE    1
#define VAYU_CELL_AP_TO_MODEM_ENDPOINT  2       /* IPA_CLIENT_APPS_WAN_PROD, GSI channel 3 */
#define VAYU_CELL_MODEM_TO_AP_ENDPOINT  11      /* IPA_CLIENT_APPS_WAN_CONS, GSI channel 6 */
#define VAYU_CELL_DATA_MUX              1
#define VAYU_CELL_IPV4                  4
#define VAYU_CELL_RAW_IP                2
#define VAYU_CELL_QMAP                  5
#define VAYU_CELL_APN_MAX               100
#define VAYU_CELL_SETTINGS_MASK         0x0000a310

#define VAYU_EI(dt, st, field, tlv) \
	{ .data_type = (dt), .elem_len = 1, .elem_size = sizeof(((st *)0)->field), \
	  .array_type = NO_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, field) }
#define VAYU_EI_OPT(st, field, tlv) VAYU_EI(QMI_OPT_FLAG, st, field, tlv)
#define VAYU_EI_STRUCT(st, field, tlv, table) \
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(((st *)0)->field), \
	  .array_type = NO_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, field), .ei_array = (table) }
#define VAYU_EI_RESULT(st) VAYU_EI_STRUCT(st, result, 0x02, vayu_cell_result_ei)

struct vayu_cell_endpoint_info {
	uint32_t endpoint_type;
	uint32_t interface_number;
};

static const struct qmi_elem_info vayu_cell_endpoint_info_ei[] = {
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_endpoint_info, endpoint_type, QMI_COMMON_TLV_TYPE),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_endpoint_info, interface_number, QMI_COMMON_TLV_TYPE),
	{ .data_type = QMI_EOTI },
};

/* DPM Open Port: one Hardware Data Ports element. RX is the modem's receiver (our TX),
 * TX is the modem's transmitter (our RX): vayu 2 and 11 respectively. */
struct vayu_cell_dpm_port {
	struct vayu_cell_endpoint_info endpoint;
	uint32_t rx_endpoint_number;
	uint32_t tx_endpoint_number;
};

struct vayu_cell_dpm_open_port_req {
	uint32_t ports_len;
	struct vayu_cell_dpm_port ports[1];
};

static const struct qmi_elem_info vayu_cell_dpm_port_ei[] = {
	VAYU_EI_STRUCT(struct vayu_cell_dpm_port, endpoint, QMI_COMMON_TLV_TYPE, vayu_cell_endpoint_info_ei),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_dpm_port, rx_endpoint_number, QMI_COMMON_TLV_TYPE),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_dpm_port, tx_endpoint_number, QMI_COMMON_TLV_TYPE),
	{ .data_type = QMI_EOTI },
};

static const struct qmi_elem_info vayu_cell_dpm_open_port_req_ei[] = {
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_cell_dpm_open_port_req, ports_len) },
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct vayu_cell_dpm_port),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_cell_dpm_open_port_req, ports), .ei_array = vayu_cell_dpm_port_ei },
	{ .data_type = QMI_EOTI },
};

/* WDA Set Data Format: raw IP in QMAP, no checksum offload, on the embedded port. */
struct vayu_cell_wda_set_format_req {
	uint32_t link_layer_protocol;           /* TLV 0x11 */
	uint32_t uplink_aggregation;            /* TLV 0x12 */
	uint32_t downlink_aggregation;          /* TLV 0x13 */
	uint32_t downlink_max_datagrams;        /* TLV 0x15 */
	uint32_t downlink_max_size;             /* TLV 0x16 */
	struct vayu_cell_endpoint_info endpoint; /* TLV 0x17 */
};

static const struct qmi_elem_info vayu_cell_wda_set_format_req_ei[] = {
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wda_set_format_req, link_layer_protocol, 0x11),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wda_set_format_req, uplink_aggregation, 0x12),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wda_set_format_req, downlink_aggregation, 0x13),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wda_set_format_req, downlink_max_datagrams, 0x15),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wda_set_format_req, downlink_max_size, 0x16),
	VAYU_EI_STRUCT(struct vayu_cell_wda_set_format_req, endpoint, 0x17, vayu_cell_endpoint_info_ei),
	{ .data_type = QMI_EOTI },
};

/* WDS Bind Mux Data Port. */
struct vayu_cell_wds_bind_req {
	struct vayu_cell_endpoint_info endpoint; /* TLV 0x10 */
	uint8_t mux_id;                         /* TLV 0x11 */
};

static const struct qmi_elem_info vayu_cell_wds_bind_req_ei[] = {
	VAYU_EI_STRUCT(struct vayu_cell_wds_bind_req, endpoint, 0x10, vayu_cell_endpoint_info_ei),
	VAYU_EI(QMI_UNSIGNED_1_BYTE, struct vayu_cell_wds_bind_req, mux_id, 0x11),
	{ .data_type = QMI_EOTI },
};

struct vayu_cell_wds_ip_family_req {
	uint8_t preference;                     /* TLV 0x01: 4 = IPv4 */
};

static const struct qmi_elem_info vayu_cell_wds_ip_family_req_ei[] = {
	VAYU_EI(QMI_UNSIGNED_1_BYTE, struct vayu_cell_wds_ip_family_req, preference, 0x01),
	{ .data_type = QMI_EOTI },
};

/* Start Network Interface: IP family, and APN only when the boot arg supplies one. */
struct vayu_cell_wds_start_req {
	uint8_t apn_valid;                      /* TLV 0x14, optional */
	char apn[VAYU_CELL_APN_MAX];
	uint8_t ip_family;                      /* TLV 0x19 */
};

static const struct qmi_elem_info vayu_cell_wds_start_req_ei[] = {
	VAYU_EI_OPT(struct vayu_cell_wds_start_req, apn_valid, 0x14),
	{ .data_type = QMI_STRING, .elem_len = VAYU_CELL_APN_MAX, .elem_size = sizeof(char),
	  .array_type = NO_ARRAY, .tlv_type = 0x14, .offset = offsetof(struct vayu_cell_wds_start_req, apn) },
	VAYU_EI(QMI_UNSIGNED_1_BYTE, struct vayu_cell_wds_start_req, ip_family, 0x19),
	{ .data_type = QMI_EOTI },
};

struct vayu_cell_wds_stop_req {
	uint32_t handle;                        /* TLV 0x01, from Start Network Interface */
};

static const struct qmi_elem_info vayu_cell_wds_stop_req_ei[] = {
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_stop_req, handle, 0x01),
	{ .data_type = QMI_EOTI },
};

struct vayu_cell_wds_start_resp {
	struct vayu_cell_result result;
	uint8_t handle_valid;                   /* TLV 0x01 */
	uint32_t handle;
	uint8_t call_end_reason_valid;          /* TLV 0x10 */
	uint16_t call_end_reason;
};

static const struct qmi_elem_info vayu_cell_wds_start_resp_ei[] = {
	VAYU_EI_RESULT(struct vayu_cell_wds_start_resp),
	VAYU_EI_OPT(struct vayu_cell_wds_start_resp, handle_valid, 0x01),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_start_resp, handle, 0x01),
	VAYU_EI_OPT(struct vayu_cell_wds_start_resp, call_end_reason_valid, 0x10),
	VAYU_EI(QMI_UNSIGNED_2_BYTE, struct vayu_cell_wds_start_resp, call_end_reason, 0x10),
	{ .data_type = QMI_EOTI },
};

struct vayu_cell_wds_settings_req {
	uint32_t requested_settings;            /* TLV 0x10: DNS/address/gateway/MTU/IP family */
};

static const struct qmi_elem_info vayu_cell_wds_settings_req_ei[] = {
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_settings_req, requested_settings, 0x10),
	{ .data_type = QMI_EOTI },
};

struct vayu_cell_wds_settings_resp {
	struct vayu_cell_result result;
	uint8_t dns1_valid;                     /* TLV 0x15 */
	uint32_t dns1;
	uint8_t dns2_valid;                     /* TLV 0x16 */
	uint32_t dns2;
	uint8_t address_valid;                  /* TLV 0x1e */
	uint32_t address;
	uint8_t gateway_valid;                  /* TLV 0x20 */
	uint32_t gateway;
	uint8_t mask_valid;                     /* TLV 0x21 */
	uint32_t mask;
	uint8_t mtu_valid;                      /* TLV 0x29 */
	uint32_t mtu;
	uint8_t ip_family_valid;                /* TLV 0x2b */
	uint8_t ip_family;
};

static const struct qmi_elem_info vayu_cell_wds_settings_resp_ei[] = {
	VAYU_EI_RESULT(struct vayu_cell_wds_settings_resp),
	VAYU_EI_OPT(struct vayu_cell_wds_settings_resp, dns1_valid, 0x15),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_settings_resp, dns1, 0x15),
	VAYU_EI_OPT(struct vayu_cell_wds_settings_resp, dns2_valid, 0x16),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_settings_resp, dns2, 0x16),
	VAYU_EI_OPT(struct vayu_cell_wds_settings_resp, address_valid, 0x1e),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_settings_resp, address, 0x1e),
	VAYU_EI_OPT(struct vayu_cell_wds_settings_resp, gateway_valid, 0x20),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_settings_resp, gateway, 0x20),
	VAYU_EI_OPT(struct vayu_cell_wds_settings_resp, mask_valid, 0x21),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_settings_resp, mask, 0x21),
	VAYU_EI_OPT(struct vayu_cell_wds_settings_resp, mtu_valid, 0x29),
	VAYU_EI(QMI_UNSIGNED_4_BYTE, struct vayu_cell_wds_settings_resp, mtu, 0x29),
	VAYU_EI_OPT(struct vayu_cell_wds_settings_resp, ip_family_valid, 0x2b),
	VAYU_EI(QMI_UNSIGNED_1_BYTE, struct vayu_cell_wds_settings_resp, ip_family, 0x2b),
	{ .data_type = QMI_EOTI },
};

#endif /* _SYS_VAYU_CELL_DATA_QMI_H_ */
