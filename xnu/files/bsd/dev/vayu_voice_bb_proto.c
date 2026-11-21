/*
 * The protocol core of Apple's QMI Audio Service (0xE1) as the vayu kernel answers it for
 * CommCenter: parse one request SDU, build its response SDU and the one indication that follows
 * it (if any), and say what the request asks of the call's audio path.  sys/vayu_voice_bb.h has
 * the message table, where it was read from, and why this service is answered locally.
 *
 * No kernel dependencies: tools/voice-test compiles this file on the host.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <sys/vayu_voice_bb.h>

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* A TLV of the request's list, or NULL; `*value_length' its length. */
static const uint8_t *
tlv_find(const uint8_t *tlvs, size_t length, uint8_t type, uint16_t *value_length)
{
	size_t at = 0;

	while (at + 3 <= length) {
		uint8_t t = tlvs[at];
		uint16_t l = get16(&tlvs[at + 1]);

		if (at + 3 + l > length) {
			return NULL;            /* (a TLV runs past the message: the list is malformed) */
		}
		if (t == type) {
			*value_length = l;
			return &tlvs[at + 3];
		}
		at += 3 + (size_t)l;
	}
	return NULL;
}

/* The whole list must walk to its end exactly. */
static bool
tlvs_well_formed(const uint8_t *tlvs, size_t length)
{
	size_t at = 0;

	while (at + 3 <= length) {
		at += 3 + (size_t)get16(&tlvs[at + 1]);
	}
	return at == length;
}

/* flags, transaction, message, then the TLV list of `tlv_length' bytes */
static size_t
sdu_header(uint8_t *out, uint8_t flags, uint16_t transaction, uint16_t message, uint16_t tlv_length)
{
	out[0] = flags;
	put16(&out[1], transaction);
	put16(&out[3], message);
	put16(&out[5], tlv_length);
	return VAYU_VOICE_BB_SDU_HEADER;
}

static void
answer(struct vayu_voice_bb_out *out, uint16_t transaction, uint16_t message, uint16_t error)
{
	uint8_t *p = out->answer;
	size_t at = sdu_header(p, VAYU_VOICE_BB_FLAG_RESPONSE, transaction, message, 7);

	p[at++] = 0x02;                 /* the result TLV: {u16 result, u16 error} */
	put16(&p[at], 4);
	at += 2;
	put16(&p[at], error != 0 ? 1 : 0);
	at += 2;
	put16(&p[at], error);
	at += 2;
	out->answer_length = at;
	out->error = error;
}

/* An indication with no TLVs: the transaction of an indication is 0. */
static void
indication(struct vayu_voice_bb_out *out, uint16_t message)
{
	out->indication_length = sdu_header(out->indication, VAYU_VOICE_BB_FLAG_INDICATION, 0, message, 0);
}

const char *
vayu_voice_bb_audio_message_name(uint16_t message)
{
	switch (message) {
	case VAYU_VOICE_BB_AUDIO_SET_AUDIO_PATH: return "SetAudioPath";
	case VAYU_VOICE_BB_AUDIO_SET_VOLUME: return "SetVolume";
	case VAYU_VOICE_BB_AUDIO_SET_TTY: return "SetTTY";
	case VAYU_VOICE_BB_AUDIO_BURST_DTMF: return "BurstDtmfTone";
	case VAYU_VOICE_BB_AUDIO_START_TONE: return "StartTone";
	case VAYU_VOICE_BB_AUDIO_STOP_TONE: return "StopTone";
	case VAYU_VOICE_BB_AUDIO_TONE_DONE_IND: return "AudioToneDone";
	case VAYU_VOICE_BB_AUDIO_INTERFACE_IND: return "AudioInterfaceStatus";
	case VAYU_VOICE_BB_AUDIO_BURST_DTMF_IND: return "BurstDtmfTone (done)";
	case VAYU_VOICE_BB_AUDIO_GET_VOCODER_TYPE: return "GetVocoderType";
	case VAYU_VOICE_BB_AUDIO_VOCODER_INFO_IND: return "VocoderTypeInfo";
	case VAYU_VOICE_BB_AUDIO_RATE_SWITCH_DONE: return "AudioRateSwitchComplete";
	case VAYU_VOICE_BB_AUDIO_STATISTICS_IND: return "AudioStatistics";
	case VAYU_VOICE_BB_AUDIO_DISTORTION_IND: return "AudioDistortion";
	case VAYU_VOICE_BB_AUDIO_PS_STATISTICS_IND: return "AudioPSCallStatistics";
	case VAYU_VOICE_BB_AUDIO_SET_CTM: return "SetCtmMode";
	}
	return "unknown";
}

int
vayu_voice_bb_audio_handle(const uint8_t *sdu, size_t length, struct vayu_voice_bb_out *out,
    struct vayu_voice_bb_effect *effect)
{
	const uint8_t *tlvs, *v;
	uint16_t transaction, message, tlv_length, vl;

	memset(out, 0, sizeof(*out));
	memset(effect, 0, sizeof(*effect));
	if (sdu == NULL || length < VAYU_VOICE_BB_SDU_HEADER || sdu[0] != VAYU_VOICE_BB_FLAG_REQUEST) {
		return -1;
	}
	transaction = get16(&sdu[1]);
	message = get16(&sdu[3]);
	tlv_length = get16(&sdu[5]);
	if ((size_t)tlv_length != length - VAYU_VOICE_BB_SDU_HEADER) {
		return -1;
	}
	tlvs = &sdu[VAYU_VOICE_BB_SDU_HEADER];
	out->message = message;
	if (!tlvs_well_formed(tlvs, tlv_length)) {
		answer(out, transaction, message, VAYU_VOICE_BB_ERR_MALFORMED);
		return 0;
	}

	switch (message) {
	case VAYU_VOICE_BB_AUDIO_SET_AUDIO_PATH:
		if ((v = tlv_find(tlvs, tlv_length, 0x10, &vl)) != NULL && vl >= 2) {
			effect->downlink = get16(v);
			effect->has_downlink = true;
		}
		if ((v = tlv_find(tlvs, tlv_length, 0x11, &vl)) != NULL && vl >= 2) {
			effect->uplink = get16(v);
			effect->has_uplink = true;
		}
		if (effect->has_downlink || effect->has_uplink) {
			effect->flags |= VAYU_VOICE_BB_EFFECT_PATH;
		}
		answer(out, transaction, message, 0);
		break;
	case VAYU_VOICE_BB_AUDIO_SET_VOLUME:
		if ((v = tlv_find(tlvs, tlv_length, 0x01, &vl)) != NULL && vl >= 2) {
			effect->volume_source = v[0];
			effect->volume_gain = v[1];
			effect->flags |= VAYU_VOICE_BB_EFFECT_VOLUME;
		}
		answer(out, transaction, message, 0);
		break;
	case VAYU_VOICE_BB_AUDIO_SET_TTY:
		if ((v = tlv_find(tlvs, tlv_length, 0x01, &vl)) != NULL && vl >= 1) {
			effect->tty = v[0];
			effect->flags |= VAYU_VOICE_BB_EFFECT_TTY;
		}
		answer(out, transaction, message, 0);
		break;
	case VAYU_VOICE_BB_AUDIO_BURST_DTMF:
		answer(out, transaction, message, 0);
		indication(out, VAYU_VOICE_BB_AUDIO_BURST_DTMF_IND);
		break;
	case VAYU_VOICE_BB_AUDIO_STOP_TONE:
		answer(out, transaction, message, 0);
		indication(out, VAYU_VOICE_BB_AUDIO_TONE_DONE_IND);
		break;
	case VAYU_VOICE_BB_AUDIO_START_TONE:
	case VAYU_VOICE_BB_AUDIO_RATE_SWITCH_DONE:
	case VAYU_VOICE_BB_AUDIO_SET_CTM:
		answer(out, transaction, message, 0);
		break;
	case VAYU_VOICE_BB_AUDIO_GET_VOCODER_TYPE:
		answer(out, transaction, message, VAYU_VOICE_BB_ERR_NOT_SUPPORTED);
		break;
	default:
		answer(out, transaction, message, VAYU_VOICE_BB_ERR_INVALID_QMI_CMD);
		break;
	}
	return 0;
}
