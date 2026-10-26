/*
 * vayu cellular data-session control plane: DPM, WDA and WDS over the modem's QRTR.
 *
 * The NAS client proved registration first (0620: LTE MegaFon). This file starts an IPv4 data
 * session only after NAS reports REGISTERED and only with boot-arg vayu_cell_data=1:
 *
 *   DPM Open Port        embedded port, vayu AP->modem endpoint 2, modem->AP endpoint 11
 *   WDA Set Data Format  raw IP in QMAP, no checksum offload, downlink aggregation bounded
 *   WDS Bind Mux         mux 1 of embedded interface 1
 *   WDS Set IP Family    IPv4
 *   WDS Start Network    default profile or the optional vayu_cell_apn boot arg
 *   WDS Get Settings     address, gateway, mask, DNS and MTU, exposed through sysctl
 *
 * There is deliberately no IPA register access, no GSI ring, no rmnet/en1 and no packet sent.
 * `kern.vayu.cellular_data=1` means the modem accepted a session and returned an IPv4 address,
 * not that iOS can exchange packets. That next stage is IPA 4.1, whose endpoints/GSI mapping
 * (2/GSI3, 11/GSI6) is different from SM7325's 2/16 (4.11) and must not be copied.
 *
 * No storage is written by the host; the rmtfs overlay handles modem-side NV/EFS activity.
 * No APN or identifier is logged. All retries are bounded and each stage is read from its reply.
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
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vayu_cell_data.h>
#include <sys/vayu_cell_data_qmi.h>
#include <sys/vayu_ipa.h>
#include <sys/vayu_pas.h>

#define DATA_REPLY_MS           5000
#define DATA_STOP_MS            40000
#define DATA_HANDLE_MAX         4096
#define DATA_REQUEST_MAX        256
#define DATA_START_MAX_ATTEMPTS 12
#define DATA_START_REPLY_MS     30000
#define DATA_SETTINGS_MAX_ATTEMPTS 6

extern void loader_log_putc(char c);

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

enum data_stage {
	DATA_DPM,
	DATA_WDA,
	DATA_BIND,
	DATA_FAMILY,
	DATA_START,
	DATA_SETTINGS,
	DATA_UP,
	DATA_FAILED,
};

static void data_clear_sysctls(void);

static struct {
	struct qmi_handle qmi;
	bool qmi_up, started;
	volatile bool running;
	volatile bool stop, registered, cleanup, restart, cleanup_request, session_uncertain;
	volatile uint32_t cleanup_seq;
	volatile bool wds_there, wda_there, dpm_there;
	uint32_t wds_node, wds_port, wda_node, wda_port, dpm_node, dpm_port;
	uint32_t wds_version, wds_instance, wda_version, wda_instance, dpm_version, dpm_instance;
	uint32_t wait_s;
	enum data_stage stage;
	uint32_t stage_attempts;
	uint64_t next_ms;
	uint32_t requests, failures;
	uint32_t wds_handle;
	uint32_t wds_handle_node, wds_handle_port, wds_generation;
	uint32_t wds_handle_generation;
	uint32_t stop_attempts;
	char apn[VAYU_CELL_APN_MAX];
} data;

/* Public state: control-plane only, never evidence of working packets. */
static int cellular_data;
static int cellular_mtu;
static char cellular_ip[20], cellular_gateway[20], cellular_netmask[20], cellular_dns[44];
SYSCTL_DECL(_kern_vayu);
SYSCTL_NODE(_kern, OID_AUTO, vayu, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "vayu");
SYSCTL_INT(_kern_vayu, OID_AUTO, cellular_data, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_data, 0,
    "modem accepted an IPv4 data session; no packet path is implied");
SYSCTL_INT(_kern_vayu, OID_AUTO, cellular_mtu, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_mtu, 0,
    "data session MTU from WDS Get Current Settings");
SYSCTL_STRING(_kern_vayu, OID_AUTO, cellular_ip, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_ip, 0,
    "data session IPv4 address");
SYSCTL_STRING(_kern_vayu, OID_AUTO, cellular_gateway, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_gateway, 0,
    "data session IPv4 gateway");
SYSCTL_STRING(_kern_vayu, OID_AUTO, cellular_netmask, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_netmask, 0,
    "data session IPv4 netmask");
SYSCTL_STRING(_kern_vayu, OID_AUTO, cellular_dns, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_dns, 0,
    "data session DNS servers");

static int
data_new_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (service->service == VAYU_CELL_WDS_SERVICE) {
		if (data.wds_there && (data.wds_node != service->node || data.wds_port != service->port)) {
			data.restart = true;
			data.cleanup = true;
			data.cleanup_seq++;
			if (data.wds_handle != 0) {
				data.session_uncertain = true;
			}
		}
		data.wds_generation++;
		data.wds_node = service->node;
		data.wds_port = service->port;
		data.wds_version = service->version;
		data.wds_instance = service->instance;
		data.wds_there = true;
	} else if (service->service == VAYU_CELL_WDA_SERVICE) {
		data.wda_node = service->node;
		data.wda_port = service->port;
		data.wda_version = service->version;
		data.wda_instance = service->instance;
		data.wda_there = true;
	} else if (service->service == VAYU_CELL_DPM_SERVICE) {
		data.dpm_node = service->node;
		data.dpm_port = service->port;
		data.dpm_version = service->version;
		data.dpm_instance = service->instance;
		data.dpm_there = true;
	} else {
		return -1;
	}
	return 0;
}

static void
data_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service)
{
	if (service->service == VAYU_CELL_WDS_SERVICE && data.wds_node == service->node && data.wds_port == service->port) {
		data.wds_there = false;
	} else if (service->service == VAYU_CELL_WDA_SERVICE && data.wda_node == service->node && data.wda_port == service->port) {
		data.wda_there = false;
	} else if (service->service == VAYU_CELL_DPM_SERVICE && data.dpm_node == service->node && data.dpm_port == service->port) {
		data.dpm_there = false;
	}
	if (data.wds_handle != 0) {
		data.session_uncertain = true;
	}
	data.restart = true;
	data.cleanup = true;
	data.cleanup_seq++;
	data.next_ms = now_ms();
	data_clear_sysctls();
}

static const struct qmi_ops data_qmi_ops = {
	.new_server = data_new_server,
	.del_server = data_del_server,
};

static int
data_request_ms(const char *what, uint32_t node, uint32_t port, uint16_t message,
    const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, struct vayu_cell_result *result, uint32_t timeout_ms)
{
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = node, .sq_port = port };
	struct qmi_txn txn;
	int ret;

	if (data.stop && !data.cleanup_request) {
		return -ECANCELED;
	}
	data.requests++;
	result->result = 0xffff;
	result->error = 0xffff;
	ret = qmi_txn_init(&data.qmi, &txn, resp_ei, resp);
	if (ret < 0) {
		say("vayu-cell-data: %s: no transaction: %d\n", what, ret);
		data.failures++;
		return ret;
	}
	ret = qmi_send_request(&data.qmi, &to, &txn, message, DATA_REQUEST_MAX, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-cell-data: %s: cannot send: %d\n", what, ret);
		data.failures++;
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, timeout_ms);
	if (ret < 0) {
		say("vayu-cell-data: %s: no usable reply in %u ms (%d)\n", what, timeout_ms, ret);
		data.failures++;
		return ret;
	}
	if (data.stop && !data.cleanup_request) {
		return -ECANCELED;
	}
	if (result->result == 0xffff && result->error == 0xffff) {
		say("vayu-cell-data: %s: reply has no result TLV\n", what);
		data.failures++;
		return -EBADMSG;
	}
	if (result->result != QMI_RESULT_SUCCESS_V01 || result->error != QMI_ERR_NONE_V01) {
		say("vayu-cell-data: %s: result %u error %u\n", what, (unsigned int)result->result,
		    (unsigned int)result->error);
		data.failures++;
		return -EIO;
	}
	return 0;
}

static int
data_request(const char *what, uint32_t node, uint32_t port, uint16_t message,
    const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, struct vayu_cell_result *result)
{
	return data_request_ms(what, node, port, message, req_ei, req, resp_ei, resp, result, DATA_REPLY_MS);
}

static void
data_clear_sysctls(void)
{
	cellular_data = 0;
	cellular_mtu = 0;
	cellular_ip[0] = 0;
	cellular_gateway[0] = 0;
	cellular_netmask[0] = 0;
	cellular_dns[0] = 0;
}

static void
data_advance(enum data_stage next)
{
	data.stage = next;
	data.stage_attempts = 0;
	data.next_ms = now_ms() + 1000;
}

/* Called only from the data thread, never from a service-discovery callback. */
static bool
data_stop_session(void)
{
	struct vayu_cell_wds_stop_req req;
	struct vayu_cell_result_resp resp = { 0 };
	int ret;

	if (data.wds_handle == 0) {
		return true;
	}
	if (!data.wds_there || data.wds_node != data.wds_handle_node ||
	    data.wds_port != data.wds_handle_port || data.wds_generation != data.wds_handle_generation) {
		say("vayu-cell-data: WDS changed or is gone; old session handle %#x is UNCERTAIN, no new Start\n",
		    data.wds_handle);
		data.session_uncertain = true;
		return false;
	}
	req.handle = data.wds_handle;
	{
		uint32_t node = data.wds_node, port = data.wds_port;

		data.cleanup_request = true;
		ret = data_request("WDS Stop Network Interface", node, port,
		    VAYU_CELL_WDS_STOP_NETWORK_INTERFACE, vayu_cell_wds_stop_req_ei, &req,
		    vayu_cell_result_resp_ei, &resp, &resp.result);
		data.cleanup_request = false;
	}
	if (ret == -EIO && (resp.result.error == 15 || resp.result.error == 9 ||
	    resp.result.error == 65 || resp.result.error == 66)) {
		/* OUT_OF_CALL/INVALID_HANDLE/SESSION_INACTIVE/SESSION_INVALID mean gone. */
		say("vayu-cell-data: Stop handle %#x: already gone (QMI error %u)\n", req.handle,
		    (unsigned int)resp.result.error);
		data.wds_handle = 0;
		data.session_uncertain = false;
		return true;
	}
	if (ret != 0) {
		/* Timeout, malformed reply or QMI failure is not proof that the PDP is gone. */
		say("vayu-cell-data: Stop handle %#x is UNCERTAIN; no new Start this boot\n", req.handle);
		data.session_uncertain = true;
		return false;
	}
	say("vayu-cell-data: WDS Stop Network Interface handle %#x accepted\n", req.handle);
	data.wds_handle = 0;
	data.session_uncertain = false;
	return true;
}

static void
data_cleanup(void)
{
	uint32_t seq = data.cleanup_seq;
	bool confirmed = data_stop_session();

	data_clear_sysctls();
	if (!confirmed) {
		/* Never Start over a possibly live PDP. A bounded Stop retry is still allowed. */
		data.session_uncertain = true;
		data.stage = DATA_FAILED;
		if (data.stop || data.wds_handle == 0 || data.stop_attempts >= 3) {
			data.restart = false;
			data.cleanup = false;
			return;
		}
		data.stop_attempts++;
		data.cleanup = true;
		data.next_ms = now_ms() + 5000;
		return;
	}
	data.stop_attempts = 0;
	if (data.stop) {
		data.restart = false;
		data.cleanup = false;
		return;
	}
	if (seq != data.cleanup_seq) {
		/* A service/registration event arrived during Stop: do not lose it. */
		data.cleanup = true;
		return;
	}
	data.cleanup = false;
	if (data.restart && !data.session_uncertain) {
		data.stage = DATA_DPM;
		data.stage_attempts = 0;
		data.next_ms = now_ms() + 3000;
	}
	data.restart = false;
}

static bool
data_idempotent_ok(int ret, const struct vayu_cell_result_resp *resp)
{
	/* A repeated idempotent configuration may answer NO_EFFECT (26). Start is never treated this way. */
	return ret == 0 || (ret == -EIO && resp->result.error == 26);
}

static void
data_retry(const char *what, int error)
{
	uint32_t limit = data.stage == DATA_START ? DATA_START_MAX_ATTEMPTS :
	    data.stage == DATA_SETTINGS ? DATA_SETTINGS_MAX_ATTEMPTS : 3;
	uint32_t delay_ms;

	if (data.stage_attempts >= limit) {
		say("vayu-cell-data: %s failed after %u attempt(s); no more writes this boot\n", what,
		    (unsigned int)data.stage_attempts);
		data.stage = DATA_FAILED;
		data.cleanup = true;           /* stop any accepted session before giving up */
		return;
	}
	if (data.stage == DATA_START) {
		delay_ms = data.stage_attempts < 4 ? 6000 : data.stage_attempts < 8 ? 15000 : 60000;
	} else {
		delay_ms = data.stage_attempts * 2000;
	}
	data.next_ms = now_ms() + delay_ms;
	say("vayu-cell-data: %s will retry in %u ms (attempt %u/%u)%s\n", what, delay_ms,
	    (unsigned int)data.stage_attempts, limit, error == 70 ? "; waiting for the packet domain to settle" : "");
}

static void
data_dpm(void)
{
	struct vayu_cell_dpm_open_port_req req = { 0 };
	struct vayu_cell_result_resp resp = { 0 };
	int ret;

	req.ports_len = 1;
	req.ports[0].endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
	req.ports[0].endpoint.interface_number = VAYU_CELL_ENDPOINT_INTERFACE;
	vayu_ipa_modem_endpoints(&req.ports[0].rx_endpoint_number, &req.ports[0].tx_endpoint_number);
	data.stage_attempts++;
	say("vayu-cell-data: DPM Open Port: embedded interface %u, modem RX endpoint %u, TX endpoint %u\n",
	    VAYU_CELL_ENDPOINT_INTERFACE, req.ports[0].rx_endpoint_number, req.ports[0].tx_endpoint_number);
	ret = data_request("DPM Open Port", data.dpm_node, data.dpm_port, VAYU_CELL_DPM_OPEN_PORT,
	    vayu_cell_dpm_open_port_req_ei, &req, vayu_cell_result_resp_ei, &resp, &resp.result);
	if (data_idempotent_ok(ret, &resp)) {
		say("vayu-cell-data: DPM embedded data port is open\n");
		data_advance(DATA_WDA);
	} else {
		data_retry("DPM Open Port", resp.result.error);
	}
}

static void
data_wda(void)
{
	struct vayu_cell_wda_set_format_req req = { 0 };
	struct vayu_cell_result_resp resp = { 0 };
	int ret;

	req.link_layer_protocol = VAYU_CELL_RAW_IP;
	req.uplink_aggregation = VAYU_CELL_QMAP;
	req.downlink_aggregation = VAYU_CELL_QMAP;
	req.downlink_max_datagrams = 16;
	req.downlink_max_size = 0x3800;
	req.endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
	req.endpoint.interface_number = VAYU_CELL_ENDPOINT_INTERFACE;
	data.stage_attempts++;
	say("vayu-cell-data: WDA Set Data Format: raw IP in QMAP, no checksum offload\n");
	ret = data_request("WDA Set Data Format", data.wda_node, data.wda_port, VAYU_CELL_WDA_SET_DATA_FORMAT,
	    vayu_cell_wda_set_format_req_ei, &req, vayu_cell_result_resp_ei, &resp, &resp.result);
	if (data_idempotent_ok(ret, &resp)) {
		say("vayu-cell-data: WDA data format accepted\n");
		data_advance(DATA_BIND);
	} else {
		data_retry("WDA Set Data Format", resp.result.error);
	}
}

static void
data_bind(void)
{
	struct vayu_cell_wds_bind_req req = { 0 };
	struct vayu_cell_result_resp resp = { 0 };
	int ret;

	req.endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
	req.endpoint.interface_number = VAYU_CELL_ENDPOINT_INTERFACE;
	req.mux_id = VAYU_CELL_DATA_MUX;
	data.stage_attempts++;
	say("vayu-cell-data: WDS Bind Mux Data Port: mux %u\n", VAYU_CELL_DATA_MUX);
	ret = data_request("WDS Bind Mux Data Port", data.wds_node, data.wds_port, VAYU_CELL_WDS_BIND_MUX_DATA_PORT,
	    vayu_cell_wds_bind_req_ei, &req, vayu_cell_result_resp_ei, &resp, &resp.result);
	if (data_idempotent_ok(ret, &resp)) {
		say("vayu-cell-data: WDS client bound to mux %u\n", VAYU_CELL_DATA_MUX);
		data_advance(DATA_FAMILY);
	} else {
		data_retry("WDS Bind Mux Data Port", resp.result.error);
	}
}

static void
data_family(void)
{
	struct vayu_cell_wds_ip_family_req req = { .preference = VAYU_CELL_IPV4 };
	struct vayu_cell_result_resp resp = { 0 };
	int ret;

	data.stage_attempts++;
	ret = data_request("WDS Set IP Family IPv4", data.wds_node, data.wds_port, VAYU_CELL_WDS_SET_IP_FAMILY,
	    vayu_cell_wds_ip_family_req_ei, &req, vayu_cell_result_resp_ei, &resp, &resp.result);
	if (data_idempotent_ok(ret, &resp)) {
		say("vayu-cell-data: WDS IP family is IPv4\n");
		data_advance(DATA_START);
	} else {
		data_retry("WDS Set IP Family IPv4", resp.result.error);
	}
}

static void
data_start_session(void)
{
	struct vayu_cell_wds_start_req req = { 0 };
	struct vayu_cell_wds_start_resp resp = { 0 };
	int ret;

	uint32_t node = data.wds_node, port = data.wds_port, generation = data.wds_generation;

	req.ip_family = VAYU_CELL_IPV4;
	if (data.apn[0] != 0) {
		req.apn_valid = 1;
		strlcpy(req.apn, data.apn, sizeof(req.apn));
	}
	if (!data.registered || data.stop || data.cleanup || data.session_uncertain) {
		return;
	}
	data.stage_attempts++;
	say("vayu-cell-data: WDS Start Network Interface: IPv4, %s\n",
	    data.apn[0] != 0 ? "boot-arg APN" : "SIM default profile");
	ret = data_request_ms("WDS Start Network Interface", node, port,
	    VAYU_CELL_WDS_START_NETWORK_INTERFACE, vayu_cell_wds_start_req_ei, &req,
	    vayu_cell_wds_start_resp_ei, &resp, &resp.result, DATA_START_REPLY_MS);
	if (resp.handle_valid) {
		/* Bind the handle to the request's generation, not to a service that may have changed. */
		data.wds_handle = resp.handle;
		data.wds_handle_node = node;
		data.wds_handle_port = port;
		data.wds_handle_generation = generation;
		data.stop_attempts = 0;
		if (ret != 0 || node != data.wds_node || port != data.wds_port || generation != data.wds_generation) {
			say("vayu-cell-data: Start returned handle %#x but not a clean current session; stopping it, no duplicate\n",
			    data.wds_handle);
			data.session_uncertain = true;
			data.stage = DATA_FAILED;
			data.cleanup = true;
			data.cleanup_seq++;
			return;
		}
	}
	if (ret == -ETIMEDOUT) {
		/* Start is not idempotent: a lost reply cannot be followed by another Start. */
		say("vayu-cell-data: Start timed out; no duplicate Start is sent (possible session needs modem reset)\n");
		data.session_uncertain = true;
		data.stage = DATA_FAILED;
		data.cleanup = true;
		data.cleanup_seq++;
		return;
	}
	if (ret == 0 && resp.handle_valid) {
		data.wds_handle = resp.handle;
		say("vayu-cell-data: WDS data session accepted, handle %#x; asking for settings\n", data.wds_handle);
		data_advance(DATA_SETTINGS);
	} else {
		if (ret == 0) {
			say("vayu-cell-data: WDS Start Network Interface returned no packet-data handle; not retried\n");
			data.session_uncertain = true;
			data.stage = DATA_FAILED;
			data.cleanup = true;
			data.cleanup_seq++;
			return;
		}
		if (ret < 0 && ret != -EIO && ret != -ETIMEDOUT) {
			say("vayu-cell-data: Start reply/transport is uncertain (%d); no duplicate Start\n", ret);
			data.session_uncertain = true;
			data.stage = DATA_FAILED;
			data.cleanup = true;
			data.cleanup_seq++;
			return;
		}
		if (ret == -EIO && resp.result.error == 26) {
			say("vayu-cell-data: Start answered NO_EFFECT: a session may already exist; no duplicate Start\n");
			data.session_uncertain = true;
			data.stage = DATA_FAILED;
			data.cleanup = true;
			data.cleanup_seq++;
			return;
		}
		if (resp.call_end_reason_valid) {
			say("vayu-cell-data: WDS call end reason %u\n", (unsigned int)resp.call_end_reason);
		}
		data_retry("WDS Start Network Interface", resp.result.error);
	}
}

static void
ip4_string(char *to, size_t size, uint32_t address)
{
	snprintf(to, size, "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xff, (address >> 8) & 0xff,
	    address & 0xff);
}

static void
data_settings(void)
{
	struct vayu_cell_wds_settings_req req = { .requested_settings = VAYU_CELL_SETTINGS_MASK };
	struct vayu_cell_wds_settings_resp resp = { 0 };
	char ip[20], gw[20], mask[20], dns1[20], dns2[20];
	int ret;

	data.stage_attempts++;
	ret = data_request("WDS Get Current Settings", data.wds_node, data.wds_port,
	    VAYU_CELL_WDS_GET_CURRENT_SETTINGS, vayu_cell_wds_settings_req_ei, &req,
	    vayu_cell_wds_settings_resp_ei, &resp, &resp.result);
	if (ret != 0) {
		data_retry("WDS Get Current Settings", resp.result.error);
		return;
	}
	ip4_string(ip, sizeof(ip), resp.address_valid ? resp.address : 0);
	ip4_string(gw, sizeof(gw), resp.gateway_valid ? resp.gateway : 0);
	ip4_string(mask, sizeof(mask), resp.mask_valid ? resp.mask : 0);
	ip4_string(dns1, sizeof(dns1), resp.dns1_valid ? resp.dns1 : 0);
	ip4_string(dns2, sizeof(dns2), resp.dns2_valid ? resp.dns2 : 0);
	say("vayu-cell-data: session settings: address %s gateway %s netmask %s DNS %s %s MTU %u\n",
	    ip, gw, mask, dns1, dns2, resp.mtu_valid ? resp.mtu : 0);
	if (resp.ip_family_valid && resp.ip_family != VAYU_CELL_IPV4) {
		say("vayu-cell-data: settings are not IPv4 (family %u); stopping the accepted session\n",
		    (unsigned int)resp.ip_family);
		data.stage = DATA_FAILED;
		data.cleanup = true;
		return;
	}
	if (!resp.address_valid || resp.address == 0) {
		say("vayu-cell-data: no IPv4 address in the settings; the session is not considered up\n");
		data_retry("WDS Get Current Settings (no address)", resp.result.error);
		return;
	}
	if (!data.registered || data.stop || data.cleanup || data.session_uncertain) {
		/* A registration loss or stop during the reply must not resurrect cellular_data=1. */
		data.cleanup = true;
		return;
	}
	strlcpy(cellular_ip, ip, sizeof(cellular_ip));
	strlcpy(cellular_gateway, gw, sizeof(cellular_gateway));
	strlcpy(cellular_netmask, mask, sizeof(cellular_netmask));
	snprintf(cellular_dns, sizeof(cellular_dns), "%s%s%s", resp.dns1_valid && resp.dns1 ? dns1 : "",
	    resp.dns1_valid && resp.dns1 && resp.dns2_valid && resp.dns2 ? " " : "",
	    resp.dns2_valid && resp.dns2 ? dns2 : "");
	cellular_mtu = resp.mtu_valid ? (int)resp.mtu : 0;
	if (data.stop || data.cleanup || data.session_uncertain || !data.registered) {
		data_clear_sysctls();
		data.cleanup = true;
		return;
	}
	cellular_data = 1;
	data.stage = DATA_UP;
	say("vayu-cell-data: CONTROL PLANE UP: IPv4 session and settings accepted; "
	    "kern.vayu.cellular_* are set. IPA/GSI/en1 is NOT up: no packets or internet yet\n");
}

static void
data_round(void)
{
	if (data.cleanup && now_ms() >= data.next_ms) {
		data_cleanup();
	}
	if (data.stop || data.cleanup) {
		return;
	}
	if (!data.registered || data.stage == DATA_UP || data.stage == DATA_FAILED || now_ms() < data.next_ms) {
		return;
	}
	if (!vayu_ipa_ready()) {
		static uint64_t last_gate_log;
		if (now_ms() - last_gate_log > 5000) {
			last_gate_log = now_ms();
			say("vayu-cell-data: WDS/DPM blocked: vayu IPA 4.1 is not ready%s%s\n",
			    vayu_ipa_pending() ? " (vayu_ipa=1 pending INIT_MODEM_DRIVER)" :
			    " (enable vayu_ipa=1 first; no Start is sent)", "");
		}
		return;
	}
	if (!data.dpm_there || !data.wda_there || !data.wds_there) {
		say("vayu-cell-data: registered, waiting for DPM %s WDA %s WDS %s\n",
		    data.dpm_there ? "there" : "MISSING", data.wda_there ? "there" : "MISSING",
		    data.wds_there ? "there" : "MISSING");
		data.next_ms = now_ms() + 5000;
		return;
	}
	switch (data.stage) {
	case DATA_DPM: data_dpm(); break;
	case DATA_WDA: data_wda(); break;
	case DATA_BIND: data_bind(); break;
	case DATA_FAMILY: data_family(); break;
	case DATA_START: data_start_session(); break;
	case DATA_SETTINGS: data_settings(); break;
	default: break;
	}
}

bool
vayu_cell_data_pending(void)
{
	return data.started && (data.cleanup || data.restart || (data.stage != DATA_UP && data.stage != DATA_FAILED));
}

static void
data_thread(void *argument __unused, wait_result_t wait __unused)
{
	uint64_t waited_ms = 0;
	int ret;

	ret = qmi_handle_init(&data.qmi, DATA_HANDLE_MAX, &data_qmi_ops, NULL);
	if (ret < 0) {
		say("vayu-cell-data: qmi_handle_init: %d\n", ret);
		goto out;
	}
	data.qmi_up = true;
	ret = qmi_add_lookup(&data.qmi, VAYU_CELL_DPM_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-cell-data: qmi_add_lookup(DPM): %d\n", ret);
		goto out;
	}
	ret = qmi_add_lookup(&data.qmi, VAYU_CELL_WDA_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-cell-data: qmi_add_lookup(WDA): %d\n", ret);
		goto out;
	}
	ret = qmi_add_lookup(&data.qmi, VAYU_CELL_WDS_SERVICE, 1, 0);
	if (ret < 0) {
		say("vayu-cell-data: qmi_add_lookup(WDS): %d\n", ret);
		goto out;
	}
	while ((!data.dpm_there || !data.wda_there || !data.wds_there) && !data.stop &&
	    waited_ms < (uint64_t)data.wait_s * 1000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited_ms += 100;
	}
	say("vayu-cell-data: after %llu ms: DPM %s WDA %s WDS %s\n", (unsigned long long)waited_ms,
	    data.dpm_there ? "there" : "MISSING", data.wda_there ? "there" : "MISSING",
	    data.wds_there ? "there" : "MISSING");
	for (;;) {
		if ((data.stage == DATA_START || data.stage == DATA_SETTINGS) && !vayu_ipa_ready()) {
			say("vayu-cell-data: IPA readiness was lost during Start/Settings; stopping the client\n");
			data.stop = true;
			data.cleanup = true;
			data.cleanup_seq++;
		}
		data_round();
		if (data.stop && !data.cleanup) {
			break;
		}
		delay_for_interval(100, NSEC_PER_MSEC);
	}
out:
	if (data.cleanup) {
		data_cleanup();
	}
	data_clear_sysctls();
	if (data.qmi_up) {
		qmi_handle_release(&data.qmi);
		data.qmi_up = false;
	}
	data.running = false;
	say("vayu-cell-data: thread done: %u request(s), %u failure(s)\n", data.requests, data.failures);
	thread_terminate(current_thread());
}

void
vayu_cell_data_registered(bool registered)
{
	if (registered && !vayu_ipa_ready()) {
		data.registered = registered;
		if (data.started) {
			data.restart = false;
			data.cleanup = true;
			data.cleanup_seq++;
			data_clear_sysctls();
		}
		return;
	}
	if (data.registered == registered) {
		return;
	}
	data.registered = registered;
	if (registered) {
		say("vayu-cell-data: NAS is REGISTERED: data-session setup may proceed\n");
		if (data.session_uncertain && data.wds_handle != 0) {
			data.cleanup = true;       /* retry the old Stop, never a new Start */
			data.next_ms = now_ms() + 1000;
		} else {
			if (data.stage == DATA_FAILED && !data.session_uncertain) {
				data.stage = DATA_DPM;
				data.stage_attempts = 0;
			}
			data.next_ms = now_ms() + 2000;
		}
	} else if (data.started) {
		say("vayu-cell-data: NAS registration lost; stop the old session and wait for a new registration\n");
		data.restart = true;
		data.cleanup = true;
		data.cleanup_seq++;
		data.next_ms = now_ms();
		data_clear_sysctls();
	}
}

void
vayu_cell_data_start(struct vayu_qrtr *q)
{
	thread_t thread;
	uint32_t enabled = 0, wait_s = 60;

	if (!PE_parse_boot_argn("vayu_cell_data", &enabled, sizeof(enabled)) || enabled != 1) {
		if (enabled != 0) {
			say("vayu-cell-data: vayu_cell_data=%u is refused: only 1 enables a data session\n", enabled);
		}
		return;
	}
	if (data.started) {
		say("vayu-cell-data: already started\n");
		return;
	}
	if (q == NULL || q->glink == NULL || !q->glink->up) {
		say("vayu-cell-data: modem GLINK is not up\n");
		return;
	}
	PE_parse_boot_argn("vayu_cell_data_wait", &wait_s, sizeof(wait_s));
	if (wait_s < 1 || wait_s > 120) {
		wait_s = 60;
	}
	memset(&data, 0, sizeof(data));
	data_clear_sysctls();
	data.wait_s = wait_s;
	data.stage = DATA_DPM;
	data.next_ms = now_ms() + 1000;
	data.running = true;
	if (PE_parse_boot_argn("vayu_cell_apn", data.apn, sizeof(data.apn))) {
		data.apn[sizeof(data.apn) - 1] = 0;
	}
	say("vayu-cell-data: vayu_cell_data=1: DPM/WDA/WDS after NAS registration and IPA readiness; no packets yet\n");
	data.started = true;
	if (kernel_thread_start(data_thread, NULL, &thread) != KERN_SUCCESS) {
		say("vayu-cell-data: no thread\n");
		data.running = false;
		data.started = false;
		return;
	}
	thread_deallocate(thread);
}

void
vayu_cell_data_stop(void)
{
	if (!data.started) {
		return;
	}
	data.stop = true;
	data.cleanup = true;
	data.cleanup_seq++;
	data.next_ms = now_ms();
	for (uint32_t waited = 0; waited < DATA_STOP_MS && data.running; waited += 20) {
		delay_for_interval(20, NSEC_PER_MSEC);
	}
	if (data.running) {
		say("vayu-cell-data: thread did not stop in %u ms; leave it marked started\n", DATA_STOP_MS);
	} else {
		data.started = false;
		say("vayu-cell-data: stopped\n");
	}
	data_clear_sysctls();
}
