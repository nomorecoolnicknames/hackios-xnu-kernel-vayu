/*
 * vayu voice, the baseband side: the QMI services CommCenter's call module opens that the
 * vayu modem (SM8150's MPSS) has not got, answered by the kernel itself behind the QMux bridge
 * (bsd/dev/vayu_qmux.c, a5's).  Today that is one service, Apple's QMI Audio Service 0xE1.
 *
 * Why it exists.  On an iPhone with a Qualcomm modem, CommCenter's call module drives the call
 * audio of the *modem* over a vendor QMI service of Apple's firmware: libCommCenterMCommandDrivers'
 * QMICallAudioDriver, QMIAudioRoutingCommandDriver and QMIAudioCommandDriver each open a client of
 * qmi::ServiceType 0xe1 ("audio"; QMIClientPool::requestClient / qmi::Client::createWithQueue with
 * `mov w1, #0xe1`, 20F66 cache 0x1f0e59c84, 0x1f0ed02e0, 0x1f0e0444c).  The VOICE service 0x09 of
 * the same module is standard QMI (Dial 0x20, End 0x21, all-call-status 0x2e, Bind 0x44: the
 * qmi::Response<32,...>/Indication<46,...> templates of the same library) and goes to the modem
 * through the bridge unchanged.  The audio service does not exist on vayu: there the modem's
 * vocoder talks to the audio DSP (q6voice: MVM/CVS/CVP, bsd/dev/vayu_audio.c), not to an I2S port
 * of the application processor.  So the kernel answers 0xE1 and turns what CommCenter asks of it
 * into what the DSP path needs: the route (SetAudioPath) and, later, the volume.
 *
 * The messages (all read from the 20F66 dyld cache, libCommCenterMCommandDrivers.dylib; the ids
 * are the immediates passed to qmi::MutableMessageBase(unsigned short) or to
 * QMIClientPool::registerUnsolicitedHandler):
 *
 *   request  0x20 SetAudioPath        TLV 0x10 downlink path (u16), 0x11 uplink path (u16)
 *   request  0x23 SetVolume           TLV 0x01 {u8 source, u8 gain}
 *   request  0x27 SetTTY              TLV 0x01 {u8 tty, u8 0}
 *   request  0x29 BurstDtmfTone       local DTMF feedback (the network's DTMF is VOICE's)
 *   request  0x2a StartTone           TLV 0x11 call-progress tone info
 *   request  0x2b StopTone
 *   request  0x2f GetVocoderType      the modem's vocoder and its I2S rate
 *   request  0x31 AudioRateSwitchComplete
 *   request  0x40 SetCtmMode
 *   ind      0x2c AudioToneDone       (TLV 0x11 tone id)
 *   ind      0x2d AudioInterfaceStatus
 *   ind      0x2e BurstDtmfTone       "DTMF Tones finished playing": CommCenter then plays the rest
 *   ind      0x30 VocoderTypeInfo
 *   ind      0x35/0x36/0x37 statistics, distortion, PS call statistics
 *
 * What the kernel answers (bsd/dev/vayu_voice_bb_proto.c):
 *   - every known request: QMI success (result TLV 0x02 = {0, 0});
 *   - GetVocoderType: QMI_ERR_NOT_SUPPORTED -- there is no vocoder on an I2S link to report, and
 *     no number here is made up;
 *   - an unknown request: QMI_ERR_INVALID_QMI_CMD;
 *   - BurstDtmfTone is followed by its 0x2e indication and StopTone by 0x2c, so that CommCenter's
 *     local DTMF queue and tone state move on (no tone is played: there is no AP path to play it
 *     into while the DSP owns the call).
 *
 * The bridge's side (what the CoreTelephony bridge calls):
 *
 *   vayu_voice_bb_local(service)       true if this module answers that service (0xE1, and only
 *                                      with vayu_call_bb=1); the bridge then allocates QMux client
 *                                      ids for it in its control service as for any other.
 *   vayu_voice_bb_request(service, client, sdu, length, answer, size, &answer_length)
 *                                      one QMI SDU (flags, transaction, message, length, TLVs: the
 *                                      bytes after the QMux header) of a request; the response SDU
 *                                      is written to `answer' (0 returned) for the bridge to frame
 *                                      and hand back.  Never blocks; may be called with the
 *                                      bridge's lock held.
 *   vayu_voice_bb_attach(deliver)      the bridge's way back for indications: deliver(service,
 *                                      client, sdu, length) is called from a thread call of this
 *                                      module (never from inside vayu_voice_bb_request), with no
 *                                      lock of this module held.
 *   vayu_voice_bb_release(service, client)  the client id was released.
 *
 * Boot-args (bsd/dev/vayu_voice.c):
 *   vayu_call_bb=1        answer the audio service 0xE1 (default 0: vayu_voice_bb_local() is false
 *                         and the bridge refuses 0xE1 as it refuses every unknown service)
 *   vayu_call_bb_trace=1  one log line per request (message name, the path/volume values; never
 *                         a number -- this service carries none)
 */
#ifndef _SYS_VAYU_VOICE_BB_H_
#define _SYS_VAYU_VOICE_BB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VAYU_VOICE_BB_SVC_AUDIO                 0xe1

#define VAYU_VOICE_BB_AUDIO_SET_AUDIO_PATH      0x0020
#define VAYU_VOICE_BB_AUDIO_SET_VOLUME          0x0023
#define VAYU_VOICE_BB_AUDIO_SET_TTY             0x0027
#define VAYU_VOICE_BB_AUDIO_BURST_DTMF          0x0029
#define VAYU_VOICE_BB_AUDIO_START_TONE          0x002a
#define VAYU_VOICE_BB_AUDIO_STOP_TONE           0x002b
#define VAYU_VOICE_BB_AUDIO_TONE_DONE_IND       0x002c
#define VAYU_VOICE_BB_AUDIO_INTERFACE_IND       0x002d
#define VAYU_VOICE_BB_AUDIO_BURST_DTMF_IND      0x002e
#define VAYU_VOICE_BB_AUDIO_GET_VOCODER_TYPE    0x002f
#define VAYU_VOICE_BB_AUDIO_VOCODER_INFO_IND    0x0030
#define VAYU_VOICE_BB_AUDIO_RATE_SWITCH_DONE    0x0031
#define VAYU_VOICE_BB_AUDIO_STATISTICS_IND      0x0035
#define VAYU_VOICE_BB_AUDIO_DISTORTION_IND      0x0036
#define VAYU_VOICE_BB_AUDIO_PS_STATISTICS_IND   0x0037
#define VAYU_VOICE_BB_AUDIO_SET_CTM             0x0040

/* QMI SDU control flags (the first byte after the QMux header of a service message) */
#define VAYU_VOICE_BB_FLAG_REQUEST              0x00
#define VAYU_VOICE_BB_FLAG_RESPONSE             0x02
#define VAYU_VOICE_BB_FLAG_INDICATION           0x04

/* QMI error codes used in answers */
#define VAYU_VOICE_BB_ERR_MALFORMED             0x0001
#define VAYU_VOICE_BB_ERR_INVALID_QMI_CMD       0x0047
#define VAYU_VOICE_BB_ERR_NOT_SUPPORTED         0x005e

#define VAYU_VOICE_BB_SDU_HEADER                7       /* flags, transaction (2), message (2), length (2) */
#define VAYU_VOICE_BB_ANSWER_MAX                64
#define VAYU_VOICE_BB_IND_MAX                   32

/* What a request asks of the call's audio path (the proto fills it, the kernel acts on it). */
#define VAYU_VOICE_BB_EFFECT_PATH               0x01u
#define VAYU_VOICE_BB_EFFECT_VOLUME             0x02u
#define VAYU_VOICE_BB_EFFECT_TTY                0x04u

struct vayu_voice_bb_effect {
	uint32_t flags;
	uint16_t downlink, uplink;      /* SetAudioPath: the QMI path values, as CommCenter sends them */
	uint8_t volume_source, volume_gain;
	uint8_t tty;
	bool has_downlink, has_uplink;
};

struct vayu_voice_bb_out {
	uint8_t answer[VAYU_VOICE_BB_ANSWER_MAX];
	size_t answer_length;
	uint8_t indication[VAYU_VOICE_BB_IND_MAX];     /* one indication to send after the answer, or none */
	size_t indication_length;
	uint16_t message;               /* the request's message id */
	uint16_t error;                 /* the QMI error put in the answer (0: success) */
};

/*
 * The protocol core (bsd/dev/vayu_voice_bb_proto.c; host-tested by tools/voice-test).  Returns 0
 * when `out' holds an answer, -1 when the SDU is not a request this core can answer at all (too
 * short, not a request, its length field disagrees with the bytes): the bridge then answers with
 * its own QMI error, as for any malformed frame.
 */
int vayu_voice_bb_audio_handle(const uint8_t *sdu, size_t length, struct vayu_voice_bb_out *out,
    struct vayu_voice_bb_effect *effect);
const char *vayu_voice_bb_audio_message_name(uint16_t message);

#ifdef KERNEL
typedef void (*vayu_voice_bb_deliver_t)(uint8_t service, uint8_t client, const uint8_t *sdu, size_t length);

bool vayu_voice_bb_local(uint8_t service);
int vayu_voice_bb_request(uint8_t service, uint8_t client, const uint8_t *sdu, size_t length, uint8_t *answer,
    size_t answer_size, size_t *answer_length);
void vayu_voice_bb_attach(vayu_voice_bb_deliver_t deliver);
void vayu_voice_bb_release(uint8_t service, uint8_t client);
#endif

#endif /* _SYS_VAYU_VOICE_BB_H_ */
