/*
 * HTC frame header parser for the W15-A HTC_MSG_READY proof.
 *
 * Freestanding.  The parser walks one CE frame at a time: HTC over CE has one
 * frame per receive descriptor, so there is no reassembly across descriptors
 * (and none is attempted; a frame that stops early is rejected, never merged
 * with the next one).
 *
 * Copyright (c) 2026 The Redwood Project.  APSL 2.0.
 */

#include <stddef.h>
#include <sys/vayu_wifi_htc.h>

bool vayu_htc_lenient;
bool vayu_htc_relaxed;

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static void
frame_zero(struct vayu_htc_frame *frame)
{
	uint8_t *bytes = (uint8_t *)frame;
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(*frame); i++) {
		bytes[i] = 0;
	}
}

enum vayu_htc_reject
vayu_htc_parse_frame(const uint8_t *frame, uint32_t frame_len,
    struct vayu_htc_frame *out)
{
	uint32_t body;

	if (out == NULL) {
		return VAYU_HTC_BAD_LEN;
	}
	frame_zero(out);
	if (frame == NULL || frame_len < VAYU_HTC_HDR_SIZE) {
		return VAYU_HTC_SHORT_HDR;
	}
	out->eid = frame[0];
	out->flags = frame[1];
	out->payload_len = le16(frame + 2);
	out->trailer_len = frame[4];
	out->msg_id = 0xffffU;
	if ((out->flags & (uint8_t)~VAYU_HTC_FLAG_KNOWN) != 0 && !vayu_htc_relaxed) {
		return VAYU_HTC_BAD_FLAGS;      /* undefined HTC flag bits */
	}
	if ((uint32_t)out->payload_len > VAYU_HTC_MAX_LEN - VAYU_HTC_HDR_SIZE) {
		return VAYU_HTC_LEN_TOO_LONG;
	}
	if ((uint32_t)out->payload_len + VAYU_HTC_HDR_SIZE > frame_len) {
		return VAYU_HTC_BAD_LEN;
	}
	if ((out->flags & VAYU_HTC_FLAG_BUNDLE) != 0) {
		return VAYU_HTC_BUNDLE;         /* W15-A does not support bundles */
	}
	/*
	 * H1: trailer_len > payload_len must be rejected whether or not the trailer
	 * flag is set.  Subtracting first underflows to a huge body and lets stale
	 * bytes beyond the declared payload synthesize a READY.  When the flag is
	 * clear the byte is undefined (Linux ignores it), so normalize it to zero
	 * rather than reject a real frame.
	 */
	if (out->trailer_len > out->payload_len) {
		return VAYU_HTC_BAD_TRAILER;
	}
	if ((out->flags & VAYU_HTC_FLAG_TRAILER) != 0) {
		if (out->trailer_len < 4) {
			return VAYU_HTC_BAD_TRAILER;
		}
	} else {
		out->trailer_len = 0;
	}
	body = (uint32_t)out->payload_len - out->trailer_len;
	if (body == 0) {
		return VAYU_HTC_TRAILER_ONLY;
	}
	if (body + VAYU_HTC_HDR_SIZE > frame_len) {
		return VAYU_HTC_BAD_LEN;        /* explicit: body must fit the CE frame */
	}
	out->body_len = (uint16_t)body;
	out->ctrl_len = (uint8_t)(body < VAYU_HTC_CTRL_MAX ? body : VAYU_HTC_CTRL_MAX);
	if (out->ctrl_len > out->body_len) {
		return VAYU_HTC_BAD_LEN;        /* cannot happen with the min above */
	}
	for (uint32_t i = 0; i < out->ctrl_len; i++) {
		out->ctrl[i] = frame[VAYU_HTC_HDR_SIZE + i];
	}
	if (out->body_len >= 2) {
		out->msg_id = le16(out->ctrl);
	}
	return VAYU_HTC_OK;
}

enum vayu_htc_reject
vayu_htc_frame_is_ready(uint32_t ce, const struct vayu_htc_frame *frame,
    struct vayu_htc_ready *out)
{
	uint16_t credits, credit_size;
	uint8_t endpoints, version;

	if (out == NULL || frame == NULL) {
		return VAYU_HTC_BAD_LEN;
	}
	if (ce != VAYU_HTC_READY_CE && !vayu_htc_relaxed) {
		return VAYU_HTC_WRONG_CE;
	}
	if (frame->eid != VAYU_HTC_EP0) {
		return VAYU_HTC_BAD_EID;
	}
	if ((frame->flags & VAYU_HTC_FLAG_BUNDLE) != 0) {
		return VAYU_HTC_BUNDLE;
	}
	if (frame->body_len < 8) {
		return VAYU_HTC_SHORT_MSG;      /* message id + ready (htc.c:624) */
	}
	if (frame->msg_id != VAYU_HTC_MSG_READY) {
		return VAYU_HTC_NOT_READY_MSG;
	}
	credits = le16(frame->ctrl + 2);
	credit_size = le16(frame->ctrl + 4);
	endpoints = frame->ctrl[6];
	if (credits == 0) {
		return VAYU_HTC_ZERO_CREDITS;   /* htc.c:646-650 */
	}
	if (credit_size == 0) {
		return VAYU_HTC_ZERO_CREDIT_SIZE;
	}
	out->raw_endpoints = endpoints;
	if (endpoints == 0 || (endpoints > VAYU_HTC_EP_COUNT && !vayu_htc_lenient)) {
		return VAYU_HTC_BAD_ENDPOINTS;
	}
	if (endpoints > VAYU_HTC_EP_COUNT) {
		endpoints = VAYU_HTC_EP_COUNT;
	}
	version = frame->body_len >= 12 ? frame->ctrl[8] : 0xffU;
	out->credits = credits;
	out->credit_size = credit_size;
	out->max_endpoints = endpoints;
	out->htc_version = version;
	out->payload_len = frame->payload_len;
	out->frame_len = (uint16_t)(frame->payload_len + VAYU_HTC_HDR_SIZE);
	return VAYU_HTC_OK;
}

const char *
vayu_htc_reject_name(enum vayu_htc_reject why)
{
	switch (why) {
	case VAYU_HTC_OK:
		return "ok";
	case VAYU_HTC_SHORT_HDR:
		return "short-htc-header";
	case VAYU_HTC_BAD_LEN:
		return "frame-shorter-than-htc-len";
	case VAYU_HTC_LEN_TOO_LONG:
		return "htc-len-above-4096";
	case VAYU_HTC_BUNDLE:
		return "bundled";
	case VAYU_HTC_BAD_TRAILER:
		return "bad-trailer-len";
	case VAYU_HTC_TRAILER_ONLY:
		return "trailer-only";
	case VAYU_HTC_SHORT_MSG:
		return "short-control-message";
	case VAYU_HTC_NOT_READY_MSG:
		return "not-htc-ready-message";
	case VAYU_HTC_BAD_FLAGS:
		return "undefined-htc-flag-bits";
	case VAYU_HTC_WRONG_CE:
		return "not-ce2";
	case VAYU_HTC_BAD_EID:
		return "eid-not-0";
	case VAYU_HTC_ZERO_CREDITS:
		return "zero-credits";
	case VAYU_HTC_ZERO_CREDIT_SIZE:
		return "zero-credit-size";
	case VAYU_HTC_BAD_ENDPOINTS:
		return "endpoints-out-of-range";
	default:
		return "unknown";
	}
}
