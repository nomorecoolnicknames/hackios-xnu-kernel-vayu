/*
 * The order in which bsd/dev/vayu_venus.c brings the Venus island up, as a plan that the
 * host test (tools/venus-hfi) can check on the build machine.  The driver executes the steps
 * of vayu_venus_power_plan() one by one; this header only says which and in what order, so
 * the order itself is testable without the phone.
 *
 * Why there is more than one order (vayu_venus_pwr, boot-arg of the same name):
 *
 *   The k25 order brought the three GDSCs up *before* any video_cc branch: mvs0/mvs1 first,
 *   then gcc video_xo/axic/axi0/axi1, video_cc XO, PLL0, the iris RCG, the iris AHB and the
 *   mvsc/mvs0/mvs1 core branches.  On the phone the mvs0 (vcodec) step then stopped at the
 *   wrapper's MMCC handshake: WRAPPER_VCODEC0_MMCC_POWER_STATUS (0xaa0e0090) read 0 and never
 *   turned bit 1, so SYS_INIT was never reached.  The vendor GDSC driver clears HW_CONTROL,
 *   switches SW_COLLAPSE and polls PWR_ON; the *clock* that makes the mvs0 power state machine
 *   and its AXI port work is video_cc_mvs0_core (CBCR 0x890), whose parent is the iris RCG, and
 *   the AXI side is gcc_video_axi0/axi1/axic.  k25 enabled all of those only after the GDSC.
 *   HYPOTHESIS (the one this flag tests): the MMCC handshake needs the core and AXI clocks
 *   running first, and the request itself is written as 1 (power), not 0.
 *
 * The two independent halves of that hypothesis are the order and the polarity of
 * WRAPPER_VCODEC0_MMCC_POWER_CONTROL, so the boot-arg picks one of four combinations, one per
 * run:
 *
 *   vayu_venus_pwr=0   the k25 order and the k25 handshake (CONTROL <- 0, a missing ack is
 *                      fatal).  This is the default and is bit-for-bit what k25 did.
 *   vayu_venus_pwr=1   clocks first (GCC AXI/XO, video_cc XO, PLL0, iris RCG, iris AHB,
 *                      mvsc/mvs0/mvs1 core), then the GDSCs; CONTROL <- 1; a missing ack is
 *                      logged and the domain is left under software control (PWR_ON is what
 *                      the ARM9 needs), so the run still reaches VIDC_CTRL_INIT and SYS_INIT.
 *   vayu_venus_pwr=2   the same clocks-first order; CONTROL <- 0.  Tells the order apart from
 *                      the polarity if 1 fails.
 *   vayu_venus_pwr=3   the k25 order; CONTROL <- 1.  The other half alone.
 *
 * Sources for the registers and the sequence: videocc-sm8150.c (0x850/0x890/0x8d0 branches,
 * PLL0 at 0x42c, iris RCG 0x7f0), gcc-sm8150.c (video AHB 0xb004, XO 0xb040, axic/axi0/axi1
 * 0xb02c/0xb024/0xb028), gdsc.c (HW_CONTROL before SW_COLLAPPE, PWR_ON poll),
 * hfi_venus_io.h:143-147 (the MMCC status/control pair).  See the long comment at the top of
 * bsd/dev/vayu_venus.c.
 */
#ifndef _SYS_VAYU_VENUS_PWR_H_
#define _SYS_VAYU_VENUS_PWR_H_

#include <stdint.h>

enum vayu_venus_power_step {
	VAYU_VENUS_POWER_RAILS = 1,     /* RPMh mmcx.lvl and mx.lvl */
	VAYU_VENUS_POWER_BCM,           /* RPMh MM3 / SH0 / MC0 / ACV */
	VAYU_VENUS_POWER_GCC_AHB,       /* gcc video_ahb 0xb004 (video_cc's registers sit behind it) */
	VAYU_VENUS_POWER_GCC_REST,      /* gcc video_xo 0xb040, axic 0xb02c, axi0 0xb024, axi1 0xb028 */
	VAYU_VENUS_POWER_CC_XO,         /* video_cc_xo 0x984 */
	VAYU_VENUS_POWER_PLL0,          /* video_cc PLL0 0x42c, 2 x the iris rate */
	VAYU_VENUS_POWER_IRIS_RCG,      /* iris_clk_src 0x7f0: PLL0/2, or XO */
	VAYU_VENUS_POWER_IRIS_AHB,      /* video_cc_iris_ahb 0x8f4 */
	VAYU_VENUS_POWER_MVSC_CORE,     /* video_cc_mvsc_core 0x850: the ARM9's clock */
	VAYU_VENUS_POWER_MVS0_CORE,     /* video_cc_mvs0_core 0x890: the encoder core's clock */
	VAYU_VENUS_POWER_MVS1_CORE,     /* video_cc_mvs1_core 0x8d0: the decoder core's clock */
	VAYU_VENUS_POWER_GDSC_MVSC,     /* GDSCR 0x814, no MMCC handshake */
	VAYU_VENUS_POWER_GDSC_MVS0,     /* GDSCR 0x874 + wrapper 0x90/0x94 */
	VAYU_VENUS_POWER_GDSC_MVS1,     /* GDSCR 0x8b4 + wrapper 0x110/0x114 */
	VAYU_VENUS_POWER_RESETS,        /* the four block resets, the interrupt mask, the ack */
	VAYU_VENUS_POWER_STEPS
};

#define VAYU_VENUS_POWER_LEGACY         0u      /* k25: GDSCs first, MMCC CONTROL <- 0, fatal */
#define VAYU_VENUS_POWER_CLOCKS_FIRST_1 1u      /* clocks first, MMCC CONTROL <- 1, tolerant */
#define VAYU_VENUS_POWER_CLOCKS_FIRST_0 2u      /* clocks first, MMCC CONTROL <- 0, tolerant */
#define VAYU_VENUS_POWER_LEGACY_1       3u      /* k25 order, MMCC CONTROL <- 1, tolerant */
#define VAYU_VENUS_POWER_MODES          4u

/*
 * The value written to WRAPPER_VCODEC0_MMCC_POWER_CONTROL in this mode, or -1 for the k25
 * handshake (CONTROL <- 0, and a missing acknowledgement is a failure).
 */
static inline int
vayu_venus_mmcc_control(uint32_t mode)
{
	switch (mode) {
	case VAYU_VENUS_POWER_CLOCKS_FIRST_1:
	case VAYU_VENUS_POWER_LEGACY_1:
		return 1;
	case VAYU_VENUS_POWER_CLOCKS_FIRST_0:
		return 0;
	default:
		return -1;
	}
}

/* True for the modes whose order is clocks first; the k25 order is what everything else gets. */
static inline int
vayu_venus_clocks_first(uint32_t mode)
{
	return mode == VAYU_VENUS_POWER_CLOCKS_FIRST_1 || mode == VAYU_VENUS_POWER_CLOCKS_FIRST_0;
}

/*
 * Fill steps[0..count) with the order for `mode' and return the count, or 0 if the buffer is
 * too small.  Every step appears exactly once.  An unknown mode gets the k25 order: nothing
 * new is ever run by accident.
 */
static inline unsigned
vayu_venus_power_plan(uint32_t mode, uint8_t *steps, unsigned capacity)
{
	static const uint8_t legacy[] = {
		VAYU_VENUS_POWER_RAILS, VAYU_VENUS_POWER_BCM, VAYU_VENUS_POWER_GCC_AHB,
		VAYU_VENUS_POWER_GDSC_MVSC, VAYU_VENUS_POWER_GDSC_MVS0, VAYU_VENUS_POWER_GDSC_MVS1,
		VAYU_VENUS_POWER_GCC_REST, VAYU_VENUS_POWER_CC_XO, VAYU_VENUS_POWER_PLL0,
		VAYU_VENUS_POWER_IRIS_RCG, VAYU_VENUS_POWER_IRIS_AHB, VAYU_VENUS_POWER_MVSC_CORE,
		VAYU_VENUS_POWER_MVS0_CORE, VAYU_VENUS_POWER_MVS1_CORE, VAYU_VENUS_POWER_RESETS,
	};
	static const uint8_t clocks_first[] = {
		VAYU_VENUS_POWER_RAILS, VAYU_VENUS_POWER_BCM, VAYU_VENUS_POWER_GCC_AHB,
		VAYU_VENUS_POWER_GCC_REST, VAYU_VENUS_POWER_CC_XO, VAYU_VENUS_POWER_PLL0,
		VAYU_VENUS_POWER_IRIS_RCG, VAYU_VENUS_POWER_IRIS_AHB, VAYU_VENUS_POWER_MVSC_CORE,
		VAYU_VENUS_POWER_MVS0_CORE, VAYU_VENUS_POWER_MVS1_CORE, VAYU_VENUS_POWER_GDSC_MVSC,
		VAYU_VENUS_POWER_GDSC_MVS0, VAYU_VENUS_POWER_GDSC_MVS1, VAYU_VENUS_POWER_RESETS,
	};
	const uint8_t *from = vayu_venus_clocks_first(mode) ? clocks_first : legacy;
	unsigned count = vayu_venus_clocks_first(mode) ?
	    (unsigned)(sizeof(clocks_first) / sizeof(clocks_first[0])) :
	    (unsigned)(sizeof(legacy) / sizeof(legacy[0]));

	if (steps == 0 || capacity < count) {
		return 0;
	}
	for (unsigned i = 0; i < count; i++) {
		steps[i] = from[i];
	}
	return count;
}

static inline const char *
vayu_venus_power_step_name(uint32_t step)
{
	switch (step) {
	case VAYU_VENUS_POWER_RAILS: return "rails";
	case VAYU_VENUS_POWER_BCM: return "bcm";
	case VAYU_VENUS_POWER_GCC_AHB: return "gcc video_ahb";
	case VAYU_VENUS_POWER_GCC_REST: return "gcc video_xo/axic/axi0/axi1";
	case VAYU_VENUS_POWER_CC_XO: return "video_cc_xo";
	case VAYU_VENUS_POWER_PLL0: return "pll0";
	case VAYU_VENUS_POWER_IRIS_RCG: return "iris_clk_src";
	case VAYU_VENUS_POWER_IRIS_AHB: return "video_cc_iris_ahb";
	case VAYU_VENUS_POWER_MVSC_CORE: return "video_cc_mvsc_core";
	case VAYU_VENUS_POWER_MVS0_CORE: return "video_cc_mvs0_core";
	case VAYU_VENUS_POWER_MVS1_CORE: return "video_cc_mvs1_core";
	case VAYU_VENUS_POWER_GDSC_MVSC: return "GDSC mvsc";
	case VAYU_VENUS_POWER_GDSC_MVS0: return "GDSC mvs0";
	case VAYU_VENUS_POWER_GDSC_MVS1: return "GDSC mvs1";
	case VAYU_VENUS_POWER_RESETS: return "resets";
	default: return "?";
	}
}

#endif /* _SYS_VAYU_VENUS_PWR_H_ */
