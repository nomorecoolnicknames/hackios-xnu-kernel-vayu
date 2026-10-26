/*
 * Wi-Fi's second step on the POCO X3 Pro (vayu, SM8150-AC): the two services of ours the modem's
 * firmware looks for before it starts its WLAN firmware, and a read-only look at WLFW.
 *
 * On the phone WLFW (QMI service 0x45) runs on the modem processor (MPSS). Before its WLAN firmware
 * starts, Linux gives the modem a pd-mapper (the SERVREG locator, QMI service 0x40) and a tftp server
 * (QRTR service 4096) from which it reads wlanmdsp.mbn. This file does the same two things, read-only,
 * and then asks WLFW four questions and no more. Everything is behind boot-arg vayu_wifi=N.
 *
 * Facts and where they come from:
 *   - WLFW is QMI service 0x45 on the modem (the bring-up notes); its message ids,
 *     structures and element tables are the ones of ref/qmi_wlfw_v01.h and ref/qmi_wlfw_v01.c, which
 *     are the WCN3990 definitions used by ath10k (ref/qmi.c).
 *   - The pd-mapper is the SERVREG locator: service 0x40 (ref/pdm_servreg_loc.qmi:3). libqrtr publishes
 *     version 0x101 instance 0 (ref/pdm_pd-mapper.c:422-423); on the wire 0x101 is version 1 instance 1,
 *     and Linux's pdr_interface.c looks it up as version 1 instance 1. We publish that.
 *   - The domain table below is the stock vayu modem partition's firmware/vayu/modem/*.jsn: sr_domain's
 *     soc/domain/subdomain and sr_service's provider/service, with qmi_instance_id (ref/pdm_pd-mapper.c:
 *     122-194). The .jsn files named in the table are the ones on that partition.
 *   - The tftp service is QRTR service 4096 version 1 instance 0 (VAYU_WIFI_PHASE0.md W5;
 *     ref/tqftpserv.c:965). Its options and their handling are those of ref/tqftpserv.c
 *     (parse_options, handle_rrq, tftp_send_oack, tftp_send_data, handle_reader), and its paths
 *     those of ref/translate.c's read-only part.
 *   - MSA: the stock device tree's icnss node has qcom,wlan-msa-memory = <0x100000>, and the fixed
 *     region pil_wlan_fw_region@8bc00000 is where it lives. WLFW's host_cap daemon_support, the
 *     ind_register client id 0x4b4e454c and the four requests are ath10k's (ref/qmi.c:25, 121-197,
 *     545-806, 964-1133).
 *   - hakios_image_file("fw", name, &size, NULL) is the boot image's fw/ directory, whole in memory,
 *     never freed.
 *
 * Boot-args (read in vayu_wifi_start()):
 *
 *   vayu_wifi=N          0: nothing (the default); 1: the pd-mapper and the tftp server; 2: also the
 *                        WLFW probe.
 *   vayu_wifi_hostcap=N  1 (default): host_cap with the normal table; 2: the 8-bit table; 0: no
 *                        host_cap at all.
 *   vayu_wifi_cap=0      do not send the cap request (default 1: send it).
 *
 * What it never does: no BDF_DOWNLOAD, no CAL_* request, no WLAN_CFG, no WLAN_MODE, no
 * INI, no ATHDIAG_*, no MAC_ADDR, no RESPOND_MEM, no SCM call and no SMEM write. It never writes to
 * any storage (the tftp server is read-only and a WRQ is refused). The only bytes that go to the
 * modem are answers to what it asked and the four read-only WLFW requests above. (Linux's
 * ath10k_qmi_add_wlan_ver_smem writes the firmware build id to SMEM; that is not done here.)
 *
 * What may be seen:
 *   H1: the modem reads wlanmdsp.mbn over tftp and then WLFW (0x45) appears -- then the firmware
 *       waited for these two services, and the WLFW probe's answers are read from a live service.
 *   H2: 0x45 appears without any tftp read -- the pd-mapper (or something else) was enough, or the
 *       firmware did not need the file this time.
 *   H3: nothing is asked at all and 0x45 never appears -- the modem waits for something else, for
 *       example rmtfs (QMI service 14; VAYU_MODEM_PHASE0.md §3), which this file does not serve either.
 *
 * The tftp server is in the link thread (one callback at a time); its transfer table is shared with
 * vayu_wifi_stop() and is protected by vayu_wifi_lock. No lock is held across a send or a close.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.qmi.h>
#include <sys/systm.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_wifi.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
/* The boot image's fw/ directory, whole and in memory that stays: never freed. */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* Every line to the loader's log as well: what is done to the modem should be known even if the kernel dies. */
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

/* ---- the file's own lock and counters ------------------------------------------------------------ */

static LCK_GRP_DECLARE(vayu_wifi_lck_grp, "vayu-wifi");
static LCK_MTX_DECLARE(vayu_wifi_lock, &vayu_wifi_lck_grp);

#define VAYU_PDM_SERVICE        0x40    /* SERVREG locator (ref/pdm_servreg_loc.qmi:3) */
#define VAYU_PDM_GET_DOMAIN_LIST 0x21
#define VAYU_PDM_PFR            0x24
#define VAYU_TFTP_SERVICE       4096    /* QRTR tftp (ref/tqftpserv.c:965) */
#define VAYU_WLFW_SERVICE       0x45
#define VAYU_WLFW_VERSION       1
#define VAYU_WLFW_INSTANCE      0

#define VAYU_PDM_LOG_MAX        32
#define VAYU_TFTP_LOG_MAX       48
#define VAYU_TFTP_XFERS         8

static uint32_t wifi_level, wifi_hostcap = 1, wifi_cap = 1;
static uint32_t wifi_msa_fixed_perm;      /* 1: TZ fixes the MSA permissions; skip the assign */
static uint32_t wifi_listen_s;
static bool wifi_started;
static bool wifi_stopping;

static struct hakios_qrtr_port *pdm_port;
static uint32_t pdm_requests, pdm_log_lines, pdm_busy;
static bool pdm_unknown_logged;
static uint8_t pdm_answer[4096];         /* only the link thread's one pd callback at a time uses it */

static struct hakios_qrtr_port *tftp_port;
static uint32_t tftp_reads, tftp_served, tftp_writes, tftp_log_lines, tftp_busy;
static uint32_t tftp_write_blocks;             /* DATA packets of the liveness probe, discarded */
static uint64_t tftp_write_bytes;
/*
 * The tftp liveness probe: the modem writes /readwrite/server_check.txt and
 * reads it back.  Keep the written payload in this kernel buffer (never on
 * storage) and serve the read from it; this is the tftp equivalent of the
 * rmtfs RAM overlay.  VAYU_TFTP_WRITE_MAX is far above the probe's size.
 */
#define VAYU_TFTP_WRITE_MAX 0x10000
static uint8_t tftp_write_file[VAYU_TFTP_WRITE_MAX];
static uint32_t tftp_write_file_size;
static char tftp_write_path[256];
static uint32_t tftp_write_path_len;
static uint16_t tftp_write_expected;
static bool tftp_write_active;
static uint64_t tftp_bytes;

/* ---- small parsing helpers ----------------------------------------------------------------------- */

/* The length of a NUL-terminated string inside `limit` bytes, or `limit` if there is no NUL. */
static size_t
vayu_wifi_strnlen(const char *s, size_t limit)
{
	size_t n = 0;

	while (n < limit && s[n] != 0) {
		n++;
	}
	return n;
}

/* A plain decimal, digits only (no sign, no space), at most `max`; false if it is not one. */
static bool
vayu_wifi_parse_uint(const char *s, uint64_t max, uint64_t *out)
{
	uint64_t value = 0;

	if (*s == 0) {
		return false;
	}
	for (; *s != 0; s++) {
		uint32_t digit;

		if (*s < '0' || *s > '9') {
			return false;
		}
		digit = (uint32_t)(*s - '0');
		if (value > (max - digit) / 10) {
			return false;
		}
		value = value * 10 + digit;
	}
	*out = value;
	return true;
}

/* ===================================================================================================
 * 1. pd-mapper: the SERVREG locator
 *
 * after ref/pdm_pd-mapper.c, ref/pdm_servreg_loc.c and ref/pdm_servreg_loc.qmi.
 * =================================================================================================== */

#define VAYU_PDM_NAME_LEN       64      /* SERVREG_NAME_LENGTH; the C string has the zero too */
#define VAYU_PDM_MAX_DOMAINS    32      /* the response's VAR_LEN_ARRAY */

struct vayu_pdm_domain_entry {
	char name[VAYU_PDM_NAME_LEN + 1];
	uint32_t instance_id;
	uint8_t service_data_valid;
	uint32_t service_data;
};

struct vayu_pdm_get_list_req {
	char name[VAYU_PDM_NAME_LEN + 1];
	uint8_t offset_valid;
	uint32_t offset;
};

struct vayu_pdm_get_list_resp {
	struct qmi_response_type_v01 result;
	uint8_t total_domains_valid;
	uint16_t total_domains;
	uint8_t db_revision_valid;
	uint16_t db_revision;
	uint8_t domain_list_valid;
	uint32_t domain_list_len;               /* a uint32_t in C; one byte on the wire (hakios.qmi.h) */
	struct vayu_pdm_domain_entry domain_list[VAYU_PDM_MAX_DOMAINS];
};

struct vayu_pdm_pfr_req {
	char service[VAYU_PDM_NAME_LEN + 1];
	char reason[256 + 1];
};

struct vayu_pdm_pfr_resp {
	struct qmi_response_type_v01 result;
};

static const struct qmi_elem_info vayu_pdm_domain_entry_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_PDM_NAME_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, name),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, instance_id),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, service_data_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, service_data),
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_get_list_req_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_PDM_NAME_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 1,
		.offset = offsetof(struct vayu_pdm_get_list_req, name),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_req, offset_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_req, offset),
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_get_list_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.tlv_type = 2,
		.offset = offsetof(struct vayu_pdm_get_list_resp, result),
		.ei_array = qmi_response_type_v01_ei,
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_resp, total_domains_valid),
	},
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_resp, total_domains),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x11,
		.offset = offsetof(struct vayu_pdm_get_list_resp, db_revision_valid),
	},
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.tlv_type = 0x11,
		.offset = offsetof(struct vayu_pdm_get_list_resp, db_revision),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_pdm_get_list_resp, domain_list_valid),
	},
	{
		.data_type = QMI_DATA_LEN,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_pdm_get_list_resp, domain_list_len),
	},
	{
		.data_type = QMI_STRUCT,
		.elem_len = VAYU_PDM_MAX_DOMAINS,
		.elem_size = sizeof(struct vayu_pdm_domain_entry),
		.array_type = VAR_LEN_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_pdm_get_list_resp, domain_list),
		.ei_array = vayu_pdm_domain_entry_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_pfr_req_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_PDM_NAME_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 1,
		.offset = offsetof(struct vayu_pdm_pfr_req, service),
	},
	{
		.data_type = QMI_STRING,
		.elem_len = 256 + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 2,
		.offset = offsetof(struct vayu_pdm_pfr_req, reason),
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_pfr_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.tlv_type = 2,
		.offset = offsetof(struct vayu_pdm_pfr_resp, result),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

/* FACT: firmware/vayu/modem/*.jsn of the stock vayu modem partition (ref/pdm_pd-mapper.c:122-194);
 * service = provider/service, domain = soc/domain/subdomain, instance = qmi_instance_id. */
static const struct {
	const char *service;
	const char *domain;
	uint32_t instance;
} vayu_pdm_domains[] = {
	{ "tms/servreg", "msm/modem/root_pd", 180 },		/* modemr.jsn */
	{ "tms/pdr_enabled", "msm/modem/root_pd", 180 },	/* modemr.jsn */
	{ "gps/gps_service", "msm/modem/root_pd", 180 },	/* modemr.jsn */
	{ "kernel/elf_loader", "msm/modem/wlan_pd", 180 },	/* modemuw.jsn */
	{ "tms/servreg", "msm/modem/wlan_pd", 180 },		/* modemuw.jsn */
	{ "wlan/fw", "msm/modem/wlan_pd", 180 },		/* modemuw.jsn */
	{ "tms/servreg", "msm/adsp/root_pd", 74 },		/* adspr.jsn */
	{ "tms/servreg", "msm/adsp/audio_pd", 74 },		/* adspua.jsn */
	{ "avs/audio", "msm/adsp/audio_pd", 74 },		/* adspua.jsn */
	{ "tms/servreg", "msm/cdsp/root_pd", 76 },		/* cdspr.jsn */
	{ "tms/servreg", "msm/slpi/root_pd", 90 },		/* slpir.jsn */
	{ "tms/servreg", "msm/slpi/sensor_pd", 90 },		/* slpius.jsn */
};

#define VAYU_PDM_DOMAIN_COUNT   (sizeof(vayu_pdm_domains) / sizeof(vayu_pdm_domains[0]))

/*
 * A GET_DOMAIN_LIST request. The answer is filled as ref/pdm_pd-mapper.c's handle_get_domain_list():
 * every entry whose service is the one asked for, `offset` matching entries skipped, total_domains
 * is the whole number of matches (not only those in this answer).
 */
static void
pdm_receive(void *context __unused, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	const uint8_t *message = data;
	struct hakios_qrtr_port *port;
	uint16_t msg_id, transaction;
	bool stopping, log;

	lck_mtx_lock(&vayu_wifi_lock);
	stopping = wifi_stopping;
	port = pdm_port;
	if (stopping || port == NULL) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	pdm_busy++;
	lck_mtx_unlock(&vayu_wifi_lock);

	if (length < QMI_HEADER_SIZE) {
		goto out;
	}
	if (message[0] != QMI_REQUEST) {
		goto out;
	}
	msg_id = (uint16_t)(message[3] | (message[4] << 8));
	transaction = (uint16_t)(message[1] | (message[2] << 8));

	{
		/* the first four requests in hex as well (run 033841: two requests that did not decode) */
		static unsigned raw_said;

		if (raw_said++ < 4) {
			char hex[3 * 48 + 1];
			size_t n = length < 48 ? length : 48;

			for (size_t i = 0; i < n; i++) {
				snprintf(hex + 3 * i, 4, " %02x", message[i]);
			}
			hex[3 * n] = 0;
			say("vayu-wifi: pd-mapper: request of %zu bytes:%s%s\n", length, hex, length > n ? " ..." : "");
		}
	}
	if (msg_id == VAYU_PDM_GET_DOMAIN_LIST) {
		struct vayu_pdm_get_list_req req = { 0 };
		struct vayu_pdm_get_list_resp resp = { 0 };
		unsigned int total = 0, first = 0, skip = 0, matching = 0, listed = 0;
		int ret;

		ret = hakios_qmi_decode(message, length, vayu_pdm_get_list_req_ei, &req);
		if (ret < 0) {
			req.name[0] = 0;
			resp.result.result = QMI_RESULT_FAILURE_V01;
			resp.result.error = QMI_ERR_MALFORMED_MSG_V01;
		} else {
			req.name[sizeof(req.name) - 1] = 0;
			for (unsigned int i = 0; i < VAYU_PDM_DOMAIN_COUNT; i++) {
				if (strcmp(vayu_pdm_domains[i].service, req.name) == 0) {
					if (total == 0) {
						first = i;
					}
					total++;
				}
			}
			resp.result.result = QMI_RESULT_SUCCESS_V01;
			resp.total_domains_valid = 1;
			resp.total_domains = (uint16_t)total;
			resp.db_revision_valid = 1;
			resp.db_revision = 1;
			skip = req.offset_valid ? req.offset : 0;
			for (unsigned int i = 0; i < VAYU_PDM_DOMAIN_COUNT; i++) {
				struct vayu_pdm_domain_entry *entry;

				if (strcmp(vayu_pdm_domains[i].service, req.name) != 0) {
					continue;
				}
				if (matching++ < skip) {
					continue;
				}
				if (listed >= VAYU_PDM_MAX_DOMAINS) {
					break;
				}
				entry = &resp.domain_list[listed++];
				snprintf(entry->name, sizeof(entry->name), "%s", vayu_pdm_domains[i].domain);
				entry->instance_id = vayu_pdm_domains[i].instance;
				entry->service_data_valid = 0;
				entry->service_data = 0;
			}
			if (listed != 0) {
				resp.domain_list_valid = 1;
				resp.domain_list_len = listed;
			}
		}

		lck_mtx_lock(&vayu_wifi_lock);
		pdm_requests++;
		log = pdm_log_lines < VAYU_PDM_LOG_MAX;
		if (log) {
			pdm_log_lines++;
		}
		lck_mtx_unlock(&vayu_wifi_lock);

		if (log) {
			char extra[96] = "";

			if (total != 0) {
				snprintf(extra, sizeof(extra), " first %s", vayu_pdm_domains[first].domain);
			}
			say("vayu-wifi: pd-mapper: node %u port %u asks for \"%s\": %u domain(s)%s\n", node,
			    from_port, req.name[0] != 0 ? req.name : "?", total, extra);
		}

		ret = hakios_qmi_encode(QMI_RESPONSE, VAYU_PDM_GET_DOMAIN_LIST, transaction,
		    vayu_pdm_get_list_resp_ei, &resp, pdm_answer, sizeof(pdm_answer));
		if (ret > 0) {
			vayu_qrtr_ops.send(port, node, from_port, pdm_answer, (size_t)ret);
		}
	} else if (msg_id == VAYU_PDM_PFR) {
		struct vayu_pdm_pfr_req req = { 0 };
		struct vayu_pdm_pfr_resp resp = { 0 };
		int ret;

		ret = hakios_qmi_decode(message, length, vayu_pdm_pfr_req_ei, &req);
		if (ret < 0) {
			req.service[0] = 0;
			req.reason[0] = 0;
			resp.result.result = QMI_RESULT_FAILURE_V01;
			resp.result.error = QMI_ERR_MALFORMED_MSG_V01;
		} else {
			req.service[sizeof(req.service) - 1] = 0;
			req.reason[sizeof(req.reason) - 1] = 0;
			resp.result.result = QMI_RESULT_SUCCESS_V01;
		}

		lck_mtx_lock(&vayu_wifi_lock);
		pdm_requests++;
		log = pdm_log_lines < VAYU_PDM_LOG_MAX;
		if (log) {
			pdm_log_lines++;
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (log) {
			say("vayu-wifi: pd-mapper: pfr: service \"%s\" reason \"%s\"\n", req.service, req.reason);
		}

		ret = hakios_qmi_encode(QMI_RESPONSE, VAYU_PDM_PFR, transaction, vayu_pdm_pfr_resp_ei,
		    &resp, pdm_answer, sizeof(pdm_answer));
		if (ret > 0) {
			vayu_qrtr_ops.send(port, node, from_port, pdm_answer, (size_t)ret);
		}
	} else {
		lck_mtx_lock(&vayu_wifi_lock);
		log = !pdm_unknown_logged;
		if (log) {
			pdm_unknown_logged = true;
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (log) {
			say("vayu-wifi: pd-mapper: node %u port %u sends message id %#x: not answered\n", node,
			    from_port, (unsigned int)msg_id);
		}
	}

out:
	lck_mtx_lock(&vayu_wifi_lock);
	pdm_busy--;
	lck_mtx_unlock(&vayu_wifi_lock);
}

/* ===================================================================================================
 * 2. tftp server: read-only
 *
 * after ref/tqftpserv.c (RRQ path, options, OACK, DATA, ACK) and ref/translate.c (paths).
 * =================================================================================================== */

#define VAYU_TFTP_BLKSIZE_DEFAULT       512
#define VAYU_TFTP_MIN_BLKSIZE           8
#define VAYU_TFTP_MAX_BLKSIZE           65464   /* RFC 2348, as ref/tqftpserv.c:23-24 */
#define VAYU_TFTP_MIN_WSIZE             1
#define VAYU_TFTP_MAX_WSIZE             65535   /* RFC 7440, as ref/tqftpserv.c:27-28 */
#define VAYU_TFTP_MAX_RSIZE             (100ULL * 1024 * 1024)
#define VAYU_TFTP_MIN_TIMEOUTMS         1
#define VAYU_TFTP_MAX_TIMEOUTMS         255000
#define VAYU_TFTP_NAME_MAX              63
#define VAYU_TFTP_PACKET_MAX            8192    /* hakios.qrtr.h: a send is up to 8192 bytes */
#define VAYU_TFTP_DATA_MAX              (VAYU_TFTP_PACKET_MAX - 4)
#define VAYU_TFTP_IDLE_NS               (10ULL * NSEC_PER_SEC)

enum {
	OP_RRQ = 1,
	OP_WRQ,
	OP_DATA,
	OP_ACK,
	OP_ERROR,
	OP_OACK,
};

/* RFC 1350 error codes used here (ref/tqftpserv.c:45-57). */
#define TFTP_ERROR_ENOENT       1
#define TFTP_ERROR_EACCESS      2
#define TFTP_ERROR_EBADOP       4
#define TFTP_ERROR_EOPTNEG      8

struct vayu_tftp_options {
	uint32_t blksize;
	uint32_t timeoutms;
	uint32_t wsize;
	uint32_t rsize;
	uint64_t seek;
	bool tsize;
	bool seek_given;
};

struct vayu_tftp_xfer {
	bool used;
	uint32_t busy;                          /* a callback or the first send is using it */
	struct hakios_qrtr_port *port;         /* the transfer's own port, NULL while opening */
	uint32_t node, client_port;
	const uint8_t *file;                    /* the boot image's memory: never freed */
	uint64_t size;
	uint64_t start;                         /* seek, clamped to size */
	uint64_t limit;                         /* bytes to send: rsize or the rest of the file */
	bool knows_length;                      /* rsize is the limit: the client knows the byte count */
	char name[VAYU_TFTP_NAME_MAX + 1];
	uint32_t blksize, wsize;
	bool ending;                            /* the last DATA packet has been sent */
	bool want_zero;                         /* ... after the ACK of this full block, send a zero one */
	bool sent_zero;
	uint64_t sent;                          /* file bytes in DATA packets */
	uint16_t next_block;
	uint16_t expected_ack;
	uint32_t blocks;                        /* DATA packets, the zero one included */
	uint64_t last_ns;
	/* set by handle_reader(), acted on once the callback's busy is given back */
	bool close_now;
	bool client_error;
	uint32_t client_error_code;
	char client_error_text[64];
};

static struct vayu_tftp_xfer tftp_xfer[VAYU_TFTP_XFERS];
static uint8_t tftp_packet[4 + VAYU_TFTP_DATA_MAX];
static uint8_t tftp_oack[512];

static bool
tftp_log_take(void)                             /* with vayu_wifi_lock held */
{
	if (tftp_log_lines >= VAYU_TFTP_LOG_MAX) {
		return false;
	}
	tftp_log_lines++;
	return true;
}

static uint16_t
tftp_get_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void
tftp_put_be16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static int
tftp_send_error(struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, uint16_t code,
    const char *text)
{
	size_t length = strlen(text) + 1;

	if (length > VAYU_TFTP_DATA_MAX) {
		length = VAYU_TFTP_DATA_MAX;
	}
	tftp_put_be16(tftp_packet, OP_ERROR);
	tftp_put_be16(tftp_packet + 2, code);
	memcpy(tftp_packet + 4, text, length);
	if (vayu_qrtr_ops.send(port, node, to_port, tftp_packet, 4 + length) != 0) {
		return -1;
	}
	return 0;
}

/* after ref/tqftpserv.c:167-205: one DATA packet, offset into the file, at most blksize bytes. */
/* A bare ACK; used by the write probe, whose payload is never stored. */
static int
tftp_send_ack(struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, uint16_t block)
{
	uint8_t ack[4];

	tftp_put_be16(ack, OP_ACK);
	tftp_put_be16(ack + 2, block);
	return vayu_qrtr_ops.send(port, node, to_port, ack, sizeof(ack));
}

static int
tftp_send_data(struct vayu_tftp_xfer *x, uint16_t block, uint64_t offset, uint32_t length)
{
	if (length > x->blksize || offset > x->size || length > x->size - offset) {
		return -1;
	}
	tftp_put_be16(tftp_packet, OP_DATA);
	tftp_put_be16(tftp_packet + 2, block);
	if (length != 0) {
		memcpy(tftp_packet + 4, x->file + offset, length);
	}
	if (vayu_qrtr_ops.send(x->port, x->node, x->client_port, tftp_packet, 4 + length) != 0) {
		return -1;
	}
	return 0;
}

/* One "name" "value" pair of an OACK; false if it does not fit. */
static bool
tftp_put_option(char **p, char *end, const char *name, uint64_t value)
{
	size_t length = strlen(name) + 1;
	char number[24];
	int n;

	if ((size_t)(end - *p) < length + sizeof(number)) {
		return false;
	}
	memcpy(*p, name, length);
	*p += length;
	n = snprintf(*p, (size_t)(end - *p), "%llu", (unsigned long long)value);
	if (n < 0 || (size_t)n >= (size_t)(end - *p)) {
		return false;
	}
	*p += n + 1;
	return true;
}

/* after ref/tqftpserv.c:218-309: the options asked for, in its order. */
static int
tftp_send_oack(struct vayu_tftp_xfer *x, const struct vayu_tftp_options *options)
{
	char *p = (char *)tftp_oack;
	char *end = (char *)tftp_oack + sizeof(tftp_oack);

	*p++ = 0;
	*p++ = OP_OACK;
	if (!tftp_put_option(&p, end, "blksize", x->blksize) ||
	    !tftp_put_option(&p, end, "timeoutms", options->timeoutms) ||
	    (options->tsize && !tftp_put_option(&p, end, "tsize", x->size)) ||
	    !tftp_put_option(&p, end, "wsize", options->wsize) ||
	    (options->rsize != 0 && !tftp_put_option(&p, end, "rsize", options->rsize)) ||
	    (options->seek_given && !tftp_put_option(&p, end, "seek", options->seek))) {
		return -1;
	}
	if (vayu_qrtr_ops.send(x->port, x->node, x->client_port, tftp_oack,
	    (size_t)(p - (char *)tftp_oack)) != 0) {
		return -1;
	}
	return 0;
}

/*
 * after ref/tqftpserv.c:311-415. An option value must be a NUL-terminated string inside the
 * datagram and a plain decimal in its range; an unknown option is ignored, as there.
 */
static int
parse_options(const char *p, const char *end, struct vayu_tftp_options *options)
{
	while (p < end) {
		const char *option = p;
		const char *value;
		size_t option_len, value_len;
		uint64_t parsed;

		option_len = vayu_wifi_strnlen(option, (size_t)(end - p));
		if (option_len == (size_t)(end - p)) {
			return -1;
		}
		p += option_len + 1;
		if (p >= end) {
			return -1;
		}
		value = p;
		value_len = vayu_wifi_strnlen(value, (size_t)(end - p));
		if (value_len == (size_t)(end - p)) {
			return -1;
		}
		p += value_len + 1;

		if (strcmp(option, "blksize") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_BLKSIZE, &parsed) ||
			    parsed < VAYU_TFTP_MIN_BLKSIZE) {
				return -1;
			}
			if (parsed > VAYU_TFTP_DATA_MAX) {
				parsed = VAYU_TFTP_DATA_MAX;    /* our QRTR datagram is at most 8192 bytes */
			}
			options->blksize = (uint32_t)parsed;
		} else if (strcmp(option, "timeoutms") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_TIMEOUTMS, &parsed) ||
			    parsed < VAYU_TFTP_MIN_TIMEOUTMS) {
				return -1;
			}
			options->timeoutms = (uint32_t)parsed;
		} else if (strcmp(option, "tsize") == 0) {
			if (!vayu_wifi_parse_uint(value, 0xffffffffffffffffULL, &parsed)) {
				return -1;
			}
			options->tsize = true;
		} else if (strcmp(option, "rsize") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_RSIZE, &parsed) || parsed < 1) {
				return -1;
			}
			options->rsize = (uint32_t)parsed;
		} else if (strcmp(option, "wsize") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_WSIZE, &parsed) || parsed < VAYU_TFTP_MIN_WSIZE) {
				return -1;
			}
			options->wsize = (uint32_t)parsed;
		} else if (strcmp(option, "seek") == 0) {
			if (!vayu_wifi_parse_uint(value, 0xffffffffffffffffULL, &parsed)) {
				return -1;
			}
			options->seek = parsed;
			options->seek_given = true;
		} else {
			/* an unknown option is ignored, as ref/tqftpserv.c:409-411 */
		}
	}
	return 0;
}

/*
 * after ref/translate.c:255-277, read-only part: one of the three prefixes, then one plain file
 * name. Returns 0 and the name, or -1 and a short reason.
 */
static int
tftp_translate(const char *path, char *name, size_t name_size, const char **why)
{
	static const char *const prefixes[] = {
		"/readonly/firmware/image/",
		"/readonly/vendor/firmware/",
		"/readonly/vendor/firmware_mnt/image/",
	};
	size_t path_len = strlen(path);

	for (unsigned int i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t prefix_len = strlen(prefixes[i]);
		const char *rest;

		if (path_len <= prefix_len || strncmp(path, prefixes[i], prefix_len) != 0) {
			continue;
		}
		rest = path + prefix_len;
		if (*rest == 0 || strchr(rest, '/') != NULL) {
			*why = "not a plain file name";
			return -1;
		}
		if (strlen(rest) > VAYU_TFTP_NAME_MAX) {
			*why = "name too long";
			return -1;
		}
		if (strcmp(rest, ".") == 0 || strcmp(rest, "..") == 0) {
			*why = "not a plain file name";
			return -1;
		}
		snprintf(name, name_size, "%s", rest);
		return 0;
	}
	*why = "path not served";
	return -1;
}

/*
 * after ref/tqftpserv.c:733-814 (handle_reader), with the rsize window understood as "when rsize is
 * the limit, stop once that many bytes have been acknowledged". ref/tqftpserv.c's loop uses
 * `(block + 1) * blksize > rsize` unguarded, which with rsize == 0 stops after one block and makes
 * wsize useless; here that test only applies when rsize is the limit (knows_length). If rsize is
 * larger than the rest of the file, the file's own end is marked the normal way (a short block or a
 * zero-length one).
 *
 * Sends at most wsize DATA packets and leaves expected_ack at the last one. A full block that ends
 * the data (and no rsize, so the client does not know the length) is followed by a zero-length block
 * on the next ACK; a short block ends the transfer by itself.
 */
static int
tftp_send_window(struct vayu_tftp_xfer *x)
{
	uint64_t sent = x->sent;
	uint64_t limit = x->limit;
	uint16_t block = x->next_block;
	uint32_t blocks = x->blocks;
	uint32_t window = x->wsize ? x->wsize : 1;
	bool ending = x->ending;
	bool want_zero = x->want_zero;
	bool sent_zero = x->sent_zero;
	int rc = 0;

	for (uint32_t i = 0; i < window; i++) {
		uint32_t length;
		uint64_t offset;

		if (sent >= limit) {
			if (blocks == 0 || (want_zero && !sent_zero)) {
				length = 0;
			} else {
				break;
			}
		} else {
			uint64_t left = limit - sent;

			length = left > x->blksize ? x->blksize : (uint32_t)left;
		}
		offset = x->start + sent;
		rc = tftp_send_data(x, block, offset, length);
		if (rc < 0) {
			break;
		}
		blocks++;
		x->expected_ack = block;
		block++;
		if (length == 0) {
			sent_zero = true;
			want_zero = false;
			ending = true;
			break;
		}
		sent += length;
		if (length < x->blksize) {
			ending = true;
			break;
		}
		if (sent >= limit) {
			if (x->knows_length) {
				ending = true;          /* the client knows rsize: no zero block */
				break;
			}
			want_zero = true;               /* the file ended on a block boundary */
		}
	}

	x->sent = sent;
	x->next_block = block;
	x->blocks = blocks;
	x->ending = ending;
	x->want_zero = want_zero;
	x->sent_zero = sent_zero;
	return rc;
}

static void
tftp_xfer_close(struct vayu_tftp_xfer *x, bool client_error, uint32_t error_code, const char *error_text)
{
	struct hakios_qrtr_port *port;
	char name[VAYU_TFTP_NAME_MAX + 1];
	uint64_t sent;
	uint32_t blocks;
	bool log;

	lck_mtx_lock(&vayu_wifi_lock);
	if (!x->used) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	if (x->busy != 0) {
		/* A callback of this port has it (a sweep can race with one); it will end it. */
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	port = x->port;
	x->port = NULL;
	x->used = false;
	memcpy(name, x->name, sizeof(name));
	sent = x->sent;
	blocks = x->blocks;
	tftp_bytes += sent;
	log = tftp_log_take();
	lck_mtx_unlock(&vayu_wifi_lock);

	if (port != NULL) {
		vayu_qrtr_ops.close(port);
	}
	if (log) {
		if (client_error) {
			say("vayu-wifi: tftp: \"%s\": sent %llu bytes in %u blocks (client error %u: %s)\n",
			    name, (unsigned long long)sent, blocks, error_code,
			    error_text != NULL ? error_text : "");
		} else {
			say("vayu-wifi: tftp: \"%s\": sent %llu bytes in %u blocks\n", name,
			    (unsigned long long)sent, blocks);
		}
	}
}

/* A transfer that has seen nothing for 10 s; called from the server port, no lock held. */
static void
tftp_sweep(void)
{
	uint64_t now;

	clock_get_uptime(&now);
	for (unsigned int i = 0; i < VAYU_TFTP_XFERS; i++) {
		struct vayu_tftp_xfer *x = &tftp_xfer[i];
		bool stale = false;

		lck_mtx_lock(&vayu_wifi_lock);
		if (x->used && x->busy == 0 && x->port != NULL && now > x->last_ns &&
		    now - x->last_ns > VAYU_TFTP_IDLE_NS) {
			stale = true;
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (stale) {
			tftp_xfer_close(x, false, 0, NULL);
		}
	}
}

/* The ACK/ERROR of one read transfer; called with x->busy held. */
static void
handle_reader(struct vayu_tftp_xfer *x, const uint8_t *data, size_t length)
{
	uint16_t opcode, block;
	size_t text_len;
	char text[64];

	if (length < 4) {
		x->close_now = true;
		return;
	}
	opcode = tftp_get_be16(data);
	if (opcode == OP_ERROR) {
		text_len = length - 4;
		if (text_len >= sizeof(text)) {
			text_len = sizeof(text) - 1;
		}
		memcpy(text, data + 4, text_len);
		text[text_len] = 0;
		x->close_now = true;
		x->client_error = true;
		x->client_error_code = tftp_get_be16(data + 2);
		snprintf(x->client_error_text, sizeof(x->client_error_text), "%s", text);
		return;
	}
	if (opcode != OP_ACK) {
		tftp_send_error(x->port, x->node, x->client_port, TFTP_ERROR_EBADOP, "Expected ACK opcode");
		x->close_now = true;
		return;
	}
	block = tftp_get_be16(data + 2);
	if (block != x->expected_ack) {
		return;                                 /* a duplicate or an old ACK: ignore it */
	}
	if (x->ending && !x->want_zero) {
		x->close_now = true;
		return;
	}
	if (tftp_send_window(x) < 0) {
		x->close_now = true;
	}
}

/* The transfer's own port; everything of that transfer is sent from it. */
static void
transfer_receive(void *context, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	struct vayu_tftp_xfer *x = context;
	bool close_now = false, client_error = false;
	uint32_t error_code = 0;
	char error_text[64] = "";

	lck_mtx_lock(&vayu_wifi_lock);
	if (wifi_stopping || !x->used || x->port == NULL || node != x->node || from_port != x->client_port) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	x->busy++;
	x->last_ns = 0;
	clock_get_uptime(&x->last_ns);
	lck_mtx_unlock(&vayu_wifi_lock);

	handle_reader(x, data, length);

	lck_mtx_lock(&vayu_wifi_lock);
	close_now = x->close_now;
	client_error = x->client_error;
	error_code = x->client_error_code;
	snprintf(error_text, sizeof(error_text), "%s", x->client_error_text);
	x->busy--;
	lck_mtx_unlock(&vayu_wifi_lock);

	if (close_now) {
		tftp_xfer_close(x, client_error, error_code, error_text);
	}
}

static void
tftp_handle_wrq(struct hakios_qrtr_port *port, uint32_t node, uint32_t from_port, const uint8_t *data,
    size_t length)
{
	const char *p = (const char *)data + 2;
	const char *end = (const char *)data + length;
	const char *filename;
	size_t filename_len, shown;
	bool log;

	filename = p;
	filename_len = vayu_wifi_strnlen(filename, (size_t)(end - p));
	if (filename_len == 0 || filename_len == (size_t)(end - p)) {
		filename = "<malformed>";
		filename_len = strlen(filename);
	}
	shown = filename_len > 100 ? 100 : filename_len;

	lck_mtx_lock(&vayu_wifi_lock);
	tftp_writes++;
	tftp_write_file_size = 0;
	tftp_write_expected = 1;
	tftp_write_active = true;
	tftp_write_path_len = filename_len > 256 ? 256 : (uint32_t)filename_len;
	memcpy(tftp_write_path, filename, tftp_write_path_len);
	tftp_write_path[tftp_write_path_len] = 0;
	log = tftp_log_take();
	lck_mtx_unlock(&vayu_wifi_lock);
	if (log) {
		say("vayu-wifi: tftp: node %u port %u writes \"%.*s\": accepted in RAM (kept for read)\n", node, from_port,
		    (int)shown, filename);
	}
	/*
	 * The modem writes /readwrite/server_check.txt as a liveness probe before
	 * it reads wlanmdsp.mbn.  ACK block 0 and then every DATA block on this
	 * same port; the payload stays in the received packet and is discarded.
	 * Nothing is opened or written on any storage.
	 */
	(void)tftp_send_ack(port, node, from_port, 0);
}

/*
 * after ref/tqftpserv.c:417-579 (handle_rrq and parse_options). The file is looked up in the boot
 * image's fw/ directory; nothing is opened on any storage.
 */
static void
tftp_handle_rrq(struct hakios_qrtr_port *port, uint32_t node, uint32_t from_port, const uint8_t *data,
    size_t length)
{
	const char *p = (const char *)data + 2;
	const char *end = (const char *)data + length;
	const char *filename, *mode;
	size_t filename_len, mode_len, shown;
	struct vayu_tftp_options options = {
		.blksize = VAYU_TFTP_BLKSIZE_DEFAULT,
		.timeoutms = 1000,
		.wsize = 1,
		.rsize = 0,
		.seek = 0,
		.tsize = false,
		.seek_given = false,
	};
	struct vayu_tftp_xfer *x = NULL;
	struct hakios_qrtr_port *transfer_port = NULL;
	const uint8_t *file = NULL;
	uint64_t size = 0;
	char name[VAYU_TFTP_NAME_MAX + 1] = "";
	char result[80] = "NOT SERVED";
	const char *why = "";
	const char *failure = "too many";
	bool have_options, bad_options = false, log, found = false, reserved = false, transfer_ready = false;
	int rc;

	filename = p;
	filename_len = vayu_wifi_strnlen(filename, (size_t)(end - p));
	if (filename_len == 0 || filename_len == (size_t)(end - p)) {
		return;                                 /* no NUL inside the datagram: not an RRQ we answer */
	}
	p += filename_len + 1;
	if (p >= end) {
		return;
	}
	mode = p;
	mode_len = vayu_wifi_strnlen(mode, (size_t)(end - p));
	if (mode_len == 0 || mode_len == (size_t)(end - p)) {
		return;
	}
	p += mode_len + 1;
	if (strcasecmp(mode, "octet") != 0) {
		lck_mtx_lock(&vayu_wifi_lock);
		tftp_reads++;
		log = tftp_log_take();
		lck_mtx_unlock(&vayu_wifi_lock);
		if (log) {
			say("vayu-wifi: tftp: node %u port %u reads \"%.*s\" (blksize %u tsize %s rsize %u seek %u "
			    "wsize %u): NOT SERVED (mode %s)\n", node, from_port, (int)filename_len, filename,
			    options.blksize, options.tsize ? "yes" : "no", options.rsize, 0, options.wsize,
			    mode);
		}
		tftp_send_error(port, node, from_port, TFTP_ERROR_EBADOP, "Only octet mode supported");
		return;
	}
	have_options = p < end;
	if (have_options && parse_options(p, end, &options) < 0) {
		bad_options = true;
	}

	if (!bad_options) {
		bool ram = false;

		lck_mtx_lock(&vayu_wifi_lock);
		if (tftp_write_file_size > 0 && tftp_write_path_len == (uint32_t)filename_len &&
		    memcmp(tftp_write_path, filename, filename_len) == 0) {
			file = tftp_write_file;
			size = tftp_write_file_size;
			snprintf(name, sizeof(name), "%s", tftp_write_path);
			ram = true;
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (ram) {
			found = true;
		} else if (tftp_translate(filename, name, sizeof(name), &why) != 0) {
			found = false;
		} else {
			file = hakios_image_file("fw", name, &size, NULL);
			found = file != NULL;
		}
	}

	if (bad_options) {
		snprintf(result, sizeof(result), "NOT SERVED (%s)", "bad options");
	} else if (file == NULL) {
		if (why[0] != 0) {
			snprintf(result, sizeof(result), "NOT SERVED (%s)", why);
		} else {
			snprintf(result, sizeof(result), "NOT FOUND");
		}
	} else if (found) {
		snprintf(result, sizeof(result), "%llu bytes", (unsigned long long)size);
	}

	if (!bad_options && found) {
		lck_mtx_lock(&vayu_wifi_lock);
		if (!wifi_stopping && tftp_port != NULL) {
			for (unsigned int i = 0; i < VAYU_TFTP_XFERS; i++) {
				if (!tftp_xfer[i].used) {
					x = &tftp_xfer[i];
					memset(x, 0, sizeof(*x));
					x->used = true;
					x->busy = 1;            /* the open and the first send, below */
					x->node = node;
					x->client_port = from_port;
					x->file = file;
					x->size = size;
					x->start = options.seek < size ? options.seek : size;
					x->limit = size > x->start ? size - x->start : 0;
					if (options.rsize != 0 && options.rsize <= x->limit) {
						x->limit = options.rsize;
						x->knows_length = true;
					}
					snprintf(x->name, sizeof(x->name), "%s", name);
					x->blksize = options.blksize;
					x->wsize = options.wsize;
					x->next_block = 1;
					x->expected_ack = 0;
					clock_get_uptime(&x->last_ns);
					reserved = true;
					break;
				}
			}
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (!reserved) {
			failure = wifi_stopping ? "stopping" : "too many";
			snprintf(result, sizeof(result), "NOT SERVED (%s)", failure);
		}
	}

	if (reserved) {
		transfer_port = vayu_qrtr_ops.open(transfer_receive, NULL, x);
		if (transfer_port == NULL) {
			lck_mtx_lock(&vayu_wifi_lock);
			x->used = false;
			x->busy = 0;
			lck_mtx_unlock(&vayu_wifi_lock);
			reserved = false;
			failure = "no port";
			snprintf(result, sizeof(result), "NOT SERVED (%s)", failure);
		} else {
			lck_mtx_lock(&vayu_wifi_lock);
			if (wifi_stopping) {
				x->used = false;
				x->busy = 0;
				lck_mtx_unlock(&vayu_wifi_lock);
				vayu_qrtr_ops.close(transfer_port);
				return;
			}
			x->port = transfer_port;
			tftp_served++;
			clock_get_uptime(&x->last_ns);
			lck_mtx_unlock(&vayu_wifi_lock);
			transfer_ready = true;
		}
	}

	shown = filename_len > 100 ? 100 : filename_len;
	lck_mtx_lock(&vayu_wifi_lock);
	tftp_reads++;
	log = tftp_log_take();
	lck_mtx_unlock(&vayu_wifi_lock);
	if (log) {
		say("vayu-wifi: tftp: node %u port %u reads \"%.*s\" (blksize %u tsize %s rsize %u seek %u "
		    "wsize %u): %s\n", node, from_port, (int)shown, filename, options.blksize,
		    options.tsize ? "yes" : "no", options.rsize, (unsigned int)options.seek, options.wsize,
		    result);
	}

	if (bad_options) {
		tftp_send_error(port, node, from_port, TFTP_ERROR_EOPTNEG, "Option negotiation failed");
		return;
	}
	if (!found) {
		tftp_send_error(port, node, from_port, TFTP_ERROR_ENOENT, "file not found");
		return;
	}
	if (!reserved) {
		tftp_send_error(port, node, from_port, 0, failure);
		return;
	}
	if (!transfer_ready) {
		lck_mtx_lock(&vayu_wifi_lock);
		x->busy = 0;
		lck_mtx_unlock(&vayu_wifi_lock);
		tftp_xfer_close(x, false, 0, NULL);
		return;
	}

	if (!have_options) {
		rc = tftp_send_window(x);
	} else {
		rc = tftp_send_oack(x, &options);
	}
	lck_mtx_lock(&vayu_wifi_lock);
	x->busy = 0;
	lck_mtx_unlock(&vayu_wifi_lock);
	if (rc < 0) {
		tftp_xfer_close(x, false, 0, NULL);
	}
}

/* The server port: RRQ and WRQ only; a read transfer then lives on its own port. */
static void
tftp_receive(void *context __unused, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	struct hakios_qrtr_port *port;
	uint16_t opcode;

	lck_mtx_lock(&vayu_wifi_lock);
	if (wifi_stopping || tftp_port == NULL) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	port = tftp_port;
	tftp_busy++;
	lck_mtx_unlock(&vayu_wifi_lock);

	tftp_sweep();
	if (length >= 2) {
		opcode = tftp_get_be16(data);
		if (opcode == OP_RRQ) {
			tftp_handle_rrq(port, node, from_port, data, length);
		} else if (opcode == OP_WRQ) {
			tftp_handle_wrq(port, node, from_port, data, length);
		} else if (opcode == OP_DATA) {
			/* The write probe's DATA arrives here; ACK it, store nothing. */
			if (length >= 4) {
				uint16_t block = tftp_get_be16((const uint8_t *)data + 2);
				size_t payload = length - 4;
				bool log;

				lck_mtx_lock(&vayu_wifi_lock);
				if (tftp_write_active && block == tftp_write_expected) {
					if (tftp_write_file_size + payload <= sizeof(tftp_write_file)) {
						memcpy(tftp_write_file + tftp_write_file_size, (const uint8_t *)data + 4, payload);
						tftp_write_file_size += (uint32_t)payload;
					}
					tftp_write_expected++;
					if (payload < 512) {
						tftp_write_active = false;
					}
				}
				tftp_write_blocks++;
				tftp_write_bytes += payload;
				log = tftp_log_take();
				lck_mtx_unlock(&vayu_wifi_lock);
				(void)tftp_send_ack(port, node, from_port, block);
				if (log) {
					say("vayu-wifi: tftp: write probe block %u, %zu bytes (RAM file %u bytes)\n",
					    block, payload, tftp_write_file_size);
				}
			}
		}
	}

	lck_mtx_lock(&vayu_wifi_lock);
	tftp_busy--;
	lck_mtx_unlock(&vayu_wifi_lock);
}

/* ===================================================================================================
 * 3. WLFW: the read-only probe
 *
 * after ref/qmi.c (ath10k's client) and ref/qmi_wlfw_v01.c / .h (message ids and tables). All the
 * tables and structures here are renamed vayu_wlfw_*; another file of the kernel has some wlfw_*.
 * =================================================================================================== */

#define VAYU_WLFW_CLIENT_ID             0x4b4e454c      /* ath10k's ATH10K_QMI_CLIENT_ID (ref/qmi.c:25) */
#define VAYU_WLFW_IND_REGISTER_REQ_V01  0x0020
#define VAYU_WLFW_FW_READY_IND_V01      0x0021
#define VAYU_WLFW_CAP_REQ_V01           0x0024
#define VAYU_WLFW_MSA_READY_IND_V01     0x002b
#define VAYU_WLFW_MSA_READY_REQ_V01     0x002e
#define VAYU_WLFW_PIN_CONNECT_RESULT_IND_V01 0x002c
#define VAYU_WLFW_MSA_INFO_REQ_V01      0x002d
#define VAYU_WLFW_HOST_CAP_REQ_V01      0x0034
#define VAYU_WLFW_FW_INIT_DONE_IND_V01  0x0038

#define VAYU_WLFW_IND_REGISTER_REQ_MAX  50
#define VAYU_WLFW_HOST_CAP_REQ_MAX      189
#define VAYU_WLFW_MSA_INFO_REQ_MAX      18
#define VAYU_WLFW_CAP_REQ_MAX           0
#define VAYU_WLFW_MSA_READY_REQ_MAX     0
#define VAYU_WLFW_MAX_MEM_REG_V01       2
#define VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 32
#define VAYU_WLFW_MAX_BUILD_ID_LEN_V01  128
#define VAYU_WLFW_MAX_NUM_GPIO_V01      32

#define VAYU_WLFW_FW_READY_V01          0x02ULL         /* ref/qmi_wlfw_v01.h:119 */
#define VAYU_WLFW_MSA_BASE              0x8bc00000ULL   /* stock DT pil_wlan_fw_region@8bc00000 */
#define VAYU_WLFW_MSA_END               0x8bd80000ULL
#define VAYU_WLFW_MSA_SIZE              0x100000U       /* stock DT icnss qcom,wlan-msa-memory = <0x100000> */

struct vayu_wlfw_memory_region_info_s_v01 {
	uint64_t region_addr;
	uint32_t size;
	uint8_t secure_flag;
};

struct vayu_wlfw_rf_chip_info_s_v01 {
	uint32_t chip_id;
	uint32_t chip_family;
};

struct vayu_wlfw_rf_board_info_s_v01 {
	uint32_t board_id;
};

struct vayu_wlfw_soc_info_s_v01 {
	uint32_t soc_id;
};

struct vayu_wlfw_fw_version_info_s_v01 {
	uint32_t fw_version;
	char fw_build_timestamp[VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 + 1];
};

struct vayu_wlfw_ind_register_req_msg_v01 {
	uint8_t fw_ready_enable_valid;
	uint8_t fw_ready_enable;
	uint8_t initiate_cal_download_enable_valid;
	uint8_t initiate_cal_download_enable;
	uint8_t initiate_cal_update_enable_valid;
	uint8_t initiate_cal_update_enable;
	uint8_t msa_ready_enable_valid;
	uint8_t msa_ready_enable;
	uint8_t pin_connect_result_enable_valid;
	uint8_t pin_connect_result_enable;
	uint8_t client_id_valid;
	uint32_t client_id;
	uint8_t request_mem_enable_valid;
	uint8_t request_mem_enable;
	uint8_t mem_ready_enable_valid;
	uint8_t mem_ready_enable;
	uint8_t fw_init_done_enable_valid;
	uint8_t fw_init_done_enable;
	uint8_t rejuvenate_enable_valid;
	uint32_t rejuvenate_enable;
	uint8_t xo_cal_enable_valid;
	uint8_t xo_cal_enable;
};

struct vayu_wlfw_ind_register_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint8_t fw_status_valid;
	uint64_t fw_status;
};

struct vayu_wlfw_host_cap_req_msg_v01 {
	uint8_t daemon_support_valid;
	uint32_t daemon_support;
	uint8_t wake_msi_valid;
	uint32_t wake_msi;
	uint8_t gpios_valid;
	uint32_t gpios_len;
	uint32_t gpios[VAYU_WLFW_MAX_NUM_GPIO_V01];
	uint8_t nm_modem_valid;
	uint8_t nm_modem;
	uint8_t bdf_support_valid;
	uint8_t bdf_support;
	uint8_t bdf_cache_support_valid;
	uint8_t bdf_cache_support;
	uint8_t m3_support_valid;
	uint8_t m3_support;
	uint8_t m3_cache_support_valid;
	uint8_t m3_cache_support;
	uint8_t cal_filesys_support_valid;
	uint8_t cal_filesys_support;
	uint8_t cal_cache_support_valid;
	uint8_t cal_cache_support;
	uint8_t cal_done_valid;
	uint8_t cal_done;
	uint8_t mem_bucket_valid;
	uint32_t mem_bucket;
	uint8_t mem_cfg_mode_valid;
	uint8_t mem_cfg_mode;
};

struct vayu_wlfw_host_cap_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct vayu_wlfw_msa_info_req_msg_v01 {
	uint64_t msa_addr;
	uint32_t size;
};

struct vayu_wlfw_msa_info_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint32_t mem_region_info_len;
	struct vayu_wlfw_memory_region_info_s_v01 mem_region_info[VAYU_WLFW_MAX_MEM_REG_V01];
};

struct vayu_wlfw_cap_req_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_cap_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint8_t chip_info_valid;
	struct vayu_wlfw_rf_chip_info_s_v01 chip_info;
	uint8_t board_info_valid;
	struct vayu_wlfw_rf_board_info_s_v01 board_info;
	uint8_t soc_info_valid;
	struct vayu_wlfw_soc_info_s_v01 soc_info;
	uint8_t fw_version_info_valid;
	struct vayu_wlfw_fw_version_info_s_v01 fw_version_info;
	uint8_t fw_build_id_valid;
	char fw_build_id[VAYU_WLFW_MAX_BUILD_ID_LEN_V01 + 1];
	uint8_t num_macs_valid;
	uint8_t num_macs;
};

struct vayu_wlfw_pin_connect_result_ind_msg_v01 {
	uint8_t pwr_pin_result_valid;
	uint32_t pwr_pin_result;
	uint8_t phy_io_pin_result_valid;
	uint32_t phy_io_pin_result;
	uint8_t rf_pin_result_valid;
	uint32_t rf_pin_result;
};

struct vayu_wlfw_fw_ready_ind_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_msa_ready_ind_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_msa_ready_req_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_msa_ready_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct vayu_wlfw_fw_init_done_ind_msg_v01 {
	char placeholder;
};

static const struct qmi_elem_info vayu_wlfw_memory_region_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_8_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint64_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_memory_region_info_s_v01, region_addr),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_memory_region_info_s_v01, size),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_memory_region_info_s_v01, secure_flag),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_rf_chip_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_rf_chip_info_s_v01, chip_id),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_rf_chip_info_s_v01, chip_family),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_rf_board_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_rf_board_info_s_v01, board_id),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_soc_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_soc_info_s_v01, soc_id),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_fw_version_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_fw_version_info_s_v01, fw_version),
	},
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_fw_version_info_s_v01, fw_build_timestamp),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_ind_register_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_ready_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_ready_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_download_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_download_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_update_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_update_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, msa_ready_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, msa_ready_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, pin_connect_result_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, pin_connect_result_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, client_id_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, client_id) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, request_mem_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, request_mem_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, mem_ready_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, mem_ready_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_init_done_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_init_done_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, rejuvenate_enable_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, rejuvenate_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, xo_cal_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, xo_cal_enable) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_ind_register_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_ind_register_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_resp_msg_v01, fw_status_valid) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_resp_msg_v01, fw_status) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_host_cap_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, wake_msi_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, wake_msi) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, gpios_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, gpios_len) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = VAYU_WLFW_MAX_NUM_GPIO_V01, .elem_size = sizeof(uint32_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, gpios) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, nm_modem_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, nm_modem) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_cache_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_cache_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_cache_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_cache_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_filesys_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_filesys_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_cache_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_cache_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_done_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_done) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1B, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_bucket_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1B, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_bucket) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1C, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_cfg_mode_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1C, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_cfg_mode) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_host_cap_8bit_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_host_cap_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_host_cap_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_msa_info_req_msg_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_8_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint64_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct vayu_wlfw_msa_info_req_msg_v01, msa_addr),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_msa_info_req_msg_v01, size),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_msa_info_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_msa_info_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{
		.data_type = QMI_DATA_LEN,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x03,
		.offset = offsetof(struct vayu_wlfw_msa_info_resp_msg_v01, mem_region_info_len),
	},
	{
		.data_type = QMI_STRUCT,
		.elem_len = VAYU_WLFW_MAX_MEM_REG_V01,
		.elem_size = sizeof(struct vayu_wlfw_memory_region_info_s_v01),
		.array_type = VAR_LEN_ARRAY,
		.tlv_type = 0x03,
		.offset = offsetof(struct vayu_wlfw_msa_info_resp_msg_v01, mem_region_info),
		.ei_array = vayu_wlfw_memory_region_info_s_v01_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_cap_req_msg_v01_ei[] = {
	{}
};

static const struct qmi_elem_info vayu_wlfw_cap_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, chip_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_rf_chip_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, chip_info),
		.ei_array = vayu_wlfw_rf_chip_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, board_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_rf_board_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x11,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, board_info),
		.ei_array = vayu_wlfw_rf_board_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, soc_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_soc_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, soc_info),
		.ei_array = vayu_wlfw_soc_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_version_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_fw_version_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x13,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_version_info),
		.ei_array = vayu_wlfw_fw_version_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_build_id_valid) },
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_WLFW_MAX_BUILD_ID_LEN_V01 + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x14,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_build_id),
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, num_macs_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, num_macs) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_pin_connect_result_ind_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, pwr_pin_result_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, pwr_pin_result) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, phy_io_pin_result_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, phy_io_pin_result) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, rf_pin_result_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, rf_pin_result) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_fw_ready_ind_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static const struct qmi_elem_info vayu_wlfw_msa_ready_ind_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static const struct qmi_elem_info vayu_wlfw_msa_ready_req_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static const struct qmi_elem_info vayu_wlfw_msa_ready_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_msa_ready_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_fw_init_done_ind_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static struct qmi_handle wlfw_handle;
static struct {
	volatile bool server;
	volatile bool running;
	uint32_t node, port;
	volatile bool stop;
	volatile bool fw_ready;         /* the FW_READY indication arrived */
	uint32_t chip_id, board_id, soc_id;
	char summary[96];
} wlfw;

static void
wlfw_fail(const char *step)
{
	snprintf(wlfw.summary, sizeof(wlfw.summary), "failed at %s", step);
}

static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int ret;

	say("vayu-wifi: wlfw: server at node %u port %u\n", service->node, service->port);
	ret = hakios_qmi_connect(qmi->sock, service->node, service->port);
	if (ret != 0) {
		say("vayu-wifi: wlfw: cannot connect to node %u port %u: %d\n", service->node, service->port, ret);
		return ret;
	}
	wlfw.node = service->node;
	wlfw.port = service->port;
	wlfw.server = true;
	return 0;
}

static void
wlfw_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service __unused)
{
	wlfw.server = false;
}

static void
wlfw_ind_fw_ready(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused, struct qmi_txn *txn __unused,
    const void *decoded __unused)
{
	say("vayu-wifi: wlfw: indication FW_READY\n");
	wlfw.fw_ready = true;
}

static void
wlfw_ind_msa_ready(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused, struct qmi_txn *txn __unused,
    const void *decoded __unused)
{
	say("vayu-wifi: wlfw: indication MSA_READY\n");
}

static void
wlfw_ind_fw_init_done(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused,
    struct qmi_txn *txn __unused, const void *decoded __unused)
{
	say("vayu-wifi: wlfw: indication FW_INIT_DONE\n");
}

static void
wlfw_ind_pin_connect_result(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused,
    struct qmi_txn *txn __unused, const void *decoded)
{
	const struct vayu_wlfw_pin_connect_result_ind_msg_v01 *ind = decoded;
	char pwr[16], phy[16], rf[16];

	if (ind->pwr_pin_result_valid) {
		snprintf(pwr, sizeof(pwr), "%u", ind->pwr_pin_result);
	} else {
		snprintf(pwr, sizeof(pwr), "?");
	}
	if (ind->phy_io_pin_result_valid) {
		snprintf(phy, sizeof(phy), "%u", ind->phy_io_pin_result);
	} else {
		snprintf(phy, sizeof(phy), "?");
	}
	if (ind->rf_pin_result_valid) {
		snprintf(rf, sizeof(rf), "%u", ind->rf_pin_result);
	} else {
		snprintf(rf, sizeof(rf), "?");
	}
	say("vayu-wifi: wlfw: indication PIN_CONNECT_RESULT: pwr_pin_result %s phy_io_pin_result %s "
	    "rf_pin_result %s\n", pwr, phy, rf);
}

static const struct qmi_msg_handler wlfw_handlers[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_FW_READY_IND_V01,
		.ei = vayu_wlfw_fw_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_fw_ready_ind_msg_v01),
		.fn = wlfw_ind_fw_ready,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_MSA_READY_IND_V01,
		.ei = vayu_wlfw_msa_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_msa_ready_ind_msg_v01),
		.fn = wlfw_ind_msa_ready,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_PIN_CONNECT_RESULT_IND_V01,
		.ei = vayu_wlfw_pin_connect_result_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_pin_connect_result_ind_msg_v01),
		.fn = wlfw_ind_pin_connect_result,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_FW_INIT_DONE_IND_V01,
		.ei = vayu_wlfw_fw_init_done_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_fw_init_done_ind_msg_v01),
		.fn = wlfw_ind_fw_init_done,
	},
	{}
};

static const struct qmi_ops wlfw_ops = {
	.new_server = wlfw_new_server,
	.del_server = wlfw_del_server,
};

/*
 * One request, waited 5 s: the QMI result and error are logged; a QMI error answer is not fatal
 * (the caller goes on), a transport failure is (the caller stops the probe).
 * `what` is the step's name for the log and the summary.
 */
static int
wlfw_request(const char *what, uint16_t message, size_t largest, const struct qmi_elem_info *req_ei,
    const void *req, const struct qmi_elem_info *resp_ei, void *resp)
{
	struct qmi_txn txn;
	int ret;

	ret = qmi_txn_init(&wlfw_handle, &txn, resp_ei, resp);
	if (ret < 0) {
		say("vayu-wifi: wlfw: %s: cannot start a transaction: %d\n", what, ret);
		wlfw_fail(what);
		return ret;
	}
	ret = qmi_send_request(&wlfw_handle, NULL, &txn, message, largest, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-wifi: wlfw: %s: cannot send: %d\n", what, ret);
		wlfw_fail(what);
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, 5000);
	if (ret < 0) {
		say("vayu-wifi: wlfw: %s: no reply: %d\n", what, ret);
		wlfw_fail(what);
		return ret;
	}
	return 0;
}

static void
wlfw_thread(void *arg __unused, wait_result_t wr __unused)
{
	struct vayu_wlfw_ind_register_req_msg_v01 ind_req = { 0 };
	struct vayu_wlfw_ind_register_resp_msg_v01 ind_resp = { 0 };
	struct vayu_wlfw_host_cap_req_msg_v01 cap_req = { 0 };
	struct vayu_wlfw_host_cap_resp_msg_v01 cap_resp = { 0 };
	struct vayu_wlfw_msa_info_req_msg_v01 msa_req = { 0 };
	struct vayu_wlfw_msa_info_resp_msg_v01 msa_resp = { 0 };
	struct vayu_wlfw_cap_req_msg_v01 info_req = { 0 };
	struct vayu_wlfw_cap_resp_msg_v01 info_resp = { 0 };
	uint64_t waited_ms = 0;
	int ret;

	ret = qmi_handle_init(&wlfw_handle, 8192, &wlfw_ops, wlfw_handlers);
	if (ret < 0) {
		say("vayu-wifi: wlfw: qmi_handle_init: %d\n", ret);
		wlfw_fail("qmi_handle_init");
		goto out;
	}
	ret = qmi_add_lookup(&wlfw_handle, VAYU_WLFW_SERVICE, VAYU_WLFW_VERSION, VAYU_WLFW_INSTANCE);
	if (ret < 0) {
		say("vayu-wifi: wlfw: qmi_add_lookup: %d\n", ret);
		wlfw_fail("qmi_add_lookup");
		goto out_release;
	}

	while (!wlfw.server && !wlfw.stop && waited_ms < (uint64_t)wifi_listen_s * 1000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited_ms += 100;
	}
	if (!wlfw.server) {
		if (!wlfw.stop) {
			say("vayu-wifi: wlfw: service 0x45 did not appear in %u s\n", wifi_listen_s);
		}
		snprintf(wlfw.summary, sizeof(wlfw.summary), "never appeared");
		goto out_release;
	}
	say("vayu-wifi: wlfw: service 0x45 at node %u port %u after %llu ms\n", wlfw.node, wlfw.port,
	    (unsigned long long)waited_ms);
	if (wlfw.stop) {
		wlfw_fail("ind_register");
		goto out_release;
	}

	/* 1. IND_REGISTER, as ath10k_qmi_ind_register_send_sync_msg() (ref/qmi.c:750-806). */
	ind_req.client_id_valid = 1;
	ind_req.client_id = VAYU_WLFW_CLIENT_ID;
	ind_req.fw_ready_enable_valid = 1;
	ind_req.fw_ready_enable = 1;
	ind_req.msa_ready_enable_valid = 1;
	ind_req.msa_ready_enable = 1;
	ind_req.pin_connect_result_enable_valid = 1;
	ind_req.pin_connect_result_enable = 1;
	ret = wlfw_request("ind_register", VAYU_WLFW_IND_REGISTER_REQ_V01, VAYU_WLFW_IND_REGISTER_REQ_MAX,
	    vayu_wlfw_ind_register_req_msg_v01_ei, &ind_req, vayu_wlfw_ind_register_resp_msg_v01_ei, &ind_resp);
	if (ret < 0) {
		goto out_release;
	}
	if (ind_resp.fw_status_valid) {
		say("vayu-wifi: wlfw: ind_register: result %u error %u fw_status %#llx\n",
		    (unsigned int)ind_resp.resp.result, (unsigned int)ind_resp.resp.error,
		    (unsigned long long)ind_resp.fw_status);
		if ((ind_resp.fw_status & VAYU_WLFW_FW_READY_V01) != 0) {
			say("vayu-wifi: wlfw: ind_register: fw_status has FW_READY\n");
		}
	} else {
		say("vayu-wifi: wlfw: ind_register: result %u error %u\n", (unsigned int)ind_resp.resp.result,
		    (unsigned int)ind_resp.resp.error);
	}
	/*
	 * FW_READY_IND (0x0021) is expected only after MSA_READY -> CAP -> BDF/cal
	 * (ath10k qmi.c:798-819); ind_register's fw_status 0 is normal here.
	 * The enable flags are already set; do not wait before host_cap.
	 */
	if (wlfw.stop) {
		wlfw_fail("host_cap");
		goto out_release;
	}

	/* 2. HOST_CAP, as ath10k_qmi_host_cap_send_sync() (ref/qmi.c:653-703). */
	if (wifi_hostcap != 0) {
		cap_req.daemon_support_valid = 1;
		cap_req.daemon_support = 0;
		ret = wlfw_request("host_cap", VAYU_WLFW_HOST_CAP_REQ_V01, VAYU_WLFW_HOST_CAP_REQ_MAX,
		    wifi_hostcap == 2 ? vayu_wlfw_host_cap_8bit_req_msg_v01_ei : vayu_wlfw_host_cap_req_msg_v01_ei,
		    &cap_req, vayu_wlfw_host_cap_resp_msg_v01_ei, &cap_resp);
		if (ret < 0) {
			goto out_release;
		}
		say("vayu-wifi: wlfw: host_cap: result %u error %u\n", (unsigned int)cap_resp.resp.result,
		    (unsigned int)cap_resp.resp.error);
	} else {
		say("vayu-wifi: wlfw: host_cap: not sent (vayu_wifi_hostcap=0)\n");
	}
	if (wlfw.stop) {
		wlfw_fail("msa_info");
		goto out_release;
	}

	/* 3. MSA_INFO, as ath10k_qmi_msa_mem_info_send_sync_msg() (ref/qmi.c:121-197). */
	msa_req.msa_addr = VAYU_WLFW_MSA_BASE;
	msa_req.size = VAYU_WLFW_MSA_SIZE;
	ret = wlfw_request("msa_info", VAYU_WLFW_MSA_INFO_REQ_V01, VAYU_WLFW_MSA_INFO_REQ_MAX,
	    vayu_wlfw_msa_info_req_msg_v01_ei, &msa_req, vayu_wlfw_msa_info_resp_msg_v01_ei, &msa_resp);
	if (ret < 0) {
		goto out_release;
	}
	say("vayu-wifi: wlfw: msa_info: result %u error %u\n", (unsigned int)msa_resp.resp.result,
	    (unsigned int)msa_resp.resp.error);
	/* Linux ath10k qmi.c: msleep(20) before setup_msa_permissions(). */
	delay_for_interval(20, NSEC_PER_MSEC);
	if (msa_resp.resp.result == QMI_RESULT_SUCCESS_V01) {
		uint32_t count = msa_resp.mem_region_info_len;

		if (count > VAYU_WLFW_MAX_MEM_REG_V01) {
			count = VAYU_WLFW_MAX_MEM_REG_V01;
		}
		for (uint32_t i = 0; i < count; i++) {
			struct vayu_wlfw_memory_region_info_s_v01 *region = &msa_resp.mem_region_info[i];
			uint64_t end = region->region_addr + region->size;
			bool outside = region->region_addr < VAYU_WLFW_MSA_BASE ||
			    end < region->region_addr || end > VAYU_WLFW_MSA_END;

			say("vayu-wifi: wlfw: msa region %u: %#llx + %#x secure %u%s\n", i,
			    (unsigned long long)region->region_addr, region->size,
			    (unsigned int)region->secure_flag,
			    outside ? " OUTSIDE 0x8bc00000..0x8bd80000" : "");
			/*
			 * Linux ath10k qmi.c:36-62, :825-850: assign EVERY MSA region,
			 * secure ones included, source HLOS -> {MSS_MSA 15, WLAN 0x18},
			 * plus WLAN_CE 0x19 for non-secure; HLOS is not a destination.
			 * This happens before MSA_READY, and a failure aborts the
			 * sequence (no MSA_READY, no cap).
			 */
			if (outside) {
				say("vayu-wifi: wlfw: msa region %u is outside the stock MSA; not assigned\n", i);
			} else if (wifi_msa_fixed_perm != 0) {
				say("vayu-wifi: wlfw: msa region %u: fixed-perm flag, assign skipped\n", i);
			} else {
				int scm_ret = vayu_scm_assign_msa(region->region_addr, region->size,
				    region->secure_flag != 0);

				say("vayu-wifi: wlfw: msa region %u assign HLOS->{15,0x18%s}: %d%s\n", i,
				    region->secure_flag != 0 ? "" : ",0x19", scm_ret,
				    scm_ret == 0 ? "" : " (MSA assign failed)");
				if (scm_ret != 0) {
					wlfw_fail("msa_assign");
					goto out_release;
				}
			}
		}
	}
	if (wlfw.stop) {
		wlfw_fail("cap");
		goto out_release;
	}

	/*
	 * 3b. MSA_READY: a synchronous REQUEST 0x002E (ath10k qmi.c:199-230),
	 * not the 0x002B host indication.  Wait for result SUCCESS; on failure
	 * abort before cap (cap would be QMI_ERR_INCOMPATIBLE_STATE, 90).
	 */
	{
		struct vayu_wlfw_msa_ready_req_msg_v01 ready_req = { 0 };
		struct vayu_wlfw_msa_ready_resp_msg_v01 ready_resp = { 0 };

		ret = wlfw_request("msa_ready", VAYU_WLFW_MSA_READY_REQ_V01, VAYU_WLFW_MSA_READY_REQ_MAX,
		    vayu_wlfw_msa_ready_req_msg_v01_ei, &ready_req,
		    vayu_wlfw_msa_ready_resp_msg_v01_ei, &ready_resp);
		if (ret < 0) {
			goto out_release;
		}
		say("vayu-wifi: wlfw: msa_ready request: result %u error %u\n",
		    (unsigned int)ready_resp.resp.result, (unsigned int)ready_resp.resp.error);
		if (ready_resp.resp.result != QMI_RESULT_SUCCESS_V01) {
			wlfw_fail("msa_ready");
			goto out_release;
		}
	}
	if (wlfw.stop) {
		wlfw_fail("cap");
		goto out_release;
	}

	/* 4. CAP, as ath10k_qmi_cap_send_sync_msg() (ref/qmi.c:572-651); no SMEM write of its result. */
	if (wifi_cap != 0) {
		ret = wlfw_request("cap", VAYU_WLFW_CAP_REQ_V01, VAYU_WLFW_CAP_REQ_MAX,
		    vayu_wlfw_cap_req_msg_v01_ei, &info_req, vayu_wlfw_cap_resp_msg_v01_ei, &info_resp);
		if (ret < 0) {
			goto out_release;
		}
		say("vayu-wifi: wlfw: cap: result %u error %u\n", (unsigned int)info_resp.resp.result,
		    (unsigned int)info_resp.resp.error);
		if (info_resp.resp.result == QMI_RESULT_SUCCESS_V01) {
			char chip_id[16], chip_family[16], board_id[16], soc_id[16], fw_version[16];
			char timestamp[VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 + 1];
			char build_id[VAYU_WLFW_MAX_BUILD_ID_LEN_V01 + 1];

			if (info_resp.chip_info_valid) {
				snprintf(chip_id, sizeof(chip_id), "%#x", info_resp.chip_info.chip_id);
				snprintf(chip_family, sizeof(chip_family), "%#x", info_resp.chip_info.chip_family);
				wlfw.chip_id = info_resp.chip_info.chip_id;
			} else {
				snprintf(chip_id, sizeof(chip_id), "?");
				snprintf(chip_family, sizeof(chip_family), "?");
			}
			if (info_resp.board_info_valid) {
				snprintf(board_id, sizeof(board_id), "%#x", info_resp.board_info.board_id);
				wlfw.board_id = info_resp.board_info.board_id;
			} else {
				snprintf(board_id, sizeof(board_id), "?");
			}
			if (info_resp.soc_info_valid) {
				snprintf(soc_id, sizeof(soc_id), "%#x", info_resp.soc_info.soc_id);
				wlfw.soc_id = info_resp.soc_info.soc_id;
			} else {
				snprintf(soc_id, sizeof(soc_id), "?");
			}
			if (info_resp.fw_version_info_valid) {
				info_resp.fw_version_info.fw_build_timestamp[VAYU_WLFW_MAX_TIMESTAMP_LEN_V01] = 0;
				snprintf(timestamp, sizeof(timestamp), "%s", info_resp.fw_version_info.fw_build_timestamp);
				snprintf(fw_version, sizeof(fw_version), "%#x", info_resp.fw_version_info.fw_version);
			} else {
				snprintf(timestamp, sizeof(timestamp), "?");
				snprintf(fw_version, sizeof(fw_version), "?");
			}
			if (info_resp.fw_build_id_valid) {
				info_resp.fw_build_id[VAYU_WLFW_MAX_BUILD_ID_LEN_V01] = 0;
				snprintf(build_id, sizeof(build_id), "%s", info_resp.fw_build_id);
			} else {
				snprintf(build_id, sizeof(build_id), "?");
			}
			say("vayu-wifi: wlfw: cap: chip_id %s chip_family %s board_id %s soc_id %s\n", chip_id,
			    chip_family, board_id, soc_id);
			say("vayu-wifi: wlfw: cap: fw_version %s fw_build_timestamp %s fw_build_id %s\n",
			    fw_version, timestamp, build_id);
		}
	} else {
		say("vayu-wifi: wlfw: cap: not sent (vayu_wifi_cap=0)\n");
	}

	snprintf(wlfw.summary, sizeof(wlfw.summary), "answered: chip %#x board %#x soc %#x", wlfw.chip_id,
	    wlfw.board_id, wlfw.soc_id);
	say("vayu-wifi: wlfw: stopped after cap: MSA permissions and MSA_READY request done; "
	    "no BDF, no wlan mode\n");

out_release:
	qmi_handle_release(&wlfw_handle);
out:
	wlfw.running = false;
	thread_terminate(current_thread());
}

/* ===================================================================================================
 * vayu_wifi_start / vayu_wifi_stop
 * =================================================================================================== */

void
vayu_wifi_start(struct vayu_qrtr *q, uint32_t listen_s)
{
	thread_t thread;
	int ret;

	(void)q;                                /* the QMI helpers' transport is already attached to it */
	if (wifi_started) {
		say("vayu-wifi: already started\n");
		return;
	}
	wifi_level = 0;
	wifi_hostcap = 1;
	wifi_cap = 1;
	PE_parse_boot_argn("vayu_wifi", &wifi_level, sizeof(wifi_level));
	PE_parse_boot_argn("vayu_wifi_hostcap", &wifi_hostcap, sizeof(wifi_hostcap));
	PE_parse_boot_argn("vayu_wifi_cap", &wifi_cap, sizeof(wifi_cap));
	PE_parse_boot_argn("vayu_msa_fixed_perm", &wifi_msa_fixed_perm, sizeof(wifi_msa_fixed_perm));
	wifi_listen_s = listen_s;
	say("vayu-wifi: vayu_wifi=%u vayu_wifi_hostcap=%u vayu_wifi_cap=%u vayu_msa_fixed_perm=%u listen=%u s\n",
	    wifi_level, wifi_hostcap, wifi_cap, wifi_msa_fixed_perm, listen_s);
	if (wifi_level == 0) {
		return;
	}
	wifi_started = true;
	wifi_stopping = false;

	if (wifi_level >= 1) {
		pdm_port = vayu_qrtr_ops.open(pdm_receive, NULL, NULL);
		if (pdm_port == NULL) {
			say("vayu-wifi: pd-mapper: no port\n");
		} else {
			ret = vayu_qrtr_publish(pdm_port, VAYU_PDM_SERVICE, 1, 1);
			if (ret != 0) {
				say("vayu-wifi: pd-mapper: cannot publish service 0x40: %d\n", ret);
				vayu_qrtr_ops.close(pdm_port);
				pdm_port = NULL;
			} else {
				say("vayu-wifi: pd-mapper: service 0x40 version 1 instance 1 at port %u\n",
				    vayu_qrtr_port_number(pdm_port));
			}
		}

		tftp_port = vayu_qrtr_ops.open(tftp_receive, NULL, NULL);
		if (tftp_port == NULL) {
			say("vayu-wifi: tftp: no port\n");
		} else {
			ret = vayu_qrtr_publish(tftp_port, VAYU_TFTP_SERVICE, 1, 0);
			if (ret != 0) {
				say("vayu-wifi: tftp: cannot publish service 4096: %d\n", ret);
				vayu_qrtr_ops.close(tftp_port);
				tftp_port = NULL;
			} else {
				say("vayu-wifi: tftp: service 4096 version 1 instance 0 at port %u\n",
				    vayu_qrtr_port_number(tftp_port));
			}
		}
	}

	if (wifi_level >= 2) {
		memset(&wlfw, 0, sizeof(wlfw));
		snprintf(wlfw.summary, sizeof(wlfw.summary), "never appeared");
		wlfw.running = true;
		if (kernel_thread_start(wlfw_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			wlfw.running = false;
			wlfw_fail("thread_start");
			say("vayu-wifi: wlfw: no thread\n");
		}
	}
}

void
vayu_wifi_stop(void)
{
	struct hakios_qrtr_port *ports[2 + VAYU_TFTP_XFERS];
	unsigned int count = 0;
	uint32_t pdm, reads, served, writes;
	uint64_t bytes;
	char wlfw_summary[96];

	if (!wifi_started || wifi_level == 0) {
		return;
	}
	wifi_started = false;

	lck_mtx_lock(&vayu_wifi_lock);
	wifi_stopping = true;
	lck_mtx_unlock(&vayu_wifi_lock);

	/* The WLFW thread first: its handle's port belongs to the transport vayu_qrtr_detach() takes away. */
	if (wifi_level >= 2 && wlfw.running) {
		uint32_t waited;

		wlfw.stop = true;
		for (waited = 0; waited < 5000 && wlfw.running; waited += 10) {
			delay_for_interval(10, NSEC_PER_MSEC);
		}
		if (wlfw.running) {
			say("vayu-wifi: wlfw: the thread did not end in 5 s\n");
		}
	}

	/* No callback should be running now; give a slow one (a send may sleep) a second. */
	for (uint32_t waited = 0; waited < 1000; waited += 10) {
		bool busy;

		lck_mtx_lock(&vayu_wifi_lock);
		busy = pdm_busy != 0 || tftp_busy != 0;
		for (unsigned int i = 0; i < VAYU_TFTP_XFERS && !busy; i++) {
			if (tftp_xfer[i].used && tftp_xfer[i].busy != 0) {
				busy = true;
			}
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (!busy) {
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}

	/* Every transfer port: xfer_close() logs the end line and adds its bytes. */
	for (unsigned int i = 0; i < VAYU_TFTP_XFERS; i++) {
		tftp_xfer_close(&tftp_xfer[i], false, 0, NULL);
	}

	lck_mtx_lock(&vayu_wifi_lock);
	if (tftp_port != NULL && tftp_busy == 0) {
		ports[count++] = tftp_port;
		tftp_port = NULL;
	}
	if (pdm_port != NULL && pdm_busy == 0) {
		ports[count++] = pdm_port;
		pdm_port = NULL;
	}
	pdm = pdm_requests;
	reads = tftp_reads;
	served = tftp_served;
	bytes = tftp_bytes;
	writes = tftp_writes;
	if (wifi_level >= 2) {
		if (wlfw.running) {
			snprintf(wlfw_summary, sizeof(wlfw_summary), "failed at stop");
		} else {
			snprintf(wlfw_summary, sizeof(wlfw_summary), "%s", wlfw.summary);
		}
	} else {
		snprintf(wlfw_summary, sizeof(wlfw_summary), "not asked");
	}
	lck_mtx_unlock(&vayu_wifi_lock);

	for (unsigned int i = 0; i < count; i++) {
		vayu_qrtr_ops.close(ports[i]);
	}

	say("vayu-wifi: done: pd-mapper %u request(s); tftp %u read(s) (%u served, %llu bytes), %u write(s) "
	    "refused; wlfw %s\n", pdm, reads, served, (unsigned long long)bytes, writes, wlfw_summary);
}
