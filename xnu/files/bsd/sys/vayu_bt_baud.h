/*
 * The Bluetooth UART's operating rate (bsd/dev/vayu_bt.c, boot-arg vayu_bt_baud): the arithmetic
 * of it, pure C for the kernel and for tools/bt-test.
 *
 * 115200 baud carries some 90 kbit/s of H4, and A2DP's audio wants 200-350: a speaker or AirPods
 * pair and their settings work, but no sound (janhurn9400/renoir 7287928f1, xreactx/oneplus-6t-
 * upstream README "A2DP works": both at 3 Mbit/s). Linux's hci_qca.c qca_set_speed() does the
 * change for the WCN399x before the firmware: the controller is told with its EDL vendor command
 * 0xfc48 and one byte, the QCA_BAUDRATE_ code (btqca.h: 0 is 115200, 0x0e is 3000000; the WCN3990's
 * own NVM, crnv21.bin, carries 0x11 = 3.2 Mbit/s in tag 17), then the host follows.
 *
 * The host side on SM8150: the GENI UART oversamples 32 times (msm_geni_serial.c:109, no hardware
 * condition; bsd/dev/vayu_bt.c's step 2 ran 2400 baud = 19.2 MHz / (32 x 250)), so 3 Mbit/s needs
 * the SE clock at 96 MHz with the serial divider 1. 96 MHz is an entry of the vendor's
 * ftbl_gcc_qupv3_wrap0_s0_clk_src (gcc-sm8150.c:624-641, lab receipt 2026-10-25-vayu-bt.md), from
 * GPLL0's even output (300 MHz) through M/N: 300 MHz x 8 / 25. The loader programs the same RCG to
 * the table's first entry, 7 372 800 Hz = 300 MHz x 384 / 15625, the same way (xnuloader/loader.c,
 * vayu_bt_loader): clk-rcg2.c __clk_rcg2_configure() with mnd_width 16, hid_width 5 -- M = m,
 * N = ~(n - m), D = ~(2d) with 2d = n clamped to [m, 2(n - m)], CFG = (2 x 1 - 1) | source 6 << 8 |
 * dual-edge 2 << 12 -- and CMD's UPDATE bit, which the RCG clears when it has taken them.
 *
 * From other HakiOS ports (APSL 2.0): xreactx/hakios-xnu-kernel, branch oneplus-6t-upstream, commit 0e5e7bd6a05c,
 * and janhurn9400/hackios-xnu-kernel, branch renoir, commit 7287928f164b (both run the UART at 3 Mbit/s).
 */
#ifndef _SYS_VAYU_BT_BAUD_H_
#define _SYS_VAYU_BT_BAUD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VAYU_BT_EDL_SET_BAUDRATE        0xfc48u         /* EDL_SET_BAUDRATE_CMD_OCF 0x48, OGF 0x3f */
#define VAYU_BT_QCA_BAUD_115200         0x00            /* btqca.h QCA_BAUDRATE_115200 */
#define VAYU_BT_QCA_BAUD_3000000        0x0e            /* btqca.h QCA_BAUDRATE_3000000 */
#define VAYU_BT_QCA_BAUD_NONE           0xff

#define VAYU_BT_GPLL0_EVEN_HZ           300000000ull    /* gcc_parent_map_0 source 6 */
#define VAYU_BT_RCG_SRC_GPLL0_EVEN      6u
#define VAYU_BT_RCG_MND_MASK            0xffffu         /* mnd_width 16 */
#define VAYU_BT_RCG_CFG_KEEP            (~(0x1fu | (0x7u << 8) | (0x3u << 12)))   /* hid, source, mode */
#define VAYU_BT_UART_OVERSAMPLING       32u

/* The QCA_BAUDRATE_ code of a rate this driver runs the wire at, or VAYU_BT_QCA_BAUD_NONE. */
static inline uint8_t
vayu_bt_qca_baud_code(uint32_t baud)
{
	return baud == 115200 ? VAYU_BT_QCA_BAUD_115200 : baud == 3000000 ? VAYU_BT_QCA_BAUD_3000000 :
	       VAYU_BT_QCA_BAUD_NONE;
}

/*
 * The SE clock a rate is run from (serial divider 1 for 3 Mbit/s, 2 for 115200 and 96 for 2400 from
 * 7.3728 MHz), and its M/N from GPLL0 even: the two entries of the vendor's table this driver uses.
 */
static inline bool
vayu_bt_rcg_mn(uint64_t rate_hz, uint32_t *m, uint32_t *n)
{
	if (rate_hz == 96000000ull) {
		*m = 8;
		*n = 25;
	} else if (rate_hz == 7372800ull) {
		*m = 384;
		*n = 15625;
	} else {
		return false;
	}
	return true;
}

/* The RCG's M, N, D and CFG words for m/n, as clk-rcg2.c writes them; cfg is the CFG word as it reads now. */
static inline bool
vayu_bt_rcg_words(uint32_t m, uint32_t n, uint32_t cfg_now, uint32_t *mreg, uint32_t *nreg, uint32_t *dreg,
    uint32_t *cfg)
{
	uint32_t twice_d = n;

	if (m == 0 || n <= m || n > VAYU_BT_RCG_MND_MASK) {
		return false;
	}
	if (twice_d < m) {
		twice_d = m;
	}
	if (twice_d > 2 * (n - m)) {
		twice_d = 2 * (n - m);
	}
	*mreg = m;
	*nreg = ~(n - m) & VAYU_BT_RCG_MND_MASK;
	*dreg = ~twice_d & VAYU_BT_RCG_MND_MASK;
	*cfg = (cfg_now & VAYU_BT_RCG_CFG_KEEP) | 1u | (VAYU_BT_RCG_SRC_GPLL0_EVEN << 8) | (2u << 12);
	return true;
}

/* What the words give, read back the way bsd/dev/vayu_bt.c's bt_dfs_rate() does for source 6 (0: not that). */
static inline uint64_t
vayu_bt_rcg_rate(uint32_t cfg, uint32_t mreg, uint32_t nreg)
{
	uint32_t hid = cfg & 0x1fu, mode = (cfg >> 12) & 0x3u, m = mreg & VAYU_BT_RCG_MND_MASK;
	uint32_t n = ((~nreg) & VAYU_BT_RCG_MND_MASK) + m;
	uint64_t rate = VAYU_BT_GPLL0_EVEN_HZ;

	if (((cfg >> 8) & 0x7u) != VAYU_BT_RCG_SRC_GPLL0_EVEN) {
		return 0;
	}
	if (hid != 0) {
		rate = rate * 2 / (hid + 1);
	}
	if (mode != 0) {
		if (m == 0 || n == 0) {
			return 0;
		}
		rate = rate * m / n;
	}
	return rate;
}

/* The serial divider for baud from a clock, or 0 when it is not an exact multiple of baud x 32. */
static inline uint32_t
vayu_bt_uart_divider(uint64_t clock_hz, uint32_t baud)
{
	uint64_t want = (uint64_t)baud * VAYU_BT_UART_OVERSAMPLING;

	return want == 0 || clock_hz < want || clock_hz % want != 0 ? 0 : (uint32_t)(clock_hz / want);
}

/* The H4 command that tells the controller its new rate: 01 48 fc 01 code. */
static inline size_t
vayu_bt_set_baud_command(uint8_t code, uint8_t out[5])
{
	out[0] = 0x01;
	out[1] = (uint8_t)(VAYU_BT_EDL_SET_BAUDRATE & 0xff);
	out[2] = (uint8_t)(VAYU_BT_EDL_SET_BAUDRATE >> 8);
	out[3] = 1;
	out[4] = code;
	return 5;
}

#endif /* _SYS_VAYU_BT_BAUD_H_ */
