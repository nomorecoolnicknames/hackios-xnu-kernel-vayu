/*
 * QUPv3 SPI for the Novatek NT36672C on the POCO X3 Pro (vayu, SM8150).
 *
 * This is the bus half of bsd/dev/vayu_nvt.c when the kernel is started with
 * "nvt_qup=1" (or "touch_driver=nvt_qup"). The Novatek protocol itself stays in
 * vayu_nvt.c; this file only moves bytes.
 *
 * Why a QUP driver and not the bit-banged pins: XNU cannot write the TLMM on this
 * phone (a write through the vendor pinctrl window at 0x03000000 is ignored, and a
 * write through the first tile alias at 0x03100000 wedged the bus). The loader
 * (xnuloader/loader.c) therefore does the pin work before it jumps here. It muxes
 * GPIO 55, 56, 57 and 58 to the "qup17" function in the SOUTH TLMM tile, applies
 * the touch I/O supply on GPIO 100, and releases reset on GPIO 12. This file does
 * not write TLMM at all; it only reads the pin registers to report what the loader
 * left.
 *
 * Addresses on vayu (from uefi/files/Resources/DTBs/vayu.dtb, decompiled):
 *
 *   spi@c80000          compatible "qcom,spi-geni", reg 0xc80000 0x4000,
 *                       interrupts <0 0x175 0>, clocks <&gcc 0x6d 0x7d 0x7e>
 *                       (GCC_QUPV3_WRAP2_S0_CLK, WRAP_2_M_AHB, WRAP_2_S_AHB),
 *                       qcom,wrapper-core = <&qupv3_2_geni_se@cc0000>
 *   qupv3_2_geni_se@cc0000   reg 0xcc0000 0x6000 (the wrapper; NOT 0x26c0000,
 *                       which is qupv3_3 for the SLPI and is disabled in this DT)
 *   pinctrl@03000000    the vendor pinctrl driver's single window; the four tiles
 *                       are at +0x100000 (west), +0x500000 (east), +0x900000
 *                       (north), +0xd00000 (south). Pins 55-58 are SOUTH:
 *                       0x03d00000 + 0x1000 * pin.
 *
 * Register layouts are from the vendor kernel (MiCode Xiaomi_Kernel_OpenSource,
 * branch vayu-r-oss, commit 0c9888aa2373958ae38787ed84b803701f73f0a1):
 *   drivers/spi/spi-geni-qcom.c
 *   drivers/platform/msm/qcom-geni-se.c
 *   drivers/clk/qcom/gcc-sm8150.c
 *   drivers/pinctrl/qcom/pinctrl-sm8150.c
 *   drivers/input/touchscreen/nt36672c/nt36xxx.c
 * and the mainline GENI register definitions (drivers/soc/qcom/qcom-geni-se.c).
 *
 * Everything is polled: no interrupt (the SPI IRQ is 0x175, but XNU has no
 * interrupt route for it here), no DMA, no command queue. A report is 67 bytes at
 * 1 MHz, about 0.6 ms, so polling is enough and keeps the driver small.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern bool vayu_rpmh_tcs_lock(void);
extern void vayu_rpmh_tcs_unlock(bool held);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t hakios_smmu_lock;     /* hakios.wifi.c: the apps SMMU's match registers and contexts */

/* ---- the blocks ------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x80000UL

#define QUP_WRAPPER_PHYS        0x00CC0000UL      /* qcom,qupv3_2_geni_se@cc0000 */
#define QUP_WRAPPER_SIZE        0x6000UL
#define QUP_SE_PHYS             0x00C80000UL      /* spi@c80000, 0x4000 registers */
#define QUP_SE_SIZE             0x4000UL

#define TLMM_SOUTH_PHYS         0x03D00000UL      /* vendor pinctrl tile SOUTH */
#define TLMM_NORTH_PHYS         0x03900000UL      /* ... NORTH (reset GPIO12) */
#define TLMM_TILE_SIZE          0x100000UL

/* ---- GCC: the SE clock, the two AHB clocks, and their vote register ---------------------------- */
/*
 * vendor gcc-sm8150.c:951-952 (s0 RCG), :2857-2862 (s0 branch), :3021-3026 (m_ahb),
 * :3034-3041 (s_ahb, hwcg bit 1); clk-rcg2.c:34-58 (CMD/CFG/DFSR bits). Mainline v6.6
 * gcc-sm8150.c:666, 2402, 2560, 2573 agree.
 */

#define GCC_QUPV3_VOTE          0x52014
#define GCC_VOTE_S_AHB          (1u << 1)         /* gcc_qupv3_wrap_2_s_ahb_clk */
#define GCC_VOTE_M_AHB          (1u << 2)         /* gcc_qupv3_wrap_2_m_ahb_clk */
#define GCC_VOTE_SE0            (1u << 4)         /* gcc_qupv3_wrap2_s0_clk (SE17 in pin/alias numbering) */

#define GCC_WRAP2_S_AHB_HALT    0x1e008
#define GCC_WRAP2_M_AHB_HALT    0x1e004
#define GCC_WRAP2_S0_HALT       0x1e144
#define GCC_WRAP2_S0_CMD        0x1e148           /* clk_rcg2 cmd_rcgr */
#define GCC_WRAP2_S0_CFG        (GCC_WRAP2_S0_CMD + 0x4)
#define GCC_WRAP2_S0_DFS        (GCC_WRAP2_S0_CMD + 0x14)  /* DFS enable + table, if the bootloader used it */
#define GCC_DFS_EN              (1u << 0)

#define GCC_HALT_BIT            (1u << 31)        /* a branch's "clock is off" bit */
#define GCC_CBCR_HW_CTL         (1u << 1)         /* s_ahb: hwcg_reg 0x1e008, hwcg_bit 1 */
#define GCC_CMD_UPDATE          (1u << 0)
#define GCC_CMD_ROOT_EN         (1u << 1)
#define GCC_CMD_ROOT_OFF        (1u << 31)
#define GCC_CFG_SRC_MASK        (0x7u << 8)
#define GCC_CFG_DIV_MASK        0x1fu
#define GCC_CFG_MODE_MASK       (0x3u << 12)
#define GCC_CFG_HW_CTRL         (1u << 20)
#define GCC_CFG_SRC_XO          0                 /* gcc_parent_map_0: P_BI_TCXO = 0 */
#define GCC_CFG_DIV_1           1
#define GCC_XO_HZ               19200000u         /* vayu's CXO; the loader prints CNTFRQ=19200000 */

/* ---- GENI / SE registers (offsets inside spi@c80000) ------------------------------------------- */

#define SE_GENI_INIT_CFG_REV    0x000
#define SE_GENI_S_INIT_CFG_REV  0x004
#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define SE_GENI_STATUS          0x040
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_CLK_CTRL_RO        0x060
#define GENI_IF_FIFO_DISABLE_RO 0x064
#define GENI_FW_REVISION_RO     0x068
#define GENI_FW_S_REVISION_RO   0x06c
#define SE_GENI_CLK_SEL         0x07c
#define SE_GENI_CFG_SEQ_START   0x084
#define SE_SPI_CPHA             0x224
#define SE_SPI_LOOPBACK         0x22c
#define SE_SPI_CPOL             0x230
#define SE_SPI_DEMUX_OUTPUT_INV 0x24c
#define SE_SPI_DEMUX_SEL        0x250
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_SPI_TRANS_CFG        0x25c
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_SPI_WORD_LEN         0x268
#define SE_SPI_TX_TRANS_LEN     0x26c
#define SE_SPI_RX_TRANS_LEN     0x270
#define SE_SPI_PRE_POST_CMD_DLY 0x274
#define SE_SPI_DELAY_COUNTERS   0x278
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL      0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_EN        0x644
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK    0x80c
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define SE_DMA_GENERAL_CFG      0xe30

#define QUPV3_HW_VER            0x004             /* inside the wrapper at 0xcc0000 */

/* GENI_FW_REVISION_RO: [15:8] is the protocol the SE's loaded firmware speaks */
#define FW_REV_PROTOCOL_MSK     (0xffu << 8)
#define FW_REV_PROTOCOL_SHFT    8
#define FW_REV_VERSION_MSK      0xffu
#define SE_PROTO_SPI            1

/* SE_SPI_* fields */
#define SPI_CPHA                (1u << 0)
#define SPI_CPOL                (1u << 2)
#define SPI_LOOPBACK_MSK        0x3u
#define SPI_CS_TOGGLE           (1u << 0)
#define SPI_WORD_LEN_MSK        0x3ffu
#define SPI_MIN_WORD_LEN        4
#define SPI_TRANS_LEN_MSK       0xffffffu

/* M_CMD opcodes and parameters */
#define SPI_TX_ONLY             1
#define SPI_RX_ONLY             2
#define SPI_FULL_DUPLEX         3
#define SPI_CS_ASSERT           8                 /* vendor spi-geni-qcom.c:86-87, mainline v6.6:63-64 */
#define SPI_CS_DEASSERT         9
#define SE_GENI_M_GP_LENGTH     0x910             /* vendor qcom-geni-se.h:166 */
#define M_GENI_CMD_CANCEL       (1u << 2)         /* vendor qcom-geni-se.h GENI_M_CMD_CTRL_REG */
#define M_GENI_CMD_ABORT        (1u << 1)
#define M_OPCODE_SHFT           27
#define M_PARAMS_MSK            0x7ffffffu

/* M_IRQ status/enable bits */
#define M_CMD_DONE              (1u << 0)
#define M_CMD_OVERRUN           (1u << 1)
#define M_ILLEGAL_CMD           (1u << 2)
#define M_CMD_FAILURE           (1u << 3)
#define M_RX_FIFO_RD_ERR        (1u << 24)
#define M_RX_FIFO_WR_ERR        (1u << 25)
#define M_RX_FIFO_WATERMARK     (1u << 26)
#define M_RX_FIFO_LAST          (1u << 27)
#define M_TX_FIFO_RD_ERR        (1u << 28)
#define M_TX_FIFO_WR_ERR        (1u << 29)
#define M_TX_FIFO_WATERMARK     (1u << 30)
#define M_ERRORS                (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | \
				 M_RX_FIFO_RD_ERR | M_RX_FIFO_WR_ERR | \
				 M_TX_FIFO_RD_ERR | M_TX_FIFO_WR_ERR)

/* GENI_CGC_CTRL and SE_DMA_GENERAL_CFG defaults used by geni_se_init() */
#define GENI_CGC_DEFAULT        (0x7fu)
#define DMA_CGC_DEFAULT         (0xfu)

/* SE_HW_PARAM_0/1 */
#define TX_FIFO_WIDTH_MSK       (0x3fu << 24)
#define TX_FIFO_WIDTH_SHFT      24
#define TX_FIFO_DEPTH_MSK       (0x3fu << 16)
#define TX_FIFO_DEPTH_SHFT      16
#define RX_FIFO_WIDTH_MSK       (0x3fu << 24)
#define RX_FIFO_WIDTH_SHFT      24
#define RX_FIFO_DEPTH_MSK       (0x3fu << 16)
#define RX_FIFO_DEPTH_SHFT      16
#define TX_FIFO_WC_MSK          0x0fffffffu
#define RX_FIFO_WC_MSK          0x01ffffffu
#define RX_LAST                 (1u << 31)

/* ---- state ------------------------------------------------------------------------------------- */

static vm_offset_t gcc_base;
static vm_offset_t wrapper_base;
static vm_offset_t se_base;
static vm_offset_t tlmm_south;
static vm_offset_t tlmm_north;

static bool qup_ready;
static bool qup_dry;
static uint32_t qup_stop;                       /* nvt_qup_stop=N: 1 before GCC writes, 2 wrapper read, 3 SE read,
                                                   4 SE writes, 5 first transfer */
static bool qup_verbose = true;
static bool qup_prefill;                        /* nvt_qup_prefill=1: fill the TX FIFO before M_CMD0 */
static bool qup_cs_test;                        /* nvt_qup_cs_test=1: a data-less CS assert/deassert first */
static uint32_t qup_speed_hz = 1000000;
static uint32_t se_clock_hz = GCC_XO_HZ;
static uint32_t tx_fifo_depth;
static uint32_t rx_fifo_depth;
static uint32_t tx_fifo_width;
static uint32_t pack_words = 1;                 /* bytes per 32-bit FIFO word */
static bool qup_dfs;                            /* nvt_qup_dfs=1: keep XBL's DFS RCG, select a level (C6) */
static uint32_t qup_clk_sel;                    /* SE_GENI_CLK_SEL = DFS level (0 without DFS) */
static uint32_t qup_os = 1;                     /* oversampling, nvt_qup_os=2 */
/*
 * nvt_qup_gsi: 2 = GPI DMA transfers (the production path since v28: the SE firmware is GSI-only, and v26
 * read the chip id this way), 1 = the P1 bring-up test, 0 = the old FIFO path. Default 2.
 */
static uint32_t qup_gsi_mode = 2;

static inline uint32_t
qrd(vm_offset_t base, uint32_t offset)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (base + offset) : "memory");
	return value;
}

static inline void
qwr(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (base + offset) : "memory");
}

static void
qup_sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
qup_sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}


/*
 * Lines go to the loader's live log as well as printf (as hakios.buttons.c does): the loader
 * reads that ring while the kernel runs, so a bus hang on the touch path still leaves its
 * last line; printf alone only reaches the loader if the kernel comes back.
 */
extern void loader_log_putc(char c);

static int qup_say(const char *format, ...) __printflike(1, 2);

static int
qup_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define QUP_LOG(...)    qup_say("nvt-qup: " __VA_ARGS__)
/*
 * The GSI path's per-transfer lines (TREs, events, doorbell, begins/done): only for the first
 * nvt_qup_gsi_trace transfers (default 64). After that a touch at 100 Hz wrote ten lines every 10 ms
 * to the kernel's buffer and the loader's log, and pushed everything else out of both (run 161610).
 * A failed transfer is still told in full.
 */
static uint32_t gsi_trace_max = 64, gsi_traced;
static bool gsi_tracing = true;
#define GSI_XLOG(...)   do { if (gsi_tracing) { QUP_LOG(__VA_ARGS__); } } while (0)

static void
qup_reg_write(vm_offset_t base, uint32_t offset, uint32_t value, const char *name)
{
	if (qup_verbose) {
		QUP_LOG("%s: write %#x = %#x\n", name, offset, value);
	}
	qwr(base, offset, value);
}

/* ---- pin state, read-only ---------------------------------------------------------------------- */

static bool
qup_pins_report(void)
{
	uint32_t cfg[4], io[4];

	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	if (tlmm_south == 0 || tlmm_north == 0) {
		QUP_LOG("cannot map the TLMM tiles (south %#lx north %#lx)\n",
		    (unsigned long)tlmm_south, (unsigned long)tlmm_north);
		return false;
	}

	for (unsigned i = 0; i < 4; i++) {
		uint32_t pin = 55 + i;

		cfg[i] = qrd(tlmm_south, 0x1000u * pin);
		io[i] = qrd(tlmm_south, 0x1000u * pin + 4);
	}
	QUP_LOG("tlmm south %#lx: cfg 55..58 = %#x %#x %#x %#x; io = %#x %#x %#x %#x; mux = %u %u %u %u\n",
	    (unsigned long)tlmm_south, cfg[0], cfg[1], cfg[2], cfg[3], io[0], io[1], io[2], io[3],
	    (cfg[0] >> 2) & 0xf, (cfg[1] >> 2) & 0xf, (cfg[2] >> 2) & 0xf, (cfg[3] >> 2) & 0xf);
	QUP_LOG("tlmm north %#lx: reset gpio12 cfg %#x io %#x; south: vddio gpio100 cfg %#x io %#x, int gpio122 cfg %#x io %#x\n",
	    (unsigned long)tlmm_north, qrd(tlmm_north, 0x1000u * 12), qrd(tlmm_north, 0x1000u * 12 + 4),
	    qrd(tlmm_south, 0x1000u * 100), qrd(tlmm_south, 0x1000u * 100 + 4),
	    qrd(tlmm_south, 0x1000u * 122), qrd(tlmm_south, 0x1000u * 122 + 4));

	for (unsigned i = 0; i < 4; i++) {
		if (((cfg[i] >> 2) & 0xf) != 1) {
			QUP_LOG("pin %u is not muxed to qup17 (function %u). The loader must run with nvt_qup=1 "
			    "and do the TLMM work; XNU does not write TLMM on this phone.\n", 55 + i, (cfg[i] >> 2) & 0xf);
			return false;
		}
	}
	return true;
}

/*
 * RESX (GPIO12) for the vendor ENG reset on the QUP path (the bring-up notes R1):
 * vendor nt36xxx.c holds RESX low from gpio_request_one(GPIOF_OUT_INIT_LOW) (:1195), writes ENG_RST
 * (:424-430), then raises RESX and waits 10 ms (:2397-2405). Narrow XNU-TLMM exception granted by the
 * maintainers 2026-10-24 (the bring-up notes): only GPIO12's output bit, only if the loader already
 * made it a GPIO output (mux 0, OE set; else it refuses), logged with readback. Off by default since
 * v29 (the NT36675 is flashless: a reset loses the firmware Android downloaded); nvt_qup_xnu_reset=1. Layout:
 * mainline pinctrl-sm8150.c v6.6:42-53 (ctl 0x1000*n, io +4, mux bit 2, oe bit 9, out bit 1),
 * :1326 GPIO12 in the NORTH tile; read-modify-write of the io register as msm_gpio_set()
 * (pinctrl-msm.c v6.6:644-649).
 */
bool vayu_qup_nvt_resx(bool high);
bool
vayu_qup_nvt_resx(bool high)
{
	uint32_t on = 0, cfg, io;      /* off by default since v29: it wipes a flashless chip's firmware */

	PE_parse_boot_argn("nvt_qup_xnu_reset", &on, sizeof(on));
	if (!on) {
		return false;
	}
	if (tlmm_north == 0) {
		tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
		if (tlmm_north == 0) {
			return false;
		}
	}
	cfg = qrd(tlmm_north, 0x1000u * 12);
	if (((cfg >> 2) & 0xf) != 0 || !((cfg >> 9) & 1)) {
		QUP_LOG("resx: gpio12 cfg %#x is not a GPIO output (mux %u oe %u); not touching it\n", cfg, (cfg >> 2) & 0xf,
		    (cfg >> 9) & 1);
		return false;
	}
	qwr(tlmm_north, 0x1000u * 12 + 4, high ? (qrd(tlmm_north, 0x1000u * 12 + 4) | 2u) :
	    (qrd(tlmm_north, 0x1000u * 12 + 4) & ~2u));
	io = qrd(tlmm_north, 0x1000u * 12 + 4);
	QUP_LOG("resx: gpio12 %s: cfg %#x io %#x (out %u, in %u)%s\n", high ? "high" : "low", qrd(tlmm_north, 0x1000u * 12),
	    io, (io >> 1) & 1, io & 1, ((io >> 1) & 1) == (high ? 1u : 0u) ? "" : " -- DID NOT READ BACK");
	return ((io >> 1) & 1) == (high ? 1u : 0u);
}

/* ---- GCC --------------------------------------------------------------------------------------- */

/*
 * A branch in hardware-gated mode keeps CLK_OFF set while its bus is idle, so
 * the vendor clk_branch_wait() (clk-branch.c:86-91) does not halt-check it. On
 * vayu wrap2 s_ahb reads 0x80000002 (CLK_OFF + HW_CTL) before and after the
 * vote, yet the loader read the wrapper's HW_VER (0x10030000) through it.
 */
static bool
qup_wait_halt(uint32_t offset, const char *name, uint32_t timeout_us, bool hwcg)
{
	uint32_t value = 0;

	for (uint32_t i = 0; i < timeout_us / 10; i++) {
		value = qrd(gcc_base, offset);
		if (hwcg && (value & GCC_CBCR_HW_CTL)) {
			QUP_LOG("%s is hardware-gated (%#x at %#x); not halt-checked\n", name, value, offset);
			return true;
		}
		if ((value & GCC_HALT_BIT) == 0) {
			return true;
		}
		qup_sleep_us(10);
	}
	QUP_LOG("%s is still halted after %u us (%#x at %#x); the clock source or vote did not take\n",
	    name, timeout_us, value, offset);
	return false;
}

/*
 * With nvt_qup_gcc=1 the loader (nvt_qup_setup) does this: it votes the two AHB
 * branches and the SE0 branch and puts the RCG at XO/1 (19.2 MHz); then this only
 * checks. Otherwise the same sequence runs here: the RCG is
 * programmed while the SE branch is off, so the SE never sees a glitch.
 */
static bool
qup_gcc_already_on(void)
{
	uint32_t want = GCC_VOTE_M_AHB | GCC_VOTE_S_AHB | GCC_VOTE_SE0;
	uint32_t cfg = qrd(gcc_base, GCC_WRAP2_S0_CFG);

	return (qrd(gcc_base, GCC_QUPV3_VOTE) & want) == want &&
	       (qrd(gcc_base, GCC_WRAP2_M_AHB_HALT) & GCC_HALT_BIT) == 0 &&
	       (qrd(gcc_base, GCC_WRAP2_S0_HALT) & GCC_HALT_BIT) == 0 &&
	       (qrd(gcc_base, GCC_WRAP2_S0_CMD) & (GCC_CMD_ROOT_OFF | GCC_CMD_UPDATE)) == 0 &&
	       (qrd(gcc_base, GCC_WRAP2_S0_DFS) & GCC_DFS_EN) == 0 &&
	       (cfg & (GCC_CFG_SRC_MASK | GCC_CFG_DIV_MASK | GCC_CFG_MODE_MASK)) ==
	       ((GCC_CFG_SRC_XO << 8) | GCC_CFG_DIV_1);
}

/*
 * C6 (the bring-up notes): Linux never writes this RCG. vendor gcc-sm8150.c:4185
 * registers gcc_qupv3_wrap2_s0_clk_src as DFS_ENABLE_RCG; for a DFS RCG the frequency table is read
 * from the per-level registers XBL programmed (mainline clk-rcg2.c:53-57 SE_CMD_DFSR 0x14, SE_PERF_DFSR(l)
 * 0x1c+4l, M 0x5c+4l, N 0x9c+4l; clk_rcg2_dfs_populate_freq() :1254-1296), and the SE picks the level
 * with SE_GENI_CLK_SEL = the table index (vendor spi-geni-qcom.c:385-395). qup_gcc_init() without the
 * flag clears DFS_EN and forces CFG = XO/1, the one step where XNU does the opposite of Linux.
 * Level cfg fields as CFG: div [4:0] (hid_width 5, = 2*div-1), src [10:8], mode [13:12] (clk-rcg2.c:34-38);
 * parents (gcc-sm8150.c:60-65 gcc_parent_map_0): 0 bi_tcxo 19.2 MHz, 1 GPLL0 (600 MHz), 6 GPLL0_EVEN
 * (300 MHz) -- the PLL rates are the usual SM8150 ones, INFERENCE, printed for checking.
 * Level picked: the first XO/1 one (the 19.2 MHz the SE has run at so far), else 0.
 */
static bool
qup_dfs_pick(void)
{
	uint32_t dfs = qrd(gcc_base, GCC_WRAP2_S0_DFS), pick = ~0u;
	uint32_t hz[8] = { 0 };

	if ((dfs & GCC_DFS_EN) == 0) {
		QUP_LOG("dfs: DFS is OFF (%#x) -- the loader (nvt_qup_gcc=1) or an earlier run cleared it; "
		    "run nvt_qup_dfs=1 from a fresh boot without nvt_qup_gcc\n", dfs);
		return false;
	}
	for (uint32_t l = 0; l < 8; l++) {
		uint32_t cfg = qrd(gcc_base, GCC_WRAP2_S0_CMD + 0x1c + 4 * l);
		uint32_t m = qrd(gcc_base, GCC_WRAP2_S0_CMD + 0x5c + 4 * l), n = qrd(gcc_base, GCC_WRAP2_S0_CMD + 0x9c + 4 * l);
		uint32_t src = (cfg >> 8) & 7, hid = cfg & 0x1f, mode = (cfg >> 12) & 3;
		uint64_t rate = src == 0 ? 19200000ull : src == 1 ? 600000000ull : src == 6 ? 300000000ull : 0;

		if (hid) {
			rate = rate * 2 / (hid + 1);
		}
		if (mode && m) {
			uint32_t nn = (~n & 0xffff) + (m & 0xffff);
			rate = nn ? rate * (m & 0xffff) / nn : 0;
		}
		hz[l] = (uint32_t)rate;
		QUP_LOG("dfs level %u: cfg %#x (src %u div-field %u mode %u) m %#x n %#x -> %u Hz\n", l, cfg, src, hid, mode, m, n,
		    hz[l]);
		if (pick == ~0u && src == 0 && hid <= 1 && mode == 0 && cfg != 0) {
			pick = l;
		}
	}
	if (pick == ~0u) {
		pick = 0;
	}
	qup_clk_sel = pick;
	se_clock_hz = hz[pick] ? hz[pick] : GCC_XO_HZ;
	QUP_LOG("dfs: cmd_dfsr %#x; level %u picked (%u Hz) -> SE_GENI_CLK_SEL %u\n", dfs, pick, se_clock_hz, pick);
	return true;
}

static bool
qup_gcc_init(void)
{
	uint32_t vote, cmd, cfg;

	gcc_base = ml_io_map(GCC_PHYS, GCC_SIZE);
	if (gcc_base == 0) {
		QUP_LOG("cannot map GCC at %#lx\n", (unsigned long)GCC_PHYS);
		return false;
	}

	QUP_LOG("gcc mapped at %#lx; reading the vote and halts\n", (unsigned long)gcc_base);
	vote = qrd(gcc_base, GCC_QUPV3_VOTE);
	QUP_LOG("gcc vote %#x: m_ahb %u, s_ahb %u, se0 %u; halts m %#x s %#x se %#x; rcg cfg %#x cmd %#x dfs %#x\n",
	    vote, (vote & GCC_VOTE_M_AHB) ? 1 : 0, (vote & GCC_VOTE_S_AHB) ? 1 : 0,
	    (vote & GCC_VOTE_SE0) ? 1 : 0, qrd(gcc_base, GCC_WRAP2_M_AHB_HALT),
	    qrd(gcc_base, GCC_WRAP2_S_AHB_HALT), qrd(gcc_base, GCC_WRAP2_S0_HALT),
	    qrd(gcc_base, GCC_WRAP2_S0_CFG), qrd(gcc_base, GCC_WRAP2_S0_CMD),
	    qrd(gcc_base, GCC_WRAP2_S0_DFS));

	if (qup_dry) {
		QUP_LOG("dry run: not writing GCC\n");
		return true;
	}
	if (qup_stop == 1) {
		QUP_LOG("nvt_qup_stop=1: stopping before any GCC write\n");
		return false;
	}
	if (qup_dfs) {
		/* C6: no write to the RCG (CMD/CFG/DFS); only the branch votes and their halt bits. */
		if (!qup_dfs_pick()) {
			return false;
		}
		vote = qrd(gcc_base, GCC_QUPV3_VOTE);
		qwr(gcc_base, GCC_QUPV3_VOTE, vote | GCC_VOTE_M_AHB | GCC_VOTE_S_AHB | GCC_VOTE_SE0);
		if (!qup_wait_halt(GCC_WRAP2_M_AHB_HALT, "wrap2 m_ahb", 10000, false) ||
		    !qup_wait_halt(GCC_WRAP2_S_AHB_HALT, "wrap2 s_ahb", 10000, true) ||
		    !qup_wait_halt(GCC_WRAP2_S0_HALT, "wrap2 se0", 10000, false)) {
			return false;
		}
		QUP_LOG("gcc on (DFS kept): vote %#x; rcg cmd %#x cfg %#x untouched\n", qrd(gcc_base, GCC_QUPV3_VOTE),
		    qrd(gcc_base, GCC_WRAP2_S0_CMD), qrd(gcc_base, GCC_WRAP2_S0_CFG));
		return true;
	}
	if (qup_gcc_already_on()) {
		se_clock_hz = GCC_XO_HZ;
		QUP_LOG("gcc on from the loader: no writes, se clock %u Hz (XO/1)\n", se_clock_hz);
		return true;
	}
	QUP_LOG("gcc not set up by the loader (no nvt_qup_gcc=1?); running the sequence here\n");

	/* SE branch off before its RCG changes. */
	qwr(gcc_base, GCC_QUPV3_VOTE, qrd(gcc_base, GCC_QUPV3_VOTE) & ~GCC_VOTE_SE0);
	qup_sleep_us(10);

	/* Root enable if the RCG has been left off. */
	cmd = qrd(gcc_base, GCC_WRAP2_S0_CMD);
	if (cmd & GCC_CMD_ROOT_OFF) {
		qwr(gcc_base, GCC_WRAP2_S0_CMD, cmd | GCC_CMD_ROOT_EN);
		for (unsigned i = 0; i < 100; i++) {
			if ((qrd(gcc_base, GCC_WRAP2_S0_CMD) & GCC_CMD_ROOT_OFF) == 0) {
				break;
			}
			qup_sleep_us(10);
		}
		QUP_LOG("rcg root was off; cmd now %#x\n", qrd(gcc_base, GCC_WRAP2_S0_CMD));
	}

	/*
	 * If the bootloader left the RCG's DFS engine enabled, SE_GENI_CLK_SEL
	 * would select a stale DFS level, not the CFG we are about to write.
	 * Turn DFS off: a single fixed XO/1 is all this driver needs.
	 */
	cfg = qrd(gcc_base, GCC_WRAP2_S0_DFS);
	if (cfg & GCC_DFS_EN) {
		qwr(gcc_base, GCC_WRAP2_S0_DFS, cfg & ~GCC_DFS_EN);
		QUP_LOG("rcg dfs was enabled (%#x); disabled for the fixed XO/1 rate\n", cfg);
	}

	/* XO / 1: src 0, div 1, normal mode, hardware control off. */
	cfg = qrd(gcc_base, GCC_WRAP2_S0_CFG);
	qwr(gcc_base, GCC_WRAP2_S0_CFG,
	    (cfg & ~(GCC_CFG_SRC_MASK | GCC_CFG_DIV_MASK | GCC_CFG_MODE_MASK | GCC_CFG_HW_CTRL)) |
	    (GCC_CFG_SRC_XO << 8) | GCC_CFG_DIV_1);
	qwr(gcc_base, GCC_WRAP2_S0_CMD, qrd(gcc_base, GCC_WRAP2_S0_CMD) | GCC_CMD_UPDATE);
	for (unsigned i = 0; i < 100; i++) {
		if ((qrd(gcc_base, GCC_WRAP2_S0_CMD) & GCC_CMD_UPDATE) == 0) {
			break;
		}
		qup_sleep_us(10);
	}
	QUP_LOG("rcg programmed to XO/%u: cfg %#x cmd %#x (cfg was %#x)\n", GCC_CFG_DIV_1,
	    qrd(gcc_base, GCC_WRAP2_S0_CFG), qrd(gcc_base, GCC_WRAP2_S0_CMD), cfg);

	/* AHB first, then the SE branch. */
	vote = qrd(gcc_base, GCC_QUPV3_VOTE);
	qwr(gcc_base, GCC_QUPV3_VOTE, vote | GCC_VOTE_M_AHB | GCC_VOTE_S_AHB);
	if (!qup_wait_halt(GCC_WRAP2_M_AHB_HALT, "wrap2 m_ahb", 10000, false) ||
	    !qup_wait_halt(GCC_WRAP2_S_AHB_HALT, "wrap2 s_ahb", 10000, true)) {
		return false;
	}
	qwr(gcc_base, GCC_QUPV3_VOTE, qrd(gcc_base, GCC_QUPV3_VOTE) | GCC_VOTE_SE0);
	if (!qup_wait_halt(GCC_WRAP2_S0_HALT, "wrap2 se0", 10000, false)) {
		return false;
	}

	se_clock_hz = GCC_XO_HZ;
	QUP_LOG("gcc on: vote %#x, se clock %u Hz (XO/1)\n", qrd(gcc_base, GCC_QUPV3_VOTE), se_clock_hz);
	return true;
}

/* ---- measuring the QUP clocks with GCC's debug counter ------------------------------------------- */
/*
 * vendor debugcc-sm8150.c: gcc_qupv3_wrap2_core_2x_clk mux 0x184, core_clk 0x183, s0 0x185
 * (:677-682), GCC debug mux 0x62000 (sel, mask 0x3ff) / post-div 0x62004 / CBCR 0x62008
 * (:332-338), counter ctl 0x62038, status 0x6203c, XO/4 CBCR 0x43008 (:72-74). Procedure
 * from vendor clk-debug.c:42-122: a 1 ms and a 14 ms run; equal counts = the clock is off.
 * These two clocks are not in gcc-sm8150.c at all (no branch/vote to set): they follow the
 * QUP0 BCM. nvt_qup_measure=1.
 */
#define GCC_DEBUG_MUX           0x62000
#define GCC_DEBUG_POST_DIV      0x62004
#define GCC_DEBUG_CBCR          0x62008
#define GCC_DEBUG_CTL           0x62038
#define GCC_DEBUG_STATUS        0x6203c
#define GCC_XO_DIV4_CBCR        0x43008
#define DEBUG_CNT_DONE          (1u << 25)
#define DEBUG_CNT_EN            (1u << 20)
#define DEBUG_CNT_MASK          0x1ffffffu

static bool
qup_debug_run(uint32_t ticks, uint32_t *count)
{
	uint64_t deadline;

	qwr(gcc_base, GCC_DEBUG_CTL, ticks);
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	while (qrd(gcc_base, GCC_DEBUG_STATUS) & DEBUG_CNT_DONE) {
		if (mach_absolute_time() > deadline) {
			return false;
		}
	}
	qwr(gcc_base, GCC_DEBUG_CTL, DEBUG_CNT_EN | ticks);
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	while ((qrd(gcc_base, GCC_DEBUG_STATUS) & DEBUG_CNT_DONE) == 0) {
		if (mach_absolute_time() > deadline) {
			qwr(gcc_base, GCC_DEBUG_CTL, ticks);
			return false;
		}
	}
	*count = qrd(gcc_base, GCC_DEBUG_STATUS) & DEBUG_CNT_MASK;
	qwr(gcc_base, GCC_DEBUG_CTL, ticks);
	return true;
}

static void
qup_measure_one(uint32_t sel, const char *name)
{
	uint32_t xo4 = qrd(gcc_base, GCC_XO_DIV4_CBCR), shortc = 0, fullc = 0;
	bool ok;

	qwr(gcc_base, GCC_DEBUG_MUX, (qrd(gcc_base, GCC_DEBUG_MUX) & ~0x3ffu) | sel);
	qwr(gcc_base, GCC_DEBUG_POST_DIV, qrd(gcc_base, GCC_DEBUG_POST_DIV) & ~0xfu);    /* divide by 1 */
	qwr(gcc_base, GCC_DEBUG_CBCR, qrd(gcc_base, GCC_DEBUG_CBCR) | 1);
	qwr(gcc_base, GCC_XO_DIV4_CBCR, xo4 | 1);
	ok = qup_debug_run(0x1000, &shortc) && qup_debug_run(0x10000, &fullc);
	qwr(gcc_base, GCC_XO_DIV4_CBCR, xo4);
	if (!ok) {
		QUP_LOG("measure %s (mux %#x): the counter did not finish\n", name, sel);
	} else if (fullc == shortc) {
		QUP_LOG("measure %s (mux %#x): OFF (counts %u / %u)\n", name, sel, shortc, fullc);
	} else {
		uint64_t hz = (((uint64_t)fullc * 10) + 15) * 4800000ULL / ((0x10000ULL * 10) + 35);
		QUP_LOG("measure %s (mux %#x): %llu Hz\n", name, sel, hz);
	}
}

static void
qup_measure(const char *when)
{
	uint32_t on = 0;

	if (!PE_parse_boot_argn("nvt_qup_measure", &on, sizeof(on)) || !on) {
		return;
	}
	QUP_LOG("measure (%s):\n", when);
	qup_measure_one(0x184, "wrap2 core_2x");
	qup_measure_one(0x183, "wrap2 core");
	qup_measure_one(0x185, "wrap2 s0 (SE17)");
}

/* ---- P0 of a GPI (GSI) path: read-only look at gpi_dma2 and the apps SMMU ------------------------ */
/*
 * SE17 runs GSI-only firmware (runs 172327..175726: every M_CMD stalls with all clocks running).
 * The stock DT gives spi@c80000 dmas = <gpi_dma2 0 0 1 64 0>, <gpi_dma2 1 0 1 64 0>: gpi-dma at
 * 0xc00000 (0x60000), qcom,gpii-mask 0xfa, max 13 gpii, iommus SID 0x7b6 with qcom,smmu-cfg 1;
 * the apps SMMU is 0x15000000 (0x100000), "qcom,skip-init" (the boot chain set it up).
 * nvt_qup_gsi_probe=1 only reads:
 *   - per gpii in the mask: TX/RX channel context 0 (state [23:20], EE [7:4], proto [2:0]) and
 *     scratch 0 (pair/proto/seid), event ring context 0, type IRQ, glob IRQ status, error log
 *     (vendor msm_gpi_mmio.h:14-60,107,155,174,205,215; states gpi.c:225-232);
 *   - SMMU: sCR0, ID0, ID1, every SMR/S2CR, the entries that match SID 0x7b6 (GPI) and 0x7a3
 *     (the wrapper's own SID), and for a translating match its context bank's CBAR, SCTLR,
 *     TTBR0, TCR, FSR (mainline arm-smmu.h v6.6:27-125,144-197; page layout arm-smmu.c:1776-1785).
 * A line goes out before each block so a bus stall names it.
 */
#define GPI2_PHYS               0x00C00000UL
#define GPI2_SIZE               0x60000UL
#define GPI2_GPII_MASK          0xfau
#define GPI2_MAX_GPII           13
#define GPI_CH_CNTXT_0(n, k)    (0x20000 + 0x4000 * (n) + 0x80 * (k))
#define GPI_CH_SCRATCH_0(n, k)  (0x20060 + 0x4000 * (n) + 0x80 * (k))
#define GPI_EV_CNTXT_0(n, k)    (0x21000 + 0x4000 * (n) + 0x80 * (k))
#define GPI_TYPE_IRQ(n)         (0x23080 + 0x4000 * (n))
#define GPI_GLOB_IRQ_STTS(n)    (0x23100 + 0x4000 * (n))
#define GPI_GPII_IRQ_STTS(n)    (0x23118 + 0x4000 * (n))
#define GPI_ERROR_LOG(n)        (0x23200 + 0x4000 * (n))
#define APPS_SMMU_PHYS          0x15000000UL
#define APPS_SMMU_SIZE          0x100000UL
#define SMMU_SCR0               0x0
#define SMMU_ID0                0x20
#define SMMU_ID1                0x24
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CB_SCTLR           0x0
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58

static void
qup_gsi_probe_smmu_sid(vm_offset_t smmu, uint32_t sid, const char *who)
{
	uint32_t scr0 = qrd(smmu, SMMU_SCR0), id0 = qrd(smmu, SMMU_ID0), id1 = qrd(smmu, SMMU_ID1);
	uint32_t numsmrg = id0 & 0xff, pgshift = (id1 & (1u << 31)) ? 16 : 12;
	uint32_t numpage = 1u << (((id1 >> 28) & 7) + 1);
	bool exids = (scr0 & (1u << 3)) != 0, found = false;

	for (uint32_t i = 0; i < numsmrg; i++) {
		uint32_t smr = qrd(smmu, SMMU_SMR(i)), s2cr = qrd(smmu, SMMU_S2CR(i));
		bool valid = exids ? (s2cr & (1u << 10)) != 0 : (smr & (1u << 31)) != 0;
		uint32_t id = smr & 0xffff, mask = (smr >> 16) & (exids ? 0xffff : 0x7fff);

		if (!valid || ((sid ^ id) & ~mask & 0xffff) != 0) {
			continue;
		}
		found = true;
		uint32_t type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		QUP_LOG("gsi-probe: smmu SID %#x (%s) matches SMR%u %#x, S2CR %#x: %s, cb %u\n", sid, who, i, smr, s2cr,
		    type == 0 ? "TRANSLATE" : type == 1 ? "BYPASS" : type == 2 ? "FAULT" : "?", cb);
		if (type == 0 && ((numpage + cb + 1) << pgshift) <= APPS_SMMU_SIZE) {
			vm_offset_t cbase = smmu + ((numpage + cb) << pgshift);

			QUP_LOG("gsi-probe: cb %u: cbar %#x sctlr %#x ttbr0 %#x_%08x tcr %#x fsr %#x\n", cb,
			    qrd(smmu + (1u << pgshift), 4 * cb), qrd(cbase, SMMU_CB_SCTLR), qrd(cbase, SMMU_CB_TTBR0 + 4),
			    qrd(cbase, SMMU_CB_TTBR0), qrd(cbase, SMMU_CB_TCR), qrd(cbase, SMMU_CB_FSR));
		}
	}
	if (!found) {
		QUP_LOG("gsi-probe: smmu SID %#x (%s): no valid SMR matches; unmatched streams follow sCR0.USFCFG=%u "
		    "(1 = fault, 0 = bypass), CLIENTPD=%u\n", sid, who, (scr0 >> 10) & 1, scr0 & 1);
	}
}

static void
qup_gsi_probe(void)
{
	uint32_t on = 0;
	vm_offset_t gpi, smmu;

	if (!PE_parse_boot_argn("nvt_qup_gsi_probe", &on, sizeof(on)) || !on) {
		return;
	}
	QUP_LOG("gsi-probe: mapping gpi_dma2 at %#lx (reads only)\n", (unsigned long)GPI2_PHYS);
	gpi = ml_io_map(GPI2_PHYS, GPI2_SIZE);
	if (gpi == 0) {
		QUP_LOG("gsi-probe: cannot map gpi_dma2\n");
		return;
	}
	for (uint32_t n = 0; n < GPI2_MAX_GPII; n++) {
		if ((GPI2_GPII_MASK & (1u << n)) == 0) {
			continue;
		}
		QUP_LOG("gsi-probe: reading gpii %u\n", n);
		uint32_t tx = qrd(gpi, GPI_CH_CNTXT_0(n, 0)), rx = qrd(gpi, GPI_CH_CNTXT_0(n, 1));
		uint32_t ev = qrd(gpi, GPI_EV_CNTXT_0(n, 0));
		QUP_LOG("gsi-probe: gpii %u: tx ctx0 %#x (state %u ee %u proto %u) scratch %#x; rx ctx0 %#x (state %u ee %u) "
		    "scratch %#x; ev ctx0 %#x (state %u); type_irq %#x glob %#x gpii_irq %#x err %#x\n", n,
		    tx, (tx >> 20) & 0xf, (tx >> 4) & 0xf, tx & 7, qrd(gpi, GPI_CH_SCRATCH_0(n, 0)),
		    rx, (rx >> 20) & 0xf, (rx >> 4) & 0xf, qrd(gpi, GPI_CH_SCRATCH_0(n, 1)),
		    ev, (ev >> 20) & 0xf, qrd(gpi, GPI_TYPE_IRQ(n)), qrd(gpi, GPI_GLOB_IRQ_STTS(n)),
		    qrd(gpi, GPI_GPII_IRQ_STTS(n)), qrd(gpi, GPI_ERROR_LOG(n)));
	}

	QUP_LOG("gsi-probe: mapping the apps SMMU at %#lx (reads only)\n", (unsigned long)APPS_SMMU_PHYS);
	smmu = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	if (smmu == 0) {
		QUP_LOG("gsi-probe: cannot map the apps SMMU\n");
		return;
	}
	QUP_LOG("gsi-probe: smmu sCR0 %#x ID0 %#x (NUMSMRG %u, EXIDS %u) ID1 %#x (NUMCB %u)\n", qrd(smmu, SMMU_SCR0),
	    qrd(smmu, SMMU_ID0), qrd(smmu, SMMU_ID0) & 0xff, (qrd(smmu, SMMU_ID0) >> 8) & 1, qrd(smmu, SMMU_ID1),
	    qrd(smmu, SMMU_ID1) & 0xff);
	qup_gsi_probe_smmu_sid(smmu, 0x7b6, "gpi_dma2");
	qup_gsi_probe_smmu_sid(smmu, 0x7a3, "qupv3_2 wrapper");
	QUP_LOG("gsi-probe: done\n");
}

/* ---- GSI P1: an SMMU context for gpi_dma2 (SID 0x7b6) and gpii 1 allocated + started, polled ------- */
/*
 * P0 (run qup-v9): gpii 1,3-7 of gpi_dma2 are NOT ALLOCATED; SID 0x7b6 has no SMR and sCR0.USFCFG=1,
 * so GPI DMA would fault. nvt_qup_gsi=1 then:
 *   1. gives SID 0x7b6 a free SMR + context bank of ours, stage-1, 39-bit, our page tables mapping
 *      five pages at IOVA 0x100000 (the DT's qcom,iova-range start): event ring, TX ring, RX ring,
 *      TX data, RX data. The SMMU sequence is hakios.sound.c's space_on() (also vayu_audio.c,
 *      proven on vayu in run 180718 for stream 0x1b21), values unchanged.
 *   2. gpii 1 (vendor gpi_find_avail_gpii(): first bit of qcom,gpii-mask 0xfa): mainline gpi.c v6.6
 *      gpi_config_interrupts(DEFAULT) :622-683, gpi_alloc_ev_chan() :1307-1347 (64 << ev-factor 2 =
 *      256 events x 16 B), gpi_alloc_chan() x2 :1268-1305 (64 TREs x 16 B, scratch0 = pair, protocol
 *      1 SPI, seid 0 from the DT's dmas cells), gpi_start_chan() :1232-1250. Commands are confirmed
 *      by polling the context's state (gpi_cmd_info :395-425), at most 250 ms (CMD_TIMEOUT_MS).
 *   3. reads every context back, the SMMU fault status, then stops, resets and frees both
 *      channels and the event ring and gives the SMR/context back as found.
 * No TRE is queued in P1; the SE is not touched.
 */
#define GSI_GPII                1
/*
 * F2 (the bring-up notes): IOVA = the physical address of our own block.
 * The DT's 0x100000 is also GCC's physical base, so any untranslated GSI access there would land
 * in the clock controller; identity mapping makes a translation miss harmless.
 */
#define GSI_IOVA                gsi_iova
#define GSI_BLOCK               0x8000u         /* physically contiguous, 32 KB aligned: 5 x 4 KB used */
#define GSI_PAGES               5               /* ev, tx ring, rx ring, tx data, rx data */
#define GSI_GRANULE             4096            /* SMMU pages (TG0 4 KB), consecutive in the block */
#define GSI_EV_ELEMENTS         256
#define GSI_CH_ELEMENTS         64
#define GSI_EL_SIZE             16
#define GPI_CH_CTX(n, k)        (0x20000 + 0x4000 * (n) + 0x80 * (k))
#define GPI_CH_QOS(n, k)        (0x2005c + 0x4000 * (n) + 0x80 * (k))
#define GPI_CH_SCRATCH(n, k, i) (0x20060 + 0x4000 * (n) + 0x80 * (k) + 4 * (i))
#define GPI_EV_CTX(n)           (0x21000 + 0x4000 * (n))
#define GPI_CH_DB(n, k)         (0x22000 + 0x4000 * (n) + 0x8 * (k))
#define GPI_EV_DB(n)            (0x22100 + 0x4000 * (n))
#define GPI_CH_CMD(n)           (0x23008 + 0x4000 * (n))
#define GPI_EV_CMD(n)           (0x23010 + 0x4000 * (n))
#define GPI_TYPE_IRQ_MSK(n)     (0x23088 + 0x4000 * (n))
#define GPI_SRC_CH_IRQ(n)       (0x23090 + 0x4000 * (n))
#define GPI_SRC_EV_IRQ(n)       (0x23094 + 0x4000 * (n))
#define GPI_SRC_CH_IRQ_MSK(n)   (0x23098 + 0x4000 * (n))
#define GPI_SRC_EV_IRQ_MSK(n)   (0x2309c + 0x4000 * (n))
#define GPI_SRC_CH_IRQ_CLR(n)   (0x230a0 + 0x4000 * (n))
#define GPI_SRC_EV_IRQ_CLR(n)   (0x230a4 + 0x4000 * (n))
#define GPI_SRC_IEOB_MSK(n)     (0x230b8 + 0x4000 * (n))
#define GPI_SRC_IEOB_CLR(n)     (0x230c0 + 0x4000 * (n))      /* mainline gpi.c v6.6:177 */
#define GPI_GLOB_IRQ_EN(n)      (0x23108 + 0x4000 * (n))
#define GPI_GPII_IRQ_EN(n)      (0x23120 + 0x4000 * (n))
#define GPI_INTSET(n)           (0x23180 + 0x4000 * (n))
#define GPI_MSI_LSB(n)          (0x23188 + 0x4000 * (n))
#define GPI_MSI_MSB(n)          (0x2318c + 0x4000 * (n))
#define GPI_SCRATCH0(n)         (0x23400 + 0x4000 * (n))
#define GPI_SCRATCH1(n)         (0x23404 + 0x4000 * (n))
#define CTX_CONFIG              0x00
#define CTX_R_LENGTH            0x04
#define CTX_BASE_LSB            0x08
#define CTX_BASE_MSB            0x0c
#define CTX_RP_LSB              0x10
#define CTX_WP_LSB              0x18
#define CTX_INT_MOD             0x20
#define CTX_MSI_LSB             0x28
#define CTX_MSI_MSB             0x2c
#define CTX_RP_UPDATE_LSB       0x30
#define CTX_RP_UPDATE_MSB       0x34
#define GPI_OP_ALLOCATE         0
#define GPI_OP_START            1
#define GPI_OP_STOP             2
#define GPI_OP_RESET            9
#define GPI_OP_DE_ALLOC         10
/* hakios.sound.c: the apps SMMU context of ours (values as there) */
#define GS_SMMU_SGFSR           0x048
#define GS_SMMU_SGFSYNR1        0x054
#define GS_CBAR_STAGE_1         (1u << 16 | 3u << 8 | 0xfu << 12)
#define GS_CBA2R(n)             (0x800 + 4 * (n))
#define GS_CB_TCR2              0x10
#define GS_CB_TTBR1             0x28
#define GS_CB_MAIR0             0x38
#define GS_CB_MAIR1             0x3c
#define GS_CB_FAR               0x60
#define GS_CB_FSYNR0            0x68
#define GS_CB_TLBIASID          0x610
#define GS_CB_TLBSYNC           0x7f0
#define GS_CB_TLBSTATUS         0x7f4
#define GS_SCTLR_ON             (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define GS_TCR_39_BITS          (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define GS_TCR2                 (1u | 1u << 4 | 7u << 15)
#define GS_MAIR0                0xf404ff44
#define GS_FSR_ALL              0xc00001fe
#define GS_PTE                  (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
/*
 * Which GPI: gpi_dma2 (0xc00000, iommus SID 0x7b6, SE17's) by default; nvt_qup_gpi=0 picks gpi_dma0
 * (vayu.dtb qcom,gpi-dma@0x800000 :10882-10891, alias gpi_dma0 :28223: reg 0x800000/0x60000,
 * gpii-mask 0xfa, iommus SID 0xd6, behind anoc_1_tbu with USB/UFS) for the stop=4 single-RX-DMA test
 * only (T7). (v15 named 0xa00000/SID 0x616 "gpi_dma0": that is gpi_dma1, and its wrap1 AHB was not
 * voted; removed.) wrap0's AHB is voted first: 0x5200c bits 6 (m_ahb, halt 0x17004) and 7 (s_ahb,
 * halt 0x17008, hwcg), vendor gcc-sm8150.c:2966-2985.
 */
#define GS_SID                  gsi_sid
#define GS_WRAP_SID             0x7a3   /* the wrapper cb, see gsi_wrap_on() */
#define GS_SCTLR_S1_BYPASS      ((GS_SCTLR_ON & ~1u) | 3u << 22)
static bool gsi_wrap_on(void);
static uint32_t gsi_sid = 0x7b6;
static vm_offset_t gsi_gpi_phys = 0x00C00000UL;

static uint64_t gsi_iova;

static struct {
	vm_offset_t gpi, smmu, cb, gr1;
	vm_offset_t pages, tables;
	uint32_t match, context, old_smr, old_s2cr, old_sctlr, pgsize, numpage;
	bool smmu_on, ev_alloc, ch_alloc[2], ch_started[2];
	/* the wrapper's own stream (nvt_qup_wrap_sid) */
	vm_offset_t cb2;
	uint32_t wrap_mode, match2, context2, old_smr2, old_s2cr2, old_sctlr2;
	bool wrap_on;
} gsi;

static void
gsi_clean(vm_offset_t va, vm_size_t len)
{
	for (vm_offset_t line = va & ~63ul; line < va + len; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The rings and data: one 32 KB block inside the kernel image (__DATA), so it is physically
 * contiguous and below 4 GB: the loader puts the kernel at PA 0xf7004000 (run logs: "kernel loaded
 * at pa f7004000..."). v17's kmem_alloc_contig below 4 GB failed (kr 3) and the fallback block was
 * at 0x1919c8000. IOVA = PA in every mode, so T6 (bypass) and the translated runs see the same address.
 */
static uint8_t gsi_static_block[GSI_BLOCK] __attribute__((aligned(GSI_BLOCK)));

static bool
gsi_alloc_block(void)
{
	gsi.pages = (vm_offset_t)gsi_static_block;
	memset(gsi_static_block, 0, sizeof(gsi_static_block));
	gsi_iova = kvtophys(gsi.pages);
	QUP_LOG("gsi: ring block %#lx (kernel __DATA) = phys/IOVA %#llx (%u KB); PTE attrs %#llx (AttrIndx 0 -> MAIR0 "
	    "byte 0 %#x = Normal non-cacheable, SH outer); CPU side cleaned with dc civac before each doorbell\n",
	    (unsigned long)gsi.pages, gsi_iova, GSI_BLOCK / 1024, (unsigned long long)GS_PTE, GS_MAIR0 & 0xff);
	for (unsigned i = 1; i < GSI_BLOCK / PAGE_SIZE; i++) {
		if (kvtophys(gsi.pages + i * PAGE_SIZE) != gsi_iova + i * PAGE_SIZE) {
			QUP_LOG("gsi: the block is not physically contiguous (page %u at %#llx)\n", i,
			    (unsigned long long)kvtophys(gsi.pages + i * PAGE_SIZE));
			return false;
		}
	}
	/*
	 * 4 KB alignment is what the rings need (the event ring is 4 KB, the channel rings 1 KB) and what the
	 * SMMU tables map; the kernel image sits at PA 0xf7004000, so __DATA is 16 KB but not 32 KB aligned.
	 */
	if (gsi_iova == 0 || (gsi_iova >> 32) != 0 || (gsi_iova & 0xfffu) != 0) {
		QUP_LOG("gsi: the block at %#llx is unusable (zero, above 4 GB or not 4 KB aligned)\n", gsi_iova);
		return false;
	}
	return true;
}

static void
gsi_put(vm_offset_t base, uint32_t off, uint32_t val, const char *name)
{
	QUP_LOG("gsi: write %s (+%#x) = %#x\n", name, off, val);
	qwr(base, off, val);
}

/* Free SMR and context (reads only), as hakios.sound.c space_find(). */
static bool
gsi_smmu_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = qrd(gsi.smmu, SMMU_SCR0), id0 = qrd(gsi.smmu, SMMU_ID0), id1 = qrd(gsi.smmu, SMMU_ID1);
	uint32_t matches = id0 & 0xff, contexts = id1 & 0xff;

	if (scr0 & (1u << 3)) {
		QUP_LOG("gsi: smmu uses 16-bit stream ids (EXIDS); not written for that\n");
		return false;
	}
	gsi.pgsize = (id1 >> 31) ? 0x10000 : 0x1000;
	gsi.numpage = 2u << ((id1 >> 28) & 7);
	gsi.match = ~0u;
	for (uint32_t n = 0; n < matches; n++) {
		uint32_t smr = qrd(gsi.smmu, SMMU_SMR(n)), s2cr = qrd(gsi.smmu, SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((((smr & 0x7fff) ^ GS_SID) & ~((smr >> 16) & 0x7fff)) == 0) {
				QUP_LOG("gsi: SMR%u %#x covers SID %#x already (s2cr %#x); left alone\n", n, smr, GS_SID, s2cr);
				return false;
			}
		} else if (gsi.match == ~0u) {
			gsi.match = n;
			gsi.old_smr = smr;
			gsi.old_s2cr = s2cr;
		}
	}
	for (gsi.context = 0; gsi.context < contexts && used[gsi.context]; gsi.context++) {
	}
	if (gsi.match == ~0u || gsi.context >= contexts) {
		QUP_LOG("gsi: no free SMR or context\n");
		return false;
	}
	gsi.gr1 = gsi.smmu + gsi.pgsize;
	gsi.cb = gsi.smmu + (gsi.numpage + gsi.context) * gsi.pgsize;
	/* the wrapper's stream is mapped by default on the GSI transfer path (nvt_qup_gsi=2); nvt_qup_wrap_sid=0/1/2 */
	gsi.wrap_mode = qup_gsi_mode == 2 ? 1 : 0;
	PE_parse_boot_argn("nvt_qup_wrap_sid", &gsi.wrap_mode, sizeof(gsi.wrap_mode));
	if (gsi.wrap_mode != 0) {
		gsi.match2 = ~0u;
		for (uint32_t n = 0; n < matches; n++) {
			uint32_t smr = qrd(gsi.smmu, SMMU_SMR(n));

			if ((smr >> 31) && (((smr & 0x7fff) ^ GS_WRAP_SID) & ~((smr >> 16) & 0x7fff)) == 0) {
				QUP_LOG("gsi: SMR%u %#x covers the wrapper SID %#x already; left alone\n", n, smr, GS_WRAP_SID);
				return false;
			}
			if (!(smr >> 31) && n != gsi.match && gsi.match2 == ~0u) {
				gsi.match2 = n;
				gsi.old_smr2 = smr;
				gsi.old_s2cr2 = qrd(gsi.smmu, SMMU_S2CR(n));
			}
		}
		gsi.context2 = gsi.context;
		if (gsi.wrap_mode == 1) {
			for (gsi.context2 = gsi.context + 1; gsi.context2 < contexts && used[gsi.context2]; gsi.context2++) {
			}
		}
		if (gsi.match2 == ~0u || gsi.context2 >= contexts) {
			QUP_LOG("gsi: no second free SMR or context for the wrapper SID\n");
			return false;
		}
		gsi.cb2 = gsi.smmu + (gsi.numpage + gsi.context2) * gsi.pgsize;
		gsi.old_sctlr2 = qrd(gsi.cb2, SMMU_CB_SCTLR);
		if (gsi.wrap_mode == 1 && (gsi.old_sctlr2 & 1)) {
			QUP_LOG("gsi: context %u is ON with no stream: somebody's; left alone\n", gsi.context2);
			return false;
		}
		QUP_LOG("gsi: wrapper SID %#x: free SMR%u (smr %#x s2cr %#x), context %u (%s)\n", GS_WRAP_SID, gsi.match2,
		    gsi.old_smr2, gsi.old_s2cr2, gsi.context2, gsi.wrap_mode == 1 ? "own, S1 bypass" : "shared with the GPI");
	}
	gsi.old_sctlr = qrd(gsi.cb, SMMU_CB_SCTLR);
	QUP_LOG("gsi: free SMR%u (smr %#x s2cr %#x), context %u (sctlr %#x)\n", gsi.match, gsi.old_smr, gsi.old_s2cr,
	    gsi.context, gsi.old_sctlr);
	if (gsi.old_sctlr & 1) {
		QUP_LOG("gsi: context %u is ON with no stream: somebody's; left alone\n", gsi.context);
		return false;
	}
	return true;
}

static bool
gsi_smmu_on(void)
{
	uint64_t *root, *second, *third, ttbr;
	uint32_t sees = 1u << 31 | GS_SID, bypass = 0;

	/*
	 * T6: nvt_qup_gsi_bypass=1: the SMR's S2CR is BYPASS (type 1, mainline arm-smmu.h v6.6:113-117)
	 * and no context bank is programmed; the identity IOVA (below 4 GB) is then the address the
	 * fabric sees. Passes -> our stage-1 context wedges the TCU; dies -> below the SMMU.
	 */
	if (PE_parse_boot_argn("nvt_qup_gsi_bypass", &bypass, sizeof(bypass)) && bypass) {
		gsi_put(gsi.smmu, SMMU_S2CR(gsi.match), 1u << 16, "s2cr (BYPASS)");
		gsi_put(gsi.smmu, SMMU_SMR(gsi.match), sees, "smr");
		gsi.smmu_on = true;
		QUP_LOG("gsi: SID %#x BYPASSES the SMMU via SMR%u (smr %#x s2cr %#x)\n", GS_SID, gsi.match,
		    qrd(gsi.smmu, SMMU_SMR(gsi.match)), qrd(gsi.smmu, SMMU_S2CR(gsi.match)));
		return qrd(gsi.smmu, SMMU_SMR(gsi.match)) == sees;
	}

	if (kmem_alloc(kernel_map, &gsi.tables, 3 * PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		return false;
	}
	root = (uint64_t *)gsi.tables;
	second = (uint64_t *)(gsi.tables + PAGE_SIZE);
	third = (uint64_t *)(gsi.tables + 2 * PAGE_SIZE);
	root[GSI_IOVA >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[GSI_IOVA >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned i = 0; i < GSI_PAGES; i++) {
		third[(GSI_IOVA >> 12 & 0x1ff) + i] = (GSI_IOVA + i * GSI_GRANULE) | GS_PTE;
	}
	gsi_clean(gsi.tables, 3 * PAGE_SIZE);
	ttbr = kvtophys(gsi.tables) | (uint64_t)gsi.context << 48;
	QUP_LOG("gsi: context %u, tables at %#llx, %u pages at IOVA %#llx; SMR%u sends SID %#x there\n", gsi.context,
	    ttbr & 0x0000fffffffff000ull, GSI_PAGES, GSI_IOVA, gsi.match, GS_SID);
	gsi_put(gsi.cb, SMMU_CB_SCTLR, 0, "sctlr");
	gsi_put(gsi.gr1, GS_CBA2R(gsi.context), 1, "cba2r");
	gsi_put(gsi.gr1, 4 * gsi.context, GS_CBAR_STAGE_1, "cbar");
	gsi_put(gsi.cb, GS_CB_TCR2, GS_TCR2, "tcr2");
	gsi_put(gsi.cb, SMMU_CB_TCR, GS_TCR_39_BITS, "tcr");
	gsi_put(gsi.cb, SMMU_CB_TTBR0, (uint32_t)ttbr, "ttbr0 lo");
	gsi_put(gsi.cb, SMMU_CB_TTBR0 + 4, (uint32_t)(ttbr >> 32), "ttbr0 hi");
	gsi_put(gsi.cb, GS_CB_TTBR1, 0, "ttbr1 lo");
	gsi_put(gsi.cb, GS_CB_TTBR1 + 4, 0, "ttbr1 hi");
	gsi_put(gsi.cb, GS_CB_MAIR0, GS_MAIR0, "mair0");
	gsi_put(gsi.cb, GS_CB_MAIR1, 0, "mair1");
	gsi_put(gsi.cb, SMMU_CB_FSR, GS_FSR_ALL, "fsr");
	gsi_put(gsi.cb, GS_CB_TLBIASID, gsi.context, "tlbiasid");
	gsi_put(gsi.cb, GS_CB_TLBSYNC, 1, "tlbsync");
	for (unsigned ms = 0; qrd(gsi.cb, GS_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			QUP_LOG("gsi: TLB sync does not finish\n");
			return false;
		}
		qup_sleep_ms(1);
	}
	gsi_put(gsi.cb, SMMU_CB_SCTLR, GS_SCTLR_ON, "sctlr");
	gsi.smmu_on = true;
	gsi_put(gsi.smmu, SMMU_S2CR(gsi.match), gsi.context, "s2cr");
	gsi_put(gsi.smmu, SMMU_SMR(gsi.match), sees, "smr");
	if (qrd(gsi.smmu, SMMU_SMR(gsi.match)) != sees || (qrd(gsi.smmu, SMMU_S2CR(gsi.match)) & 0x300ff) != gsi.context ||
	    !(qrd(gsi.cb, SMMU_CB_SCTLR) & 1) || qrd(gsi.cb, SMMU_CB_TTBR0) != (uint32_t)ttbr) {
		QUP_LOG("gsi: the SMMU did not take it (smr %#x s2cr %#x sctlr %#x ttbr0 %#x)\n", qrd(gsi.smmu, SMMU_SMR(gsi.match)),
		    qrd(gsi.smmu, SMMU_S2CR(gsi.match)), qrd(gsi.cb, SMMU_CB_SCTLR), qrd(gsi.cb, SMMU_CB_TTBR0));
		return false;
	}
	QUP_LOG("gsi: SID %#x goes through context %u\n", GS_SID, gsi.context);
	return gsi_wrap_on();
}

/*
 * The wrapper's stream (nvt_qup_wrap_sid). vayu stock DT qcom,qupv3_2_geni_se@cc0000 (recovery-live.dts
 * :23636-23647): qcom,iommu-s1-bypass, child cb iommus = <apps_smmu 0x7a3 0x0>. The SPI driver maps its
 * data buffers through that cb device, not the GPI's (vendor spi-geni-qcom.c:774,785 ->
 * geni_se_iommu_map_buf(wrapper_dev), qcom-geni-se.c:1606), so the DMA TRE addresses are IOVAs of SID
 * 0x7a3; INFERENCE: the data transfers are mastered with that SID, and with no SMR for it they meet
 * USFCFG=1. Vendor S1 bypass (qcom-geni-se.c:1563-1569 DOMAIN_ATTR_S1_BYPASS) is a translating S2CR
 * to a context whose SCTLR has M clear and SHCFG = NSH (arm-smmu.c:1995-2012, :2465-2471; SCTLR_SHCFG
 * [23:22], arm-smmu-regs.h:211-213) -- not S2CR type BYPASS, which is what every bypass run here used.
 * =1: that, in a context of its own (IOVA = PA). =2: a second SMR into the GPI's identity-mapped
 * context (the data pages are inside the mapped block, IOVA = PA as well).
 */
static bool
gsi_wrap_on(void)
{
	uint32_t sees = 1u << 31 | GS_WRAP_SID;

	if (gsi.wrap_mode == 0) {
		return true;
	}
	if (gsi.wrap_mode == 1) {
		gsi_put(gsi.cb2, SMMU_CB_SCTLR, 0, "wrap sctlr");
		gsi_put(gsi.gr1, GS_CBA2R(gsi.context2), 1, "wrap cba2r");
		gsi_put(gsi.gr1, 4 * gsi.context2, GS_CBAR_STAGE_1, "wrap cbar");
		gsi_put(gsi.cb2, GS_CB_TCR2, GS_TCR2, "wrap tcr2");
		gsi_put(gsi.cb2, SMMU_CB_TCR, GS_TCR_39_BITS, "wrap tcr");
		gsi_put(gsi.cb2, GS_CB_MAIR0, GS_MAIR0, "wrap mair0");
		gsi_put(gsi.cb2, SMMU_CB_FSR, GS_FSR_ALL, "wrap fsr");
		gsi_put(gsi.cb2, SMMU_CB_SCTLR, GS_SCTLR_S1_BYPASS, "wrap sctlr (S1 bypass, NSH)");
	}
	gsi.wrap_on = true;
	gsi_put(gsi.smmu, SMMU_S2CR(gsi.match2), gsi.context2, "wrap s2cr");
	gsi_put(gsi.smmu, SMMU_SMR(gsi.match2), sees, "wrap smr");
	QUP_LOG("gsi: wrapper SID %#x goes through context %u (smr %#x s2cr %#x sctlr %#x)\n", GS_WRAP_SID, gsi.context2,
	    qrd(gsi.smmu, SMMU_SMR(gsi.match2)), qrd(gsi.smmu, SMMU_S2CR(gsi.match2)), qrd(gsi.cb2, SMMU_CB_SCTLR));
	return qrd(gsi.smmu, SMMU_SMR(gsi.match2)) == sees;
}

static void
gsi_wrap_off(void)
{
	if (!gsi.wrap_on) {
		return;
	}
	QUP_LOG("gsi: wrapper context %u: fsr %#x far %#x_%08x\n", gsi.context2, qrd(gsi.cb2, SMMU_CB_FSR),
	    qrd(gsi.cb2, GS_CB_FAR + 4), qrd(gsi.cb2, GS_CB_FAR));
	gsi_put(gsi.smmu, SMMU_SMR(gsi.match2), gsi.old_smr2, "wrap smr (as found)");
	gsi_put(gsi.smmu, SMMU_S2CR(gsi.match2), gsi.old_s2cr2, "wrap s2cr (as found)");
	if (gsi.wrap_mode == 1) {
		gsi_put(gsi.cb2, SMMU_CB_SCTLR, gsi.old_sctlr2, "wrap sctlr (as found)");
	}
	gsi.wrap_on = false;
}

static void
gsi_smmu_faults(const char *when)
{
	if (!gsi.smmu_on) {
		return;
	}
	QUP_LOG("gsi: smmu (%s): context fsr %#x far %#llx fsynr0 %#x; global sgfsr %#x sgfsynr1 %#x\n", when,
	    qrd(gsi.cb, SMMU_CB_FSR), (uint64_t)qrd(gsi.cb, GS_CB_FAR + 4) << 32 | qrd(gsi.cb, GS_CB_FAR),
	    qrd(gsi.cb, GS_CB_FSYNR0), qrd(gsi.smmu, GS_SMMU_SGFSR), qrd(gsi.smmu, GS_SMMU_SGFSYNR1));
	if (gsi.wrap_on) {
		QUP_LOG("gsi: smmu (%s): wrapper context %u fsr %#x far %#x_%08x fsynr0 %#x\n", when, gsi.context2,
		    qrd(gsi.cb2, SMMU_CB_FSR), qrd(gsi.cb2, GS_CB_FAR + 4), qrd(gsi.cb2, GS_CB_FAR), qrd(gsi.cb2, GS_CB_FSYNR0));
	}
}

static void
gsi_smmu_off(void)
{
	if (!gsi.smmu_on) {
		return;
	}
	gsi_smmu_faults("before giving it back");
	gsi_wrap_off();
	gsi_put(gsi.smmu, SMMU_SMR(gsi.match), gsi.old_smr, "smr (as found)");
	gsi_put(gsi.smmu, SMMU_S2CR(gsi.match), gsi.old_s2cr, "s2cr (as found)");
	gsi_put(gsi.cb, SMMU_CB_SCTLR, gsi.old_sctlr, "sctlr (as found)");
	gsi.smmu_on = false;
}

static void
gsi_ctx_show(const char *name, uint32_t base)
{
	QUP_LOG("gsi: %s ctx: cfg %#x len %#x base %#x_%08x rp %#x_%08x wp %#x_%08x\n", name,
	    qrd(gsi.gpi, base + CTX_CONFIG), qrd(gsi.gpi, base + CTX_R_LENGTH), qrd(gsi.gpi, base + CTX_BASE_MSB),
	    qrd(gsi.gpi, base + CTX_BASE_LSB), qrd(gsi.gpi, base + CTX_RP_LSB + 4), qrd(gsi.gpi, base + CTX_RP_LSB),
	    qrd(gsi.gpi, base + CTX_WP_LSB + 4), qrd(gsi.gpi, base + CTX_WP_LSB));
}

/* One channel (chid 0/1) or event-ring (chid < 0) command; true once the context shows `want`. */
static bool
gsi_cmd(int chid, uint32_t opcode, uint32_t want, const char *name)
{
	uint32_t ctx = chid < 0 ? GPI_EV_CTX(GSI_GPII) : GPI_CH_CTX(GSI_GPII, chid), state = 0;
	uint64_t deadline;

	if (chid < 0) {
		qwr(gsi.gpi, GPI_EV_CMD(GSI_GPII), opcode << 24);
	} else {
		qwr(gsi.gpi, GPI_CH_CMD(GSI_GPII), opcode << 24 | (uint32_t)chid);
	}
	clock_interval_to_deadline(250, NSEC_PER_MSEC, &deadline);
	do {
		state = (qrd(gsi.gpi, ctx) >> 20) & 0xf;
		if (state == want) {
			break;
		}
	} while (mach_absolute_time() < deadline);
	QUP_LOG("gsi: %s %s: state %u%s (src ch irq %#x ev irq %#x, glob %#x err %#x)\n", chid < 0 ? "ev" : chid ? "rx" : "tx",
	    name, state, state == want ? "" : " -- NOT what it should be", qrd(gsi.gpi, GPI_SRC_CH_IRQ(GSI_GPII)),
	    qrd(gsi.gpi, GPI_SRC_EV_IRQ(GSI_GPII)), qrd(gsi.gpi, GPI_GLOB_IRQ_STTS(GSI_GPII)),
	    qrd(gsi.gpi, GPI_ERROR_LOG(GSI_GPII)));
	qwr(gsi.gpi, GPI_SRC_CH_IRQ_CLR(GSI_GPII), qrd(gsi.gpi, GPI_SRC_CH_IRQ(GSI_GPII)));
	qwr(gsi.gpi, GPI_SRC_EV_IRQ_CLR(GSI_GPII), qrd(gsi.gpi, GPI_SRC_EV_IRQ(GSI_GPII)));
	return state == want;
}

static bool
gsi_gpii_up(void)
{
	uint32_t n = GSI_GPII;

	/* gpi_config_interrupts(DEFAULT_IRQ_SETTINGS): the interrupt lines are not routed in XNU. */
	gsi_put(gsi.gpi, GPI_TYPE_IRQ_MSK(n), 0x4f, "type irq msk");
	gsi_put(gsi.gpi, GPI_SRC_IEOB_MSK(n), 1, "ieob msk");
	gsi_put(gsi.gpi, GPI_SRC_CH_IRQ_MSK(n), 3, "ch irq msk");
	gsi_put(gsi.gpi, GPI_SRC_EV_IRQ_MSK(n), 1, "ev irq msk");
	gsi_put(gsi.gpi, GPI_GLOB_IRQ_EN(n), 1, "glob irq en (error only, as mainline/vendor)");
	gsi_put(gsi.gpi, GPI_GPII_IRQ_EN(n), 0xf, "gpii irq en");
	gsi_put(gsi.gpi, GPI_MSI_LSB(n), 0, "msi lsb");
	gsi_put(gsi.gpi, GPI_MSI_MSB(n), 0, "msi msb");
	gsi_put(gsi.gpi, GPI_SCRATCH0(n), 0, "scratch0");
	gsi_put(gsi.gpi, GPI_SCRATCH1(n), 0, "scratch1");
	gsi_put(gsi.gpi, GPI_INTSET(n), 1, "intset");
	gsi_put(gsi.gpi, GPI_ERROR_LOG(n), 0, "error log");

	/* gpi_alloc_ev_chan() */
	if (!gsi_cmd(-1, GPI_OP_ALLOCATE, 1, "allocate")) {
		return false;
	}
	gsi.ev_alloc = true;
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_CONFIG, GSI_EL_SIZE << 24 | 1u << 16 | 2, "ev cfg");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_R_LENGTH, GSI_EV_ELEMENTS * GSI_EL_SIZE, "ev len");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_BASE_LSB, (uint32_t)GSI_IOVA, "ev base lo");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_BASE_MSB, (uint32_t)(GSI_IOVA >> 32), "ev base hi");
	gsi_put(gsi.gpi, GPI_EV_DB(n) + 4, (uint32_t)(GSI_IOVA >> 32), "ev db hi");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_INT_MOD, 0, "ev int mod");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_MSI_LSB, 0, "ev msi lo");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_MSI_MSB, 0, "ev msi hi");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_RP_UPDATE_LSB, 0, "ev rp update lo");
	gsi_put(gsi.gpi, GPI_EV_CTX(n) + CTX_RP_UPDATE_MSB, 0, "ev rp update hi");
	gsi_put(gsi.gpi, GPI_EV_DB(n), (uint32_t)(GSI_IOVA + (GSI_EV_ELEMENTS - 1) * GSI_EL_SIZE), "ev db (ring full of room)");

	/* gpi_alloc_chan() for TX (chid 0, out) and RX (chid 1, in) */
	for (int k = 0; k < 2; k++) {
		uint64_t ring = GSI_IOVA + (uint64_t)(1 + k) * GSI_GRANULE;

		if (!gsi_cmd(k, GPI_OP_ALLOCATE, 1, "allocate")) {
			return false;
		}
		gsi.ch_alloc[k] = true;
		gsi_put(gsi.gpi, GPI_CH_CTX(n, k) + CTX_CONFIG, GSI_EL_SIZE << 24 | (k == 0 ? 1u : 0u) << 3 | 2, "ch cfg");
		gsi_put(gsi.gpi, GPI_CH_CTX(n, k) + CTX_R_LENGTH, GSI_CH_ELEMENTS * GSI_EL_SIZE, "ch len");
		gsi_put(gsi.gpi, GPI_CH_CTX(n, k) + CTX_BASE_LSB, (uint32_t)ring, "ch base lo");
		gsi_put(gsi.gpi, GPI_CH_CTX(n, k) + CTX_BASE_MSB, (uint32_t)(ring >> 32), "ch base hi");
		gsi_put(gsi.gpi, GPI_CH_DB(n, k) + 4, (uint32_t)(ring >> 32), "ch db hi");
		gsi_put(gsi.gpi, GPI_CH_SCRATCH(n, k, 0), (uint32_t)(!k) << 16 | 1u << 4 | 0, "ch scratch0 (pair, SPI, seid 0)");
		gsi_put(gsi.gpi, GPI_CH_SCRATCH(n, k, 1), 0, "ch scratch1");
		gsi_put(gsi.gpi, GPI_CH_SCRATCH(n, k, 2), 0, "ch scratch2");
		gsi_put(gsi.gpi, GPI_CH_SCRATCH(n, k, 3), 0, "ch scratch3");
		gsi_put(gsi.gpi, GPI_CH_QOS(n, k), 1, "ch qos");
	}
	for (int k = 0; k < 2; k++) {
		if (!gsi_cmd(k, GPI_OP_START, 2, "start")) {
			return false;
		}
		gsi.ch_started[k] = true;
	}
	return true;
}

/* false = a channel did not stop: DMA may still run, so the caller must leave the SMR in place. */
static bool
gsi_gpii_down(void)
{
	for (int k = 0; k < 2; k++) {
		if (gsi.ch_started[k]) {
			if (!gsi_cmd(k, GPI_OP_STOP, 3, "stop")) {
				QUP_LOG("gsi: channel %d did not stop; leaving gpii %u and the SMMU entry as they are\n", k, GSI_GPII);
				return false;
			}
			gsi.ch_started[k] = false;
		}
		if (gsi.ch_alloc[k]) {
			gsi_cmd(k, GPI_OP_RESET, 1, "reset");
			gsi_cmd(k, GPI_OP_DE_ALLOC, 0, "de-alloc");
			gsi.ch_alloc[k] = false;
		}
	}
	if (gsi.ev_alloc) {
		gsi_cmd(-1, GPI_OP_RESET, 1, "reset");
		gsi_cmd(-1, GPI_OP_DE_ALLOC, 0, "de-alloc");
		gsi.ev_alloc = false;
	}
	gsi_put(gsi.gpi, GPI_INTSET(GSI_GPII), 0, "intset off");
	gsi_put(gsi.gpi, GPI_TYPE_IRQ_MSK(GSI_GPII), 0, "type irq msk off");
	return true;
}

/*
 * T10 (the bring-up notes, H10-1): the NoC QoS ports of the QUP masters.
 * Vendor msm_bus programs every qcom,ap-owned master at probe (msm_bus_fabric_rpmh.c:1002-1026 ->
 * msm_bus_noc_qos_init(), msm_bus_noc_rpmh.c:337-377); nothing in UEFI/XNU does it for QUP.
 * Register = qos_base + base-offset + qos-off * qport + reg (NOC_QOS_REG_BASE and the *_ADDR macros,
 * msm_bus_noc_rpmh.c:38-120): MAINCTL_LOW 0x8 (DFLT_PRIO [6:4], URGFWD_EN [3], LIMIT_EN [0]), MODE 0xc,
 * BW 0x10, SAT 0x14, LIMITBW 0x18, REGUL0CTL 0x40, REGUL0BW 0x48.
 * Fabrics (vayu recovery DT ad-hoc-bus :18850-18853, fab-aggre1_noc/fab-aggre2_noc :19216-19238):
 * aggre1 0x16e0000 base-offset 0x4000, aggre2 0x1700000 base-offset 0x6000, qos-off 0x1000 for both.
 * Ports (qcom,qport, :19398-19660): aggre1 ufs 0, usb3_0 2, qup0 7; aggre2 crypto 0, sdc2 0xe,
 * qup1 0x30, qup2 0x31. All are prio 2; none of qup0/1/2 has lim-params/regulator/forwarding, and
 * their QoS needs no node-qos-clks, only the node's BCM (QUP0) vote (msm_bus_vote_qos_bcms) =
 * nvt_qup_bcm=2. QUP ports are read last, so a stall points at them. ufs and usb3_0 do need
 * node-qos-clks (aggre ufs phy axi / usb3 prim axi, :19436-19439, :19455-19458) which XNU does not
 * hold, so they are read only with nvt_qup_noc_ref=1.
 *
 * nvt_qup_noc_qos=1 then writes for qup0 and qup2 what msm_bus_noc_qos_init() leaves with those DT
 * values: MAINCTL_LOW prio 2, limiter and urgent forwarding off (noc_set_qos_dflt_prio/limiter/
 * forwarding :172-230, :299-315); LIMITBW bw/sat 0 (:200-224); REGUL0CTL and REGUL0BW 0 (the
 * regulator code writes each field read under its own mask back into a zero word, :232-297). MODE is
 * never written by vendor (NOC_QOS_MODEn_ADDR is unused), so it is left alone.
 */
static void
qup_noc_qos(const char *when)
{
	static const struct { int fab; uint32_t port; bool clk; const char *name; } p[] = {
		{ 0, 0, true, "aggre1 ufs" }, { 0, 2, true, "aggre1 usb3_0" }, { 1, 0, false, "aggre2 crypto" },
		{ 1, 0xe, false, "aggre2 sdc2" }, { 1, 0x30, false, "aggre2 qup1" }, { 0, 7, false, "aggre1 qup0" },
		{ 1, 0x31, false, "aggre2 qup2" },
	};
	static const uint32_t fab_phys[2] = { 0x16e0000, 0x1700000 }, fab_off[2] = { 0x4000, 0x6000 };
	static vm_offset_t fab[2];
	uint32_t fix = 0, ref = 0;

	if (fab[0] == 0) {
		fab[0] = ml_io_map(fab_phys[0], 0x40000);
		fab[1] = ml_io_map(fab_phys[1], 0x40000);
		if (fab[0] == 0 || fab[1] == 0) {
			QUP_LOG("noc qos: cannot map aggre1/aggre2\n");
			return;
		}
	}
	PE_parse_boot_argn("nvt_qup_noc_qos", &fix, sizeof(fix));
	PE_parse_boot_argn("nvt_qup_noc_ref", &ref, sizeof(ref));
	for (int pass = 0; pass < (fix ? 2 : 1); pass++) {
		for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
			vm_offset_t b = fab[p[i].fab];
			uint32_t r = fab_off[p[i].fab] + 0x1000 * p[i].port;

			if (pass == 1 && (p[i].port == 7 || p[i].port == 0x31)) {
				qwr(b, r + 0x8, (qrd(b, r + 0x8) & ~0x79u) | 2u << 4);
				qwr(b, r + 0x18, qrd(b, r + 0x18) & ~0x03ff07ffu);
				qwr(b, r + 0x40, 0);
				qwr(b, r + 0x48, 0);
				__asm__ volatile ("dsb sy" ::: "memory");
			} else if (pass == 1 || (p[i].clk && !ref)) {
				continue;
			}
			QUP_LOG("noc qos (%s%s): %s at %#x: mainctl %#x mode %#x bw %#x sat %#x limitbw %#x regul0ctl %#x regul0bw %#x\n",
			    when, pass ? ", after the vendor values" : "", p[i].name, fab_phys[p[i].fab] + r, qrd(b, r + 0x8),
			    qrd(b, r + 0xc), qrd(b, r + 0x10), qrd(b, r + 0x14), qrd(b, r + 0x18), qrd(b, r + 0x40), qrd(b, r + 0x48));
		}
	}
}

static void qup_gsi_dump(void);

/* P1. Returns after giving everything back. */
static void
qup_gsi_p1(void)
{
	if (qup_gsi_mode != 1) {
		return;
	}
	QUP_LOG("gsi: P1 begins (gpi_dma2 gpii %u, SID %#x)\n", GSI_GPII, GS_SID);
	qup_noc_qos("P1");
	gsi.gpi = ml_io_map(GPI2_PHYS, GPI2_SIZE);
	gsi.smmu = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	if (gsi.gpi == 0 || gsi.smmu == 0 ||
	    !gsi_alloc_block()) {
		QUP_LOG("gsi: cannot map GPI/SMMU or allocate the rings\n");
		return;
	}
	gsi_clean(gsi.pages, GSI_BLOCK);
	for (int k = 0; k < 2; k++) {
		uint32_t st = (qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, k)) >> 20) & 0xf;
		if (st != 0) {
			QUP_LOG("gsi: gpii %u channel %d is in state %u, not NOT ALLOCATED; stopping\n", GSI_GPII, k, st);
			return;
		}
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (gsi_smmu_find() && gsi_smmu_on()) {
		if (gsi_gpii_up()) {
			QUP_LOG("gsi: P1 PASSED: event ring allocated, TX and RX channels started on gpii %u\n", GSI_GPII);
			qup_gsi_dump();         /* C1: the SMR/GICD table in a run that returns */
		} else {
			QUP_LOG("gsi: P1 FAILED at the step above\n");
		}
		gsi_ctx_show("ev", GPI_EV_CTX(GSI_GPII));
		gsi_ctx_show("tx", GPI_CH_CTX(GSI_GPII, 0));
		gsi_ctx_show("rx", GPI_CH_CTX(GSI_GPII, 1));
		gsi_smmu_faults("after start");
		if (!gsi_gpii_down()) {
			lck_mtx_unlock(&hakios_smmu_lock);
			return;
		}
	}
	gsi_smmu_off();
	lck_mtx_unlock(&hakios_smmu_lock);
	QUP_LOG("gsi: P1 done; gpii %u and the SMMU are back as found\n", GSI_GPII);
}

/* ---- GSI P2: SPI transfers through gpii 1 (nvt_qup_gsi=2) ---------------------------------------- */
/*
 * The SE goes to GPI mode as mainline geni_se_select_gpi_mode() (qcom-geni-se.c v6.6:323-341:
 * SE_IRQ_EN 0, M_IRQ_EN without done/watermarks, GENI_DMA_MODE_EN, SE_GSI_EVENT_EN rx/tx/m/s).
 * A transfer is the TREs of mainline gpi_create_spi_tre() (gpi.c v6.6:1712-1787) as
 * spi-geni-qcom.c setup_gsi_xfer() (:456-544) asks for them: on TX always CONFIG0 (word size
 * 8-4, packing, CPOL/CPHA 0, clock source 0 / divider), GO (cmd TX 1 / RX 2 / DUPLEX 3
 * (linux/dma/qcom-gpi-dma.h), CS 0, no fragmentation, rx_len for RX and duplex), DMA (IEOT) when there
 * is TX data; on RX one DMA TRE. Then the TX and RX doorbells; completion = a transfer-complete
 * event (type 0x22) on the TX channel (spi_gsi_callback_result is on TX only), code EOT/EOB/
 * SUCCESS; 0x10 = error (gpi.c v6.6:223-228,239-246,1093-1133). Polled, 1 s at most. Data pages
 * at IOVA 0x103000 (TX) / 0x104000 (RX), one 4 KB granule each.
 */
#define TRE_DMA                 0x10
#define TRE_NOP                 0x01            /* MSM_GPI_TRE_NOP, vendor msm_gpi.h:22 */
#define TRE_GO                  0x20
#define TRE_CONFIG0             0x22
#define TRE_CHAIN               (1u << 0)
#define TRE_IEOB                (1u << 8)
#define TRE_IEOT                (1u << 9)
#define TRE_LINK                (1u << 11)
#define GSI_SPI_TX              1
#define GSI_SPI_RX              2
#define GSI_SPI_DUPLEX          3
#define EV_XFER_COMPLETE        0x22
#define EV_STALE                0xff
#define TCE_SUCCESS             1
#define TCE_EOT                 2
#define TCE_EOB                 4
#define TCE_UNEXP_ERR           16
#define GSI_EVENT_EN_ALL        0xfu            /* DMA_RX, DMA_TX, GENI_M, GENI_S (qcom-geni-se.c v6.6:168-172) */

static bool qup_gsi_active;
void vayu_qup_spi_fini(void);
static uint32_t qup_gsi_stop;                   /* nvt_qup_gsi_stop=1 after GPI mode, 2 after TREs, 3 after doorbells,
                                                   4 one RX DMA TRE before the SE is touched, 5 RX doorbell with
                                                   no element, 6 as 4 with no GPI read after the doorbell,
                                                   7 SE configured and gpii up, no GPI mode, 8 transfer doorbell
                                                   and then no GPI/SE/SMMU read */
static uint32_t qup_gsi_keep;                   /* nvt_qup_gsi_keep=1: fini reports and leaves everything up */
static void qup_snapshot(const char *when);
static void qup_se_regs(const char *when);
static uint32_t gsi_tx_i, gsi_rx_i, gsi_ev_rp_i, gsi_ev_wp_i;

static vm_offset_t
gsi_va(unsigned granule)
{
	return gsi.pages + granule * GSI_GRANULE;
}

static void
gsi_tre_put(int chid, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	uint32_t *i = chid ? &gsi_rx_i : &gsi_tx_i;
	volatile uint32_t *tre = (volatile uint32_t *)(gsi_va(1 + chid) + *i * GSI_EL_SIZE);

	tre[0] = d0;
	tre[1] = d1;
	tre[2] = d2;
	tre[3] = d3;
	gsi_clean((vm_offset_t)tre, GSI_EL_SIZE);
	GSI_XLOG("gsi: %s tre %u: %08x %08x %08x %08x\n", chid ? "rx" : "tx", *i, d0, d1, d2, d3);
	*i = (*i + 1) % GSI_CH_ELEMENTS;
}

static void
gsi_ring_db(int chid)
{
	uint32_t i = chid ? gsi_rx_i : gsi_tx_i;

	qwr(gsi.gpi, GPI_CH_DB(GSI_GPII, chid), (uint32_t)(GSI_IOVA + (uint64_t)(1 + chid) * GSI_GRANULE + i * GSI_EL_SIZE));
}

/*
 * F4: read-only preflight before the first GPI DMA (nvt_qup_gsi_dump=1): every valid SMR/S2CR of
 * the apps SMMU, and the distributor's state for INTID 621 (gpi_dma2 gpii 1 = SPI 0x24d), 405 and
 * 97 (fable-hang §5). GICD at 0x17a00000 (vayu.dtb); ISENABLER/IGROUPR/IROUTER as gic_v3_irq.c:48-56,
 * ISPENDR 0x200 + 4n (GICv3 architecture, same file's layout).
 */
#define GICD_PHYS_VAYU          0x17a00000UL
static void
qup_gsi_dump(void)
{
	uint32_t on = 0;
	static const uint32_t intids[] = { 621, 405, 97 };
	vm_offset_t gicd;

	if (!PE_parse_boot_argn("nvt_qup_gsi_dump", &on, sizeof(on)) || !on || gsi.smmu == 0) {
		return;
	}
	uint32_t matches = qrd(gsi.smmu, SMMU_ID0) & 0xff;
	for (uint32_t n = 0; n < matches; n++) {
		uint32_t smr = qrd(gsi.smmu, SMMU_SMR(n)), s2cr = qrd(gsi.smmu, SMMU_S2CR(n));
		if (smr >> 31) {
			QUP_LOG("dump: SMR%u %#x (sid %#x mask %#x) S2CR %#x (type %u cb %u)\n", n, smr, smr & 0x7fff,
			    (smr >> 16) & 0x7fff, s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
		}
	}
	gicd = ml_io_map(GICD_PHYS_VAYU, 0x10000);
	if (gicd == 0) {
		return;
	}
	for (unsigned i = 0; i < sizeof(intids) / sizeof(intids[0]); i++) {
		uint32_t id = intids[i];
		QUP_LOG("dump: GICD INTID %u: enabled %u pending %u group1 %u irouter %#x_%08x\n", id,
		    (qrd(gicd, 0x100 + 4 * (id / 32)) >> (id % 32)) & 1, (qrd(gicd, 0x200 + 4 * (id / 32)) >> (id % 32)) & 1,
		    (qrd(gicd, 0x80 + 4 * (id / 32)) >> (id % 32)) & 1, qrd(gicd, 0x6000 + 8 * id + 4), qrd(gicd, 0x6000 + 8 * id));
	}
}

/*
 * T0' (fable-hang §8): the TBUs' power. vayu.dtb: anoc_2_tbu@0x15189000 (stream-id-range 0x400+0x400,
 * ours 0x7b6) vdd-supply = gdsc@0x17d048 hlos1_vote_aggre_noc_mmu_tbu2_gdsc; anoc_1_tbu (0x000+0x400,
 * gpi_dma0's 0xd6, USB/UFS) = gdsc@0x17d044 tbu1; audio tbu = gdsc@0x17d040. GDSCR bit 31 PWR_ON,
 * bit 0 SW_COLLAPSE (mainline gdsc.c v6.6:20,26). nvt_qup_tbu2_on=1 clears SW_COLLAPSE in our own
 * HLOS vote register 0x17d048 (what gdsc enable does) and waits for PWR_ON (qcom,gds-timeout 500 us).
 */
static void
qup_tbu_gdsc(void)
{
	static const struct { uint32_t off; const char *name; } g[] = {
		{ 0x7d040, "audio tbu" }, { 0x7d044, "tbu1 (anoc_1)" }, { 0x7d048, "tbu2 (anoc_2, SID 0x400-0x7ff)" },
	};
	uint32_t on = 0;

	for (unsigned i = 0; i < 3; i++) {
		uint32_t v = qrd(gcc_base, g[i].off);
		QUP_LOG("gdsc %s at %#x: %#x (PWR_ON %u, SW_COLLAPSE %u)\n", g[i].name, 0x100000 + g[i].off, v, v >> 31, v & 1);
	}
	if (PE_parse_boot_argn("nvt_qup_tbu2_on", &on, sizeof(on)) && on && (qrd(gcc_base, 0x7d048) & 1)) {
		qwr(gcc_base, 0x7d048, qrd(gcc_base, 0x7d048) & ~1u);
		for (unsigned us = 0; us < 500 && !(qrd(gcc_base, 0x7d048) >> 31); us += 10) {
			qup_sleep_us(10);
		}
		QUP_LOG("gdsc tbu2: SW_COLLAPSE cleared; now %#x (PWR_ON %u)\n", qrd(gcc_base, 0x7d048), qrd(gcc_base, 0x7d048) >> 31);
	}
}

/* Bring-up for real use: SMMU + gpii 1 stay up (the SMR stays valid, so others leave it alone). */
static bool
qup_gsi_bringup(void)
{
	bool ok = false;

	if (qup_gsi_mode != 2) {
		return false;
	}
	uint32_t which = 2;
	PE_parse_boot_argn("nvt_qup_gsi_stop", &qup_gsi_stop, sizeof(qup_gsi_stop));
	if (PE_parse_boot_argn("nvt_qup_gpi", &which, sizeof(which)) && which == 0) {
		if (qup_gsi_stop != 4) {
			QUP_LOG("gsi: nvt_qup_gpi=0 is only for nvt_qup_gsi_stop=4; not bringing anything up\n");
			return false;
		}
		gsi_sid = 0xd6;
		gsi_gpi_phys = 0x00800000UL;
		qwr(gcc_base, 0x5200c, qrd(gcc_base, 0x5200c) | (1u << 6) | (1u << 7));
		qup_sleep_us(100);
		QUP_LOG("gsi: wrap0 ahb vote 0x5200c = %#x; m_ahb 0x17004 = %#x, s_ahb 0x17008 = %#x\n",
		    qrd(gcc_base, 0x5200c), qrd(gcc_base, 0x17004), qrd(gcc_base, 0x17008));
		if (qrd(gcc_base, 0x17004) & GCC_HALT_BIT) {
			QUP_LOG("gsi: wrap0 m_ahb is still off; not touching gpi_dma0\n");
			return false;
		}
	}
	QUP_LOG("gsi: P2 bring-up (gpi_dma%u at %#lx, gpii %u, SID %#x)\n", which == 0 ? 0 : 2, (unsigned long)gsi_gpi_phys,
	    GSI_GPII, GS_SID);
	qup_noc_qos("P2");
	qup_tbu_gdsc();
	gsi.gpi = ml_io_map(gsi_gpi_phys, GPI2_SIZE);
	gsi.smmu = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	if (gsi.gpi == 0 || gsi.smmu == 0) {
		QUP_LOG("gsi: cannot map GPI (%#lx) or the SMMU (%#lx)\n", (unsigned long)gsi.gpi, (unsigned long)gsi.smmu);
		return false;
	}
	if (!gsi_alloc_block()) {
		QUP_LOG("gsi: cannot allocate the ring block\n");
		return false;
	}
	gsi_clean(gsi.pages, GSI_BLOCK);
	for (int k = 0; k < 2; k++) {
		uint32_t st = (qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, k)) >> 20) & 0xf;
		if (st != 0) {
			QUP_LOG("gsi: gpii %u channel %d is in state %u, not NOT ALLOCATED; not using it\n", GSI_GPII, k, st);
			return false;
		}
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (gsi_smmu_find() && gsi_smmu_on()) {
		ok = gsi_gpii_up();
		if (!ok && gsi_gpii_down()) {
			gsi_smmu_off();
		}
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!ok) {
		QUP_LOG("gsi: P2 bring-up failed; nothing left on\n");
		return false;
	}
	/*
	 * Polled: transfer-completion (IEOB) interrupts off and cleared. Nothing in XNU takes the
	 * GPI's interrupt lines; a level IEOB left asserted is a suspect for the v11 hang.
	 */
	gsi_put(gsi.gpi, GPI_TYPE_IRQ_MSK(GSI_GPII), 0x4f & ~0x08u, "type irq msk (no IEOB)");
	gsi_put(gsi.gpi, GPI_SRC_IEOB_MSK(GSI_GPII), 0, "ieob msk off");
	gsi_put(gsi.gpi, GPI_SRC_IEOB_CLR(GSI_GPII), 1, "ieob clr");
	PE_parse_boot_argn("nvt_qup_gsi_stop", &qup_gsi_stop, sizeof(qup_gsi_stop));
	PE_parse_boot_argn("nvt_qup_gsi_keep", &qup_gsi_keep, sizeof(qup_gsi_keep));
	gsi_tx_i = gsi_rx_i = gsi_ev_rp_i = 0;
	gsi_ev_wp_i = GSI_EV_ELEMENTS - 1;
	QUP_LOG("gsi: gpii %u up, SMMU context %u for SID %#x\n", GSI_GPII, gsi.context, GS_SID);
	qup_gsi_dump();
	if (qup_gsi_stop == 6) {
		/*
		 * C3 (§10.2/10.4): as stop=4 up to the RX doorbell, then not one read of the GPI or the SMMU.
		 * Returns -> the CPU used to stall on the first GPI register read after the doorbell (S2, GSI
		 * wedged), not a dead fabric (S1) or a stuck return (S3). fini must not read the GPI either.
		 */
		gsi_tre_put(1, (uint32_t)(GSI_IOVA + 4 * GSI_GRANULE), (uint32_t)((GSI_IOVA + 4 * GSI_GRANULE) >> 32), 16,
		    TRE_DMA << 16);
		QUP_LOG("gsi: nvt_qup_gsi_stop=6: ringing the RX doorbell; no GPI/SMMU read after it\n");
		gsi_ring_db(1);
		for (uint32_t s = 1; s <= 10; s++) {
			qup_sleep_ms(1000);
			QUP_LOG("gsi: alive %u s after the doorbell\n", s);
		}
		qup_gsi_active = false;
		return false;
	}
	if (qup_gsi_stop == 5) {
		/*
		 * T8: the RX doorbell with WP = RP = the ring base, i.e. no new element (gpi_write_ch_db()
		 * writes the IOVA of WP, mainline gpi.c v6.6:732-740): the GSI is woken but has nothing to
		 * fetch. Dies -> the doorbell/firmware wake-up kills; lives -> the TRE fetch (first DMA) does.
		 */
		QUP_LOG("gsi: nvt_qup_gsi_stop=5: RX doorbell at rp %#x, no element\n",
		    qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_RP_LSB));
		gsi_ring_db(1);
		qup_sleep_ms(100);
		QUP_LOG("gsi: stop 5: rx ctx rp %#x wp %#x state %u; glob %#x err %#x\n",
		    qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_RP_LSB), qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_WP_LSB),
		    (qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1)) >> 20) & 0xf, qrd(gsi.gpi, GPI_GLOB_IRQ_STTS(GSI_GPII)),
		    qrd(gsi.gpi, GPI_ERROR_LOG(GSI_GPII)));
		gsi_smmu_faults("stop 5");
		qup_gsi_active = true;
		vayu_qup_spi_fini();
		return false;
	}
	if (qup_gsi_stop == 4) {
		/*
		 * F3/T1: the first DMA by SID 0x7b6 on its own: one RX DMA TRE (16 B into the RX page),
		 * RX doorbell only, SE untouched (it starts no transfer, so nothing should move except
		 * possibly the TRE fetch).
		 */
		gsi_tre_put(1, (uint32_t)(GSI_IOVA + 4 * GSI_GRANULE), (uint32_t)((GSI_IOVA + 4 * GSI_GRANULE) >> 32), 16,
		    TRE_DMA << 16);
		QUP_LOG("gsi: nvt_qup_gsi_stop=4: ringing the RX doorbell only\n");
		gsi_ring_db(1);
		qup_sleep_ms(100);
		QUP_LOG("gsi: stop 4: rx ctx rp %#x wp %#x; glob %#x err %#x\n", qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_RP_LSB),
		    qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_WP_LSB), qrd(gsi.gpi, GPI_GLOB_IRQ_STTS(GSI_GPII)),
		    qrd(gsi.gpi, GPI_ERROR_LOG(GSI_GPII)));
		gsi_smmu_faults("stop 4");
		qup_gsi_active = true;  /* so fini reports (keep) or takes it down */
		vayu_qup_spi_fini();
		return false;
	}
	return true;
}

/* geni_se_select_gpi_mode() */
static void
qup_gsi_select_mode(void)
{
	QUP_LOG("gsi: SE to GPI mode (SE_IRQ_EN 0xe1c, M_IRQ_EN 0x614, DMA_MODE_EN 0x258, GSI_EVENT_EN 0xe18)\n");
	qwr(se_base, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	qwr(se_base, SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	qup_reg_write(se_base, SE_IRQ_EN, 0, "se irq en (gsi)");
	qup_reg_write(se_base, SE_GENI_M_IRQ_EN,
	    qrd(se_base, SE_GENI_M_IRQ_EN) & ~(M_CMD_DONE | M_TX_FIFO_WATERMARK | M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST),
	    "m irq en (gsi)");
	qup_reg_write(se_base, SE_GENI_DMA_MODE_EN, 1, "dma mode en (gsi)");
	qup_reg_write(se_base, SE_GSI_EVENT_EN, qrd(se_base, SE_GSI_EVENT_EN) | GSI_EVENT_EN_ALL, "gsi event en");
	qup_gsi_active = true;
	qup_snapshot("gsi mode");
	qup_se_regs("gsi mode");
	if (qup_gsi_stop == 1) {
		QUP_LOG("gsi: nvt_qup_gsi_stop=1: SE in GPI mode; no transfer\n");
		qup_ready = false;
	}
}

/* Events from our read index up to the context's RP; true once TX completes (*err on an error code). */
static bool
gsi_events(bool *err)
{
	uint32_t rp = qrd(gsi.gpi, GPI_EV_CTX(GSI_GPII) + CTX_RP_LSB);
	uint32_t hw = (uint32_t)((rp - (uint32_t)GSI_IOVA) / GSI_EL_SIZE) % GSI_EV_ELEMENTS;
	bool tx_done = false;

	while (gsi_ev_rp_i != hw) {
		volatile uint32_t *ev = (volatile uint32_t *)(gsi_va(0) + gsi_ev_rp_i * GSI_EL_SIZE);
		uint32_t type, chid, code, len;

		gsi_clean((vm_offset_t)ev, GSI_EL_SIZE);
		type = (ev[3] >> 16) & 0xff;
		chid = ev[3] >> 24;
		code = ev[2] >> 24;
		len = ev[2] & 0xffffff;
		GSI_XLOG("gsi: event %u: %08x %08x %08x %08x (type %#x chid %u code %#x len %u)\n", gsi_ev_rp_i, ev[0], ev[1],
		    ev[2], ev[3], type, chid, code, len);
		if (type == EV_XFER_COMPLETE || type == 0x30) {
			if (code == TCE_UNEXP_ERR) {
				*err = true;
			}
			if (chid == 0) {
				tx_done = true;
			}
		}
		gsi_ev_rp_i = (gsi_ev_rp_i + 1) % GSI_EV_ELEMENTS;
		gsi_ev_wp_i = (gsi_ev_wp_i + 1) % GSI_EV_ELEMENTS;
		qwr(gsi.gpi, GPI_EV_DB(GSI_GPII), (uint32_t)(GSI_IOVA + gsi_ev_wp_i * GSI_EL_SIZE));
		qwr(gsi.gpi, GPI_SRC_IEOB_CLR(GSI_GPII), 1);
	}
	return tx_done;
}

static bool
gsi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t cmd = (tx && rx) ? GSI_SPI_DUPLEX : tx ? GSI_SPI_TX : GSI_SPI_RX;
	uint32_t div = (se_clock_hz + qup_os * qup_speed_hz - 1) / (qup_os * qup_speed_hz), go_flags;
	uint64_t deadline;
	bool err = false, done = false;

	if (length == 0 || length > GSI_GRANULE) {
		return false;
	}
	uint32_t shape = 0, ieot = 0;   /* nvt_qup_gsi_shape / nvt_qup_gsi_ieot, see below */
	PE_parse_boot_argn("nvt_qup_gsi_shape", &shape, sizeof(shape));
	PE_parse_boot_argn("nvt_qup_gsi_ieot", &ieot, sizeof(ieot));
	static uint8_t shape4_rx[1];
	if (shape == 4) {
		/* C7: RX-only, one byte; the received byte goes to our own buffer */
		tx = NULL;
		rx = shape4_rx;
		length = 1;
		cmd = GSI_SPI_RX;
	} else if (shape != 0) {
		rx = NULL;              /* RX TREs only in shapes 0 and 4 */
	}
	if (gsi_traced == 0) {
		PE_parse_boot_argn("nvt_qup_gsi_trace", &gsi_trace_max, sizeof(gsi_trace_max));
	}
	gsi_tracing = gsi_traced < gsi_trace_max;
	if (gsi_traced++ == gsi_trace_max) {
		QUP_LOG("gsi: %u transfers traced; from here on only failures (nvt_qup_gsi_trace=N for more)\n", gsi_trace_max);
	}
	GSI_XLOG("gsi: xfer cmd %u len %u begins\n", cmd, length);
	if (tx) {
		memcpy((void *)gsi_va(3), tx, length);
		gsi_clean(gsi_va(3), length);
	}
	if (rx) {
		memset((void *)gsi_va(4), 0, length);
		gsi_clean(gsi_va(4), length);
		gsi_tre_put(1, (uint32_t)(GSI_IOVA + 4 * GSI_GRANULE), (uint32_t)((GSI_IOVA + 4 * GSI_GRANULE) >> 32), length,
		    TRE_DMA << 16);
	}
	/*
	 * nvt_qup_gsi_shape (v21 3e: the first real transfer dies with the SE in GSI mode): what goes on the
	 * TX ring. 0 = vendor setup_gsi_xfer() (spi-geni-qcom.c:620-765): CONFIG0, GO, DMA. 1 = only the TX DMA
	 * TRE (GSI reads the buffer; no GO, so the SE gets no command). 2 = only CONFIG0 + GO (TX-only, length
	 * 0 on the bus side: the SE acts, nothing is read from memory). 3 = one NOP TRE (type 0x01,
	 * msm_gpi.h:22; no vendor user, so its handling is a HYPOTHESIS) with IEOT: the event-ring write
	 * alone, no SE. 4 = RX-only, 1 byte (RX DMA TRE, CONFIG0, GO cmd 2 rx_len 1 IEOB+LINK). 5 = CONFIG0 + GO
	 * cmd 8, IEOB. In 1-3 the last TRE has no chain and carries IEOT only with nvt_qup_gsi_ieot=1
	 * (3 always), i.e. without it the GSI should write no event. RX TREs only in shape 0.
	 */
	if (shape == 1) {
		if (!tx) {
			return false;
		}
		gsi_tre_put(0, (uint32_t)(GSI_IOVA + 3 * GSI_GRANULE), (uint32_t)((GSI_IOVA + 3 * GSI_GRANULE) >> 32), length,
		    TRE_DMA << 16 | (ieot ? TRE_IEOT : 0));
	} else if (shape == 2) {
		gsi_tre_put(0, 8 - 4 | 1u << 24 | 1u << 25, 0, (div & 0xfff) | qup_clk_sel << 16, TRE_CONFIG0 << 16 | TRE_CHAIN);
		gsi_tre_put(0, GSI_SPI_TX | 0u << 8, 0, 0, TRE_GO << 16 | (ieot ? TRE_IEOT : 0));
	} else if (shape == 3) {
		gsi_tre_put(0, 0, 0, 0, TRE_NOP << 16 | TRE_IEOT);
	} else if (shape == 5) {
		/*
		 * C7: CONFIG0 + GO with command 8 (SPI_CS_ASSERT, vendor spi-geni-qcom.c:86 -- a FIFO M_CMD
		 * opcode; that the GSI GO passes it on is a HYPOTHESIS), IEOB, no DMA.
		 */
		gsi_tre_put(0, 8 - 4 | 1u << 24 | 1u << 25, 0, (div & 0xfff) | qup_clk_sel << 16, TRE_CONFIG0 << 16 | TRE_CHAIN);
		gsi_tre_put(0, 8u | 0u << 8, 0, 0, TRE_GO << 16 | TRE_IEOB);
	} else {
		gsi_tre_put(0, 8 - 4 | 1u << 24 | 1u << 25, 0, (div & 0xfff) | qup_clk_sel << 16, TRE_CONFIG0 << 16 | TRE_CHAIN);
		go_flags = cmd == GSI_SPI_RX ? (TRE_IEOB | TRE_LINK) : cmd == GSI_SPI_TX ? TRE_CHAIN : (TRE_CHAIN | TRE_LINK);
		/* rx_len in words (8-bit words = bytes) for RX and duplex, 0 for TX-only (setup_gsi_xfer :700-716) */
		gsi_tre_put(0, cmd | 0u << 8, 0, cmd == GSI_SPI_TX ? 0 : length, TRE_GO << 16 | go_flags);
		if (tx) {
			gsi_tre_put(0, (uint32_t)(GSI_IOVA + 3 * GSI_GRANULE), (uint32_t)((GSI_IOVA + 3 * GSI_GRANULE) >> 32), length,
			    TRE_DMA << 16 | TRE_IEOT);
		}
	}
	if (qup_gsi_stop == 2) {
		QUP_LOG("gsi: nvt_qup_gsi_stop=2: TREs written; no doorbell\n");
		qup_ready = false;
		return false;
	}
	/*
	 * vendor order: dma_async_issue_pending(tx) before rx (setup_gsi_xfer :759-762); shape 4 rings RX
	 * first so the RX buffer is posted before the GO (§11 C7).
	 */
	GSI_XLOG("gsi: ringing %s doorbell(s) (shape %u, ieot %u, clk_sel %u, div %u)\n",
	    rx ? (shape == 4 ? "RX then TX" : "TX then RX") : "TX", shape, ieot, qup_clk_sel, div);
	if (shape == 4) {
		gsi_ring_db(1);
	}
	gsi_ring_db(0);
	if (rx && shape != 4) {
		gsi_ring_db(1);
	}
	if (qup_gsi_stop == 8) {
		/* (a): after the doorbell(s) not one read of the GPI, the SE or the SMMU, and no fini */
		for (uint32_t sec = 1; sec <= 10; sec++) {
			qup_sleep_ms(1000);
			QUP_LOG("gsi: alive %u s after the transfer doorbell\n", sec);
		}
		qup_gsi_active = false;
		qup_ready = false;
		return false;
	}
	if (qup_gsi_stop == 3) {
		qup_sleep_ms(100);
		QUP_LOG("gsi: nvt_qup_gsi_stop=3: doorbell rung, 100 ms later: tx rp %#x, ev rp %#x; not polling\n",
		    qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 0) + CTX_RP_LSB), qrd(gsi.gpi, GPI_EV_CTX(GSI_GPII) + CTX_RP_LSB));
		gsi_smmu_faults("stop 3");
		qup_ready = false;
		return false;
	}

	if (shape != 0) {
		qup_sleep_ms(100);
		qup_se_regs("100 ms after the transfer doorbell");
		QUP_LOG("gsi: 100 ms after: tx ctx state %u rp %#x wp %#x; ev rp %#x; glob %#x err %#x\n",
		    (qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 0)) >> 20) & 0xf, qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 0) + CTX_RP_LSB),
		    qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 0) + CTX_WP_LSB), qrd(gsi.gpi, GPI_EV_CTX(GSI_GPII) + CTX_RP_LSB),
		    qrd(gsi.gpi, GPI_GLOB_IRQ_STTS(GSI_GPII)), qrd(gsi.gpi, GPI_ERROR_LOG(GSI_GPII)));
	}
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	while (!done && mach_absolute_time() < deadline) {
		done = gsi_events(&err);
	}
	if (!done || err) {
		QUP_LOG("gsi: xfer cmd %u len %u %s; tx ctx rp %#x wp %#x, rx rp %#x, ev rp %#x; glob %#x err %#x\n", cmd, length,
		    err ? "ERROR event" : "NO completion in 1 s", qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 0) + CTX_RP_LSB),
		    qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 0) + CTX_WP_LSB), qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_RP_LSB),
		    qrd(gsi.gpi, GPI_EV_CTX(GSI_GPII) + CTX_RP_LSB), qrd(gsi.gpi, GPI_GLOB_IRQ_STTS(GSI_GPII)),
		    qrd(gsi.gpi, GPI_ERROR_LOG(GSI_GPII)));
		qup_snapshot("gsi xfer failed");
		gsi_smmu_faults("gsi xfer failed");
		if (shape != 0) {
			qup_se_regs("diagnostic shape, no completion");
			qup_ready = false;      /* one diagnostic transfer only */
		}
		return false;
	}
	if (rx) {
		gsi_clean(gsi_va(4), length);
		memcpy(rx, (const void *)gsi_va(4), length);
	}
	GSI_XLOG("gsi: xfer cmd %u len %u done\n", cmd, length);
	if (shape != 0) {
		/* diagnostic shapes: one transfer, report, stop the probe */
		qup_se_regs("after the completion event");
		QUP_LOG("gsi: shape %u COMPLETED (first byte %#x, rx ctx rp %#x); stopping the probe here\n", shape,
		    rx ? rx[0] : 0, qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_RP_LSB));
		qup_ready = false;
		return false;
	}
	return true;
}

/* After a probe-only run: everything back as found. */
void vayu_qup_spi_fini(void);
void
vayu_qup_spi_fini(void)
{
	if (!qup_gsi_active) {
		return;
	}
	QUP_LOG("gsi: taking it down\n");
	if (se_base != 0) {
		qup_reg_write(se_base, SE_GSI_EVENT_EN, 0, "gsi event en off");
		qup_reg_write(se_base, SE_GENI_DMA_MODE_EN, 0, "dma mode off");
	}
	if (qup_gsi_keep) {
		QUP_LOG("gsi: nvt_qup_gsi_keep=1: not taking it down; tx rp %#x rx rp %#x ev rp %#x glob %#x err %#x\n",
		    qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 0) + CTX_RP_LSB), qrd(gsi.gpi, GPI_CH_CTX(GSI_GPII, 1) + CTX_RP_LSB),
		    qrd(gsi.gpi, GPI_EV_CTX(GSI_GPII) + CTX_RP_LSB), qrd(gsi.gpi, GPI_GLOB_IRQ_STTS(GSI_GPII)),
		    qrd(gsi.gpi, GPI_ERROR_LOG(GSI_GPII)));
		gsi_smmu_faults("keep");
		return;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (gsi_gpii_down()) {
		gsi_smmu_off();
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	qup_gsi_active = false;
	qup_ready = false;
	QUP_LOG("gsi: down; gpii %u and the SMMU are back as found\n", GSI_GPII);
}

/* ---- the QUP core clock: an RPMh bus vote (BCM "QUP0") -------------------------------------------- */
/*
 * The GENI sequencer did not step on vayu (runs 172327, 172950): a data-less CS assert never
 * finished, and cancel/abort were not taken, while every AHB register read worked. The vendor
 * qcom-geni-se.c votes the wrapper's bus path before any SE use (se_geni_clks_on() ->
 * geni_se_add_ab_ib(), :1104-1117) with a clock plan of 0/19.2/50/.../236 MHz (:46-47); the
 * path is the DT's qcom,bus-mas-id 0x99 = mas-qhm-qup2, whose BCM is "QUP0" (vayu.dtb:
 * bcm-qup0 cell 0x1b80; mas-qhm-qup2 qcom,bcms = <bcm-qup0>). Mainline votes the same thing as
 * the "qup-core" path at CORE_2X_50_MHZ (spi-geni-qcom.c v6.6:1122; geni-se.h:295-300 "Core
 * 2X clock frequency to BCM threshold mapping"). XNU votes nothing, so the QUP core clock may
 * simply be off.
 *
 * nvt_qup_bcm=1 looks "QUP0" up in the command database and prints it; nvt_qup_bcm=2 also
 * sends one active-only vote through the apps RSC (DRV 2). The database's place on vayu is not
 * settled (the vendor node at 0xc3f000c is a pointer in AOP message RAM that the loader cannot
 * read; mainline sm8150.dtsi reserves 0x85f20000), so it comes from nvt_qup_cmd_db=<phys>;
 * nvt_qup_bcm_addr=<rpmh address> skips the lookup.
 *
 * Layouts: cmd-db header/entries and magic db 30 03 0c, slave id 5 = BCM (mainline cmd-db.c
 * v6.6:22-66,95; cmd-db.h:14); RSC v2 TCS registers at the TCS base (vayu.dtb apps_rsc
 * reg 0x18220d00), first TCS = an active one (qcom,tcs-config <2 2> first) (rpmh-rsc.c
 * v6.6:84-92,157-167); BCM command word (tcs.h v6.6:63-79).
 */
#define QUP_RSC_TCS_PHYS        0x18220d00UL
#define QUP_RSC_TCS_SIZE        0x1000UL
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_CMPL       0x10
#define TCS_CONTROL             0x14
#define TCS_AMC_ENABLE          (1u << 16)
#define TCS_AMC_TRIGGER         (1u << 24)
#define TCS_STATUS              0x18              /* per TCS: nonzero = idle */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define TCS_CMD_STATUS(n)       (0x3c + 20 * (n))
#define CMD_MSGID_WRITE_4       ((1u << 16) | (1u << 8) | 4)    /* write, response required, 4 bytes */
#define BCM_CMD(commit, valid, x, y) (((uint32_t)(commit) << 30) | ((uint32_t)(valid) << 29) | \
	                              (((x) & 0x3fffu) << 14) | ((y) & 0x3fffu))
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_SLAVE_BCM        5

struct qup_cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct qup_cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct qup_cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct qup_cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t
qup_bcm_lookup(uint64_t db_phys, const char *name)
{
	char id[8] = { 0 };

	memcpy(id, name, strnlen(name, sizeof(id)));   /* (a cmd-db id fills all 8 bytes with no NUL: "lnbclka2"; strlcpy kept 7) */
	const struct qup_cmd_db_header *db;
	vm_offset_t map;

	QUP_LOG("bcm: mapping the command database at %#llx\n", db_phys);
	map = ml_io_map_wcomb((vm_offset_t)(db_phys & ~0xfffULL), CMD_DB_SIZE + 0x1000);
	if (map == 0) {
		QUP_LOG("bcm: cannot map %#llx\n", db_phys);
		return 0;
	}
	db = (const struct qup_cmd_db_header *)(map + (db_phys & 0xfff));
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		QUP_LOG("bcm: no command database there (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return 0;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct qup_cmd_db_slave *slave = &db->slave[s];
		const struct qup_cmd_db_entry *entry = (const struct qup_cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_SLAVE_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, 8) == 0) {
				/* aux data: struct bcm_db { le32 unit; le16 width; u8 vcd; u8 reserved; } (mainline icc-rpmh.h) */
				const uint8_t *aux = db->data + slave->data_offset + entry[i].offset;

				QUP_LOG("bcm: \"%s\" is at RPMh address %#x (%u BCM entries); aux len %u: unit %u width %u vcd %u\n",
				    name, entry[i].address, slave->count, entry[i].length,
				    entry[i].length >= 4 ? (uint32_t)(aux[0] | aux[1] << 8 | aux[2] << 16 | (uint32_t)aux[3] << 24) : 0,
				    entry[i].length >= 6 ? (uint32_t)(aux[4] | aux[5] << 8) : 0, entry[i].length >= 7 ? aux[6] : 0);
				return entry[i].address;
			}
		}
		QUP_LOG("bcm: the BCM slave has %u entries, none is \"%s\"\n", slave->count, name);
	}
	return 0;
}

/* One active-only BCM vote through the first (active) TCS of the apps RSC; waits for completion. */
static bool
qup_bcm_vote_locked(uint32_t address, uint32_t data)
{
	vm_offset_t tcs = ml_io_map(QUP_RSC_TCS_PHYS, QUP_RSC_TCS_SIZE);
	uint32_t irq_enable;
	bool done = false;

	if (tcs == 0) {
		QUP_LOG("bcm: cannot map the RSC\n");
		return false;
	}
	irq_enable = qrd(tcs, TCS_IRQ_ENABLE);
	QUP_LOG("bcm: tcs0 status %#x control %#x irq en %#x st %#x cmd_en %#x\n", qrd(tcs, TCS_STATUS),
	    qrd(tcs, TCS_CONTROL), irq_enable, qrd(tcs, TCS_IRQ_STATUS), qrd(tcs, TCS_CMD_ENABLE));
	for (unsigned i = 0; qrd(tcs, TCS_STATUS) == 0; i++) {
		if (i > 1000) {
			QUP_LOG("bcm: tcs0 stays busy; not voting\n");
			return false;
		}
		qup_sleep_us(100);
	}
	qwr(tcs, TCS_CONTROL, qrd(tcs, TCS_CONTROL) & ~TCS_AMC_TRIGGER);
	qwr(tcs, TCS_CONTROL, qrd(tcs, TCS_CONTROL) & ~TCS_AMC_ENABLE);
	qwr(tcs, TCS_IRQ_CLEAR, 1);
	qwr(tcs, TCS_CMD_MSGID(0), CMD_MSGID_WRITE_4);
	qwr(tcs, TCS_CMD_ADDR(0), address);
	qwr(tcs, TCS_CMD_DATA(0), data);
	qwr(tcs, TCS_WAIT_FOR_CMPL, 1);
	qwr(tcs, TCS_CMD_ENABLE, 1);
	qwr(tcs, TCS_IRQ_ENABLE, irq_enable | 1);         /* status follows the enable; nothing routes the interrupt */
	qwr(tcs, TCS_CONTROL, TCS_AMC_ENABLE);
	qwr(tcs, TCS_CONTROL, TCS_AMC_ENABLE | TCS_AMC_TRIGGER);
	for (unsigned i = 0; i < 2000; i++) {
		if (qrd(tcs, TCS_IRQ_STATUS) & 1) {
			done = true;
			break;
		}
		qup_sleep_us(100);
	}
	QUP_LOG("bcm: vote %#x -> %#x %s; cmd status %#x\n", data, address, done ? "done" : "NOT done after 200 ms",
	    qrd(tcs, TCS_CMD_STATUS(0)));
	qwr(tcs, TCS_CONTROL, qrd(tcs, TCS_CONTROL) & ~TCS_AMC_TRIGGER);
	qwr(tcs, TCS_CONTROL, qrd(tcs, TCS_CONTROL) & ~TCS_AMC_ENABLE);
	qwr(tcs, TCS_CMD_ENABLE, 0);
	qwr(tcs, TCS_WAIT_FOR_CMPL, 0);
	qwr(tcs, TCS_IRQ_CLEAR, 1);
	qwr(tcs, TCS_IRQ_ENABLE, irq_enable);
	return done;
}

/* TCS 0 is shared with vayu_pas.c and vayu_cpufreq.c: one request at a time (vayu_rpmh_tcs_lock()). */
static bool
qup_bcm_vote(uint32_t address, uint32_t data)
{
	bool held = vayu_rpmh_tcs_lock(), done;

	done = qup_bcm_vote_locked(address, data);
	vayu_rpmh_tcs_unlock(held);
	return done;
}

/*
 * The path from the QUP2 master to memory in vayu.dtb: mas-qhm-qup2 (cell 0x99, BCM QUP0) ->
 * slv-qns-a2noc-snoc -> mas-qnm-aggre2-noc (BCM "SN11", bcm-sn11 cell 0x1b75). Mainline
 * interconnect/qcom/sm8150.c v6.6:1462-1466,1511-1515 has bcm_qup0 on qhm_qup2 and bcm_sn11 on qnm_aggre2_noc. Linux votes both before the
 * first GPI DMA; XNU voted neither (QUP0 only from v7 on). nvt_qup_bcm_sn11=1 votes SN11 too,
 * with the same value as QUP0.
 */
static void
qup_bcm_vote_name(uint64_t db, const char *name, uint32_t vote)
{
	uint32_t address = qup_bcm_lookup(db, name);

	if (address == 0 || ((address >> 16) & 0xf) != CMD_DB_SLAVE_BCM) {
		QUP_LOG("bcm: %s: %#x is not a BCM address; not voting\n", name, address);
		return;
	}
	QUP_LOG("bcm: voting %s (%#x) x=y=%#x\n", name, address, vote);
	qup_bcm_vote(address, BCM_CMD(1, 1, vote, vote));
}

static void
qup_core_vote(void)
{
	uint32_t mode = 0, address = 0, sn11 = 0;
	uint64_t db = 0x85f20000;       /* FACT on this phone: 3D stream run 163450, magic matches */

	if (!PE_parse_boot_argn("nvt_qup_bcm", &mode, sizeof(mode)) || mode == 0) {
		QUP_LOG("bcm: no QUP0 vote (nvt_qup_bcm=1 looks it up, =2 votes)\n");
		return;
	}
	if (!PE_parse_boot_argn("nvt_qup_bcm_addr", &address, sizeof(address))) {
		PE_parse_boot_argn("nvt_qup_cmd_db", &db, sizeof(db));
		address = qup_bcm_lookup(db, "QUP0");
	}
	if (address == 0 || ((address >> 16) & 0xf) != CMD_DB_SLAVE_BCM) {
		QUP_LOG("bcm: %#x is not a BCM address; not voting\n", address);
		return;
	}
	if (mode < 2) {
		QUP_LOG("bcm: lookup only (nvt_qup_bcm=1)\n");
		return;
	}
	/*
	 * Default: the smallest nonzero vote (v7 showed it completes but CS still stalls).
	 * nvt_qup_bcm_vote=<n> sets both x and y (0x3fff is the field's maximum, tcs.h v6.6:68).
	 */
	uint32_t vote = 1;
	PE_parse_boot_argn("nvt_qup_bcm_vote", &vote, sizeof(vote));
	qup_bcm_vote(address, BCM_CMD(1, 1, vote, vote));
	if (PE_parse_boot_argn("nvt_qup_bcm_sn11", &sn11, sizeof(sn11)) && sn11) {
		qup_bcm_vote_name(db, "SN11", vote);
	}
}

/* ---- GENI -------------------------------------------------------------------------------------- */

static void
qup_packing(uint32_t words)
{
	uint32_t cfg0, cfg1;

	/*
	 * One 8-bit SPI word per "vector"; MSB first. The vector fields are
	 * start[9:5], direction[4], length[3:1], stop[0], and four vectors are
	 * packed into CFG0 (0,1) and CFG1 (2,3) at 10 bits each. These are the
	 * values the vendor's se_get_packing_config() produces for bpw=8:
	 *
	 *   1 word:  cfg0 0x000000ff, cfg1 0
	 *   2 words: cfg0 0x0007fcfe, cfg1 0
	 *   4 words: cfg0 0x0007f8fe, cfg1 0x000ffefe
	 *
	 * (0xfe = start 7, dir 1, length 7, stop 0; the last vector gets stop 1.)
	 */
	if (words >= 4) {
		cfg0 = 0x0007f8fe;
		cfg1 = 0x000ffefe;
	} else if (words == 2) {
		cfg0 = 0x0007fcfe;
		cfg1 = 0;
	} else {
		cfg0 = 0x000000ff;
		cfg1 = 0;
	}
	qwr(se_base, SE_GENI_TX_PACKING_CFG0, cfg0);
	qwr(se_base, SE_GENI_TX_PACKING_CFG1, cfg1);
	qwr(se_base, SE_GENI_RX_PACKING_CFG0, cfg0);
	qwr(se_base, SE_GENI_RX_PACKING_CFG1, cfg1);
	/* 0 = 4x8, 1 = 2x16, 2 = 1x32; bpw/16 is 0 for 8-bit words. */
	qwr(se_base, SE_GENI_BYTE_GRAN, 8 / 16);
	QUP_LOG("packing: %u byte(s) per FIFO word, cfg0 %#x cfg1 %#x\n", words, cfg0, cfg1);
}

/*
 * Everything that says whether the sequencer runs: its IRQs and status, the command, the
 * FIFOs, the general-purpose length counter, the SE clock settings, and GCC's view of the
 * SE clock (branch 0x1e144, RCG 0x1e148/0x1e14c).
 */
static void
qup_snapshot(const char *when)
{
	QUP_LOG("%s: m_irq %#x s_irq %#x status %#x m_cmd0 %#x fifo tx %#x rx %#x gp_len %#x | "
	    "clk_ctrl_ro %#x m_clk_cfg %#x clk_sel %#x cgc %#x if_disable %#x | gcc se0 %#x rcg %#x/%#x\n",
	    when, qrd(se_base, SE_GENI_M_IRQ_STATUS), qrd(se_base, SE_GENI_S_IRQ_STATUS),
	    qrd(se_base, SE_GENI_STATUS), qrd(se_base, SE_GENI_M_CMD0), qrd(se_base, SE_GENI_TX_FIFO_STATUS),
	    qrd(se_base, SE_GENI_RX_FIFO_STATUS), qrd(se_base, SE_GENI_M_GP_LENGTH),
	    qrd(se_base, GENI_CLK_CTRL_RO), qrd(se_base, GENI_SER_M_CLK_CFG), qrd(se_base, SE_GENI_CLK_SEL),
	    qrd(se_base, GENI_CGC_CTRL), qrd(se_base, GENI_IF_FIFO_DISABLE_RO),
	    qrd(gcc_base, GCC_WRAP2_S0_HALT), qrd(gcc_base, GCC_WRAP2_S0_CMD), qrd(gcc_base, GCC_WRAP2_S0_CFG));
}

/*
 * v23: why the GENI sequencer does not execute (v5-v8: register M_CMDs never finish; v22: a GSI DMA
 * TRE pushing data into the SE wedges the SoC, CONFIG0+GO alone does not). Read-only dump of what the
 * mainline SE firmware-load path sets last (6.18 qcom-geni-se.c geni_load_se_fw() :1187-1295) and the
 * vendor SE/GENI registers (vendor qcom-geni-se.h:126-171): wrapper QUPV3_SE_AHB_M_CFG 0x118
 * (AHB_M_CLK_CGC_ON bit 0), QUPV3_COMMON_CFG 0x120, QUPV3_COMMON_CGC_CTRL 0x21c (COMMON_CSR_SLV_CLK_CGC_ON
 * bit 0) (mainline :179-191); SE GENI_SER_S_CLK_CFG 0x4c, SE_GENI_IOS 0x908, SE_GENI_S_GP_LENGTH 0x914,
 * SE_DMA_DEBUG_REG0 0xe40, SE_GENI_M_CMD_CTRL 0x604, SE_GENI_S_CMD0 0x630.
 * nvt_qup_se_ctl_read=1 also reads SE_GENI_CLK_CTRL 0x2000 (SER_CLK_SEL bit 0), SE_DMA_IF_EN 0x2004
 * (DMA_IF_EN bit 0), SE_FIFO_IF_DISABLE 0x2008 (mainline :230-232, :275-278). That page may be
 * TZ-protected: a write to 0x2008 hung the bus (run 1716xx), so it is opt-in and read last.
 */
static void
qup_se_regs(const char *when)
{
	uint32_t ctl = 0;

	QUP_LOG("se regs (%s): wrapper ahb_m_cfg %#x common_cfg %#x common_cgc %#x | status %#x m_cmd0 %#x m_cmd_ctrl %#x "
	    "m_irq %#x s_cmd0 %#x s_irq %#x ios %#x m_gp %#x s_gp %#x\n", when,
	    qrd(wrapper_base, 0x118), qrd(wrapper_base, 0x120), qrd(wrapper_base, 0x21c),
	    qrd(se_base, SE_GENI_STATUS), qrd(se_base, SE_GENI_M_CMD0), qrd(se_base, SE_GENI_M_CMD_CTRL),
	    qrd(se_base, SE_GENI_M_IRQ_STATUS), qrd(se_base, 0x630), qrd(se_base, SE_GENI_S_IRQ_STATUS),
	    qrd(se_base, 0x908), qrd(se_base, SE_GENI_M_GP_LENGTH), qrd(se_base, 0x914));
	QUP_LOG("se regs (%s): cgc %#x clk_ctrl_ro %#x m_clk %#x s_clk %#x clk_sel %#x | dma_mode %#x se_irq_en %#x "
	    "gsi_ev_en %#x dma_gen %#x dma_dbg0 %#x | init_cfg %#x/%#x fw %#x/%#x fifo tx %#x rx %#x\n", when,
	    qrd(se_base, GENI_CGC_CTRL), qrd(se_base, GENI_CLK_CTRL_RO), qrd(se_base, GENI_SER_M_CLK_CFG),
	    qrd(se_base, 0x4c), qrd(se_base, SE_GENI_CLK_SEL), qrd(se_base, SE_GENI_DMA_MODE_EN), qrd(se_base, SE_IRQ_EN),
	    qrd(se_base, SE_GSI_EVENT_EN), qrd(se_base, SE_DMA_GENERAL_CFG), qrd(se_base, 0xe40),
	    qrd(se_base, SE_GENI_INIT_CFG_REV), qrd(se_base, SE_GENI_S_INIT_CFG_REV), qrd(se_base, GENI_FW_REVISION_RO),
	    qrd(se_base, GENI_FW_S_REVISION_RO), qrd(se_base, SE_GENI_TX_FIFO_STATUS), qrd(se_base, SE_GENI_RX_FIFO_STATUS));
	if (PE_parse_boot_argn("nvt_qup_se_ctl_read", &ctl, sizeof(ctl)) && ctl) {
		QUP_LOG("se regs (%s): reading SE+0x2000/0x2004/0x2008 (may hang: TZ page?)\n", when);
		QUP_LOG("se regs (%s): clk_ctrl %#x dma_if_en %#x fifo_if_disable %#x\n", when,
		    qrd(se_base, 0x2000), qrd(se_base, 0x2004), qrd(se_base, 0x2008));
	}
}

/*
 * Candidate fixes, each flag-gated, each a write mainline geni_load_se_fw() does:
 * nvt_qup_wrap_cgc=1: wrapper QUPV3_SE_AHB_M_CFG |= AHB_M_CLK_CGC_ON and QUPV3_COMMON_CGC_CTRL |=
 *   COMMON_CSR_SLV_CLK_CGC_ON (:1223, :1226), i.e. hardware-controlled clock gating of the SE-side AHB
 *   master and the common CSR slave.
 * nvt_qup_se_dmaif=1: SE_DMA_IF_EN |= DMA_IF_EN (:1284) -- the SE's DMA/GSI data interface.
 * nvt_qup_se_clksel=1: SE_GENI_CLK_CTRL |= SER_CLK_SEL (:1280) -- serial clock to the sequencer.
 * The last two are in the 0x2000 page (see qup_se_regs); run them only after nvt_qup_se_ctl_read=1
 * returned and showed the bit clear.
 */
static void
qup_se_fixes(void)
{
	uint32_t on = 0;

	if (PE_parse_boot_argn("nvt_qup_wrap_cgc", &on, sizeof(on)) && on) {
		qup_reg_write(wrapper_base, 0x118, qrd(wrapper_base, 0x118) | 1u, "wrapper ahb_m_cfg (AHB_M_CLK_CGC_ON)");
		qup_reg_write(wrapper_base, 0x21c, qrd(wrapper_base, 0x21c) | 1u, "wrapper common cgc (CSR_SLV_CLK_CGC_ON)");
	}
	on = 0;
	if (PE_parse_boot_argn("nvt_qup_se_clksel", &on, sizeof(on)) && on) {
		qup_reg_write(se_base, 0x2000, qrd(se_base, 0x2000) | 1u, "se clk ctrl (SER_CLK_SEL)");
	}
	on = 0;
	if (PE_parse_boot_argn("nvt_qup_se_dmaif", &on, sizeof(on)) && on) {
		qup_reg_write(se_base, 0x2004, qrd(se_base, 0x2004) | 1u, "se dma if en (DMA_IF_EN)");
	}
}

/* Mainline handle_fifo_timeout(): cancel first, then abort; report whether either took. */
static void
qup_stop_cmd(void)
{
	qwr(se_base, SE_GENI_TX_WATERMARK, 0);
	qwr(se_base, SE_GENI_M_CMD_CTRL, M_GENI_CMD_CANCEL);
	qup_sleep_us(1000);
	QUP_LOG("cancel: m_irq %#x status %#x\n", qrd(se_base, SE_GENI_M_IRQ_STATUS), qrd(se_base, SE_GENI_STATUS));
	if (qrd(se_base, SE_GENI_STATUS) & 1) {
		qwr(se_base, SE_GENI_M_CMD_CTRL, M_GENI_CMD_ABORT);
		qup_sleep_us(1000);
		QUP_LOG("abort: m_irq %#x status %#x\n", qrd(se_base, SE_GENI_M_IRQ_STATUS),
		    qrd(se_base, SE_GENI_STATUS));
	}
	qwr(se_base, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
}

/*
 * A command with no data: if even CS assert never reports M_CMD_DONE, the sequencer does not
 * run at all (its clock), and the FIFO interface is not the problem.
 */
static bool
qup_cmd_nodata(uint32_t opcode, const char *name)
{
	uint64_t deadline;

	qwr(se_base, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	qup_snapshot(name);
	qwr(se_base, SE_GENI_M_CMD0, opcode << M_OPCODE_SHFT);
	clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (qrd(se_base, SE_GENI_M_IRQ_STATUS) & M_CMD_DONE) {
			QUP_LOG("cs test: %s done (m_irq %#x status %#x)\n", name, qrd(se_base, SE_GENI_M_IRQ_STATUS),
			    qrd(se_base, SE_GENI_STATUS));
			qwr(se_base, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
			return true;
		}
	}
	QUP_LOG("cs test: %s NOT done after 100 ms\n", name);
	qup_snapshot(name);
	qup_stop_cmd();
	return false;
}

static bool
qup_se_configure(void)
{
	uint32_t fw, proto, version, div;

	if (qup_stop == 3) {
		QUP_LOG("nvt_qup_stop=3: wrapper read done; stopping before the first SE read\n");
		return false;
	}
	QUP_LOG("se: first read of the SE (fw revision at %#x); a hang here is the SE clock or s_ahb\n",
	    GENI_FW_REVISION_RO);
	fw = qrd(se_base, GENI_FW_REVISION_RO);
	proto = (fw & FW_REV_PROTOCOL_MSK) >> FW_REV_PROTOCOL_SHFT;
	version = fw & FW_REV_VERSION_MSK;
	QUP_LOG("se: init_cfg %#x s_init_cfg %#x fw %#x (protocol %u, version %u) s_fw %#x\n",
	    qrd(se_base, SE_GENI_INIT_CFG_REV), qrd(se_base, SE_GENI_S_INIT_CFG_REV), fw, proto, version,
	    qrd(se_base, GENI_FW_S_REVISION_RO));

	if (proto != SE_PROTO_SPI) {
		QUP_LOG("the SE is not in SPI mode (protocol %u, not %u). The protocol firmware is loaded "
		    "before UEFI (by XBL), and XNU cannot load it. Boot Android once (which sets this SE to "
		    "SPI), then reboot straight into the loader without a power cycle.\n", proto, SE_PROTO_SPI);
		return false;
	}

	tx_fifo_width = (qrd(se_base, SE_HW_PARAM_0) & TX_FIFO_WIDTH_MSK) >> TX_FIFO_WIDTH_SHFT;
	tx_fifo_depth = (qrd(se_base, SE_HW_PARAM_0) & TX_FIFO_DEPTH_MSK) >> TX_FIFO_DEPTH_SHFT;
	rx_fifo_depth = (qrd(se_base, SE_HW_PARAM_1) & RX_FIFO_DEPTH_MSK) >> RX_FIFO_DEPTH_SHFT;
	QUP_LOG("se hw: tx %u words x %u bits, rx %u words; fifo_if_disable %u (0 = FIFO, 1 = GSI)\n",
	    tx_fifo_depth, tx_fifo_width, rx_fifo_depth, qrd(se_base, GENI_IF_FIFO_DISABLE_RO) & 1);

	if (tx_fifo_width == 0 || tx_fifo_depth == 0 || rx_fifo_depth == 0) {
		QUP_LOG("the FIFO parameters are zero (%#x/%#x); the SE is not responding\n",
		    qrd(se_base, SE_HW_PARAM_0), qrd(se_base, SE_HW_PARAM_1));
		return false;
	}

	/*
	 * GENI_IF_DISABLE_RO bit 0 set means the firmware prefers GPI DMA. Mainline
	 * spi-geni-qcom.c (v6.6, spi_geni_init: lines 691-712) then tries a GPI channel and,
	 * without one, falls back to FIFO mode with no further write; this does the same.
	 * The register is read-only: nothing is written to change it. (An earlier version
	 * wrote an unsourced "SE_FIFO_IF_DISABLE" at 0x2008, which hung the bus: run 171654.)
	 */
	if (qrd(se_base, SE_GENI_STATUS) & 1) {
		/*
		 * A command left active by an earlier run. Cancel/abort need the sequencer to step;
		 * the only harder reset, GCC_QUPV3_WRAPPER_2_BCR (0x1e000, vendor gcc-sm8150.c:4149),
		 * resets the whole wrapper and may drop the SE firmware XBL loaded, so it is not used.
		 */
		qup_snapshot("stale command at probe start");
		qup_stop_cmd();
		QUP_LOG("stale command %s\n", (qrd(se_base, SE_GENI_STATUS) & 1) ? "STILL ACTIVE (sequencer not stepping)" : "cleared");
	}
	if (qrd(se_base, GENI_IF_FIFO_DISABLE_RO) & 1) {
		QUP_LOG("fifo_if_disable is 1 (GPI DMA preferred); no GPI here, so FIFO mode as mainline does\n");
	}

	pack_words = tx_fifo_width / 8;
	if (pack_words < 1) {
		pack_words = 1;
	} else if (pack_words > 4) {
		pack_words = 4;
	}

	/* geni_se_init(): common CGC, output mux, no DMA, FIFO. */
	qup_reg_write(se_base, GENI_CGC_CTRL, qrd(se_base, GENI_CGC_CTRL) | GENI_CGC_DEFAULT, "geni cgc");
	qup_reg_write(se_base, SE_DMA_GENERAL_CFG,
	    qrd(se_base, SE_DMA_GENERAL_CFG) | DMA_CGC_DEFAULT, "dma cgc");
	qup_reg_write(se_base, GENI_OUTPUT_CTRL, 0x7f, "output ctrl");
	qup_reg_write(se_base, GENI_FORCE_DEFAULT, 1, "force default");
	qup_reg_write(se_base, SE_IRQ_EN, (1u << 2) | (1u << 3), "se irq en");
	qup_reg_write(se_base, SE_GENI_DMA_MODE_EN, qrd(se_base, SE_GENI_DMA_MODE_EN) & ~1u, "dma mode");
	qup_reg_write(se_base, SE_GSI_EVENT_EN, 0, "gsi event");
	qup_reg_write(se_base, SE_GENI_M_IRQ_CLEAR, 0xffffffffu, "m irq clear");
	qup_reg_write(se_base, SE_GENI_S_IRQ_CLEAR, 0xffffffffu, "s irq clear");
	qup_reg_write(se_base, SE_GENI_M_IRQ_EN,
	    M_CMD_DONE | M_TX_FIFO_WATERMARK | M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST | M_ERRORS, "m irq en");
	qup_reg_write(se_base, SE_GENI_S_IRQ_EN, 0x3e, "s irq en");

	qup_reg_write(se_base, SE_SPI_LOOPBACK, qrd(se_base, SE_SPI_LOOPBACK) & ~SPI_LOOPBACK_MSK, "spi loopback");
	qup_reg_write(se_base, SE_SPI_CPHA, qrd(se_base, SE_SPI_CPHA) & ~SPI_CPHA, "spi cpha");
	qup_reg_write(se_base, SE_SPI_CPOL, qrd(se_base, SE_SPI_CPOL) & ~SPI_CPOL, "spi cpol");
	qup_reg_write(se_base, SE_SPI_DEMUX_SEL, 0, "spi demux sel");
	qup_reg_write(se_base, SE_SPI_DEMUX_OUTPUT_INV, 0, "spi demux inv");
	qup_reg_write(se_base, SE_SPI_TRANS_CFG, qrd(se_base, SE_SPI_TRANS_CFG) & ~SPI_CS_TOGGLE, "spi trans cfg");
	qup_reg_write(se_base, SE_SPI_PRE_POST_CMD_DLY, 0, "spi pre/post delay");
	qup_reg_write(se_base, SE_SPI_DELAY_COUNTERS, 0, "spi delay counters");
	qup_reg_write(se_base, SE_SPI_WORD_LEN,
	    (qrd(se_base, SE_SPI_WORD_LEN) & ~SPI_WORD_LEN_MSK) | (8 - SPI_MIN_WORD_LEN), "spi word len");

	qup_packing(pack_words);

	/* vendor get_spi_clk_cfg(): div = DIV_ROUND_UP(sclk, oversampling * speed) (spi-geni-qcom.c:263) */
	div = (se_clock_hz + qup_os * qup_speed_hz - 1) / (qup_os * qup_speed_hz);
	if (div < 1) {
		div = 1;
	}
	qup_reg_write(se_base, SE_GENI_CLK_SEL, qup_clk_sel, "se clk sel");
	qup_reg_write(se_base, GENI_SER_M_CLK_CFG, (div << 4) | 1, "m clk cfg");
	qup_reg_write(se_base, SE_GENI_RX_WATERMARK, 0, "rx wm");
	qup_reg_write(se_base, SE_GENI_RX_RFR_WATERMARK, rx_fifo_depth > 2 ? rx_fifo_depth - 2 : 0, "rx rfr wm");

	QUP_LOG("se configured: SPI mode 0, 8-bit, %u Hz source / %u = %u Hz, %u bytes per FIFO word\n",
	    se_clock_hz, div, se_clock_hz / div, pack_words);
	qup_snapshot("configured");
	qup_se_regs("configured");
	qup_se_fixes();
	qup_se_regs("after the fixes");
	if (qup_cs_test) {
		qup_cmd_nodata(SPI_CS_ASSERT, "cs assert");
		qup_cmd_nodata(SPI_CS_DEASSERT, "cs deassert");
	}
	return true;
}

/* ---- one SPI transfer, polled ---------------------------------------------------------------- */

static uint32_t
qup_word_from_bytes(const uint8_t *buf, uint32_t offset, uint32_t bytes)
{
	uint32_t word = 0;

	for (uint32_t i = 0; i < bytes; i++) {
		word |= (uint32_t)buf[offset + i] << (8 * i);
	}
	return word;
}

static void
qup_bytes_from_word(uint32_t word, uint8_t *buf, uint32_t offset, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes; i++) {
		buf[offset + i] = (uint8_t)(word >> (8 * i));
	}
}


bool
vayu_qup_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint64_t deadline;
	uint32_t m_cmd, tx_sent = 0, rx_got = 0;
	uint32_t iterations = 0;

	if (!qup_ready || length == 0 || length > SPI_TRANS_LEN_MSK) {
		return false;
	}
	if (qup_gsi_active) {
		return gsi_xfer(tx, rx, length);
	}

	if (!tx && !rx) {
		return false;
	}
	if (tx && rx) {
		m_cmd = SPI_FULL_DUPLEX;
	} else if (tx) {
		m_cmd = SPI_TX_ONLY;
	} else {
		m_cmd = SPI_RX_ONLY;
	}

	qwr(se_base, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	qwr(se_base, SE_GENI_TX_WATERMARK, 0);
	if (tx) {
		qwr(se_base, SE_SPI_TX_TRANS_LEN, length & SPI_TRANS_LEN_MSK);
	}
	if (rx) {
		qwr(se_base, SE_SPI_RX_TRANS_LEN, length & SPI_TRANS_LEN_MSK);
	}

	if (qup_verbose) {
		QUP_LOG("xfer: cmd %u len %u tx %s rx %s; before: m_cmd0 %#x status %#x irq %#x fifo tx %#x rx %#x\n",
		    m_cmd, length, tx ? "yes" : "no", rx ? "yes" : "no", qrd(se_base, SE_GENI_M_CMD0),
		    qrd(se_base, SE_GENI_STATUS), qrd(se_base, SE_GENI_M_IRQ_STATUS),
		    qrd(se_base, SE_GENI_TX_FIFO_STATUS), qrd(se_base, SE_GENI_RX_FIFO_STATUS));
	}

	/* nvt_qup_prefill=1: words in first, so the FIFO count shows whether AHB writes land. */
	if (qup_prefill && tx) {
		while (tx_sent < length && tx_sent < tx_fifo_depth * pack_words) {
			uint32_t bytes = length - tx_sent < pack_words ? length - tx_sent : pack_words;

			qwr(se_base, SE_GENI_TX_FIFOn, qup_word_from_bytes(tx, tx_sent, bytes));
			tx_sent += bytes;
		}
		QUP_LOG("prefill: %u bytes written; tx fifo status %#x before M_CMD0\n", tx_sent,
		    qrd(se_base, SE_GENI_TX_FIFO_STATUS));
	}

	/* Start the command; the sequencer pulls bytes from the TX FIFO. */
	qwr(se_base, SE_GENI_M_CMD0, (m_cmd << M_OPCODE_SHFT));

	/* Fill what the TX FIFO holds. */
	if (tx && !qup_prefill) {
		uint32_t used = qrd(se_base, SE_GENI_TX_FIFO_STATUS) & TX_FIFO_WC_MSK;
		uint32_t space = used < tx_fifo_depth ? tx_fifo_depth - used : 0;

		while (space > 0 && tx_sent < length) {
			uint32_t bytes = length - tx_sent < pack_words ? length - tx_sent : pack_words;

			qwr(se_base, SE_GENI_TX_FIFOn, qup_word_from_bytes(tx, tx_sent, bytes));
			tx_sent += bytes;
			space--;
		}
	}

	if (qup_verbose) {
		qup_snapshot("after start+fill");
	}
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	uint64_t next_snap = 0;
	clock_interval_to_deadline(100, NSEC_PER_MSEC, &next_snap);

	for (;;) {
		uint32_t irq = qrd(se_base, SE_GENI_M_IRQ_STATUS);
		uint32_t rx_words, tx_room;

		iterations++;
		if (irq & M_ERRORS) {
			QUP_LOG("xfer error: irq %#x status %#x tx_fifo %#x rx_fifo %#x\n", irq,
			    qrd(se_base, SE_GENI_STATUS), qrd(se_base, SE_GENI_TX_FIFO_STATUS),
			    qrd(se_base, SE_GENI_RX_FIFO_STATUS));
			qup_stop_cmd();
			return false;
		}

		/* Drain everything the RX FIFO has. */
		rx_words = qrd(se_base, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MSK;
		while (rx_words > 0 && rx_got < length) {
			uint32_t word = qrd(se_base, SE_GENI_RX_FIFOn);
			uint32_t bytes = length - rx_got < pack_words ? length - rx_got : pack_words;

			if (rx != NULL) {
				qup_bytes_from_word(word, rx, rx_got, bytes);
			}
			rx_got += bytes;
			rx_words--;
		}

		/* Keep the TX FIFO fed. */
		if (tx != NULL && tx_sent < length) {
			uint32_t used = qrd(se_base, SE_GENI_TX_FIFO_STATUS) & TX_FIFO_WC_MSK;

			tx_room = used < tx_fifo_depth ? tx_fifo_depth - used : 0;
			while (tx_room > 0 && tx_sent < length) {
				uint32_t bytes = length - tx_sent < pack_words ? length - tx_sent : pack_words;

				qwr(se_base, SE_GENI_TX_FIFOn, qup_word_from_bytes(tx, tx_sent, bytes));
				tx_sent += bytes;
				tx_room--;
			}
		}

		if (irq & M_CMD_DONE) {
			/* The last word may have arrived with the done bit. */
			rx_words = qrd(se_base, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MSK;
			while (rx_words > 0 && rx_got < length) {
				uint32_t word = qrd(se_base, SE_GENI_RX_FIFOn);
				uint32_t bytes = length - rx_got < pack_words ? length - rx_got : pack_words;

				if (rx != NULL) {
					qup_bytes_from_word(word, rx, rx_got, bytes);
				}
				rx_got += bytes;
				rx_words--;
			}
			break;
		}

		if (qup_verbose && mach_absolute_time() > next_snap) {
			qup_snapshot("waiting");
			clock_interval_to_deadline(100, NSEC_PER_MSEC, &next_snap);
		}
		if (mach_absolute_time() > deadline) {
			QUP_LOG("xfer timeout after %u polls: irq %#x status %#x m_cmd0 %#x tx %u/%u rx %u/%u; "
			    "fifo tx %#x rx %#x\n", iterations, qrd(se_base, SE_GENI_M_IRQ_STATUS),
			    qrd(se_base, SE_GENI_STATUS), qrd(se_base, SE_GENI_M_CMD0), tx_sent, length, rx_got, length,
			    qrd(se_base, SE_GENI_TX_FIFO_STATUS), qrd(se_base, SE_GENI_RX_FIFO_STATUS));
			qup_stop_cmd();
			return false;
		}

		if ((iterations & 0x3ff) == 0) {
			qup_sleep_us(1);
		}
	}

	qwr(se_base, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	if (qup_verbose) {
		QUP_LOG("xfer done: %u polls, tx %u/%u, rx %u/%u, irq %#x status %#x\n", iterations,
		    tx_sent, length, rx_got, length, qrd(se_base, SE_GENI_M_IRQ_STATUS),
		    qrd(se_base, SE_GENI_STATUS));
	}
	if (rx != NULL && rx_got != length) {
		QUP_LOG("xfer short: rx %u of %u\n", rx_got, length);
		return false;
	}
	return true;
}

/* ---- entry ------------------------------------------------------------------------------------- */

static void
qup_parse_args(void)
{
	uint32_t value;

	if (PE_parse_boot_argn("nvt_qup_dry", &value, sizeof(value))) {
		qup_dry = value != 0;
	}
	if (PE_parse_boot_argn("nvt_qup_verbose", &value, sizeof(value))) {
		qup_verbose = value != 0;
	}
	PE_parse_boot_argn("nvt_qup_stop", &qup_stop, sizeof(qup_stop));
	if (PE_parse_boot_argn("nvt_qup_prefill", &value, sizeof(value))) {
		qup_prefill = value != 0;
	}
	if (PE_parse_boot_argn("nvt_qup_cs_test", &value, sizeof(value))) {
		qup_cs_test = value != 0;
	}
	/* DFS kept is the default on the GSI path (v24: the GO executes only with it); nvt_qup_dfs=0/1 overrides */
	PE_parse_boot_argn("nvt_qup_gsi", &qup_gsi_mode, sizeof(qup_gsi_mode));
	qup_dfs = qup_gsi_mode == 2;
	if (PE_parse_boot_argn("nvt_qup_dfs", &value, sizeof(value))) {
		qup_dfs = value != 0;
	}
	if (PE_parse_boot_argn("nvt_qup_os", &value, sizeof(value)) && (value == 1 || value == 2)) {
		qup_os = value;
	}
	if (PE_parse_boot_argn("nvt_qup_speed", &value, sizeof(value))) {
		if (value >= 100000 && value <= 4800000) {
			qup_speed_hz = value;
		}
	}
}

/* The wrapper/SE half of the bring-up: map, the wrapper's version, qup_se_configure(). */
static bool
qup_se_bringup(void)
{
	uint32_t hw_ver;

	wrapper_base = ml_io_map(QUP_WRAPPER_PHYS, QUP_WRAPPER_SIZE);
	se_base = ml_io_map(QUP_SE_PHYS, QUP_SE_SIZE);
	if (wrapper_base == 0 || se_base == 0) {
		QUP_LOG("cannot map the QUP wrapper or the SE\n");
		return false;
	}
	if (qup_stop == 2) {
		QUP_LOG("nvt_qup_stop=2: GCC done; stopping before the wrapper read\n");
		return false;
	}
	QUP_LOG("reading the wrapper hw version through s_ahb\n");
	hw_ver = qrd(wrapper_base, QUPV3_HW_VER);
	QUP_LOG("wrapper %#lx hw version %#x\n", (unsigned long)wrapper_base, hw_ver);

	if (qup_stop == 4) {
		QUP_LOG("nvt_qup_stop=4: SE read; stopping before the SE is configured (first SE writes)\n");
		return false;
	}
	return qup_se_configure();
}

bool
vayu_qup_spi_init(void)
{
	if (qup_ready) {
		return true;
	}
	qup_parse_args();

	QUP_LOG("begin: wrapper %#lx, se %#lx, gcc %#lx, speed %u Hz%s\n", (unsigned long)QUP_WRAPPER_PHYS,
	    (unsigned long)QUP_SE_PHYS, (unsigned long)GCC_PHYS, qup_speed_hz, qup_dry ? " (dry run)" : "");

	/*
	 * nvt_qup_nopins=1 (§10.4 C4): the GSI-only tests (stop=4/5/6) need neither the pins nor the SE, so
	 * they can run under the v2 loader (live log), which does not mux GPIO 55-58. The run stops after
	 * the GSI step; the SE is never driven on unmuxed pins.
	 */
	uint32_t nopins = 0;
	PE_parse_boot_argn("nvt_qup_nopins", &nopins, sizeof(nopins));
	if (!qup_pins_report() && !nopins) {
		return false;
	}
	if (qup_dry) {
		QUP_LOG("dry run: pins are read above; not touching GCC or the SE\n");
		return false;
	}

	if (!qup_gcc_init()) {
		return false;
	}
	qup_measure("before the QUP0 vote");
	qup_core_vote();
	qup_measure("after the QUP0 vote");
	qup_gsi_probe();
	qup_gsi_p1();
	/*
	 * nvt_qup_se_first=1: vendor order. spi-geni-qcom.c (vayu-r-oss) runs geni_se_init() (:936) before it
	 * requests and configures the GPI channels (:952 on), and selects GSI mode per message (:836). Without
	 * the flag the gpii is started first and qup_se_configure() (FIFO-mode init: DMA_MODE_EN 0,
	 * GSI_EVENT_EN 0, FORCE_DEFAULT, ...) then runs on an SE whose GSI channels are live; that was the
	 * case in every run that died (v11-v19), and in none that returned (v20 R2-R4 stopped before it).
	 */
	uint32_t se_first = 1;          /* vendor order is the default since v28; nvt_qup_se_first=0 = the old order */
	bool gsi_up = false, se_done = false;
	PE_parse_boot_argn("nvt_qup_se_first", &se_first, sizeof(se_first));
	if (se_first) {
		if (!qup_se_bringup()) {
			return false;
		}
		se_done = true;
	}
	gsi_up = qup_gsi_bringup();
	if (nopins == 1 && !gsi_up) {     /* nvt_qup_nopins=2 goes on to the SE (FIFO CS test without the loader's pins) */
		QUP_LOG("nvt_qup_nopins=1: GSI step over; not touching the SE (pins may not be muxed)\n");
		return false;
	}
	if (!se_done && !qup_se_bringup()) {
		return false;
	}
	if (gsi_up && qup_gsi_stop == 7) {
		QUP_LOG("gsi: nvt_qup_gsi_stop=7: SE configured %s gpii %u came up; no GPI mode, no TRE\n",
		    se_first ? "before" : "after", GSI_GPII);
		vayu_qup_spi_fini();
		return false;
	}
	if (gsi_up) {
		qup_gsi_select_mode();
		if (qup_gsi_stop == 1) {
			return false;           /* F1: v12's stop=1 fell through to qup_ready = true */
		}
	}
	qup_ready = true;
	if (qup_stop == 5) {
		QUP_LOG("nvt_qup_stop=5: SE configured; stopping before the first transfer\n");
		qup_ready = false;
		return false;
	}
	QUP_LOG("ready: SE17 (spi@c80000) through wrapper 2 (cc0000), %s, no interrupts, no TLMM writes\n",
	    qup_gsi_active ? "GPI DMA (gpi_dma2 gpii 1)" : "polled FIFO");
	return true;
}
