/*
 * WCN3990 copy-engine wire format and host-ring arithmetic (W15-A).
 *
 * This header and vayu_wifi_ce_wire.c are freestanding: no XNU header, no
 * allocation, no I/O.  They are compiled into the kernel and, unchanged, into
 * the host tests (gcc/clang with ASan/UBSan).  The kernel side
 * (bsd/dev/vayu_wifi_ce.c) owns the registers, the DMA pool and the polling.
 *
 * Facts and where they come from:
 *   - struct ce_desc_64 { __le64 addr; __le16 nbytes; __le16 flags; __le32
 *     toeplitz_hash_result; } is Linux ath10k ce.h:58-63; WCN3990 has
 *     target_64bit = true (core.c:545), so CE_DESC_SIZE_64 = 16 bytes.
 *     CE_DESC_ADDR_MASK = GENMASK_ULL(34, 0) (ce.h:42) and the upper address
 *     word carries CE_DESC_ADDR_HI_MASK = GENMASK(4, 0) (ce.h:43).
 *   - host_ce_config_wlan[] is ath10k snoc.c:141-246 as shipped with the vayu
 *     tree in /dev/shm/vayu-wifi-ref/snoc.c (CE4 src_nentries 2048), which is
 *     also ios-flame phone/wifi/radio.c:155-160 (MIT).  The older 4.14
 *     vayu-kdbg copy has CE4 = 256; the difference is CE4's host->target HTT
 *     source ring, which W15-A never posts to.  Values are facts about the
 *     host ring configuration, attributed here, not copied code.
 *   - Rings are powers of two and the write index is masked; the host may have
 *     at most nentries-1 buffers posted (CE_RING_DELTA(write, sw-1), ce.c:362-
 *     366 and _rx_num_free_bufs), which vayu_ce_rx_post_count() reproduces.
 *   - A received descriptor is ready when the CE has advanced CUR_DRRI and the
 *     descriptor's nbytes is non-zero; Linux treats nbytes == 0 as "not yet"
 *     (ce.c:915-923), and ath10k warns and drops a frame longer than the
 *     buffer (snoc.c:595-600).  This module drops it too (clamping would hand
 *     a truncated HTC frame to the parser).
 *
 * Copyright (c) 2026 The Redwood Project.  APSL 2.0, like the rest of bsd/dev.
 */

#ifndef _SYS_VAYU_WIFI_CE_WIRE_H_
#define _SYS_VAYU_WIFI_CE_WIRE_H_

#include <stdbool.h>
#include <stdint.h>

#define VAYU_CE_COUNT            12
#define VAYU_CE_DESC_SIZE        16
#define VAYU_CE_ADDR_MASK        0x00000007ffffffffULL   /* 35 bits, ce.h:42 */
#define VAYU_CE_ADDR_HI_MASK     0x1fU                  /* ce.h:43 */
#define VAYU_CE_RX_POST_WANT     32                     /* W15-A: 32 buffers per RX CE */
#define VAYU_CE_POOL_MAX         (2U << 20)             /* hard cap, checked before allocating */
#define VAYU_CE_ALIGN            64U                     /* ring/buffer alignment: one cache line */
#define VAYU_CE_DIAG_CE          7                      /* host-only diagnostic window */
#define VAYU_CE_RX_BUF_DEFAULT   2048U

/* One row of ath10k host_ce_config_wlan (src_nentries/src_sz_max/dest_nentries/flags). */
struct vayu_ce_cfg {
	uint16_t src_nentries;
	uint16_t src_sz_max;
	uint16_t dest_nentries;
	uint16_t flags;                 /* bit 3 = CE_ATTR_DIS_INTR */
};

#define VAYU_CE_CFG_DIS_INTR     0x0008

/* The 12 host pipes, in CE order.  See the file comment for the source. */
extern const struct vayu_ce_cfg vayu_ce_host_cfg[VAYU_CE_COUNT];

/* Decoded 16-byte descriptor as the CE and the host see it in memory. */
struct vayu_ce_desc {
	uint64_t addr;                  /* raw le64 field; vayu_ce_rx_verdict() rejects bits above 34 */
	uint16_t nbytes;
	uint16_t flags;
	uint32_t toeplitz;
};

/* Host DMA pool layout, computed from vayu_ce_host_cfg[] with overflow checks. */
struct vayu_ce_pool {
	uint64_t size;                  /* bytes before page rounding */
	uint64_t rri_off;               /* 12 x u32 read-index array */
	uint64_t src_off[VAYU_CE_COUNT];
	uint64_t dst_off[VAYU_CE_COUNT];
	uint64_t rx_off[VAYU_CE_COUNT];
	uint32_t src_n[VAYU_CE_COUNT];  /* power-of-two entries, 0 = no ring */
	uint32_t dst_n[VAYU_CE_COUNT];
	uint32_t rx_post[VAYU_CE_COUNT];/* buffers posted, 0 = no host RX ring */
	uint32_t rx_bufsz[VAYU_CE_COUNT];
};

/* 0 stays 0 (no ring); values that would overflow 32 bits return 0. */
uint32_t vayu_ce_roundup_pow2(uint32_t n);

/* A ring needs at least two entries: mask = n-1 must not be 0 for index math. */
bool vayu_ce_ring_ok(uint32_t nentries);

/* mask must come from vayu_ce_ring_ok() nentries. */
uint32_t vayu_ce_ring_incr(uint32_t mask, uint32_t index);
uint32_t vayu_ce_ring_delta(uint32_t mask, uint32_t from, uint32_t to);
uint32_t vayu_ce_rx_post_count(uint32_t nentries, uint32_t want);

/* true when the row describes a host receive ring that W15-A posts to. */
bool vayu_ce_is_host_rx(uint32_t ce);

/* Fills out; on any error returns false and leaves out zeroed. */
bool vayu_ce_pool_layout(const struct vayu_ce_cfg *cfg, uint32_t rx_post_want,
    struct vayu_ce_pool *out);
/* W21h: per-CE want (VAYU_CE_COUNT entries); buffers allocated per vayu_ce_rx_buf_count(). */
bool vayu_ce_pool_layout_want(const struct vayu_ce_cfg *cfg, const uint32_t *rx_post_want,
    struct vayu_ce_pool *out);
uint32_t vayu_ce_rx_buf_count(uint32_t nentries, uint32_t post);

/* What the CPU should do with one destination descriptor. */
enum vayu_ce_rx_verdict {
	VAYU_CE_RX_OK = 0,
	VAYU_CE_RX_NOT_READY,           /* nbytes == 0: the DRRI ran ahead of the descriptor */
	VAYU_CE_RX_TOO_BIG,             /* nbytes > buffer: drop, do not clamp */
	VAYU_CE_RX_BAD_ADDR,            /* addr outside the pool or zero */
};

enum vayu_ce_rx_verdict vayu_ce_rx_verdict(const struct vayu_ce_desc *desc,
    uint32_t buf_size, uint32_t buf_count, uint64_t pool_pa, uint64_t pool_size);

/*
 * Receive-ring state machine, shared by the kernel and the host ring regression.
 *
 * The producer write index (wr) is what the host advertises to the CE with DR_WR_IDX.
 * The invariant is that every slot in [sw, wr) has a descriptor that the host wrote for
 * one of the `post` buffers: re-posting must happen at wr, never at the slot just
 * consumed.  Posting at the consumed slot while advancing wr advertises descriptors that
 * were never written (addr = 0), which is a DMA to PA 0 / SMMU fault.
 */
struct vayu_ce_rx_ring {
	uint8_t *desc;          /* descriptor ring, host address, 16 bytes per slot */
	uint64_t buf_pa;        /* device address of buffer 0 */
	uint32_t nentries;      /* power of two, 2.. */
	uint32_t post;          /* advertised buffers, 1..nentries-1 */
	uint32_t nbuf;          /* buffers in the pool: post if it divides nentries, else nentries */
	uint32_t bufsz;
	uint32_t sw;            /* next slot the host will consume */
	uint32_t wr;            /* next slot the host will advertise (producer) */
};

bool vayu_ce_rx_ring_init(struct vayu_ce_rx_ring *ring, uint8_t *desc, uint64_t buf_pa,
    uint32_t nentries, uint32_t post, uint32_t bufsz, uint32_t sw, uint32_t wr);

/* Write a receive descriptor for the next producer slot and advance wr. */
bool vayu_ce_rx_ring_post(struct vayu_ce_rx_ring *ring);

/* true when every advertised slot [sw, wr) carries a pool-owned, buffer-aligned,
 * non-zero address (and nbytes is still 0, i.e. it was written by the host and the CE
 * has not produced into it yet).  bad_slot receives the first offending slot.
 * Use this before the first DR_WR_IDX doorbell; for a timeout diagnostic, where the CE
 * may legitimately have produced descriptors that were not drained, use the address
 * check below. */
bool vayu_ce_rx_ring_window_ok(const struct vayu_ce_rx_ring *ring, uint32_t *bad_slot);

/* Address-only variant: every advertised slot carries a pool-owned, buffer-aligned,
 * non-zero address, regardless of nbytes.  A non-zero nbytes here means the CE
 * produced that descriptor and it was not drained yet (expected at a timeout). */
bool vayu_ce_rx_ring_window_addr_ok(const struct vayu_ce_rx_ring *ring, uint32_t *bad_slot);

/* true = stop after this slot (a real READY was parsed). */
typedef bool (*vayu_ce_rx_slot_fn)(void *ctx, uint32_t slot, const struct vayu_ce_desc *desc,
    enum vayu_ce_rx_verdict verdict);

/* Consume up to max descriptors the CE has produced (drri), re-posting each consumed
 * buffer at the producer index.  Returns the number consumed.
 * If the device's DRRI is outside the advertised window [sw, wr) (more produced than
 * was posted, or behind sw), *drri_outside is set and nothing is read or re-posted:
 * descriptors outside the window may be stale or never written. */
/* W19 vayu_wifi_ce_addrmask=1: drop the WCN3990 descriptor flag bits above bit 34 of the
 * written-back destination address before the verdict (ath10k and flame never read it). */
extern bool vayu_ce_rx_addr_mask;
extern uint64_t vayu_ce_rx_addr_hi_seen;

uint32_t vayu_ce_rx_ring_drain(struct vayu_ce_rx_ring *ring, uint32_t drri, uint32_t max,
    vayu_ce_rx_slot_fn fn, void *ctx, bool *drri_outside);

void vayu_ce_desc_encode(uint8_t out[VAYU_CE_DESC_SIZE], uint64_t addr,
    uint16_t nbytes, uint16_t flags);
void vayu_ce_desc_decode(const uint8_t in[VAYU_CE_DESC_SIZE], struct vayu_ce_desc *out);

#endif /* _SYS_VAYU_WIFI_CE_WIRE_H_ */
