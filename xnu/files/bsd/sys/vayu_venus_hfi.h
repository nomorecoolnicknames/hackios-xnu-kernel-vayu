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
#define VAYU_HFI_CMD_SYS_PING                  0x00210002u

/* Packet types, firmware -> host. */
#define VAYU_HFI_MSG_SYS_INIT                  0x00020001u
#define VAYU_HFI_MSG_SYS_PROPERTY_INFO         0x0002000au
#define VAYU_HFI_MSG_EVENT_NOTIFY              0x00021001u
#define VAYU_HFI_MSG_SYS_PING_ACK              0x00220002u

/* Properties used by M1. */
#define VAYU_HFI_PROPERTY_SYS_DEBUG_CONFIG     0x00000001u
#define VAYU_HFI_PROPERTY_SYS_CODEC_POWER_PLANE_CTRL 0x00000005u
#define VAYU_HFI_PROPERTY_SYS_IMAGE_VERSION    0x00000006u

#define VAYU_HFI_DEBUG_MODE_QUEUE              0x00000001u
#define VAYU_HFI_EVENT_SYS_ERROR               0x00000001u
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

#endif /* _SYS_VAYU_VENUS_HFI_H_ */
