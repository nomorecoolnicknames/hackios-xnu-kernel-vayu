/*
 * vayu cellular data-session control plane: DPM, WDA and WDS over the modem's QRTR.
 *
 * The NAS client proved registration first (0620: LTE). This file starts an IPv4 data
 * session only after NAS reports REGISTERED. Two switches exist:
 *
 *   vayu_cell_data=1        the old full-path gate: WDS Start only when the IPA QMI handshake,
 *                           the GSI/data path and en1 are all ready.
 *   vayu_cell_lte=1         this task's switch: the APN of vayu_cell_apn, without it the SIM's default profile;
 *                           vayu_cell_lte_control=1 (default) allows WDS Start and the address
 *                           after the IPA QMI handshake alone, without GSI. It never claims
 *                           packets in that mode. The packet path is used when vayu_ipa_data=1
 *                           and vayu_cell_net=1 are ready too; vayu_cell_ping=1 then sends the
 *                           ICMP test.
 *   vayu_cell_data_port=1   with vayu_cell_lte=1 or vayu_wds=1: DPM Open Port and WDA Set Data
 *                           Format only, then stop -- no Bind, no Start, no address. The session is
 *                           then CommCenter's own (bsd/dev/vayu_qmux.c, vayu_qmux_wds_bind=1).
 *   vayu_cell_data_watch=1  an UP session is ended too (interface down, WDS Stop) when the IPA
 *                           handshake or GSI is lost under it; without it only Start/Settings are.
 *
 * The WDS conversation is:
 *
 *   DPM Open Port        embedded port, vayu AP->modem endpoint 2, modem->AP endpoint 11
 *   WDA Set Data Format  raw IP in QMAP, no checksum offload, downlink aggregation bounded
 *   WDS Bind Mux         mux 1 of embedded interface 1
 *   WDS Set IP Family    IPv4
 *   WDS Start Network    default profile or the optional vayu_cell_apn boot arg
 *   WDS Get Settings     address, gateway, mask, DNS and MTU, exposed through sysctl
 *
 * This file itself never touches an IPA register or a GSI ring. In control-only mode it also does
 * not touch en1 or send a packet: `kern.vayu.cellular_data=1` then means the modem accepted a
 * session and returned an IPv4 address, not that iOS can exchange packets. When the data path is
 * ready (vayu_ipa_data.c + vayu_cell_net.c) the same session configures en1 and the ICMP test may
 * run. IPv4 logging is masked to the last octet; the full value stays in the sysctls/interface.
 *
 * a11 (h1-h3: WDS Start "result 1 error 14" CALL_FAILED on UMTS with PS detached and on LTE with PS
 * attached; the reply carried a Packet Data Handle TLV of 0, which this file took for a session and
 * so never retried nor printed the reason). The call end reason (TLV 0x10) and the verbose call end
 * reason (TLV 0x11: type, reason) are now always printed. Three switches, all default 0:
 *   vayu_cell_start_retry=1    a Start answered with an error and handle 0 created no session (the
 *                              modem says so with the error itself): it is retried with the usual
 *                              Start backoff (6 s x3, 15 s x4, 60 s, 12 attempts) instead of ending
 *                              the client.
 *   vayu_cell_profile_log=1    read only: WDS Get Default Profile Number (3GPP, embedded) and Get
 *                              Profile Settings of that index, once before the first Start: name, PDP
 *                              type, APN, authentication (no username or password is decoded).
 *   vayu_cell_start_profile=N  Start with the 3GPP profile index N (TLV 0x31, 1..24) and no APN TLV,
 *                              as a modem-provisioned profile is used by the vendor's data services;
 *                              255: the index the modem reports as its default (Get Default Profile Number).
 * a11 i-series: every Start (APN, profile #1, after a PS attach) is refused with call end reason 3
 * (GENERIC_NO_SERVICE) and verbose CM 2001 (QMI_WDS_VERBOSE_CALL_END_REASON_CM_NO_SERVICE) while NAS
 * says REGISTERED LTE, CS+PS attached. The NAS client sees the registered subscription; the hypothesis
 * is that this WDS client is not on it (the phone is dual-SIM; vayu_cell_provision moves logical slot 1
 * to physical slot 2, and WDS's default is the modem's default data subscription). Two switches:
 *   vayu_cell_wds_diag=1       read only: WDS Get Bind Subscription and DSD Get System Status (DSD
 *                              0x2a, the data system the modem's data services see) before the first
 *                              Start and after every refused one (at most 4 times).
 *   vayu_cell_wds_sub=N        WDS Bind Subscription (0x00af) to N (1 primary, 2 secondary) once,
 *                              before the first Start; the reply and the binding read back are printed.
 * a11 j1 (vayu_cell_wds_sub=1): no refusal any more, the Start's reply did not come in 30 s, and from
 * then on the modem answered no QMI request of any service (UIM, NAS, DMS), and GLINK sends failed
 * with ENOBUFS. Two switches, default 0/30000:
 *   vayu_cell_start_timeout_ms=MS  how long one Start reply is waited for (1000..180000; default 30000;
 *                              libqmi's qmicli waits 180 s for it).
 *   vayu_cell_start_adopt=1    after a Start timeout: the modem's own state is printed (crash item 421,
 *                              SMP2P 435), then WDS Get Packet Service Status every 5 s for 60 s. If the
 *                              session is CONNECTED, one more Start fetches its handle (the modem
 *                              answers NO_EFFECT with the handle of the session that exists), and the
 *                              session is taken over: settings, interface, ping. Otherwise it ends as before.
 * a11 k0: the Start (subscription 1) made the modem fail: SFR "ds_rmnet_meta_sm.c:5461:Assertion 0 failed",
 * SMP2P 435 slave-kernel 0x7 (error fatal). The modem's rmnet instance for this call was set up by
 * DPM Open Port, WDA Set Data Format and WDS Bind Mux; each switch below changes one of those, all default
 * to the old sequence. The WDA reply (what the modem applied) is always printed now.
 *   vayu_cell_wds_sub_first=1  with vayu_cell_wds_sub: Bind Subscription before Bind Mux Data Port (k0 bound
 *                              the subscription after the mux binding and the IP family, so the rmnet
 *                              instance was bound under the client's old "any" subscription).
 *   vayu_cell_wda_ul_agg=0     WDA uplink aggregation DISABLED instead of QMAP (the request gives no uplink
 *                              maximum datagrams/size, TLVs 0x1B/0x1C).
 *   vayu_cell_dpm=0            no DPM Open Port (the libqmi/ModemManager recipe for an IPA modem goes
 *                              WDA -> Bind Mux -> Start; vendor netmgr opens the port first).
 * a11 m-series: m0 (sub first) read the client's subscription back as 255 after Bind Mux: the mux
 * binding resets it, and the Start was refused CM 2001 again. m1 (no UL aggregation) asserted the same.
 * m2 (no DPM): WDA was refused. The WDA reply said "DL max 16 x 0x3f" to a request of 16 x 0x3800 (the
 * request bytes are libqmi's: TLV 0x16, u32 little endian, host-checked). Now always:
 *   - the WDA request and reply datagrams are printed as hex ("vayu-cell-data: WDA tx/rx ...");
 *   - vayu_cell_wds_sub_first=2: Bind Subscription before Bind Mux and again after it, so the mux
 *     binding is made while the client is on subscription 1 and the client stays on it for the Start.
 *   - vayu_cell_wait_modem_init=1 (a11 o-series): no WDS Start until the modem's IPA DRIVER_INIT_COMPLETE
 *     (0x35) was answered: INIT_MODEM_DRIVER says "init pending 1" and every Start so far came before a
 *     0x35 that never came (n0/n1).
 *   - vayu_cell_wda_dl_datagrams=N / vayu_cell_wda_dl_size=B (defaults 16 / 0x3800, the AP's EP11
 *     aggregation: 16 packets, 14 KiB, in 16 KiB receive buffers) for a WDA probe that differs only there.
 *
 * No storage is written by the host; the rmtfs overlay handles modem-side NV/EFS activity.
 * The APN is a boot arg only, not an identifier. All retries are bounded and each stage is
 * read from its reply.
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
#include <sys/hakios.net.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vayu_cell_data.h>
#include <sys/vayu_cell_data_qmi.h>
#include <sys/vayu_cell_net.h>
#include <sys/vayu_ipa.h>
#include <sys/vayu_ipa_data.h>
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

/*
 * One summary line per bring-up stage, the shape the reports grep:
 *   vayu-data: S<n> <step>: OK|ERROR|TIMEOUT <detail>
 * The detailed vayu-cell-data/vayu-ipa-data lines stay as they are; this is the index.
 */
static void
data_stage_line(uint32_t stage, const char *step, const char *result, const char *format, ...)
{
	char detail[180], line[260];
	va_list list;

	va_start(list, format);
	vsnprintf(detail, sizeof(detail), format, list);
	va_end(list);
	snprintf(line, sizeof(line), "vayu-data: S%u %s: %s %s\n", stage, step, result, detail);
	for (const char *c = line; *c != 0; c++) loader_log_putc(*c);
	printf("%s", line);
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
	DATA_PORT,              /* vayu_cell_data_port=1: DPM and WDA done, no session of our own */
};

static void data_clear_sysctls(void);
static bool data_packet_path_ready(void);

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
	/* vayu_cell_lte=1: this task's opt-in. control_only=1 (default) starts WDS and reads the
	 * address after the IPA QMI handshake even when the GSI/data path is not ready; it never
	 * claims packets in that state. vayu_cell_data=1 keeps the old fail-closed behaviour.
	 * vayu_wds=1 is the k26 switch: the same WDS client, the APN of vayu_cell_apn, and pdp_ip0 on the
	 * net side; it also defaults to control_only=1 and never enables IPv6. */
	bool lte;
	bool wds;
	bool control_only;
	bool watch_up;                 /* vayu_cell_data_watch=1: a lost IPA/GSI ends an UP session too */
	bool port_only;                /* vayu_cell_data_port=1: DPM + WDA only; WDS is CommCenter's */
	bool net_session;              /* vayu_cell_net_session(true) was called for this session */
	uint64_t stats_ms;
	uint32_t stats_last_tx, stats_last_rx;
	char apn[VAYU_CELL_APN_MAX];
	/* a11 switches (see the top of this file) */
	bool start_retry, profile_log, profile_done;
	uint32_t start_profile;        /* 0 none, 1..24, 255 the modem's default */
	uint8_t profile_index;         /* resolved for 255; 0 unknown */
	bool wds_diag, sub_done;
	uint32_t wds_sub, diag_count;
	volatile bool dsd_there;
	uint32_t dsd_node, dsd_port;
	uint32_t start_timeout_ms;
	bool adopt;
	bool dpm_skip;
	uint32_t sub_first;            /* 1 before Bind Mux, 2 before and after it */
	uint32_t wda_ul_agg, wda_dl_datagrams, wda_dl_size;
	bool wait_modem_init;
	bool raw_want;                 /* data_request_ms keeps the reply datagram in raw_reply */
	uint8_t raw_reply[160];
	size_t raw_length;
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
SYSCTL_INT(_kern_vayu, OID_AUTO, ipv4_only, CTLFLAG_RD | CTLFLAG_LOCKED, &hakios_ipv4_only, 0,
    "vayu_ipv4_only=1: WDS is IPv4 only and en0/en1/pdp_ip0 carry no IPv6");

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
	} else if (service->service == VAYU_CELL_DSD_SERVICE) {
		data.dsd_node = service->node;
		data.dsd_port = service->port;
		data.dsd_there = true;
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
	if (service->service == VAYU_CELL_DSD_SERVICE) {
		data.dsd_there = false;        /* diagnostics only: no session depends on it */
		return;
	}
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
	data.raw_length = 0;
	if (data.raw_want) {
		txn.raw = data.raw_reply;
		txn.raw_size = sizeof(data.raw_reply);
		txn.raw_length = 0;
	}
	ret = qmi_send_request(&data.qmi, &to, &txn, message, DATA_REQUEST_MAX, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-cell-data: %s: cannot send: %d\n", what, ret);
		data.failures++;
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, timeout_ms);
	if (data.raw_want) data.raw_length = txn.raw_length;
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
	if (data.net_session) {
		vayu_cell_net_session(false, 0, 0, 0, 0, 0, 0);
		data.net_session = false;
	}
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
		data_stage_line(3, "DPM OPEN PORT", "OK", "embedded interface %u, modem RX EP %u, TX EP %u",
		    VAYU_CELL_ENDPOINT_INTERFACE, req.ports[0].rx_endpoint_number, req.ports[0].tx_endpoint_number);
		data_advance(DATA_WDA);
	} else {
		data_retry("DPM Open Port", resp.result.error);
	}
}

/* One datagram as hex, 32 bytes a line. */
static void
data_say_hex(const char *what, const uint8_t *bytes, size_t length)
{
	for (size_t at = 0; at < length; at += 32) {
		char hex[3 * 32 + 1];
		size_t used = 0;

		for (size_t i = at; i < length && i < at + 32; i++) {
			used += (size_t)snprintf(hex + used, sizeof(hex) - used, "%02x ", bytes[i]);
		}
		say("vayu-cell-data: %s +%03zu: %s\n", what, at, hex);
	}
}

static void
data_wda(void)
{
	struct vayu_cell_wda_set_format_req req = { 0 };
	struct vayu_cell_wda_set_format_resp resp = { 0 };
	int ret;

	req.link_layer_protocol = VAYU_CELL_RAW_IP;
	req.uplink_aggregation = data.wda_ul_agg;      /* VAYU_CELL_QMAP unless vayu_cell_wda_ul_agg=0 */
	req.downlink_aggregation = VAYU_CELL_QMAP;
	req.downlink_max_datagrams = data.wda_dl_datagrams;
	req.downlink_max_size = data.wda_dl_size;
	req.endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
	req.endpoint.interface_number = VAYU_CELL_ENDPOINT_INTERFACE;
	data.stage_attempts++;
	say("vayu-cell-data: WDA Set Data Format: raw IP in QMAP, no checksum offload\n");
	{
		uint8_t tx[96];
		int length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDA_SET_DATA_FORMAT, 0, vayu_cell_wda_set_format_req_ei,
		    &req, tx, sizeof(tx));

		if (length > 0) data_say_hex("WDA tx (transaction 0 here)", tx, (size_t)length);
	}
	data.raw_want = true;
	ret = data_request("WDA Set Data Format", data.wda_node, data.wda_port, VAYU_CELL_WDA_SET_DATA_FORMAT,
	    vayu_cell_wda_set_format_req_ei, &req, vayu_cell_wda_set_format_resp_ei, &resp, &resp.result);
	data.raw_want = false;
	if (data.raw_length > 0) data_say_hex("WDA rx", data.raw_reply, data.raw_length);
	else say("vayu-cell-data: WDA rx: no raw reply kept (ret %d)\n", ret);
	say("vayu-cell-data: WDA applied: link %s%u, UL agg %s%u (max %s%u x %s%u), DL agg %s%u (max %s%u x %s%#x), "
	    "flow control %s%u (asked: UL agg %u)\n",
	    resp.llp_valid ? "" : "-", resp.llp, resp.ul_valid ? "" : "-", resp.ul,
	    resp.ul_datagrams_valid ? "" : "-", resp.ul_datagrams, resp.ul_size_valid ? "" : "-", resp.ul_size,
	    resp.dl_valid ? "" : "-", resp.dl, resp.dl_datagrams_valid ? "" : "-", resp.dl_datagrams,
	    resp.dl_size_valid ? "" : "-", resp.dl_size, resp.flow_valid ? "" : "-", (unsigned)resp.flow,
	    req.uplink_aggregation);
	if (data_idempotent_ok(ret, (struct vayu_cell_result_resp *)&resp)) {
		say("vayu-cell-data: WDA data format accepted\n");
		data_stage_line(4, "WDA SET DATA FORMAT", "OK", "raw IP in QMAP, downlink max %u x %#x",
		    req.downlink_max_datagrams, req.downlink_max_size);
		if (data.port_only) {
			/* vayu_cell_data_port=1: the port is ready; the session is CommCenter's, over
			 * the QMux bridge (vayu_qmux_wds_bind=1 points its Bind at this port). */
			data_stage_line(5, "WDS BIND MUX", "SKIPPED",
			    "vayu_cell_data_port=1: embedded interface %u is ready for CommCenter's WDS client",
			    VAYU_CELL_ENDPOINT_INTERFACE);
			data.stage = DATA_PORT;
			return;
		}
		data_advance(DATA_BIND);
	} else {
		data_retry("WDA Set Data Format", resp.result.error);
	}
}

static void data_wds_bind_subscription(void);

static void
data_bind(void)
{
	struct vayu_cell_wds_bind_req req = { 0 };
	struct vayu_cell_result_resp resp = { 0 };
	int ret;

	req.endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
	req.endpoint.interface_number = VAYU_CELL_ENDPOINT_INTERFACE;
	req.mux_id = VAYU_CELL_DATA_MUX;
	if (data.wds_sub != 0 && data.sub_first && !data.sub_done) {
		data_wds_bind_subscription();   /* vayu_cell_wds_sub_first=1: before the mux binding */
	}
	data.stage_attempts++;
	say("vayu-cell-data: WDS Bind Mux Data Port: mux %u\n", VAYU_CELL_DATA_MUX);
	ret = data_request("WDS Bind Mux Data Port", data.wds_node, data.wds_port, VAYU_CELL_WDS_BIND_MUX_DATA_PORT,
	    vayu_cell_wds_bind_req_ei, &req, vayu_cell_result_resp_ei, &resp, &resp.result);
	if (data_idempotent_ok(ret, &resp)) {
		say("vayu-cell-data: WDS client bound to mux %u\n", VAYU_CELL_DATA_MUX);
		if (data.wds_sub != 0 && data.sub_first == 2) {
			data.sub_done = false;  /* the mux binding put the client back on "any" (m0 read 255) */
			data_wds_bind_subscription();
		}
		data_stage_line(5, "WDS BIND MUX", "OK", "embedded interface %u mux %u", VAYU_CELL_ENDPOINT_INTERFACE,
		    VAYU_CELL_DATA_MUX);
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
		data_stage_line(6, "WDS IP FAMILY", "OK", "IPv4");
		data_advance(DATA_START);
	} else {
		data_retry("WDS Set IP Family IPv4", resp.result.error);
	}
}

/* QmiWdsVerboseCallEndReasonType, and the 3GPP session-management causes (TS 24.008 10.5.6.6) that
 * a PDP/PDN activation is refused with. Anything else is printed as its number. */
static const char *
data_verbose_type_name(uint16_t type)
{
	switch (type) {
	case 1: return "MIP";
	case 2: return "internal";
	case 3: return "call manager";
	case 6: return "3GPP";
	case 7: return "PPP";
	case 8: return "EHRPD";
	case 9: return "IPv6";
	default: return "unknown type";
	}
}

static const char *
data_3gpp_cause_name(uint16_t reason)
{
	switch (reason) {
	case 8: return "operator determined barring";
	case 25: return "LLC or SNDCP failure";
	case 26: return "insufficient resources";
	case 27: return "missing or unknown APN";
	case 28: return "unknown PDP address or type";
	case 29: return "user authentication failed";
	case 30: return "activation rejected by GGSN/PGW";
	case 31: return "activation rejected, unspecified";
	case 32: return "service option not supported";
	case 33: return "requested service option not subscribed";
	case 34: return "service option temporarily out of order";
	case 35: return "NSAPI already used";
	case 36: return "regular deactivation";
	case 37: return "QoS not accepted";
	case 38: return "network failure";
	case 39: return "reactivation requested";
	case 40: return "feature not supported";
	case 50: return "PDN type IPv4 only allowed";
	case 51: return "PDN type IPv6 only allowed";
	case 52: return "single address bearers only allowed";
	case 112: return "APN restriction value incompatible";
	default: return "";
	}
}

/* vayu_cell_profile_log=1 / vayu_cell_start_profile=255: what the modem would use without an APN. */
static void
data_profile_read(void)
{
	struct vayu_cell_wds_default_profile_req dreq = { .profile = { .type = 0, .family = 0 } };
	struct vayu_cell_wds_default_profile_resp dresp = { 0 };
	struct vayu_cell_wds_profile_req preq = { 0 };
	static struct vayu_cell_wds_profile_resp presp;
	uint8_t index = 0;
	int ret;

	data.profile_done = true;
	ret = data_request("WDS Get Default Profile Number", data.wds_node, data.wds_port,
	    VAYU_CELL_WDS_GET_DEFAULT_PROFILE_NUM, vayu_cell_wds_default_profile_req_ei, &dreq,
	    vayu_cell_wds_default_profile_resp_ei, &dresp, &dresp.result);
	if (ret == 0 && dresp.index_valid) {
		index = dresp.index;
		data.profile_index = index;
	}
	say("vayu-cell-data: WDS default 3GPP profile (embedded): %s%u\n", ret == 0 && dresp.index_valid ? "#" : "unknown ",
	    (unsigned)index);
	if (data.start_profile != 0 && data.start_profile != 255) index = (uint8_t)data.start_profile;
	if (index == 0) return;
	memset(&presp, 0, sizeof(presp));
	preq.profile.type = 0;
	preq.profile.family = index;            /* the second byte is the index in this message */
	ret = data_request("WDS Get Profile Settings", data.wds_node, data.wds_port, VAYU_CELL_WDS_GET_PROFILE_SETTINGS,
	    vayu_cell_wds_profile_req_ei, &preq, vayu_cell_wds_profile_resp_ei, &presp, &presp.result);
	if (ret != 0) return;
	say("vayu-cell-data: WDS profile 3GPP #%u: name \"%s\", PDP type %s%u (0 IPv4, 2 IPv6, 3 IPv4v6), APN \"%s\", "
	    "authentication %s%u (0 none, 1 PAP, 2 CHAP)\n", (unsigned)index, presp.name_valid ? presp.name : "",
	    presp.pdp_type_valid ? "" : "absent ", (unsigned)presp.pdp_type, presp.apn_valid ? presp.apn : "",
	    presp.auth_valid ? "" : "absent ", (unsigned)presp.auth);
}

/* vayu_cell_wds_diag=1: which subscription this WDS client is on, and which data systems DSD sees. */
static void
data_wds_diag(const char *when)
{
	struct vayu_cell_wds_get_subscription_resp sub = { 0 };
	static struct vayu_cell_dsd_status_resp dsd;
	char systems[160];
	size_t used = 0;
	int ret;

	if (!data.wds_diag || data.diag_count >= 4) return;
	data.diag_count++;
	ret = data_request("WDS Get Bind Subscription", data.wds_node, data.wds_port, VAYU_CELL_WDS_GET_BIND_SUBSCRIPTION,
	    NULL, NULL, vayu_cell_wds_get_subscription_resp_ei, &sub, &sub.result);
	say("vayu-cell-data: diag %s: WDS client subscription %s%u (0 default, 1 primary, 2 secondary)\n", when,
	    ret == 0 && sub.subscription_valid ? "" : "unknown ", (unsigned)sub.subscription);
	if (!data.dsd_there) {
		say("vayu-cell-data: diag %s: DSD service 0x2a not seen\n", when);
		return;
	}
	memset(&dsd, 0, sizeof(dsd));
	ret = data_request("DSD Get System Status", data.dsd_node, data.dsd_port, VAYU_CELL_DSD_GET_SYSTEM_STATUS,
	    NULL, NULL, vayu_cell_dsd_status_resp_ei, &dsd, &dsd.result);
	systems[0] = 0;
	for (uint32_t i = 0; ret == 0 && dsd.systems_valid && i < dsd.systems_len && i < VAYU_CELL_DSD_SYSTEMS_MAX; i++) {
		used += (size_t)snprintf(systems + used, used < sizeof(systems) ? sizeof(systems) - used : 0,
		    "%s{tech %u rat %u so %#llx}", i ? " " : "", dsd.systems[i].technology, dsd.systems[i].rat,
		    (unsigned long long)dsd.systems[i].so_mask);
		if (used >= sizeof(systems)) break;
	}
	say("vayu-cell-data: diag %s: DSD available systems %s%u%s%s (tech 0 3GPP; rat 1 WCDMA, 2 GERAN, 3 LTE, 6 5G)\n",
	    when, ret == 0 && dsd.systems_valid ? "" : "unknown ", (unsigned)dsd.systems_len, systems[0] ? ": " : "",
	    systems);
}

/* vayu_cell_wds_sub=N: bind this WDS client to subscription N once, before the first Start. */
static void
data_wds_bind_subscription(void)
{
	struct vayu_cell_wds_subscription_req req = { .subscription = data.wds_sub };
	struct vayu_cell_result_resp resp = { 0 };
	int ret;

	data.sub_done = true;
	ret = data_request("WDS Bind Subscription", data.wds_node, data.wds_port, VAYU_CELL_WDS_BIND_SUBSCRIPTION,
	    vayu_cell_wds_subscription_req_ei, &req, vayu_cell_result_resp_ei, &resp, &resp.result);
	say("vayu-cell-data: vayu_cell_wds_sub=%u: WDS Bind Subscription %s (result %u error %u)\n", data.wds_sub,
	    ret == 0 ? "OK" : "REFUSED", (unsigned)resp.result.result, (unsigned)resp.result.error);
	data_stage_line(6, "WDS BIND SUBSCRIPTION", ret == 0 ? "OK" : "ERROR", "subscription %u, error %u",
	    data.wds_sub, (unsigned)resp.result.error);
}

/* vayu_cell_start_adopt=1: after a Start without a reply, is there a session? true: connected. */
static bool
data_adopt_poll(void)
{
	for (uint32_t round = 0; round < 12 && !data.stop; round++) {
		struct vayu_cell_wds_pss_resp resp = { 0 };
		int ret = data_request("WDS Get Packet Service Status", data.wds_node, data.wds_port,
		    VAYU_CELL_WDS_GET_PACKET_SERVICE_STATUS, NULL, NULL, vayu_cell_wds_pss_resp_ei, &resp, &resp.result);

		say("vayu-cell-data: adopt: packet service status %s%u (1 disconnected, 2 connected, 4 authenticating), "
		    "round %u/12\n", ret == 0 && resp.status_valid ? "" : "no answer ", (unsigned)resp.status, round + 1);
		if (ret == 0 && resp.status_valid && resp.status == VAYU_CELL_WDS_CONNECTED) return true;
		if (ret == 0 && resp.status_valid && resp.status == 1 && round >= 2) return false;
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
	return false;
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
	if (data.wait_modem_init && !vayu_ipa_modem_init_complete()) {
		static uint64_t last;

		if (now_ms() - last > 30000) {
			last = now_ms();
			say("vayu-cell-data: WDS Start held: the modem's IPA init is still pending (no 0x35 yet; "
			    "vayu_cell_wait_modem_init=1)\n");
		}
		data.next_ms = now_ms() + 1000;
		return;
	}
	if ((data.profile_log || data.start_profile == 255) && !data.profile_done) {
		data_profile_read();
	}
	if (data.wds_sub != 0 && !data.sub_done) {
		data_wds_bind_subscription();
	}
	if (data.stage_attempts == 0) {
		data_wds_diag("before the first Start");
	}
	if (data.start_profile != 0) {
		uint8_t index = data.start_profile == 255 ? data.profile_index : (uint8_t)data.start_profile;

		if (index != 0) {
			req.apn_valid = 0;      /* the profile's own APN */
			req.profile_valid = 1;
			req.profile = index;
		}
	}
	/* 0041 stage 4: the old path requires the whole chain. vayu_cell_lte=1 with
	 * vayu_cell_lte_control=1 (the default) requires only the IPA QMI handshake: WDS gives the
	 * address without GSI, and the modem's own 0x24 counters say whether a packet crossed it. */
	{
		bool packet_path = data_packet_path_ready();
		bool control_mode = data.lte && data.control_only;

		if (!vayu_ipa_ready() || (!packet_path && !control_mode)) {
			static uint64_t last_blocked;
			if (now_ms() - last_blocked > 5000) {
				last_blocked = now_ms();
				data_stage_line(7, "WDS START", "ERROR",
				    "blocked: IPA handshake %s, GSI %s, %s %s (%s)",
				    vayu_ipa_ready() ? "ready" : "NOT READY",
				    vayu_ipa_data_ready() ? "ready" : "NOT READY",
				    vayu_cell_net_name(),
				    vayu_cell_net_ready() ? "ready" : "NOT READY (need vayu_wds=1)",
				    control_mode ? "control-only also needs the handshake" : "full path");
			}
			return;
		}
	}
	{
		uint32_t qmi_stage = vayu_ipa_qmi_stage();
		uint32_t needed = data.lte ? 2 : 4;

		if (qmi_stage < needed) {
			static uint64_t last_refused;
			if (now_ms() - last_refused > 5000) {
				last_refused = now_ms();
				data_stage_line(7, "WDS START", "ERROR",
				    "vayu_ipa_qmi=%u: stage %u is not enabled; no data session", qmi_stage,
				    needed);
			}
			return;
		}
	}
	data.stage_attempts++;
	if (hakios_ipv4_only != 0) {
		say("vayu-cell-data: vayu_ipv4_only=1: WDS Start asks for PDP IPv4 only, APN %s; no IPv6 "
		    "address is requested and pdp_ip0 must show no inet6\n",
		    data.apn[0] != 0 ? data.apn : "(SIM default profile)");
	}
	if (req.profile_valid) {
		say("vayu-cell-data: WDS Start Network Interface: IPv4, 3GPP profile #%u (vayu_cell_start_profile=%u, "
		    "no APN TLV), mode %s, attempt %u\n", (unsigned)req.profile, data.start_profile,
		    data_packet_path_ready() ? "full path" : "control only", data.stage_attempts);
	} else {
		say("vayu-cell-data: WDS Start Network Interface: IPv4, APN %s (%s), mode %s\n",
		    data.apn[0] != 0 ? data.apn : "(SIM default profile)",
		    data.apn[0] != 0 ? "boot arg" : "no APN given",
		    data_packet_path_ready() ? "full path" : "control only");
	}
	ret = data_request_ms("WDS Start Network Interface", node, port,
	    VAYU_CELL_WDS_START_NETWORK_INTERFACE, vayu_cell_wds_start_req_ei, &req,
	    vayu_cell_wds_start_resp_ei, &resp, &resp.result, data.start_timeout_ms);
	if (ret == -ETIMEDOUT && data.adopt) {
		vayu_ipa_modem_snapshot("after a WDS Start timeout");
		if (data_adopt_poll()) {
			/* The session exists; a second Start on this client is NO_EFFECT and carries its handle. */
			memset(&resp, 0, sizeof(resp));
			ret = data_request_ms("WDS Start Network Interface (handle of the connected session)", node, port,
			    VAYU_CELL_WDS_START_NETWORK_INTERFACE, vayu_cell_wds_start_req_ei, &req,
			    vayu_cell_wds_start_resp_ei, &resp, &resp.result, data.start_timeout_ms);
			say("vayu-cell-data: adopt: CONNECTED; handle %s%#x (ret %d, error %u)\n",
			    resp.handle_valid ? "" : "absent ", resp.handle, ret, (unsigned)resp.result.error);
			if (resp.handle_valid && resp.handle != 0) {
				data.wds_handle = resp.handle;
				data.wds_handle_node = node;
				data.wds_handle_port = port;
				data.wds_handle_generation = generation;
				data.stop_attempts = 0;
				data_stage_line(7, "WDS START", "OK", "adopted after a timeout, handle %#x", data.wds_handle);
				data_advance(DATA_SETTINGS);
				return;
			}
			say("vayu-cell-data: adopt: connected but no handle; ending as before (no Stop is possible)\n");
		} else {
			vayu_ipa_modem_snapshot("after the adopt polls");
		}
		ret = -ETIMEDOUT;
		memset(&resp, 0, sizeof(resp));
	}
	if (ret != 0 && (resp.call_end_reason_valid || resp.verbose_valid)) {
		const char *cause = resp.verbose_valid && resp.verbose.type == 6 ?
		    data_3gpp_cause_name(resp.verbose.reason) : "";

		say("vayu-cell-data: WDS Start refused: call end reason %s%u, verbose %s (%u) reason %u%s%s%s; "
		    "handle TLV %s%#x\n",
		    resp.call_end_reason_valid ? "" : "absent ", (unsigned)resp.call_end_reason,
		    resp.verbose_valid ? data_verbose_type_name(resp.verbose.type) : "absent",
		    (unsigned)resp.verbose.type, (unsigned)resp.verbose.reason,
		    cause[0] != 0 ? " (" : "", cause, cause[0] != 0 ? ")" : "",
		    resp.handle_valid ? "" : "absent ", resp.handle);
		data_stage_line(7, "WDS START", "ERROR", "error %u, call end %u, verbose %s/%u%s%s",
		    (unsigned)resp.result.error, (unsigned)resp.call_end_reason,
		    resp.verbose_valid ? data_verbose_type_name(resp.verbose.type) : "-", (unsigned)resp.verbose.reason,
		    cause[0] != 0 ? " " : "", cause);
		data_wds_diag("after a refused Start");
	}
	if (data.start_retry && ret == -EIO && resp.handle_valid && resp.handle == 0) {
		/* An error answer with a zero handle: the modem refused the call, there is no session to
		 * stop, and another Start is not a duplicate. */
		say("vayu-cell-data: Start refused with handle 0: no session exists; retried (vayu_cell_start_retry=1)\n");
		resp.handle_valid = 0;
	}
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
		data_stage_line(7, "WDS START", "TIMEOUT", "no reply in %u ms; no duplicate Start", data.start_timeout_ms);
		data.session_uncertain = true;
		data.stage = DATA_FAILED;
		data.cleanup = true;
		data.cleanup_seq++;
		return;
	}
	if (ret == 0 && resp.handle_valid) {
		data.wds_handle = resp.handle;
		say("vayu-cell-data: WDS data session accepted, handle %#x; asking for settings\n", data.wds_handle);
		if (data.wds) {
			say("vayu-wds: start ok handle %#x (IPv4, APN %s)\n", data.wds_handle,
			    data.apn[0] != 0 ? data.apn : "SIM default");
		}
		data_stage_line(7, "WDS START", "OK", "IPv4 %s, handle %#x",
		    data.apn[0] != 0 ? "boot-arg APN" : "SIM default profile", data.wds_handle);
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
		data_retry("WDS Start Network Interface", resp.result.error);
	}
}

static void
ip4_string(char *to, size_t size, uint32_t address)
{
	snprintf(to, size, "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xff, (address >> 8) & 0xff,
	    address & 0xff);
}

/* Logs only: the assignment allows the last octet in a log, nothing more. The full address stays
 * in cellular_ip and in the WDS/ifnet state, where the interface needs it. */
static void
ip4_masked(char *to, size_t size, uint32_t address)
{
	snprintf(to, size, "x.x.x.%u", address & 0xff);
}

/* The modem's own counters (WDS 0x24). They say whether a packet crossed the modem, which the
 * GSI ring alone cannot prove. Logged only when a counter changed, or when force is set. */
static void
data_stats(const char *when, bool force)
{
	struct vayu_cell_wds_stats_resp resp = { 0 };
	int ret;

	if (data.wds_handle == 0 || !data.wds_there || data.cleanup) {
		return;
	}
	ret = data_request_ms("WDS Get Packet Statistics", data.wds_node, data.wds_port,
	    VAYU_CELL_WDS_GET_PACKET_STATISTICS, NULL, NULL, vayu_cell_wds_stats_resp_ei, &resp,
	    &resp.result, 2000);
	if (ret != 0) {
		return;
	}
	if (!force && resp.tx_packets == data.stats_last_tx && resp.rx_packets == data.stats_last_rx) {
		return;
	}
	data.stats_last_tx = resp.tx_packets;
	data.stats_last_rx = resp.rx_packets;
	say("vayu-cell-data: WDS statistics %s: tx %u pkts/%llu B, rx %u pkts/%llu B, "
	    "errors %u/%u, dropped %u/%u\n", when, (unsigned)resp.tx_packets,
	    (unsigned long long)resp.tx_bytes, (unsigned)resp.rx_packets,
	    (unsigned long long)resp.rx_bytes, (unsigned)resp.tx_errors, (unsigned)resp.rx_errors,
	    (unsigned)resp.tx_dropped, (unsigned)resp.rx_dropped);
}

static bool
data_packet_path_ready(void)
{
	return vayu_ipa_data_ready() && vayu_cell_net_ready();
}

static void
data_settings(void)
{
	struct vayu_cell_wds_settings_req req = { .requested_settings = VAYU_CELL_SETTINGS_MASK };
	struct vayu_cell_wds_settings_resp resp = { 0 };
	char ip[20], gw[20], mask[20], dns1[20], dns2[20];
	char ip_log[20], gw_log[20], dns1_log[20], dns2_log[20];
	bool packet_path;
	int ret;

	data.stage_attempts++;
	ret = data_request("WDS Get Current Settings", data.wds_node, data.wds_port,
	    VAYU_CELL_WDS_GET_CURRENT_SETTINGS, vayu_cell_wds_settings_req_ei, &req,
	    vayu_cell_wds_settings_resp_ei, &resp, &resp.result);
	if (ret != 0) {
		data_stage_line(8, "WDS GET SETTINGS", ret == -ETIMEDOUT ? "TIMEOUT" : "ERROR", "result %u error %u",
		    (unsigned)resp.result.result, (unsigned)resp.result.error);
		data_retry("WDS Get Current Settings", resp.result.error);
		return;
	}
	ip4_string(ip, sizeof(ip), resp.address_valid ? resp.address : 0);
	ip4_string(gw, sizeof(gw), resp.gateway_valid ? resp.gateway : 0);
	ip4_string(mask, sizeof(mask), resp.mask_valid ? resp.mask : 0);
	ip4_string(dns1, sizeof(dns1), resp.dns1_valid ? resp.dns1 : 0);
	ip4_string(dns2, sizeof(dns2), resp.dns2_valid ? resp.dns2 : 0);
	ip4_masked(ip_log, sizeof(ip_log), resp.address_valid ? resp.address : 0);
	ip4_masked(gw_log, sizeof(gw_log), resp.gateway_valid ? resp.gateway : 0);
	ip4_masked(dns1_log, sizeof(dns1_log), resp.dns1_valid ? resp.dns1 : 0);
	ip4_masked(dns2_log, sizeof(dns2_log), resp.dns2_valid ? resp.dns2 : 0);
	packet_path = data_packet_path_ready();
	say("vayu-cell-data: WDS runtime settings (Get Current Settings 0x2d): address %s gateway %s "
	    "netmask %s DNS %s %s MTU %u\n", ip_log, gw_log, mask, dns1_log, dns2_log,
	    resp.mtu_valid ? resp.mtu : 0);
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
	data.stats_ms = now_ms();
	if (packet_path) {
		vayu_cell_net_session(true, resp.address, resp.mask_valid ? resp.mask : 0,
		    resp.gateway_valid ? resp.gateway : 0, resp.dns1_valid ? resp.dns1 : 0,
		    resp.dns2_valid ? resp.dns2 : 0, resp.mtu_valid ? resp.mtu : 0);
		data.net_session = true;
	}
	data_stage_line(8, "WDS GET SETTINGS", "OK", "address %s gateway %s mask %s DNS %s %s MTU %u",
	    ip_log, gw_log, mask, dns1_log, dns2_log, resp.mtu_valid ? resp.mtu : 0);
	if (data.wds) {
		say("vayu-wds: ipv4 addr %s gw %s mask %s dns %s %s mtu %u\n", ip_log, gw_log, mask,
		    dns1_log, dns2_log, resp.mtu_valid ? resp.mtu : 0);
	}
	if (packet_path) {
		data_stage_line(9, vayu_cell_net_name(), "OK",
		    "configuration queued: address %s peer %s", ip_log, gw_log);
		if (data.wds) {
			say("vayu-wds: %s up requested: %s/%s peer %s mtu %u (IPv4 only)\n",
			    vayu_cell_net_name(), ip_log, mask, gw_log, resp.mtu_valid ? resp.mtu : 0);
		}
		say("vayu-cell-data: CONTROL PLANE UP: IPv4 session/settings accepted; %s configuration "
		    "queued. IPA/GSI packet counters, not this line, prove transport\n", vayu_cell_net_name());
	} else {
		data_stage_line(9, vayu_cell_net_name(), "SKIPPED",
		    "control only: GSI %s, %s %s; address held, no packets",
		    vayu_ipa_data_ready() ? "ready" : "NOT READY", vayu_cell_net_name(),
		    vayu_cell_net_ready() ? "ready" : "NOT READY (need vayu_wds=1)");
		if (data.wds) {
			say("vayu-wds: address held; %s stays down until GSI is ready (vayu_ipa_data=1) "
			    "and the interface module is up (vayu_wds=1); no packet claim\n",
			    vayu_cell_net_name());
		}
		say("vayu-cell-data: CONTROL PLANE UP (control only): the modem accepted the IPv4 session "
		    "and returned an address; the packet path is not up, so no ping and no packet claim. "
		    "Enable vayu_ipa=1 vayu_ipa_data=1 vayu_wds=1 (or vayu_cell_net=1) for the data path\n");
	}
	data_stats("after settings", true);
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
	if (!data.registered || now_ms() < data.next_ms) {
		return;
	}
	if (data.stage == DATA_UP) {
		/* A control-only session has no en1, but the modem's counters still tell whether
		 * anything crossed it (and later prove the ping). */
		if (now_ms() - data.stats_ms >= 5000) {
			data.stats_ms = now_ms();
			data_stats("up", false);
		}
		return;
	}
	if (data.stage == DATA_FAILED || data.stage == DATA_PORT) {
		return;
	}
	if (!vayu_ipa_ready() || (!data_packet_path_ready() && !(data.lte && data.control_only))) {
		static uint64_t last_gate_log;
		if (now_ms() - last_gate_log > 5000) {
			last_gate_log = now_ms();
			say("vayu-cell-data: WDS/DPM blocked: IPA handshake %s, GSI/tables %s, %s %s; mode %s; "
			    "no Start is sent\n",
			    vayu_ipa_ready() ? "ready" : "NOT READY", vayu_ipa_data_ready() ? "ready" : "NOT READY",
			    vayu_cell_net_name(),
			    vayu_cell_net_ready() ? "ready" : "NOT READY (need vayu_wds=1)",
			    data.lte && data.control_only ? "control-only (waits for the IPA handshake only)" :
			    "full path (needs GSI and the interface)");
			data_stage_line(1, "IPA QMI READY", vayu_ipa_ready() ? "OK" : "TIMEOUT",
			    "vayu_ipa_qmi=%u; handshake %s", vayu_ipa_qmi_stage(),
			    vayu_ipa_ready() ? "done" : "not complete");
			data_stage_line(2, "GSI CHANNELS", vayu_ipa_data_ready() ? "OK" : "ERROR",
			    "vayu_ipa_data=%s; %s", vayu_ipa_data_ready() ? "1" : "off/not ready",
			    vayu_ipa_data_ready() ? "4 channels, endpoints and tables ready" : "data path not ready");
		}
		return;
	}
	{
		static bool announced;

		if (!announced) {
			announced = true;
			data_stage_line(1, "IPA QMI READY", "OK",
			    "vayu_ipa_qmi=%u handshake complete; DPM/WDA/WDS may start", vayu_ipa_qmi_stage());
		}
	}
	if (!data.dpm_there || !data.wda_there || !data.wds_there) {
		say("vayu-cell-data: registered, waiting for DPM %s WDA %s WDS %s\n",
		    data.dpm_there ? "there" : "MISSING", data.wda_there ? "there" : "MISSING",
		    data.wds_there ? "there" : "MISSING");
		data.next_ms = now_ms() + 5000;
		return;
	}
	switch (data.stage) {
	case DATA_DPM:
		if (data.dpm_skip) {
			say("vayu-cell-data: DPM Open Port not sent (vayu_cell_dpm=0)\n");
			data_stage_line(3, "DPM OPEN PORT", "SKIPPED", "vayu_cell_dpm=0: WDA, Bind Mux and Start only");
			data_advance(DATA_WDA);
		} else {
			data_dpm();
		}
		break;
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
	return data.started && (data.cleanup || data.restart ||
	    (data.stage != DATA_UP && data.stage != DATA_FAILED && data.stage != DATA_PORT));
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
	if (data.wds_diag && qmi_add_lookup(&data.qmi, VAYU_CELL_DSD_SERVICE, 1, 0) < 0) {
		say("vayu-cell-data: qmi_add_lookup(DSD) failed; the diag reads WDS only\n");
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
		/* Control-only needs the handshake, not GSI: the address comes from WDS. A full-path
		 * session dies if GSI or the interface goes away under it. */
		if ((data.stage == DATA_START || data.stage == DATA_SETTINGS) &&
		    (!vayu_ipa_ready() || (!vayu_ipa_data_ready() && !(data.lte && data.control_only)))) {
			say("vayu-cell-data: IPA readiness was lost during Start/Settings; stopping the client\n");
			data_stage_line(7, "WDS START", "ERROR", "IPA readiness lost during Start/Settings");
			data.stop = true;
			data.cleanup = true;
			data.cleanup_seq++;
		}
		/* vayu_cell_data_watch=1: the same once the session is UP. Without it an UP session whose
		 * IPA handshake or GSI went away (vayu_ipa_qmi.c: a modem filter request not served clears
		 * data_ready) keeps pdp_ip0/en1 configured, and its route, over a path that carries nothing. */
		if (data.watch_up && data.stage == DATA_UP && !data.cleanup &&
		    (!vayu_ipa_ready() || (data.net_session && !vayu_ipa_data_ready()))) {
			say("vayu-cell-data: IPA readiness was lost while UP (handshake %s, GSI %s); ending the session\n",
			    vayu_ipa_ready() ? "ready" : "LOST", vayu_ipa_data_ready() ? "ready" : "LOST");
			data_stage_line(10, "DATA UP", "ERROR", "IPA readiness lost; interface and WDS session taken down");
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
	uint32_t enabled = 0, lte = 0, wds = 0, wait_s = 60;
	bool want_data, want_lte, want_wds;

	want_data = PE_parse_boot_argn("vayu_cell_data", &enabled, sizeof(enabled)) && enabled == 1;
	want_lte = PE_parse_boot_argn("vayu_cell_lte", &lte, sizeof(lte)) && lte == 1;
	want_wds = PE_parse_boot_argn("vayu_wds", &wds, sizeof(wds)) && wds == 1;
	if (wds != 0 && !want_wds) {
		say("vayu-wds: vayu_wds=%u is refused: only 1 starts the WDS client and pdp_ip0\n", wds);
		return;
	}
	if (!want_data && !want_lte && !want_wds) {
		if (enabled != 0 || lte != 0) {
			say("vayu-cell-data: refused: vayu_cell_data=%u vayu_cell_lte=%u; only 1 enables a data session\n",
			    enabled, lte);
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
	data.lte = want_lte || want_wds;
	data.wds = want_wds;
	{
		uint32_t watch = 0;

		PE_parse_boot_argn("vayu_cell_data_watch", &watch, sizeof(watch));
		data.watch_up = watch == 1;
		watch = 0;
		PE_parse_boot_argn("vayu_cell_data_port", &watch, sizeof(watch));
		data.port_only = watch == 1;
	}
	if (data.lte) {
		uint32_t control = 1;

		/* Default: WDS control plane is allowed after the handshake without GSI. The packet
		 * path stays disengaged until vayu_ipa_data=1 and the interface module are also ready. */
		PE_parse_boot_argn(want_wds ? "vayu_wds_control" : "vayu_cell_lte_control", &control,
		    sizeof(control));
		data.control_only = control == 1;
	}
	/* The APN is the operator's, so it is the boot arguments' (vayu_cell_apn=); without one WDS Start
	 * carries no APN and the modem uses the SIM's default profile. */
	if (!PE_parse_boot_argn("vayu_cell_apn", data.apn, sizeof(data.apn))) {
		data.apn[0] = 0;
	}
	data.apn[sizeof(data.apn) - 1] = 0;
	{
		uint32_t v = 0;

		data.start_retry = PE_parse_boot_argn("vayu_cell_start_retry", &v, sizeof(v)) && v == 1;
		v = 0;
		data.profile_log = PE_parse_boot_argn("vayu_cell_profile_log", &v, sizeof(v)) && v == 1;
		v = 0;
		if (PE_parse_boot_argn("vayu_cell_start_profile", &v, sizeof(v)) && ((v >= 1 && v <= 24) || v == 255)) {
			data.start_profile = v;
		}
		data.start_timeout_ms = DATA_START_REPLY_MS;
		if (PE_parse_boot_argn("vayu_cell_start_timeout_ms", &v, sizeof(v)) && v >= 1000 && v <= 180000) {
			data.start_timeout_ms = v;
		}
		v = 0;
		data.adopt = PE_parse_boot_argn("vayu_cell_start_adopt", &v, sizeof(v)) && v == 1;
		v = 0;
		data.sub_first = PE_parse_boot_argn("vayu_cell_wds_sub_first", &v, sizeof(v)) && (v == 1 || v == 2) ? v : 0;
		v = 0;
		data.wait_modem_init = PE_parse_boot_argn("vayu_cell_wait_modem_init", &v, sizeof(v)) && v == 1;
		if (data.wait_modem_init) say("vayu-cell-data: a11: WDS Start waits for the modem's 0x35\n");
		data.wda_dl_datagrams = 16;
		data.wda_dl_size = 0x3800;
		if (PE_parse_boot_argn("vayu_cell_wda_dl_datagrams", &v, sizeof(v)) && v >= 1 && v <= 64) {
			data.wda_dl_datagrams = v;
		}
		if (PE_parse_boot_argn("vayu_cell_wda_dl_size", &v, sizeof(v)) && v >= 1500 && v <= 65536) {
			data.wda_dl_size = v;
		}
		v = 1;
		data.dpm_skip = PE_parse_boot_argn("vayu_cell_dpm", &v, sizeof(v)) && v == 0;
		data.wda_ul_agg = VAYU_CELL_QMAP;
		if (PE_parse_boot_argn("vayu_cell_wda_ul_agg", &v, sizeof(v)) && v == 0) {
			data.wda_ul_agg = 0;
		}
		if (data.sub_first || data.dpm_skip || data.wda_ul_agg != VAYU_CELL_QMAP || data.wda_dl_datagrams != 16 ||
		    data.wda_dl_size != 0x3800) {
			say("vayu-cell-data: a11: wds_sub_first %u, dpm %s, WDA UL aggregation %u, DL %u x %#x\n", data.sub_first,
			    data.dpm_skip ? "skipped" : "opened", data.wda_ul_agg, data.wda_dl_datagrams, data.wda_dl_size);
		}
		if (data.adopt || data.start_timeout_ms != DATA_START_REPLY_MS) {
			say("vayu-cell-data: a11: start timeout %u ms, adopt %u\n", data.start_timeout_ms, data.adopt);
		}
		v = 0;
		data.wds_diag = PE_parse_boot_argn("vayu_cell_wds_diag", &v, sizeof(v)) && v == 1;
		v = 0;
		if (PE_parse_boot_argn("vayu_cell_wds_sub", &v, sizeof(v)) && v >= 1 && v <= 2) {
			data.wds_sub = v;
		}
		if (data.wds_diag || data.wds_sub != 0) {
			say("vayu-cell-data: a11: wds_diag %u, wds_sub %u\n", data.wds_diag, data.wds_sub);
		}
		if (data.start_retry || data.profile_log || data.start_profile != 0) {
			say("vayu-cell-data: a11: start_retry %u, profile_log %u, start_profile %u%s\n", data.start_retry,
			    data.profile_log, data.start_profile, data.start_profile == 255 ? " (the modem's default)" : "");
		}
	}
	if (want_wds) {
		say("vayu-wds: vayu_wds=1: APN %s, control-only %u, pdp_ip0; WDS Start requires "
		    "vayu_ipa_ready(); packets and pdp_ip0 need vayu_ipa_data=1 and GSI\n",
		    data.apn[0] != 0 ? data.apn : "(SIM default profile)", data.control_only ? 1 : 0);
	} else if (want_lte) {
		say("vayu-cell-data: vayu_cell_lte=1: APN %s, control-only %u; WDS Start still requires "
		    "vayu_ipa_ready() (firmware + INIT_MODEM_DRIVER + INIT_COMPLETE)\n",
		    data.apn[0] != 0 ? data.apn : "(SIM default profile)", data.control_only ? 1 : 0);
		if (hakios_ipv4_only != 0) {
			say("vayu-cell-data: vayu_ipv4_only=1: APN %s, WDS PDP IPv4 only; en1 and pdp_ip0 must "
			    "show no inet6\n", data.apn[0] != 0 ? data.apn : "(SIM default profile)");
		}
	} else {
		say("vayu-cell-data: vayu_cell_data=1 (legacy full-path gate): DPM/WDA/WDS only after IPA "
		    "and GSI are ready\n");
	}
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
