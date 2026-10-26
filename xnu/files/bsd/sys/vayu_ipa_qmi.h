/* QMI IPA 4.1 messages used by vayu_ipa_qmi.c, from vayu-r-oss ipa_qmi_service_v01.[ch]. */
#ifndef _SYS_VAYU_IPA_QMI_H_
#define _SYS_VAYU_IPA_QMI_H_

#include <sys/vayu_cell_qmi.h>

#define VAYU_IPA_QMI_SERVICE            0x31
#define VAYU_IPA_QMI_INSTANCE           1
#define VAYU_IPA_MODEM_INSTANCE         2
#define VAYU_IPA_INIT_MODEM_DRIVER      0x0021
#define VAYU_IPA_INIT_COMPLETE_IND      0x0022
#define VAYU_IPA_INDICATION_REGISTER    0x0020
#define VAYU_IPA_DRIVER_INIT_COMPLETE   0x0035
#define VAYU_IPA_CONFIG_REQ             0x0027
#define VAYU_IPA_INSTALL_FILTER_RULE    0x0023
#define VAYU_IPA_FILTER_INSTALLED_NOTIF 0x0024
#define VAYU_IPA_PLATFORM_MSM_ANDROID   3

#define VAYU_IPA_EI(dt, st, field, tlv) \
	{ .data_type = (dt), .elem_len = 1, .elem_size = sizeof(((st *)0)->field), \
	  .array_type = NO_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, field) }
#define VAYU_IPA_OPT(st, field, tlv) VAYU_IPA_EI(QMI_OPT_FLAG, st, field, tlv)
#define VAYU_IPA_STRUCT(st, field, tlv, table) \
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(((st *)0)->field), \
	  .array_type = NO_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, field), .ei_array = (table) }
#define VAYU_IPA_RESULT(st) VAYU_IPA_STRUCT(st, result, 0x02, vayu_cell_result_ei)

struct vayu_ipa_offsets {
	uint32_t start, end;
};
static const struct qmi_elem_info vayu_ipa_offsets_ei[] = {
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_offsets, start, QMI_COMMON_TLV_TYPE),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_offsets, end, QMI_COMMON_TLV_TYPE),
	{ .data_type = QMI_EOTI },
};

struct vayu_ipa_route {
	uint32_t start, num_indices;
};
static const struct qmi_elem_info vayu_ipa_route_ei[] = {
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_route, start, QMI_COMMON_TLV_TYPE),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_route, num_indices, QMI_COMMON_TLV_TYPE),
	{ .data_type = QMI_EOTI },
};

struct vayu_ipa_memory {
	uint32_t start, size;
};
static const struct qmi_elem_info vayu_ipa_memory_ei[] = {
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_memory, start, QMI_COMMON_TLV_TYPE),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_memory, size, QMI_COMMON_TLV_TYPE),
	{ .data_type = QMI_EOTI },
};

struct vayu_ipa_init_req {
	uint8_t platform_type_valid;
	uint32_t platform_type;                 /* 0x10: 3 = MSM Android */
	uint8_t hdr_tbl_info_valid;
	struct vayu_ipa_offsets hdr_tbl_info;   /* 0x11 */
	uint8_t v4_route_tbl_info_valid;
	struct vayu_ipa_route v4_route_tbl_info; /* 0x12 */
	uint8_t v6_route_tbl_info_valid;
	struct vayu_ipa_route v6_route_tbl_info; /* 0x13 */
	uint8_t v4_filter_tbl_start_addr_valid;
	uint32_t v4_filter_tbl_start_addr;      /* 0x14 */
	uint8_t v6_filter_tbl_start_addr_valid;
	uint32_t v6_filter_tbl_start_addr;      /* 0x15 */
	uint8_t modem_mem_info_valid;
	struct vayu_ipa_memory modem_mem_info;  /* 0x16 */
	uint8_t ctrl_comm_dest_end_pt_valid;
	uint32_t ctrl_comm_dest_end_pt;         /* 0x17: vayu 11 */
	uint8_t is_ssr_bootup_valid;
	uint8_t is_ssr_bootup;                  /* 0x18: absent on the first boot */
	uint8_t hdr_proc_ctx_tbl_info_valid;
	struct vayu_ipa_offsets hdr_proc_ctx_tbl_info; /* 0x19 */
	uint8_t zip_tbl_info_valid;
	struct vayu_ipa_offsets zip_tbl_info;   /* 0x1a: absent on vayu */
	uint8_t v4_hash_route_tbl_info_valid;
	struct vayu_ipa_route v4_hash_route_tbl_info; /* 0x1b */
	uint8_t v6_hash_route_tbl_info_valid;
	struct vayu_ipa_route v6_hash_route_tbl_info; /* 0x1c */
	uint8_t v4_hash_filter_tbl_start_addr_valid;
	uint32_t v4_hash_filter_tbl_start_addr; /* 0x1d */
	uint8_t v6_hash_filter_tbl_start_addr_valid;
	uint32_t v6_hash_filter_tbl_start_addr; /* 0x1e */
	uint8_t hw_stats_quota_base_addr_valid;
	uint32_t hw_stats_quota_base_addr;      /* 0x1f */
	uint8_t hw_stats_quota_size_valid;
	uint32_t hw_stats_quota_size;           /* 0x20 */
	uint8_t hw_drop_stats_base_addr_valid;
	uint32_t hw_drop_stats_base_addr;       /* 0x21 */
	uint8_t hw_drop_stats_table_size_valid;
	uint32_t hw_drop_stats_table_size;      /* 0x22 */
};

static const struct qmi_elem_info vayu_ipa_init_req_ei[] = {
	VAYU_IPA_OPT(struct vayu_ipa_init_req, platform_type_valid, 0x10),
	VAYU_IPA_EI(QMI_SIGNED_4_BYTE_ENUM, struct vayu_ipa_init_req, platform_type, 0x10),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, hdr_tbl_info_valid, 0x11),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, hdr_tbl_info, 0x11, vayu_ipa_offsets_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v4_route_tbl_info_valid, 0x12),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, v4_route_tbl_info, 0x12, vayu_ipa_route_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v6_route_tbl_info_valid, 0x13),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, v6_route_tbl_info, 0x13, vayu_ipa_route_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v4_filter_tbl_start_addr_valid, 0x14),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, v4_filter_tbl_start_addr, 0x14),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v6_filter_tbl_start_addr_valid, 0x15),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, v6_filter_tbl_start_addr, 0x15),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, modem_mem_info_valid, 0x16),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, modem_mem_info, 0x16, vayu_ipa_memory_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, ctrl_comm_dest_end_pt_valid, 0x17),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, ctrl_comm_dest_end_pt, 0x17),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, is_ssr_bootup_valid, 0x18),
	VAYU_IPA_EI(QMI_UNSIGNED_1_BYTE, struct vayu_ipa_init_req, is_ssr_bootup, 0x18),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, hdr_proc_ctx_tbl_info_valid, 0x19),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, hdr_proc_ctx_tbl_info, 0x19, vayu_ipa_offsets_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, zip_tbl_info_valid, 0x1a),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, zip_tbl_info, 0x1a, vayu_ipa_offsets_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v4_hash_route_tbl_info_valid, 0x1b),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, v4_hash_route_tbl_info, 0x1b, vayu_ipa_route_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v6_hash_route_tbl_info_valid, 0x1c),
	VAYU_IPA_STRUCT(struct vayu_ipa_init_req, v6_hash_route_tbl_info, 0x1c, vayu_ipa_route_ei),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v4_hash_filter_tbl_start_addr_valid, 0x1d),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, v4_hash_filter_tbl_start_addr, 0x1d),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, v6_hash_filter_tbl_start_addr_valid, 0x1e),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, v6_hash_filter_tbl_start_addr, 0x1e),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, hw_stats_quota_base_addr_valid, 0x1f),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, hw_stats_quota_base_addr, 0x1f),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, hw_stats_quota_size_valid, 0x20),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, hw_stats_quota_size, 0x20),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, hw_drop_stats_base_addr_valid, 0x21),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, hw_drop_stats_base_addr, 0x21),
	VAYU_IPA_OPT(struct vayu_ipa_init_req, hw_drop_stats_table_size_valid, 0x22),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_req, hw_drop_stats_table_size, 0x22),
	{ .data_type = QMI_EOTI },
};

struct vayu_ipa_init_resp {
	struct vayu_cell_result result;
	uint8_t ctrl_comm_dest_end_pt_valid;
	uint32_t ctrl_comm_dest_end_pt;         /* 0x10 */
	uint8_t default_end_pt_valid;
	uint32_t default_end_pt;                /* 0x11 */
	uint8_t modem_driver_init_pending_valid;
	uint8_t modem_driver_init_pending;      /* 0x12 */
};
static const struct qmi_elem_info vayu_ipa_init_resp_ei[] = {
	VAYU_IPA_RESULT(struct vayu_ipa_init_resp),
	VAYU_IPA_OPT(struct vayu_ipa_init_resp, ctrl_comm_dest_end_pt_valid, 0x10),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_resp, ctrl_comm_dest_end_pt, 0x10),
	VAYU_IPA_OPT(struct vayu_ipa_init_resp, default_end_pt_valid, 0x11),
	VAYU_IPA_EI(QMI_UNSIGNED_4_BYTE, struct vayu_ipa_init_resp, default_end_pt, 0x11),
	VAYU_IPA_OPT(struct vayu_ipa_init_resp, modem_driver_init_pending_valid, 0x12),
	VAYU_IPA_EI(QMI_UNSIGNED_1_BYTE, struct vayu_ipa_init_resp, modem_driver_init_pending, 0x12),
	{ .data_type = QMI_EOTI },
};

struct vayu_ipa_init_complete_ind {
	struct vayu_cell_result master_driver_init_status; /* mandatory TLV 0x02 */
};
static const struct qmi_elem_info vayu_ipa_init_complete_ind_ei[] = {
	VAYU_IPA_STRUCT(struct vayu_ipa_init_complete_ind, master_driver_init_status, 0x02, vayu_cell_result_ei),
	{ .data_type = QMI_EOTI },
};

#endif /* _SYS_VAYU_IPA_QMI_H_ */
