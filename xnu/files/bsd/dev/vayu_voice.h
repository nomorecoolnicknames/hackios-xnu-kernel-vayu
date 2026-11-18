/*
 * voice on the audio DSP: the q6voice services (MVM, CVS, CVP) and the APR packets that put a call's
 * TX (microphone) and RX (earpiece/speaker) legs on the AFE ports.
 *
 * Every number and every payload layout below is the one Android's audio kernel sends on the same
 * ADSP family (techpack/audio of the Elite downstream tree: include/dsp/q6voice.h, dsp/q6voice.c;
 * the APR message layout in drivers/soc/qcom/apr.c, Android's q6core.c):
 *
 *   MVM (service 9, APR_SVC_ADSP_MVM)   the "voice manager": sessions, which stream and vocproc
 *                                       belong together, and START/STOP VOICE.
 *   CVS (service 10, APR_SVC_ADSP_CVS)  the "voice stream": the vocoder frames to and from the
 *                                       modem, or -- direction 3 -- a TX+RX loopback inside the DSP
 *                                       that needs no modem and no call.
 *   CVP (service 11, APR_SVC_ADSP_CVP)  the "vocproc": the two AFE legs (TX microphone, RX
 *                                       earpiece/speaker) and the processing between them.
 *
 * The services are on the same GLINK channel as AFE/ASM/ADM ("apr_audio_svc", not "apr_voice_svc":
 * the latter is the modem-side voice services 3..6, apr.h APR_SVC_MVM/CVS/CVP of APR_DOMAIN_MODEM).
 * proof: ipc/apr.c:101-211 (client_id APR_CLIENT_AUDIO for MVM/CVS/CVP of the QDSP6 table),
 * ipc/apr.c:673-686 (packets from APR_DOMAIN_MODEM with services 3..6 are the VOICE client);
 * the stock DT has only "apr_audio_svc" for qcom,apr_tal_rpmsg (recovery-live.dtb:
 * qcom,glink-channels = "apr_audio_svc", qcom,intents = <0x200 0x14>).
 *
 * The packet builders below are the exact bytes this kernel sends: vayu_audio.c fills these
 * structures and hands them to the GLINK channel unchanged, and hosttest/voice_packets_test.c
 * compiles this file on the host and checks every field, offset and size.
 *
 * The file has no kernel dependencies; it needs uint8_t/uint16_t/uint32_t/uint64_t, size_t and
 * memset() from its includer (vayu_audio.c: libkern.h; the host test: <string.h>).
 */
#ifndef _VAYU_VOICE_H_
#define _VAYU_VOICE_H_

/* ---- APR: the 20-byte header every packet starts with (five words: hdr_field/pkt_size, the
 * source service/domain/port, the destination service/domain/port, token, opcode) ---- */
#define VAPR_MSG_TYPE_SEQ_CMD   2
#define VAPR_DOMAIN_ADSP        4
#define VAPR_DOMAIN_APPS        5

struct vayu_apr_header {
	uint16_t field;                 /* kind << 8 | (header bytes / 4) << 4: 0x0250 for this header */
	uint16_t size;                  /* the whole packet, bytes */
	uint8_t from_service;
	uint8_t from_domain;
	uint16_t from_port;             /* a session index we choose, or the handle an answer carries back */
	uint8_t to_service;
	uint8_t to_domain;
	uint16_t to_port;               /* 0 for a create, the session's handle afterwards */
	uint32_t token;                 /* the answer carries it back */
	uint32_t opcode;
} __attribute__((packed));

/* All fields of one packet: the same header builder for every command (host-tested). */
static inline void
vapr_header(struct vayu_apr_header *header, uint8_t service, uint16_t from_port, uint16_t to_port, uint32_t token,
    uint32_t opcode, uint32_t packet_size)
{
	header->field = (uint16_t)((VAPR_MSG_TYPE_SEQ_CMD << 8) | ((sizeof(*header) / 4) << 4));
	header->size = (uint16_t)packet_size;
	header->from_service = service;
	header->from_domain = VAPR_DOMAIN_APPS;
	header->from_port = from_port;
	header->to_service = service;
	header->to_domain = VAPR_DOMAIN_ADSP;
	header->to_port = to_port;
	header->token = token;
	header->opcode = opcode;
}

/* ---- the services (include/ipc/apr.h: APR_SVC_ADSP_MVM/CVS/CVP) ---- */
#define VAPR_SVC_MVM            9
#define VAPR_SVC_CVS            10
#define VAPR_SVC_CVP            11
#define VAPR_SESSION_NAME_LEN   20      /* q6voice.h SESSION_NAME_LEN */

/* ---- MVM commands (q6voice.h:434-490) ---- */
#define VAPR_MVM_CREATE_PASSIVE     0x000110FFu /* a modem-controlled (CS/VoLTE) call */
#define VAPR_MVM_CREATE_FULL        0x000110FEu /* the AP controls the call (VoIP, or the loopback) */
#define VAPR_MVM_ATTACH_STREAM      0x0001123Cu
#define VAPR_MVM_DETACH_STREAM      0x0001123Du
#define VAPR_MVM_ATTACH_VOCPROC     0x0001123Eu
#define VAPR_MVM_DETACH_VOCPROC     0x0001123Fu
#define VAPR_MVM_START_VOICE        0x00011190u
#define VAPR_MVM_STANDBY_VOICE      0x00011191u
#define VAPR_MVM_STOP_VOICE         0x00011192u
#define VAPR_MVM_SET_POLICY_DUAL_CONTROL 0x00011327u /* the modem controls a passive session */

/* ---- CVS commands (q6voice.h:772-800) ---- */
#define VAPR_CVS_CREATE_PASSIVE     0x00011140u
#define VAPR_CVS_CREATE_FULL        0x000110F7u
#define VAPR_CVS_SET_MEDIA_TYPE     0x00011186u

/* ---- CVP commands (q6voice.h:1262-1330) ---- */
#define VAPR_CVP_CREATE_V2          0x000112BFu
#define VAPR_CVP_CREATE_V3          0x00013169u
#define VAPR_CVP_SET_DEVICE_V2      0x000112C6u
#define VAPR_CVP_ENABLE             0x000100C6u
#define VAPR_CVP_DISABLE            0x000110E1u
#define VAPR_CVP_TOPOLOGY_SET_DEV_CHANNELS 0x00013199u
#define VAPR_CVP_TOPOLOGY_COMMIT    0x00013198u

/* ---- common (q6voice.h:455, 9295-9296, 50) ---- */
#define VAPR_DESTROY_SESSION        0x0001003Cu /* APRV2_IBASIC_CMD_DESTROY_SESSION */
#define VAPR_SET_PARAM_V2           0x0001133Du /* VSS_ICOMMON_CMD_SET_PARAM_V2 */
#define VAPR_VERSION_GET            0x00011378u /* VSS_IVERSION_CMD_GET */
#define VAPR_VERSION_RSP            0x00011379u /* VSS_IVERSION_RSP_GET */

/* ---- streams and vocproc: directions, media, networks, topologies (q6voice.h) ---- */
#define VAPR_DIRECTION_TX           0       /* VSS_ISTREAM_CMD_CREATE_FULL_CONTROL_SESSION: TX only */
#define VAPR_DIRECTION_RX           1
#define VAPR_DIRECTION_TX_RX        2       /* the call's two legs */
#define VAPR_DIRECTION_LOOPBACK     3       /* TX and RX loopback: no modem, no call (q6voice.h:988) */

#define VAPR_MEDIA_PCM_8K           0x00010FCBu
#define VAPR_MEDIA_PCM_16K          0x00010FCCu
#define VAPR_MEDIA_PCM_48K          0x00010FD6u
#define VAPR_NETWORK_DEFAULT        0x00010037u
#define VAPR_NETWORK_VOIP           0x00011362u

#define VAPR_TOPOLOGY_NONE          0x00010F70u /* pass-through: no calibration data needed */
#define VAPR_TOPOLOGY_TX_SM_ECNS_V2 0x00010F89u /* the stock uplink topology (EC/NS); needs ACDB cal */
#define VAPR_TOPOLOGY_RX_DEFAULT    0x00010F77u

#define VAPR_PROFILE_NONE           0x0001135Eu /* VSS_ICOMMON_CAL_NETWORK_ID_NONE */
#define VAPR_VOCPROC_EC_INT         0x00010F7Cu /* VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING */
#define VAPR_VOCPROC_EC_EXT         0x00010F7Du
#define VAPR_PORT_NONE              0xFFFFu     /* VSS_IVOCPROC_PORT_ID_NONE */

/* ---- the SET_PARAM_V2 parameters (q6voice.h:880-900, 995) ---- */
#define VAPR_MODULE_CVD_GENERIC             0x0001316Eu /* VSS_MODULE_CVD_GENERIC */
#define VAPR_PARAM_TX_PORT_MEDIA_INFO       0x00013253u /* VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO */
#define VAPR_PARAM_RX_PORT_MEDIA_INFO       0x00013254u
#define VAPR_PARAM_TX_CHANNEL_INFO          0x0001328Eu /* VSS_PARAM_VOCPROC_TX_CHANNEL_INFO */
#define VAPR_PARAM_RX_CHANNEL_INFO          0x0001328Fu

#define VAPR_CHANNELS_MAX   8               /* VSS_NUM_CHANNELS_MAX */

/* ---- one session by name: MVM create (passive/full) and CVS passive create ---- */
struct vapr_session_create {
	struct vayu_apr_header hdr;
	char name[VAPR_SESSION_NAME_LEN];
} __attribute__((packed));

/* ---- CVS full control: direction, the two media types, the network, the name ---- */
struct vapr_cvs_create {
	struct vayu_apr_header hdr;
	uint16_t direction;
	uint32_t enc_media;
	uint32_t dec_media;
	uint32_t network;
	char name[VAPR_SESSION_NAME_LEN];
} __attribute__((packed));

/* ---- CVP full control V2/V3: the two AFE ports, their topologies, the profile, EC mixing ---- */
struct vapr_cvp_create {
	struct vayu_apr_header hdr;
	uint16_t direction;
	uint16_t tx_port;
	uint32_t tx_topology;
	uint16_t rx_port;
	uint32_t rx_topology;
	uint32_t profile;
	uint32_t vocproc_mode;
	uint16_t ec_ref_port;
	char name[VAPR_SESSION_NAME_LEN];
} __attribute__((packed));

/* ---- a command whose only payload is a session handle (attach/detach stream and vocproc) ---- */
struct vapr_handle {
	struct vayu_apr_header hdr;
	uint16_t handle;
} __attribute__((packed));

/* ---- a command with no payload (enable/disable, commit, start/stop, destroy, version) ---- */
struct vapr_bare {
	struct vayu_apr_header hdr;
} __attribute__((packed));

/* ---- SET_POLICY_DUAL_CONTROL: one flag byte (q6voice.h:608-611: bool enable_flag, packed) ---- */
struct vapr_flag {
	struct vayu_apr_header hdr;
	uint8_t flag;
} __attribute__((packed));

/* ---- CVP API 1: TOPOLOGY_SET_DEV_CHANNELS (tx first, then rx: q6voice.h:1524-1540) ---- */
struct vapr_dev_channels {
	struct vayu_apr_header hdr;
	uint16_t tx_channels;
	uint16_t rx_channels;
} __attribute__((packed));

/* ---- CVP API 2+: VOCPROC_*_CHANNEL_INFO as a SET_PARAM_V2 in-band parameter ---- */
struct vapr_channel_info {
	struct vayu_apr_header hdr;
	uint32_t mem_handle;            /* 0: in band */
	uint64_t mem_address;           /* ignored when mem_handle is 0 */
	uint32_t mem_size;              /* sizeof the parameter data: 28 */
	uint32_t module_id;             /* VSS_MODULE_CVD_GENERIC */
	uint32_t param_id;              /* VSS_PARAM_VOCPROC_{TX,RX}_CHANNEL_INFO */
	uint16_t param_size;            /* sizeof the channel info: 16 */
	uint16_t reserved;
	uint32_t num_channels;
	uint32_t bits_per_sample;
	uint8_t channel_mapping[VAPR_CHANNELS_MAX];
} __attribute__((packed));

/* ---- the CVD 2.3+ endpoint media format, RX or TX (q6voice.h:216-240, 306) ---- */
struct vapr_media_format {
	struct vayu_apr_header hdr;
	uint32_t mem_handle;
	uint64_t mem_address;
	uint32_t mem_size;              /* sizeof the parameter data: 32 */
	uint32_t module_id;             /* VSS_MODULE_CVD_GENERIC */
	uint32_t param_id;              /* VSS_PARAM_{TX,RX}_PORT_ENDPOINT_MEDIA_INFO */
	uint16_t param_size;            /* sizeof the media format info: 20 */
	uint16_t reserved;
	uint32_t port_id;               /* the AFE port */
	uint16_t num_channels;
	uint16_t bits_per_sample;
	uint32_t sample_rate;
	uint8_t channel_mapping[VAPR_CHANNELS_MAX];
} __attribute__((packed));

/* ---- the AFE's own loopback (AFE_MODULE_LOOPBACK, AFE_PARAM_ID_LOOPBACK_CONFIG; q6afe.c:4371-4428,
 * apr_audio-v2.h:1661-1705): not APR voice, but the diagnostic that proves the two AFE ports without
 * the DSP's voice services. Sent through AFE_PORT_CMD_SET_PARAM_V2 to the source (TX) port. ---- */
#define VAPR_AFE_MODULE_LOOPBACK        0x00010205u
#define VAPR_AFE_PARAM_LOOPBACK_CONFIG  0x0001020Bu
#define VAPR_AFE_LOOPBACK_MINOR         1
#define VAPR_AFE_LOOPBACK_MODE_DEFAULT  1

struct vapr_afe_loopback {
	uint32_t minor_version;         /* AFE_API_VERSION_LOOPBACK_CONFIG */
	uint16_t dst_port_id;           /* the RX port the TX port's samples go to */
	uint16_t routing_mode;          /* LB_MODE_DEFAULT */
	uint16_t enable;
	uint16_t reserved;
} __attribute__((packed));

/* ---- the builders: fill a packet, return its length in bytes ---- */

static inline uint32_t
vapr_build_session_create(struct vapr_session_create *packet, uint8_t service, uint16_t from_port, uint16_t to_port,
    uint32_t token, uint32_t opcode, const char *name)
{
	uint32_t i;

	memset(packet, 0, sizeof(*packet));
	for (i = 0; i + 1 < VAPR_SESSION_NAME_LEN && name[i] != '\0'; i++) {
		packet->name[i] = name[i];
	}
	vapr_header(&packet->hdr, service, from_port, to_port, token, opcode, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_cvs_create(struct vapr_cvs_create *packet, uint16_t from_port, uint32_t token, uint16_t direction,
    uint32_t enc_media, uint32_t dec_media, uint32_t network, const char *name)
{
	uint32_t i;

	memset(packet, 0, sizeof(*packet));
	packet->direction = direction;
	packet->enc_media = enc_media;
	packet->dec_media = dec_media;
	packet->network = network;
	for (i = 0; i + 1 < VAPR_SESSION_NAME_LEN && name[i] != '\0'; i++) {
		packet->name[i] = name[i];
	}
	vapr_header(&packet->hdr, VAPR_SVC_CVS, from_port, 0, token, VAPR_CVS_CREATE_FULL, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_cvp_create(struct vapr_cvp_create *packet, uint16_t from_port, uint32_t token, uint32_t opcode,
    uint16_t direction, uint16_t tx_port, uint32_t tx_topology, uint16_t rx_port, uint32_t rx_topology,
    uint32_t profile, uint32_t vocproc_mode, uint16_t ec_ref_port, const char *name)
{
	uint32_t i;

	memset(packet, 0, sizeof(*packet));
	packet->direction = direction;
	packet->tx_port = tx_port;
	packet->tx_topology = tx_topology;
	packet->rx_port = rx_port;
	packet->rx_topology = rx_topology;
	packet->profile = profile;
	packet->vocproc_mode = vocproc_mode;
	packet->ec_ref_port = ec_ref_port;
	for (i = 0; i + 1 < VAPR_SESSION_NAME_LEN && name[i] != '\0'; i++) {
		packet->name[i] = name[i];
	}
	vapr_header(&packet->hdr, VAPR_SVC_CVP, from_port, 0, token, opcode, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_handle(struct vapr_handle *packet, uint8_t service, uint16_t from_port, uint16_t to_port, uint32_t token,
    uint32_t opcode, uint16_t handle)
{
	memset(packet, 0, sizeof(*packet));
	packet->handle = handle;
	vapr_header(&packet->hdr, service, from_port, to_port, token, opcode, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_bare(struct vapr_bare *packet, uint8_t service, uint16_t from_port, uint16_t to_port, uint32_t token,
    uint32_t opcode)
{
	memset(packet, 0, sizeof(*packet));
	vapr_header(&packet->hdr, service, from_port, to_port, token, opcode, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_flag(struct vapr_flag *packet, uint8_t service, uint16_t from_port, uint16_t to_port, uint32_t token,
    uint32_t opcode, uint8_t flag)
{
	memset(packet, 0, sizeof(*packet));
	packet->flag = flag;
	vapr_header(&packet->hdr, service, from_port, to_port, token, opcode, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_dev_channels(struct vapr_dev_channels *packet, uint16_t from_port, uint16_t to_port, uint32_t token,
    uint16_t tx_channels, uint16_t rx_channels)
{
	memset(packet, 0, sizeof(*packet));
	packet->tx_channels = tx_channels;
	packet->rx_channels = rx_channels;
	vapr_header(&packet->hdr, VAPR_SVC_CVP, from_port, to_port, token, VAPR_CVP_TOPOLOGY_SET_DEV_CHANNELS,
	    (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_channel_info(struct vapr_channel_info *packet, uint16_t from_port, uint16_t to_port, uint32_t token,
    uint32_t param_id, uint32_t channels, uint32_t bits, const uint8_t mapping[VAPR_CHANNELS_MAX])
{
	memset(packet, 0, sizeof(*packet));
	/* (the DSP's mem_size is the parameter data alone: module, id, size, reserved and the 16 bytes of
	 * channel info; q6voice.c voice_send_cvp_channel_info_v2() sets sizeof(vss_icommon_param_data_channel_info_v2_t)) */
	packet->mem_size = (uint32_t)(sizeof(*packet) - sizeof(struct vayu_apr_header) - 16);
	packet->module_id = VAPR_MODULE_CVD_GENERIC;
	packet->param_id = param_id;
	packet->param_size = (uint16_t)(packet->mem_size - 12);
	packet->num_channels = channels;
	packet->bits_per_sample = bits;
	if (mapping != NULL) {
		memcpy(packet->channel_mapping, mapping, VAPR_CHANNELS_MAX);
	}
	vapr_header(&packet->hdr, VAPR_SVC_CVP, from_port, to_port, token, VAPR_SET_PARAM_V2, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

static inline uint32_t
vapr_build_media_format(struct vapr_media_format *packet, uint16_t from_port, uint16_t to_port, uint32_t token,
    uint32_t param_id, uint32_t port_id, uint32_t channels, uint32_t bits, uint32_t rate,
    const uint8_t mapping[VAPR_CHANNELS_MAX])
{
	memset(packet, 0, sizeof(*packet));
	/* (mem_size is sizeof(vss_icommon_param_data_t): module, id, size, reserved and the 20 bytes of
	 * media format info; q6voice.c voice_send_cvp_media_format_cmd()) */
	packet->mem_size = (uint32_t)(sizeof(*packet) - sizeof(struct vayu_apr_header) - 16);
	packet->module_id = VAPR_MODULE_CVD_GENERIC;
	packet->param_id = param_id;
	packet->param_size = (uint16_t)(packet->mem_size - 12);
	packet->port_id = port_id;
	packet->num_channels = (uint16_t)channels;
	packet->bits_per_sample = (uint16_t)bits;
	packet->sample_rate = rate;
	if (mapping != NULL) {
		memcpy(packet->channel_mapping, mapping, VAPR_CHANNELS_MAX);
	}
	vapr_header(&packet->hdr, VAPR_SVC_CVP, from_port, to_port, token, VAPR_SET_PARAM_V2, (uint32_t)sizeof(*packet));
	return (uint32_t)sizeof(*packet);
}

#endif /* _VAYU_VOICE_H_ */
