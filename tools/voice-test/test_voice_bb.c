/*
 * Host tests of bsd/dev/vayu_voice_bb_proto.c: the kernel's answers to CommCenter's requests on
 * Apple's QMI Audio Service 0xE1.  Each request is the SDU CommCenter's QMIAudioRoutingCommandDriver,
 * QMICallAudioDriver or QMIAudioCommandDriver builds (20F66; sys/vayu_voice_bb.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/vayu_voice_bb.h>

static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static size_t
req(uint8_t *out, uint16_t txn, uint16_t msg, const uint8_t *tlvs, uint16_t tlv_length)
{
	out[0] = 0x00;
	out[1] = (uint8_t)txn;
	out[2] = (uint8_t)(txn >> 8);
	out[3] = (uint8_t)msg;
	out[4] = (uint8_t)(msg >> 8);
	out[5] = (uint8_t)tlv_length;
	out[6] = (uint8_t)(tlv_length >> 8);
	if (tlv_length != 0) {
		memcpy(out + 7, tlvs, tlv_length);
	}
	return 7 + (size_t)tlv_length;
}

/* the answer is a response to (txn, msg) with result TLV {result, error} */
static void
check_answer(const struct vayu_voice_bb_out *o, uint16_t txn, uint16_t msg, uint16_t error)
{
	CHECK(o->answer_length == 14);
	CHECK(o->answer[0] == 0x02);
	CHECK((o->answer[1] | o->answer[2] << 8) == txn);
	CHECK((o->answer[3] | o->answer[4] << 8) == msg);
	CHECK((o->answer[5] | o->answer[6] << 8) == 7);
	CHECK(o->answer[7] == 0x02 && o->answer[8] == 4 && o->answer[9] == 0);
	CHECK((o->answer[10] | o->answer[11] << 8) == (error != 0 ? 1 : 0));
	CHECK((o->answer[12] | o->answer[13] << 8) == error);
	CHECK(o->error == error);
}

int
main(void)
{
	uint8_t sdu[128];
	struct vayu_voice_bb_out o;
	struct vayu_voice_bb_effect e;
	size_t n;

	/* SetAudioPath 0x20: TLV 0x10 downlink, 0x11 uplink (u16 each) */
	{
		const uint8_t t[] = { 0x10, 2, 0, 3, 0, 0x11, 2, 0, 5, 0 };
		n = req(sdu, 0x1234, 0x20, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 0x1234, 0x20, 0);
		CHECK(e.flags == VAYU_VOICE_BB_EFFECT_PATH);
		CHECK(e.has_downlink && e.downlink == 3 && e.has_uplink && e.uplink == 5);
		CHECK(o.indication_length == 0);
	}
	/* only a downlink */
	{
		const uint8_t t[] = { 0x10, 2, 0, 9, 0 };
		n = req(sdu, 1, 0x20, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		CHECK(e.has_downlink && e.downlink == 9 && !e.has_uplink);
	}
	/* SetVolume 0x23: TLV 0x01 {source, gain} */
	{
		const uint8_t t[] = { 0x01, 2, 0, 1, 77 };
		n = req(sdu, 2, 0x23, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 2, 0x23, 0);
		CHECK(e.flags == VAYU_VOICE_BB_EFFECT_VOLUME && e.volume_source == 1 && e.volume_gain == 77);
	}
	/* SetTTY 0x27 */
	{
		const uint8_t t[] = { 0x01, 2, 0, 1, 0 };
		n = req(sdu, 3, 0x27, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 3, 0x27, 0);
		CHECK(e.flags == VAYU_VOICE_BB_EFFECT_TTY && e.tty == 1);
	}
	/* BurstDtmfTone 0x29: success, then the 0x2e indication */
	{
		const uint8_t t[] = { 0x10, 3, 0, 1, '5', 0 };
		n = req(sdu, 4, 0x29, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 4, 0x29, 0);
		CHECK(o.indication_length == 7);
		CHECK(o.indication[0] == 0x04 && o.indication[1] == 0 && o.indication[2] == 0);
		CHECK(o.indication[3] == 0x2e && o.indication[4] == 0 && o.indication[5] == 0 && o.indication[6] == 0);
	}
	/* StartTone 0x2a: success, no indication; StopTone 0x2b: success and AudioToneDone 0x2c */
	{
		const uint8_t t[] = { 0x11, 4, 0, 2, 2, 0, 4 };
		n = req(sdu, 5, 0x2a, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 5, 0x2a, 0);
		CHECK(o.indication_length == 0);
		n = req(sdu, 6, 0x2b, NULL, 0);
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 6, 0x2b, 0);
		CHECK(o.indication_length == 7 && o.indication[3] == 0x2c);
	}
	/* GetVocoderType 0x2f: NOT_SUPPORTED; AudioRateSwitchComplete 0x31, SetCtmMode 0x40: success */
	n = req(sdu, 7, 0x2f, NULL, 0);
	CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
	check_answer(&o, 7, 0x2f, VAYU_VOICE_BB_ERR_NOT_SUPPORTED);
	n = req(sdu, 8, 0x31, NULL, 0);
	CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
	check_answer(&o, 8, 0x31, 0);
	n = req(sdu, 9, 0x40, NULL, 0);
	CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
	check_answer(&o, 9, 0x40, 0);
	/* an unknown message: INVALID_QMI_CMD */
	n = req(sdu, 10, 0x55, NULL, 0);
	CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
	check_answer(&o, 10, 0x55, VAYU_VOICE_BB_ERR_INVALID_QMI_CMD);
	/* a TLV that runs past the message: MALFORMED, no effect */
	{
		const uint8_t t[] = { 0x10, 9, 0, 3, 0 };
		n = req(sdu, 11, 0x20, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 11, 0x20, VAYU_VOICE_BB_ERR_MALFORMED);
		CHECK(e.flags == 0);
	}
	/* a short TLV value is not taken */
	{
		const uint8_t t[] = { 0x10, 1, 0, 3 };
		n = req(sdu, 12, 0x20, t, sizeof(t));
		CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == 0);
		check_answer(&o, 12, 0x20, 0);
		CHECK(e.flags == 0);
	}
	/* not answered at all: too short, not a request, length field wrong */
	CHECK(vayu_voice_bb_audio_handle(sdu, 6, &o, &e) == -1);
	CHECK(vayu_voice_bb_audio_handle(NULL, 0, &o, &e) == -1);
	n = req(sdu, 13, 0x20, NULL, 0);
	sdu[0] = 0x02;
	CHECK(vayu_voice_bb_audio_handle(sdu, n, &o, &e) == -1);
	n = req(sdu, 14, 0x20, NULL, 0);
	CHECK(vayu_voice_bb_audio_handle(sdu, n + 1, &o, &e) == -1);
	/* names */
	CHECK(strcmp(vayu_voice_bb_audio_message_name(0x20), "SetAudioPath") == 0);
	CHECK(strcmp(vayu_voice_bb_audio_message_name(0x99), "unknown") == 0);

	if (failures != 0) {
		fprintf(stderr, "voice-test: %d failure(s)\n", failures);
		return 1;
	}
	printf("voice-test: PASS\n");
	return 0;
}
