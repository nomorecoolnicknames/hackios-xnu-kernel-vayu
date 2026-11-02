/*
 * WCN3990 copy-engine wire format and host-ring arithmetic (W15-A).
 *
 * Freestanding on purpose: the same file is built into the kernel and into the
 * host tests.  No register access, no allocation, no kernel header.  See
 * bsd/sys/vayu_wifi_ce_wire.h for the references (ath10k ce.h/snoc.c, ISC; and
 * ios-flame phone/wifi/radio.c, MIT).
 *
 * Copyright (c) 2026 The Redwood Project.  APSL 2.0.
 */

#include <stddef.h>
#include <sys/vayu_wifi_ce_wire.h>

/*
 * ath10k snoc.c host_ce_config_wlan[] (vayu reference tree, CE4 = 2048).
 * flags bit 3 is CE_ATTR_DIS_INTR (ce.h:287); only CE4 has it.
 */
const struct vayu_ce_cfg vayu_ce_host_cfg[VAYU_CE_COUNT] = {
	/* CE0: host->target HTC control streams */
	{ 16,   2048, 0,   0 },
	/* CE1: target->host HTT + HTC control */
	{ 0,    2048, 512, 0 },
	/* CE2: target->host WMI (also the HTC control downlink) */
	{ 0,    2048, 64,  0 },
	/* CE3: host->target WMI */
	{ 32,   2048, 0,   0 },
	/* CE4: host->target HTT, no interrupt */
	{ 2048, 256,  0,   VAYU_CE_CFG_DIS_INTR },
	/* CE5: target->host HTT (ipa_uc->target) */
	{ 0,    512,  512, 0 },
	/* CE6: target autonomous hif_memcpy */
	{ 0,    0,    0,   0 },
	/* CE7: ce_diag, host-only diagnostic window */
	{ 2,    2048, 2,   0 },
	/* CE8: target to uMC */
	{ 0,    2048, 128, 0 },
	/* CE9: target->host HTT */
	{ 0,    2048, 512, 0 },
	/* CE10: target->host HTT */
	{ 0,    2048, 512, 0 },
	/* CE11: target autonomous qcache memcpy / PKTLOG */
	{ 0,    2048, 512, 0 },
};

uint32_t
vayu_ce_roundup_pow2(uint32_t n)
{
	uint32_t p = 1;

	if (n == 0 || n > 0x80000000U) {
		return 0;                       /* 0 stays 0; above 2^31 cannot be a ring */
	}
	while (p < n) {
		p <<= 1;
	}
	return p;
}

bool
vayu_ce_ring_ok(uint32_t nentries)
{
	return (nentries >= 2) && ((nentries & (nentries - 1)) == 0);
}

uint32_t
vayu_ce_ring_incr(uint32_t mask, uint32_t index)
{
	return (index + 1) & mask;
}

uint32_t
vayu_ce_ring_delta(uint32_t mask, uint32_t from, uint32_t to)
{
	return (to - from) & mask;
}

uint32_t
vayu_ce_rx_post_count(uint32_t nentries, uint32_t want)
{
	uint32_t limit;

	if (!vayu_ce_ring_ok(nentries) || want == 0) {
		return 0;
	}
	limit = nentries - 1;           /* Linux never posts the last slot */
	return want < limit ? want : limit;
}

bool
vayu_ce_is_host_rx(uint32_t ce)
{
	if (ce >= VAYU_CE_COUNT) {
		return false;
	}
	/* CE7 is the host-only diagnostic window: Linux has no recv callback for it. */
	return vayu_ce_host_cfg[ce].dest_nentries != 0 && ce != VAYU_CE_DIAG_CE;
}

static void
pool_zero(struct vayu_ce_pool *pool)
{
	uint8_t *bytes = (uint8_t *)pool;
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(*pool); i++) {
		bytes[i] = 0;
	}
}

static bool
pool_add(uint64_t *total, uint64_t bytes, uint64_t *offset)
{
	uint64_t at;

	if (bytes == 0) {
		*offset = 0;
		return true;
	}
	at = (*total + (uint64_t)VAYU_CE_ALIGN - 1) & ~(uint64_t)(VAYU_CE_ALIGN - 1);
	if (at > (uint64_t)VAYU_CE_POOL_MAX || bytes > (uint64_t)VAYU_CE_POOL_MAX - at) {
		return false;
	}
	*offset = at;
	*total = at + bytes;
	return true;
}

uint32_t
vayu_ce_rx_buf_count(uint32_t nentries, uint32_t post)
{
	if (post == 0 || nentries == 0) {
		return 0;
	}
	/* A count that does not divide the ring (n-1) needs one buffer per slot: slot % n. */
	return (nentries % post) == 0 ? post : nentries;
}

bool
vayu_ce_pool_layout(const struct vayu_ce_cfg *cfg, uint32_t rx_post_want,
    struct vayu_ce_pool *out)
{
	uint32_t want[VAYU_CE_COUNT];
	uint32_t ce;

	for (ce = 0; ce < VAYU_CE_COUNT; ce++) {
		want[ce] = rx_post_want;
	}
	return vayu_ce_pool_layout_want(cfg, want, out);
}

bool
vayu_ce_pool_layout_want(const struct vayu_ce_cfg *cfg, const uint32_t *rx_post_want,
    struct vayu_ce_pool *out)
{
	uint64_t total = 0;
	uint32_t ce;

	if (cfg == NULL || out == NULL) {
		return false;
	}
	pool_zero(out);
	if (pool_add(&total, (uint64_t)VAYU_CE_COUNT * sizeof(uint32_t), &out->rri_off) == false) {
		return false;
	}
	for (ce = 0; ce < VAYU_CE_COUNT; ce++) {
		uint32_t n = vayu_ce_roundup_pow2(cfg[ce].src_nentries);

		if (cfg[ce].src_nentries != 0 && !vayu_ce_ring_ok(n)) {
			pool_zero(out);
			return false;
		}
		out->src_n[ce] = n;
		if (n != 0) {
			uint64_t bytes = (uint64_t)n * VAYU_CE_DESC_SIZE;

			if (bytes > (uint64_t)VAYU_CE_POOL_MAX ||
			    pool_add(&total, bytes, &out->src_off[ce]) == false) {
				pool_zero(out);
				return false;
			}
		}
	}
	for (ce = 0; ce < VAYU_CE_COUNT; ce++) {
		uint32_t n = vayu_ce_roundup_pow2(cfg[ce].dest_nentries);

		if (cfg[ce].dest_nentries != 0 && !vayu_ce_ring_ok(n)) {
			pool_zero(out);
			return false;
		}
		out->dst_n[ce] = n;
		if (n != 0) {
			uint64_t bytes = (uint64_t)n * VAYU_CE_DESC_SIZE;

			if (bytes > (uint64_t)VAYU_CE_POOL_MAX ||
			    pool_add(&total, bytes, &out->dst_off[ce]) == false) {
				pool_zero(out);
				return false;
			}
		}
	}
	for (ce = 0; ce < VAYU_CE_COUNT; ce++) {
		uint32_t post;
		uint64_t bytes;

		if (!vayu_ce_is_host_rx(ce)) {
			continue;
		}
		if (cfg[ce].src_sz_max == 0) {
			pool_zero(out);
			return false;           /* a receive ring without a buffer size */
		}
		post = vayu_ce_rx_post_count(out->dst_n[ce], rx_post_want[ce]);
		if (post == 0) {
			pool_zero(out);
			return false;
		}
		bytes = (uint64_t)vayu_ce_rx_buf_count(out->dst_n[ce], post) * cfg[ce].src_sz_max;
		if (bytes > (uint64_t)VAYU_CE_POOL_MAX ||
		    pool_add(&total, bytes, &out->rx_off[ce]) == false) {
			pool_zero(out);
			return false;
		}
		out->rx_post[ce] = post;
		out->rx_bufsz[ce] = cfg[ce].src_sz_max;
	}
	out->size = total;
	return true;
}

void
vayu_ce_desc_encode(uint8_t out[VAYU_CE_DESC_SIZE], uint64_t addr,
    uint16_t nbytes, uint16_t flags)
{
	uint64_t a = addr & VAYU_CE_ADDR_MASK;

	out[0] = (uint8_t)(a);
	out[1] = (uint8_t)(a >> 8);
	out[2] = (uint8_t)(a >> 16);
	out[3] = (uint8_t)(a >> 24);
	out[4] = (uint8_t)(a >> 32);
	out[5] = (uint8_t)(a >> 40);
	out[6] = (uint8_t)(a >> 48);
	out[7] = (uint8_t)(a >> 56);
	out[8] = (uint8_t)(nbytes);
	out[9] = (uint8_t)(nbytes >> 8);
	out[10] = (uint8_t)(flags);
	out[11] = (uint8_t)(flags >> 8);
	out[12] = 0;                    /* toeplitz_hash_result, device-owned on RX */
	out[13] = 0;
	out[14] = 0;
	out[15] = 0;
}

void
vayu_ce_desc_decode(const uint8_t in[VAYU_CE_DESC_SIZE], struct vayu_ce_desc *out)
{
	out->addr = (uint64_t)in[0] | (uint64_t)in[1] << 8 | (uint64_t)in[2] << 16 |
	    (uint64_t)in[3] << 24 | (uint64_t)in[4] << 32 | (uint64_t)in[5] << 40 |
	    (uint64_t)in[6] << 48 | (uint64_t)in[7] << 56;
	out->nbytes = (uint16_t)((uint16_t)in[8] | (uint16_t)in[9] << 8);
	out->flags = (uint16_t)((uint16_t)in[10] | (uint16_t)in[11] << 8);
	out->toeplitz = (uint32_t)in[12] | (uint32_t)in[13] << 8 |
	    (uint32_t)in[14] << 16 | (uint32_t)in[15] << 24;
}

bool vayu_ce_rx_addr_mask;
uint64_t vayu_ce_rx_addr_hi_seen;

enum vayu_ce_rx_verdict
vayu_ce_rx_verdict(const struct vayu_ce_desc *desc, uint32_t buf_size,
    uint32_t buf_count, uint64_t pool_pa, uint64_t pool_size)
{
	uint64_t offset;

	if (desc->nbytes == 0) {
		return VAYU_CE_RX_NOT_READY;
	}
	if (desc->nbytes > buf_size) {
		return VAYU_CE_RX_TOO_BIG;
	}
	if ((desc->addr & ~VAYU_CE_ADDR_MASK) != 0) {
		return VAYU_CE_RX_BAD_ADDR;
	}
	if (desc->addr < pool_pa) {
		return VAYU_CE_RX_BAD_ADDR;
	}
	offset = desc->addr - pool_pa;
	if (offset >= pool_size || buf_size == 0 || buf_count == 0) {
		return VAYU_CE_RX_BAD_ADDR;
	}
	/* The address must be exactly the start of one of the posted buffers. */
	if ((offset % buf_size) != 0 || (offset / buf_size) >= buf_count) {
		return VAYU_CE_RX_BAD_ADDR;
	}
	return VAYU_CE_RX_OK;
}

bool
vayu_ce_rx_ring_init(struct vayu_ce_rx_ring *ring, uint8_t *desc, uint64_t buf_pa,
    uint32_t nentries, uint32_t post, uint32_t bufsz, uint32_t sw, uint32_t wr)
{
	if (ring == NULL || desc == NULL || !vayu_ce_ring_ok(nentries) || post == 0 ||
	    post >= nentries || bufsz == 0 || sw >= nentries || wr >= nentries) {
		return false;
	}
	ring->desc = desc;
	ring->buf_pa = buf_pa;
	ring->nentries = nentries;
	ring->post = post;
	ring->nbuf = vayu_ce_rx_buf_count(nentries, post);
	ring->bufsz = bufsz;
	ring->sw = sw;
	ring->wr = wr;
	return true;
}

bool
vayu_ce_rx_ring_post(struct vayu_ce_rx_ring *ring)
{
	uint32_t slot;
	uint64_t addr;

	if (ring == NULL || ring->desc == NULL || !vayu_ce_ring_ok(ring->nentries) ||
	    ring->post == 0 || ring->post >= ring->nentries) {
		return false;
	}
	slot = ring->wr;
	if (slot >= ring->nentries) {
		return false;
	}
	addr = ring->buf_pa + (uint64_t)(slot % ring->nbuf) * ring->bufsz;
	vayu_ce_desc_encode(ring->desc + (uint64_t)slot * VAYU_CE_DESC_SIZE, addr, 0, 0);
	ring->wr = vayu_ce_ring_incr(ring->nentries - 1, slot);
	return true;
}

static bool
rx_ring_window_check(const struct vayu_ce_rx_ring *ring, bool require_idle,
    uint32_t *bad_slot)
{
	uint32_t mask, slot, left;

	if (ring == NULL || ring->desc == NULL || !vayu_ce_ring_ok(ring->nentries) ||
	    ring->post == 0 || ring->post >= ring->nentries || ring->bufsz == 0) {
		if (bad_slot != NULL) {
			*bad_slot = 0;
		}
		return false;
	}
	mask = ring->nentries - 1;
	slot = ring->sw;
	left = vayu_ce_ring_delta(mask, ring->sw, ring->wr);
	while (left-- != 0) {
		struct vayu_ce_desc d;
		uint64_t offset;

		vayu_ce_desc_decode(ring->desc + (uint64_t)slot * VAYU_CE_DESC_SIZE, &d);
		if ((require_idle && d.nbytes != 0) || d.addr == 0 || d.addr < ring->buf_pa) {
			if (bad_slot != NULL) {
				*bad_slot = slot;
			}
			return false;
		}
		offset = d.addr - ring->buf_pa;
		if (offset >= (uint64_t)ring->nbuf * ring->bufsz ||
		    (offset % ring->bufsz) != 0) {
			if (bad_slot != NULL) {
				*bad_slot = slot;
			}
			return false;
		}
		slot = vayu_ce_ring_incr(mask, slot);
	}
	return true;
}

bool
vayu_ce_rx_ring_window_ok(const struct vayu_ce_rx_ring *ring, uint32_t *bad_slot)
{
	return rx_ring_window_check(ring, true, bad_slot);
}

bool
vayu_ce_rx_ring_window_addr_ok(const struct vayu_ce_rx_ring *ring, uint32_t *bad_slot)
{
	return rx_ring_window_check(ring, false, bad_slot);
}

uint32_t
vayu_ce_rx_ring_drain(struct vayu_ce_rx_ring *ring, uint32_t drri, uint32_t max,
    vayu_ce_rx_slot_fn fn, void *ctx, bool *drri_outside)
{
	uint32_t mask, window, produced, limit, i, consumed = 0;

	if (drri_outside != NULL) {
		*drri_outside = false;
	}
	if (ring == NULL || ring->desc == NULL || !vayu_ce_ring_ok(ring->nentries) ||
	    ring->post == 0 || max == 0) {
		return 0;
	}
	mask = ring->nentries - 1;
	/*
	 * The CE may only have written slots in the host-advertised window [sw, wr).
	 * A DRRI that implies more (or a DRRI behind sw) would make the loop read
	 * stale or never-written descriptors, so refuse it before touching anything.
	 */
	window = vayu_ce_ring_delta(mask, ring->sw, ring->wr);
	produced = vayu_ce_ring_delta(mask, ring->sw, drri & mask);
	if (produced > window) {
		if (drri_outside != NULL) {
			*drri_outside = true;
		}
		return 0;
	}
	limit = produced < max ? produced : max;
	for (i = 0; i < limit; i++) {
		uint32_t slot = ring->sw;
		struct vayu_ce_desc desc;
		enum vayu_ce_rx_verdict verdict;
		bool stop;

		vayu_ce_desc_decode(ring->desc + (uint64_t)slot * VAYU_CE_DESC_SIZE, &desc);
		if (vayu_ce_rx_addr_mask && desc.nbytes != 0 && (desc.addr & ~VAYU_CE_ADDR_MASK) != 0) {
			vayu_ce_rx_addr_hi_seen = desc.addr;
			desc.addr &= VAYU_CE_ADDR_MASK;
		}
		verdict = vayu_ce_rx_verdict(&desc, ring->bufsz, ring->nbuf, ring->buf_pa,
		    (uint64_t)ring->nbuf * ring->bufsz);
		if (verdict == VAYU_CE_RX_NOT_READY) {
			break;                  /* the CE index ran ahead of the descriptor */
		}
		ring->sw = vayu_ce_ring_incr(mask, slot);
		stop = fn != NULL && fn(ctx, slot, &desc, verdict);
		/*
		 * Re-post at the producer index, after the callback has consumed the
		 * buffer.  This is the C1 fix: posting at `slot` while advancing wr
		 * would advertise a descriptor that was never written (addr 0).
		 */
		if (!vayu_ce_rx_ring_post(ring)) {
			break;
		}
		consumed++;
		if (stop) {
			break;
		}
	}
	return consumed;
}
