/*
 * The HFI 4xx wire format of the Venus / IRIS1 video encoder (vayu, SM8150-AC), as bytes:
 * the packet builders and parsers and the queue table, shared by bsd/dev/vayu_venus.c and
 * tools/venus-hfi (which tests them on the build machine).
 *
 * Nothing kernel-side is in here on purpose: only <stdint.h> and <stdbool.h>, so the same code
 * that writes the firmware's queue is the code the host test checks word for word.
 *
 * The layout is mainline Linux's HFI 4xx (drivers/media/platform/qcom/venus/hfi_cmds.c,
 * hfi_msgs.h, hfi_venus.c:32-94, :747-783; the vendor driver's hfi_packetization.c matches).
 * All words are little-endian host order on arm64; every packet starts with its own size in
 * bytes, and every size is a multiple of four.
 *
 * Tag: FACT = read in those sources; HYPOTHESIS = what this phone's firmware will accept.
 */
#ifndef _SYS_VAYU_VENUS_HFI_H_
#define _SYS_VAYU_VENUS_HFI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Packet types, host -> firmware. */
#define VAYU_HFI_CMD_SYS_INIT                  0x00010001u
#define VAYU_HFI_CMD_SYS_SET_PROPERTY          0x00010005u
#define VAYU_HFI_CMD_SYS_GET_PROPERTY          0x00010006u
#define VAYU_HFI_CMD_SYS_SESSION_INIT          0x00010007u     /* hfi_cmds.h:23 */
#define VAYU_HFI_CMD_SYS_SESSION_END           0x00010008u
#define VAYU_HFI_CMD_SESSION_SET_PROPERTY      0x00011001u     /* hfi_cmds.h:28 */
#define VAYU_HFI_CMD_SESSION_SET_BUFFERS       0x00011002u
#define VAYU_HFI_CMD_SESSION_GET_SEQUENCE_HEADER 0x00011003u
#define VAYU_HFI_CMD_SYS_PING                  0x00210002u
#define VAYU_HFI_CMD_SESSION_LOAD_RESOURCES    0x00211001u     /* hfi_cmds.h:33-40 */
#define VAYU_HFI_CMD_SESSION_START             0x00211002u
#define VAYU_HFI_CMD_SESSION_STOP              0x00211003u
#define VAYU_HFI_CMD_SESSION_EMPTY_BUFFER      0x00211004u
#define VAYU_HFI_CMD_SESSION_FILL_BUFFER       0x00211005u
#define VAYU_HFI_CMD_SESSION_GET_PROPERTY      0x00211009u
#define VAYU_HFI_CMD_SESSION_RELEASE_RESOURCES 0x0021100cu

/* Packet types, firmware -> host. */
#define VAYU_HFI_MSG_SYS_INIT                  0x00020001u
#define VAYU_HFI_MSG_SYS_SESSION_INIT          0x00020006u     /* hfi_msgs.h:14 */
#define VAYU_HFI_MSG_SYS_PROPERTY_INFO         0x0002000au
#define VAYU_HFI_MSG_EVENT_NOTIFY              0x00021001u
#define VAYU_HFI_MSG_SYS_PING_ACK              0x00220002u
#define VAYU_HFI_MSG_SESSION_LOAD_RESOURCES    0x00221001u     /* hfi_msgs.h:30-48 */
#define VAYU_HFI_MSG_SESSION_START             0x00221002u
#define VAYU_HFI_MSG_SESSION_STOP              0x00221003u
#define VAYU_HFI_MSG_SESSION_EMPTY_BUFFER      0x00221007u
#define VAYU_HFI_MSG_SESSION_FILL_BUFFER       0x00221008u
#define VAYU_HFI_MSG_SESSION_PROPERTY_INFO     0x00221009u
#define VAYU_HFI_MSG_SESSION_RELEASE_RESOURCES 0x0022100au
#define VAYU_HFI_MSG_SESSION_END               0x00020007u

/* Properties used by M1. */
#define VAYU_HFI_PROPERTY_SYS_DEBUG_CONFIG     0x00000001u
#define VAYU_HFI_PROPERTY_SYS_CODEC_POWER_PLANE_CTRL 0x00000005u
#define VAYU_HFI_PROPERTY_SYS_IMAGE_VERSION    0x00000006u

/* Session properties (hfi_helper.h:436-460, :482-538). */
#define VAYU_HFI_PROPERTY_PARAM_FRAME_SIZE                       0x00001001u
#define VAYU_HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_INFO   0x00001002u
#define VAYU_HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT       0x00001003u
#define VAYU_HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT            0x00001005u
#define VAYU_HFI_PROPERTY_PARAM_WORK_MODE                        0x00001015u
#define VAYU_HFI_PROPERTY_CONFIG_FRAME_RATE                      0x00002001u
#define VAYU_HFI_PROPERTY_PARAM_VENC_RATE_CONTROL                0x02005004u
#define VAYU_HFI_PROPERTY_CONFIG_VENC_TARGET_BITRATE             0x02006001u
#define VAYU_HFI_PROPERTY_CONFIG_VENC_MAX_BITRATE                0x02006007u
#define VAYU_HFI_PROPERTY_CONFIG_VENC_SYNC_FRAME_SEQUENCE_HEADER 0x02006008u
#define VAYU_HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS             0x00202001u

/* Session domain, codec, colour format and rate control (hfi.h:15-17, hfi_helper.h:237-247, :968, :230-235). */
#define VAYU_HFI_SESSION_TYPE_ENC              1u
#define VAYU_HFI_CODEC_H264                    0x00000002u
#define VAYU_HFI_CODEC_HEVC                    0x00002000u
#define VAYU_HFI_COLOR_FORMAT_NV12             0x00000002u
#define VAYU_HFI_HEVC_PROFILE_MAIN             0x00000001u
#define VAYU_HFI_HEVC_LEVEL_51                 0x00000100u
#define VAYU_HFI_RATE_CONTROL_VBR_CFR          0x01000003u
#define VAYU_HFI_WORK_MODE_2                   0x00000002u

/* Buffer types (hfi_helper.h:393-413): the 4xx numbers. */
#define VAYU_HFI_BUFFER_INPUT                  0x00000001u
#define VAYU_HFI_BUFFER_OUTPUT                 0x00000002u
#define VAYU_HFI_BUFFER_OUTPUT2                0x00000003u
#define VAYU_HFI_BUFFER_INTERNAL_PERSIST       0x00000004u
#define VAYU_HFI_BUFFER_INTERNAL_PERSIST_1     0x00000005u
#define VAYU_HFI_BUFFER_INTERNAL_SCRATCH       0x00000006u
#define VAYU_HFI_BUFFER_INTERNAL_SCRATCH_1     0x00000007u
#define VAYU_HFI_BUFFER_INTERNAL_SCRATCH_2     0x00000008u
#define VAYU_HFI_BUFFER_TYPE_MAX               11u

/* The one buffer-requirement record (hfi_helper.h:1173-1181), eight words. */
#define VAYU_HFI_BUFFER_REQUIREMENT_WORDS      8u

#define VAYU_HFI_DEBUG_MODE_QUEUE              0x00000001u
#define VAYU_HFI_EVENT_SYS_ERROR               0x00000001u
#define VAYU_HFI_EVENT_SESSION_ERROR           0x00000002u
#define VAYU_HFI_VIDEO_ARCH_OX                 0x00000001u

/* The queue table and the three interface queues (mainline hfi_venus.c:32-94). */
#define VAYU_HFI_IFACEQ_NUM                    3
#define VAYU_HFI_QUEUE_CMD                     0
#define VAYU_HFI_QUEUE_MSG                     1
#define VAYU_HFI_QUEUE_DBG                     2
#define VAYU_HFI_PACKET_BYTES                  1024u
#define VAYU_HFI_PACKET_COUNT                  50u
#define VAYU_HFI_PACKET_CLIENTS                16u
#define VAYU_HFI_QUEUE_BYTES                   (VAYU_HFI_PACKET_BYTES * VAYU_HFI_PACKET_COUNT * \
	                                        VAYU_HFI_PACKET_CLIENTS)     /* 0xc8000 */
#define VAYU_HFI_TABLE_BYTES                   0x000000c0u
#define VAYU_HFI_TABLE_HEADER_BYTES            0x00000018u
#define VAYU_HFI_QHDR_OFFSET                   0x00000018u
#define VAYU_HFI_QHDR_BYTES                    0x00000038u
#define VAYU_HFI_QHDR_TYPE                     0x01010000u
#define VAYU_HFI_PACKET_MAX                    1024u    /* the queue's packet size (hfi_venus.c:36-44) */
#define VAYU_HFI_MESSAGE_MAX                   16384u   /* what this side will read out of the message queue */

/*
 * The table header: six dwords. FACT (hfi_queue_table_header): version, size, qhdr0_offset,
 * qhdr_size, num_q, num_active_q.
 */
static inline void
vayu_hfi_table_init(volatile uint32_t *table)
{
	table[0] = 0;
	table[1] = VAYU_HFI_TABLE_BYTES;
	table[2] = VAYU_HFI_QHDR_OFFSET;
	table[3] = VAYU_HFI_QHDR_BYTES;
	table[4] = VAYU_HFI_IFACEQ_NUM;
	table[5] = VAYU_HFI_IFACEQ_NUM;
}

/*
 * One queue header: fourteen dwords, the same ones mainline's venus_set_qhdr_defaults()
 * writes (hfi_venus.c:747-760), with start_addr and the queue type filled in. The debug queue
 * asks for no receive interrupt (hfi_venus.c:826-830): nobody reads it in M1 and a queue
 * interrupt with no reader is a storm.
 */
static inline void
vayu_hfi_queue_header_init(volatile uint32_t *qhdr, uint32_t index, uint32_t start_addr)
{
	qhdr[0] = 1;                                    /* status: valid */
	qhdr[1] = start_addr;
	qhdr[2] = VAYU_HFI_QHDR_TYPE | index;           /* cmd 0, msg 1, dbg 2 */
	qhdr[3] = VAYU_HFI_QUEUE_BYTES / 4;             /* q_size, in dwords */
	qhdr[4] = 0;                                    /* pkt_size */
	qhdr[5] = 0;                                    /* pkt_drop_cnt */
	qhdr[6] = 1;                                    /* rx_wm */
	qhdr[7] = 1;                                    /* tx_wm */
	qhdr[8] = index == VAYU_HFI_QUEUE_DBG ? 0u : 1u;        /* rx_req */
	qhdr[9] = 0;                                    /* tx_req */
	qhdr[10] = 0;                                   /* rx_irq_status */
	qhdr[11] = 0;                                   /* tx_irq_status */
	qhdr[12] = 0;                                   /* read_idx */
	qhdr[13] = 0;                                   /* write_idx */
}

/* ---- the ring indices ------------------------------------------------------------------------- */

/* Dwords in use, from read to write; a ring of qsize dwords keeps one slot free. */
static inline uint32_t
vayu_hfi_ring_used(uint32_t qsize, uint32_t read_idx, uint32_t write_idx)
{
	if (qsize == 0) {
		return 0;
	}
	return write_idx >= read_idx ? write_idx - read_idx : qsize - (read_idx - write_idx);
}

static inline uint32_t
vayu_hfi_ring_free(uint32_t qsize, uint32_t read_idx, uint32_t write_idx)
{
	uint32_t used = vayu_hfi_ring_used(qsize, read_idx, write_idx);

	return qsize > used ? qsize - used : 0;
}

static inline bool
vayu_hfi_ring_index_ok(uint32_t qsize, uint32_t index)
{
	return qsize != 0 && index < qsize;
}

/* Where the write pointer goes after dwords more; qsize must be non-zero. */
static inline uint32_t
vayu_hfi_ring_advance(uint32_t qsize, uint32_t index, uint32_t dwords)
{
	return qsize != 0 ? (index + dwords) % qsize : 0;
}

/* ---- host -> firmware packets ---------------------------------------------------------------- */

/*
 * SYS_INIT: {size 12, HFI_CMD_SYS_INIT, HFI_VIDEO_ARCH_OX} (hfi_cmds.c:14-19, hfi_cmds.h:46-49).
 * Returns the bytes written, 0 if the caller's buffer cannot hold it.
 */
static inline uint32_t
vayu_hfi_sys_init(uint32_t *packet, uint32_t words)
{
	if (packet == NULL || words < 3) {
		return 0;
	}
	packet[0] = 12;
	packet[1] = VAYU_HFI_CMD_SYS_INIT;
	packet[2] = VAYU_HFI_VIDEO_ARCH_OX;
	return 12;
}

/* SYS_SET_PROPERTY, one property: {20 or 24, 0x10005, 1, property, value[, value2]}. */
static inline uint32_t
vayu_hfi_sys_set_property(uint32_t *packet, uint32_t words, uint32_t property,
    const uint32_t *values, uint32_t count)
{
	uint32_t size = 16 + 4 * count;

	if (packet == NULL || values == NULL || count == 0 || count > 2 || words * 4 < size) {
		return 0;
	}
	packet[0] = size;
	packet[1] = VAYU_HFI_CMD_SYS_SET_PROPERTY;
	packet[2] = 1;                                  /* num_properties */
	packet[3] = property;
	for (uint32_t i = 0; i < count; i++) {
		packet[4 + i] = values[i];
	}
	return size;
}

/* The debug property: {24, 0x10005, 1, 1, config, HFI_DEBUG_MODE_QUEUE} (hfi_cmds.c:38-50). */
static inline uint32_t
vayu_hfi_sys_debug_config(uint32_t *packet, uint32_t words, uint32_t config)
{
	const uint32_t values[2] = { config, VAYU_HFI_DEBUG_MODE_QUEUE };

	return vayu_hfi_sys_set_property(packet, words, VAYU_HFI_PROPERTY_SYS_DEBUG_CONFIG,
	           values, 2);
}

/* The codec power plane: {20, 0x10005, 1, 5, enable} (hfi_cmds.c:125-134). */
static inline uint32_t
vayu_hfi_sys_codec_power_plane(uint32_t *packet, uint32_t words, uint32_t enable)
{
	return vayu_hfi_sys_set_property(packet, words,
	           VAYU_HFI_PROPERTY_SYS_CODEC_POWER_PLANE_CTRL, &enable, 1);
}

/* SYS_GET_PROPERTY for the image version: {16, 0x10006, 1, 6} (hfi_cmds.c:154-160). */
static inline uint32_t
vayu_hfi_sys_get_image_version(uint32_t *packet, uint32_t words)
{
	if (packet == NULL || words < 4) {
		return 0;
	}
	packet[0] = 16;
	packet[1] = VAYU_HFI_CMD_SYS_GET_PROPERTY;
	packet[2] = 1;
	packet[3] = VAYU_HFI_PROPERTY_SYS_IMAGE_VERSION;
	return 16;
}

/* SYS_PING: {12, 0x210002, cookie} (hfi_cmds.c:145-152). */
static inline uint32_t
vayu_hfi_sys_ping(uint32_t *packet, uint32_t words, uint32_t cookie)
{
	if (packet == NULL || words < 3) {
		return 0;
	}
	packet[0] = 12;
	packet[1] = VAYU_HFI_CMD_SYS_PING;
	packet[2] = cookie;
	return 12;
}

/* ---- firmware -> host packets ---------------------------------------------------------------- */

/*
 * A packet's own size and type. The size word must agree with the bytes the queue says the
 * packet has: a disagreement is a broken queue, not a packet to guess about.
 */
static inline bool
vayu_hfi_message(const uint32_t *packet, uint32_t size, uint32_t *type)
{
	if (packet == NULL || size < 8 || (size & 3) != 0 || packet[0] != size) {
		return false;
	}
	if (type != NULL) {
		*type = packet[1];
	}
	return true;
}

/*
 * SYS_INIT done: {size, 0x20001, error_type, num_properties, data...}; mainline rejects a
 * non-zero error and a property count of zero (hfi_msgs.c:220-247).
 */
static inline bool
vayu_hfi_sys_init_done(const uint32_t *packet, uint32_t size, uint32_t *error,
    uint32_t *properties)
{
	if (!vayu_hfi_message(packet, size, NULL) || packet[1] != VAYU_HFI_MSG_SYS_INIT ||
	    size < 16) {
		return false;
	}
	if (error != NULL) {
		*error = packet[2];
	}
	if (properties != NULL) {
		*properties = packet[3];
	}
	return true;
}

static inline bool
vayu_hfi_sys_init_ok(const uint32_t *packet, uint32_t size)
{
	uint32_t error = ~0u, properties = 0;

	return vayu_hfi_sys_init_done(packet, size, &error, &properties) &&
	       error == 0 && properties != 0;
}

/*
 * The image version string out of SYS_PROPERTY_INFO {size, 0x2000a, count, property, data...}.
 * The data may begin with its own length word (hfi_helper.h:1091-1094) or be the string
 * itself; both are handled. `text' points into the packet and `length' is its byte count,
 * trailing NULs included -- the caller makes it printable.
 */
static inline bool
vayu_hfi_image_version(const uint32_t *packet, uint32_t size, const uint8_t **text,
    uint32_t *length)
{
	const uint8_t *data;
	uint32_t left, skip = 0, declared;

	if (!vayu_hfi_message(packet, size, NULL) ||
	    packet[1] != VAYU_HFI_MSG_SYS_PROPERTY_INFO || size < 20 ||
	    packet[3] != VAYU_HFI_PROPERTY_SYS_IMAGE_VERSION) {
		return false;
	}
	left = size - 16;
	declared = packet[4];
	if (left >= 4 && declared > 0 && declared <= left - 4) {
		skip = 4;
		left = declared;
	}
	data = (const uint8_t *)&packet[4] + skip;
	if (text != NULL) {
		*text = data;
	}
	if (length != NULL) {
		*length = left;
	}
	return true;
}

/* SYS_PING's ack: {12, 0x220002, cookie}; a different cookie means somebody else's ping. */
static inline bool
vayu_hfi_ping_ack(const uint32_t *packet, uint32_t size, uint32_t cookie)
{
	return vayu_hfi_message(packet, size, NULL) && packet[1] == VAYU_HFI_MSG_SYS_PING_ACK &&
	       size >= 12 && packet[2] == cookie;
}

/* An asynchronous event: {size >= 16, 0x21001, session, event, data...}. */
static inline bool
vayu_hfi_event(const uint32_t *packet, uint32_t size, uint32_t *session, uint32_t *event,
    uint32_t *data0, uint32_t *data1)
{
	if (!vayu_hfi_message(packet, size, NULL) || packet[1] != VAYU_HFI_MSG_EVENT_NOTIFY ||
	    size < 16) {
		return false;
	}
	if (session != NULL) {
		*session = packet[2];
	}
	if (event != NULL) {
		*event = packet[3];
	}
	if (data0 != NULL) {
		*data0 = size >= 20 ? packet[4] : 0;
	}
	if (data1 != NULL) {
		*data1 = size >= 24 ? packet[5] : 0;
	}
	return true;
}

/* ---- sessions: M2 (open) and M3 (one frame) -------------------------------------------------- */

/*
 * SESSION_INIT: {20, 0x10007, session_id, session_domain, session_codec}; mainline
 * pkt_session_init() (hfi_cmds.c:163-176) with session_domain VIDC_SESSION_TYPE_ENC = 1
 * (hfi.h:15-17) and the codec from hfi_helper.h:237,247.
 */
static inline uint32_t
vayu_hfi_session_init(uint32_t *packet, uint32_t words, uint32_t session, uint32_t domain,
    uint32_t codec)
{
	if (packet == NULL || words < 5 || session == 0 || codec == 0) {
		return 0;
	}
	packet[0] = 20;
	packet[1] = VAYU_HFI_CMD_SYS_SESSION_INIT;
	packet[2] = session;
	packet[3] = domain;
	packet[4] = codec;
	return 20;
}

/*
 * SESSION_SET_PROPERTY, one property: {20 + 4*n, 0x11001, session, 1, property, values...}.
 * The layout is hfi_session_set_property_pkt (hfi_cmds.h:101-105): the header is sixteen
 * bytes, then the property word, then its payload (pkt_session_set_property_1x()).
 */
static inline uint32_t
vayu_hfi_session_property(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t property, const uint32_t *values, uint32_t count)
{
	uint32_t size = 20 + 4 * count;

	if (packet == NULL || session == 0 || (count != 0 && values == NULL) || count > 10 ||
	    words * 4 < size) {
		return 0;
	}
	packet[0] = size;
	packet[1] = VAYU_HFI_CMD_SESSION_SET_PROPERTY;
	packet[2] = session;
	packet[3] = 1;                                  /* num_properties */
	packet[4] = property;
	for (uint32_t i = 0; i < count; i++) {
		packet[5 + i] = values[i];
	}
	return size;
}

/* FRAME_SIZE: {buffer_type, width, height} (hfi_helper.h:889-893). */
static inline uint32_t
vayu_hfi_session_frame_size(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t buffer_type, uint32_t width, uint32_t height)
{
	const uint32_t values[3] = { buffer_type, width, height };

	return vayu_hfi_session_property(packet, words, session,
	           VAYU_HFI_PROPERTY_PARAM_FRAME_SIZE, values, 3);
}

/* UNCOMPRESSED_FORMAT_SELECT: {buffer_type, format} (hfi_helper.h:993-996). */
static inline uint32_t
vayu_hfi_session_color_format(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t buffer_type, uint32_t format)
{
	const uint32_t values[2] = { buffer_type, format };

	return vayu_hfi_session_property(packet, words, session,
	           VAYU_HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT, values, 2);
}

/* CONFIG_FRAME_RATE: {buffer_type, framerate in 16.16} (hfi_helper.h:682-685). */
static inline uint32_t
vayu_hfi_session_frame_rate(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t buffer_type, uint32_t fps)
{
	const uint32_t values[2] = { buffer_type, fps << 16 };

	return vayu_hfi_session_property(packet, words, session,
	           VAYU_HFI_PROPERTY_CONFIG_FRAME_RATE, values, 2);
}

/* The encoder's profile and level: {profile, level} (hfi_helper.h:796-799). */
static inline uint32_t
vayu_hfi_session_profile_level(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t profile, uint32_t level)
{
	const uint32_t values[2] = { profile, level };

	return vayu_hfi_session_property(packet, words, session,
	           VAYU_HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT, values, 2);
}

/* CONFIG_VENC_TARGET_BITRATE: {bits per second, layer} (hfi_helper.h:571-574). */
static inline uint32_t
vayu_hfi_session_bitrate(uint32_t *packet, uint32_t words, uint32_t session, uint32_t property,
    uint32_t bitrate, uint32_t layer)
{
	const uint32_t values[2] = { bitrate, layer };

	return vayu_hfi_session_property(packet, words, session, property, values, 2);
}

/* One scalar property (work mode, rate control, sync-frame header): {value}. */
static inline uint32_t
vayu_hfi_session_scalar(uint32_t *packet, uint32_t words, uint32_t session, uint32_t property,
    uint32_t value)
{
	return vayu_hfi_session_property(packet, words, session, property, &value, 1);
}

/* SESSION_LOAD_RESOURCES / START / STOP / RELEASE_RESOURCES / END: {12, type, session}. */
static inline uint32_t
vayu_hfi_session_command(uint32_t *packet, uint32_t words, uint32_t session, uint32_t type)
{
	if (packet == NULL || words < 3 || session == 0) {
		return 0;
	}
	packet[0] = 12;
	packet[1] = type;
	packet[2] = session;
	return 12;
}

/* SESSION_GET_PROPERTY, one property: {20, 0x211009, session, 1, property} (hfi_cmds.h:140-145). */
static inline uint32_t
vayu_hfi_session_get_property(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t property)
{
	if (packet == NULL || words < 5 || session == 0) {
		return 0;
	}
	packet[0] = 20;
	packet[1] = VAYU_HFI_CMD_SESSION_GET_PROPERTY;
	packet[2] = session;
	packet[3] = 1;
	packet[4] = property;
	return 20;
}

/*
 * SESSION_SET_BUFFERS: hfi_session_set_buffers_pkt (hfi_cmds.h:141-149), eight header words
 * then one address per buffer for input/internal, and an {address, extradata} pair per buffer
 * for output (pkt_session_set_buffers(), hfi_cmds.c:182-215).
 */
static inline uint32_t
vayu_hfi_session_set_buffers(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t buffer_type, uint32_t buffer_size, const uint32_t *addresses, uint32_t count)
{
	bool output = buffer_type == VAYU_HFI_BUFFER_OUTPUT || buffer_type == VAYU_HFI_BUFFER_OUTPUT2;
	uint32_t size = 32 + (output ? 8 : 4) * count;

	if (packet == NULL || session == 0 || addresses == NULL || count == 0 || count > 32 ||
	    words * 4 < size) {
		return 0;
	}
	packet[0] = size;
	packet[1] = VAYU_HFI_CMD_SESSION_SET_BUFFERS;
	packet[2] = session;
	packet[3] = buffer_type;
	packet[4] = buffer_size;
	packet[5] = 0;                                  /* extradata_size */
	packet[6] = buffer_size;                        /* min_buffer_size */
	packet[7] = count;
	for (uint32_t i = 0; i < count; i++) {
		if (output) {
			packet[8 + 2 * i] = addresses[i];
			packet[9 + 2 * i] = 0;          /* extradata address */
		} else {
			packet[8 + i] = addresses[i];
		}
	}
	return size;
}

/*
 * SESSION_EMPTY_BUFFER with an uncompressed NV12 frame (the encoder's input):
 * hfi_session_empty_buffer_uncompressed_plane0_pkt (hfi_cmds.h:157-172), sixteen words.
 */
static inline uint32_t
vayu_hfi_session_empty_buffer(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t address, uint32_t alloc_len, uint32_t filled_len, uint32_t tag)
{
	if (packet == NULL || words < 16 || session == 0 || address == 0) {
		return 0;
	}
	packet[0] = 64;
	packet[1] = VAYU_HFI_CMD_SESSION_EMPTY_BUFFER;
	packet[2] = session;
	packet[3] = 0;                                  /* view_id */
	packet[4] = 0;                                  /* time_stamp_hi */
	packet[5] = 0;                                  /* time_stamp_lo */
	packet[6] = 0;                                  /* flags */
	packet[7] = 0;                                  /* mark_target */
	packet[8] = 0;                                  /* mark_data */
	packet[9] = alloc_len;
	packet[10] = filled_len;
	packet[11] = 0;                                 /* offset */
	packet[12] = tag;                               /* input_tag */
	packet[13] = address;                           /* packet_buffer */
	packet[14] = 0;                                 /* extradata_buffer */
	packet[15] = 0;                                 /* data */
	return 64;
}

/*
 * SESSION_FILL_BUFFER: hfi_session_fill_buffer_pkt (hfi_cmds.h:192-202), eleven words.
 * stream_id 0 is HFI_BUFFER_OUTPUT (pkt_session_ftb(), hfi_cmds.c:315-340).
 */
static inline uint32_t
vayu_hfi_session_fill_buffer(uint32_t *packet, uint32_t words, uint32_t session,
    uint32_t stream, uint32_t address, uint32_t alloc_len, uint32_t tag)
{
	if (packet == NULL || words < 11 || session == 0 || address == 0) {
		return 0;
	}
	packet[0] = 44;
	packet[1] = VAYU_HFI_CMD_SESSION_FILL_BUFFER;
	packet[2] = session;
	packet[3] = stream;
	packet[4] = 0;                                  /* offset */
	packet[5] = alloc_len;
	packet[6] = 0;                                  /* filled_len */
	packet[7] = tag;                                /* output_tag */
	packet[8] = address;                            /* packet_buffer */
	packet[9] = 0;                                  /* extradata_buffer */
	packet[10] = 0;                                 /* data */
	return 44;
}

/* ---- session replies ------------------------------------------------------------------------- */

/*
 * The simple done packets: {size, type, session_id, error_type}; the session's id is word 2 and
 * the error word 3 (hfi_msgs.h:120-149). `want' is one of VAYU_HFI_MSG_SESSION_*.
 */
static inline bool
vayu_hfi_session_done(const uint32_t *packet, uint32_t size, uint32_t want, uint32_t *session,
    uint32_t *error)
{
	if (!vayu_hfi_message(packet, size, NULL) || packet[1] != want || size < 16) {
		return false;
	}
	if (session != NULL) {
		*session = packet[2];
	}
	if (error != NULL) {
		*error = packet[3];
	}
	return true;
}

/* SESSION_INIT done: {size, 0x20006, session, error, num_properties, data...} (hfi_msgs.h:104-109). */
static inline bool
vayu_hfi_session_init_done(const uint32_t *packet, uint32_t size, uint32_t *session,
    uint32_t *error, uint32_t *properties)
{
	if (!vayu_hfi_message(packet, size, NULL) || packet[1] != VAYU_HFI_MSG_SYS_SESSION_INIT ||
	    size < 20) {
		return false;
	}
	if (session != NULL) {
		*session = packet[2];
	}
	if (error != NULL) {
		*error = packet[3];
	}
	if (properties != NULL) {
		*properties = packet[4];
	}
	return true;
}

/*
 * SESSION_PROPERTY_INFO: {size, 0x221009, session, num_properties, property, data...}
 * (hfi_msgs.h:222-227). The payload of BUFFER_REQUIREMENTS is an array of eight-word records
 * (hfi_helper.h:1173-1181), indexed by buffer type.
 */
static inline bool
vayu_hfi_session_property_info(const uint32_t *packet, uint32_t size, uint32_t *session,
    uint32_t *property, const uint32_t **data, uint32_t *data_words)
{
	if (!vayu_hfi_message(packet, size, NULL) ||
	    packet[1] != VAYU_HFI_MSG_SESSION_PROPERTY_INFO || size < 20) {
		return false;
	}
	if (session != NULL) {
		*session = packet[2];
	}
	if (property != NULL) {
		*property = packet[4];
	}
	if (data != NULL) {
		*data = &packet[5];
	}
	if (data_words != NULL) {
		*data_words = (size - 20) / 4;
	}
	return true;
}

/* The one buffer-requirement record. */
struct vayu_hfi_buffer_requirement {
	uint32_t type;
	uint32_t size;
	uint32_t region_size;
	uint32_t hold_count;
	uint32_t count_min;
	uint32_t count_actual;
	uint32_t contiguous;
	uint32_t alignment;
};

static inline bool
vayu_hfi_buffer_requirement(const uint32_t *data, uint32_t data_words, uint32_t index,
    struct vayu_hfi_buffer_requirement *out)
{
	uint32_t at = index * VAYU_HFI_BUFFER_REQUIREMENT_WORDS;
	const uint32_t *w;

	if (data == NULL || out == NULL ||
	    (uint64_t)(index + 1) * VAYU_HFI_BUFFER_REQUIREMENT_WORDS > data_words) {
		return false;
	}
	w = &data[at];
	out->type = w[0];
	out->size = w[1];
	out->region_size = w[2];
	out->hold_count = w[3];
	out->count_min = w[4];
	out->count_actual = w[5];
	out->contiguous = w[6];
	out->alignment = w[7];
	return true;
}

/* The encoder's compressed FILL_BUFFER done, hfi_msg_session_fbd_compressed_pkt (hfi_msgs.h:190-210). */
struct vayu_hfi_fill_done {
	uint32_t session;
	uint32_t timestamp_hi;
	uint32_t timestamp_lo;
	uint32_t error;
	uint32_t flags;
	uint32_t offset;
	uint32_t alloc_len;
	uint32_t filled_len;
	uint32_t input_tag;
	uint32_t output_tag;
	uint32_t picture_type;
	uint32_t packet_buffer;
	uint32_t extradata_buffer;
};

static inline bool
vayu_hfi_session_fill_done(const uint32_t *packet, uint32_t size, struct vayu_hfi_fill_done *out)
{
	if (!vayu_hfi_message(packet, size, NULL) ||
	    packet[1] != VAYU_HFI_MSG_SESSION_FILL_BUFFER || size < 72) {
		return false;
	}
	if (out != NULL) {
		out->session = packet[2];
		out->timestamp_hi = packet[3];
		out->timestamp_lo = packet[4];
		out->error = packet[5];
		out->flags = packet[6];
		out->offset = packet[10];
		out->alloc_len = packet[11];
		out->filled_len = packet[12];
		out->input_tag = packet[13];
		out->output_tag = packet[14];
		out->picture_type = packet[15];
		out->packet_buffer = packet[16];
		out->extradata_buffer = packet[17];
	}
	return true;
}

/* EMPTY_BUFFER done: {size, 0x221007, session, error, offset, filled_len, input_tag, buffer}. */
static inline bool
vayu_hfi_session_empty_done(const uint32_t *packet, uint32_t size, uint32_t *session,
    uint32_t *error, uint32_t *filled_len, uint32_t *buffer)
{
	if (!vayu_hfi_message(packet, size, NULL) ||
	    packet[1] != VAYU_HFI_MSG_SESSION_EMPTY_BUFFER || size < 32) {
		return false;
	}
	if (session != NULL) {
		*session = packet[2];
	}
	if (error != NULL) {
		*error = packet[3];
	}
	if (filled_len != NULL) {
		*filled_len = packet[5];
	}
	if (buffer != NULL) {
		*buffer = packet[7];
	}
	return true;
}

#endif /* _SYS_VAYU_VENUS_HFI_H_ */
