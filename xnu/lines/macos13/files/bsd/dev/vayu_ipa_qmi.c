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
 * vayu_ipa_try=BITS (absent: 0, the old behaviour, nothing new happens) adds experiments for a modem
 * that never answers INIT_MODEM_DRIVER:
 *   bit 0 (1)   send INDICATION_REGISTER 0x20 to the modem's IPA service first: a message with a
 *               trivial handler whose answer proves the service dispatches (2 s wait, "S2 VERIFY").
 *   bit 1 (2)   INIT carries is_ssr_bootup=1 (TLV 0x18). Stock sends 0/absent on a cold boot and 1
 *               after an SSR; on the SSR path the modem need not wait for the AP's uC before it
 *               answers, so this is the cheapest test of "the modem's handler waits for the uC".
 *   bit 2 (4)   INIT carries platform_type=2 instead of 3 (the generated enum's values are not in
 *               any source on the stand; both values are logged so the answer, if any, decides).
 *   bit 3 (8)   also send INSTALL_FILTER_RULE 0x23 with no rules: the message the AP's data path
 *               always sends and the modem's service always serves, so any answer proves dispatch.
 *   bit 4 (16)  send the same INIT_MODEM_DRIVER once more, but from the port we published as
 *               0x31/1 (vayu_qrtr_ops.send(host_port, ...)) instead of the client socket's own
 *               port: the address the modem's client already knows as the AP's IPA service. If
 *               the silence is about *who* asks, this answers where the client-port one does not.
 * Bits combine. Every line printed for them begins "S2 VERIFY", "S2 HOST 0x21" or "S2 SUMMARY",
 * or names the variant in the attempt line, so a run without the argument is byte-for-byte the
 * run of k16.
 *
 * k18 keeps what the probes found in globals and repeats it: one "S2 SUMMARY" line after the
 * attempts, after every late INIT, and every two minutes while the thread idles. r20 (k17,
 * vayu_ipa_try=9) lost the whole handshake block: the kernel's message buffer is 1 MB and the
 * capture kept only its first 512 KB and last 6.5 MB, so the lines were written and never read.
 * A summary repeated later cannot be lost that way. With no vayu_ipa_try, none of this prints.
 *
 * k19 continues where the k18 run (VAYU_IPA_TRY=bits 0,3,4) left the question: the modem's IPA
 * service answered 0x20 (result 1 error 3 INTERNAL), 0x23 (1, 94 NOT_SUPPORTED) and even an
 * unknown message id in 1 ms, but never 0x21, not from the client port and not from the published
 * host port; and its own SMP2P "ipa" entry never appeared. Five more opt-in bits ask, in this
 * order of decisiveness:
 *   bit 5 (0x20)  after the attempts: 0x20 and 0x7fff again. Alive: only the 0x21 handler is
 *                 pending. Silent: the 0x21 request wedged the modem's IPA task.
 *   bit 6 (0x40)  a 0x21 with no TLV at all (7-byte header).
 *   bit 7 (0x80)  a 0x21 with only platform_type (TLV 0x10).
 *   bit 8 (0x100) survey the modem's SMP2P items for its "ipa" (clock query) entry. Read only.
 *   bit 9 (0x200) clear our own "ipa" entry to 0 and set it back to 3 with an APCS ring: a
 *                 notification the modem sees after it booted, not the one it missed before MPSS.
 *                 The only write, and only when asked for; the survey does not do it.
 *   bit 10 (0x400) read the modem's crash-reason item (SMEM 421) before, between and after the
 *                 attempts: its text is the modem's own last words (the k18 run's census found an
 *                 "ipa_qmi_server.c:559: IPA Assert" there that was not at boot). Read only.
 *
 * vayu_ipa_seq=BITS (absent: 0, the old behaviour, nothing new happens) is the clean 0x21 sequence
 * this file was asked for after the k18 run: one client connection, no second source port, and the
 * two things the modem's IPA server needs before it can answer INIT_MODEM_DRIVER.
 *   bit 0 (1)  NEW_LOOKUP: before the QMI client's first request, send the QRTR NEW_LOOKUP control
 *              packet the client side of Linux sends through the name service, naming service 0x31
 *              and instance 2 and this client's own port (vayu_pas.c: vayu_qrtr_lookup_announce);
 *              send it a second time once the service has been found, so a name service that does
 *              not remember an early lookup still sees one naming the live service.
 *              The modem's QMI framework calls the IPA service's connect callback for a client the
 *              name service has seen; its connect callback is what stores ipa_qmi_server.client_handle,
 *              the value the 0x21 handler checks (firmware strings: "ipa_qmi_server.c:559: IPA Assert:
 *              connection_handle == ipa_qmi_server.client_handle failed !\"Invalid Connection Handle\"").
 *   bit 1 (2)  SMP2P CLOCK: survey the modem's "ipa" entry; if the modem has one, answer it with a
 *              fresh edge on our own entry (0 -> 3, an APCS ring at each step) before 0x21. The k18
 *              AP wrote 3 once before MPSS existed, so the modem may never have seen the event.
 *   bit 2 (4)  NO HOST PORT: a marker only. Any nonzero vayu_ipa_seq refuses vayu_ipa_try
 *              bit 4 (0x10) and never sends 0x21 from the published 0x31/1 port. The published
 *              port is a *different* connection handle from the client socket; the k18 host-port
 *              0x21 is the request that preceded the modem's assert by 60 s. Set the bit if the
 *              SUMMARY should name it (+no-host); the refusal itself does not depend on it.
 * The 0x20/0x23 probes are not part of the sequence: the modem's IPA server has no
 * INDICATION_REGISTER handler (only its client side sends that message) and answers 0x23 with
 * NOT_SUPPORTED ("Use QMI_IPA_INSTALL_FILTER_RULE_EX_REQ_V01 instead"). They are still available
 * through vayu_ipa_try, but a sequence run leaves them off.
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
static uint32_t qmi_regwait_ms = 5000, qmi_attempts = 3;
static bool qmi_trace;
/* vayu_ipa_try: opt-in experiments for a modem that does not answer INIT_MODEM_DRIVER (see the top
 * of this file). 0 or absent: nothing below changes anything. */
#define VAYU_IPA_TRY_HELLO      (1u << 0)
#define VAYU_IPA_TRY_SSR        (1u << 1)
#define VAYU_IPA_TRY_PLATFORM2  (1u << 2)
#define VAYU_IPA_TRY_FILTER     (1u << 3)
#define VAYU_IPA_TRY_HOST_INIT  (1u << 4)
/* k19's extra bits (all off by default, like the five above):
 *   bit 5 (32)   POST:  after the attempts, ask the service the two questions it answered before
 *                       (0x20 and an unknown id). Both answered: the task lives and only the 0x21
 *                       handler is pending. Both silent: our 0x21 wedged the modem's IPA task.
 *   bit 6 (64)   EMPTY: the same INIT_MODEM_DRIVER as a 7-byte header, no TLV at all. An answer
 *                       (any result/error) separates "the handler blocks on its body" from
 *                       "the handler blocks always"; silence here too settles it the other way.
 *   bit 7 (128)  MIN:   the same INIT with only platform_type (TLV 0x10, 4 bytes).
 *   bit 8 (256)  SMP2P: survey the modem's SMP2P items for an "ipa" entry (its clock query). Read
 *                       only: it says whether the modem ever asked the AP for the IPA clock.
 *   bit 9 (512)  DANCE:  clear our own "ipa" entry to 0 and set it back to 3 with an APCS ring: a
 *                       notification the modem can see *after* it booted, not one it missed. The
 *                       only write; separate from the survey so a run can have one without the
 *                       other. */
#define VAYU_IPA_TRY_POST       (1u << 5)
#define VAYU_IPA_TRY_EMPTY      (1u << 6)
#define VAYU_IPA_TRY_MIN        (1u << 7)
#define VAYU_IPA_TRY_SMP2P      (1u << 8)
#define VAYU_IPA_TRY_DANCE      (1u << 9)
/* bit 10 (1024) CRASH: read the modem's crash-reason item (SMEM 421) and print its text, before
 * the first request, after every 0x21 attempt and after the late ones. The k18 run's census at
 * shutdown found item 421 in the global partition carrying
 * "ipa_qmi_server.c:559:IPA Assert: connection_handle == ipa_qmi_server.client_handle" although it
 * was absent at boot; this dates that write to the interval between two reads. Read only. */
#define VAYU_IPA_TRY_CRASH      (1u << 10)
/* vayu_ipa_seq: the clean 0x21 sequence (see the top of this file). 0 or absent: nothing below
 * changes anything; the k25 behaviour is kept byte for byte. */
#define VAYU_IPA_SEQ_NEWLOOKUP  (1u << 0)
#define VAYU_IPA_SEQ_CLOCK      (1u << 1)
#define VAYU_IPA_SEQ_NO_HOST    (1u << 2)
static uint32_t qmi_try;
static uint32_t qmi_seq;

/* The last 0x21 answer, so the sequence's SUMMARY can say answered/silent and with what result
 * even when the reply was an error (ipa_init_modem_driver() only keeps init_done on success). */
static volatile bool init_answered;
static volatile uint16_t init_result, init_error;

/* What the probes found, kept so that a later line can repeat it (see the top of this file):
 *   0 not asked, 1 asked but silent, 2 answered success, 3 answered with an error/failed to send. */
static volatile uint32_t probe_020, probe_023;
static uint32_t init_attempts_made;
/* The bit-4 request from the published port: its transaction id, what came back, and when it went. */
static volatile uint16_t host_init_id, host_init_result, host_init_error;
static volatile uint64_t host_init_started;
static volatile uint32_t host_init_state;   /* 0 not asked, 1 waiting, 2 answered ok, 3 answered error, 4 silent, 5 send error */

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

/* The QMI error code in words (bsd/sys/hakios.qmi.h: the values the modem's IPA service uses in
 * its replies). The k18 run answered 0x20 with 3 and 0x23 with 94. */
static const char *
ipa_error_name(uint16_t error)
{
	switch (error) {
	case QMI_ERR_NONE_V01: return "NONE";
	case QMI_ERR_MALFORMED_MSG_V01: return "MALFORMED_MSG";
	case QMI_ERR_NO_MEMORY_V01: return "NO_MEMORY";
	case QMI_ERR_INTERNAL_V01: return "INTERNAL";
	case QMI_ERR_CLIENT_IDS_EXHAUSTED_V01: return "CLIENT_IDS_EXHAUSTED";
	case QMI_ERR_INVALID_ID_V01: return "INVALID_ID";
	case QMI_ERR_ENCODING_V01: return "ENCODING";
	case QMI_ERR_DISABLED_V01: return "DISABLED";
	case QMI_ERR_INCOMPATIBLE_STATE_V01: return "INCOMPATIBLE_STATE";
	case QMI_ERR_NOT_SUPPORTED_V01: return "NOT_SUPPORTED";
	default: return "other";
	}
}

/* Up to 24 bytes of a reply on one line: when a decode fails, the raw answer is the evidence. */
static void
ipa_say_hex(const char *what, const uint8_t *bytes, size_t length)
{
	char hex[3 * 24 + 4];
	size_t at = 0;

	if (length > 24) length = 24;
	for (size_t i = 0; i < length && at + 4 < sizeof(hex); i++) {
		at += (size_t)snprintf(hex + at, sizeof(hex) - at, "%02x ", bytes[i]);
	}
	hex[at] = 0;
	say("vayu-ipa-qmi: %s: raw %s\n", what, hex);
}

/* k19, vayu_ipa_try bit 10: the modem's crash-reason item (SMEM 421, qcom,crash_reason_smem) as
 * text. Empty at boot; whatever the modem writes into it later is its own last words. */
static void
ipa_crash_item(const char *when)
{
	uint32_t size = 0, at = vayu_smem_item(1, 421, &size);
	char text[129];
	uint32_t length;

	if (at == 0) {
		say("vayu-ipa-qmi: S2 CRASH 421 %s: absent\n", when);
		return;
	}
	memset(text, 0, sizeof(text));
	length = size < sizeof(text) - 1 ? size : sizeof(text) - 1;
	if (!vayu_smem_peek(at, text, length)) {
		say("vayu-ipa-qmi: S2 CRASH 421 %s: %u bytes at %#x, unreadable\n", when, size, at);
		return;
	}
	for (uint32_t i = 0; i < length; i++) {
		if (text[i] != 0 && (text[i] < 0x20 || text[i] > 0x7e)) text[i] = '.';
	}
	say("vayu-ipa-qmi: S2 CRASH 421 %s: %u bytes: \"%s\"\n", when, size, text);
}

/* k19 (vayu_ipa_try bits 5..7): one request to the modem's IPA service with its own timeout,
 * decoded by the plain result/error table. The ordinary INIT keeps ipa_init_modem_driver(): this
 * is for questions whose shape is a decision (empty body, one TLV, a re-ask after a timeout). */
static void
ipa_ask(const char *what, uint16_t message, const struct qmi_elem_info *req_ei, const void *req,
    uint64_t timeout_ms)
{
	struct vayu_ipa_reg_resp resp = { 0 };
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = ipa_node, .sq_port = ipa_port };
	struct qmi_txn txn;
	uint8_t raw[96];
	uint64_t started;
	int ret;

	ret = qmi_txn_init(&ipa_qmi, &txn, vayu_ipa_reg_resp_ei, &resp);
	if (ret < 0) {
		say("vayu-ipa-qmi: %s: ERROR no transaction (%d)\n", what, ret);
		return;
	}
	txn.raw = raw;
	txn.raw_size = sizeof(raw);
	txn.raw_length = 0;
	started = now_ms();
	ret = qmi_send_request(&ipa_qmi, &to, &txn, message, 256, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-ipa-qmi: %s: ERROR send %d\n", what, ret);
		return;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, timeout_ms);
	if (ret == -ETIMEDOUT) {
		say("vayu-ipa-qmi: %s: TIMEOUT no reply in %llu ms to node %u port %u\n", what, timeout_ms,
		    ipa_node, ipa_port);
		return;
	}
	if (ret < 0) {
		say("vayu-ipa-qmi: %s: ERROR reply not decoded (%d) after %llu ms\n", what, ret,
		    now_ms() - started);
		if (txn.raw_length > 0) ipa_say_hex(what, txn.raw, txn.raw_length);
		return;
	}
	say("vayu-ipa-qmi: %s: answered after %llu ms (result %u %s, error %u %s, %zu bytes)\n", what,
	    now_ms() - started, (unsigned)resp.result.result,
	    resp.result.result == QMI_RESULT_SUCCESS_V01 ? "SUCCESS" : "FAILURE",
	    (unsigned)resp.result.error, ipa_error_name(resp.result.error), txn.raw_length);
}

/* vayu_ipa_try bit 7: the smallest INIT_MODEM_DRIVER that still says which platform this is. */
struct vayu_ipa_min_req {
	uint8_t platform_type_valid;
	uint32_t platform_type;
};
static const struct qmi_elem_info vayu_ipa_min_req_ei[] = {
	VAYU_IPA_OPT(struct vayu_ipa_min_req, platform_type_valid, 0x10),
	VAYU_IPA_EI(QMI_SIGNED_4_BYTE_ENUM, struct vayu_ipa_min_req, platform_type, 0x10),
	{ .data_type = QMI_EOTI },
};

/* The body of INIT_MODEM_DRIVER, byte-for-byte the vayu IPA 4.1 memory layout from downstream
 * (ipa_utils.c ipa_4_1_mem_part, ipa_qmi_service.c ipa3_qmi_init_modem_send_sync_msg). Shared by
 * the client-socket request and by vayu_ipa_try bit 4's send from the published host port, so the
 * two carry the same bytes and only the source port differs. */
static void
ipa_init_request(struct vayu_ipa_init_req *req)
{
	uint32_t o = vayu_ipa_restricted_bytes();

	memset(req, 0, sizeof(*req));
	req->platform_type_valid = 1;
	req->platform_type = (qmi_try & VAYU_IPA_TRY_PLATFORM2) ? 2 : VAYU_IPA_PLATFORM_MSM_ANDROID;
	req->hdr_tbl_info_valid = 1;
	req->hdr_tbl_info.start = 0x688 + o;
	req->hdr_tbl_info.end = 0x688 + o + 0x140 - 1;
	req->v4_route_tbl_info_valid = 1;
	req->v4_route_tbl_info.start = 0x508 + o;
	req->v4_route_tbl_info.num_indices = 7;
	req->v6_route_tbl_info_valid = 1;
	req->v6_route_tbl_info.start = 0x608 + o;
	req->v6_route_tbl_info.num_indices = 7;
	req->v4_filter_tbl_start_addr_valid = 1;
	req->v4_filter_tbl_start_addr = 0x308 + o;
	req->v6_filter_tbl_start_addr_valid = 1;
	req->v6_filter_tbl_start_addr = 0x408 + o;
	req->modem_mem_info_valid = 1;
	req->modem_mem_info.start = 0x13f0 + o;
	req->modem_mem_info.size = 0x100c;
	req->ctrl_comm_dest_end_pt_valid = 1;
	req->ctrl_comm_dest_end_pt = 11;       /* IPA_CLIENT_APPS_WAN_CONS */
	/* is_ssr_bootup absent: first boot (stock: !ipa3_uc_loaded_check()). vayu_ipa_try bit 1 asks
	 * for the post-SSR shape instead; that is the only difference in the message. */
	if (qmi_try & VAYU_IPA_TRY_SSR) {
		req->is_ssr_bootup_valid = 1;
		req->is_ssr_bootup = 1;
	}
	req->hdr_proc_ctx_tbl_info_valid = 1;
	req->hdr_proc_ctx_tbl_info.start = 0x7d0 + o;
	req->hdr_proc_ctx_tbl_info.end = 0x7d0 + o + 0x200 - 1;
	req->v4_hash_route_tbl_info_valid = 1;
	req->v4_hash_route_tbl_info.start = 0x488 + o;
	req->v4_hash_route_tbl_info.num_indices = 7;
	req->v6_hash_route_tbl_info_valid = 1;
	req->v6_hash_route_tbl_info.start = 0x588 + o;
	req->v6_hash_route_tbl_info.num_indices = 7;
	req->v4_hash_filter_tbl_start_addr_valid = 1;
	req->v4_hash_filter_tbl_start_addr = 0x288 + o;
	req->v6_hash_filter_tbl_start_addr_valid = 1;
	req->v6_hash_filter_tbl_start_addr = 0x388 + o;
	req->hw_stats_quota_base_addr_valid = 1;
	req->hw_stats_quota_base_addr = 0xc30 + o;
	req->hw_stats_quota_size_valid = 1;
	req->hw_stats_quota_size = 0x60;
	req->hw_drop_stats_base_addr_valid = 1;
	req->hw_drop_stats_base_addr = 0x13d0 + o;
	req->hw_drop_stats_table_size_valid = 1;
	req->hw_drop_stats_table_size = 0x20;
}

/* Returns true when the modem answered success. */
static bool
ipa_init_modem_driver(void)
{
	struct vayu_ipa_init_req req;
	struct vayu_ipa_init_resp resp = { 0 };
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = ipa_node, .sq_port = ipa_port };
	struct qmi_txn txn;
	uint32_t o = vayu_ipa_restricted_bytes();
	uint64_t started;
	int ret;

	ipa_init_request(&req);
	init_answered = false;
	init_result = init_error = 0xffff;

	ret = qmi_txn_init(&ipa_qmi, &txn, vayu_ipa_init_resp_ei, &resp);
	if (ret < 0) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR no transaction (%d)\n", ret);
		return false;
	}
	started = now_ms();
	ret = qmi_send_request(&ipa_qmi, &to, &txn, VAYU_IPA_INIT_MODEM_DRIVER, 256, vayu_ipa_init_req_ei, &req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR send %d\n", ret);
		return false;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, qmi_timeout_ms);
	if (ret == -ETIMEDOUT) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: TIMEOUT no reply in %u ms (restricted %#x)\n", qmi_timeout_ms, o);
		return false;
	}
	if (ret < 0) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR reply not decoded (%d)\n", ret);
		return false;
	}
	init_answered = true;
	init_result = resp.result.result;
	init_error = resp.result.error;
	if (resp.result.result != 0 || resp.result.error != 0) {
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: ERROR result %u error %u after %llu ms\n",
		    (unsigned)resp.result.result, (unsigned)resp.result.error, now_ms() - started);
		return false;
	}
	init_done = true;
	say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: OK after %llu ms (restricted %#x; ctrl ep %s%u, default ep %s%u, "
	    "init pending %s%u)\n", now_ms() - started, o,
	    resp.ctrl_comm_dest_end_pt_valid ? "" : "-", resp.ctrl_comm_dest_end_pt,
	    resp.default_end_pt_valid ? "" : "-", resp.default_end_pt,
	    resp.modem_driver_init_pending_valid ? "" : "-", resp.modem_driver_init_pending);
	return true;
}

/*
 * A request the IPA service has no handler for (0x7fff). A service whose loop is running answers
 * an invalid message id with an error response at once; silence means the modem's IPA task has not
 * reached its command loop (the same probe idea as hakios.mpss.c, 24 October).
 */
static void
ipa_probe(void)
{
	struct vayu_ipa_reg_resp resp = { 0 };
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = ipa_node, .sq_port = ipa_port };
	struct qmi_txn txn;
	uint8_t raw[96];
	int ret;

	ret = qmi_txn_init(&ipa_qmi, &txn, vayu_ipa_reg_resp_ei, &resp);
	if (ret < 0) return;
	txn.raw = raw;
	txn.raw_size = sizeof(raw);
	txn.raw_length = 0;
	ret = qmi_send_request(&ipa_qmi, &to, &txn, 0x7fff, 32, NULL, NULL);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-ipa-qmi: S2 PROBE 0x7fff: ERROR send %d\n", ret);
		return;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, 1000);
	/*
	 * k18 printed "ERROR (nothing back ...)" here for every reply that came back: the wait
	 * returns the number of bytes decoded (hakios.qmi.h: "not negative: the bytes decoded"),
	 * which is never 0 for a reply, so the success branch could not be reached. The run's
	 * "ERROR" was therefore an answer. Say it as it is.
	 */
	if (ret > 0) {
		say("vayu-ipa-qmi: S2 PROBE 0x7fff: OK the service answered (result %u error %u, %zu bytes): "
		    "its loop runs\n", (unsigned)resp.result.result, (unsigned)resp.result.error, txn.raw_length);
	} else if (ret == -ETIMEDOUT) {
		say("vayu-ipa-qmi: S2 PROBE 0x7fff: TIMEOUT (nothing back to an unknown message id)\n");
	} else {
		say("vayu-ipa-qmi: S2 PROBE 0x7fff: ERROR reply not decoded (%d): something came back\n", ret);
		if (txn.raw_length > 0) ipa_say_hex("S2 PROBE 0x7fff", txn.raw, txn.raw_length);
	}
}

/* vayu_ipa_try bits 0 and 3: two messages addressed to the modem's IPA *service*, not to our own
 * server, each with a trivial or well-known handler:
 *   0x20 INDICATION_REGISTER — the mirror of our own handler (ipa_qmi_service.c:97-155); on vayu
 *        Linux sends it to the modem only on the APQ/MHI path (:2160, rmnet_ipa.c:1693), so the
 *        modem's service *may* not implement it: an answer is proof it dispatches, silence is not
 *        proof of the opposite.
 *   0x23 INSTALL_FILTER_RULE with no rules — the message the AP's data path always sends to the
 *        modem (ipa_qmi_service.c:696; stock tolerates filter_spec_list_len == 0: "IPACM pass
 *        zero rules to Q6"), so the modem's service does serve it. Any answer (success or an
 *        error) proves the service dispatched the request instead of sitting on it.
 * Both are 7-byte headers (all fields optional), harmless on a modem with no data session, and
 * each answer is logged with its result/error. */
static void
ipa_verify(uint16_t message, const char *name, uint32_t *state)
{
	struct vayu_ipa_reg_resp resp = { 0 };
	struct sockaddr_qrtr to = { .sq_family = AF_QIPCRTR, .sq_node = ipa_node, .sq_port = ipa_port };
	struct qmi_txn txn;
	uint64_t started;
	int ret;

	*state = 1;                    /* asked, nothing back so far */
	ret = qmi_txn_init(&ipa_qmi, &txn, vayu_ipa_reg_resp_ei, &resp);
	if (ret < 0) {
		say("vayu-ipa-qmi: S2 VERIFY %s: ERROR no transaction (%d)\n", name, ret);
		*state = 3;
		return;
	}
	started = now_ms();
	ret = qmi_send_request(&ipa_qmi, &to, &txn, message, 32, NULL, NULL);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-ipa-qmi: S2 VERIFY %s: ERROR send %d\n", name, ret);
		*state = 3;
		return;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, 2000);
	if (ret == -ETIMEDOUT) {
		say("vayu-ipa-qmi: S2 VERIFY %s: TIMEOUT 2000 ms to node %u port %u: no answer at all\n", name,
		    ipa_node, ipa_port);
	} else if (ret < 0) {
		say("vayu-ipa-qmi: S2 VERIFY %s: ERROR reply not decoded (%d)\n", name, ret);
		*state = 3;
	} else {
		*state = 2;
		say("vayu-ipa-qmi: S2 VERIFY %s: OK after %llu ms (result %u error %u): the modem's IPA service "
		    "dispatched it\n", name, now_ms() - started, (unsigned)resp.result.result,
		    (unsigned)resp.result.error);
	}
}

static void
ipa_hello(void)
{
	if (qmi_try & VAYU_IPA_TRY_HELLO) ipa_verify(VAYU_IPA_INDICATION_REGISTER, "0x20 INDICATION_REGISTER", &probe_020);
	if (qmi_try & VAYU_IPA_TRY_FILTER) ipa_verify(VAYU_IPA_INSTALL_FILTER_RULE, "0x23 INSTALL_FILTER_RULE(empty)", &probe_023);
	say("vayu-ipa-qmi: S2 VERIFY RESULT: 0x20 %s, 0x23 %s (%s)\n",
	    (qmi_try & VAYU_IPA_TRY_HELLO) ? (probe_020 == 2 ? "answered" : "silent") : "notasked",
	    (qmi_try & VAYU_IPA_TRY_FILTER) ? (probe_023 == 2 ? "answered" : "silent") : "notasked",
	    probe_020 == 2 || probe_023 == 2 ? "the service dispatches; the silence is in 0x21 or its state" :
	    "nothing dispatched: the service or its whole task is not serving");
}

/*
 * k19, vayu_ipa_try bit 8: the SMP2P side. Read-only except for our own entry.
 *
 * Linux's AP writes its outgoing IPA entry (qcom,smp2p-map-ipa-1-out, one smem state, two bits:
 * 0 = clock response complete, 1 = clock voted -- ipa_i.h:269-271) *in answer* to the modem's
 * query on the in entry (ipa.c:8621-8626, ipa3_smp2p_modem_clk_query_isr), i.e. as an event while
 * the modem runs. vayu_pas.c:1386-1403 writes the same value 3 before MPSS starts and rings APCS
 * once, when there is no modem yet to hear it. If the modem's IPA task waits for that
 * notification after its own boot, it never gets one. The survey shows whether the modem ever
 * asked (its own item then carries an "ipa" entry); the dance gives it a fresh edge now.
 */
#define IPA_SMP2P_OURS  428
#define IPA_SMP2P_MAGIC 0x504d5324U

static unsigned
ipa_smp2p_survey(const char *when)
{
	unsigned found = 0;

	for (uint32_t item = 424; item <= 444; item++) {
		uint32_t size = 0, at = vayu_smem_item(1, item, &size);
		uint32_t entries;

		if (at == 0 || size < 40 || vayu_smem_word(at) != IPA_SMP2P_MAGIC) continue;
		entries = vayu_smem_word(at + 12) >> 16;
		if (entries > 16) entries = 16;
		for (uint32_t i = 0; i < entries && 40 + 20 * i <= size; i++) {
			if (vayu_smem_word(at + 20 + 20 * i) != 0x00617069) continue;   /* "ipa" */
			say("vayu-ipa-qmi: S2 SMP2P %s: item %u entry %u \"ipa\" = %#x%s\n", when, item, i,
			    vayu_smem_word(at + 36 + 20 * i), item == IPA_SMP2P_OURS ? " (ours)" : " (the modem's)");
			found++;
		}
	}
	if (found == 0) {
		say("vayu-ipa-qmi: S2 SMP2P %s: no item in 424..444 carries an \"ipa\" entry: the modem never "
		    "asked for the clock\n", when);
	}
	return found;
}

/* Our own "ipa" entry (item 428) to `value`; false: the item or the entry is not there. */
static bool
ipa_smp2p_write_ours(uint32_t value)
{
	uint32_t size = 0, at = vayu_smem_item(1, IPA_SMP2P_OURS, &size), entries;

	if (at == 0 || size < 40 || vayu_smem_word(at) != IPA_SMP2P_MAGIC) return false;
	entries = vayu_smem_word(at + 12) >> 16;
	if (entries > 16) entries = 16;
	for (uint32_t i = 0; i < entries && 40 + 20 * i <= size; i++) {
		if (vayu_smem_word(at + 20 + 20 * i) == 0x00617069) {
			vayu_smem_poke(at + 36 + 20 * i, value);
			return true;
		}
	}
	return false;
}

static void
ipa_smp2p_dance(void)
{
	if (!ipa_smp2p_write_ours(0)) {
		say("vayu-ipa-qmi: S2 SMP2P DANCE: ERROR our item %u carries no \"ipa\" entry\n", IPA_SMP2P_OURS);
		return;
	}
	vayu_apcs_ring(14);                    /* dt: the modem's APCS IPC bit (vayu_modem.c:136) */
	delay_for_interval(200, NSEC_PER_MSEC);
	(void)ipa_smp2p_write_ours(3);
	vayu_apcs_ring(14);
	say("vayu-ipa-qmi: S2 SMP2P DANCE: our \"ipa\" went 0 -> 3, an APCS ring at each step (answer valid + "
	    "clock on, this time while the modem runs)\n");
}

/*
 * vayu_ipa_try bit 4: one more INIT_MODEM_DRIVER, the same bytes, sent from the port published as
 * 0x31/1 (host_port) instead of the client socket's own port. The modem's IPA service then sees a
 * request from the address its own client uses as the AP's IPA service. An answer is read in
 * ipa_receive(); the wait here only keeps the thread from running on into the idle loop mute.
 */
static void
ipa_init_from_host(void)
{
	struct vayu_ipa_init_req req;
	uint8_t buffer[512];
	uint64_t waited = 0;
	int length, ret;

	if (host_port == NULL) {
		say("vayu-ipa-qmi: S2 HOST 0x21: ERROR no published port 0x31/1\n");
		return;
	}
	ipa_init_request(&req);
	memset(buffer, 0, sizeof(buffer));
	host_init_id = (uint16_t)(0x7100 + init_attempts_made);
	host_init_result = host_init_error = 0xffff;
	host_init_state = 1;
	host_init_started = now_ms();
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_IPA_INIT_MODEM_DRIVER, host_init_id, vayu_ipa_init_req_ei,
	    &req, buffer, sizeof(buffer));
	if (length < 0) {
		say("vayu-ipa-qmi: S2 HOST 0x21: ERROR encode %d\n", length);
		host_init_state = 5;
		return;
	}
	say("vayu-ipa-qmi: S2 HOST 0x21: sending INIT_MODEM_DRIVER from the published port 0x31/1 to node %u "
	    "port %u: %d bytes, transaction %#x\n", ipa_node, ipa_port, length, host_init_id);
	ret = vayu_qrtr_ops.send(host_port, ipa_node, ipa_port, buffer, (size_t)length);
	if (ret != 0) {
		say("vayu-ipa-qmi: S2 HOST 0x21: ERROR send %d\n", ret);
		host_init_state = 5;
		return;
	}
	while (host_init_state == 1 && !ipa_stop && waited < qmi_timeout_ms) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited += 100;
	}
	if (host_init_state == 2 || host_init_state == 3) {
		return;                        /* the answer's line came from ipa_receive() */
	}
	host_init_state = 4;
	say("vayu-ipa-qmi: S2 HOST 0x21: TIMEOUT no reply in %llu ms to node %u port %u from the host port\n",
	    waited, ipa_node, ipa_port);
}

/* One line that repeats what the probes found (see the top of this file): it is printed after the
 * attempts, after every late INIT and every two minutes in the idle loop, so a capture that drops
 * the middle of the message buffer still shows the result later. */
static const char *
probe_name(uint32_t state, const char *asked)
{
	if (!asked[0]) return "notasked";

	switch (state) {
	case 2: return "answered";
	case 3: return "error";
	default: return "silent";
	}
}

static void
ipa_summary(const char *when)
{
	char host[48];
	char seq[48], answer[64];

	if (qmi_try == 0 && qmi_seq == 0) return;      /* the default run says nothing new */
	if (qmi_seq != 0) {
		snprintf(seq, sizeof(seq), "%s%s%s",
		    (qmi_seq & VAYU_IPA_SEQ_NEWLOOKUP) ? "newlookup" : "",
		    (qmi_seq & VAYU_IPA_SEQ_CLOCK) ? ((qmi_seq & VAYU_IPA_SEQ_NEWLOOKUP) ? "+clock" : "clock") : "",
		    (qmi_seq & VAYU_IPA_SEQ_NO_HOST) ? "+no-host" : "");
		if (init_done) {
			strlcpy(answer, "answered result 0 error 0", sizeof(answer));
		} else if (init_answered) {
			snprintf(answer, sizeof(answer), "answered result %u error %u", (unsigned)init_result,
			    (unsigned)init_error);
		} else {
			strlcpy(answer, "silent", sizeof(answer));
		}
		say("vayu-ipa-qmi: S2 SUMMARY %s: seq %s, 0x21 from the client port %s, INIT_MODEM_DRIVER %s after %u "
		    "attempt(s), IPA %s, host 0x21 disabled\n", when, seq[0] != 0 ? seq : "none", answer,
		    init_done ? "ok" : "no", init_attempts_made, vayu_ipa_ready() ? "ready" : "NOT ready");
		return;
	}
	switch (host_init_state) {
	case 2:
	case 3:
		snprintf(host, sizeof(host), "answered result %u error %u", (unsigned)host_init_result,
		    (unsigned)host_init_error);
		break;
	case 4: strlcpy(host, "silent", sizeof(host)); break;
	case 5: strlcpy(host, "error", sizeof(host)); break;
	default: strlcpy(host, (qmi_try & VAYU_IPA_TRY_HOST_INIT) ? "pending" : "notasked", sizeof(host)); break;
	}
	say("vayu-ipa-qmi: S2 SUMMARY %s: 0x20 %s, 0x23 %s, 0x21 from host 0x31/1 %s, INIT_MODEM_DRIVER %s after %u "
	    "attempt(s), IPA %s\n", when,
	    probe_name(probe_020, (qmi_try & VAYU_IPA_TRY_HELLO) ? "asked" : ""),
	    probe_name(probe_023, (qmi_try & VAYU_IPA_TRY_FILTER) ? "asked" : ""), host,
	    init_done ? "ok" : "no", init_attempts_made, vayu_ipa_ready() ? "ready" : "NOT ready");
}

/* The modem's client registers indications on our 0x31/1 first; in 115037 our INIT raced it.
 * Then up to vayu_ipa_qmi_attempts requests, each with the usual timeout. */
static void
stage_init(void)
{
	uint64_t waited;

	if ((qmi_try & VAYU_IPA_TRY_CRASH) || qmi_seq != 0) ipa_crash_item("before the first request");
	if (qmi_seq & VAYU_IPA_SEQ_CLOCK) {
		unsigned found = ipa_smp2p_survey("before the sequence");

		if (found != 0) {
			ipa_smp2p_dance();
		} else {
			say("vayu-ipa-qmi: S2 SMP2P CLOCK: no modem \"ipa\" entry yet; the 0 -> 3 dance is "
			    "skipped (the NEW_LOOKUP is the only change this attempt)\n");
		}
	} else if (qmi_try & VAYU_IPA_TRY_SMP2P) {
		ipa_smp2p_survey("before the INIT");
		if (qmi_try & VAYU_IPA_TRY_DANCE) ipa_smp2p_dance();
	}
	waited = wait_for(&indication_wanted, qmi_regwait_ms);
	say("vayu-ipa-qmi: S2 INDICATION_REGISTER: %s after %llu ms%s\n", indication_wanted ? "OK" : "TIMEOUT", waited,
	    indication_wanted ? "" : " (the INIT is still sent; see the probe)");
	ipa_probe();
	if (qmi_try & (VAYU_IPA_TRY_HELLO | VAYU_IPA_TRY_FILTER)) ipa_hello();
	if (qmi_try & VAYU_IPA_TRY_EMPTY) {
		ipa_ask("S2 EMPTY 0x21 (a header, no TLV)", VAYU_IPA_INIT_MODEM_DRIVER, NULL, NULL, 5000);
	}
	if (qmi_try & VAYU_IPA_TRY_MIN) {
		struct vayu_ipa_min_req min = { .platform_type_valid = 1,
			.platform_type = VAYU_IPA_PLATFORM_MSM_ANDROID };

		ipa_ask("S2 MIN 0x21 (only platform_type)", VAYU_IPA_INIT_MODEM_DRIVER, vayu_ipa_min_req_ei, &min, 5000);
	}
	for (uint32_t attempt = 1; attempt <= qmi_attempts && !ipa_stop; attempt++) {
		(void)vayu_ipa_uc_look("qmi", 0);      /* the uC state at every attempt, no wait */
		init_attempts_made = attempt;
		say("vayu-ipa-qmi: S2 INIT_MODEM_DRIVER: attempt %u/%u to node %u port %u%s%s%s\n", attempt, qmi_attempts,
		    ipa_node, ipa_port, (qmi_try & VAYU_IPA_TRY_SSR) ? " (is_ssr_bootup=1)" : "",
		    (qmi_try & VAYU_IPA_TRY_PLATFORM2) ? " (platform_type=2)" : "",
		    qmi_try != 0 ? " [vayu_ipa_try]" : "");
		if (ipa_init_modem_driver()) return;
		if ((qmi_try & VAYU_IPA_TRY_CRASH) || qmi_seq != 0) {
			char when[32];

			snprintf(when, sizeof(when), "after attempt %u", attempt);
			ipa_crash_item(when);
		}
		if (attempt < qmi_attempts) delay_for_interval(1000, NSEC_PER_MSEC);
	}
	if ((qmi_try & VAYU_IPA_TRY_CRASH) || qmi_seq != 0) ipa_crash_item("after the attempts");
	/* bit 5: our 0x21 has timed out; is the service's task still there for the questions it
	 * answered before, or did the INIT take it down? */
	if ((qmi_try & VAYU_IPA_TRY_POST) && !init_done) {
		ipa_ask("S2 POST 0x21: 0x20 INDICATION_REGISTER", VAYU_IPA_INDICATION_REGISTER, NULL, NULL, 3000);
		ipa_ask("S2 POST 0x21: 0x7fff unknown id", 0x7fff, NULL, NULL, 3000);
	}
	if ((qmi_try & VAYU_IPA_TRY_SMP2P) || (qmi_seq & VAYU_IPA_SEQ_CLOCK)) ipa_smp2p_survey("after the attempts");
	if ((qmi_try & VAYU_IPA_TRY_HOST_INIT) && !init_done) {
		if (qmi_seq != 0) {
			say("vayu-ipa-qmi: S2 HOST 0x21: refused by vayu_ipa_seq (a second connection handle is "
			    "what the k18 modem asserted on)\n");
		} else {
			ipa_init_from_host();
		}
	}
	ipa_summary("after the attempts");
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
	if (m[0] == QMI_RESPONSE && message == VAYU_IPA_INIT_MODEM_DRIVER && host_init_state == 1 &&
	    (uint16_t)(m[1] | m[2] << 8) == host_init_id) {
		host_init_result = result;
		host_init_error = error;
		host_init_state = result == 0 ? 2 : 3;
		say("vayu-ipa-qmi: S2 HOST 0x21: the modem answered after %llu ms (result %u error %u): its IPA "
		    "service dispatched the request from port 0x31/1\n", now_ms() - host_init_started,
		    (unsigned)result, (unsigned)error);
		return;
	}
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
	if (qmi_trace) hakios_qmi_trace(&wds_qmi, true);
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
	if (qmi_trace) hakios_qmi_trace(&ipa_qmi, true);
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
	if (qmi_seq & VAYU_IPA_SEQ_NEWLOOKUP) {
		/*
		 * A second NEW_LOOKUP, now that the service is known to be up: the first one (sent when
		 * the lookup was registered) may have reached the modem's name service before its IPA
		 * service registered, and a name service that does not remember lookups would then have
		 * nobody to connect. This one names the live service/instance from the client port.
		 */
		ret = qmi_add_lookup(&ipa_qmi, VAYU_IPA_QMI_SERVICE, VAYU_IPA_QMI_INSTANCE, VAYU_IPA_MODEM_INSTANCE);
		say("vayu-ipa-qmi: S1 NEW_LOOKUP after the service: %s\n", ret == 0 ? "sent" : "ERROR");
	}
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
	stage_init();
	if (!init_done) {
		say("vayu-ipa-qmi: S2 handshake stopped; WDS Start remains blocked\n");
		goto idle;
	}
	ipa_send_complete();
	(void)wait_for(&driver_complete, 10000);
	ipa_summary("after the attempts");
	say("vayu-ipa-qmi: S2 RESULT: %s (INIT_MODEM_DRIVER %s, INDICATION_REGISTER %s, INIT_COMPLETE %s, 0x35 %s)\n",
	    vayu_ipa_ready() ? "OK IPA READY" : "ERROR not ready", init_done ? "ok" : "no",
	    indication_wanted ? "ok" : "no", complete_sent ? "sent" : "NOT SENT", driver_complete ? "ok" : "TIMEOUT");
	if (stage >= 3 && !ipa_stop) stage_wds();
	if (stage >= 4) {
		say("vayu-ipa-qmi: S4 WDS START: %s (vayu_cell_data=1 drives it)\n",
		    vayu_ipa_ready() ? "allowed" : "ERROR blocked: IPA handshake not complete");
	}
idle:
	{
		/* A late answer is still useful: the uC/modem may only become ready later in the boot.
		 * Four more requests, one every 30 s; never more (this is an init message, not WDS Start). */
		uint64_t next = now_ms() + 30000;
		uint64_t next_summary = now_ms() + 120000;
		uint32_t late = 0;

		while (!ipa_stop) {
			if (!init_done && stage >= 2 && ipa_modem_there && late < 4 && now_ms() >= next) {
				late++;
				next = now_ms() + 30000;
				say("vayu-ipa-qmi: S2 LATE INIT %u/4: asking again\n", late);
				if (ipa_init_modem_driver()) {
					ipa_send_complete();
					say("vayu-ipa-qmi: S2 LATE INIT %u/4: OK; the handshake completed late\n", late);
				}
				if ((qmi_try & VAYU_IPA_TRY_SMP2P) || (qmi_seq & VAYU_IPA_SEQ_CLOCK)) {
					ipa_smp2p_survey("after a late INIT");
				}
				if ((qmi_try & VAYU_IPA_TRY_CRASH) || qmi_seq != 0) ipa_crash_item("after a late INIT");
				ipa_summary("after a late INIT");
			}
			if (now_ms() >= next_summary) {
				next_summary = now_ms() + 120000;
				ipa_summary("heartbeat");    /* nothing when vayu_ipa_try is absent */
			}
			delay_for_interval(100, NSEC_PER_MSEC);
		}
	}
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
	PE_parse_boot_argn("vayu_ipa_qmi_regwait", &qmi_regwait_ms, sizeof(qmi_regwait_ms));
	PE_parse_boot_argn("vayu_ipa_qmi_attempts", &qmi_attempts, sizeof(qmi_attempts));
	if (qmi_timeout_ms < 1000 || qmi_timeout_ms > 60000) qmi_timeout_ms = 30000;
	if (qmi_regwait_ms > 30000) qmi_regwait_ms = 30000;
	if (qmi_attempts < 1 || qmi_attempts > 5) qmi_attempts = 3;
	{
		uint32_t trace = 0;

		qmi_trace = PE_parse_boot_argn("vayu_qmi_trace", &trace, sizeof(trace)) && trace == 1;
	}
	if (PE_parse_boot_argn("vayu_ipa_try", &qmi_try, sizeof(qmi_try)) && qmi_try != 0) {
		say("vayu-ipa-qmi: vayu_ipa_try=%#x: extra probes/variants on (0x20 hello %s, 0x23 filter %s, "
		    "INIT is_ssr_bootup %s, platform_type %s, 0x21 from host 0x31/1 %s, post-0x21 %s, "
		    "empty 0x21 %s, min 0x21 %s, smp2p survey %s, smp2p dance %s, crash421 %s)\n", qmi_try,
		    (qmi_try & VAYU_IPA_TRY_HELLO) ? "on" : "off", (qmi_try & VAYU_IPA_TRY_FILTER) ? "on" : "off",
		    (qmi_try & VAYU_IPA_TRY_SSR) ? "1" : "absent",
		    (qmi_try & VAYU_IPA_TRY_PLATFORM2) ? "2" : "3",
		    (qmi_try & VAYU_IPA_TRY_HOST_INIT) ? "on" : "off",
		    (qmi_try & VAYU_IPA_TRY_POST) ? "on" : "off",
		    (qmi_try & VAYU_IPA_TRY_EMPTY) ? "on" : "off",
		    (qmi_try & VAYU_IPA_TRY_MIN) ? "on" : "off",
		    (qmi_try & VAYU_IPA_TRY_SMP2P) ? "on" : "off",
		    (qmi_try & VAYU_IPA_TRY_DANCE) ? "on" : "off",
		    (qmi_try & VAYU_IPA_TRY_CRASH) ? "on" : "off");
	}
	if (PE_parse_boot_argn("vayu_ipa_seq", &qmi_seq, sizeof(qmi_seq)) && qmi_seq != 0) {
		bool host_was = (qmi_try & VAYU_IPA_TRY_HOST_INIT) != 0;

		/*
		 * The sequence never sends 0x21 from the published 0x31/1 port: the k18 host-port send
		 * is the request that preceded the modem's ipa_qmi_server.c:559 handle assert. Clear the
		 * bit even when the job still carries it, so the old experiment cannot come back by
		 * accident.
		 */
		qmi_try &= ~VAYU_IPA_TRY_HOST_INIT;
		if (qmi_seq & VAYU_IPA_SEQ_NEWLOOKUP) {
			vayu_qrtr_lookup_announce(VAYU_IPA_QMI_SERVICE);
		}
		say("vayu-ipa-qmi: vayu_ipa_seq=%#x: clean 0x21 sequence (NEW_LOOKUP %s, SMP2P clock %s, "
		    "host 0x21 forbidden%s)\n", qmi_seq,
		    (qmi_seq & VAYU_IPA_SEQ_NEWLOOKUP) ? "on" : "off",
		    (qmi_seq & VAYU_IPA_SEQ_CLOCK) ? "on" : "off",
		    host_was ? "; bit 0x10 was set and is ignored" : "");
	}
	say("vayu-ipa-qmi: vayu_ipa_qmi=%u wait %u ms, INIT_MODEM_DRIVER timeout %u ms in %u attempt(s), "
	    "indication wait %u ms\n", stage, qmi_wait_ms, qmi_timeout_ms, qmi_attempts, qmi_regwait_ms);
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
