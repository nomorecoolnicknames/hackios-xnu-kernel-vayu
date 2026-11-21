/* SPDX-License-Identifier: MIT-0 */
/*
 * The framebuffer stand-in's frame-info answers for sweetfb_frameinfo=N (docs/ios27/PRESENT.md, "Frame info"): the key
 * list of IOMFB's SupportedFrameInfo call (user client selector 74), the mask of ChangeFrameInfo (selector 73), and the
 * values of the frame-info notification (type 3) the kernel sends when a swap reaches the panel. Plain C over integers,
 * no kernel: the kernel's iokit/Kernel/arm/SweetFramebuffer.cpp (the xnu fork) calls it, tools/sweetfb_frameinfo_test.c
 * checks it on a host. NOT run on a phone when written (18 Nov 2026).
 *
 * What iOS 27.0.1 (24A446) does with these, read from its IOMobileFramebuffer and QuartzCore (names and structure only):
 * - SupportedFrameInfo (selector 74): scalar in the index of a key; scalars out the number of keys and a mode (1: values
 *   come through a shared data queue instead of notifications; 0 here); 64 bytes out, the key's name. IOMFB asks for index
 *   0 first and gives up on frame info when the call fails or the number is 0 (which is what the stand-in answered
 *   before: CoreAnimation then never asks for frame info at all).
 * - CoreAnimation's window server then enables, one at a time through ChangeFrameInfo (selector 73, scalars 3 and the
 *   mask of key indices), the tags it wants that the list offers, and skips the others: Presentation_time,
 *   Presentation_delta, Requested_presentation, Performance_headroom, Thermal_throttle, Min_FrameTime, Max_FrameTime,
 *   Vbl_FrameTime, Last_Fence_or_Event_Time.
 * - The notification (type 3, IOMFB's FrameInfoNotifyFunc): 64-bit arguments [mask, swap id, one value per bit of the
 *   mask in the order of the key indices]. IOMFB makes a dictionary {key name: value} of them and calls CoreAnimation's
 *   IOMFBServer::frame_info_callback(swap id, dictionary). Time keys are in mach_continuous_time's clock; IOMFB moves
 *   them to mach_absolute_time's (kern_egest_timestamp; 0 stays 0).
 * - CoreAnimation reads Presentation_time, Requested_presentation and Last_Fence_or_Event_Time there, and passes
 *   Presentation_time to IOMFBDisplay::add_timing, the only writer of the display timings its clients read (and the
 *   reference its display timer's phase is aligned with); frame_info_callback is its only caller.
 *
 * Offered here, only what the kernel knows (bit of N, then the key):
 *   1  Presentation_time         the time the present of that swap finished (the frame is in the buffer the panel is
 *                                scanned out of from then on; "when it reached the glass" to within that refresh).
 *                                Certain as to meaning (CoreAnimation logs it as "presented at"); the value is ours.
 *   2  Last_Fence_or_Event_Time  the time the kernel saw the swap's last GPU chain done (STARLTE: the vsync thread's wait
 *                                for it returned). Meaning inferred from the name only: switchable for that reason.
 * Not offered (not known to the kernel, or meaning not known): Requested_presentation (CoreAnimation's target time: in
 * the swap record, offset not found), Presentation_delta, Min/Max/Vbl_FrameTime, Performance_headroom, Thermal_throttle.
 * A notification goes out only when Presentation_time is enabled and known: CoreAnimation takes it as a timing.
 */
#ifndef HAKIOS_SWEETFB_FRAMEINFO_H
#define HAKIOS_SWEETFB_FRAMEINFO_H

#include <stdint.h>
#include <string.h>

#define SFI_NAME_BYTES          64      /* SupportedFrameInfo's structure out */
#define SFI_KEY_PRESENTED       0       /* bit 1 of sweetfb_frameinfo */
#define SFI_KEY_FENCE           1       /* bit 2 */
#define SFI_KEYS                2
#define SFI_TYPE                3       /* IOMFB's notification type, and ChangeFrameInfo's first scalar */
#define SFI_ARGS                (2 + SFI_KEYS)

static const char sfi_key_name[SFI_KEYS][32] = { "Presentation_time", "Last_Fence_or_Event_Time" };

/* the keys offered for boot-arg value `on`: bits of SFI_KEY_*, nothing else */
static inline uint32_t
sfi_on(uint32_t on)
{
	return on & ((1u << SFI_KEYS) - 1);
}

/* how many keys are offered */
static inline uint32_t
sfi_count(uint32_t on)
{
	uint32_t n = 0;
	for (uint32_t k = 0; k < SFI_KEYS; k++) {
		n += (sfi_on(on) >> k) & 1;
	}
	return n;
}

/* the key (SFI_KEY_*) at an index of the offered list, or -1 */
static inline int
sfi_key_at(uint32_t on, uint64_t index)
{
	uint64_t i = 0;
	for (uint32_t k = 0; k < SFI_KEYS; k++) {
		if ((sfi_on(on) >> k) & 1) {
			if (i == index) {
				return (int)k;
			}
			i++;
		}
	}
	return -1;
}

/*
 * SupportedFrameInfo (selector 74) for index: count and mode always written (count 0 when nothing is offered, as before),
 * the name (zero-filled to SFI_NAME_BYTES) when the index is offered. Returns 0, or -1 for an index not offered.
 */
static inline int
sfi_supported(uint32_t on, uint64_t index, uint64_t *count, uint64_t *mode, char name[SFI_NAME_BYTES])
{
	int k = sfi_key_at(on, index);
	*count = sfi_count(on);
	*mode = 0;
	memset(name, 0, SFI_NAME_BYTES);
	if (k < 0) {
		return -1;
	}
	memcpy(name, sfi_key_name[k], strlen(sfi_key_name[k]));
	return 0;
}

/* ChangeFrameInfo (selector 73): the new mask, kept to the indices offered */
static inline uint32_t
sfi_mask(uint32_t on, uint64_t mask)
{
	uint32_t n = sfi_count(on);
	return (uint32_t)(mask & ((1ull << n) - 1));
}

/* one shown swap: its id and what the kernel knows (0: not known) */
struct sfi_frame {
	uint32_t swap_id;
	uint64_t value[SFI_KEYS];       /* by SFI_KEY_*, mach_continuous_time */
};

/*
 * The notification's arguments for a client whose mask is `mask` (indices of the offered list): [mask sent, swap id,
 * values in index order]. Returns how many (at most SFI_ARGS), or 0 when nothing is to be sent: Presentation_time not
 * offered, not enabled or not known. A key enabled but not known (0) is left out of the mask sent.
 */
static inline uint32_t
sfi_message(uint32_t on, uint32_t mask, const struct sfi_frame *f, uint64_t out[SFI_ARGS])
{
	uint64_t v[SFI_ARGS];
	uint32_t sent = 0, n = 2;
	if (!(sfi_on(on) & (1u << SFI_KEY_PRESENTED))) {
		return 0;               /* (when it is offered, Presentation_time is index 0: the first key) */
	}
	mask = sfi_mask(on, mask);
	for (uint32_t i = 0; i < sfi_count(on); i++) {
		int k = sfi_key_at(on, i);
		if (k < 0 || !((mask >> i) & 1) || f->value[k] == 0) {
			continue;
		}
		sent |= 1u << i;
		v[n++] = f->value[k];
	}
	if (!(sent & 1u)) {
		return 0;               /* (out untouched) */
	}
	v[0] = sent;
	v[1] = f->swap_id;
	memcpy(out, v, n * sizeof(v[0]));
	return n;
}

#endif /* HAKIOS_SWEETFB_FRAMEINFO_H */
