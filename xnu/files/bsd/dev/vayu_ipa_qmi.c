/*
 * vayu IPA QMI service 0x31: the modem-facing IPA 4.1 handshake, in stages.
 *
 * 0641 proved the ordering requirement: WDS Start crashes the modem at
 * ipa_ipfltr.init_done when this handshake has not happened. This client sends the exact
 * vayu-r-oss INIT_MODEM_DRIVER layout, waits for its answer and then sends INIT_COMPLETE.
 * Only then vayu_ipa_ready() may become true (once the firmware/PAS stage is also ready).
 *
 * vayu_ipa_qmi=N (absent: 2, the old behaviour) picks how far it goes; every stage prints one
 * line "vayu-ipa-qmi: S<n> <step>: OK|ERROR|TIMEOUT ...":
 *   0  nothing
 *   1  SMP2P "ipa" (ours 428 = 3, the modem's 435 if it asked) + lookup of the modem's 0x31/2; no request
 *   2  + host 0x31/1 published, INIT_MODEM_DRIVER, INDICATION_REGISTER, INIT_COMPLETE, 0x35
 *   3  + read-only WDS Get Packet Service Status (0x22)
 *   4  + WDS Start allowed in vayu_cell_data.c (needs vayu_cell_data=1 vayu_cell_net=1 vayu_ipa_data=1)
 * vayu_ipa_qmi_wait=MS (60000) for the lookup, vayu_ipa_qmi_timeout=MS (30000) for INIT_MODEM_DRIVER.
 *
 * Stock (vayu-r-oss ipa_qmi_service.c): one INIT_MODEM_DRIVER, 60 s timeout (:39, :677);
 * MASTER_DRIVER_INIT_COMPLETE_IND once both INIT_MODEM_DRIVER answered and INDICATION_REGISTER came
 * (:132, :1621), sent from the A5 server handle; 0x35 (INIT_MODEM_DRIVER_CMPLT) is not waited for (:285).
 *
 * It does not configure GSI rings or carry packets. Those belong to vayu_ipa_data.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/errno.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>
#include <sys/vayu_ipa.h>
#include <sys/vayu_ipa_data.h>
#include <sys/vayu_ipa_qmi.h>
#include <sys/vayu_pas.h>

extern void loader_log_putc(char c);
extern bool vayu_ipa_firmware_ready(void);

static struct qmi_handle ipa_qmi, wds_qmi;
static bool ipa_qmi_up, wds_qmi_up, ipa_started, ipa_running;
static volatile bool ipa_stop, ipa_modem_there, wds_there;
static uint32_t ipa_node, ipa_port, ipa_version, wds_node, wds_port, wds_version;
static volatile bool indication_wanted, init_done, complete_sent, driver_complete;
static uint32_t client_node, client_port;
static struct hakios_qrtr_port *host_port;
static uint32_t qmi_wait_ms = 60000, qmi_timeout_ms = 30000;

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
	for (const char *c = line; *c != 0; c++) loader_log_putc(*c);
	printf("%s", line);
	return length;
}

static uint64_t
now_ms(void)
{
	uint64_t abs = mach_absolute_time(), ns = 0;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / NSEC_PER_MSEC;
}

uint32_t
vayu_ipa_qmi_stage(void)
{
	uint32_t stage = 2;

	PE_parse_boot_argn("vayu_ipa_qmi", &stage, sizeof(stage));
	return stage;
}

/* Wait until *flag or stop, at most ms; returns the time waited. */
static uint64_t
wait_for(volatile bool *flag, uint64_t ms)
{
	uint64_t waited = 0;

	while (!*flag && !ipa_stop && waited < ms) {
		delay_for_interval(50, NSEC_PER_MSEC);
		waited += 50;
	}
	return waited;
}

static int
ipa_result(const uint8_t *data, size_t length, uint16_t *result, uint16_t *error)
{
	if (length < 7) return -1;
	for (size_t at = 7; at + 3 <= length;) {
		uint16_t size = (uint16_t)(data[at + 1] | data[at + 2] << 8);
		if (size > length - at - 3) return -1;
		if (data[at] == 2 && size >= 4) {
			*result = (uint16_t)(data[at + 3] | data[at + 4] << 8);
			*error = (uint16_t)(data[at + 5] | data[at + 6] << 8);
			return 0;
		}
		at += 3 + size;
	}
	return -1;
}

/* INIT_MODEM_DRIVER, byte-for-byte the vayu IPA 4.1 memory layout from downstream
 * (ipa_utils.c ipa_4_1_mem_part, ipa_qmi_service.c ipa3_qmi_init_modem_send_sync_msg). */
static void
ipa_init_modem_driver(void)
{
	struct vayu_ipa_init_req req = { 0 };
	struct vayu_ipa_init_resp resp = { 0 };
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = ipa_node, .sq_port = ipa_port };
	struct qmi_txn txn;
	uint32_t o = vayu_ipa_restricted_bytes();
	uint64_t started;
	int ret;

	req.platform_type_valid = 1;
	req.platform_type = VAYU_IPA_PLATFORM_MSM_ANDROID;
	req.hdr_tbl_info_valid = 1;
	req.hdr_tbl_info.start = 0x688 + o;
	req.hdr_tbl_info.end = 0x688 + o + 0x140 - 1;
	req.v4_route_tbl_info_valid = 1;
	req.v4_route_tbl_info.start = 0x508 + o;
	req.v4_route_tbl_info.num_indices = 7;
	req.v6_route_tbl_info_valid = 1;
	req.v6_route_tbl_info.start = 0x608 + o;
	req.v6_route_tbl_info.num_indices = 7;
	req.v4_filter_tbl_start_addr_valid = 1;
	req.v4_filter_tbl_start_addr = 0x308 + o;
	req.v6_filter_tbl_start_addr_valid = 1;
	req.v6_filter_tbl_start_addr = 0x408 + o;
	req.modem_mem_info_valid = 1;
	req.modem_mem_info.start = 0x13f0 + o;
	req.modem_mem_info.size = 0x100c;
	req.ctrl_comm_dest_end_pt_valid = 1;
	req.ctrl_comm_dest_end_pt = 11;       /* IPA_CLIENT_APPS_WAN_CONS */
	/* is_ssr_bootup absent: first boot (stock: !ipa3_uc_loaded_check()). */
	req.hdr_proc_ctx_tbl_info_valid = 1;
	req.hdr_proc_ctx_tbl_info.start = 0x7d0 + o;
	req.hdr_proc_ctx_tbl_info.end = 0x7d0 + o + 0x200 - 1;
	req.v4_hash_route_tbl_info_valid = 1;
	req.v4_hash_route_tbl_info.start = 0x488 + o;
	req.v4_hash_route_tbl_info.num_indices = 7;
	req.v6_hash_route_tbl_info_valid = 1;
	req.v6_hash_route_tbl_info.start = 0x588 + o;
	req.v6_hash_route_tbl_info.num_indices = 7;
	req.v4_hash_filter_tbl_start_addr_valid = 1;
	req.v4_hash_filter_tbl_start_addr = 0x288 + o;
	req.v6_hash_filter_tbl_start_addr_valid = 1;
	req.v6_hash_filter_tbl_start_addr = 0x388 + o;
	req.hw_stats_quota_base_addr_valid = 1;
	req.hw_stats_quota_base_addr = 0xc30 + o;
	req.hw_stats_quota_size_valid = 1;
	req.hw_stats_quota_size = 0x60;
	req.hw_drop_stats_base_addr_valid = 1;
	req.hw_drop_stats_base_addr = 0x13d0 + o;
	req.hw_drop_stats_table_size_valid = 1;
	req.hw_drop_stats_table_size = 0x20;

	ret = qmi_txn_init(&ipa_qmi, &txn, vayu_ipa_init_resp_ei, &resp);
	if (ret < 0) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR no transaction (%d)\n", ret);
		return;
	}
	started = now_ms();
	ret = qmi_send_request(&ipa_qmi, &to, &txn, VAYU_IPA_INIT_MODEM_DRIVER, 256, vayu_ipa_init_req_ei, &req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR send %d\n", ret);
		return;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, qmi_timeout_ms);
	if (ret == -ETIMEDOUT) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: TIMEOUT no reply in %u ms (restricted %#x)\n", qmi_timeout_ms, o);
		return;
	}
	if (ret < 0) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR reply not decoded (%d)\n", ret);
		return;
	}
	if (resp.result.result != 0 || resp.result.error != 0) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR result %u error %u after %llu ms\n",
		    (unsigned)resp.result.result, (unsigned)resp.result.error, now_ms() - started);
		return;
	}
	init_done = true;
	say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: OK after %llu ms (restricted %#x; ctrl ep %s%u, default ep %s%u, "
	    "init pending %s%u)\n", now_ms() - started, o,
	    resp.ctrl_comm_dest_end_pt_valid ? "" : "-", resp.ctrl_comm_dest_end_pt,
	    resp.default_end_pt_valid ? "" : "-", resp.default_end_pt,
	    resp.modem_driver_init_pending_valid ? "" : "-", resp.modem_driver_init_pending);
}

/*
 * MASTER_DRIVER_INIT_COMPLETE_IND (0x22), once INIT_MODEM_DRIVER succeeded and the modem's client
 * registered for indications (stock :132/:1621; 0x35 is not a condition). Sent from our published
 * server port, as stock sends it from the A5 server handle: the modem's client knows that address only.
 */
static void
ipa_send_complete(void)
{
	static const uint8_t ind[14] = { 4, 0, 0, VAYU_IPA_INIT_COMPLETE_IND & 0xff, VAYU_IPA_INIT_COMPLETE_IND >> 8,
		                         7, 0, 0x02, 4, 0, 0, 0, 0, 0 };
	int ret;

	if (!indication_wanted || !init_done || complete_sent || ipa_stop || client_port == 0 || host_port == NULL) return;
	ret = vayu_qrtr_ops.send(host_port, client_node, client_port, ind, sizeof(ind));
	if (ret == 0) {
		complete_sent = true;
		say("vayu-ipa-qmi: S2 INIT_COMPLETE_IND: OK sent to node %u port %u (0x35 %s)\n", client_node, client_port,
		    driver_complete ? "seen" : "not yet");
	} else {
		say("vayu-ipa-qmi: S2 INIT_COMPLETE_IND: ERROR send %d\n", ret);
	}
}

static void
ipa_receive(void *context __unused, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t result = 0xffff, error = 0xffff, message;

	if (length < 7 || node != ipa_node) return;
	message = (uint16_t)(m[3] | m[4] << 8);
	(void)ipa_result(m, length, &result, &error);
	if (m[0] == 0) {
		/* A request on our published host port. Answer with the same transaction id.
		 * Only the handshake requests are implemented. Claiming success for a filter or
		 * configuration request without applying it would be a fake data-ready path. */
		bool supported = message == VAYU_IPA_INDICATION_REGISTER ||
		    message == VAYU_IPA_DRIVER_INIT_COMPLETE;
		uint16_t result = supported ? 0 : 1;
		uint16_t error = supported ? 0 : QMI_ERR_NOT_SUPPORTED_V01;
		uint8_t answer[14] = { 2, m[1], m[2], m[3], m[4], 7, 0, 2, 4, 0,
		    (uint8_t)result, (uint8_t)(result >> 8), (uint8_t)error, (uint8_t)(error >> 8) };

		if (!supported) vayu_ipa_data_filter_error();
		(void)vayu_qrtr_ops.send(host_port, node, from_port, answer, sizeof(answer));
		if (!supported) {
			say("vayu-ipa-qmi: modem request %#x (%zu bytes) answered NOT_SUPPORTED (nothing was changed)\n",
			    message, length);
		}
	} else {
		say("vayu-ipa-qmi: modem message %#x type %u on the host port: result %u error %u\n", message, m[0],
		    result, error);
	}
	switch (message) {
	case VAYU_IPA_INDICATION_REGISTER:
		client_node = node;
		client_port = from_port;
		indication_wanted = true;
		say("vayu-ipa-qmi: S2 INDICATION_REGISTER: OK from node %u port %u (answered success)\n", node, from_port);
		ipa_send_complete();
		break;
	case VAYU_IPA_DRIVER_INIT_COMPLETE:
		driver_complete = true;
		say("vayu-ipa-qmi: S2 MODEM_INIT_CMPLT(0x35): OK (answered success)\n");
		break;
	default:
		break;
	}
}

static int
ipa_new_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	int ret;

	if (service->instance != VAYU_IPA_MODEM_INSTANCE) return -1;
	ret = hakios_qmi_connect(qmi->sock, service->node, service->port);
	if (ret != 0) return ret;
	ipa_node = service->node;
	ipa_port = service->port;
	ipa_version = service->version;
	ipa_modem_there = true;
	return 0;
}

static void
ipa_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (ipa_node == service->node && ipa_port == service->port) {
		ipa_modem_there = false;
		indication_wanted = init_done = complete_sent = driver_complete = false;
		say("vayu-ipa-qmi: modem IPA service went away\n");
	}
}

static const struct qmi_ops ipa_qmi_ops = {
	.new_server = ipa_new_server,
	.del_server = ipa_del_server,
};

static int
wds_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	if (wds_there) return -1;
	if (hakios_qmi_connect(qmi->sock, service->node, service->port) != 0) return -1;
	wds_node = service->node;
	wds_port = service->port;
	wds_version = service->version;
	wds_there = true;
	return 0;
}

static void
wds_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (wds_node == service->node && wds_port == service->port) wds_there = false;
}

static const struct qmi_ops wds_qmi_ops = {
	.new_server = wds_new_server,
	.del_server = wds_del_server,
};

/* S1: our "ipa" entry must read 3 (response done + clock on); the modem's is logged as it is. */
static void
stage_smp2p(void)
{
	bool ours_found = false, theirs_found = false;
	uint32_t ours = 0, theirs = 0;
	uint64_t waited = 0;

	for (;;) {
		vayu_ipa_smp2p_read(&ours_found, &ours, &theirs_found, &theirs);
		if ((ours_found && (ours & 3) == 3) || ipa_stop || waited >= 5000) break;
		delay_for_interval(100, NSEC_PER_MSEC);
		waited += 100;
	}
	say("vayu-ipa-qmi: S1 SMP2P: %s ours \"ipa\" %s%#x, modem \"ipa\" %s%#x, firmware %s (%llu ms)\n",
	    ours_found && (ours & 3) == 3 ? "OK" : "ERROR", ours_found ? "" : "absent ", ours,
	    theirs_found ? "" : "absent ", theirs, vayu_ipa_firmware_ready() ? "ready" : "NOT READY", waited);
}

/* S3: WDS answers a read-only question once IPA is initialised. */
static void
stage_wds(void)
{
	struct vayu_ipa_wds_status_resp resp = { 0 };
	struct sockaddr_qrtr to;
	struct qmi_txn txn;
	uint64_t waited;
	int ret;

	ret = qmi_handle_init(&wds_qmi, 1024, &wds_qmi_ops, NULL);
	if (ret < 0) {
		say("vayu-ipa-qmi: S3 WDS LOOKUP: ERROR qmi_handle_init %d\n", ret);
		return;
	}
	wds_qmi_up = true;
	ret = qmi_add_lookup(&wds_qmi, VAYU_IPA_WDS_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-ipa-qmi: S3 WDS LOOKUP: ERROR qmi_add_lookup %d\n", ret);
		return;
	}
	waited = wait_for(&wds_there, 20000);
	if (!wds_there) {
		say("vayu-ipa-qmi: S3 WDS LOOKUP: TIMEOUT service 0x01 not seen in %llu ms\n", waited);
		return;
	}
	say("vayu-ipa-qmi: S3 WDS LOOKUP: OK node %u port %u version %u\n", wds_node, wds_port, wds_version);
	to = (struct sockaddr_qrtr){ .sq_family = AF_QIPCRTR, .sq_node = wds_node, .sq_port = wds_port };
	ret = qmi_txn_init(&wds_qmi, &txn, vayu_ipa_wds_status_resp_ei, &resp);
	if (ret < 0) {
		say("vayu-ipa-qmi: S3 WDS GET_PKT_SRVC_STATUS: ERROR no transaction %d\n", ret);
		return;
	}
	ret = qmi_send_request(&wds_qmi, &to, &txn, VAYU_IPA_WDS_GET_PKT_SRVC_STATUS, 64, NULL, NULL);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-ipa-qmi: S3 WDS GET_PKT_SRVC_STATUS: ERROR send %d\n", ret);
		return;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, 5000);
	if (ret == -ETIMEDOUT) {
		say("vayu-ipa-qmi: S3 WDS GET_PKT_SRVC_STATUS: TIMEOUT 5000 ms\n");
	} else if (ret < 0) {
		say("vayu-ipa-qmi: S3 WDS GET_PKT_SRVC_STATUS: ERROR reply not decoded (%d)\n", ret);
	} else if (resp.result.result != 0 || resp.result.error != 0) {
		say("vayu-ipa-qmi: S3 WDS GET_PKT_SRVC_STATUS: ERROR result %u error %u\n",
		    (unsigned)resp.result.result, (unsigned)resp.result.error);
	} else {
		say("vayu-ipa-qmi: S3 WDS GET_PKT_SRVC_STATUS: OK connection status %u (1 disconnected, 2 connected)\n",
		    resp.status);
	}
}

static void
ipa_thread(void *arg, wait_result_t wr __unused)
{
	uint32_t stage = (uint32_t)(uintptr_t)arg;
	uint64_t waited;
	int ret;

	stage_smp2p();
	ret = qmi_handle_init(&ipa_qmi, 4096, &ipa_qmi_ops, NULL);
	if (ret < 0) {
		say("vayu-ipa-qmi: S1 LOOKUP 0x31/2: ERROR qmi_handle_init %d\n", ret);
		goto out;
	}
	ipa_qmi_up = true;
	ret = qmi_add_lookup(&ipa_qmi, VAYU_IPA_QMI_SERVICE, VAYU_IPA_QMI_INSTANCE, VAYU_IPA_MODEM_INSTANCE);
	if (ret < 0) {
		say("vayu-ipa-qmi: S1 LOOKUP 0x31/2: ERROR qmi_add_lookup %d\n", ret);
		goto out;
	}
	waited = wait_for(&ipa_modem_there, qmi_wait_ms);
	if (!ipa_modem_there) {
		say("vayu-ipa-qmi: S1 LOOKUP 0x31/2: TIMEOUT not announced in %llu ms\n", waited);
		goto out;
	}
	say("vayu-ipa-qmi: S1 LOOKUP 0x31/2: OK node %u port %u version %u after %llu ms\n", ipa_node, ipa_port,
	    ipa_version, waited);
	if (stage < 2) goto idle;

	if (!vayu_ipa_firmware_ready()) {
		say("vayu-ipa-qmi: S2 PUBLISH 0x31/1: ERROR IPA firmware/SMP2P not ready; no INIT_MODEM_DRIVER\n");
		goto out;
	}
	host_port = vayu_qrtr_ops.open(ipa_receive, NULL, NULL);
	ret = host_port == NULL ? -ENOMEM :
	    vayu_qrtr_publish(host_port, VAYU_IPA_QMI_SERVICE, VAYU_IPA_QMI_INSTANCE, VAYU_IPA_QMI_INSTANCE);
	if (ret != 0) {
		say("vayu-ipa-qmi: S2 PUBLISH 0x31/1: ERROR %d\n", ret);
		goto out;
	}
	say("vayu-ipa-qmi: S2 PUBLISH 0x31/1: OK\n");
	ipa_init_modem_driver();
	if (!init_done) {
		say("vayu-ipa-qmi: S2 handshake stopped; WDS Start remains blocked\n");
		goto idle;
	}
	waited = wait_for(&indication_wanted, 15000);
	if (!indication_wanted) {
		say("vayu-ipa-qmi: S2 INDICATION_REGISTER: TIMEOUT none in %llu ms after INIT_MODEM_DRIVER\n", waited);
	}
	ipa_send_complete();
	(void)wait_for(&driver_complete, 10000);
	say("vayu-ipa-qmi: S2 RESULT: %s (INIT_MODEM_DRIVER %s, INDICATION_REGISTER %s, INIT_COMPLETE %s, 0x35 %s)\n",
	    vayu_ipa_ready() ? "OK IPA READY" : "ERROR not ready", init_done ? "ok" : "no",
	    indication_wanted ? "ok" : "no", complete_sent ? "sent" : "NOT SENT", driver_complete ? "ok" : "TIMEOUT");
	if (stage >= 3 && !ipa_stop) stage_wds();
	if (stage >= 4) {
		say("vayu-ipa-qmi: S4 WDS START: %s (vayu_cell_data=1 drives it)\n",
		    vayu_ipa_ready() ? "allowed" : "ERROR blocked: IPA handshake not complete");
	}
idle:
	while (!ipa_stop) delay_for_interval(100, NSEC_PER_MSEC);
out:
	if (wds_qmi_up) {
		qmi_handle_release(&wds_qmi);
		wds_qmi_up = false;
	}
	if (ipa_qmi_up) {
		qmi_handle_release(&ipa_qmi);
		ipa_qmi_up = false;
	}
	ipa_running = false;
	say("vayu-ipa-qmi: thread done\n");
	thread_terminate(current_thread());
}

bool
vayu_ipa_ready(void)
{
	return vayu_ipa_firmware_ready() && init_done && complete_sent;
}

bool
vayu_ipa_pending(void)
{
	return ipa_started && !vayu_ipa_ready();
}

void
vayu_ipa_modem_endpoints(uint32_t *to_modem, uint32_t *from_modem)
{
	*to_modem = 2;
	*from_modem = 11;
}

void
vayu_ipa_start(struct vayu_qrtr *q)
{
	thread_t thread;
	uint32_t enabled = 0, stage;

	if (!PE_parse_boot_argn("vayu_ipa", &enabled, sizeof(enabled)) || enabled != 1) {
		if (enabled != 0) say("vayu-ipa: vayu_ipa=%u refused: only 1 enables the IPA path\n", enabled);
		return;
	}
	if (ipa_started) return;
	stage = vayu_ipa_qmi_stage();
	PE_parse_boot_argn("vayu_ipa_qmi_wait", &qmi_wait_ms, sizeof(qmi_wait_ms));
	PE_parse_boot_argn("vayu_ipa_qmi_timeout", &qmi_timeout_ms, sizeof(qmi_timeout_ms));
	if (qmi_timeout_ms < 1000 || qmi_timeout_ms > 60000) qmi_timeout_ms = 30000;
	say("vayu-ipa-qmi: vayu_ipa_qmi=%u wait %u ms, INIT_MODEM_DRIVER timeout %u ms\n", stage, qmi_wait_ms,
	    qmi_timeout_ms);
	if (stage == 0) return;
	if (q == NULL) {
		say("vayu-ipa-qmi: S1 SMP2P: ERROR no QRTR transport\n");
		return;
	}
	ipa_started = true;
	ipa_running = true;
	ipa_stop = false;
	if (kernel_thread_start(ipa_thread, (void *)(uintptr_t)stage, &thread) != KERN_SUCCESS) {
		say("vayu-ipa-qmi: S1 SMP2P: ERROR no thread\n");
		ipa_running = false;
		return;
	}
	thread_deallocate(thread);
}

void
vayu_ipa_stop(void)
{
	if (!ipa_started) return;
	ipa_stop = true;
	/* A pending INIT_MODEM_DRIVER wait is not interruptible: give it its whole timeout. */
	for (uint32_t waited = 0; waited < qmi_timeout_ms + 12000 && ipa_running; waited += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	if (host_port != NULL) {
		vayu_qrtr_ops.close(host_port);
		host_port = NULL;
	}
	init_done = complete_sent = indication_wanted = driver_complete = false;
	ipa_modem_there = wds_there = false;
	ipa_started = false;
	say("vayu-ipa: stopped\n");
}
