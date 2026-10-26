/*
 * Tests of the QMI messages of bsd/dev/vayu_cell.c and of the two helpers that decide what of a
 * SIM's identifiers may be kept: the card status and serving system replies are decoded from
 * bytes written out here by hand (and encoded back to the same bytes), the requests of a file
 * read and of a network register are encoded and compared, and the digit and masking helpers are
 * tried on the contents a real card has. tools/cell-test/run.sh compiles and runs this on the
 * machine that builds the kernel, with bsd/dev/hakios.qmi.c as it is in the kernel.
 *
 * What is NOT tested here: the modem. Every byte below is this repository's reading of libqmi's
 * message descriptions (sys/vayu_cell_qmi.h names them); whether an X24 answers like this is a
 * HYPOTHESIS until vayu_cell runs on the phone.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/hakios.qmi.h>
#include <sys/vayu_cell.h>
#include <sys/vayu_cell_qmi.h>
#include <sys/vayu_cell_data_qmi.h>

static int checks, failures;

static void
check(bool ok, const char *format, ...)
{
	va_list list;

	checks++;
	if (ok) {
		return;
	}
	failures++;
	printf("FAIL: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

static void
check_bytes(const uint8_t *got, int got_length, const uint8_t *want, size_t want_length, const char *what)
{
	bool same = got_length == (int)want_length && memcmp(got, want, want_length) == 0;

	check(same, "%s: the bytes are not the expected ones", what);
	if (!same) {
		printf("  got  (%d):", got_length);
		for (int i = 0; i < got_length && i < 96; i++) {
			printf(" %02x", got[i]);
		}
		printf("\n  want (%zu):", want_length);
		for (size_t i = 0; i < want_length && i < 96; i++) {
			printf(" %02x", want[i]);
		}
		printf("\n");
	}
}

/* ---- UIM: Get Card Status (0x002f) ------------------------------------------------------------ */

/*
 * TLV 2   Operation Result: result 0, error 0.
 * TLV 0x10 UIM Card Status: one card (present, UPIN disabled, 3 tries), one application (USIM,
 *         READY, AID a0000000871002, PIN1/PIN2 disabled, 3 tries and 10 PUK tries).
 */
static const uint8_t card_status_wire[] = {
	0x02, 0x34, 0x12, 0x2f, 0x00, 0x2e, 0x00,
	0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x24, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01,
	0x01, 0x03, 0x03, 0x0a, 0x00,
	0x01,
	0x02, 0x07, 0x00, 0x00, 0x00, 0x00,
	0x07, 0xa0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02,
	0x00, 0x03, 0x03, 0x0a, 0x03, 0x03, 0x0a,
};

static void
test_card_status(void)
{
	struct vayu_cell_uim_get_card_status_resp resp;
	uint8_t out[512];
	uint8_t damaged[sizeof(card_status_wire)];
	int length;

	memset(&resp, 0, sizeof(resp));
	length = hakios_qmi_decode(card_status_wire, sizeof(card_status_wire), vayu_cell_uim_get_card_status_resp_ei, &resp);
	check(length == (int)sizeof(card_status_wire), "card status: decode returned %d, not %zu", length,
	    sizeof(card_status_wire));
	check(resp.result.result == 0 && resp.result.error == 0, "card status: result %u error %u",
	    (unsigned int)resp.result.result, (unsigned int)resp.result.error);
	check(resp.card_status_valid == 1, "card status: the card status TLV was not seen");
	check(resp.card_status.cards_len == 1, "card status: %u cards, not 1", (unsigned int)resp.card_status.cards_len);
	check(resp.card_status.index_gw_primary == 1, "card status: index GW primary %u, not 1",
	    (unsigned int)resp.card_status.index_gw_primary);
	check(resp.card_status.cards[0].card_state == VAYU_CELL_CARD_PRESENT, "card status: card state %u, not present",
	    (unsigned int)resp.card_status.cards[0].card_state);
	check(resp.card_status.cards[0].upin_state == VAYU_CELL_PIN_DISABLED, "card status: UPIN state %u",
	    (unsigned int)resp.card_status.cards[0].upin_state);
	check(resp.card_status.cards[0].upin_retries == 3, "card status: UPIN retries %u",
	    (unsigned int)resp.card_status.cards[0].upin_retries);
	check(resp.card_status.cards[0].app_len == 1, "card status: %u applications",
	    (unsigned int)resp.card_status.cards[0].app_len);
	check(resp.card_status.cards[0].app[0].type == VAYU_CELL_UIM_APP_TYPE_USIM, "card status: application type %u",
	    (unsigned int)resp.card_status.cards[0].app[0].type);
	check(resp.card_status.cards[0].app[0].state == VAYU_CELL_UIM_APP_READY, "card status: application state %u",
	    (unsigned int)resp.card_status.cards[0].app[0].state);
	check(resp.card_status.cards[0].app[0].aid_len == 7, "card status: AID length %u",
	    (unsigned int)resp.card_status.cards[0].app[0].aid_len);
	check(memcmp(resp.card_status.cards[0].app[0].aid, "\xa0\x00\x00\x00\x87\x10\x02", 7) == 0,
	    "card status: the AID is not a0000000871002");
	check(resp.card_status.cards[0].app[0].pin1_state == VAYU_CELL_PIN_DISABLED, "card status: PIN1 state %u",
	    (unsigned int)resp.card_status.cards[0].app[0].pin1_state);

	memset(out, 0, sizeof(out));
	length = hakios_qmi_encode(QMI_RESPONSE, VAYU_CELL_UIM_GET_CARD_STATUS, 0x1234,
	    vayu_cell_uim_get_card_status_resp_ei, &resp, out, sizeof(out));
	check_bytes(out, length, card_status_wire, sizeof(card_status_wire), "card status: encoded back");

	/* A message shorter than its TLV says is refused. */
	memcpy(damaged, card_status_wire, sizeof(damaged));
	check(hakios_qmi_decode(damaged, sizeof(damaged) - 1, vayu_cell_uim_get_card_status_resp_ei, &resp) < 0,
	    "card status: a truncated message was taken");

	/* More cards than the structure holds is refused: the count is the table's limit. */
	memcpy(damaged, card_status_wire, sizeof(damaged));
	damaged[25] = 5;                        /* the cards' count, after the four indexes */
	check(hakios_qmi_decode(damaged, sizeof(damaged), vayu_cell_uim_get_card_status_resp_ei, &resp) < 0,
	    "card status: 5 cards were taken into a 4-card structure");
}

/* ---- NAS: Get Serving System (0x0024) --------------------------------------------------------- */

/*
 * TLV 2    Operation Result: 0, 0.
 * TLV 0x01 Serving System: registered, CS and PS attached, 3GPP, radio LTE.
 * TLV 0x10 Roaming Indicator: 1 (off: home).
 * TLV 0x12 Current PLMN: MCC 250, MNC 1, "MTS RUS".
 * TLV 0x1c LAC 0x1234; TLV 0x1d CID 0x12345678.
 * TLV 0x21 Detailed Service Status: status 2 (full service), capability 1 (CS+PS).
 */
static const uint8_t serving_wire[] = {
	0x02, 0x35, 0x12, 0x24, 0x00, 0x37, 0x00,
	0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x06, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x08,
	0x10, 0x01, 0x00, 0x01,
	0x12, 0x0c, 0x00, 0xfa, 0x00, 0x01, 0x00, 0x07, 0x4d, 0x54, 0x53, 0x20, 0x52, 0x55, 0x53,
	0x1c, 0x02, 0x00, 0x34, 0x12,
	0x1d, 0x04, 0x00, 0x78, 0x56, 0x34, 0x12,
	0x21, 0x05, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00,
};

static void
test_serving_system(void)
{
	struct vayu_cell_nas_get_serving_system_resp resp;
	uint8_t out[512];
	uint8_t extra[sizeof(serving_wire) + 8];
	int length;

	memset(&resp, 0, sizeof(resp));
	length = hakios_qmi_decode(serving_wire, sizeof(serving_wire), vayu_cell_nas_get_serving_system_resp_ei, &resp);
	check(length == (int)sizeof(serving_wire), "serving: decode returned %d, not %zu", length, sizeof(serving_wire));
	check(resp.result.result == 0 && resp.result.error == 0, "serving: result %u error %u",
	    (unsigned int)resp.result.result, (unsigned int)resp.result.error);
	check(resp.serving_valid == 1, "serving: the serving system TLV was not seen");
	check(resp.serving.registration_state == VAYU_CELL_NAS_REG_REGISTERED, "serving: registration state %u",
	    (unsigned int)resp.serving.registration_state);
	check(resp.serving.cs_attach_state == 1 && resp.serving.ps_attach_state == 1, "serving: attach %u/%u",
	    (unsigned int)resp.serving.cs_attach_state, (unsigned int)resp.serving.ps_attach_state);
	check(resp.serving.selected_network == 2, "serving: selected network %u",
	    (unsigned int)resp.serving.selected_network);
	check(resp.serving.radio_len == 1 && resp.serving.radio[0] == VAYU_CELL_NAS_RADIO_LTE,
	    "serving: radio length %u first %u", (unsigned int)resp.serving.radio_len,
	    (unsigned int)(resp.serving.radio_len ? resp.serving.radio[0] : 0));
	check(resp.roam_valid == 1 && resp.roam == VAYU_CELL_NAS_ROAM_OFF, "serving: roaming %u/%u",
	    (unsigned int)resp.roam_valid, (unsigned int)resp.roam);
	check(resp.plmn_valid == 1, "serving: the PLMN TLV was not seen");
	check(resp.plmn.mcc == 250 && resp.plmn.mnc == 1, "serving: MCC %u MNC %u",
	    (unsigned int)resp.plmn.mcc, (unsigned int)resp.plmn.mnc);
	check(strcmp(resp.plmn.description, "MTS RUS") == 0, "serving: the description is \"%s\"", resp.plmn.description);
	check(resp.lac_valid == 1 && resp.lac == 0x1234, "serving: LAC %#x", (unsigned int)resp.lac);
	check(resp.cid_valid == 1 && resp.cid == 0x12345678, "serving: CID %#x", (unsigned int)resp.cid);
	check(resp.detailed_valid == 1 && resp.detailed.status == 2 && resp.detailed.capability == 1,
	    "serving: detailed %u/%u", (unsigned int)resp.detailed.status, (unsigned int)resp.detailed.capability);

	memset(out, 0, sizeof(out));
	length = hakios_qmi_encode(QMI_RESPONSE, VAYU_CELL_NAS_GET_SERVING_SYSTEM, 0x1235,
	    vayu_cell_nas_get_serving_system_resp_ei, &resp, out, sizeof(out));
	check_bytes(out, length, serving_wire, sizeof(serving_wire), "serving: encoded back");

	/* A TLV the table does not know (0x7f) is skipped, not refused. */
	memcpy(extra, serving_wire, sizeof(serving_wire));
	extra[sizeof(serving_wire) + 0] = 0x7f;
	extra[sizeof(serving_wire) + 1] = 0x02;
	extra[sizeof(serving_wire) + 2] = 0x00;
	extra[sizeof(serving_wire) + 3] = 0xaa;
	extra[sizeof(serving_wire) + 4] = 0xbb;
	extra[5] = (uint8_t)(extra[5] + 5);     /* the message's length */
	memset(&resp, 0, sizeof(resp));
	check(hakios_qmi_decode(extra, sizeof(serving_wire) + 5, vayu_cell_nas_get_serving_system_resp_ei, &resp) >= 0 &&
	    resp.plmn_valid == 1 && resp.plmn.mcc == 250, "serving: an unknown TLV was not skipped");

	/* A string that is not NUL-terminated inside its TLV is refused (the codec's rule). */
	memcpy(extra, serving_wire, sizeof(serving_wire));
	extra[34] = 0x7f;                       /* the description's length, now past the TLV */
	check(hakios_qmi_decode(extra, sizeof(serving_wire), vayu_cell_nas_get_serving_system_resp_ei, &resp) < 0,
	    "serving: an over-long description was taken");
}

/* ---- NAS: Get Signal Info (0x004f) ------------------------------------------------------------ */

/* TLV 2 result; TLV 0x14 LTE: RSSI -65 dBm, RSRQ -10 dB, RSRP -87 dBm, SNR 12.3 dB. */
static const uint8_t signal_wire[] = {
	0x02, 0x36, 0x12, 0x4f, 0x00, 0x10, 0x00,
	0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x14, 0x06, 0x00, 0xbf, 0xf6, 0xa9, 0xff, 0x7b, 0x00,
};

static void
test_signal_info(void)
{
	struct vayu_cell_nas_get_signal_info_resp resp;
	uint8_t out[256];
	int length;

	memset(&resp, 0, sizeof(resp));
	length = hakios_qmi_decode(signal_wire, sizeof(signal_wire), vayu_cell_nas_get_signal_info_resp_ei, &resp);
	check(length == (int)sizeof(signal_wire), "signal: decode returned %d", length);
	check(resp.lte_valid == 1, "signal: the LTE TLV was not seen");
	check((int8_t)resp.lte.rssi == -65, "signal: RSSI %d", (int)(int8_t)resp.lte.rssi);
	check((int8_t)resp.lte.rsrq == -10, "signal: RSRQ %d", (int)(int8_t)resp.lte.rsrq);
	check(resp.lte.rsrp == -87, "signal: RSRP %d", (int)resp.lte.rsrp);
	check(resp.lte.snr == 123, "signal: SNR %d", (int)resp.lte.snr);

	memset(out, 0, sizeof(out));
	length = hakios_qmi_encode(QMI_RESPONSE, VAYU_CELL_NAS_GET_SIGNAL_INFO, 0x1236,
	    vayu_cell_nas_get_signal_info_resp_ei, &resp, out, sizeof(out));
	check_bytes(out, length, signal_wire, sizeof(signal_wire), "signal: encoded back");
}

/* ---- the requests ----------------------------------------------------------------------------- */

static void
test_requests(void)
{
	struct vayu_cell_uim_read_record_req record = { 0 };
	struct vayu_cell_uim_read_transparent_req transparent = { 0 };
	struct vayu_cell_nas_initiate_register_req reg = { .action = VAYU_CELL_NAS_REGISTER_AUTOMATIC };
	static const uint8_t record_wire[] = {
		0x00, 0x01, 0x00, 0x21, 0x00, 0x14, 0x00,
		0x01, 0x02, 0x00, 0x00, 0x00,
		0x02, 0x05, 0x00, 0xe2, 0x2f, 0x02, 0x00, 0x3f,
		0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t transparent_wire[] = {
		0x00, 0x02, 0x00, 0x20, 0x00, 0x14, 0x00,
		0x01, 0x02, 0x00, 0x00, 0x00,
		0x02, 0x05, 0x00, 0xe2, 0x2f, 0x02, 0x00, 0x3f,
		0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t register_wire[] = {
		0x00, 0x03, 0x00, 0x22, 0x00, 0x04, 0x00,
		0x01, 0x01, 0x00, 0x01,
	};
	uint8_t out[128];
	int length;

	record.session.session_type = VAYU_CELL_UIM_SESSION_PRIMARY_GW;
	record.session.aid_len = 0;
	record.file.file_id = VAYU_CELL_UIM_EF_ICCID;
	record.file.path_len = 2;
	record.file.path[0] = 0x00;
	record.file.path[1] = 0x3f;
	record.record.record_number = 0;
	record.record.record_length = 0;
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_UIM_READ_RECORD, 1, vayu_cell_uim_read_record_req_ei,
	    &record, out, sizeof(out));
	check_bytes(out, length, record_wire, sizeof(record_wire), "read record request");

	transparent.session.session_type = VAYU_CELL_UIM_SESSION_PRIMARY_GW;
	transparent.session.aid_len = 0;
	transparent.file.file_id = VAYU_CELL_UIM_EF_ICCID;
	transparent.file.path_len = 2;
	transparent.file.path[0] = 0x00;
	transparent.file.path[1] = 0x3f;
	transparent.read.offset = 0;
	transparent.read.length = 0;
	length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_UIM_READ_TRANSPARENT, 2, vayu_cell_uim_read_transparent_req_ei,
	    &transparent, out, sizeof(out));
	check_bytes(out, length, transparent_wire, sizeof(transparent_wire), "read transparent request");

	length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_NAS_INITIATE_NETWORK_REGISTER, 3,
	    vayu_cell_nas_initiate_register_req_ei, &reg, out, sizeof(out));
	check_bytes(out, length, register_wire, sizeof(register_wire), "initiate register request");

	/* The reply to it: result 0 alone. */
	{
		static const uint8_t reply[] = { 0x02, 0x03, 0x00, 0x22, 0x00, 0x07, 0x00,
			0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
		struct vayu_cell_nas_register_resp resp;

		memset(&resp, 0, sizeof(resp));
		check(hakios_qmi_decode(reply, sizeof(reply), vayu_cell_nas_register_resp_ei, &resp) == (int)sizeof(reply) &&
		    resp.result.result == 0, "initiate register reply");
	}
}

/* ---- DMS ONLINE, UIM slot/provisioning and UMTS signal --------------------------------------- */

static void
test_modem_state_messages(void)
{
	uint8_t out[256];
	int length;

	/* UIM Switch Slot: logical slot 1 -> physical slot 2. */
	{
		struct vayu_cell_uim_switch_slot_req request = { .logical_slot = 1, .physical_slot = 2 };
		static const uint8_t want[] = {
			0x00, 0x04, 0x00, 0x46, 0x00, 0x0b, 0x00,
			0x01, 0x01, 0x00, 0x01,
			0x02, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
		};

		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_UIM_SWITCH_SLOT, 4,
		    vayu_cell_uim_switch_slot_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "UIM switch slot request");
	}

	/* Primary GW provisioning: activate, logical slot 1, AID a0000000871002. */
	{
		struct vayu_cell_uim_change_provisioning_req request = { 0 };
		static const uint8_t aid[] = { 0xa0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02 };
		static const uint8_t want[] = {
			0x00, 0x05, 0x00, 0x38, 0x00, 0x11, 0x00,
			0x01, 0x02, 0x00, 0x00, 0x01,
			0x10, 0x09, 0x00, 0x01, 0x07,
			0xa0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02,
		};

		request.change.session_type = VAYU_CELL_UIM_SESSION_PRIMARY_GW;
		request.change.activate = 1;
		request.application.slot = 1;
		request.application.aid_len = sizeof(aid);
		memcpy(request.application.aid, aid, sizeof(aid));
		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_UIM_CHANGE_PROVISIONING, 5,
		    vayu_cell_uim_change_provisioning_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "UIM change provisioning request");
	}

	/* DMS ONLINE is one byte in TLV 1; Get Operating Mode returns the same mode. */
	{
		struct vayu_cell_dms_set_operating_mode_req request = { .mode = VAYU_CELL_DMS_MODE_ONLINE };
		struct vayu_cell_dms_get_operating_mode_resp response;
		static const uint8_t want[] = {
			0x00, 0x06, 0x00, 0x2e, 0x00, 0x04, 0x00,
			0x01, 0x01, 0x00, 0x00,
		};
		static const uint8_t reply[] = {
			0x02, 0x07, 0x00, 0x2d, 0x00, 0x0b, 0x00,
			0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x01, 0x01, 0x00, 0x00,
		};

		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_DMS_SET_OPERATING_MODE, 6,
		    vayu_cell_dms_set_operating_mode_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "DMS set ONLINE request");
		memset(&response, 0, sizeof(response));
		length = hakios_qmi_decode(reply, sizeof(reply), vayu_cell_dms_get_operating_mode_resp_ei, &response);
		check(length == (int)sizeof(reply) && response.mode_valid == 1 &&
		    response.mode == VAYU_CELL_DMS_MODE_ONLINE, "DMS get operating mode response");
	}

	/* NAS Get Signal Info: WCDMA RSSI -86 dBm and raw ECIO -95. */
	{
		struct vayu_cell_nas_get_signal_info_resp response;
		static const uint8_t reply[] = {
			0x02, 0x08, 0x00, 0x4f, 0x00, 0x0d, 0x00,
			0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x13, 0x03, 0x00, 0xaa, 0xa1, 0xff,
		};

		memset(&response, 0, sizeof(response));
		length = hakios_qmi_decode(reply, sizeof(reply), vayu_cell_nas_get_signal_info_resp_ei, &response);
		check(length == (int)sizeof(reply) && response.wcdma_valid == 1, "UMTS signal TLV decoded");
		check((int8_t)response.wcdma.rssi == -86 && response.wcdma.ecio == -95,
		    "UMTS RSSI %d ECIO %d", (int)(int8_t)response.wcdma.rssi, (int)response.wcdma.ecio);
		memset(out, 0, sizeof(out));
		length = hakios_qmi_encode(QMI_RESPONSE, VAYU_CELL_NAS_GET_SIGNAL_INFO, 8,
		    vayu_cell_nas_get_signal_info_resp_ei, &response, out, sizeof(out));
		check_bytes(out, length, reply, sizeof(reply), "UMTS signal encoded back");
	}
}

/* ---- DPM/WDA/WDS data-session control messages ----------------------------------------------- */

static void
test_data_messages(void)
{
	uint8_t out[256];
	int length;

	/* DPM Open Port: one hardware port, embedded interface 1, modem RX 2 / TX 11 (vayu). */
	{
		struct vayu_cell_dpm_open_port_req request = { 0 };
		static const uint8_t want[] = {
			0x00, 0x09, 0x00, 0x20, 0x00, 0x14, 0x00,
			0x11, 0x11, 0x00, 0x01,
			0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
		};

		request.ports_len = 1;
		request.ports[0].endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
		request.ports[0].endpoint.interface_number = 1;
		request.ports[0].rx_endpoint_number = 2;
		request.ports[0].tx_endpoint_number = 11;
		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_DPM_OPEN_PORT, 9,
		    vayu_cell_dpm_open_port_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "DPM open port request");
	}

	/* WDA: raw IP/QMAP, 16 datagrams and 0x3800 bytes, embedded interface 1. */
	{
		struct vayu_cell_wda_set_format_req request = { 0 };
		static const uint8_t want[] = {
			0x00, 0x0a, 0x00, 0x20, 0x00, 0x2e, 0x00,
			0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
			0x12, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00,
			0x13, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00,
			0x15, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00,
			0x16, 0x04, 0x00, 0x00, 0x38, 0x00, 0x00,
			0x17, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		};

		request.link_layer_protocol = VAYU_CELL_RAW_IP;
		request.uplink_aggregation = request.downlink_aggregation = VAYU_CELL_QMAP;
		request.downlink_max_datagrams = 16;
		request.downlink_max_size = 0x3800;
		request.endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
		request.endpoint.interface_number = 1;
		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDA_SET_DATA_FORMAT, 10,
		    vayu_cell_wda_set_format_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "WDA set data format request");
	}

	/* WDS bind + IP family, then Start Network Interface without an APN TLV. */
	{
		struct vayu_cell_wds_bind_req bind = { 0 };
		struct vayu_cell_wds_ip_family_req family = { .preference = VAYU_CELL_IPV4 };
		struct vayu_cell_wds_start_req start = { .ip_family = VAYU_CELL_IPV4 };
		static const uint8_t bind_want[] = {
			0x00, 0x0b, 0x00, 0xa2, 0x00, 0x0f, 0x00,
			0x10, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
			0x11, 0x01, 0x00, 0x01,
		};
		static const uint8_t family_want[] = {
			0x00, 0x0c, 0x00, 0x4d, 0x00, 0x04, 0x00,
			0x01, 0x01, 0x00, 0x04,
		};
		static const uint8_t start_want[] = {
			0x00, 0x0d, 0x00, 0x20, 0x00, 0x04, 0x00,
			0x19, 0x01, 0x00, 0x04,
		};

		bind.endpoint.endpoint_type = VAYU_CELL_ENDPOINT_EMBEDDED;
		bind.endpoint.interface_number = 1;
		bind.mux_id = 1;
		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDS_BIND_MUX_DATA_PORT, 11,
		    vayu_cell_wds_bind_req_ei, &bind, out, sizeof(out));
		check_bytes(out, length, bind_want, sizeof(bind_want), "WDS bind mux request");
		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDS_SET_IP_FAMILY, 12,
		    vayu_cell_wds_ip_family_req_ei, &family, out, sizeof(out));
		check_bytes(out, length, family_want, sizeof(family_want), "WDS IP family request");
		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDS_START_NETWORK_INTERFACE, 13,
		    vayu_cell_wds_start_req_ei, &start, out, sizeof(out));
		check_bytes(out, length, start_want, sizeof(start_want), "WDS start request");
	}

	/* The accepted session carries a handle; settings decode address/gateway/DNS/MTU. */
	{
		struct vayu_cell_wds_start_resp start;
		struct vayu_cell_wds_settings_resp settings;
		static const uint8_t accepted[] = {
			0x02, 0x0d, 0x00, 0x20, 0x00, 0x0e, 0x00,
			0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x01, 0x04, 0x00, 0x78, 0x56, 0x34, 0x12,
		};
		static const uint8_t current[] = {
			0x02, 0x0e, 0x00, 0x2d, 0x00, 0x35, 0x00,
			0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x15, 0x04, 0x00, 0x08, 0x08, 0x08, 0x08,
			0x16, 0x04, 0x00, 0x01, 0x01, 0x01, 0x01,
			0x1e, 0x04, 0x00, 0x93, 0x13, 0x14, 0x0a,
			0x20, 0x04, 0x00, 0x94, 0x13, 0x14, 0x0a,
			0x21, 0x04, 0x00, 0xf8, 0xff, 0xff, 0xff,
			0x29, 0x04, 0x00, 0xdc, 0x05, 0x00, 0x00,
			0x2b, 0x01, 0x00, 0x04,
		};

		memset(&start, 0, sizeof(start));
		length = hakios_qmi_decode(accepted, sizeof(accepted), vayu_cell_wds_start_resp_ei, &start);
		check(length == (int)sizeof(accepted) && start.handle_valid == 1 && start.handle == 0x12345678,
		    "WDS start response handle");
		memset(&settings, 0, sizeof(settings));
		length = hakios_qmi_decode(current, sizeof(current), vayu_cell_wds_settings_resp_ei, &settings);
		check(length == (int)sizeof(current), "WDS settings decode returned %d", length);
		check(settings.dns1_valid && settings.dns1 == 0x08080808 && settings.dns2_valid && settings.dns2 == 0x01010101,
		    "WDS DNS values");
		check(settings.address_valid && settings.address == 0x0a141393 && settings.gateway_valid &&
		    settings.gateway == 0x0a141394 && settings.mask_valid && settings.mask == 0xfffffff8 &&
		    settings.mtu_valid && settings.mtu == 1500 && settings.ip_family_valid && settings.ip_family == 4,
		    "WDS address/gateway/mask/MTU/IP family values");
	}
}

static void
test_data_lifecycle_messages(void)
{
	uint8_t out[256];
	int length;

	/* Stop uses the handle returned by Start; no second Start is implied. */
	{
		struct vayu_cell_wds_stop_req request = { .handle = 0x12345678 };
		static const uint8_t want[] = {
			0x00, 0x0f, 0x00, 0x21, 0x00, 0x07, 0x00,
			0x01, 0x04, 0x00, 0x78, 0x56, 0x34, 0x12,
		};

		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDS_STOP_NETWORK_INTERFACE, 15,
		    vayu_cell_wds_stop_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "WDS stop request");
	}

	/* APN is present only when supplied; top-level strings carry no trailing NUL. */
	{
		struct vayu_cell_wds_start_req request = { .ip_family = VAYU_CELL_IPV4, .apn_valid = 1 };
		static const uint8_t want[] = {
			0x00, 0x10, 0x00, 0x20, 0x00, 0x0f, 0x00,
			0x14, 0x08, 0x00, 'i', 'n', 't', 'e', 'r', 'n', 'e', 't',
			0x19, 0x01, 0x00, 0x04,
		};

		memcpy(request.apn, "internet", sizeof("internet"));
		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDS_START_NETWORK_INTERFACE, 16,
		    vayu_cell_wds_start_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "WDS start with APN request");
	}

	/* Get Current Settings asks for DNS, address, gateway, MTU and IP family. */
	{
		struct vayu_cell_wds_settings_req request = { .requested_settings = VAYU_CELL_SETTINGS_MASK };
		static const uint8_t want[] = {
			0x00, 0x11, 0x00, 0x2d, 0x00, 0x07, 0x00,
			0x10, 0x04, 0x00, 0x10, 0xa3, 0x00, 0x00,
		};

		length = hakios_qmi_encode(QMI_REQUEST, VAYU_CELL_WDS_GET_CURRENT_SETTINGS, 17,
		    vayu_cell_wds_settings_req_ei, &request, out, sizeof(out));
		check_bytes(out, length, want, sizeof(want), "WDS settings request");
	}
}

/* ---- the card's digits, and what of them may be shown ----------------------------------------- */

static void
test_digits(void)
{
	/* EF_ICCID of 20 digits: 89442012345678901234, low nibble first (TS 51.011). */
	static const uint8_t iccid[] = { 0x98, 0x44, 0x02, 0x21, 0x43, 0x65, 0x87, 0x09, 0x21, 0x43 };
	/* EF_IMSI of 15 digits: 250011234567890, one byte of length, then low nibble first, 0xf parity. */
	static const uint8_t imsi[] = { 0x0f, 0x52, 0x00, 0x11, 0x32, 0x54, 0x76, 0x98, 0xf0 };
	char digits[32];
	char tail[VAYU_CELL_TAIL_MAX];
	uint32_t count;

	count = vayu_cell_uim_digits(iccid, sizeof(iccid), false, digits, sizeof(digits));
	check(count == 20 && memcmp(digits, "89442012345678901234", 20) == 0, "ICCID digits: %u \"%.*s\"",
	    (unsigned int)count, (int)count, digits);
	vayu_cell_mask_tail(digits, count, tail);
	check(strcmp(tail, "...1234") == 0, "ICCID tail: \"%s\"", tail);
	check(strlen(tail) == 7, "ICCID tail length %zu", strlen(tail));

	count = vayu_cell_uim_digits(imsi, sizeof(imsi), true, digits, sizeof(digits));
	check(count == 15 && memcmp(digits, "250011234567890", 15) == 0, "IMSI digits: %u \"%.*s\"",
	    (unsigned int)count, (int)count, digits);
	vayu_cell_mask_tail(digits, count, tail);
	check(strcmp(tail, "...7890") == 0, "IMSI tail: \"%s\"", tail);

	/* A nibble that is not a digit and not 0xf: nothing is trusted. */
	{
		static const uint8_t bad[] = { 0x98, 0x4a, 0x02 };

		check(vayu_cell_uim_digits(bad, sizeof(bad), false, digits, sizeof(digits)) == 0,
		    "a non-BCD nibble was decoded");
	}
	/* A file shorter than its own length byte says: nothing is trusted. */
	{
		static const uint8_t short_imsi[] = { 0x0f, 0x52, 0x00 };

		check(vayu_cell_uim_digits(short_imsi, sizeof(short_imsi), true, digits, sizeof(digits)) == 0,
		    "an IMSI shorter than its length byte was decoded");
	}

	/* Short identifiers show nothing: the whole of a short number must not be its "tail". */
	{
		static const char four[] = "1234";
		static const char five[] = "12345";
		static const char eight[] = "12345678";

		vayu_cell_mask_tail(four, 4, tail);
		check(strcmp(tail, "...") == 0, "four digits: \"%s\"", tail);
		vayu_cell_mask_tail(five, 5, tail);
		check(strcmp(tail, "...45") == 0, "five digits: \"%s\"", tail);
		vayu_cell_mask_tail(eight, 8, tail);
		check(strcmp(tail, "...5678") == 0, "eight digits: \"%s\"", tail);
	}
}

int
main(void)
{
	test_card_status();
	test_serving_system();
	test_signal_info();
	test_requests();
	test_modem_state_messages();
	test_data_messages();
	test_data_lifecycle_messages();
	test_digits();
	if (failures != 0) {
		printf("cell-test: %d of %d checks FAILED\n", failures, checks);
		return EXIT_FAILURE;
	}
	printf("cell-test: ALL PASS (%d checks)\n", checks);
	return EXIT_SUCCESS;
}
