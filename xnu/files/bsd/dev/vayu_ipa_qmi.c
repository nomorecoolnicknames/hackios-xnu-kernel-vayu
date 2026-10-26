/*
 * vayu IPA QMI service 0x31: the modem-facing IPA 4.1 handshake.
 *
 * 0641 proved the ordering requirement: WDS Start crashes the modem at
 * ipa_ipfltr.init_done when this handshake has not happened. This client sends the exact
 * vayu-r-oss INIT_MODEM_DRIVER layout, waits for its answer and then sends INIT_COMPLETE.
 * Only then vayu_ipa_ready() may become true (once the firmware/PAS stage is also ready).
 *
 * It does not configure GSI rings or carry packets. Those belong to vayu_ipa.c's later stages.
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
#include <sys/vayu_ipa_qmi.h>
#include <sys/vayu_pas.h>

extern void loader_log_putc(char c);
extern bool vayu_ipa_firmware_ready(void);

static struct qmi_handle ipa_qmi;
static bool ipa_qmi_up, ipa_started, ipa_running;
static volatile bool ipa_stop, ipa_modem_there;
static uint32_t ipa_node, ipa_port;
static volatile bool indication_wanted, init_done, complete_sent, driver_complete;
static uint32_t client_node, client_port;
static struct hakios_qrtr_port *host_port;

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

static int
ipa_qmi_request(uint16_t message, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, struct vayu_cell_result *result)
{
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = client_node, .sq_port = client_port };
	struct qmi_txn txn;
	int ret;

	ret = qmi_txn_init(&ipa_qmi, &txn, resp_ei, resp);
	if (ret < 0) return ret;
	ret = qmi_send_request(&ipa_qmi, &to, &txn, message, 256, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, 10000);
	if (ret < 0) {
		say("vayu-ipa: INIT_MODEM_DRIVER no reply in 10000 ms (%d)\n", ret);
		return ret;
	}
	if (result->result != 0 || result->error != 0) {
		say("vayu-ipa: INIT_MODEM_DRIVER result %u error %u\n",
		    (unsigned int)result->result, (unsigned int)result->error);
		return -EIO;
	}
	return 0;
}

/* INIT_MODEM_DRIVER, byte-for-byte the vayu IPA 4.1 memory layout from downstream. */
static int
ipa_init_modem_driver(void)
{
	struct vayu_ipa_init_req req = { 0 };
	struct vayu_ipa_init_resp resp = { 0 };

	req.platform_type_valid = 1;
	req.platform_type = VAYU_IPA_PLATFORM_MSM_ANDROID;
	req.hdr_tbl_info_valid = 1;
	req.hdr_tbl_info.start = 0x688;
	req.hdr_tbl_info.end = 0x688 + 0x140 - 1;
	req.v4_route_tbl_info_valid = 1;
	req.v4_route_tbl_info.start = 0x508;
	req.v4_route_tbl_info.num_indices = 7;
	req.v6_route_tbl_info_valid = 1;
	req.v6_route_tbl_info.start = 0x608;
	req.v6_route_tbl_info.num_indices = 7;
	req.v4_filter_tbl_start_addr_valid = 1;
	req.v4_filter_tbl_start_addr = 0x308;
	req.v6_filter_tbl_start_addr_valid = 1;
	req.v6_filter_tbl_start_addr = 0x408;
	req.modem_mem_info_valid = 1;
	req.modem_mem_info.start = 0x13f0;
	req.modem_mem_info.size = 0x100c;
	req.ctrl_comm_dest_end_pt_valid = 1;
	req.ctrl_comm_dest_end_pt = 11;       /* IPA_CLIENT_APPS_WAN_CONS */
	/* is_ssr_bootup absent: first boot, not a modem-only SSR. */
	req.hdr_proc_ctx_tbl_info_valid = 1;
	req.hdr_proc_ctx_tbl_info.start = 0x7d0;
	req.hdr_proc_ctx_tbl_info.end = 0x7d0 + 0x200 - 1;
	/* vayu supports hashable filter/route tables. */
	req.v4_hash_route_tbl_info_valid = 1;
	req.v4_hash_route_tbl_info.start = 0x488;
	req.v4_hash_route_tbl_info.num_indices = 7;
	req.v6_hash_route_tbl_info_valid = 1;
	req.v6_hash_route_tbl_info.start = 0x588;
	req.v6_hash_route_tbl_info.num_indices = 7;
	req.v4_hash_filter_tbl_start_addr_valid = 1;
	req.v4_hash_filter_tbl_start_addr = 0x288;
	req.v6_hash_filter_tbl_start_addr_valid = 1;
	req.v6_hash_filter_tbl_start_addr = 0x388;
	req.hw_stats_quota_base_addr_valid = 1;
	req.hw_stats_quota_base_addr = 0xc30;
	req.hw_stats_quota_size_valid = 1;
	req.hw_stats_quota_size = 0x60;
	req.hw_drop_stats_base_addr_valid = 1;
	req.hw_drop_stats_base_addr = 0x13d0;
	req.hw_drop_stats_table_size_valid = 1;
	req.hw_drop_stats_table_size = 0x20;

	{
		uint32_t o = vayu_ipa_restricted_bytes();
		req.hdr_tbl_info.start += o;
		req.hdr_tbl_info.end += o;
		req.v4_route_tbl_info.start += o;
		req.v6_route_tbl_info.start += o;
		req.v4_filter_tbl_start_addr += o;
		req.v6_filter_tbl_start_addr += o;
		req.modem_mem_info.start += o;
		req.hdr_proc_ctx_tbl_info.start += o;
		req.hdr_proc_ctx_tbl_info.end += o;
		req.v4_hash_route_tbl_info.start += o;
		req.v6_hash_route_tbl_info.start += o;
		req.v4_hash_filter_tbl_start_addr += o;
		req.v6_hash_filter_tbl_start_addr += o;
		req.hw_stats_quota_base_addr += o;
		req.hw_drop_stats_base_addr += o;
		say("vayu-ipa: restricted byte offset %#x applied to table addresses\n", o);
	}

	say("vayu-ipa: INIT_MODEM_DRIVER: MSM Android, routes 0..7, modem mem %#x/%#x, ctrl endpoint %u\n",
	    req.modem_mem_info.start, req.modem_mem_info.size, req.ctrl_comm_dest_end_pt);
	return ipa_qmi_request(VAYU_IPA_INIT_MODEM_DRIVER, vayu_ipa_init_req_ei, &req,
	    vayu_ipa_init_resp_ei, &resp, &resp.result);
}

static void
ipa_send_complete(void)
{
	struct vayu_ipa_init_complete_ind ind = { 0 };
	struct qmi_txn txn;
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = ipa_node, .sq_port = ipa_port };
	int ret;

	if (!indication_wanted || !init_done || !driver_complete || complete_sent || ipa_stop) return;
	ret = qmi_txn_init(&ipa_qmi, &txn, NULL, NULL);
	if (ret < 0) {
		say("vayu-ipa: INIT_COMPLETE: no transaction (%d)\n", ret);
		return;
	}
	ret = qmi_send_indication(&ipa_qmi, &to, &txn, VAYU_IPA_INIT_COMPLETE_IND, 7,
	    vayu_ipa_init_complete_ind_ei, &ind);
	qmi_txn_cancel(&txn);
	if (ret == 0) {
		complete_sent = true;
		say("vayu-ipa: INIT_COMPLETE indication sent\n");
	} else {
		say("vayu-ipa: INIT_COMPLETE send failed: %d\n", ret);
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
	say("vayu-ipa: modem message %#x type %u: result %u error %u\n", message, m[0], result, error);
	if (m[0] == 0) {
		/* A request on our published host port. Answer with the same transaction id,
		 * result TLV 0x02 = success. The table rules themselves are a later stage. */
		uint8_t answer[14] = { 2, m[1], m[2], m[3], m[4], 7, 0, 2, 4, 0, 0, 0, 0, 0 };
		(void)vayu_qrtr_ops.send(host_port, node, from_port, answer, sizeof(answer));
		say("vayu-ipa: modem request %#x answered success\n", message);
	}
	switch (message) {
	case VAYU_IPA_INDICATION_REGISTER:
		client_node = node;
		client_port = from_port;
		indication_wanted = true;
		ipa_send_complete();
		break;
	case VAYU_IPA_DRIVER_INIT_COMPLETE:
		driver_complete = true;
		say("vayu-ipa: DRIVER_INIT_COMPLETE received\n");
		ipa_send_complete();
		break;
	case VAYU_IPA_CONFIG_REQ:
	case VAYU_IPA_INSTALL_FILTER_RULE:
	case VAYU_IPA_FILTER_INSTALLED_NOTIF:
		say("vayu-ipa: modem IPA request %#x seen (table command, not yet applied)\n", message);
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
	say("vayu-ipa: modem IPA service at node %u port %u\n", service->node, service->port);
	ret = hakios_qmi_connect(qmi->sock, service->node, service->port);
	if (ret != 0) return ret;
	ipa_node = service->node;
	ipa_port = service->port;
	ipa_modem_there = true;
	return 0;
}

static void
ipa_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (ipa_node == service->node && ipa_port == service->port) {
		ipa_modem_there = false;
		indication_wanted = init_done = complete_sent = driver_complete = false;
		say("vayu-ipa: modem IPA service went away\n");
	}
}

static const struct qmi_ops ipa_qmi_ops = {
	.new_server = ipa_new_server,
	.del_server = ipa_del_server,
};

static void
ipa_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t waited = 0;
	int ret;

	ret = qmi_handle_init(&ipa_qmi, 4096, &ipa_qmi_ops, NULL);
	if (ret < 0) {
		say("vayu-ipa: qmi_handle_init: %d\n", ret);
		goto out;
	}
	ipa_qmi_up = true;
	ret = qmi_add_lookup(&ipa_qmi, VAYU_IPA_QMI_SERVICE, VAYU_IPA_QMI_INSTANCE, VAYU_IPA_MODEM_INSTANCE);
	if (ret < 0) {
		say("vayu-ipa: qmi_add_lookup: %d\n", ret);
		goto out;
	}
	while (!ipa_modem_there && !ipa_stop && waited < 120000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited += 100;
	}
	if (!ipa_modem_there) {
		say("vayu-ipa: modem IPA service 0x31/2 did not appear\n");
		goto out;
	}
	if (!vayu_ipa_firmware_ready()) {
		say("vayu-ipa: IPA firmware/prerequisites are not ready; INIT_MODEM_DRIVER is not sent\n");
		goto out;
	}
	for (unsigned attempt = 1; attempt <= 3 && !ipa_stop && !init_done; attempt++) {
		say("vayu-ipa: INIT_MODEM_DRIVER attempt %u/3\n", attempt);
		if (ipa_init_modem_driver() == 0) {
			init_done = true;
			break;
		}
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	if (!init_done) {
		say("vayu-ipa: INIT_MODEM_DRIVER failed; WDS Start remains blocked\n");
		goto out;
	}
	ipa_send_complete();
	while (!ipa_stop) delay_for_interval(100, NSEC_PER_MSEC);
out:
	if (ipa_qmi_up) {
		qmi_handle_release(&ipa_qmi);
		ipa_qmi_up = false;
	}
	ipa_running = false;
	say("vayu-ipa: QMI thread done\n");
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
	uint32_t enabled = 0;
	int ret;

	(void)q;
	if (!PE_parse_boot_argn("vayu_ipa", &enabled, sizeof(enabled)) || enabled != 1) {
		if (enabled != 0) say("vayu-ipa: vayu_ipa=%u refused: only 1 enables the IPA path\n", enabled);
		return;
	}
	if (ipa_started) return;
	ipa_started = true;
	ipa_running = true;
	ipa_stop = false;
	if (q == NULL) {
		say("vayu-ipa: no QRTR transport\n");
		ipa_running = false;
		return;
	}
	if (!vayu_ipa_firmware_ready()) {
		say("vayu-ipa: not publishing host IPA service: firmware/prerequisites are not ready\n");
		ipa_running = false;
		return;
	}
	host_port = vayu_qrtr_ops.open(ipa_receive, NULL, NULL);
	if (host_port == NULL) {
		say("vayu-ipa: cannot open the host IPA port\n");
		ipa_running = false;
		return;
	}
	ret = vayu_qrtr_publish(host_port, VAYU_IPA_QMI_SERVICE, VAYU_IPA_QMI_INSTANCE, VAYU_IPA_QMI_INSTANCE);
	if (ret != 0) {
		say("vayu-ipa: cannot publish host IPA service: %d\n", ret);
		vayu_qrtr_ops.close(host_port);
		host_port = NULL;
		ipa_running = false;
		return;
	}
	say("vayu-ipa: host IPA service 0x31/1 published\n");
	if (kernel_thread_start(ipa_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-ipa: no QMI thread\n");
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
	for (uint32_t waited = 0; waited < 12000 && ipa_running; waited += 20) delay_for_interval(20, NSEC_PER_MSEC);
	if (host_port != NULL) {
		vayu_qrtr_ops.close(host_port);
		host_port = NULL;
	}
	init_done = complete_sent = indication_wanted = false;
	ipa_started = false;
	say("vayu-ipa: stopped\n");
}
