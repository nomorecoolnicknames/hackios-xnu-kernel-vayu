/*
 * HTC frame header parser for the W15-A HTC_MSG_READY proof.
 *
 * Freestanding (no XNU header, no allocation, no I/O): the same file is built
 * into the kernel and into the host tests.
 *
 * Facts and where they come from:
 *   - The HTC header is 8 bytes: eid, flags, __le16 len, trailer_len,
 *     control_byte1/seq, pad0, pad1 (Linux ath10k htc.h:64-77).
 *   - The payload of an ep0 control message starts with
 *     struct ath10k_ath10k_htc_msg_hdr { __le16 message_id; }, followed by the
 *     union; ath10k_htc_ready is { __le16 credit_count; __le16 credit_size;
 *     u8 max_endpoints; u8 pad0; } (htc.h:118-133).  So a READY payload is
 *     message_id at 0, credits at 2, credit size at 4, endpoints at 6.
 *   - ath10k_htc_wait_target() requires at least sizeof(hdr) + sizeof(ready) =
 *     8 payload bytes, message_id == ATH10K_HTC_MSG_READY_ID (1), and both
 *     credits and credit size non-zero (htc.c:624-650).  The extended READY
 *     form adds htc_version and max_msgs_per_htc_bundle when the payload is at
 *     least sizeof(hdr) + sizeof(ready_ext) = 12 bytes (htc.c:655-663).
 *   - The trailer is present when flags bit 1 is set; trailer_len must be at
 *     least one record header (4 bytes) and no larger than the payload
 *     (htc.c:413-430).
 *   - The HTC control downlink of the WCN3990 service map is pipe 2 = CE2
 *     (target_service_to_ce_map_wlan, SVC 0x0001 PIPEDIR_IN pipe 2; flame
 *     radio.c:382 ep[0].dl = 2).  A READY-looking frame on another CE is not a
 *     READY for this stage.
 *
 * Copyright (c) 2026 The Redwood Project.  APSL 2.0.
 */

#ifndef _SYS_VAYU_WIFI_HTC_H_
#define _SYS_VAYU_WIFI_HTC_H_

#include <stdbool.h>
#include <stdint.h>

#define VAYU_HTC_HDR_SIZE        8U
#define VAYU_HTC_MAX_LEN         4096U
#define VAYU_HTC_CTRL_MAX        12U      /* enough for READY / READY_EXT / CONNECT_RESP */
#define VAYU_HTC_EP_COUNT        8U
#define VAYU_HTC_EP0             0U
#define VAYU_HTC_MSG_READY       1U
#define VAYU_HTC_FLAG_TRAILER    0x02U
#define VAYU_HTC_FLAG_NEED_CREDIT 0x01U
#define VAYU_HTC_FLAG_BUNDLE     0xf0U
#define VAYU_HTC_FLAG_KNOWN      (VAYU_HTC_FLAG_NEED_CREDIT | VAYU_HTC_FLAG_TRAILER | VAYU_HTC_FLAG_BUNDLE)
#define VAYU_HTC_READY_CE        2U

enum vayu_htc_reject {
	VAYU_HTC_OK = 0,
	VAYU_HTC_SHORT_HDR,
	VAYU_HTC_BAD_LEN,
	VAYU_HTC_LEN_TOO_LONG,
	VAYU_HTC_BUNDLE,
	VAYU_HTC_BAD_TRAILER,
	VAYU_HTC_TRAILER_ONLY,
	VAYU_HTC_SHORT_MSG,
	VAYU_HTC_NOT_READY_MSG,
	VAYU_HTC_BAD_FLAGS,
	VAYU_HTC_WRONG_CE,
	VAYU_HTC_BAD_EID,
	VAYU_HTC_ZERO_CREDITS,
	VAYU_HTC_ZERO_CREDIT_SIZE,
	VAYU_HTC_BAD_ENDPOINTS,
};

struct vayu_htc_frame {
	uint8_t eid;
	uint8_t flags;
	uint8_t trailer_len;
	uint16_t payload_len;           /* the header's len field (body + trailer) */
	uint16_t body_len;              /* payload_len - trailer_len */
	uint16_t msg_id;                /* first le16 of the body, 0xffff if absent */
	uint8_t ctrl[VAYU_HTC_CTRL_MAX];
	uint8_t ctrl_len;               /* min(body_len, VAYU_HTC_CTRL_MAX) */
};

struct vayu_htc_ready {
	uint16_t credits;
	uint16_t credit_size;
	uint8_t max_endpoints;
	uint8_t raw_endpoints;          /* ctrl[6] as sent; differs from max_endpoints only when lenient */
	uint8_t htc_version;            /* 0xff when only the short READY form is present */
	uint16_t payload_len;
	uint16_t frame_len;
};

/* Header/trailer/length validation; does not look at the CE or the message id. */
enum vayu_htc_reject vayu_htc_parse_frame(const uint8_t *frame, uint32_t frame_len,
    struct vayu_htc_frame *out);

/* True READY only: ce 2, eid 0, message 1, short/long bounds, credits, endpoints. */
enum vayu_htc_reject vayu_htc_frame_is_ready(uint32_t ce,
    const struct vayu_htc_frame *frame, struct vayu_htc_ready *out);

/*
 * W20 vayu_wifi_htc_lenient=1: accept a READY whose max_endpoints (ctrl[6]) is above
 * VAYU_HTC_EP_COUNT and clamp it; 0 still rejects.  Default false = W19 behaviour.
 */
extern bool vayu_htc_lenient;

/*
 * W21 vayu_wifi_htc_connect != 0: undefined HTC flag bits and a READY outside CE2
 * are logged by the caller instead of rejected.  Default false = W20 behaviour.
 */
extern bool vayu_htc_relaxed;

const char *vayu_htc_reject_name(enum vayu_htc_reject why);

#endif /* _SYS_VAYU_WIFI_HTC_H_ */
