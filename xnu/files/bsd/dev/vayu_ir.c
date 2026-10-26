/*
 * The IR blaster of the POCO X3 Pro (vayu, SM8150-AC): QUP wrapper 0 SE1, spi@884000
 * (registers at physical 0x00884000, 0x4000 bytes, vayu.dtb), as a raw MOSI bit stream.
 *
 * There is no carrier logic anywhere in the phone's software stack: the vendor's ir-spi driver
 * sends the user's bytes as one spi_sync_transfer() with 32 bits per word at 1 920 000 Hz, a
 * 150 000-byte buffer (vendor-ir-spi.c:43-45, 62-107), so the bits themselves are the 38 kHz
 * waveform. This file does the same through GENI's FIFO mode: one write() is one TX-only SPI
 * command, the whole buffer goes out MSB first with 32-bit words.
 *
 * Byte order (this is the part that is easy to get wrong): mainline's geni_spi_handle_tx()
 * builds a FIFO word with `u8 *fifo_byte = (u8 *)&fifo_word; fifo_byte[j] = tx_buf[i++]`
 * (spi-geni-qcom.c:734-743), i.e. user byte 0 lands in FIFO word bits 7:0 on a little-endian
 * CPU. The packing that spi_setup_word_len() selects for 32 bits per word is MSB first
 * (spi-geni-qcom.c:286-305), so GENI shifts bit 31 out first: on the wire, the fourth byte of
 * every four-byte group leaves first (bit 7 of it first), then the third, the second and the
 * first. ir_fifo_word() below reproduces exactly that, so a buffer built for the vendor driver
 * is sent by this driver bit for bit the same.
 *
 * The loader owns everything outside the SE: pins 114-117 are muxed to qup1 (TLMM exception 7)
 * and the SE's clock is voted (GCC exception 7a, vayu_ir_clk=1) at the 19.2 MHz TCXO. XNU writes
 * no TLMM and no GCC register here, as the rest of this tree does. The SE's own divider
 * GENI_SER_M_CLK_CFG is set to 10 -> 1.92 MHz, and SE_GENI_CLK_SEL to 0 (no DFS level), which is
 * mainline's get_spi_clk_cfg()/geni_spi_set_clock_and_bw() for a divider with oversampling 1
 * (spi-geni-qcom.c:120-150, 308-348, 652-655). Mainline writes only M_CLK_CFG for an SPI master;
 * S_CLK_CFG is not touched (:333-336), and neither is it here.
 *
 * Everything is polled: XNU has no interrupt route for this QUP, so no ISR runs (the SE's IRQ
 * groups are enabled as mainline and vayu_bt.c:1645 do, but the GIC line is never routed). The
 * TX FIFO is fed on the M_TX_FIFO_WATERMARK status bit exactly as mainline's ISR would feed it
 * (spi-geni-qcom.c:715-751, 882-896), and M_CMD_DONE ends the transfer. Every wait is bounded;
 * the transfer's total budget is 2 x its duration at 1.92 MHz + 50 ms. A timeout runs mainline's
 * sequence, cancel then abort (spi-geni-qcom.c:152-225).
 *
 *   vayu_ir=1   map the SE, a read-only probe (protocol, status, clocks, FIFO), then /dev/vayu-ir
 *               (0666). Without the boot-arg nothing at all happens.
 *
 * vayu_ir_init() runs the probe in a kernel thread, like vayu_haptic_init() (vayu_haptic.c:
 * 1221-1277). Every line goes to the loader's log as well as the kernel's, through the same
 * tee as vayu_haptic.c:106-133. vayu_ir_quiesce(), called at the run's end and on a panic
 * (osfmk/arm64/loader_return.c), cancels/aborts a transfer in flight and logs with
 * loader_log_putc() only -- no printf, because interrupts are off there.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vayu_ir.h>

/* ---- the block -------------------------------------------------------------------------------- */

#define IR_SE_PHYS              0x00884000UL    /* spi@884000, QUP wrapper 0 SE1 (vayu.dtb) */
#define IR_SE_SIZE              0x4000UL        /* its "reg" size there */

/* ---- GENI / SE registers (offsets inside spi@884000) ------------------------------------------ */

#define GENI_FORCE_DEFAULT_REG  0x020           /* geni-se.h:84; written qcom-geni-se.c:335 */
#define GENI_OUTPUT_CTRL        0x024           /* geni-se.h:85; qcom-geni-se.c:334 */
#define GENI_CGC_CTRL           0x028           /* qcom-geni-se.c:198; qcom-geni-se.c:325-327 */
#define SE_GENI_STATUS          0x040           /* geni-se.h:86 */
#define GENI_SER_M_CLK_CFG      0x048           /* geni-se.h:87; spi-geni-qcom.c:336 */
#define GENI_IF_DISABLE_RO      0x064           /* geni-se.h:89 (FIFO_IF_DISABLE bit 0, :144) */
#define GENI_FW_REVISION_RO     0x068           /* geni-se.h:90; geni-se.h:332-339 */
#define SE_GENI_CLK_SEL         0x07c           /* geni-se.h:91; spi-geni-qcom.c:335 */
#define SE_SPI_CPHA             0x224           /* spi-geni-qcom.c:24 */
#define SE_SPI_LOOPBACK         0x22c           /* spi-geni-qcom.c:27 */
#define SE_SPI_CPOL             0x230           /* spi-geni-qcom.c:32 */
#define SE_SPI_DEMUX_OUTPUT_INV 0x24c           /* spi-geni-qcom.c:35 */
#define SE_SPI_DEMUX_SEL        0x250           /* spi-geni-qcom.c:38 */
#define SE_GENI_BYTE_GRAN       0x254           /* qcom-geni-se.c:202; qcom-geni-se.c:582-583 */
#define SE_GENI_DMA_MODE_EN     0x258           /* geni-se.h:93 */
#define SE_SPI_TRANS_CFG        0x25c           /* spi-geni-qcom.c:41 */
#define SE_GENI_TX_PACKING_CFG0 0x260           /* qcom-geni-se.c:203; written :567 */
#define SE_GENI_TX_PACKING_CFG1 0x264           /* qcom-geni-se.c:204; written :568 */
#define SE_SPI_WORD_LEN         0x268           /* spi-geni-qcom.c:44; written :305 */
#define SE_SPI_TX_TRANS_LEN     0x26c           /* spi-geni-qcom.c:48; written :841 */
#define SE_GENI_RX_PACKING_CFG0 0x284           /* qcom-geni-se.c:205; written :571 */
#define SE_GENI_RX_PACKING_CFG1 0x288           /* qcom-geni-se.c:206; written :572 */
#define SE_GENI_M_CMD0          0x600           /* geni-se.h:94; geni-se.h:350-356 */
#define SE_GENI_M_CMD_CTRL_REG  0x604           /* geni-se.h:95; geni-se.h:386-414 */
#define SE_GENI_M_IRQ_STATUS    0x610           /* geni-se.h:96 */
#define SE_GENI_M_IRQ_EN        0x614           /* geni-se.h:97 */
#define SE_GENI_M_IRQ_CLEAR     0x618           /* geni-se.h:98 (write-1-to-clear, qcom-geni-se.c:341) */
#define SE_GENI_S_IRQ_CLEAR     0x648           /* geni-se.h:105; qcom-geni-se.c:342 */
#define SE_GENI_TX_FIFOn        0x700           /* geni-se.h:108; written spi-geni-qcom.c:743 */
#define SE_GENI_TX_WATERMARK_REG 0x80c          /* geni-se.h:112; spi-geni-qcom.c:895 */
#define SE_GENI_TX_FIFO_STATUS  0x800           /* geni-se.h:110; TX_FIFO_WC [27:0], :238-239 */
#define IR_TX_FIFO_WC_MSK       0x0fffffffu
#define SE_GSI_EVENT_EN         0xe18           /* qcom-geni-se.c:226; cleared :318, :340 */
#define SE_IRQ_EN               0xe1c           /* qcom-geni-se.c:227 */
#define SE_HW_PARAM_0           0xe24           /* geni-se.h:125; fields :273-281 */
#define SE_DMA_GENERAL_CFG      0xe30           /* qcom-geni-se.c:228; qcom-geni-se.c:329-332 */

/* GENI_FW_REVISION_RO: [15:8] is the protocol the SE's loaded firmware speaks (geni-se.h:146-148) */
#define IR_FW_REV_PROTOCOL_MSK  (0xffu << 8)
#define IR_FW_REV_PROTOCOL_SHFT 8
#define IR_FW_REV_VERSION_MSK   0xffu
#define IR_SE_PROTO_SPI         1               /* geni-se.h:35 */

/* SE_GENI_STATUS (geni-se.h:134-136) */
#define IR_M_ACTIVE             (1u << 0)       /* M_GENI_CMD_ACTIVE */

/* GENI_SER_M_CLK_CFG (geni-se.h:138-141) */
#define IR_SER_CLK_EN           (1u << 0)
#define IR_SER_CLK_DIV_SHFT     4

/* SE_GENI_CLK_SEL: CLK_SEL_MSK is GENMASK(2,0) (geni-se.h:150-151); 0 = the fixed source. */

/* SE_IRQ_EN: the four group enables, as geni_se_io_set_mode() and vayu_bt.c:1645 set them. */
#define IR_SE_IRQ_BITS          0x0fu

/* SE_GENI_DMA_MODE_EN (geni-se.h:156-157) */
#define IR_GENI_DMA_MODE_EN     (1u << 0)

/* geni_se_init()'s defaults (qcom-geni-se.c:242-252, 266-270, 305-335) */
#define IR_GENI_CGC_DEFAULT     0x7fu           /* DEFAULT_CGC_EN, qcom-geni-se.c:252 */
#define IR_DMA_CGC_DEFAULT      0x0fu           /* four DMA CGC bits, qcom-geni-se.c:267-270 */
#define IR_GENI_OUTPUT_DEFAULT  0x7fu           /* DEFAULT_IO_OUTPUT_CTRL_MSK, qcom-geni-se.c:240 */
#define IR_FORCE_DEFAULT        1u              /* FORCE_DEFAULT, geni-se.h:129 */

/* SE_SPI_TRANS_CFG: CS_TOGGLE is BIT(1) (spi-geni-qcom.c:41-42), cleared at :691-693 */
#define IR_SPI_CS_TOGGLE        (1u << 1)

/* SE_SPI_WORD_LEN: word_len = bits_per_word - MIN_WORD_LEN (spi-geni-qcom.c:44-46, 304-305) */
#define IR_SPI_WORD_LEN_MSK     0x3ffu
#define IR_SPI_MIN_WORD_LEN     4u

/* M_CMD opcodes and parameters (spi-geni-qcom.c:62-74; geni-se.h:160-161) */
#define IR_SPI_TX_ONLY          1u              /* SPI_TX_ONLY, spi-geni-qcom.c:63 */
#define IR_M_OPCODE_SHFT        27
#define IR_M_CMD_CANCEL         (1u << 2)       /* M_GENI_CMD_CANCEL, geni-se.h:165 */
#define IR_M_CMD_ABORT          (1u << 1)       /* M_GENI_CMD_ABORT, geni-se.h:166 */

/* M_IRQ status/enable bits (geni-se.h:179-210) */
#define IR_M_IRQ_CMD_DONE       (1u << 0)       /* M_CMD_DONE_EN */
#define IR_M_IRQ_CMD_CANCEL     (1u << 4)       /* M_CMD_CANCEL_EN */
#define IR_M_IRQ_CMD_ABORT      (1u << 5)       /* M_CMD_ABORT_EN */
#define IR_M_IRQ_TX_WM          (1u << 30)      /* M_TX_FIFO_WATERMARK_EN */

/*
 * The source mask mainline ends up with in FIFO mode: geni_se_init() sets
 * M_COMMON_GENI_M_IRQ_EN (geni-se.h:206-210, qcom-geni-se.c:368-370) and
 * geni_se_select_fifo_mode() adds CMD_DONE, TX/RX watermark and RX_LAST
 * (qcom-geni-se.c:388-392). 0x7fc0007f = bits 0-6, 22-30.
 */
#define IR_M_IRQ_EN_BITS        0x7fc0007fu

/* Status bits that mean the command did not run as asked. */
#define IR_M_IRQ_ERRORS         ((1u << 1) | (1u << 2) | (1u << 3) |             /* overrun, illegal, failure */ \
                                 (1u << 24) | (1u << 25) |                       /* RX FIFO read/write error */ \
                                 (1u << 28) | (1u << 29))                        /* TX FIFO read/write error */
#define IR_M_IRQ_FATAL          (IR_M_IRQ_ERRORS | IR_M_IRQ_CMD_CANCEL | IR_M_IRQ_CMD_ABORT)

/*
 * Packing for bits_per_word = 32, one word per FIFO entry, MSB first, exactly what
 * geni_se_config_packing(se, 32, 1, true, true, true) produces (qcom-geni-se.c:532-584):
 * the four 10-bit vectors are 0x3fe, 0x2fe, 0x1fe and 0xff (start 31, 23, 15, 7; direction 1;
 * length 7; the last carries STOP), cfg0 = vec0 | vec1 << 10, cfg1 = vec2 | vec3 << 10.
 */
#define IR_TX_PACK_CFG0         0x000bfbfeu
#define IR_TX_PACK_CFG1         0x0003fdfeu
#define IR_BYTE_GRAN_32         2u              /* bpw / 16 = 1 x 32 (qcom-geni-se.c:575-583) */

/* ---- hardware constants and limits ------------------------------------------------------------ */

#define IR_SE_CLK_HZ            19200000u       /* the loader's TCXO vote, vayu_qup_spi.c:104-106 */
#define IR_SPI_HZ               1920000u        /* vendor-ir-spi.c:43 */
#define IR_CLK_DIV              (IR_SE_CLK_HZ / IR_SPI_HZ)  /* 10: 19.2 MHz -> 1.92 MHz, spi-geni-qcom.c:333-336 */
#define IR_MAX_BYTES            150000u         /* vendor-ir-spi.c:45 */
#define IR_TX_WM                1u              /* "an entire FIFO worth per IRQ", spi-geni-qcom.c:646-647 */
#define IR_TX_FIFO_WIDTH_SHFT   24              /* geni-se.h:273-274 */
#define IR_TX_FIFO_WIDTH_MSK    0x3fu
#define IR_TX_FIFO_DEPTH_SHFT   16              /* geni-se.h:279-281; as vayu_bt.c:321-323 */
#define IR_TX_FIFO_DEPTH_MSK    0x3fu
#define IR_TRANS_LEN_MSK        0xffffffu       /* spi-geni-qcom.c:50 */
#define IR_STOP_TIMEOUT_US      20000u          /* cancel and abort each: mainline waits HZ (spi-geni-qcom.c:173,184) */

/* ---- logging: the loader's log too, as vayu_haptic.c:106-133 ---------------------------------- */

extern void loader_log_putc(char c);
extern void delay(int usec);
extern void IOSleep(unsigned int milliseconds);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static int ir_log(bool also_kernel, const char *format, va_list list) __printflike(2, 0);
static int tee_printf(const char *format, ...) __printflike(1, 2);
static int loader_printf(const char *format, ...) __printflike(1, 2);

static int
ir_log(bool also_kernel, const char *format, va_list list)
{
	char line[320];
	int length;

	length = vsnprintf(line, sizeof(line), format, list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (also_kernel && ml_get_interrupts_enabled()) {
		(printf)("%s", line);
	}
	return length;
}

static int
tee_printf(const char *format, ...)
{
	va_list list;
	int length;

	va_start(list, format);
	length = ir_log(true, format, list);
	va_end(list);
	return length;
}

/* The way out (vayu_ir_quiesce): interrupts are off, so loader_log_putc() only. */
static int
loader_printf(const char *format, ...)
{
	va_list list;
	int length;

	va_start(list, format);
	length = ir_log(false, format, list);
	va_end(list);
	return length;
}

#define printf(...) tee_printf(__VA_ARGS__)

/* ---- state ------------------------------------------------------------------------------------ */

static vm_offset_t ir_se;
static uint32_t ir_tx_depth, ir_tx_width;
static LCK_GRP_DECLARE(ir_lock_group, "vayu-ir");
static LCK_MTX_DECLARE(ir_lock_storage, &ir_lock_group);
static lck_mtx_t *const ir_lock = &ir_lock_storage;
static volatile bool ir_xfer_active;
static volatile uint32_t ir_xfer_bytes;
static bool ir_ready, ir_configured;

/* ---- small helpers ---------------------------------------------------------------------------- */

static inline uint32_t
ir_rd(uint32_t offset)
{
	return *(volatile uint32_t *)(ir_se + offset);
}

static inline void
ir_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(ir_se + offset) = value;
}

static uint64_t
ir_now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/* 2 x the transfer's duration at 1.92 MHz, plus 50 ms, as the task's budget. */
static uint64_t
ir_timeout_us(uint32_t bytes)
{
	uint64_t duration = (uint64_t)bytes * 8u * 1000000u / IR_SPI_HZ;

	return duration * 2u + 50000u;
}

/*
 * Cancel, then abort, as mainline's handle_se_timeout() does (spi-geni-qcom.c:152-225):
 * stop the watermark first, ask for cancel, wait for its completion bit (or for the M
 * sequencer to go idle), and only then abort. Returns 1 for cancel, 2 for abort, 0 when
 * neither was acknowledged. No printf: vayu_ir_quiesce() calls it with interrupts off.
 */
static int
ir_stop_cmd(uint32_t timeout_us)
{
	uint64_t deadline;
	uint32_t status;

	ir_wr(SE_GENI_TX_WATERMARK_REG, 0);
	ir_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);

	ir_wr(SE_GENI_M_CMD_CTRL_REG, IR_M_CMD_CANCEL);
	deadline = ir_now_us() + timeout_us;
	for (;;) {
		uint32_t irq = ir_rd(SE_GENI_M_IRQ_STATUS);

		status = ir_rd(SE_GENI_STATUS);
		if ((irq != 0xffffffffu && (irq & IR_M_IRQ_CMD_CANCEL) != 0) ||
		    (status != 0xffffffffu && (status & IR_M_ACTIVE) == 0)) {
			ir_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
			return 1;
		}
		if (ir_now_us() >= deadline) {
			break;
		}
	}

	ir_wr(SE_GENI_M_CMD_CTRL_REG, IR_M_CMD_ABORT);
	deadline = ir_now_us() + timeout_us;
	for (;;) {
		uint32_t irq = ir_rd(SE_GENI_M_IRQ_STATUS);

		status = ir_rd(SE_GENI_STATUS);
		if ((irq != 0xffffffffu && (irq & IR_M_IRQ_CMD_ABORT) != 0) ||
		    (status != 0xffffffffu && (status & IR_M_ACTIVE) == 0)) {
			ir_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
			return 2;
		}
		if (ir_now_us() >= deadline) {
			break;
		}
	}

	ir_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	return 0;
}

/*
 * One FIFO word from four user bytes: buf[0] -> bits 7:0, buf[1] -> 15:8, buf[2] -> 23:16,
 * buf[3] -> 31:24, which is mainline's fifo_byte[j] copy (spi-geni-qcom.c:734-743). With the
 * MSB-first packing above, buf[3] is therefore the first byte on the wire, bit 7 first.
 */
static inline uint32_t
ir_fifo_word(const uint8_t *buf, uint32_t offset)
{
	uint32_t word = 0;
	uint8_t *byte = (uint8_t *)&word;

	byte[0] = buf[offset + 0];
	byte[1] = buf[offset + 1];
	byte[2] = buf[offset + 2];
	byte[3] = buf[offset + 3];
	return word;
}

/*
 * Fill what room the FIFO has: its depth less the words already in it, read from SE_GENI_TX_FIFO_STATUS
 * before every batch (geni-se.h:110, 238-239). Run 145156 (): feeding a fixed (depth - watermark)
 * on every watermark status bit pushed 45 words into the 16-word FIFO in 278 us -> M_TX_FIFO_WR_ERR (bit 29).
 */
static bool
ir_feed_tx(uint32_t *sent, uint32_t words, const uint8_t *buf)
{
	uint32_t level = ir_rd(SE_GENI_TX_FIFO_STATUS) & IR_TX_FIFO_WC_MSK;
	uint32_t room = level < ir_tx_depth ? ir_tx_depth - level : 0;

	while (room != 0 && *sent < words) {
		ir_wr(SE_GENI_TX_FIFOn, ir_fifo_word(buf, *sent * 4u));
		(*sent)++;
		room--;
	}
	return *sent < words;
}

/* ---- configuring the SE, per transfer --------------------------------------------------------- */

/*
 * Every write re-applies this idempotent setup, so a failed transfer never leaves the next one
 * with a half-configured engine. It is mainline's spi_geni_init() FIFO branch (spi-geni-qcom.c:
 * 608-699) plus geni_se_init() (qcom-geni-se.c:357-376) with the SPI parameters of mode 0.
 */
static bool
ir_configure(void)
{
	uint32_t status, old_clk_sel, old_mclk, trans_cfg;

	status = ir_rd(SE_GENI_STATUS);
	if (status != 0xffffffffu && (status & IR_M_ACTIVE) != 0) {
		int how;

		printf("vayu-ir: a command is still active before this write (SE_GENI_STATUS 0x%08x); stopping it\n", status);
		how = ir_stop_cmd(IR_STOP_TIMEOUT_US);
		if (how == 0) {
			printf("vayu-ir: the stale command did not stop; not starting another\n");
			return false;
		}
		printf("vayu-ir: the stale command stopped (%s)\n", how == 1 ? "cancel" : "abort");
	}

	old_clk_sel = ir_rd(SE_GENI_CLK_SEL);
	old_mclk = ir_rd(GENI_SER_M_CLK_CFG);

	/* geni_se_irq_clear(): drop stale status; no GSI events. */
	ir_wr(SE_GSI_EVENT_EN, 0);
	ir_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	ir_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);

	/* geni_se_io_init() (qcom-geni-se.c:321-336). */
	ir_wr(GENI_CGC_CTRL, ir_rd(GENI_CGC_CTRL) | IR_GENI_CGC_DEFAULT);
	ir_wr(SE_DMA_GENERAL_CFG, ir_rd(SE_DMA_GENERAL_CFG) | IR_DMA_CGC_DEFAULT);
	ir_wr(GENI_OUTPUT_CTRL, IR_GENI_OUTPUT_DEFAULT);
	ir_wr(GENI_FORCE_DEFAULT_REG, IR_FORCE_DEFAULT);

	/* FIFO mode, DMA off (qcom-geni-se.c:395-398, spi-geni-qcom.c:676-687). */
	ir_wr(SE_GENI_DMA_MODE_EN, ir_rd(SE_GENI_DMA_MODE_EN) & ~IR_GENI_DMA_MODE_EN);

	/* Mode 0, chip select 0, automatic CS (spi-geni-qcom.c:680-684). */
	ir_wr(SE_SPI_LOOPBACK, 0);
	ir_wr(SE_SPI_DEMUX_SEL, 0);
	ir_wr(SE_SPI_CPHA, 0);
	ir_wr(SE_SPI_CPOL, 0);
	ir_wr(SE_SPI_DEMUX_OUTPUT_INV, 0);
	trans_cfg = ir_rd(SE_SPI_TRANS_CFG);
	ir_wr(SE_SPI_TRANS_CFG, trans_cfg & ~IR_SPI_CS_TOGGLE);        /* :689-694 */

	/* 32-bit words, MSB first, one SPI word per FIFO entry (spi-geni-qcom.c:286-305). */
	ir_wr(SE_GENI_TX_PACKING_CFG0, IR_TX_PACK_CFG0);
	ir_wr(SE_GENI_TX_PACKING_CFG1, IR_TX_PACK_CFG1);
	ir_wr(SE_GENI_RX_PACKING_CFG0, IR_TX_PACK_CFG0);              /* tx_cfg=true, rx_cfg=true, :302-303 */
	ir_wr(SE_GENI_RX_PACKING_CFG1, IR_TX_PACK_CFG1);
	ir_wr(SE_GENI_BYTE_GRAN, IR_BYTE_GRAN_32);
	ir_wr(SE_SPI_WORD_LEN, (32u - IR_SPI_MIN_WORD_LEN) & IR_SPI_WORD_LEN_MSK);

	/*
	 * 19.2 MHz / 10 = 1.92 MHz, fixed source (CLK_SEL 0 = no DFS level), which is what the
	 * loader left: vayu_ir_clk=1 says the SE clock is on at the TCXO. Mainline writes CLK_SEL
	 * then M_CLK_CFG (:333-336); there is no S_CLK_CFG write for an SPI master.
	 */
	ir_wr(SE_GENI_CLK_SEL, 0);
	ir_wr(GENI_SER_M_CLK_CFG, (IR_CLK_DIV << IR_SER_CLK_DIV_SHFT) | IR_SER_CLK_EN);

	/*
	 * Nothing in XNU routes this QUP's interrupt, so no ISR runs; the status is polled. The
	 * group enables are still set the way geni_se_io_set_mode() (qcom-geni-se.c:305-312) and
	 * the phone's working BT driver (vayu_bt.c:1645) set them, and M_IRQ_EN is mainline's FIFO
	 * source mask (geni-se.h:179-210, qcom-geni-se.c:368-370, 388-392). vayu_bt.c:1090-1099
	 * proves the same combination polled with no GIC route on this phone.
	 */
	ir_wr(SE_IRQ_EN, ir_rd(SE_IRQ_EN) | IR_SE_IRQ_BITS);
	ir_wr(SE_GENI_M_IRQ_EN, IR_M_IRQ_EN_BITS);

	if (!ir_configured) {
		ir_configured = true;
		printf("vayu-ir: configured: SPI mode 0 (CPOL 0, CPHA 0), 32-bit words MSB first, FIFO (DMA off), "
		    "1.92 MHz = 19.2 MHz / %u, CLK_SEL 0 (was %u), M_CLK_CFG 0x%08x (was 0x%08x), TX FIFO %u x %u bits, "
		    "watermark %u, CS0\n", IR_CLK_DIV, old_clk_sel & 7u,
		    (IR_CLK_DIV << IR_SER_CLK_DIV_SHFT) | IR_SER_CLK_EN, old_mclk, ir_tx_depth, ir_tx_width, IR_TX_WM);
	}
	return true;
}

/* ---- one transfer, polled --------------------------------------------------------------------- */

/*
 * The command is set up and fed the way mainline's setup_se_xfer() and geni_spi_handle_tx() do
 * it (spi-geni-qcom.c:797-900, 715-751): TX_TRANS_LEN is the number of 32-bit words, M_CMD is
 * SPI_TX_ONLY with no parameters, the first FIFO's worth goes in at once, and the watermark (1)
 * asks for the next when the FIFO has room. M_CMD_DONE ends it. The caller holds ir_lock.
 */
static int
ir_transfer_locked(const uint8_t *buf, uint32_t bytes, uint64_t *elapsed_us, uint32_t *final_irq)
{
	uint32_t words = bytes / 4u, sent = 0, irq = 0, polls = 0;
	uint64_t start, deadline;
	bool ok = false;

	*final_irq = 0;
	start = ir_now_us();
	if (!ir_configure()) {
		*final_irq = ir_rd(SE_GENI_M_IRQ_STATUS);
		*elapsed_us = ir_now_us() - start;
		return EIO;
	}

	ir_wr(SE_GENI_TX_WATERMARK_REG, 0);
	ir_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	ir_wr(SE_SPI_TX_TRANS_LEN, words & IR_TRANS_LEN_MSK);

	start = ir_now_us();
	ir_xfer_bytes = bytes;
	ir_xfer_active = true;
	ir_wr(SE_GENI_M_CMD0, IR_SPI_TX_ONLY << IR_M_OPCODE_SHFT);

	if (ir_feed_tx(&sent, words, buf)) {
		ir_wr(SE_GENI_TX_WATERMARK_REG, IR_TX_WM);
	}

	deadline = start + ir_timeout_us(bytes);
	for (;;) {
		irq = ir_rd(SE_GENI_M_IRQ_STATUS);

		if ((irq & IR_M_IRQ_FATAL) != 0) {
			printf("vayu-ir: transfer error: M_IRQ_STATUS 0x%08x, SE_GENI_STATUS 0x%08x, %u of %u words queued; "
			    "cancelling/aborting\n", irq, ir_rd(SE_GENI_STATUS), sent, words);
			ir_stop_cmd(IR_STOP_TIMEOUT_US);
			break;
		}
		if ((irq & IR_M_IRQ_CMD_DONE) != 0) {
			if (sent != words) {
				printf("vayu-ir: premature M_CMD_DONE with %u of %u words queued; cancelling/aborting\n",
				    sent, words);
				ir_stop_cmd(IR_STOP_TIMEOUT_US);
				break;
			}
			ok = true;
			break;
		}
		/* polled: top the FIFO up whenever it has room (the watermark bit is only cleared, not trusted for the count) */
		if ((irq & IR_M_IRQ_TX_WM) != 0) {
			ir_wr(SE_GENI_M_IRQ_CLEAR, IR_M_IRQ_TX_WM);
		}
		if (sent < words && !ir_feed_tx(&sent, words, buf)) {
			ir_wr(SE_GENI_TX_WATERMARK_REG, 0);
		}
		if (ir_now_us() >= deadline) {
			printf("vayu-ir: transfer timed out after %llu us: M_IRQ_STATUS 0x%08x, SE_GENI_STATUS 0x%08x, "
			    "%u of %u words queued; cancelling/aborting\n",
			    (unsigned long long)(ir_now_us() - start), irq, ir_rd(SE_GENI_STATUS), sent, words);
			ir_stop_cmd(IR_STOP_TIMEOUT_US);
			break;
		}
		if ((++polls & 0x3ffu) == 0) {
			delay(1);               /* one microsecond in a thousand polls, as vayu_qup_spi.c:2651-2653 */
		}
	}

	*elapsed_us = ir_now_us() - start;
	*final_irq = irq;
	if (ok) {
		ir_wr(SE_GENI_TX_WATERMARK_REG, 0);
		ir_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	}
	ir_xfer_active = false;
	ir_xfer_bytes = 0;
	return ok ? 0 : EIO;
}

/* ---- /dev/vayu-ir ----------------------------------------------------------------------------- */

static int
ir_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
ir_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	user_ssize_t resid = uio_resid(uio);
	uint8_t *buf;
	uint64_t elapsed;
	uint32_t final_irq;
	int error;

	if (resid < 0 || resid > (user_ssize_t)IR_MAX_BYTES) {
		return EINVAL;
	}
	if ((resid & 3) != 0) {
		return EINVAL;
	}
	if (resid == 0) {
		return 0;
	}
	if (!ir_ready || ir_se == 0 || ir_lock == NULL) {
		return ENXIO;
	}

	/*
	 * The lock covers the copy as well as the transfer, as the vendor driver's does
	 * (vendor-ir-spi.c:74-94): one buffer and one command exist at a time, however many
	 * writers there are.
	 */
	lck_mtx_lock(ir_lock);
	buf = kalloc_data((vm_size_t)resid, Z_WAITOK);
	if (buf == NULL) {
		lck_mtx_unlock(ir_lock);
		return ENOMEM;
	}
	error = uiomove((caddr_t)buf, (int)resid, uio);
	if (error != 0) {
		kfree_data(buf, (vm_size_t)resid);
		lck_mtx_unlock(ir_lock);
		return error;
	}

	error = ir_transfer_locked(buf, (uint32_t)resid, &elapsed, &final_irq);
	printf("vayu-ir: %u bytes, %u words, %llu us, M_IRQ_STATUS 0x%08x, %s\n",
	    (unsigned)resid, (unsigned)(resid / 4), (unsigned long long)elapsed, final_irq,
	    error == 0 ? "sent" : "failed (EIO)");

	kfree_data(buf, (vm_size_t)resid);
	lck_mtx_unlock(ir_lock);
	return error;
}

static const struct cdevsw ir_cdevsw = {
	.d_open = ir_open, .d_close = ir_open, .d_read = eno_rdwrt, .d_write = ir_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool
ir_make_node(void)
{
	int major = cdevsw_add(-1, &ir_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "vayu-ir") == NULL) {
		printf("vayu-ir: /dev/vayu-ir cannot be made\n");
		return false;
	}
	printf("vayu-ir: /dev/vayu-ir (0666) is made; one write() is one TX-only SPI transfer of raw bits\n");
	return true;
}

/* ---- step 1: the read-only look --------------------------------------------------------------- */

static bool
ir_probe(void)
{
	uint32_t fw, status, mclk, clksel, hw0, if_disable, proto, version;

	ir_se = ml_io_map(IR_SE_PHYS, IR_SE_SIZE);
	if (ir_se == 0) {
		printf("vayu-ir: cannot map the SE at 0x%08lx\n", (unsigned long)IR_SE_PHYS);
		return false;
	}

	printf("vayu-ir: QUP wrapper 0 SE1 at 0x%08lx (spi@884000), a read-only look; nothing written\n",
	    (unsigned long)IR_SE_PHYS);

	fw = ir_rd(GENI_FW_REVISION_RO);
	status = ir_rd(SE_GENI_STATUS);
	mclk = ir_rd(GENI_SER_M_CLK_CFG);
	clksel = ir_rd(SE_GENI_CLK_SEL);
	proto = (fw & IR_FW_REV_PROTOCOL_MSK) >> IR_FW_REV_PROTOCOL_SHFT;
	version = fw & IR_FW_REV_VERSION_MSK;
	printf("vayu-ir: GENI_FW_REVISION_RO 0x%08x (protocol %u, version %u), SE_GENI_STATUS 0x%08x, "
	    "GENI_SER_M_CLK_CFG 0x%08x, SE_GENI_CLK_SEL 0x%08x\n", fw, proto, version, status, mclk, clksel);

	if (fw == 0xffffffffu || fw == 0) {
		printf("vayu-ir: SE1 not clocked (loader vayu_ir_clk=1?): GENI_FW_REVISION_RO reads 0x%08x; "
		    "nothing written, no node\n", fw);
		return false;
	}
	if (proto != IR_SE_PROTO_SPI) {
		printf("vayu-ir: the SE's firmware is protocol %u, not 1 (SPI): nothing written, no node. XNU cannot "
		    "load SE firmware; boot Android once (it sets this SE to SPI) and reboot straight into the loader.\n", proto);
		return false;
	}
	if (status != 0xffffffffu && (status & IR_M_ACTIVE) != 0) {
		printf("vayu-ir: a command was left active (SE_GENI_STATUS 0x%08x); the first write will cancel it\n", status);
	}

	hw0 = ir_rd(SE_HW_PARAM_0);
	ir_tx_width = (hw0 >> IR_TX_FIFO_WIDTH_SHFT) & IR_TX_FIFO_WIDTH_MSK;
	ir_tx_depth = (hw0 >> IR_TX_FIFO_DEPTH_SHFT) & IR_TX_FIFO_DEPTH_MSK;
	if_disable = ir_rd(GENI_IF_DISABLE_RO) & 1u;
	printf("vayu-ir: SE_HW_PARAM_0 0x%08x: TX FIFO %u words x %u bits; FIFO_IF_DISABLE %u%s\n",
	    hw0, ir_tx_depth, ir_tx_width, if_disable,
	    if_disable ? " (GPI preferred; mainline falls back to FIFO, and so does this)" : "");

	if (ir_tx_width != 32u || ir_tx_depth < 2u) {
		printf("vayu-ir: this driver writes 32-bit words and needs a TX FIFO of at least 2 words; "
		    "the SE says %u x %u: nothing written, no node\n", ir_tx_depth, ir_tx_width);
		return false;
	}

	/*
	 * GENI_FW_REVISION_RO's protocol is what XBL loaded; if it is SPI and the SE answers, the
	 * 19.2 MHz TCXO behind the loader's vayu_ir_clk=1 is good enough for a 1.92 MHz shift clock.
	 * Nothing outside the SE is read or written: the pins are the loader's (TLMM exception 7),
	 * and so is GCC (exception 7a).
	 */
	printf("vayu-ir: the loader's pins 114-117 (qup1) and SE clock are taken as given; XNU writes no TLMM and no GCC\n");
	return true;
}

/*
 * vayu_ir_selftest=N (1..60): N bursts of a 38 kHz square wave (25 ones, 25 zeros: 38.4 kHz) for 500 ms, one a
 * second, through the same path as a write() -- so the LED can be seen in a phone camera without any user-space
 * tool. Built in kernel memory, words in wire order (bit 31 first) stored little-endian, as vayu-irtest.c does.
 */
static void
ir_selftest(void)
{
	uint32_t n = 0, bytes = IR_SPI_HZ / 8u / 2u, nbits = bytes * 8u;   /* 500 ms = 120000 bytes */
	uint8_t *buf;

	if (!PE_parse_boot_argn("vayu_ir_selftest", &n, sizeof(n)) || n == 0) {
		return;
	}
	n = n > 60 ? 60 : n;
	buf = kalloc_data(bytes, Z_WAITOK | Z_ZERO);
	if (buf == NULL) {
		printf("vayu-ir: selftest: no memory\n");
		return;
	}
	for (uint32_t w = 0; w < bytes / 4u; w++) {
		uint32_t word = 0;

		for (uint32_t k = 0; k < 32u; k++) {
			uint32_t bit = w * 32u + k;

			word = (word << 1) | (bit < nbits && bit % 50u < 25u ? 1u : 0u);
		}
		buf[4 * w + 0] = (uint8_t)word;
		buf[4 * w + 1] = (uint8_t)(word >> 8);
		buf[4 * w + 2] = (uint8_t)(word >> 16);
		buf[4 * w + 3] = (uint8_t)(word >> 24);
	}
	printf("vayu-ir: selftest: %u bursts of 38.4 kHz, 500 ms each, one a second (look at the LED through a camera)\n", n);
	for (uint32_t i = 0; i < n; i++) {
		uint64_t elapsed;
		uint32_t final_irq;
		int error;

		lck_mtx_lock(ir_lock);
		error = ir_transfer_locked(buf, bytes, &elapsed, &final_irq);
		lck_mtx_unlock(ir_lock);
		/* (run 145156: XBL left SE1's RCG in DFS, so the SE rate is DFS level 0's, not known to be 19.2 MHz: measured here) */
		printf("vayu-ir: selftest burst %u: %u bytes, %llu us (%llu kbit/s measured; 1920 wanted), M_IRQ_STATUS 0x%08x, %s\n", i + 1,
		    bytes, (unsigned long long)elapsed, elapsed ? (unsigned long long)bytes * 8u * 1000u / elapsed : 0ull, final_irq,
		    error == 0 ? "sent" : "FAILED");
		if (error != 0) {
			break;
		}
		IOSleep(500);
	}
	kfree_data(buf, bytes);
}

/* ---- the start -------------------------------------------------------------------------------- */

static void
vayu_ir_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (!ir_probe()) {
		goto out;
	}

	/*
	 * Ready before the node exists: a user who can open /dev/vayu-ir then always finds a
	 * mapped SE and a lock. If making the node fails, the rest is unreachable anyway.
	 */
	ir_ready = true;
	if (!ir_make_node()) {
		ir_ready = false;
		goto out;
	}
	printf("vayu-ir: ready; write up to %u bytes (a multiple of 4), the bits are the waveform\n", IR_MAX_BYTES);
	ir_selftest();

out:
	thread_terminate(current_thread());
}

void
vayu_ir_init(void)
{
	thread_t thread;
	uint32_t on = 0;

	if (!PE_parse_boot_argn("vayu_ir", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(vayu_ir_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("vayu-ir: the probe's thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}

/*
 * At the run's end and on a panic's way out: called from osfmk/arm64/loader_return.c. If a
 * transfer is in flight, stop it with mainline's cancel-then-abort sequence, bounded (20 ms
 * each), and say so through loader_log_putc() only: no printf here, interrupts are off. It
 * takes no lock and does not touch the transfer's buffer.
 */
void
vayu_ir_quiesce(void)
{
	uint32_t bytes, irq;
	int how;

	if (ir_se == 0 || !ir_xfer_active) {
		return;
	}
	bytes = ir_xfer_bytes;
	irq = ir_rd(SE_GENI_M_IRQ_STATUS);
	how = ir_stop_cmd(IR_STOP_TIMEOUT_US);
	ir_xfer_active = false;
	loader_printf("vayu-ir: quiesce: a %u-byte transfer stopped by %s; M_IRQ_STATUS was 0x%08x, SE_GENI_STATUS now 0x%08x\n",
	    bytes, how == 1 ? "cancel" : how == 2 ? "abort" : "neither (the SE did not answer)", irq, ir_rd(SE_GENI_STATUS));
}
