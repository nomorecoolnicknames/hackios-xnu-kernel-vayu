/*
 * The 4-byte QMAP/MAP header of the modem's data path (vayu, bsd/dev/vayu_cell_net.c).
 *
 * This is the v1 MAP header the modem uses after WDA Set Data Format asks for QMAP aggregation
 * (bsd/dev/vayu_cell_data.c): Qualcomm's QMAP header, written down in Linux's
 * include/linux/if_rmnet.h as
 *
 *     struct rmnet_map_header { u8 flags; u8 mux_id; __be16 pkt_len; };
 *
 * and filled the same way by qcom-usb-kernel-drivers' qmap.h (cd_rsvd_pad, mux_id, pkt_len).
 *
 * Bits 0..5 of `flags` are the padding, bit 6 "next header", bit 7 "command"; `mux_id` follows;
 * `pkt_len` is big-endian and counts the payload *including* the padding but not the 4-byte
 * header (Documentation/networking/device_drivers/cellular/qualcomm/rmnet.rst, "MAP packet v1").
 * That one multi-byte field is big-endian, unlike the WDS address words, which vayu_cell_data.c
 * prints with shifts and therefore holds in host order.
 *
 * Everything here is pure: it reads only the bytes handed to it, never more than `available`, and
 * decides nothing about which mux id is useful. The boundary (bsd/dev/vayu_cell_net.c, and the IPA
 * receive path to come) accepts mux 1 and rejects command frames; a command frame still decodes so
 * that the receiver can count it and say why it was dropped. The `next` bit is reported, not
 * followed: this port asks WDA for no checksum offload, so the only header there can be is one
 * this code does not understand, and its payload will fail the IPv4 nibble check at the boundary.
 *
 * No unbounded reads: the header needs 4 bytes, the payload length is a 16-bit bound, and a
 * packet whose header length or padding does not fit in `available` is refused.
 *
 * The test of this file is tools/cell-net-test (host, ASan/UBSan).
 */
#ifndef _SYS_VAYU_CELL_QMAP_H_
#define _SYS_VAYU_CELL_QMAP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VAYU_QMAP_HEADER_LEN    4
#define VAYU_QMAP_PAD_MAX       0x3f
#define VAYU_QMAP_LENGTH_MAX    0xffff
#define VAYU_QMAP_MUX_MAX       0xff
#define VAYU_QMAP_FLAG_NEXT     0x40
#define VAYU_QMAP_FLAG_COMMAND  0x80

struct vayu_qmap_header {
	uint8_t  pad_len;       /* bytes of padding after the payload */
	bool     next_header;   /* bit 6: another header follows the MAP header */
	bool     command;       /* bit 7: a MAP command, not a data packet */
	uint16_t length;        /* payload plus padding, not the 4-byte header */
	uint8_t  mux_id;
};

/* Decode the header at `packet`. Only the 4 header bytes are looked at. */
static inline bool
vayu_qmap_header_decode(const uint8_t *packet, size_t available,
    struct vayu_qmap_header *header)
{
	uint8_t flags;

	if (packet == NULL || header == NULL || available < VAYU_QMAP_HEADER_LEN) {
		return false;
	}
	flags = packet[0];
	header->pad_len = flags & VAYU_QMAP_PAD_MAX;
	header->next_header = (flags & VAYU_QMAP_FLAG_NEXT) != 0;
	header->command = (flags & VAYU_QMAP_FLAG_COMMAND) != 0;
	header->mux_id = packet[1];
	header->length = (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
	return true;
}

/* Build a header; refuses a padding field that does not fit in its six bits. */
static inline bool
vayu_qmap_header_encode(uint8_t *packet, const struct vayu_qmap_header *header)
{
	if (packet == NULL || header == NULL || header->pad_len > VAYU_QMAP_PAD_MAX) {
		return false;
	}
	packet[0] = (uint8_t)(header->pad_len |
	    (header->next_header ? VAYU_QMAP_FLAG_NEXT : 0) |
	    (header->command ? VAYU_QMAP_FLAG_COMMAND : 0));
	packet[1] = header->mux_id;
	packet[2] = (uint8_t)(header->length >> 8);
	packet[3] = (uint8_t)header->length;
	return true;
}

/*
 * Decode one element of a MAP aggregation: the header, then `length` payload bytes, of which the
 * last `pad_len` are padding. The element must fit in `available`; bytes past it belong to the
 * next element and are left for the caller. `payload` points into the caller's buffer, so it is
 * valid exactly as long as that buffer is.
 *
 * An empty payload (length 0, or all of the length is padding) is refused: the receiver has
 * nothing to hand to IP, and Linux's rmnet_map_deaggregate() refuses `pkt_len == 0` for the same
 * reason.
 */
static inline bool
vayu_qmap_packet_decode(const uint8_t *packet, size_t available,
    struct vayu_qmap_header *header, const uint8_t **payload,
    size_t *payload_length)
{
	size_t total, data_length;

	if (!vayu_qmap_header_decode(packet, available, header)) {
		return false;
	}
	if (header->length == 0 || header->pad_len > header->length) {
		return false;
	}
	total = VAYU_QMAP_HEADER_LEN + (size_t)header->length;
	if (total > available) {
		return false;
	}
	data_length = (size_t)header->length - header->pad_len;
	if (data_length == 0) {
		return false;
	}
	if (payload != NULL) {
		*payload = packet + VAYU_QMAP_HEADER_LEN;
	}
	if (payload_length != NULL) {
		*payload_length = data_length;
	}
	return true;
}

/*
 * Walk a whole aggregation, one element per call. On success `*cursor` and `*remaining` advance
 * past the element (header + payload + padding); on failure neither is changed, so the caller
 * can drop the rest without having to reason about a partial step.
 */
static inline bool
vayu_qmap_next(const uint8_t **cursor, size_t *remaining,
    struct vayu_qmap_header *header, const uint8_t **payload,
    size_t *payload_length)
{
	size_t step, available;

	if (cursor == NULL || remaining == NULL || header == NULL) {
		return false;
	}
	available = *remaining;
	if (!vayu_qmap_packet_decode(*cursor, available, header, payload,
	    payload_length)) {
		return false;
	}
	step = VAYU_QMAP_HEADER_LEN + (size_t)header->length;
	*cursor += step;
	*remaining = available - step;
	return true;
}

#endif /* _SYS_VAYU_CELL_QMAP_H_ */
