/*
 * Bluetooth's UART on the POCO X3 Pro (vayu, SM8150): Phase 1, steps 1..9 -- a read-only look,
 * the GENI UART in FIFO mode at 2400 8N1, the pins, the rails, the QCA power pulses, the SoC
 * version, HCI, the QCA firmware download at 115200 with HCI again, and then the H4 tty and the
 * kernel control that iOS's bluetoothd uses (/dev/cu.bluetooth or "com.vayu.bt.uart"), with a
 * trace of every HCI packet.
 *
 * Step 1 writes no configuration register: every access is a 32-bit load, except the PMIC
 * arbiter's read-command word in its observer channel -- the same store vayu_camera.c and
 * vayu_haptic.c make and the only way to read a PMIC register at all. It prints the QUP wrapper
 * 2 version, the wrapper's two AHB halt registers and the SE3 branch, SE3's RCG and its eight
 * DFS levels, the SE's GENI/UART registers and FIFO depths, GPIO 43-46, and a readout of the
 * four PMIC regulators the Bluetooth part uses.
 *
 * Steps 2..8 are the first parts of this file that write anything:
 *   (a) SE3 registers inside 0x00c8c000..0x00c8cfff only, never offset 0x1000 or above (an SE's
 *       0x2000+ window is TrustZone-only and a read there hangs);
 *   (b) RPMh VRM votes through vayu_rpmh_send() for ldoa7, ldoc2 and ldoc11 only.
 * XNU writes no TLMM and no GCC register here: those are read only, as everywhere else in this
 * tree (the loader does the pin muxing and the SE3/AHB clock votes, vayu_bt_loader=2).
 *
 * Step 9 is the hand-over to iOS: one struct tty (ttymalloc) behind two devfs nodes on one dev_t,
 * /dev/cu.bluetooth and /dev/uart.bluetooth, the kernel control "com.vayu.bt.uart" (a
 * PF_SYSTEM/SOCK_STREAM kernel control for bluetoothd's H4:POSIX driver), and one kernel thread
 * that from then on owns the SE -- it drains the tty's output queue and the kctl TX ring into
 * bt_tx_quiet() and feeds bt_rx() bytes to the line discipline through l_rint and to the kctl
 * client with ctl_enqueuedata(). Steps 1..8 used the SE from this probe's thread; after step 9
 * no other path in this file calls bt_tx() or bt_rx(). The tty stays raw at 115200 whatever
 * speed userland asks for: the UART's dividers are never touched by the tty; the kctl's
 * termios is answered and stored, not applied. An opt-in shim (vayu_bt_vsc=1|2) sits between
 * t_outq/the kctl ring and bt_tx_quiet(): it reassembles complete H4 packets, answers a selected
 * vendor opcode (OGF 0x3f) with a synthetic event, and passes everything else, including every
 * standard command, to the controller unchanged. Mode 1 uses bt_vsc_rules; mode 2 also answers an
 * unlisted vendor opcode with a Command Complete status 0.
 *
 * Run 031412, the first hardware run, found (and this file now assumes):
 *   - SE3's RCG is not in DFS: the DFS word (cmd + 0x14) reads 0x8020, bit 0 clear, and all
 *     eight DFSR/M/N words read 0. The bt2 loader programs the RCG itself to 7 372 800 Hz
 *     (cfg source 6, hid 1, mode 2; M 384, N ~(15625 - 384)). Step 1 reads the one rate from
 *     the RCG's CFG (cmd + 4), M (cmd + 8) and N (cmd + 0xc), stores it as level 0 and marks
 *     levels 1..7 invalid; the eight DFSR lines are printed only when DFS is on.
 *   - The PMIC LDOs are not readable through the apps arbiter: ppids 0x1d, 0x46, 0x441 and
 *     0x44a answer "no channel of the arbiter". Step 4 treats such a rail as unknown, votes it
 *     (with vayu_bt_rails=1) and no longer stops on a readout failure; S4A is only read.
 *   - Step 2 worked: the GENI UART came up at 2400 baud with divider 250 from 19.2 MHz; the
 *     FIFO depths are TX 16 words and RX 32 words.
 *
 * The facts, with their sources:
 *
 *   the node    stock vayu.dts:23389-23405 "qcom,qup_uart@0xc8c000", compatible
 *               "qcom,msm-geni-serial-hs", alias hsuart0 (:27591). Its wrapper is
 *               "qcom,qupv3-geni-se" at 0xcc0000 (:23650-23662): QUP wrapper 2, SE3
 *               (the bring-up notes). SE registers: 0xc8c000, 0x4000 bytes.
 *               The wrapper's own version is QUPV3_HW_VER at 0x00cc0004 (vendor
 *               qcom-geni-se.h; vayu_qup_spi.c reads it through s_ahb).
 *   the GENI    the geni-se/msm_geni_serial offsets: 0x020 GENI_FORCE_DEFAULT_REG, 0x024
 *               GENI_OUTPUT_CTRL, 0x028 GENI_CGC_CTRL, 0x040 SE_GENI_STATUS (bit0 M command
 *               active, bit12 S command active), 0x048/0x04c SER_M/S_CLK_CFG, 0x064
 *               IF_DISABLE_RO, 0x068 FW_REVISION_RO (bits 15:8 = protocol: 1 SPI, 2 UART,
 *               3 I2C; geni-se.h:36,180-181), 0x07c CLK_SEL, 0x22c UART_LOOPBACK_CFG, 0x258
 *               DMA_MODE_EN, 0x25c TX_TRANS_CFG, 0x260/0x264 TX packing, 0x268 TX word len,
 *               0x26c TX stop bit, 0x270 TX trans len, 0x280 RX trans cfg, 0x284/0x288 RX
 *               packing, 0x28c RX word len, 0x294 RX stale count, 0x2a4/0x2a8 parity, 0x2ac
 *               MANUAL_RFR, 0x600/0x604 M command, 0x610..0x618 M IRQ, 0x630/0x634 S command,
 *               0x640..0x648 S IRQ, 0x700/0x780 TX/RX FIFO, 0x800/0x804 FIFO status, 0x80c/
 *               0x810/0x814 watermarks, 0xc44/0xd44 DMA IRQ clear, 0xe18 SE_GSI_EVENT_EN,
 *               0xe1c SE_IRQ_EN, 0xe24/0xe28 HW_PARAM_0/1, 0xe30 SE_DMA_GENERAL_CFG (vendor
 *               drivers/tty/serial/msm_geni_serial.c and drivers/platform/msm/qcom-geni-se.h,
 *               Xiaomi_Kernel_OpenSource vayu-r-oss; the same offsets as bsd/dev/vayu_qup_spi.c).
 *               UART_START_TX is M opcode 1 ([31:27]); S UART_START_READ is opcode 1 with
 *               UART_PARAM_RFR_OPEN 0x80, i.e. 0x08000080. Packing for 8-bit chars, four per
 *               word LSB first: 0x0004380e / 0x000c3e0e. Oversampling is 32, so a baud rate
 *               asks the RCG for baud * 32; the stale count is 10 * 16 = 0xa0.
 *   the clocks  GCC at 0x100000 (as vayu_qup_spi.c GCC_PHYS). The vote word at 0x52014 has
 *               bit 1 for wrap 2 s_ahb and bit 2 for wrap 2 m_ahb (vayu_qup_spi.c:79-81) and
 *               bit 7 for wrap 2 SE3 (gcc-sm8150.c:2453-2458; VAYU_BT_PHASE0.md §1). A
 *               branch's halt register shows bit 31 while its clock is off
 *               (vayu_qup_spi.c:92): wrapper s_ahb 0x1e008 and m_ahb 0x1e004
 *               (vayu_qup_spi.c:84-85), SE3's own branch 0x1e4d4 (gcc-sm8150.c:2454).
 *               SE3's RCG is at cmd 0x1e4d8, cfg 0x1e4dc (cmd + 4, as vayu_qup_spi.c:88;
 *               gcc-sm8150.c:731-732), its DFS word at cmd + 0x14 (as vayu_qup_spi.c:89) and
 *               its DFS table at cmd + 0x1c (DFSR), cmd + 0x5c (M) and cmd + 0x9c (N), one
 *               entry every 4 bytes for eight levels (vendor gcc-sm8150.c:4188 and
 *               clk-rcg2.c). DFSR: source [10:8], hid [4:0], mode [13:12]. gcc_parent_map_0:
 *               source 0 = XO 19 200 000, 1 = GPLL0 600 MHz, 6 = GPLL0_EVEN 300 MHz; any other
 *               source is unknown here and that level is skipped. Rate = parent; if hid != 0
 *               the rate is rate * 2 / (hid + 1); if mode != 0, m = M & 0xffff,
 *               n = ((~N) & 0xffff) + m and rate = rate * m / n. The expected rate for
 *               7 372 800 Hz is GPLL0_EVEN with m 384, n 15625. With DFS off the same fields
 *               come from CFG (cmd + 4), M (cmd + 8) and N (cmd + 0xc), and that one rate is
 *               level 0.
 *   the pins    mainline pinctrl-sm8150.c:1356-1359: GPIO 43-46 are in the EAST tile and
 *               function 1 is "qup13". The tile is at 0x03500000 (sm8150.dtsi:2381-2385),
 *               with one 0x1000-byte block per pin (ctl_reg = 0x1000 * id); cfg is at +0x0
 *               and io at +0x4, as the other vayu drivers read the TLMM. GPIO43 = CTS,
 *               44 = RTS, 45 = TX, 46 = RX (stock vayu.dts:11537-11620; the phone's live DT
 *               recovery-live.dts:11524-11600), so their cfg registers are
 *               0x0352b000 .. 0x0352e000: one 0x4000-byte mapping at 0x0352b000. Cfg bits,
 *               as vayu_audio.c's show_pins() decodes them: 1:0 pull, 5:2 function, 8:6 drive
 *               ((n + 1) * 2 mA), 9 output enable; io bit 0 is the input level and bit 1 the
 *               output value.
 *   the rails   stock vayu.dts and the phone's live DT recovery-live.dts:27906-27918:
 *               pm8150 S4A (ppid 0x01d) is vddio 1.8 V; pm8150 L7A (0x046) is the Bluetooth
 *               vdd-core/xtal rail, 1.8 V (RPMh ldoa7); pm8150l L2C (0x441) is vdd-pa,
 *               1.304 V (ldoc2); pm8150l L11C (0x44a) is vdd-ldo, 3.312 V (ldoc11). The
 *               regulator registers, read through the SPMI arbiter's observer: 0x04-0x05
 *               type/subtype, 0x08 status1 (bit7 VREG_OK, a hypothesis), 0x40-0x41 vset in
 *               mV little-endian, 0x45 mode, 0x46 en_ctl (bit7 enabled). "On" here is
 *               en_ctl bit7 or status1 bit7. RPMh (vendor rpmh-regulator.c): the command
 *               database address of a resource, +0 voltage in mV, +4 enable 1/0, +8 mode
 *               (PMIC5 LDO: 7 = HPM). One vayu_rpmh_send() per rail with three commands.
 *   the power   the QCA in-band wake/sleep handshake of upstream hci_qca.c
 *               (qca_regulator_init / qca_send_power_pulse) and vendor bluetooth-power.c:
 *               0xc0 at 2400 baud, then 0xfc at 115200; 0xfd/0xfc/0xfe are the in-band sleep
 *               bytes. The version request 01 00 fc 01 19 and the 0xff/00 02 answer are
 *               btqca.c's qca_read_soc_version / qca_soc_ver.
 *   the download  upstream btqca.c qca_tlv_check_data / qca_tlv_send_segment and
 *               qca_tlv_dnld_mode: the rampatch is a type 1 TLV whose header carries
 *               total_size (le32 @4), data_length (@8), format (@12), signature (@13),
 *               download_mode (@14), product_id (le16 @16), rom_build (le16 @18),
 *               patch_version (le16 @20) and entry (le32 @24). Its data goes out as H4
 *               01 00 fc (seg + 2) 1e seg, at most 243 bytes a segment; NONE (0) and CC (2)
 *               acknowledge every segment, VSE (1) and VSE_CC (3) only the last. The ack is
 *               the vendor event 04 ff 03 00 04 00: cresp 0x00, rtype 0x04, result. The NVM
 *               is a multi TLV (type 4) around a type 2 set whose tags are id le16, len le16,
 *               8 reserved bytes and len data bytes; tag 17 (HCI) normally gets data[0] |= 0x80
 *               (in-band sleep) and data[2] = 0 (QCA_BAUDRATE_115200), tag 27 (deep sleep)
 *               data[0] |= 0x01, and tag 2 is the BD address. vayu_bt_ibs=0 (the default) clears
 *               those two bits instead, so the controller should never start the 0xfd/0xfe
 *               handshake (a hypothesis; the RX framer filters those bytes either way).
 *               For crnv21.bin, tag 17's data at file offset 55 are 82 01 11 08 04 14 28 02 15
 *               before the patch, 82 01 00 08 04 14 28 02 15 with IBS on, and 02 01 00 08 04 14
 *               28 02 15 with vayu_bt_ibs=0.
 *   the tty     iOS 16.5's bluetoothd reads BT_HCI_TRANSPORT (one transport name, e.g.
 *               H4; user space work from its strings), probes access("/dev/cu.bluetooth") and then "/dev/uart.bluetooth",
 *               opens the tty, does tcgetattr / cfmakeraw / cfsetspeed / tcsetattr (TIOCGETA
 *               0x40487413, TIOCSETA 0x80487414) and reads it through EVFILT_READ. It speaks
 *               plain H4: 01 command, 02 ACL, 03 SCO, 04 event.
 *   the kctl    iOS 16.5's bluetoothd POSIX HCI driver (FUN_10006669c) does not open the tty.
 *               With BT_HCI_TRANSPORT=H4:POSIX:<name>,<baud> it makes socket(AF_SYSTEM 32,
 *               SOCK_STREAM 1, SYSPROTO_CONTROL 2), ioctl(CTLIOCGINFO 0xc0644e03,
 *               {ctl_name = name}), connect(sockaddr_ctl{sc_len 32, sc_family 32,
 *               ss_sysaddr 2, sc_id, sc_unit = 0}), getsockopt(2, TIOCGETA 0x40487413,
 *               &termios, &len = 0x48), cfmakeraw/cfsetspeed, setsockopt(2, TIOCSETA
 *               0x80487414, &termios, 0x48) and then reads/writes plain H4 on the socket. The
 *               kernel control is "com.vayu.bt.uart": CTL_FLAG_REG_SOCK_STREAM, 16 KB each
 *               way, no CTL_FLAG_PRIVILEGED (bluetoothd runs as mobile), one client at a time.
 *               The termios is answered and stored, never applied to the UART.
 *   the blue    bluetoothd asks BlueTool to power the controller before it uses HCI. Until
 *               that daemon exists, a stand-in that answers result = 0 (an XPC dictionary whose
 *               "result" key is an int64 0) is enough (FUN_1002722c8 in the 16.5 decompile; the
 *               request itself is not needed by this driver).
 *   the kqueue  a character device reaches EVFILTID_TTY only when d_type == D_TTY *and*
 *               cdevsw_flags[major] has CDEVSW_SELECT_KQUEUE: spec_kqfilter (spec_vnops.c:831-
 *               885) reads that flag at :863 and takes the tty path at :872; only
 *               cdevsw_setkqueueok (bsd_stubs.c:287-305) sets it, cdevsw_add does not. Without
 *               it the knote falls to EVFILTID_SPEC, whose attach refuses a plain EVFILT_READ
 *               without NOTE_LOWAT=1 (spec_vnops.c:3022-3027). filt_ttyattach (tty.c:3368-3415)
 *               then calls the driver's d_select with FREAD|FMARK and expects the selrecord on
 *               tp->t_rsel/t_wsel, which ttyselect (tty.c:1532-1593) makes even when data is
 *               ready; that is why d_select is ttyselect() under tty_lock.
 *
 * Boot-args (all parsed with PE_parse_boot_argn):
 *   vayu_bt=N         0/absent: nothing. Steps run in order 1..N (at most 9) and stop at the
 *                     first failure.
 *   vayu_bt_wait=S    seconds to sleep before step 1, default 20 (lets touch/boot finish their
 *                     RPMh TCS 0 use).
 *   vayu_bt_rails=0|1 default 1: step 4 votes the rails that the PMIC readout shows off or
 *                     cannot read; 0 never votes (and stops if a readable rail is off).
 *   vayu_bt_cts=0|1   default 0: 1 honours CTS (TX_TRANS_CFG bit 1 cleared), 0 ignores it
 *                     (steps 5..7).
 *   vayu_bt_fw_cts=0|1 default 1: step 8 honours CTS for the download; 0 ignores it.
 *   vayu_bt_ibs=0|1   default 0: 0 patches the NVM's tag 17 with in-band sleep off (bit 7
 *                     cleared) and tag 27 with deep sleep off (bit 0 cleared); 1 leaves both on,
 *                     as upstream btqca does. The RX framer filters 0xfd/0xfe/0xfc either way.
 *   vayu_bt_vsc=0|1|2 default 0: the outbound vendor-command shim in step 9. 0 leaves every
 *                     byte alone; 1 answers the host commands (type 0x01) in bt_vsc_rules and
 *                     passes every other vendor opcode (OGF 0x3f) to the controller; 2 passes
 *                     nothing unlisted and answers it with a Command Complete status 0. A
 *                     standard command (OGF != 0x3f) always goes to the chip unchanged.
 *   vayu_bt_keep=0|1  default 0: 1 leaves rails, clock and RX running at the end; 0 cancels the
 *                     S command. Step 9 always behaves as 1: the tty and the kctl need both.
 *   vayu_bt_giveback=0|1  default 0: 1 also votes "enable 0" for the rails this run turned on (same
 *                     RPMh client as UEFI: off by default).
 *
 * What userland must run after step 9:
 *
 *     BT_HCI_TRANSPORT=H4:POSIX:com.vayu.bt.uart,115200 bluetoothd
 *
 * That is bluetoothd's own POSIX driver: it looks the kernel control's name up with CTLIOCGINFO,
 * connects a PF_SYSTEM/SOCK_STREAM socket to it, gets and sets the termios with TIOCGETA and
 * TIOCSETA, and then speaks plain H4 on the socket. Plain BT_HCI_TRANSPORT=H4 BT_BAUDRATE=115200
 * still goes through the tty. bluetoothd also asks BlueTool to power the controller; a stand-in
 * that answers result = 0 (an XPC dictionary whose "result" key is an int64 0) is enough
 * (FUN_1002722c8). The nodes are 0666 root:wheel, and /dev/uart.bluetooth is the same dev_t.
 *
 * The SE is only touched when the wrapper's AHB path runs: m_ahb's halt bit clear, and s_ahb
 * either clear or hardware-gated (bit 1 HW_CTL; bit 31 stays set while an idle hardware-gated
 * branch has its bus off, as the loader and vayu_qup_spi.c treat it). Every wait has a timeout,
 * and on one the relevant registers are printed and the step chain stops; nothing spins forever
 * and nothing panics. The probe runs in a kernel thread, like vayu_haptic_init(), so a slow
 * register does not hold up boot. All sleeps are delay() based, as this file may not use IOSleep.
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/kern_control.h>
#include <sys/mbuf.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <sys/vayu_bt.h>
#include <sys/vayu_pas.h>

/*
 * Every line of this file also goes into the loader's log (vayu_camera.c's cam_printf): test sessions return
 * the loader's log, and the kernel message buffer (128 KB) has long wrapped by then, so plain printf lines
 * (the "vayu-bt: vsc:" answers among them) were missing from 231946 and 003059. The console printf only with
 * interrupts on (not on the way out: loader_return runs with interrupts off).
 */
extern void loader_log_putc(char c);
extern boolean_t ml_get_interrupts_enabled(void);
static int bt_printf(const char *format, ...) __printflike(1, 2);
#define printf bt_printf

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void delay(int usec);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* ---- the blocks ------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL    /* vayu_qup_spi.c:60 (GCC_PHYS) */
#define GCC_SIZE                0x80000UL       /* vayu_qup_spi.c:61 */

#define QUP_SE_PHYS             0x00C8C000UL    /* stock vayu.dts:23389-23405, qcom,qup_uart@0xc8c000 */
#define QUP_SE_SIZE             0x4000UL        /* its "reg" there: 0xc8c000, 0x4000 */

#define QUP_WRAPPER_PHYS        0x00CC0000UL    /* qcom,qupv3_2_geni_se@cc0000 */
#define QUP_WRAPPER_SIZE        0x1000UL        /* its version register only, 0x00cc0004 */

#define TLMM_BT_PHYS            0x0352B000UL    /* pin 43: east tile 0x03500000 (sm8150.dtsi:2381-2385)
                                                     + 0x1000 * 43 (pinctrl-sm8150.c ctl_reg) */
#define TLMM_BT_SIZE            0x4000UL        /* pins 43..46, one 0x1000 block each */
#define TLMM_PIN_STRIDE         0x1000u         /* ctl_reg = 0x1000 * id (pinctrl-sm8150.c) */

/* ---- GCC: the wrapper's AHB branches, SE3's branch and its RCG --------------------------------- */

#define GCC_QUPV3_VOTE          0x52014         /* the wrap 2 vote word: vayu_qup_spi.c:79 */
#define GCC_VOTE_S_AHB          (1u << 1)       /* gcc_qupv3_wrap_2_s_ahb_clk: vayu_qup_spi.c:80 */
#define GCC_VOTE_M_AHB          (1u << 2)       /* gcc_qupv3_wrap_2_m_ahb_clk: vayu_qup_spi.c:81 */
#define GCC_VOTE_SE3            (1u << 7)       /* wrap2_s3: gcc-sm8150.c:2453-2458, VAYU_BT_PHASE0.md §1 */

#define GCC_WRAP2_S_AHB_HALT    0x1e008         /* vayu_qup_spi.c:84 */
#define GCC_WRAP2_M_AHB_HALT    0x1e004         /* vayu_qup_spi.c:85 */
#define GCC_WRAP2_S3_HALT       0x1e4d4         /* gcc-sm8150.c:2454 */
#define GCC_WRAP2_S3_CMD        0x1e4d8         /* gcc-sm8150.c:731-732 */
#define GCC_WRAP2_S3_CFG        (GCC_WRAP2_S3_CMD + 0x4)   /* cfg = cmd + 4: vayu_qup_spi.c:88 */
#define GCC_WRAP2_S3_RCG_M      (GCC_WRAP2_S3_CMD + 0x8)   /* the RCG's one M (not a DFS level) */
#define GCC_WRAP2_S3_RCG_N      (GCC_WRAP2_S3_CMD + 0xc)   /* the RCG's one N (not a DFS level) */
#define GCC_WRAP2_S3_DFS        (GCC_WRAP2_S3_CMD + 0x14)  /* as vayu_qup_spi.c:89 */

#define GCC_WRAP2_S3_DFSR(n)    (GCC_WRAP2_S3_CMD + 0x1c + 4u * (n))  /* vendor gcc-sm8150.c:4188 */
#define GCC_WRAP2_S3_M(n)       (GCC_WRAP2_S3_CMD + 0x5c + 4u * (n))  /* clk-rcg2.c: M of level n */
#define GCC_WRAP2_S3_N(n)       (GCC_WRAP2_S3_CMD + 0x9c + 4u * (n))  /* clk-rcg2.c: N of level n */
#define GCC_DFS_LEVELS          8

#define GCC_DFSR_SRC_SHFT       8               /* DFSR [10:8] parent */
#define GCC_DFSR_SRC_MASK       0x7u
#define GCC_DFSR_HID_SHFT       0               /* DFSR [4:0] hid */
#define GCC_DFSR_HID_MASK       0x1fu
#define GCC_DFSR_MODE_SHFT      12              /* DFSR [13:12] mode */
#define GCC_DFSR_MODE_MASK      0x3u
#define GCC_DFSR_DFS_EN         (1u << 0)       /* not in DFSR: the DFS word's enable */

#define GCC_HALT_BIT            (1u << 31)      /* a branch's "clock is off" bit: vayu_qup_spi.c:92 */
#define GCC_CBCR_HW_CTL         (1u << 1)       /* s_ahb is hardware-gated: vayu_qup_spi.c:102 */
#define GCC_CMD_ROOT_OFF        (1u << 31)      /* RCG cmd root off: vayu_qup_spi.c:96 */
#define GCC_CFG_SRC_MASK        (0x7u << 8)     /* CFG source [10:8]: vayu_qup_spi.c:97 */
#define GCC_CFG_SRC_SHFT        8
#define GCC_CFG_DIV_MASK        0x1fu           /* CFG divider [4:0]: vayu_qup_spi.c:98 */

/* gcc_parent_map_0 (vendor gcc-sm8150.c and clk-rcg2.c): the three parents known here. */
#define GCC_PARENT_XO           0
#define GCC_PARENT_GPLL0        1
#define GCC_PARENT_GPLL0_EVEN   6
#define GCC_XO_HZ               19200000ull
#define GCC_GPLL0_HZ            600000000ull
#define GCC_GPLL0_EVEN_HZ       300000000ull

#define QUPV3_HW_VER            0x004           /* inside the wrapper at 0xcc0000: qcom-geni-se.h */

/* ---- GENI / SE registers (offsets inside qcom,qup_uart@0xc8c000) -------------------------------- */

#define GENI_FORCE_DEFAULT_REG  0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define SE_GENI_STATUS          0x040
#define GENI_SER_M_CLK_CFG      0x048
#define GENI_SER_S_CLK_CFG      0x04c
#define GENI_IF_DISABLE_RO      0x064
#define GENI_FW_REVISION_RO     0x068
#define SE_GENI_CLK_SEL         0x07c
#define SE_UART_LOOPBACK_CFG    0x22c
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_UART_TX_TRANS_CFG    0x25c
#define SE_UART_TX_PACKING_CFG0 0x260
#define SE_UART_TX_PACKING_CFG1 0x264
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BIT_LEN 0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_UART_RX_PACKING_CFG0 0x284
#define SE_UART_RX_PACKING_CFG1 0x288
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE_CNT    0x294
#define SE_UART_TX_PARITY_CFG   0x2a4
#define SE_UART_RX_PARITY_CFG   0x2a8
#define SE_UART_MANUAL_RFR      0x2ac
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_CMD_CTRL_REG  0x634
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
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define SE_DMA_GENERAL_CFG      0xe30

/* SE_GENI_STATUS */
#define SE_STATUS_M_ACTIVE      (1u << 0)       /* M_GENI_CMD_ACTIVE */
#define SE_STATUS_S_ACTIVE      (1u << 12)      /* S_GENI_CMD_ACTIVE */

/* SER_M/S_CLK_CFG */
#define SER_CLK_EN              (1u << 0)
#define SER_CLK_DIV_SHFT        4               /* [15:4] divider */
#define SER_CLK_DIV_MASK        0xfffu

/* GENI_IF_DISABLE_RO */
#define IF_DISABLE_FIFO         (1u << 0)       /* FIFO_IF_DISABLE */

/* GENI_FW_REVISION_RO: [15:8] is the protocol the SE's loaded firmware speaks */
#define FW_REV_PROTOCOL_MSK     (0xffu << 8)    /* geni-se.h:36,180-181 */
#define FW_REV_PROTOCOL_SHFT    8
#define FW_REV_VERSION_MSK      0xffu
#define SE_PROTO_SPI            1               /* geni-se.h:36 */
#define SE_PROTO_UART           2
#define SE_PROTO_I2C            3

/* SE_IRQ_EN and the DMA CGC default of geni_se_init() */
#define SE_IRQ_BITS             0xfu
#define DMA_CGC_DEFAULT         0xfu
#define GENI_CGC_DEFAULT        0x7fu
#define GENI_OUTPUT_DEFAULT     0x7fu

/* SE_GENI_M/S_CMD_CTRL_REG */
#define GENI_CMD_CANCEL         (1u << 2)       /* M_GENI_CMD_CANCEL */
#define GENI_CMD_ABORT          (1u << 1)       /* M_GENI_CMD_ABORT */

/* M/S IRQ status, same low bits on both sides */
#define GENI_IRQ_CMD_DONE       (1u << 0)
#define GENI_IRQ_CMD_CANCEL     (1u << 4)
#define GENI_IRQ_CMD_ABORT      (1u << 5)
#define GENI_IRQ_RX_WM          (1u << 26)
#define GENI_IRQ_RX_LAST        (1u << 27)
#define GENI_IRQ_TX_WM          (1u << 30)

/* M/S command opcodes */
#define M_OPCODE_START_TX       (1u << 27)      /* UART_START_TX = 1 */
#define S_CMD_START_READ        0x08000080u     /* UART_START_READ, UART_PARAM_RFR_OPEN 0x80 */

/* TX_TRANS_CFG */
#define TX_TRANS_PARITY_EN      (1u << 0)
#define TX_TRANS_CTS_MASK       (1u << 1)       /* 1: ignore CTS */

/* FIFO status */
#define TX_FIFO_WC_MASK         0x0fffffffu     /* [27:0] */
#define RX_FIFO_WC_MASK         0x01ffffffu     /* [24:0] */
#define RX_FIFO_LAST            (1u << 31)
#define RX_FIFO_LBV_SHFT        28              /* [30:28] RX_LAST_BYTE_VALID */
#define RX_FIFO_LBV_MASK        0x7u

/* SE_HW_PARAM_0/1 */
#define FIFO_DEPTH_SHFT         16              /* [21:16] depth in words */
#define FIFO_DEPTH_MASK         0x3fu
#define TX_FIFO_WIDTH_SHFT      24              /* [29:24] width in bits */
#define TX_FIFO_WIDTH_MASK      0x3fu

/* UART configuration for this file */
#define UART_WORD_LEN           8
#define UART_STALE_CNT          0xa0            /* 10 * 16 */
#define UART_OVERSAMPLING       32
#define UART_MAX_DIV            4095u

/* ---- PMIC arbiter (the read path of vayu_camera.c / vayu_haptic.c) ------------------------------ */

#define ARB_CORE_PHYS           0x0c440000UL    /* the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* which system may write through each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* reads: 0x80 per channel for our system (the first) */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0               /* who we are to the arbiter: the DT's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long" */
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

/* The PMIC regulator registers (vendor regulator core, as the camera survey reads them). */
#define PMIC_REG_TYPE           0x04
#define PMIC_REG_STATUS1        0x08
#define PMIC_REG_VSET           0x40
#define PMIC_REG_MODE           0x45
#define PMIC_REG_EN_CTL         0x46
#define PMIC_STATUS1_VREG_OK    (1u << 7)       /* a hypothesis, as the task says */
#define PMIC_EN_CTL_ENABLED     (1u << 7)

/* ---- TLMM pin registers, as vayu_audio.c's show_pins() decodes them ----------------------------- */

#define TLMM_CFG_PULL_MASK      0x3u            /* cfg [1:0] pull */
#define TLMM_CFG_FUNC_SHFT      2               /* cfg [5:2] function */
#define TLMM_CFG_FUNC_MASK      0xfu
#define TLMM_CFG_FUNC_QUP13     1
#define TLMM_CFG_DRIVE_SHFT     6               /* cfg [8:6] drive: (n + 1) * 2 mA */
#define TLMM_CFG_DRIVE_MASK     0x7u
#define TLMM_CFG_OE_SHFT        9               /* cfg [9] output enable */
#define TLMM_IO_IN              (1u << 0)       /* io [0] input level */

/* ---- the rails --------------------------------------------------------------------------------- */

struct peripheral {
	const char *name;
	uint16_t id;            /* slave << 8 | peripheral */
	uint16_t channel;
	bool found;
};

struct bt_rail {
	const char *id;         /* the RPMh resource name; NULL: read only, we may not vote it */
	const char *name;       /* for the log */
	uint16_t ppid;          /* sid << 8 | peripheral */
	uint32_t mv;            /* the voltage to vote, in mV */
	struct peripheral pmic;
	uint32_t addr;          /* vayu_rpmh_address(id), looked up when it is voted */
	bool voted_on;          /* this run turned it on: the end gives it back */
	bool known;             /* the last readout answered: on/off is meaningful */
	bool on;                /* en_ctl bit7 or status1 bit7, from the last readout */
	uint8_t status1, mode, en_ctl;
	uint16_t vset;
};

static struct bt_rail bt_rails[] = {
	/* pm8150 S4A: vddio 1.8 V. Shared I/O; no RPMh name of ours, so it is only read. */
	{ .id = NULL, .name = "pm8150 S4A (vddio 1.8 V)", .ppid = 0x01d, .mv = 1800 },
	/* pm8150 L7A: the Bluetooth vdd-core/xtal rail, 1.8 V, RPMh ldoa7. */
	{ .id = "ldoa7", .name = "pm8150 L7A (BT vdd-core/xtal 1.8 V)", .ppid = 0x046, .mv = 1800 },
	/* pm8150l L2C: vdd-pa 1.304 V, RPMh ldoc2. */
	{ .id = "ldoc2", .name = "pm8150l L2C (vdd-pa 1.304 V)", .ppid = 0x441, .mv = 1304 },
	/* pm8150l L11C: vdd-ldo 3.312 V, RPMh ldoc11. */
	{ .id = "ldoc11", .name = "pm8150l L11C (vdd-ldo 3.312 V)", .ppid = 0x44a, .mv = 3312 },
};

#define BT_RAIL_COUNT   (sizeof(bt_rails) / sizeof(bt_rails[0]))

/* ---- state ------------------------------------------------------------------------------------- */

static vm_offset_t gcc_va, se_va, wrapper_va, tlmm_va;
static vm_offset_t arb_core, arb_cnfg, arb_observer;
static bool arb_ready, arb_tried;

static uint32_t g_level = 0;
static uint32_t g_wait_s = 20;
static bool g_rails = true;
static bool g_cts = false;
static bool g_fw_cts = true;
static bool g_keep = false;
static bool g_giveback = false;
static bool g_ibs = false;              /* vayu_bt_ibs: 0 keeps the NVM's in-band/deep sleep off */
static uint32_t g_fw_rom = 0x21;        /* step 6's fw_rom: the crbtfw/crnv names; default 0x21 */

static bool g_step8_done = false;       /* "step 8 done" was printed: step 9 may run */
static bool g_step9_ok = false;         /* the tty/kctl are up: bt_finish() leaves everything running */

static bool g_rx_running = false;
static uint32_t g_baud = 2400;
static uint32_t g_tx_depth, g_rx_depth, g_tx_width;

static uint64_t g_dfs_rate[GCC_DFS_LEVELS];
static bool g_dfs_valid[GCC_DFS_LEVELS];

static uint32_t g_arb_channels;

/* ---- small helpers ----------------------------------------------------------------------------- */

/* One 32-bit load, as before. */
static inline uint32_t
bt_rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

/* One 32-bit store. Used for the SE (through bt_se_wr) and for the arbiter's read command only. */
static inline void
bt_wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
}

static uint64_t
bt_now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/* IOSleep is not available here: a millisecond sleep is delay(us), in chunks that fit an int. */
static void
bt_sleep_ms(uint64_t ms)
{
	while (ms != 0) {
		uint32_t chunk = ms > 1000 ? 1000 : (uint32_t)ms;

		delay((int)(chunk * 1000));
		ms -= chunk;
	}
}

static void
bt_hex(char *out, size_t size, const uint8_t *bytes, uint32_t count)
{
	size_t at = 0;

	out[0] = 0;
	for (uint32_t i = 0; i < count && at + 4 < size; i++) {
		at += (size_t)snprintf(out + at, size - at, "%s%02x", i != 0 ? " " : "", (unsigned)bytes[i]);
	}
}

/* One line per 32 bytes at most, as the rules ask. */
static void
bt_dump_counted(const char *what, const uint8_t *bytes, uint32_t count)
{
	if (count == 0) {
		printf("vayu-bt: %s: 0 bytes\n", what);
		return;
	}
	for (uint32_t off = 0; off < count; off += 32) {
		char hex[3 * 32 + 1];
		uint32_t chunk = count - off > 32 ? 32 : count - off;

		bt_hex(hex, sizeof(hex), bytes + off, chunk);
		if (off == 0) {
			printf("vayu-bt: %s: %u bytes: %s\n", what, count, hex);
		} else {
			printf("vayu-bt: %s: ... %s\n", what, hex);
		}
	}
}

static const char *
bt_protocol_name(uint32_t protocol)
{
	switch (protocol) {
	case SE_PROTO_SPI:
		return "SPI";
	case SE_PROTO_UART:
		return "UART";
	case SE_PROTO_I2C:
		return "I2C";
	default:
		return "none/unknown";
	}
}

/* ---- GCC DFS read, read-only ------------------------------------------------------------------- */

/*
 * The state of one branch, as the loader reads it (xnuloader/loader.c nvt_gcc_branch_on) and as
 * vayu_qup_spi.c does: s_ahb is hardware-gated, so bit 31 stays set while its bus is idle and
 * bit 1 (HW_CTL) says "the clock will run when the bus moves". A hardware-gated s_ahb counts as
 * running; an m_ahb or SE3 with bit 31 set does not.
 */
static const char *
bt_branch_state(uint32_t value, bool hwcg)
{
	if (hwcg && (value & GCC_CBCR_HW_CTL) != 0) {
		return "hw-gated";
	}
	return (value & GCC_HALT_BIT) != 0 ? "OFF" : "on";
}

/* The wrapper 2 AHB path: m_ahb on, and s_ahb on or hardware-gated. */
static bool
bt_ahb_on(void)
{
	uint32_t s_ahb = bt_rd(gcc_va, GCC_WRAP2_S_AHB_HALT);
	uint32_t m_ahb = bt_rd(gcc_va, GCC_WRAP2_M_AHB_HALT);

	return ((s_ahb & GCC_HALT_BIT) == 0 || (s_ahb & GCC_CBCR_HW_CTL) != 0) &&
	       (m_ahb & GCC_HALT_BIT) == 0;
}

/*
 * The rate of one DFS level, from its DFSR, M and N words. Returns 0 and *valid = false when the
 * parent is not one of gcc_parent_map_0's three known sources.
 */
static uint64_t
bt_dfs_rate(uint32_t dfsr, uint32_t mreg, uint32_t nreg, bool *valid)
{
	uint32_t src = (dfsr >> GCC_DFSR_SRC_SHFT) & GCC_DFSR_SRC_MASK;
	uint32_t hid = (dfsr >> GCC_DFSR_HID_SHFT) & GCC_DFSR_HID_MASK;
	uint32_t mode = (dfsr >> GCC_DFSR_MODE_SHFT) & GCC_DFSR_MODE_MASK;
	uint64_t rate;

	*valid = false;
	switch (src) {
	case GCC_PARENT_XO:
		rate = GCC_XO_HZ;
		break;
	case GCC_PARENT_GPLL0:
		rate = GCC_GPLL0_HZ;
		break;
	case GCC_PARENT_GPLL0_EVEN:
		rate = GCC_GPLL0_EVEN_HZ;
		break;
	default:
		return 0;
	}
	if (hid != 0) {
		rate = rate * 2 / (hid + 1);
	}
	if (mode != 0) {
		uint32_t m = mreg & 0xffffu;
		uint32_t n = ((~nreg) & 0xffffu) + m;

		if (m == 0 || n == 0) {
			return 0;
		}
		rate = rate * m / n;
	}
	*valid = true;
	return rate;
}

/* ---- the PMIC arbiter, read only --------------------------------------------------------------- */

static void
bt_arb_lock(void)
{
	hakios_spmi_lock();
}

static void
bt_arb_unlock(void)
{
	hakios_spmi_unlock();
}

static const char *
bt_arb_wait(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = bt_rd(channel, CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" :
	       (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" :
	       (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static const char *bt_spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count);

/* A dropped transfer is asked for again, twice at most (the buttons' driver sees one now and then). */
static const char *
bt_spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = bt_spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/*
 * The arbiter's read command: the one store this file makes outside the SE. It does not change a
 * PMIC register, it is how an observer read is asked for (as vayu_camera.c and vayu_haptic.c do).
 */
static const char *
bt_spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel;
	const char *problem;
	uint32_t data[2];

	if (!p->found) {
		return "no channel";
	}
	channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	bt_arb_lock();
	bt_wr(channel, CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = bt_arb_wait(channel);
	data[0] = bt_rd(channel, OBSERVER_DATA0);
	data[1] = count > 4 ? bt_rd(channel, OBSERVER_DATA1) : 0;
	bt_arb_unlock();
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/* Which channel is that ppid's; the first, unless a later one is ours to write through. */
static void
bt_find_channel(struct bt_rail *r)
{
	bool found = false;

	r->pmic.name = r->name;
	r->pmic.id = r->ppid;
	r->pmic.found = false;
	for (uint32_t n = 0; n < g_arb_channels; n++) {
		uint32_t map = bt_rd(arb_core, ARB_CHANNEL(n));
		uint32_t owner = 0xff;

		if (map == 0 || ((map >> 8) & 0xfff) != r->ppid) {
			continue;
		}
		if (arb_cnfg != 0) {
			owner = bt_rd(arb_cnfg, ARB_OWNER(n)) & 7;
		}
		if (!found || owner == ARB_SYSTEM) {
			r->pmic.found = true;
			r->pmic.channel = (uint16_t)n;
			found = true;
		}
		if (owner == ARB_SYSTEM) {
			break;
		}
	}
	if (!found) {
		printf("vayu-bt: pmic %s (ppid %#x): no channel of the arbiter\n", r->name,
		    (unsigned)r->ppid);
	}
}

static bool
bt_arb_setup(void)
{
	uint32_t version, channels;

	if (arb_ready) {
		return true;
	}
	if (arb_tried) {
		return false;
	}
	arb_tried = true;
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	if (arb_core == 0 || arb_observer == 0) {
		printf("vayu-bt: pmic: cannot map the arbiter (core %#lx, observer %#lx): no readout\n",
		    (unsigned long)arb_core, (unsigned long)arb_observer);
		return false;
	}
	version = bt_rd(arb_core, ARB_VERSION);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("vayu-bt: pmic: the arbiter version %#x is not version 5: no readout\n", version);
		return false;
	}
	channels = bt_rd(arb_core, ARB_FEATURES) & 0x7ffu;
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	g_arb_channels = channels;
	printf("vayu-bt: pmic: arbiter version %#x, %u channels%s\n", version, channels,
	    arb_cnfg == 0 ? " (the owner map is not readable)" : "");
	for (unsigned i = 0; i < BT_RAIL_COUNT; i++) {
		bt_find_channel(&bt_rails[i]);
	}
	arb_ready = true;
	return true;
}

static bool
bt_pmic_read(struct bt_rail *r)
{
	uint8_t type[2], status1, vset[2], mode, en;
	const char *problem;

	r->known = false;
	if (!arb_ready || !r->pmic.found) {
		printf("vayu-bt: pmic %s (ppid %#x): %s\n", r->name, (unsigned)r->ppid,
		    arb_ready ? "no channel" : "the arbiter is not readable");
		return false;
	}
	problem = bt_spmi_read(&r->pmic, PMIC_REG_TYPE, type, 2);
	if (problem == NULL) {
		problem = bt_spmi_read(&r->pmic, PMIC_REG_STATUS1, &status1, 1);
	}
	if (problem == NULL) {
		problem = bt_spmi_read(&r->pmic, PMIC_REG_VSET, vset, 2);
	}
	if (problem == NULL) {
		problem = bt_spmi_read(&r->pmic, PMIC_REG_MODE, &mode, 1);
	}
	if (problem == NULL) {
		problem = bt_spmi_read(&r->pmic, PMIC_REG_EN_CTL, &en, 1);
	}
	if (problem != NULL) {
		printf("vayu-bt: pmic %s (ppid %#x, channel %u): %s\n", r->name, (unsigned)r->ppid,
		    (unsigned)r->pmic.channel, problem);
		return false;
	}
	r->status1 = status1;
	r->vset = (uint16_t)(vset[0] | (vset[1] << 8));
	r->mode = mode;
	r->en_ctl = en;
	r->on = (en & PMIC_EN_CTL_ENABLED) != 0 || (status1 & PMIC_STATUS1_VREG_OK) != 0;
	r->known = true;
	printf("vayu-bt: pmic %s (ppid %#x, channel %u): type %u sub %u, status1 %#x (vreg ok %u), "
	    "vset %u mV, mode %#x, en_ctl %#x (enabled %u): %s\n", r->name, (unsigned)r->ppid,
	    (unsigned)r->pmic.channel, (unsigned)type[0], (unsigned)type[1], (unsigned)status1,
	    (status1 & PMIC_STATUS1_VREG_OK) ? 1 : 0, (unsigned)r->vset, (unsigned)mode,
	    (unsigned)en, (en & PMIC_EN_CTL_ENABLED) ? 1 : 0, r->on ? "on" : "off");
	return true;
}

static bool
bt_pmic_survey(void)
{
	bool all = true;

	if (!bt_arb_setup()) {
		return false;
	}
	for (unsigned i = 0; i < BT_RAIL_COUNT; i++) {
		if (!bt_pmic_read(&bt_rails[i])) {
			all = false;
		}
	}
	return all;
}

/* ---- the pins, read-only ------------------------------------------------------------------------ */

static bool
bt_tlmm_ready(void)
{
	if (tlmm_va == 0) {
		tlmm_va = ml_io_map(TLMM_BT_PHYS, TLMM_BT_SIZE);
	}
	if (tlmm_va == 0) {
		printf("vayu-bt: cannot map the TLMM window at 0x352b000: the pins are not read\n");
		return false;
	}
	return true;
}

/*
 * One mapping covers pins 43..46. In each 0x1000-byte block the cfg register is at +0x0 and
 * the io register at +0x4 (pinctrl-sm8150.c ctl_reg, and as vayu_qup_spi.c reads TLMM).
 * Returns true when all four are function 1 (qup13).
 */
static bool
bt_pins(void)
{
	static const char *const names[] = { "CTS", "RTS", "TX", "RX" };   /* GPIO 43..46: stock vayu.dts:11537-11620 */
	bool all = true;

	if (!bt_tlmm_ready()) {
		return false;
	}
	for (uint32_t i = 0; i < 4; i++) {
		uint32_t pin = 43 + i;
		uint32_t cfg = bt_rd(tlmm_va, TLMM_PIN_STRIDE * i);
		uint32_t io = bt_rd(tlmm_va, TLMM_PIN_STRIDE * i + 4);
		uint32_t function = (cfg >> TLMM_CFG_FUNC_SHFT) & TLMM_CFG_FUNC_MASK;
		uint32_t drive = (cfg >> TLMM_CFG_DRIVE_SHFT) & TLMM_CFG_DRIVE_MASK;

		printf("vayu-bt: gpio%u (%s): cfg %#x: function %u%s, pull %u, %u mA, oe %u; io %#x (in %u)\n",
		    pin, names[i], cfg, function, function == TLMM_CFG_FUNC_QUP13 ? " (qup13)" : "",
		    cfg & TLMM_CFG_PULL_MASK, (drive + 1) * 2, (cfg >> TLMM_CFG_OE_SHFT) & 1, io,
		    io & TLMM_IO_IN);
		if (function != TLMM_CFG_FUNC_QUP13) {
			all = false;
		}
	}
	return all;
}

/* CTS is GPIO 43's input bit; -1 when the pin cannot be read. */
static int
bt_cts_level(void)
{
	if (!bt_tlmm_ready()) {
		return -1;
	}
	return (bt_rd(tlmm_va, 4) & TLMM_IO_IN) != 0;
}

static void
bt_report_cts(const char *when)
{
	int cts = bt_cts_level();

	if (cts < 0) {
		printf("vayu-bt: %s is not readable\n", when);
	} else {
		printf("vayu-bt: %s is %s\n", when, cts != 0 ? "high" : "low");
	}
}

/* ---- the SE: mapping, guarded reads and logged writes ------------------------------------------- */

static bool
bt_se_ready(void)
{
	if (se_va != 0) {
		return true;
	}
	se_va = ml_io_map(QUP_SE_PHYS, QUP_SE_SIZE);
	if (se_va == 0) {
		printf("vayu-bt: cannot map the SE at 0xc8c000\n");
		return false;
	}
	return true;
}

/*
 * Offsets 0x0000..0x0fff only. An SE's 0x2000+ window is TrustZone-only; a read there hangs the
 * CPU, so the guard refuses it before any access.
 */
static uint32_t
bt_se_rd(uint32_t offset)
{
	if (offset >= 0x1000u || se_va == 0) {
		printf("vayu-bt: refusing an SE read at %#x (outside 0x0000..0x0fff)\n", offset);
		return 0xffffffffu;
	}
	return bt_rd(se_va, offset);
}

/*
 * Every state-changing SE write is printed before it is made, as the rules ask, except the TX
 * FIFO feed (one tx line per command is enough) and the firmware segments' quiet path, which
 * uses bt_wr directly as step 8 asks.
 */
static void
bt_se_wr(uint32_t offset, uint32_t value, const char *what)
{
	if (offset >= 0x1000u || se_va == 0) {
		printf("vayu-bt: refusing an SE write at %#x = %#x (%s)\n", offset, value, what);
		return;
	}
	printf("vayu-bt: write se %#x = %#x (%s)\n", offset, value, what);
	bt_wr(se_va, offset, value);
}

static void
bt_se_update(uint32_t offset, uint32_t bits, bool set, const char *what)
{
	uint32_t value = bt_se_rd(offset);

	value = set ? value | bits : value & ~bits;
	bt_se_wr(offset, value, what);
}

/* Wait for ((value & mask) != 0) == set; false on timeout. *last is the final read. */
static bool
bt_se_wait(uint32_t offset, uint32_t mask, bool set, uint32_t timeout_us, uint32_t *last)
{
	uint64_t deadline = bt_now_us() + timeout_us;
	uint32_t value;

	for (;;) {
		value = bt_se_rd(offset);
		if (((value & mask) != 0) == set) {
			if (last != NULL) {
				*last = value;
			}
			return true;
		}
		if (bt_now_us() >= deadline) {
			if (last != NULL) {
				*last = value;
			}
			return false;
		}
		delay(10);
	}
}

/*
 * The cancel/abort dance of geni_se_cancel_s_cmd()/geni_se_abort_s_cmd(): write cancel, wait for
 * its IRQ bit up to 10 ms, else write abort and wait for that. A command that finished on its own
 * (its STATUS active bit clear) counts as done.
 */
static bool
bt_cancel_wait(uint32_t irq_offset, uint32_t irq_bit, uint32_t status_bit, uint32_t *irq)
{
	uint64_t deadline = bt_now_us() + 10000;
	uint32_t value;

	for (;;) {
		value = bt_se_rd(irq_offset);
		if ((value & irq_bit) != 0 || (bt_se_rd(SE_GENI_STATUS) & status_bit) == 0) {
			*irq = value;
			return true;
		}
		if (bt_now_us() >= deadline) {
			*irq = value;
			return false;
		}
		delay(10);
	}
}

static bool
bt_cancel_side(const char *side, uint32_t status_bit, uint32_t ctrl_offset, uint32_t irq_offset,
    uint32_t clear_offset)
{
	uint32_t irq = 0;

	printf("vayu-bt: %s command active: cancel it\n", side);
	bt_se_wr(ctrl_offset, GENI_CMD_CANCEL, "cancel");
	if (!bt_cancel_wait(irq_offset, GENI_IRQ_CMD_CANCEL, status_bit, &irq)) {
		printf("vayu-bt: %s cancel did not complete: status %#x, irq %#x; abort\n", side,
		    bt_se_rd(SE_GENI_STATUS), irq);
		bt_se_wr(ctrl_offset, GENI_CMD_ABORT, "abort");
		if (!bt_cancel_wait(irq_offset, GENI_IRQ_CMD_ABORT, status_bit, &irq)) {
			printf("vayu-bt: %s abort did not complete: status %#x, irq %#x\n", side,
			    bt_se_rd(SE_GENI_STATUS), irq);
			bt_se_wr(clear_offset, 0xffffffffu, "clear the IRQs");
			return false;
		}
	}
	bt_se_wr(clear_offset, 0xffffffffu, "clear the IRQs");
	return true;
}

static bool
bt_cancel_m(void)
{
	return bt_cancel_side("M", SE_STATUS_M_ACTIVE, SE_GENI_M_CMD_CTRL_REG, SE_GENI_M_IRQ_STATUS,
	    SE_GENI_M_IRQ_CLEAR);
}

static bool
bt_cancel_s(void)
{
	return bt_cancel_side("S", SE_STATUS_S_ACTIVE, SE_GENI_S_CMD_CTRL_REG, SE_GENI_S_IRQ_STATUS,
	    SE_GENI_S_IRQ_CLEAR);
}

static bool
bt_cancel_active(void)
{
	if ((bt_se_rd(SE_GENI_STATUS) & SE_STATUS_M_ACTIVE) != 0 && !bt_cancel_m()) {
		return false;
	}
	if ((bt_se_rd(SE_GENI_STATUS) & SE_STATUS_S_ACTIVE) != 0 && !bt_cancel_s()) {
		return false;
	}
	return true;
}

static bool
bt_read_fifo_params(void)
{
	uint32_t hw0 = bt_se_rd(SE_HW_PARAM_0);
	uint32_t hw1 = bt_se_rd(SE_HW_PARAM_1);

	g_tx_depth = (hw0 >> FIFO_DEPTH_SHFT) & FIFO_DEPTH_MASK;
	g_tx_width = (hw0 >> TX_FIFO_WIDTH_SHFT) & TX_FIFO_WIDTH_MASK;
	g_rx_depth = (hw1 >> FIFO_DEPTH_SHFT) & FIFO_DEPTH_MASK;
	if (g_tx_depth == 0 || g_rx_depth < 2) {
		printf("vayu-bt: the FIFO depths read as tx %u words, rx %u words: stopping\n",
		    g_tx_depth, g_rx_depth);
		return false;
	}
	return true;
}

/* ---- baud rate: a DFS level and an integer divider ---------------------------------------------- */

static bool bt_start_rx(void);

static bool
bt_set_baud(uint32_t baud)
{
	uint64_t desired = (uint64_t)baud * UART_OVERSAMPLING;
	uint64_t rate = 0;
	uint32_t level = 0, divider;

	for (uint32_t i = 0; i < GCC_DFS_LEVELS; i++) {
		if (!g_dfs_valid[i] || g_dfs_rate[i] < desired || g_dfs_rate[i] % desired != 0) {
			continue;
		}
		if (rate == 0 || g_dfs_rate[i] < rate) {
			rate = g_dfs_rate[i];
			level = i;
		}
	}
	if (rate == 0) {
		printf("vayu-bt: baud %u: no DFS level is a multiple of %llu Hz: stopping\n", baud,
		    (unsigned long long)desired);
		return false;
	}
	divider = (uint32_t)(rate / desired);
	if (divider < 1 || divider > UART_MAX_DIV) {
		printf("vayu-bt: baud %u: the divider %u is outside 1..%u: stopping\n", baud, divider,
		    UART_MAX_DIV);
		return false;
	}
	if (g_rx_running && !bt_cancel_s()) {
		return false;
	}
	bt_se_wr(SE_GENI_CLK_SEL, level, "CLK_SEL: the DFS level");
	bt_se_wr(GENI_SER_M_CLK_CFG, (divider << SER_CLK_DIV_SHFT) | SER_CLK_EN, "M clock divider");
	bt_se_wr(GENI_SER_S_CLK_CFG, (divider << SER_CLK_DIV_SHFT) | SER_CLK_EN, "S clock divider");
	printf("vayu-bt: baud %u: dfs level %u (%llu Hz), divider %u\n", baud, level,
	    (unsigned long long)rate, divider);
	g_baud = baud;
	if (g_rx_running) {
		(void)bt_start_rx();
	}
	return true;
}

/* ---- RX and TX, polled (nothing routes the GIC interrupt) --------------------------------------- */

static bool
bt_start_rx(void)
{
	bt_se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu, "clear S IRQs");
	bt_se_wr(SE_GENI_S_CMD0, S_CMD_START_READ, "UART_START_READ, RFR open");
	g_rx_running = true;
	return true;
}

/*
 * Drain the RX FIFO into buf (at most max bytes kept) for timeout_us. The FIFO is always drained
 * completely: a word beyond max is dropped, not left to block the next read.
 */
static uint32_t
bt_rx(uint8_t *buf, uint32_t max, uint32_t timeout_us)
{
	uint64_t deadline = bt_now_us() + timeout_us;
	uint32_t got = 0;

	do {
		uint32_t status = bt_se_rd(SE_GENI_RX_FIFO_STATUS);
		uint32_t words = status & RX_FIFO_WC_MASK;

		if (words > 256) {
			/* A bus error can read as all ones; do not try to drain 33 million words. */
			printf("vayu-bt: rx_fifo_status %#x has an impossible word count %u: reading 256\n",
			    status, words);
			words = 256;
		}
		if (words != 0) {
			bool last = (status & RX_FIFO_LAST) != 0;
			uint32_t lbv = (status >> RX_FIFO_LBV_SHFT) & RX_FIFO_LBV_MASK;
			uint32_t bytes = 4 * (words - 1) + ((last && lbv != 0) ? lbv : 4);

			for (uint32_t w = 0; w < words; w++) {
				uint32_t word = bt_se_rd(SE_GENI_RX_FIFOn);

				for (uint32_t k = 0; k < 4; k++) {
					if (4 * w + k >= bytes) {
						break;
					}
					if (got < max) {
						buf[got++] = (uint8_t)(word >> (8 * k));
					}
				}
			}
			/* an IRQ acknowledge, not configuration: not printed (it would be one line per drain) */
			bt_wr(se_va, SE_GENI_S_IRQ_CLEAR, bt_se_rd(SE_GENI_S_IRQ_STATUS));
		} else {
			delay(200);
		}
	} while (bt_now_us() < deadline);
	return got;
}

/*
 * The TX half: wait for the M command to be idle, clear its IRQs, set the length, start it and
 * feed the FIFO. Up to 4 * tx_depth bytes fit the FIFO at once; longer writes (the firmware's
 * 243-byte segments) start the command first and, before every word, wait until the FIFO count
 * is below its depth, 1 ms at most per word. The FIFO feed itself is never logged word by word.
 * quiet: no tx line and no write lines at all (bt_wr directly), as the firmware segments ask; a
 * failure still prints.
 */
static bool
bt_tx_words(const uint8_t *bytes, uint32_t count, bool quiet)
{
	uint32_t words, status = 0, irq = 0;
	uint64_t extra;
	bool long_write = count > 4 * g_tx_depth;

	if (count == 0) {
		return true;
	}
	if (!quiet) {
		char hex[3 * 32 + 1];
		uint32_t shown = count > 32 ? 32 : count;

		bt_hex(hex, sizeof(hex), bytes, shown);
		printf("vayu-bt: tx %u bytes: %s%s\n", count, hex, count > shown ? " ..." : "");
	}
	if (!bt_se_wait(SE_GENI_STATUS, SE_STATUS_M_ACTIVE, false, 10000, &status)) {
		printf("vayu-bt: tx: the M command stays active: status %#x, m_irq %#x, tx_fifo %#x\n",
		    status, bt_se_rd(SE_GENI_M_IRQ_STATUS), bt_se_rd(SE_GENI_TX_FIFO_STATUS));
		return false;
	}
	if (quiet) {
		bt_wr(se_va, SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
		bt_wr(se_va, SE_UART_TX_TRANS_LEN, count);
		bt_wr(se_va, SE_GENI_M_CMD0, M_OPCODE_START_TX);
	} else {
		bt_se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu, "clear M IRQs");
		bt_se_wr(SE_UART_TX_TRANS_LEN, count, "TX length");
		bt_se_wr(SE_GENI_M_CMD0, M_OPCODE_START_TX, "UART_START_TX");
	}
	words = (count + 3) / 4;
	for (uint32_t w = 0; w < words; w++) {
		uint32_t word = 0;

		if (long_write) {
			uint64_t deadline = bt_now_us() + 100000;   /* 100 ms: CTS may hold TX while the controller writes the patch */

			for (;;) {
				uint32_t txf = bt_se_rd(SE_GENI_TX_FIFO_STATUS);

				if ((txf & TX_FIFO_WC_MASK) < g_tx_depth) {
					break;
				}
				if (bt_now_us() >= deadline) {
					printf("vayu-bt: tx: the TX FIFO stays full before word %u of "
					    "%u: status %#x, m_irq %#x, tx_fifo %#x\n", w + 1, words,
					    bt_se_rd(SE_GENI_STATUS), bt_se_rd(SE_GENI_M_IRQ_STATUS), txf);
					if (quiet) {
						bt_wr(se_va, SE_GENI_M_IRQ_CLEAR,
						    bt_se_rd(SE_GENI_M_IRQ_STATUS));
					} else {
						bt_se_wr(SE_GENI_M_IRQ_CLEAR,
						    bt_se_rd(SE_GENI_M_IRQ_STATUS),
						    "clear the M IRQs that were set");
					}
					(void)bt_cancel_m();
					return false;
				}
				delay(10);
			}
		}
		for (uint32_t k = 0; k < 4; k++) {
			uint32_t at = 4 * w + k;

			if (at < count) {
				word |= (uint32_t)bytes[at] << (8 * k);
			}
		}
		/*
		 * The FIFO feed is not logged word by word in either path: the one tx line above is
		 * the command's line.
		 */
		bt_wr(se_va, SE_GENI_TX_FIFOn, word);
	}
	extra = (uint64_t)2 * (10ull * count * 1000000ull / g_baud);
	if (!bt_se_wait(SE_GENI_M_IRQ_STATUS, GENI_IRQ_CMD_DONE, true,
	    (uint32_t)(20000ull + extra), &irq)) {
		printf("vayu-bt: tx timeout: status %#x, m_irq %#x, tx_fifo_status %#x, m_clk_cfg %#x\n",
		    bt_se_rd(SE_GENI_STATUS), irq, bt_se_rd(SE_GENI_TX_FIFO_STATUS),
		    bt_se_rd(GENI_SER_M_CLK_CFG));
		if (quiet) {
			bt_wr(se_va, SE_GENI_M_IRQ_CLEAR, irq);
		} else {
			bt_se_wr(SE_GENI_M_IRQ_CLEAR, irq, "clear the M IRQs that were set");
		}
		(void)bt_cancel_m();
		return false;
	}
	if (quiet) {
		bt_wr(se_va, SE_GENI_M_IRQ_CLEAR, irq);
	} else {
		bt_se_wr(SE_GENI_M_IRQ_CLEAR, irq, "clear the M IRQs that were set");
	}
	return true;
}

static bool
bt_tx(const uint8_t *bytes, uint32_t count)
{
	return bt_tx_words(bytes, count, false);
}

/* The firmware's path: no tx line, no per-write lines. */
static bool
bt_tx_quiet(const uint8_t *bytes, uint32_t count)
{
	return bt_tx_words(bytes, count, true);
}

/* ---- H4: find one complete event in what the controller has said --------------------------------- */

struct bt_h4 {
	uint8_t data[512];
	uint32_t got;
	uint32_t off;           /* where the event starts in data */
	uint32_t len;           /* 3 + the length byte */
	bool found;
};

/*
 * The first complete H4 event ("04 code len payload..."), skipping leading 0xfd/0xfc/0xfe (the
 * in-band sleep bytes) and any other byte. A 0x04 without its whole event means "need more".
 */
static bool
bt_h4_find(const uint8_t *bytes, uint32_t count, uint32_t *off, uint32_t *len)
{
	for (uint32_t i = 0; i < count; i++) {
		uint32_t total;

		if (bytes[i] != 0x04) {
			continue;
		}
		if (i + 3 > count) {
			return false;
		}
		total = 3u + bytes[i + 2];
		if (i + total > count) {
			return false;
		}
		*off = i;
		*len = total;
		return true;
	}
	return false;
}

static uint16_t
bt_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
bt_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t
bt_le24(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/*
 * Collect until one complete H4 event that matches is there or timeout_ms passes. The caller has
 * already sent the command. want_event: 0 for any event, else the event code (0xff for the
 * download's vendor ack). want_opcode as in bt_h4_xfer.
 */
static void
bt_h4_collect(uint8_t want_event, uint16_t want_opcode, uint32_t timeout_ms, struct bt_h4 *h4)
{
	uint64_t deadline;
	uint32_t from = 0;      /* events before this offset were stepped over already */

	h4->got = 0;
	h4->off = 0;
	h4->len = 0;
	h4->found = false;
	deadline = bt_now_us() + (uint64_t)timeout_ms * 1000;
	for (;;) {
		/*
		 * An event that is not the one wanted (a vendor event, another opcode's answer) is
		 * logged and stepped over; the search goes on after it.
		 */
		while (bt_h4_find(h4->data + from, h4->got - from, &h4->off, &h4->len)) {
			const uint8_t *event = h4->data + from + h4->off;

			h4->off += from;
			if ((want_event == 0 || event[1] == want_event) &&
			    (want_opcode == 0 ||
			    (event[1] == 0x0e && event[2] >= 3 && bt_le16(event + 4) == want_opcode))) {
				h4->found = true;
				return;
			}
			bt_dump_counted("an event that is not the answer (stepped over)", event, h4->len);
			from = h4->off + h4->len;
		}
		{
			uint64_t now = bt_now_us();
			uint32_t slice;

			if (now >= deadline || h4->got >= sizeof(h4->data)) {
				return;
			}
			slice = (uint32_t)(deadline - now);
			if (slice > 10000) {
				slice = 10000;
			}
			h4->got += bt_rx(h4->data + h4->got, (uint32_t)(sizeof(h4->data) - h4->got), slice);
		}
	}
}

/*
 * Send one HCI command and collect until one complete H4 event is there or timeout_ms passes.
 * False only when the command could not be sent; h4->found says whether an event came. drain:
 * drop and log whatever the step before left in the RX FIFO first (the firmware segments set it
 * off, so the ack of the segment before is never thrown away).
 */
static bool
bt_h4_xfer(const uint8_t *command, uint32_t count, uint16_t want_opcode, uint32_t timeout_ms,
    struct bt_h4 *h4, bool drain)
{
	if (drain) {
		/* Whatever is still in the RX FIFO belongs to the step before: log it and drop it. */
		uint8_t stale[64];
		uint32_t n = bt_rx(stale, sizeof(stale), 2000);

		if (n != 0) {
			bt_dump_counted("rx left over before this command (dropped)", stale, n);
		}
	}
	if (!bt_tx(command, count)) {
		return false;
	}
	bt_h4_collect(0, want_opcode, timeout_ms, h4);
	return true;
}

/* The bytes skipped before the event, when there are any. */
static void
bt_h4_skipped(const char *what, const struct bt_h4 *h4)
{
	if (h4->found && h4->off != 0) {
		bt_dump_counted(what, h4->data, h4->off);
	}
}

/* One Command Complete for opcode: payload[0] = num, [1..2] = opcode, [3] = status. */
static bool
bt_cmd_complete(const struct bt_h4 *h4, uint16_t opcode, const uint8_t **payload, uint32_t *plen)
{
	const uint8_t *event;

	if (!h4->found) {
		return false;
	}
	event = h4->data + h4->off;
	if (event[1] != 0x0e || event[2] < 4) {
		return false;
	}
	if (bt_le16(event + 4) != opcode) {
		return false;
	}
	*payload = event + 3;
	*plen = event[2];
	return true;
}

/* ---- step 1: the read-only look ---------------------------------------------------------------- */

static bool
bt_step1(void)
{
	uint32_t vote, s_ahb, m_ahb, se3, cmd, cfg, dfs;
	bool ahb_on;

	printf("vayu-bt: step 1: a read-only look at QUP wrapper 2 SE3 (the Bluetooth UART) and GPIO 43-46\n");

	gcc_va = ml_io_map(GCC_PHYS, GCC_SIZE);
	if (gcc_va == 0) {
		printf("vayu-bt: cannot map GCC at 0x100000: nothing read\n");
		return false;
	}

	/*
	 * The vote and the two wrapper AHB halt registers first: they alone decide whether an
	 * SE register read can be made at all. Nothing in this block touches the SE.
	 */
	vote = bt_rd(gcc_va, GCC_QUPV3_VOTE);
	s_ahb = bt_rd(gcc_va, GCC_WRAP2_S_AHB_HALT);
	m_ahb = bt_rd(gcc_va, GCC_WRAP2_M_AHB_HALT);
	printf("vayu-bt: gcc vote 0x52014 = %#x: s_ahb %u m_ahb %u se3 %u\n", vote,
	    (vote & GCC_VOTE_S_AHB) ? 1 : 0, (vote & GCC_VOTE_M_AHB) ? 1 : 0,
	    (vote & GCC_VOTE_SE3) ? 1 : 0);

	se3 = bt_rd(gcc_va, GCC_WRAP2_S3_HALT);
	printf("vayu-bt: gcc s_ahb 0x1e008 = %#x (%s), m_ahb 0x1e004 = %#x (%s), se3 0x1e4d4 = %#x (%s)\n",
	    s_ahb, bt_branch_state(s_ahb, true), m_ahb, bt_branch_state(m_ahb, false), se3,
	    bt_branch_state(se3, false));

	cmd = bt_rd(gcc_va, GCC_WRAP2_S3_CMD);
	cfg = bt_rd(gcc_va, GCC_WRAP2_S3_CFG);
	dfs = bt_rd(gcc_va, GCC_WRAP2_S3_DFS);
	printf("vayu-bt: se3 rcg cmd %#x cfg %#x dfs %#x: source %u, divider %u, root %s\n", cmd, cfg, dfs,
	    (cfg & GCC_CFG_SRC_MASK) >> GCC_CFG_SRC_SHFT, cfg & GCC_CFG_DIV_MASK,
	    (cmd & GCC_CMD_ROOT_OFF) ? "off" : "on");
	printf("vayu-bt: se3 dfs word (cmd + 0x14) = %#x: enable %u\n", dfs, (dfs & GCC_DFSR_DFS_EN) ? 1 : 0);

	if ((dfs & GCC_DFSR_DFS_EN) == 0) {
		/*
		 * Run 031412: the loader programs the RCG itself and leaves DFS off, so every
		 * DFSR/M/N word reads 0. The RCG's CFG has a DFSR's layout; its M and N sit at
		 * cmd + 8 and cmd + 0xc. That one rate is level 0; the other levels do not exist.
		 */
		uint32_t m = bt_rd(gcc_va, GCC_WRAP2_S3_RCG_M);
		uint32_t n = bt_rd(gcc_va, GCC_WRAP2_S3_RCG_N);
		bool valid;

		g_dfs_rate[0] = bt_dfs_rate(cfg, m, n, &valid);
		g_dfs_valid[0] = valid;
		for (uint32_t i = 1; i < GCC_DFS_LEVELS; i++) {
			g_dfs_rate[i] = 0;
			g_dfs_valid[i] = false;
		}
		printf("vayu-bt: se3 rcg is not in DFS: its one rate is %llu Hz (cfg %#x m %#x n %#x)\n",
		    (unsigned long long)g_dfs_rate[0], cfg, m, n);
	} else {
		/* The eight DFS levels: one line each, as geni's RCG keeps them. */
		for (uint32_t i = 0; i < GCC_DFS_LEVELS; i++) {
			uint32_t dfsr = bt_rd(gcc_va, GCC_WRAP2_S3_DFSR(i));
			uint32_t m = bt_rd(gcc_va, GCC_WRAP2_S3_M(i));
			uint32_t n = bt_rd(gcc_va, GCC_WRAP2_S3_N(i));
			uint32_t src = (dfsr >> GCC_DFSR_SRC_SHFT) & GCC_DFSR_SRC_MASK;
			uint32_t hid = (dfsr >> GCC_DFSR_HID_SHFT) & GCC_DFSR_HID_MASK;
			uint32_t mode = (dfsr >> GCC_DFSR_MODE_SHFT) & GCC_DFSR_MODE_MASK;
			bool valid;

			g_dfs_rate[i] = bt_dfs_rate(dfsr, m, n, &valid);
			g_dfs_valid[i] = valid;
			printf("vayu-bt: dfs level %u: dfsr %#x m %#x n %#x: src %u hid %u mode %u = %llu Hz%s\n",
			    i, dfsr, m, n, src, hid, mode, (unsigned long long)g_dfs_rate[i],
			    valid ? "" : " (source unknown: level skipped)");
		}
	}

	ahb_on = ((s_ahb & GCC_HALT_BIT) == 0 || (s_ahb & GCC_CBCR_HW_CTL) != 0) &&
		 (m_ahb & GCC_HALT_BIT) == 0;
	if (ahb_on && (s_ahb & GCC_HALT_BIT) != 0) {
		printf("vayu-bt: wrapper 2 s_ahb is hardware-gated (%#x): it counts as running, "
		    "as the loader and vayu_qup_spi.c treat it\n", s_ahb);
	}

	wrapper_va = ml_io_map(QUP_WRAPPER_PHYS, QUP_WRAPPER_SIZE);
	if (wrapper_va == 0) {
		printf("vayu-bt: cannot map QUP wrapper 2 at 0xcc0000: QUPV3_HW_VER not read\n");
	} else if (!ahb_on) {
		printf("vayu-bt: qup wrapper 2 QUPV3_HW_VER 0xcc0004: not read while the wrapper AHB clock is off\n");
	} else {
		printf("vayu-bt: qup wrapper 2 QUPV3_HW_VER 0xcc0004 = %#x\n",
		    bt_rd(wrapper_va, QUPV3_HW_VER));
	}

	if (!ahb_on) {
		printf("vayu-bt: wrapper 2 AHB clock off (s_ahb %#x, m_ahb %#x): the SE is not read\n",
		    s_ahb, m_ahb);
	} else if (!bt_se_ready()) {
		printf("vayu-bt: cannot map the SE at 0xc8c000: it is not read\n");
	} else {
		uint32_t fw = bt_se_rd(GENI_FW_REVISION_RO);
		uint32_t status = bt_se_rd(SE_GENI_STATUS);
		uint32_t clk_sel = bt_se_rd(SE_GENI_CLK_SEL);
		uint32_t protocol = (fw & FW_REV_PROTOCOL_MSK) >> FW_REV_PROTOCOL_SHFT;
		uint32_t version = fw & FW_REV_VERSION_MSK;
		uint32_t mclk = bt_se_rd(GENI_SER_M_CLK_CFG);
		uint32_t sclk = bt_se_rd(GENI_SER_S_CLK_CFG);
		uint32_t if_dis = bt_se_rd(GENI_IF_DISABLE_RO);
		uint32_t dma = bt_se_rd(SE_GENI_DMA_MODE_EN);
		uint32_t mcmd = bt_se_rd(SE_GENI_M_CMD0);
		uint32_t mirq = bt_se_rd(SE_GENI_M_IRQ_STATUS);
		uint32_t men = bt_se_rd(SE_GENI_M_IRQ_EN);
		uint32_t scmd = bt_se_rd(SE_GENI_S_CMD0);
		uint32_t sirq = bt_se_rd(SE_GENI_S_IRQ_STATUS);
		uint32_t sen = bt_se_rd(SE_GENI_S_IRQ_EN);
		uint32_t txf = bt_se_rd(SE_GENI_TX_FIFO_STATUS);
		uint32_t rxf = bt_se_rd(SE_GENI_RX_FIFO_STATUS);
		uint32_t irqen = bt_se_rd(SE_IRQ_EN);
		uint32_t hw0 = bt_se_rd(SE_HW_PARAM_0);
		uint32_t hw1 = bt_se_rd(SE_HW_PARAM_1);

		g_tx_depth = (hw0 >> FIFO_DEPTH_SHFT) & FIFO_DEPTH_MASK;
		g_tx_width = (hw0 >> TX_FIFO_WIDTH_SHFT) & TX_FIFO_WIDTH_MASK;
		g_rx_depth = (hw1 >> FIFO_DEPTH_SHFT) & FIFO_DEPTH_MASK;

		printf("vayu-bt: se3 at 0xc8c000: fw revision %#x (protocol %u = %s, version %#x), "
		    "status %#x, clk_sel %#x\n", fw, protocol, bt_protocol_name(protocol), version, status,
		    clk_sel);
		printf("vayu-bt: the SE is loaded as UART: %s\n", protocol == SE_PROTO_UART ? "YES" : "NO");
		printf("vayu-bt: se3 status 0x040 = %#x: m command active %u, s command active %u\n",
		    status, (status & SE_STATUS_M_ACTIVE) ? 1 : 0, (status & SE_STATUS_S_ACTIVE) ? 1 : 0);
		printf("vayu-bt: se3 ser_m_clk_cfg 0x048 = %#x (enable %u, divider %u), "
		    "ser_s_clk_cfg 0x04c = %#x (enable %u, divider %u)\n", mclk, (mclk & SER_CLK_EN) ? 1 : 0,
		    (mclk >> SER_CLK_DIV_SHFT) & SER_CLK_DIV_MASK, sclk, (sclk & SER_CLK_EN) ? 1 : 0,
		    (sclk >> SER_CLK_DIV_SHFT) & SER_CLK_DIV_MASK);
		printf("vayu-bt: se3 if_disable_ro 0x064 = %#x: FIFO %s\n", if_dis,
		    (if_dis & IF_DISABLE_FIFO) ? "DISABLED" : "allowed");
		printf("vayu-bt: se3 fw_revision 0x068 = %#x (protocol %u = %s, version %#x), "
		    "clk_sel 0x07c = %#x (dfs level %u)\n", fw, protocol, bt_protocol_name(protocol),
		    version, clk_sel, clk_sel & 0x7u);
		printf("vayu-bt: se3 dma_mode_en 0x258 = %#x: %s\n", dma,
		    (dma & 1u) ? "DMA mode" : "FIFO mode");
		printf("vayu-bt: se3 m_cmd0 0x600 = %#x, m_irq_status 0x610 = %#x, m_irq_en 0x614 = %#x\n",
		    mcmd, mirq, men);
		printf("vayu-bt: se3 s_cmd0 0x630 = %#x, s_irq_status 0x640 = %#x, s_irq_en 0x644 = %#x\n",
		    scmd, sirq, sen);
		printf("vayu-bt: se3 tx_fifo_status 0x800 = %#x (%u words), rx_fifo_status 0x804 = %#x "
		    "(%u words, last %u, last byte valid %u)\n", txf, txf & TX_FIFO_WC_MASK, rxf,
		    rxf & RX_FIFO_WC_MASK, (rxf & RX_FIFO_LAST) ? 1 : 0,
		    (rxf >> RX_FIFO_LBV_SHFT) & RX_FIFO_LBV_MASK);
		printf("vayu-bt: se3 irq_en 0xe1c = %#x\n", irqen);
		printf("vayu-bt: se3 hw_param_0 0xe24 = %#x (tx depth %u words, width %u bits), "
		    "hw_param_1 0xe28 = %#x (rx depth %u words)\n", hw0,
		    (hw0 >> FIFO_DEPTH_SHFT) & FIFO_DEPTH_MASK,
		    (hw0 >> TX_FIFO_WIDTH_SHFT) & TX_FIFO_WIDTH_MASK, hw1,
		    (hw1 >> FIFO_DEPTH_SHFT) & FIFO_DEPTH_MASK);
	}

	(void)bt_pins();
	(void)bt_pmic_survey();

	printf("vayu-bt: step 1 done, nothing written\n");
	return true;
}

/* ---- step 2: the clock, the GENI UART at 2400 8N1 and RX ---------------------------------------- */

static bool
bt_step2(void)
{
	uint32_t halt, fw, protocol, if_dis;

	printf("vayu-bt: step 2: the SE3 clock, the GENI UART at 2400 8N1, RX running\n");
	if (gcc_va == 0) {
		printf("vayu-bt: step 2: step 1 did not map GCC: stopping\n");
		return false;
	}
	halt = bt_rd(gcc_va, GCC_WRAP2_S3_HALT);
	if ((halt & GCC_HALT_BIT) != 0) {
		printf("vayu-bt: step 2: the SE3 clock is off: run with the loader's vayu_bt_loader=2\n");
		return false;
	}
	printf("vayu-bt: step 2: the SE3 clock is on (0x1e4d4 = %#x)\n", halt);

	/* The same AHB condition as step 1 (hardware-gated s_ahb counts as running). */
	if (!bt_ahb_on()) {
		printf("vayu-bt: step 2: the wrapper 2 AHB clock is off (s_ahb %#x, m_ahb %#x): "
		    "run with the loader's vayu_bt_loader=2\n",
		    bt_rd(gcc_va, GCC_WRAP2_S_AHB_HALT), bt_rd(gcc_va, GCC_WRAP2_M_AHB_HALT));
		return false;
	}
	if (!bt_se_ready()) {
		return false;
	}
	fw = bt_se_rd(GENI_FW_REVISION_RO);
	protocol = (fw & FW_REV_PROTOCOL_MSK) >> FW_REV_PROTOCOL_SHFT;
	if (protocol != SE_PROTO_UART) {
		printf("vayu-bt: step 2: the SE's firmware is protocol %u (%s), not UART: stopping\n",
		    protocol, bt_protocol_name(protocol));
		return false;
	}
	if_dis = bt_se_rd(GENI_IF_DISABLE_RO);
	if ((if_dis & IF_DISABLE_FIFO) != 0) {
		printf("vayu-bt: FIFO interface disabled by firmware: this SE needs GSI/DMA; stopping\n");
		return false;
	}
	if (!bt_read_fifo_params()) {
		return false;
	}
	if (!bt_cancel_active()) {
		printf("vayu-bt: step 2: a command would not cancel: stopping\n");
		return false;
	}

	/* vendor geni_se_init() + geni_se_select_fifo_mode() */
	bt_se_update(GENI_CGC_CTRL, GENI_CGC_DEFAULT, true, "GENI_CGC_CTRL |= 0x7f");
	bt_se_update(SE_DMA_GENERAL_CFG, DMA_CGC_DEFAULT, true, "SE_DMA_GENERAL_CFG |= 0xf");
	bt_se_wr(GENI_OUTPUT_CTRL, GENI_OUTPUT_DEFAULT, "GENI_OUTPUT_CTRL = 0x7f");
	bt_se_wr(GENI_FORCE_DEFAULT_REG, 1u, "GENI_FORCE_DEFAULT_REG = 1");
	bt_se_update(SE_IRQ_EN, SE_IRQ_BITS, true, "SE_IRQ_EN |= 0xf");
	bt_se_update(SE_GENI_DMA_MODE_EN, 1u, false, "SE_GENI_DMA_MODE_EN &= ~1 (FIFO)");
	bt_se_wr(SE_GSI_EVENT_EN, 0, "SE_GSI_EVENT_EN = 0");
	bt_se_wr(SE_GENI_RX_WATERMARK, g_rx_depth / 2, "RX watermark");
	bt_se_wr(SE_GENI_RX_RFR_WATERMARK, g_rx_depth - 2, "RX RFR watermark");
	bt_se_wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu, "clear M IRQs");
	bt_se_wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu, "clear S IRQs");
	bt_se_wr(SE_DMA_TX_IRQ_CLR, 0xffffffffu, "clear DMA TX IRQs");
	bt_se_wr(SE_DMA_RX_IRQ_CLR, 0xffffffffu, "clear DMA RX IRQs");

	/* vendor msm_geni_serial.c: four 8-bit chars per word, LSB first */
	bt_se_wr(SE_UART_TX_PACKING_CFG0, 0x0004380eu, "TX packing cfg0");
	bt_se_wr(SE_UART_TX_PACKING_CFG1, 0x000c3e0eu, "TX packing cfg1");
	bt_se_wr(SE_UART_RX_PACKING_CFG0, 0x0004380eu, "RX packing cfg0");
	bt_se_wr(SE_UART_RX_PACKING_CFG1, 0x000c3e0eu, "RX packing cfg1");
	bt_se_wr(SE_UART_RX_STALE_CNT, UART_STALE_CNT, "RX stale count 10 * 16");
	bt_se_wr(SE_UART_MANUAL_RFR, 0x80000000u, "manual RFR, ready");
	delay(10);
	bt_se_wr(SE_UART_MANUAL_RFR, 0x80000002u, "manual RFR, not ready during setup");
	bt_se_wr(SE_UART_LOOPBACK_CFG, 0, "no loopback");
	bt_se_wr(SE_UART_TX_TRANS_CFG, g_cts ? 0u : TX_TRANS_CTS_MASK,
	    g_cts ? "honour CTS" : "ignore CTS");
	bt_se_wr(SE_UART_RX_TRANS_CFG, 0, "RX trans cfg");
	bt_se_wr(SE_UART_TX_PARITY_CFG, 0, "no TX parity");
	bt_se_wr(SE_UART_RX_PARITY_CFG, 0, "no RX parity");
	bt_se_wr(SE_UART_TX_WORD_LEN, UART_WORD_LEN, "8-bit TX words");
	bt_se_wr(SE_UART_RX_WORD_LEN, UART_WORD_LEN, "8-bit RX words");
	bt_se_wr(SE_UART_TX_STOP_BIT_LEN, 0, "1 stop bit");

	if (!bt_set_baud(2400)) {
		return false;
	}
	bt_se_wr(SE_UART_MANUAL_RFR, 0, "hardware RFR");
	printf("vayu-bt: step 2: read back 0x048 %#x, 0x07c %#x, 0x25c %#x, 0x260 %#x, 0x294 %#x\n",
	    bt_se_rd(GENI_SER_M_CLK_CFG), bt_se_rd(SE_GENI_CLK_SEL), bt_se_rd(SE_UART_TX_TRANS_CFG),
	    bt_se_rd(SE_UART_TX_PACKING_CFG0), bt_se_rd(SE_UART_RX_STALE_CNT));
	(void)bt_start_rx();
	printf("vayu-bt: step 2 done: GENI UART at 2400 8N1, RX running\n");
	return true;
}

/* ---- step 3: the pins, read-only ---------------------------------------------------------------- */

static bool
bt_step3(void)
{
	printf("vayu-bt: step 3: the four Bluetooth pins (read only)\n");
	if (!bt_pins()) {
		static const char *const names[] = { "CTS", "RTS", "TX", "RX" };

		if (tlmm_va != 0) {
			for (uint32_t i = 0; i < 4; i++) {
				uint32_t cfg = bt_rd(tlmm_va, TLMM_PIN_STRIDE * i);
				uint32_t function = (cfg >> TLMM_CFG_FUNC_SHFT) & TLMM_CFG_FUNC_MASK;

				if (function != TLMM_CFG_FUNC_QUP13) {
					printf("vayu-bt: step 3: gpio%u (%s) is function %u, not 1 (qup13): "
					    "run with the loader's vayu_bt_loader=2\n", 43 + i, names[i],
					    function);
				}
			}
		}
		printf("vayu-bt: step 3: the pins are not right: stopping\n");
		return false;
	}
	printf("vayu-bt: step 3: all four pins are function 1 (qup13): OK\n");
	bt_report_cts("step 3: CTS (gpio43, io bit 0)");
	printf("vayu-bt: step 3 done: the pins are muxed to qup13\n");
	return true;
}

/* ---- step 4: the rails ------------------------------------------------------------------------- */

static bool
bt_step4(void)
{
	uint32_t voted = 0;
	bool readout_ok;

	printf("vayu-bt: step 4: the rails (a PMIC readout, then RPMh votes for the ones that are off "
	    "or unknown)\n");
	readout_ok = bt_pmic_survey();
	if (!readout_ok) {
		printf("vayu-bt: step 4: the PMIC readout did not answer for every rail: the ones it "
		    "did not answer for are unknown\n");
	}
	for (unsigned i = 0; i < BT_RAIL_COUNT; i++) {
		struct bt_rail *r = &bt_rails[i];

		if (!r->known) {
			if (r->id == NULL) {
				printf("vayu-bt: step 4: %s: state unknown (not readable from the apps "
				    "arbiter): not ours to vote, going on\n", r->name);
			} else if (g_rails) {
				printf("vayu-bt: step 4: %s: state unknown (not readable from the apps "
				    "arbiter): voting\n", r->name);
			} else {
				printf("vayu-bt: step 4: %s: state unknown (not readable from the apps "
				    "arbiter) and vayu_bt_rails=0: not voted, going on\n", r->name);
			}
			continue;
		}
		if (r->on) {
			continue;
		}
		if (r->id == NULL) {
			/* S4A: read only, no RPMh name of ours; it never stops the step. */
			printf("vayu-bt: step 4: %s reads off; not ours to vote, going on\n", r->name);
			continue;
		}
		if (!g_rails) {
			printf("vayu-bt: step 4: %s is off and vayu_bt_rails=0: stopping\n", r->name);
			return false;
		}
	}

	for (unsigned i = 0; i < BT_RAIL_COUNT; i++) {
		struct bt_rail *r = &bt_rails[i];
		uint32_t commands[3], values[3], addr;

		if ((r->known && r->on) || r->id == NULL) {
			continue;       /* a rail that is already on is never voted; S4A is only read */
		}
		addr = vayu_rpmh_address(r->id);
		if (addr == 0) {
			printf("vayu-bt: step 4: no cmd-db address for %s: stopping\n", r->id);
			return false;
		}
		r->addr = addr;
		commands[0] = addr;
		commands[1] = addr + 8;
		commands[2] = addr + 4;
		values[0] = r->mv;
		values[1] = 7;          /* PMIC5 LDO high power mode */
		values[2] = 1;          /* enable */
		printf("vayu-bt: step 4: vote %s (%s) on: %u mV, mode 7, enable 1 at %#x\n", r->id,
		    r->name, r->mv, addr);
		if (!vayu_rpmh_send(commands, values, 3, "vayu-bt")) {
			printf("vayu-bt: step 4: the vote for %s did not report done: stopping\n", r->id);
			return false;
		}
		r->voted_on = true;
		voted++;
	}

	if (voted != 0 && readout_ok) {
		bt_sleep_ms(5);
		printf("vayu-bt: step 4: the PMIC readout after the votes\n");
		if (!bt_pmic_survey()) {
			printf("vayu-bt: step 4: the PMIC readout after the votes did not answer\n");
		}
		for (unsigned i = 0; i < BT_RAIL_COUNT; i++) {
			if (bt_rails[i].voted_on && bt_rails[i].known && !bt_rails[i].on) {
				printf("vayu-bt: step 4: %s still reads off after the vote "
				    "(status1 %#x, en_ctl %#x)\n", bt_rails[i].name,
				    (unsigned)bt_rails[i].status1, (unsigned)bt_rails[i].en_ctl);
			}
		}
	} else if (voted != 0) {
		printf("vayu-bt: step 4: the readout after the votes is skipped: not readable from "
		    "the apps arbiter\n");
	}
	bt_sleep_ms(20);
	printf("vayu-bt: step 4 done: %u rail(s) turned on by us\n", voted);
	return true;
}

/* ---- step 5: the QCA power pulses --------------------------------------------------------------- */

static bool
bt_step5(void)
{
	static const uint8_t first = 0xc0, second = 0xfc;
	uint8_t rx[256];
	uint32_t got;
	bool sleep_byte = false;

	printf("vayu-bt: step 5: the power pulses (0xc0 at 2400 baud, then 0xfc at 115200)\n");
	bt_report_cts("step 5: CTS before the pulses");
	if (!bt_tx(&first, 1)) {
		printf("vayu-bt: step 5: the 0xc0 pulse was not sent: stopping\n");
		return false;
	}
	bt_sleep_ms(10);
	if (!bt_set_baud(115200)) {
		return false;
	}
	if (!bt_tx(&second, 1)) {
		printf("vayu-bt: step 5: the 0xfc pulse was not sent: stopping\n");
		return false;
	}
	bt_sleep_ms(100);
	got = bt_rx(rx, sizeof(rx), 50000);
	bt_dump_counted("rx after the pulses", rx, got);
	for (uint32_t i = 0; i < got; i++) {
		if (rx[i] == 0xfd || rx[i] == 0xfc || rx[i] == 0xfe) {
			sleep_byte = true;
		}
	}
	if (sleep_byte) {
		printf("vayu-bt: rx after the pulses: in-band sleep byte(s) seen (0xfd/0xfc/0xfe)\n");
	}
	bt_report_cts("step 5: CTS after the pulses");
	printf("vayu-bt: step 5 done: the pulses are out\n");
	return true;
}

/* ---- step 6: the SoC version ------------------------------------------------------------------- */

static bool
bt_step6(void)
{
	static const uint8_t command[5] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
	struct bt_h4 h4;
	const uint8_t *event, *payload;
	uint8_t code, len;

	printf("vayu-bt: step 6: the SoC version request\n");
	if (!bt_h4_xfer(command, sizeof(command), 0, 500, &h4, true)) {
		printf("vayu-bt: step 6: the request was not sent: stopping\n");
		return false;
	}
	bt_dump_counted("step 6: rx", h4.data, h4.got);
	bt_h4_skipped("step 6: skipped before the event", &h4);
	if (!h4.found) {
		printf("vayu-bt: step 6: no answer to the version request\n");
		return false;
	}
	event = h4.data + h4.off;
	code = event[1];
	len = event[2];
	payload = event + 3;

	if (code == 0xff && len >= 14 && payload[0] == 0x00 && payload[1] == 0x02) {
		uint32_t product_id = bt_le32(payload + 2);
		uint16_t patch_ver = bt_le16(payload + 6);
		uint16_t rom_ver = bt_le16(payload + 8);
		uint32_t soc_id = bt_le32(payload + 10);
		uint32_t soc_ver = (soc_id << 16) | rom_ver;
		uint32_t fw_rom;

		/*
		 * The file names follow the soc id, not upstream btqca's per-compatible rom formula: run
		 * 044646 answered soc id 0x40010224, rom 0x1001, for which the WCN3990 formula gives 0x01
		 * (no such file anywhere) and the WCN3998 one 0x21. INFERENCE (vendor libbt-vendor,
		 * Cherokee): 0x40010224 = 2.1 -> crbtfw21/crnv21, 0x40010214 = 2.0 -> crbtfw20/crnv20;
		 * both files sets are on the phone's bluetooth partition. Anything else: the WCN3990
		 * formula, printed as a guess.
		 */
		if (soc_id == 0x40010224u) {
			fw_rom = 0x21;
		} else if (soc_id == 0x40010214u) {
			fw_rom = 0x20;
		} else {
			fw_rom = ((soc_ver & 0xf00u) >> 4) | (soc_ver & 0xfu);
			printf("vayu-bt: step 6: soc id %#x is not in the table: fw_rom %#x is a guess\n", soc_id, fw_rom);
		}
		g_fw_rom = fw_rom;
		printf("vayu-bt: step 6: product id %#x, patch version %#x, rom version %#x, soc id %#x\n",
		    product_id, (unsigned)patch_ver, (unsigned)rom_ver, soc_id);
		printf("vayu-bt: step 6: soc version %#x, fw_rom %#x\n", soc_ver, fw_rom);
		printf("vayu-bt: step 6: firmware names crbtfw%02x.tlv / crnv%02x.bin\n",
		    fw_rom, fw_rom);
	} else if (code == 0x0e && len >= 4) {
		printf("vayu-bt: step 6: Command Complete opcode %#06x, status %#x\n",
		    (unsigned)bt_le16(payload + 1), (unsigned)payload[3]);
		if (len > 4) {
			bt_dump_counted("step 6: rest of the answer", payload + 4, len - 4);
		}
	} else {
		printf("vayu-bt: step 6: event %#x with %u payload bytes is not a version answer; raw above\n",
		    (unsigned)code, (unsigned)len);
	}
	printf("vayu-bt: step 6 done: the version request was answered\n");
	return true;
}

/* ---- step 7: HCI ------------------------------------------------------------------------------- */

static bool
bt_step7(void)
{
	static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };   /* HCI_Reset */
	static const uint8_t version[4] = { 0x01, 0x01, 0x10, 0x00 }; /* Read_Local_Version */
	static const uint8_t bdaddr[4] = { 0x01, 0x09, 0x10, 0x00 };  /* Read_BD_ADDR */
	struct bt_h4 h4;
	const uint8_t *payload;
	uint32_t plen;
	uint8_t status;

	printf("vayu-bt: step 7: HCI\n");

	if (!bt_h4_xfer(reset, sizeof(reset), 0x0c03, 500, &h4, true)) {
		printf("vayu-bt: step 7: HCI_Reset was not sent: stopping\n");
		return false;
	}
	bt_h4_skipped("step 7: skipped before HCI_Reset", &h4);
	if (!bt_cmd_complete(&h4, 0x0c03, &payload, &plen)) {
		bt_dump_counted("step 7: the answer to HCI_Reset", h4.data, h4.got);
		printf("vayu-bt: step 7: no Command Complete for HCI_Reset: stopping\n");
		return false;
	}
	status = payload[3];
	printf("vayu-bt: HCI_Reset: Command Complete, status %#x\n", (unsigned)status);
	if (status != 0) {
		printf("vayu-bt: step 7: HCI_Reset was refused: stopping\n");
		return false;
	}

	if (!bt_h4_xfer(version, sizeof(version), 0x1001, 500, &h4, true)) {
		printf("vayu-bt: step 7: Read_Local_Version was not sent: stopping\n");
		return false;
	}
	bt_h4_skipped("step 7: skipped before Read_Local_Version", &h4);
	if (!bt_cmd_complete(&h4, 0x1001, &payload, &plen)) {
		bt_dump_counted("step 7: the answer to Read_Local_Version", h4.data, h4.got);
		printf("vayu-bt: step 7: no Command Complete for Read_Local_Version: stopping\n");
		return false;
	}
	if (plen < 12) {
		bt_dump_counted("step 7: the short answer to Read_Local_Version", h4.data, h4.got);
		printf("vayu-bt: step 7: Read_Local_Version answered only %u payload bytes: stopping\n",
		    plen);
		return false;
	}
	{
		uint16_t manufacturer = bt_le16(payload + 8);

		printf("vayu-bt: Read_Local_Version: status %#x, hci_ver %#x, hci_rev %#x, lmp_ver %#x, "
		    "manufacturer %u%s, lmp_subver %#x\n", (unsigned)payload[3], (unsigned)payload[4],
		    (unsigned)bt_le16(payload + 5), (unsigned)payload[7], (unsigned)manufacturer,
		    manufacturer == 29 ? " (Qualcomm)" : "", (unsigned)bt_le16(payload + 10));
	}

	if (!bt_h4_xfer(bdaddr, sizeof(bdaddr), 0x1009, 500, &h4, true)) {
		printf("vayu-bt: step 7: Read_BD_ADDR was not sent: stopping\n");
		return false;
	}
	bt_h4_skipped("step 7: skipped before Read_BD_ADDR", &h4);
	if (!bt_cmd_complete(&h4, 0x1009, &payload, &plen)) {
		bt_dump_counted("step 7: the answer to Read_BD_ADDR", h4.data, h4.got);
		printf("vayu-bt: step 7: no Command Complete for Read_BD_ADDR: stopping\n");
		return false;
	}
	if (plen < 10) {
		bt_dump_counted("step 7: the short answer to Read_BD_ADDR", h4.data, h4.got);
		printf("vayu-bt: step 7: Read_BD_ADDR answered only %u payload bytes: stopping\n", plen);
		return false;
	}
	if (payload[3] != 0) {
		printf("vayu-bt: Read_BD_ADDR: status %#x: stopping\n", (unsigned)payload[3]);
		return false;
	}
	printf("vayu-bt: Read_BD_ADDR: %02x:%02x:%02x:%02x:%02x:%02x\n", (unsigned)payload[9],
	    (unsigned)payload[8], (unsigned)payload[7], (unsigned)payload[6],
	    (unsigned)payload[5], (unsigned)payload[4]);

	printf("vayu-bt: step 7 done: the controller answers HCI\n");
	return true;
}

/* ---- step 8: the QCA firmware download (upstream btqca.c) --------------------------------------- */

#define QCA_HCI_CC_OPCODE       0xfc00          /* QCA_HCI_CC_OPCODE */
#define QCA_TLV_PATCH_TYPE      0x1eu           /* QCA_TLV_PATCH_TYPE, the byte after the length */
#define QCA_SEGMENT_MAX         243             /* qca_tlv_send_segment's payload */
#define QCA_ACK_TIMEOUT_MS      1000u
#define QCA_TLV_TYPE_PATCH      1
#define QCA_TLV_TYPE_SINGLE     2
#define QCA_TLV_TYPE_MULTI      4
#define QCA_DNLD_MODE_NONE      0               /* qca_tlv_dnld_mode */
#define QCA_DNLD_MODE_VSE       1
#define QCA_DNLD_MODE_CC        2
#define QCA_DNLD_MODE_VSE_CC    3
#define QCA_TAG_ID_BD_ADDR      2
#define QCA_TAG_ID_HCI          17
#define QCA_TAG_ID_DEEP_SLEEP   27
#define QCA_TAG_HEADER          12              /* id le16, len le16, 8 reserved bytes */
#define QCA_BAUDRATE_115200     0x00
#define QCA_TAG_HCI_IBS         0x80            /* tag 17 data[0] bit 7: in-band sleep on */
#define QCA_TAG_DEEP_SLEEP_ON   0x01            /* tag 27 data[0] bit 0: deep sleep on */

/* The one extra line a TX timeout needs when CTS was honoured for the download. */
static void
bt_step8_tx_hint(void)
{
	if (g_fw_cts) {
		printf("vayu-bt: step 8: CTS honoured: retry with vayu_bt_fw_cts=0\n");
	}
}

/*
 * Wait for the answer to one firmware segment: one complete vendor event, 04 ff 03 00 04 00.
 * Events of another code are stepped over (and printed by the collector). False when no 0xff
 * event came or when cresp is not 0 or rtype is not 4 (the event is printed first); result != 0
 * is printed and the download goes on.
 */
static bool
bt_qca_ack(const char *what)
{
	struct bt_h4 h4;
	const uint8_t *event;

	bt_h4_collect(0xff, 0, QCA_ACK_TIMEOUT_MS, &h4);
	if (!h4.found) {
		printf("vayu-bt: step 8: %s: no vendor event 0xff in %u ms\n", what, QCA_ACK_TIMEOUT_MS);
		return false;
	}
	event = h4.data + h4.off;
	if (event[2] < 3) {
		bt_dump_counted("step 8: the short vendor event", event, h4.len);
		printf("vayu-bt: step 8: %s: the vendor event carries %u payload bytes, not 3: "
		    "stopping\n", what, (unsigned)event[2]);
		return false;
	}
	if (event[3] != 0x00 || event[4] != 0x04) {
		bt_dump_counted("step 8: the refused download ack", event, h4.len);
		printf("vayu-bt: step 8: %s: cresp %#x rtype %#x is not 00 04: stopping\n", what,
		    (unsigned)event[3], (unsigned)event[4]);
		return false;
	}
	if (event[5] != 0x00) {
		printf("vayu-bt: step 8: %s: the controller answered result %#x, going on\n", what,
		    (unsigned)event[5]);
	}
	return true;
}

/*
 * Whatever the controller said between two segments that need no ack. A complete event is
 * printed, so an error does not go unnoticed; a vendor event whose cresp is not 0 stops the
 * download. The RX FIFO is not drained-and-dropped before a segment: no ack may be lost.
 */
static bool
bt_qca_rx_between(void)
{
	uint8_t rx[256];
	uint32_t got, at = 0;

	got = bt_rx(rx, sizeof(rx), 0);
	while (at < got) {
		uint32_t off, len;
		const uint8_t *event;

		if (!bt_h4_find(rx + at, got - at, &off, &len)) {
			break;
		}
		event = rx + at + off;
		if (off != 0) {
			bt_dump_counted("rx between segments, before the event", rx + at, off);
		}
		bt_dump_counted("rx between segments", event, len);
		if (event[1] == 0xff && event[2] >= 3 && event[3] != 0x00) {
			printf("vayu-bt: step 8: between segments the controller answered cresp %#x: "
			    "stopping\n", (unsigned)event[3]);
			return false;
		}
		at += off + len;
	}
	if (at < got) {
		bt_dump_counted("rx between segments, no complete event", rx + at, got - at);
	}
	return true;
}

/*
 * One file in H4 segments of at most 243 bytes: 01 00 fc (seg + 2) 1e seg, then the bytes.
 * ack_each: wait for the ack after every segment (NONE, CC); ack_last: only after the last
 * (VSE, VSE_CC). progress: one line every that many segments (0: none).
 */
static bool
bt_qca_send(const char *what, const uint8_t *data, uint64_t size, bool ack_each, bool ack_last,
    uint32_t progress)
{
	uint8_t command[6 + QCA_SEGMENT_MAX];
	uint64_t off = 0, start = bt_now_us();
	uint32_t total = (uint32_t)((size + QCA_SEGMENT_MAX - 1) / QCA_SEGMENT_MAX);
	uint32_t sent = 0;

	while (off < size) {
		uint32_t seg = size - off > QCA_SEGMENT_MAX ? QCA_SEGMENT_MAX : (uint32_t)(size - off);
		bool last;

		command[0] = 0x01;
		command[1] = (uint8_t)(QCA_HCI_CC_OPCODE & 0xff);
		command[2] = (uint8_t)(QCA_HCI_CC_OPCODE >> 8);
		command[3] = (uint8_t)(seg + 2);
		command[4] = (uint8_t)QCA_TLV_PATCH_TYPE;
		command[5] = (uint8_t)seg;
		memcpy(command + 6, data + off, seg);
		if (!bt_tx_quiet(command, 6 + seg)) {
			printf("vayu-bt: step 8: %s segment %u/%u was not sent\n", what, sent + 1, total);
			bt_step8_tx_hint();
			return false;
		}
		off += seg;
		sent++;
		last = off >= size;
		if (ack_each || (ack_last && last)) {
			if (!bt_qca_ack(what)) {
				return false;
			}
		} else if (!bt_qca_rx_between()) {
			return false;
		}
		if (progress != 0 && sent % progress == 0) {
			printf("vayu-bt: %s %u/%u segments, %llu bytes, %llu ms\n", what, sent, total,
			    (unsigned long long)off, (unsigned long long)((bt_now_us() - start) / 1000));
		}
	}
	printf("vayu-bt: step 8: %s sent %u segments, %llu bytes in %llu ms\n", what, sent,
	    (unsigned long long)size, (unsigned long long)((bt_now_us() - start) / 1000));
	return true;
}

/*
 * The NVM's tags (btqca.c qca_tlv_check_data): a multi TLV (type 4) around a type 2 set, or a
 * type 2 file itself. Each tag is id le16, len le16, 8 reserved bytes and len data bytes; the
 * walk stops at the inner set's end. Patch tag 17 (HCI: stay at 115200) and tag 27 (deep
 * sleep): with vayu_bt_ibs=1 both bits are set as upstream does, with 0 (the default) both are
 * cleared, so the controller keeps in-band sleep and deep sleep off; print tag 2 (the BD
 * address) as it is.
 */
static bool
bt_nvm_patch(uint8_t *data, uint64_t size)
{
	uint32_t at, end, tags = 0;
	bool hci = false, deep = false;

	if (size < 8) {
		printf("vayu-bt: step 8: the NVM is only %llu bytes: stopping\n",
		    (unsigned long long)size);
		return false;
	}
	switch (data[0]) {
	case QCA_TLV_TYPE_MULTI:
		if (data[4] != QCA_TLV_TYPE_SINGLE) {
			printf("vayu-bt: step 8: the NVM's inner TLV is type %u, not 2: stopping\n",
			    (unsigned)data[4]);
			return false;
		}
		at = 8;
		end = at + bt_le24(data + 5);
		break;
	case QCA_TLV_TYPE_SINGLE:
		at = 4;
		end = at + bt_le24(data + 1);
		break;
	default:
		printf("vayu-bt: step 8: the NVM's outer TLV is type %u, not 4 or 2: stopping\n",
		    (unsigned)data[0]);
		return false;
	}
	if (end > size) {
		printf("vayu-bt: step 8: the NVM's tag set ends at %u, past the file's %llu bytes: "
		    "stopping\n", end, (unsigned long long)size);
		return false;
	}
	while (at + QCA_TAG_HEADER <= end) {
		uint16_t id = bt_le16(data + at);
		uint16_t len = bt_le16(data + at + 2);
		uint8_t *tag = data + at + QCA_TAG_HEADER;

		if (at + QCA_TAG_HEADER + len > end) {
			printf("vayu-bt: step 8: nvm tag %u (%u bytes) runs past the tag set at %u: "
			    "stopping\n", (unsigned)id, (unsigned)len, end);
			return false;
		}
		tags++;
		switch (id) {
		case QCA_TAG_ID_BD_ADDR:
			if (len >= 6) {
				printf("vayu-bt: nvm tag 2 (BD address): %02x:%02x:%02x:%02x:%02x:%02x\n",
				    (unsigned)tag[0], (unsigned)tag[1], (unsigned)tag[2],
				    (unsigned)tag[3], (unsigned)tag[4], (unsigned)tag[5]);
			} else {
				bt_dump_counted("nvm tag 2 (BD address, short)", tag, len);
			}
			break;
		case QCA_TAG_ID_HCI:
			bt_dump_counted("nvm tag 17 (HCI) before", tag, len);
			if (len < 3) {
				printf("vayu-bt: step 8: nvm tag 17 is only %u bytes: not patched\n",
				    (unsigned)len);
				break;
			}
			tag[0] = g_ibs ? (tag[0] | QCA_TAG_HCI_IBS) : (tag[0] & (uint8_t)~QCA_TAG_HCI_IBS);
			tag[2] = QCA_BAUDRATE_115200;
			bt_dump_counted("nvm tag 17 (HCI) after", tag, len);
			hci = true;
			break;
		case QCA_TAG_ID_DEEP_SLEEP:
			bt_dump_counted("nvm tag 27 (deep sleep) before", tag, len);
			if (len < 1) {
				printf("vayu-bt: step 8: nvm tag 27 is empty: not patched\n");
				break;
			}
			tag[0] = g_ibs ? (tag[0] | QCA_TAG_DEEP_SLEEP_ON) :
			    (tag[0] & (uint8_t)~QCA_TAG_DEEP_SLEEP_ON);
			bt_dump_counted("nvm tag 27 (deep sleep) after", tag, len);
			deep = true;
			break;
		default:
			break;
		}
		at += QCA_TAG_HEADER + len;
	}
	printf("vayu-bt: step 8: nvm: %u tags; tag 17 %s, tag 27 %s; in-band sleep %s, deep sleep %s "
	    "(vayu_bt_ibs=%u)\n", tags, hci ? "patched" : "not found", deep ? "patched" : "not found",
	    g_ibs ? "on" : "off", g_ibs ? "on" : "off", g_ibs ? 1 : 0);
	return true;
}

static bool
bt_step8(void)
{
	static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };   /* HCI_Reset */
	static const uint8_t version[4] = { 0x01, 0x01, 0x10, 0x00 }; /* Read_Local_Version */
	static const uint8_t bdaddr[4] = { 0x01, 0x09, 0x10, 0x00 };  /* Read_BD_ADDR */
	char rampatch_name[24], nvm_name[24];
	const uint8_t *rampatch, *nvm;
	uint64_t rampatch_size = 0, nvm_size = 0, start = bt_now_us();
	uint8_t *patched = NULL;
	uint32_t total_size, data_length, entry, value;
	uint16_t product_id, rom_build, patch_version, lmp_subver = 0;
	uint8_t format, signature, download_mode;
	bool ack_each, ack_last, hci_reset_ok = false;
	struct bt_h4 h4;
	const uint8_t *payload;
	uint32_t plen;
	uint8_t status;

	printf("vayu-bt: step 8: the firmware download at %u 8N1 (no baud change)\n", g_baud);
	value = g_fw_cts ? 0u : TX_TRANS_CTS_MASK;
	bt_se_wr(SE_UART_TX_TRANS_CFG, value,
	    g_fw_cts ? "honour CTS for the download" : "ignore CTS for the download");
	printf("vayu-bt: step 8: TX_TRANS_CFG = %#x: CTS %s (vayu_bt_fw_cts=%u)\n", value,
	    g_fw_cts ? "honoured" : "ignored", g_fw_cts ? 1 : 0);

	snprintf(rampatch_name, sizeof(rampatch_name), "crbtfw%02x.tlv", g_fw_rom);
	snprintf(nvm_name, sizeof(nvm_name), "crnv%02x.bin", g_fw_rom);
	rampatch = hakios_image_file("fw", rampatch_name, &rampatch_size, NULL);
	if (rampatch == NULL) {
		printf("vayu-bt: step 8: no fw/%s in the image: stopping\n", rampatch_name);
		return false;
	}
	nvm = hakios_image_file("fw", nvm_name, &nvm_size, NULL);
	if (nvm == NULL) {
		printf("vayu-bt: step 8: no fw/%s in the image: stopping\n", nvm_name);
		return false;
	}
	printf("vayu-bt: step 8: fw/%s: %llu bytes, fw/%s: %llu bytes\n", rampatch_name,
	    (unsigned long long)rampatch_size, nvm_name, (unsigned long long)nvm_size);

	/* (a) the rampatch header, checked as qca_tlv_check_data() does */
	if (rampatch_size < 28) {
		printf("vayu-bt: step 8: %s is only %llu bytes, too short for a rampatch header: "
		    "stopping\n", rampatch_name, (unsigned long long)rampatch_size);
		return false;
	}
	if (rampatch[0] != QCA_TLV_TYPE_PATCH || bt_le24(rampatch + 1) != rampatch_size - 4) {
		printf("vayu-bt: step 8: %s is not a rampatch (type %u, length %u, %llu bytes): "
		    "stopping\n", rampatch_name, (unsigned)rampatch[0], bt_le24(rampatch + 1),
		    (unsigned long long)rampatch_size);
		return false;
	}
	total_size = bt_le32(rampatch + 4);
	data_length = bt_le32(rampatch + 8);
	format = rampatch[12];
	signature = rampatch[13];
	download_mode = rampatch[14];
	product_id = bt_le16(rampatch + 16);
	rom_build = bt_le16(rampatch + 18);
	patch_version = bt_le16(rampatch + 20);
	entry = bt_le32(rampatch + 24);
	printf("vayu-bt: step 8: rampatch header: total_size %u, data_length %u, format %#x, "
	    "signature %#x, download_mode %u, product_id %#x, rom_build %#x, patch_version %#x, "
	    "entry %#x\n", total_size, data_length, (unsigned)format, (unsigned)signature,
	    (unsigned)download_mode, (unsigned)product_id, (unsigned)rom_build,
	    (unsigned)patch_version, entry);
	if (download_mode > QCA_DNLD_MODE_VSE_CC) {
		printf("vayu-bt: step 8: download_mode %u is not 0..3: stopping\n",
		    (unsigned)download_mode);
		return false;
	}
	ack_each = download_mode == QCA_DNLD_MODE_NONE || download_mode == QCA_DNLD_MODE_CC;
	ack_last = download_mode == QCA_DNLD_MODE_VSE || download_mode == QCA_DNLD_MODE_VSE_CC;
	printf("vayu-bt: step 8: rampatch download mode %u: an ack %s\n",
	    (unsigned)download_mode, ack_each ? "after every segment" : "only after the last segment");
	if (!bt_qca_send("rampatch", rampatch, rampatch_size, ack_each, ack_last, 100)) {
		return false;
	}
	bt_sleep_ms(10);

	/* (d, e) the NVM, copied so that its tags can be patched, then sent */
	if (nvm_size == 0) {
		printf("vayu-bt: step 8: fw/%s is empty: stopping\n", nvm_name);
		return false;
	}
	patched = kalloc_data((vm_size_t)nvm_size, Z_WAITOK);
	if (patched == NULL) {
		printf("vayu-bt: step 8: no memory for the %llu-byte NVM: stopping\n",
		    (unsigned long long)nvm_size);
		return false;
	}
	memcpy(patched, nvm, (size_t)nvm_size);
	if (!bt_nvm_patch(patched, nvm_size)) {
		kfree_data(patched, (vm_size_t)nvm_size);
		return false;
	}
	printf("vayu-bt: step 8: nvm download mode NONE: an ack after every segment\n");
	if (!bt_qca_send("nvm", patched, nvm_size, true, false, 5)) {
		kfree_data(patched, (vm_size_t)nvm_size);
		return false;
	}
	kfree_data(patched, (vm_size_t)nvm_size);

	/* (f) HCI again */
	printf("vayu-bt: step 8: HCI again\n");
	if (!bt_h4_xfer(reset, sizeof(reset), 0x0c03, 500, &h4, true)) {
		printf("vayu-bt: step 8: HCI_Reset was not sent\n");
		bt_step8_tx_hint();
		return false;
	}
	bt_h4_skipped("step 8: skipped before HCI_Reset", &h4);
	if (!bt_cmd_complete(&h4, 0x0c03, &payload, &plen)) {
		bt_dump_counted("step 8: the answer to HCI_Reset", h4.data, h4.got);
		printf("vayu-bt: step 8: no Command Complete for HCI_Reset\n");
		return false;
	}
	status = payload[3];
	printf("vayu-bt: HCI_Reset: Command Complete, status %#x\n", (unsigned)status);
	if (status != 0) {
		printf("vayu-bt: step 8: HCI_Reset was refused: stopping\n");
		return false;
	}
	hci_reset_ok = true;

	if (!bt_h4_xfer(version, sizeof(version), 0x1001, 500, &h4, true)) {
		printf("vayu-bt: step 8: Read_Local_Version was not sent\n");
		bt_step8_tx_hint();
		return false;
	}
	bt_h4_skipped("step 8: skipped before Read_Local_Version", &h4);
	if (!bt_cmd_complete(&h4, 0x1001, &payload, &plen)) {
		bt_dump_counted("step 8: the answer to Read_Local_Version", h4.data, h4.got);
		printf("vayu-bt: step 8: no Command Complete for Read_Local_Version\n");
		return false;
	}
	if (plen < 12) {
		bt_dump_counted("step 8: the short answer to Read_Local_Version", h4.data, h4.got);
		printf("vayu-bt: step 8: Read_Local_Version answered only %u payload bytes: stopping\n",
		    plen);
		return false;
	}
	{
		uint16_t manufacturer = bt_le16(payload + 8);

		lmp_subver = bt_le16(payload + 10);
		printf("vayu-bt: Read_Local_Version: status %#x, hci_ver %#x, hci_rev %#x, lmp_ver %#x, "
		    "manufacturer %u%s, lmp_subver %#x\n", (unsigned)payload[3], (unsigned)payload[4],
		    (unsigned)bt_le16(payload + 5), (unsigned)payload[7], (unsigned)manufacturer,
		    manufacturer == 29 ? " (Qualcomm)" : "", (unsigned)lmp_subver);
	}
	printf("vayu-bt: lmp_subver %#x (rampatch patch_version %#x)\n", (unsigned)lmp_subver,
	    (unsigned)patch_version);

	if (!bt_h4_xfer(bdaddr, sizeof(bdaddr), 0x1009, 500, &h4, true)) {
		printf("vayu-bt: step 8: Read_BD_ADDR was not sent\n");
		bt_step8_tx_hint();
		return false;
	}
	bt_h4_skipped("step 8: skipped before Read_BD_ADDR", &h4);
	if (!bt_cmd_complete(&h4, 0x1009, &payload, &plen)) {
		bt_dump_counted("step 8: the answer to Read_BD_ADDR", h4.data, h4.got);
		printf("vayu-bt: step 8: no Command Complete for Read_BD_ADDR\n");
		return false;
	}
	if (plen < 10) {
		bt_dump_counted("step 8: the short answer to Read_BD_ADDR", h4.data, h4.got);
		printf("vayu-bt: step 8: Read_BD_ADDR answered only %u payload bytes: stopping\n", plen);
		return false;
	}
	if (payload[3] != 0) {
		printf("vayu-bt: Read_BD_ADDR: status %#x: stopping\n", (unsigned)payload[3]);
		return false;
	}
	printf("vayu-bt: Read_BD_ADDR: %02x:%02x:%02x:%02x:%02x:%02x\n", (unsigned)payload[9],
	    (unsigned)payload[8], (unsigned)payload[7], (unsigned)payload[6],
	    (unsigned)payload[5], (unsigned)payload[4]);

	printf("vayu-bt: step 8: %llu ms in all\n",
	    (unsigned long long)((bt_now_us() - start) / 1000));
	/*
	 * The vendor's check that the patch took: the EDL version request again (run 050558: lmp_subver
	 * stayed 0x2be while hci/lmp_ver went 0x8 -> 0xa). Before the download patch_ver was 0x6699.
	 */
	{
		static const uint8_t edl_version[5] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };

		if (bt_h4_xfer(edl_version, sizeof(edl_version), 0, 500, &h4, true) && h4.found) {
			const uint8_t *ev = h4.data + h4.off;

			if (ev[1] == 0xff && ev[2] >= 14 && ev[3] == 0x00 && ev[4] == 0x02) {
				printf("vayu-bt: step 8: version after the download: patch version %#x, rom version "
				    "%#x, soc id %#x (rampatch patch_version %#x)\n", (unsigned)bt_le16(ev + 9),
				    (unsigned)bt_le16(ev + 11), bt_le32(ev + 13), (unsigned)patch_version);
			} else {
				bt_dump_counted("step 8: the answer to the version request after the download", ev,
				    h4.len);
			}
		} else {
			printf("vayu-bt: step 8: no answer to the version request after the download\n");
		}
	}

	if (hci_reset_ok) {
		printf("vayu-bt: step 8 done: firmware %s + %s loaded, the controller answers HCI\n",
		    rampatch_name, nvm_name);
		g_step8_done = true;
	}
	return true;
}

/* ---- step 9: the H4 tty and kernel control for bluetoothd -------------------------------------- */

/*
 * One tty, two names, one dev_t. Its line discipline is TTYDISC, i.e. linesw[0] (tty_conf.c:102-
 * 112): ttyopen/ttylclose/ttread/ttwrite for open/close/read/write, ttwwakeup as l_start, ttymodem
 * as l_modem, and l_noioctl, which answers ENOTTY for everything so ttioctl_locked() handles the
 * termios commands. bluetoothd's tcgetattr/tcsetattr are TIOCGETA 0x40487413 and TIOCSETA
 * 0x80487414, both of which that path knows (tty.c:1136-1149, 1174-1240).
 *
 * d_type is D_TTY, but that alone does not make EVFILT_READ work: spec_kqfilter() (spec_vnops.c:
 * 831-885) reads cdevsw_flags[major] at :863 and takes the tty filter only when
 * CDEVSW_SELECT_KQUEUE is there and d_type is D_TTY (:872). cdevsw_add() does not set that flag;
 * cdevsw_setkqueueok() (bsd_stubs.c:287-305) does. Without it the knote would be EVFILTID_SPEC,
 * whose attach refuses a plain EVFILT_READ without NOTE_LOWAT=1 (spec_vnops.c:3022-3027).
 */

#define BT_TTY_BAUD             115200
#define BT_TTY_IO_SIZE          256             /* one poll's worth each way */
#define BT_TTY_POLL_US          1000            /* delay() when neither direction moved a byte */
#define BT_TTY_IOCTLS           32              /* distinct command numbers remembered */
#define BT_TTY_NAME_LEN         64

static struct tty *g_tty;
static struct tty *g_tty_table[1];      /* d_ttys, as ttselect() would expect */
static void *g_tty_cu_node, *g_tty_uart_node;
static int g_tty_major = -1;
static volatile bool g_tty_io_stop = false;    /* the io thread's only way out; nothing asks yet */
static volatile bool g_tty_tx_wanted = false;
static uint32_t g_tty_speed_asked = 0;
static u_long g_tty_ioctls[BT_TTY_IOCTLS];
static uint32_t g_tty_ioctls_seen = 0;

/* Used by bt_tty_close() before its definition below. */
static int bt_tty_stop(struct tty *tp, int rw);

/*
 * t_param: called from ttioctl_locked() with the tty lock held (tty.c:1219) for every TIOCSETA*.
 * The GENI dividers are never touched from here: step 8 left the UART at 115200 8N1 and that is
 * what the controller and bluetoothd both use. The requested speed is logged once per change, not
 * once per tcsetattr.
 */
static int
bt_tty_param(__unused struct tty *tp, struct termios *t)
{
	uint32_t speed = (uint32_t)(t->c_ospeed != 0 ? t->c_ospeed : t->c_ispeed);

	if (speed != g_tty_speed_asked) {
		g_tty_speed_asked = speed;
		printf("vayu-bt: tty: speed %u asked; the UART stays at 115200\n", speed);
	}
	return 0;
}

/*
 * t_oproc: ttstart()/ttwrite()/ttywait() call this with the tty lock held (tty.c:1859-1866,
 * tty.c:2515, tty.c:1661). It must not transmit and must not sleep: it only tells the io thread
 * that t_outq has something. The thread polls every millisecond anyway, so the flag is a hint.
 */
static void
bt_tty_oproc(__unused struct tty *tp)
{
	g_tty_tx_wanted = true;
}

/* True the first time this command number is seen; the small table's races only duplicate a line. */
static bool
bt_tty_ioctl_first(u_long cmd)
{
	for (uint32_t i = 0; i < g_tty_ioctls_seen; i++) {
		if (g_tty_ioctls[i] == cmd) {
			return false;
		}
	}
	if (g_tty_ioctls_seen < BT_TTY_IOCTLS) {
		g_tty_ioctls[g_tty_ioctls_seen++] = cmd;
		return true;
	}
	return false;
}

static void
bt_tty_log(const char *what)
{
	char name[BT_TTY_NAME_LEN];

	name[0] = 0;
	proc_selfname(name, sizeof(name));
	printf("vayu-bt: tty: %s by pid %d (%s)\n", what, proc_selfpid(), name);
}

/*
 * As ptsopen (tty_dev.c:258-309): first open sets the termios defaults and the water marks, then
 * l_modem(tp, 1) brings carrier on, then l_open (ttyopen) marks the tty open and, because CLOCAL,
 * connected. The cfmakeraw equivalent is here, not in the io thread: iflag/oflag/lflag 0 and
 * CS8|CREAD|CLOCAL, with both speeds 115200, as the userland side expects.
 */
static int
bt_tty_open(dev_t dev, int flag, __unused int devtype, __unused proc_t p)
{
	struct tty *tp = g_tty;
	int error = 0;

	if (tp == NULL) {
		return ENXIO;
	}
	tty_lock(tp);
	if ((tp->t_state & TS_ISOPEN) == 0) {
		termioschars(&tp->t_termios);           /* the default control chars: tty_dev.c:274 */
		tp->t_iflag = 0;
		tp->t_oflag = 0;
		tp->t_lflag = 0;
		tp->t_cflag = CS8 | CREAD | CLOCAL;
		tp->t_ispeed = tp->t_ospeed = BT_TTY_BAUD;
		ttsetwater(tp);                         /* as ptsopen: would be done in xxparam() */
	} else if ((tp->t_state & TS_XCLUDE) && !kauth_cred_issuser(kauth_cred_get())) {   /* ptsopen: suser() != 0 */
		error = EBUSY;
		goto out;
	}
	if (tp->t_oproc != NULL) {                      /* the controller is still here */
		(void)(*linesw[tp->t_line].l_modem)(tp, 1);
	}
	while ((tp->t_state & TS_CARR_ON) == 0) {
		if (flag & FNONBLOCK) {
			break;
		}
		error = ttysleep(tp, TSA_CARR_ON(tp), TTIPRI | PCATCH, __FUNCTION__, 0);
		if (error != 0) {
			goto out;
		}
	}
	error = (*linesw[tp->t_line].l_open)(dev, tp);
	if (error == 0) {
		bt_tty_log("open");
		printf("vayu-bt: tty: raw 8N1 at %u, carrier on, /dev/cu.bluetooth and "
		    "/dev/uart.bluetooth\n", (unsigned)BT_TTY_BAUD);
	}
out:
	tty_unlock(tp);
	return error;
}

/* As ptsclose (tty_dev.c:312-368): l_close, ttyclose, then a flush; the tty itself stays. */
static int
bt_tty_close(__unused dev_t dev, int flag, __unused int mode, __unused proc_t p)
{
	struct tty *tp = g_tty;
	int error;

	if (tp == NULL) {
		return ENXIO;
	}
	bt_tty_log("close");
	tty_lock(tp);
	error = (*linesw[tp->t_line].l_close)(tp, flag);
	(void)ttyclose(tp);
	(void)bt_tty_stop(tp, FREAD | FWRITE);
	tty_unlock(tp);
	return error;
}

static int
bt_tty_read(__unused dev_t dev, struct uio *uio, int flag)
{
	struct tty *tp = g_tty;
	int error;

	if (tp == NULL) {
		return ENXIO;
	}
	tty_lock(tp);
	if (tp->t_oproc == NULL) {
		error = EIO;
	} else {
		error = (*linesw[tp->t_line].l_read)(tp, uio, flag);
	}
	tty_unlock(tp);
	return error;
}

static int
bt_tty_write(__unused dev_t dev, struct uio *uio, int flag)
{
	struct tty *tp = g_tty;
	int error;

	if (tp == NULL) {
		return ENXIO;
	}
	tty_lock(tp);
	if (tp->t_oproc == NULL) {
		error = EIO;
	} else {
		error = (*linesw[tp->t_line].l_write)(tp, uio, flag);
	}
	tty_unlock(tp);
	return error;
}

/*
 * As ptyioctl (tty_dev.c:932-1152): the line discipline's l_ioctl first, ttioctl_locked() when
 * it answers ENOTTY, all under the tty lock. Every command number is printed the first time it
 * is seen; an error's value is printed with it (a TIOCMGET on a tty with no modem lines is one).
 */
static int
bt_tty_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct tty *tp = g_tty;
	int error;

	if (tp == NULL) {
		return ENXIO;
	}
	if (bt_tty_ioctl_first(cmd)) {
		printf("vayu-bt: tty: ioctl %#lx\n", cmd);
	}
	tty_lock(tp);
	error = (*linesw[tp->t_line].l_ioctl)(tp, cmd, data, flag, p);
	if (error == ENOTTY) {
		error = ttioctl_locked(tp, cmd, data, flag, p);
	}
	tty_unlock(tp);
	if (error != 0) {
		printf("vayu-bt: tty: ioctl %#lx: %d\n", cmd, error);
	}
	return error;
}

/* d_stop: ttystop() calls this with the tty lock held (tty.c:1690-1700); there is nothing to stop. */
static int
bt_tty_stop(__unused struct tty *tp, __unused int rw)
{
	return 0;
}

static int
bt_tty_reset(__unused int uban)
{
	return 0;
}

/*
 * d_select: ttyselect() is called with the lock held (tty.c:1532-1593). Its FMARK path matters
 * here: filt_ttyattach() (tty.c:3368-3415) selects our device with FREAD|FMARK and needs the
 * selrecord on tp->t_rsel even when data is already there, or the knote cannot attach.
 */
static int
bt_tty_select(__unused dev_t dev, int rw, void *wql, proc_t p)
{
	struct tty *tp = g_tty;
	int retval;

	if (tp == NULL) {
		return ENXIO;
	}
	tty_lock(tp);
	retval = ttyselect(tp, rw, wql, p);
	tty_unlock(tp);
	return retval;
}

static const struct cdevsw bt_tty_cdevsw = {
	.d_open = bt_tty_open, .d_close = bt_tty_close, .d_read = bt_tty_read,
	.d_write = bt_tty_write, .d_ioctl = bt_tty_ioctl, .d_stop = bt_tty_stop,
	.d_reset = bt_tty_reset, .d_ttys = g_tty_table, .d_select = bt_tty_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = D_TTY
};

/* ---- the kernel control: bluetoothd's H4:POSIX transport -------------------------------------- */

/*
 * bluetoothd's POSIX HCI driver (16.5, FUN_10006669c) never opens /dev/cu.bluetooth: with
 * BT_HCI_TRANSPORT=H4:POSIX:<name>,<baud> it finds a kernel control by name (CTLIOCGINFO), connects
 * a PF_SYSTEM/SOCK_STREAM socket to it, gets and sets a termios through getsockopt/setsockopt
 * (TIOCGETA 0x40487413, TIOCSETA 0x80487414), then reads and writes plain H4 on the socket. This
 * control is that name: "com.vayu.bt.uart". As network_agent.c:240-270 and tcp_ccdbg.c:150-170 do,
 * ctl_register() takes a struct kern_ctl_reg (bsd/sys/kern_control.h); CTL_FLAG_REG_SOCK_STREAM
 * selects a stream socket, and CTL_FLAG_PRIVILEGED is deliberately absent because bluetoothd runs
 * as mobile. One client at a time.
 *
 * The send callback copies the mbuf chain into a 16 KB TX ring that the step 9 io thread drains
 * through the same H4 framer as t_outq. Until that thread starts, bytes a client sent wait here
 * (bluetoothd may connect before the firmware is in). Like the tty nodes, the control is made
 * when the probe starts, not in step 9. RX bytes the framer keeps go to the client with
 * ctl_enqueuedata() and to the tty when it is open.
 */

#define BT_KCTL_NAME            "com.vayu.bt.uart"
#define BT_KCTL_SEND_SIZE       (16 * 1024)     /* ctl_sendsize: client -> kernel */
#define BT_KCTL_RECV_SIZE       (16 * 1024)     /* ctl_recvsize: kernel -> client */
#define BT_KCTL_TX_SIZE         (16 * 1024)     /* the send callback's ring */
#define BT_KCTL_IO_SIZE         256             /* bytes the io thread drains in one turn */
#define BT_KCTL_LOG_EVERY       100             /* drops/ENOBUFS: the first and every this many */
#define BT_KCTL_UNKNOWN_OPTS    16              /* distinct unknown optnames remembered */

/* setsockopt's int makes TIOCSETA* negative: compare these as uint32_t. */
#define BT_KCTL_TIOCGETA        0x40487413u     /* TIOCGETA (ttycom.h:108) */
#define BT_KCTL_TIOCSETA        0x80487414u     /* TIOCSETA (ttycom.h:109) */
#define BT_KCTL_TIOCSETAW       0x80487415u     /* TIOCSETAW (ttycom.h:110) */
#define BT_KCTL_TIOCSETAF       0x80487416u     /* TIOCSETAF (ttycom.h:111) */

/* The kernel-side termios is 72 bytes (four 8-byte flags, c_cc[20], two 8-byte speeds). */
_Static_assert(sizeof(struct termios) == 72, "the kctl TIOCGETA answer is 72 bytes");
_Static_assert(BT_KCTL_TIOCGETA == (uint32_t)TIOCGETA, "the kctl getopt name is TIOCGETA");
_Static_assert(BT_KCTL_TIOCSETA == (uint32_t)TIOCSETA, "the kctl setopt name is TIOCSETA");
_Static_assert(BT_KCTL_TIOCSETAW == (uint32_t)TIOCSETAW, "the kctl setopt name is TIOCSETAW");
_Static_assert(BT_KCTL_TIOCSETAF == (uint32_t)TIOCSETAF, "the kctl setopt name is TIOCSETAF");

static LCK_GRP_DECLARE(kctl_lck_grp, "vayu-bt-kctl");
static LCK_MTX_DECLARE(kctl_lock, &kctl_lck_grp);

static kern_ctl_ref g_kctl_ctlref;      /* the registered control */
static kern_ctl_ref g_kctl_ref;         /* the one connected client's ref */
static uint32_t g_kctl_unit;
static bool g_kctl_connected;

/* Raw, CS8|CREAD|CLOCAL, 115200: what TIOCGETA answers before bluetoothd's first TIOCSETA*. */
static struct termios g_kctl_termios = {
	.c_iflag = 0,
	.c_oflag = 0,
	.c_cflag = CS8 | CREAD | CLOCAL,
	.c_lflag = 0,
	.c_ispeed = BT_TTY_BAUD,
	.c_ospeed = BT_TTY_BAUD,
};

struct bt_kctl_tx {
	uint8_t buf[BT_KCTL_TX_SIZE];
	uint32_t head;                  /* the next byte to send */
	uint32_t tail;                  /* the next free byte */
	uint32_t count;
	uint32_t drops;                 /* drop events, not bytes */
	uint64_t dropped;               /* bytes the ring could not take */
};

static struct bt_kctl_tx g_kctl_tx;

struct bt_kctl_drop {
	bool log;
	uint32_t now;
	uint32_t events;
	uint64_t bytes;
};

static uint32_t g_kctl_rx_enobufs;
static uint64_t g_kctl_rx_enobuf_bytes;
static uint32_t g_kctl_rx_nodest;
static uint64_t g_kctl_rx_nodest_bytes;

static u_int32_t g_kctl_unknown_get[BT_KCTL_UNKNOWN_OPTS];
static u_int32_t g_kctl_unknown_set[BT_KCTL_UNKNOWN_OPTS];
static uint32_t g_kctl_unknown_get_seen;
static uint32_t g_kctl_unknown_set_seen;

static uint32_t
bt_kctl_termios_speed(const struct termios *t)
{
	return (uint32_t)(t->c_ospeed != 0 ? t->c_ospeed : t->c_ispeed);
}

/* Under kctl_lock: append what fits; the caller logs the drop once the lock is gone. */
static void
bt_kctl_tx_put_locked(const uint8_t *bytes, uint32_t count, struct bt_kctl_drop *drop)
{
	uint32_t space = BT_KCTL_TX_SIZE - g_kctl_tx.count;
	uint32_t take = count < space ? count : space;
	uint32_t first = BT_KCTL_TX_SIZE - g_kctl_tx.tail;

	if (first > take) {
		first = take;
	}
	memcpy(&g_kctl_tx.buf[g_kctl_tx.tail], bytes, first);
	if (take > first) {
		memcpy(&g_kctl_tx.buf[0], bytes + first, take - first);
	}
	g_kctl_tx.tail = (g_kctl_tx.tail + take) % BT_KCTL_TX_SIZE;
	g_kctl_tx.count += take;

	if (take != count) {
		uint32_t lost = count - take;

		g_kctl_tx.drops++;
		g_kctl_tx.dropped += lost;
		if (g_kctl_tx.drops == 1 || g_kctl_tx.drops % BT_KCTL_LOG_EVERY == 0) {
			drop->log = true;
			drop->now = lost;
			drop->events = g_kctl_tx.drops;
			drop->bytes = g_kctl_tx.dropped;
		}
	}
}

/* The io thread's side of the ring: at most max bytes, in order, under the same lock. */
static uint32_t
bt_kctl_tx_drain(uint8_t *out, uint32_t max)
{
	uint32_t count, first;

	lck_mtx_lock(&kctl_lock);
	count = g_kctl_tx.count < max ? g_kctl_tx.count : max;
	if (count != 0) {
		first = BT_KCTL_TX_SIZE - g_kctl_tx.head;
		if (first > count) {
			first = count;
		}
		memcpy(out, &g_kctl_tx.buf[g_kctl_tx.head], first);
		if (count > first) {
			memcpy(out + first, &g_kctl_tx.buf[0], count - first);
		}
		g_kctl_tx.head = (g_kctl_tx.head + count) % BT_KCTL_TX_SIZE;
		g_kctl_tx.count -= count;
	}
	lck_mtx_unlock(&kctl_lock);
	return count;
}

static errno_t
bt_kctl_connect(kern_ctl_ref kctlref, struct sockaddr_ctl *sac, void **unitinfo)
{
	char name[BT_TTY_NAME_LEN];
	uint32_t busy_unit;

	*unitinfo = NULL;

	lck_mtx_lock(&kctl_lock);
	if (g_kctl_connected) {
		if (g_kctl_ref == kctlref && g_kctl_unit == sac->sc_unit) {
			/* The same socket connected again: not a second client. */
			lck_mtx_unlock(&kctl_lock);
			printf("vayu-bt: kctl: connect unit %u repeated; already connected\n",
			    (unsigned)sac->sc_unit);
			return 0;
		}
		busy_unit = g_kctl_unit;
		lck_mtx_unlock(&kctl_lock);
		printf("vayu-bt: kctl: connect unit %u refused: unit %u has the client (EBUSY)\n",
		    (unsigned)sac->sc_unit, (unsigned)busy_unit);
		return EBUSY;
	}
	g_kctl_ref = kctlref;
	g_kctl_unit = sac->sc_unit;
	g_kctl_connected = true;
	lck_mtx_unlock(&kctl_lock);

	name[0] = 0;
	proc_selfname(name, sizeof(name));
	printf("vayu-bt: kctl: connect unit %u by pid %d (%s)\n", (unsigned)sac->sc_unit,
	    proc_selfpid(), name);
	return 0;
}

static errno_t
bt_kctl_disconnect(kern_ctl_ref kctlref, u_int32_t unit, __unused void *unitinfo)
{
	uint32_t queued = 0;
	bool ours = false;

	lck_mtx_lock(&kctl_lock);
	/* A refused second connect also reaches here: only the client's own unit is forgotten. */
	if (g_kctl_connected && g_kctl_ref == kctlref && g_kctl_unit == unit) {
		queued = g_kctl_tx.count;
		g_kctl_tx.head = g_kctl_tx.tail = g_kctl_tx.count = 0;
		g_kctl_ref = NULL;
		g_kctl_unit = 0;
		g_kctl_connected = false;
		ours = true;
	}
	lck_mtx_unlock(&kctl_lock);

	if (ours) {
		printf("vayu-bt: kctl: disconnect unit %u\n", (unsigned)unit);
		if (queued != 0) {
			printf("vayu-bt: kctl: %u bytes queued by unit %u were dropped at disconnect\n",
			    (unsigned)queued, (unsigned)unit);
		}
	}
	return 0;
}

/* The send callback owns the mbuf chain: copy it, then mbuf_freem() it, as kern_control.h says. */
static errno_t
bt_kctl_send(__unused kern_ctl_ref kctlref, u_int32_t unit, __unused void *unitinfo,
    mbuf_t m, __unused int flags)
{
	struct bt_kctl_drop drop = { .log = false };

	if (m == NULL) {
		return 0;
	}
	lck_mtx_lock(&kctl_lock);
	/* A send still in flight when the client disconnects must not seed a later client's ring. */
	if (!g_kctl_connected || g_kctl_unit != unit) {
		lck_mtx_unlock(&kctl_lock);
		mbuf_freem(m);
		return 0;
	}
	for (mbuf_t n = m; n != NULL; n = mbuf_next(n)) {
		const uint8_t *data = (const uint8_t *)mbuf_data(n);
		uint32_t len = (uint32_t)mbuf_len(n);

		if (data != NULL && len != 0) {
			bt_kctl_tx_put_locked(data, len, &drop);
		}
	}
	lck_mtx_unlock(&kctl_lock);
	mbuf_freem(m);

	if (drop.log) {
		printf("vayu-bt: kctl: TX ring full: %u byte(s) dropped, %u drops and %llu bytes "
		    "in all\n", (unsigned)drop.now, (unsigned)drop.events,
		    (unsigned long long)drop.bytes);
	}
	return 0;
}

static void
bt_kctl_unknown_opt(bool get, int opt)
{
	uint32_t value = (uint32_t)opt;
	u_int32_t *table = get ? g_kctl_unknown_get : g_kctl_unknown_set;
	uint32_t *seen = get ? &g_kctl_unknown_get_seen : &g_kctl_unknown_set_seen;
	bool first = false;

	lck_mtx_lock(&kctl_lock);
	for (uint32_t i = 0; i < *seen; i++) {
		if (table[i] == value) {
			lck_mtx_unlock(&kctl_lock);
			return;
		}
	}
	if (*seen < BT_KCTL_UNKNOWN_OPTS) {
		table[(*seen)++] = value;
		first = true;
	}
	lck_mtx_unlock(&kctl_lock);

	if (first) {
		printf("vayu-bt: kctl: %s optname %#x is not handled: ENOPROTOOPT\n",
		    get ? "get" : "set", value);
	}
}

/* TIOCGETA: the current termios, never the UART's. bluetoothd passes a 72-byte buffer. */
static errno_t
bt_kctl_getopt(__unused kern_ctl_ref kctlref, __unused u_int32_t unit, __unused void *unitinfo,
    int opt, void *data, size_t *len)
{
	uint32_t name = (uint32_t)opt;

	if (name != BT_KCTL_TIOCGETA) {
		bt_kctl_unknown_opt(true, opt);
		return ENOPROTOOPT;
	}
	if (len == NULL) {
		return EINVAL;
	}
	if (data == NULL) {
		*len = sizeof(struct termios);
		return 0;
	}
	if (*len < sizeof(struct termios)) {
		*len = sizeof(struct termios);
		return EINVAL;
	}
	lck_mtx_lock(&kctl_lock);
	memcpy(data, &g_kctl_termios, sizeof(struct termios));
	lck_mtx_unlock(&kctl_lock);
	*len = sizeof(struct termios);
	return 0;
}

/*
 * TIOCSETA/TIOCSETAW/TIOCSETAF: store the termios and log the speed and c_cflag once per change.
 * The UART keeps its dividers at 115200; the CRTSCTS bit is only a promise that CTS is honoured.
 */
static errno_t
bt_kctl_setopt(__unused kern_ctl_ref kctlref, __unused u_int32_t unit, __unused void *unitinfo,
    int opt, void *data, size_t len)
{
	uint32_t name = (uint32_t)opt;
	struct termios t;
	uint32_t speed;
	bool changed;

	if (name != BT_KCTL_TIOCSETA && name != BT_KCTL_TIOCSETAW && name != BT_KCTL_TIOCSETAF) {
		bt_kctl_unknown_opt(false, opt);
		return ENOPROTOOPT;
	}
	if (data == NULL || len < sizeof(struct termios)) {
		return EINVAL;
	}
	memcpy(&t, data, sizeof(struct termios));
	speed = bt_kctl_termios_speed(&t);

	lck_mtx_lock(&kctl_lock);
	changed = t.c_cflag != g_kctl_termios.c_cflag ||
	    t.c_ispeed != g_kctl_termios.c_ispeed || t.c_ospeed != g_kctl_termios.c_ospeed;
	memcpy(&g_kctl_termios, &t, sizeof(struct termios));
	lck_mtx_unlock(&kctl_lock);

	if (changed) {
		printf("vayu-bt: kctl: termios speed %u cflag %#lx (the UART stays at 115200, CTS "
		    "honoured)\n", (unsigned)speed, (unsigned long)t.c_cflag);
	}
	return 0;
}

/*
 * Deliver framer-kept RX bytes to the connected client. Returns true when there was a client,
 * whether or not ctl_enqueuedata() took the bytes: an ENOBUFS is counted and logged here, so
 * the io thread does not count the same bytes as having no reader.
 */
static bool
bt_kctl_rx_to_client(const uint8_t *bytes, uint32_t count)
{
	errno_t error;
	uint32_t events = 0;
	uint64_t lost = 0;
	bool log = false;

	/*
	 * The lock is held across ctl_enqueuedata() so a disconnect cannot clear the client (or a
	 * new connect reuse its unit) between the check and the enqueue. The callbacks are always
	 * invoked with the socket lock dropped, so kctl_lock -> ctl/socket lock is the only order.
	 */
	lck_mtx_lock(&kctl_lock);
	if (!g_kctl_connected) {
		lck_mtx_unlock(&kctl_lock);
		return false;
	}
	error = ctl_enqueuedata(g_kctl_ref, g_kctl_unit, (void *)bytes, count, 0);
	if (error != 0) {
		g_kctl_rx_enobufs++;
		g_kctl_rx_enobuf_bytes += count;
		if (g_kctl_rx_enobufs == 1 || g_kctl_rx_enobufs % BT_KCTL_LOG_EVERY == 0) {
			events = g_kctl_rx_enobufs;
			lost = g_kctl_rx_enobuf_bytes;
			log = true;
		}
	}
	lck_mtx_unlock(&kctl_lock);

	if (log) {
		printf("vayu-bt: kctl: RX %u bytes: ctl_enqueuedata failed: %d%s; %llu bytes lost "
		    "in %u events\n", (unsigned)count, error, error == ENOBUFS ? " (ENOBUFS)" : "",
		    (unsigned long long)lost, (unsigned)events);
	}
	return true;
}

/* No client and no open tty: say so, or the controller's bytes would vanish without a trace. */
static void
bt_kctl_rx_no_reader(uint32_t count)
{
	uint32_t events = 0;
	uint64_t lost = 0;
	bool log = false;

	lck_mtx_lock(&kctl_lock);
	g_kctl_rx_nodest++;
	g_kctl_rx_nodest_bytes += count;
	if (g_kctl_rx_nodest == 1 || g_kctl_rx_nodest % BT_KCTL_LOG_EVERY == 0) {
		events = g_kctl_rx_nodest;
		lost = g_kctl_rx_nodest_bytes;
		log = true;
	}
	lck_mtx_unlock(&kctl_lock);
	if (log) {
		printf("vayu-bt: kctl: RX %u bytes had no reader (no client, tty closed); %llu bytes "
		    "lost in %u events\n", (unsigned)count, (unsigned long long)lost,
		    (unsigned)events);
	}
}

/*
 * The one way bytes go toward the host, used by the controller's real RX and by the synthetic
 * answers of the VSC shim: the kctl client gets a copy and, while the tty is open, the line
 * discipline gets every byte. The kctl copy is made first and outside the tty lock, as before.
 */
static void
bt_deliver_to_host(const uint8_t *bytes, uint32_t count)
{
	struct tty *tp = g_tty;
	bool client = bt_kctl_rx_to_client(bytes, count);
	bool tty_open = false;

	if (tp != NULL) {
		tty_lock(tp);
		/*
		 * Nothing goes to the line discipline before the first open or after the last
		 * close: those bytes would be read as stale events. The kctl client, if any,
		 * has its copy already.
		 */
		if ((tp->t_state & TS_ISOPEN) != 0) {
			for (uint32_t i = 0; i < count; i++) {
				(*linesw[tp->t_line].l_rint)(bytes[i], tp);
			}
			tty_open = true;
		}
		tty_unlock(tp);
	}
	if (!client && !tty_open) {
		bt_kctl_rx_no_reader(count);
	}
}

/* The probe-start registration (vayu_bt >= 9), modelled on netagent_register_control() but
 * without CTL_FLAG_PRIVILEGED: bluetoothd runs as mobile. */
static bool
bt_kctl_create(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, BT_KCTL_NAME, sizeof(reg.ctl_name));
	reg.ctl_name[sizeof(reg.ctl_name) - 1] = 0;
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = BT_KCTL_SEND_SIZE;
	reg.ctl_recvsize = BT_KCTL_RECV_SIZE;
	reg.ctl_connect = bt_kctl_connect;
	reg.ctl_disconnect = bt_kctl_disconnect;
	reg.ctl_send = bt_kctl_send;
	reg.ctl_setopt = bt_kctl_setopt;
	reg.ctl_getopt = bt_kctl_getopt;

	error = ctl_register(&reg, &g_kctl_ctlref);
	if (error != 0) {
		printf("vayu-bt: kctl: ctl_register(\"%s\") failed: %d\n", BT_KCTL_NAME, error);
		g_kctl_ctlref = NULL;
		return false;
	}
	printf("vayu-bt: kctl: \"%s\" registered (SOCK_STREAM, %u bytes each way, no root); "
	    "bluetoothd: BT_HCI_TRANSPORT=H4:POSIX:%s,115200\n", BT_KCTL_NAME,
	    (unsigned)BT_KCTL_SEND_SIZE, BT_KCTL_NAME);
	return true;
}

/* ---- step 9's H4 framer: packet boundaries, the trace, and the sleep bytes ---------------------- */

#define BT_H4_CMD               0x01            /* host -> controller, hdr opcode le16 + len u8 */
#define BT_H4_ACL               0x02            /* hdr handle le16 + len le16 */
#define BT_H4_SCO               0x03            /* hdr handle le16 + len u8 */
#define BT_H4_EVT               0x04            /* hdr code + len u8 */
#define BT_H4_WAKE_IND          0xfd            /* hci_qca.c's in-band sleep bytes, at a boundary */
#define BT_H4_SLEEP_IND         0xfe
#define BT_H4_WAKE_ACK          0xfc
#define BT_H4_HDR_MAX           4
#define BT_H4_FIRST             16              /* bytes of a packet the trace shows */
#define BT_H4_TRACE_PACKETS     200             /* per direction: one line each */
#define BT_H4_SUMMARY_EVERY     500             /* then one summary line every this many packets */
#define BT_H4_SLEEP_LOGGED      10              /* the first 10 in-band bytes of each kind */
#define BT_H4_STRAY_LOGGED      10              /* the first 10 unexpected boundary bytes */

struct bt_h4_framer {
	bool in_packet;
	uint8_t type;
	uint8_t hdr[BT_H4_HDR_MAX];
	uint8_t first[BT_H4_FIRST];
	uint32_t hdr_len, hdr_got;
	uint32_t payload_len, payload_got;
	uint32_t first_len;
};

struct bt_h4_way {
	struct bt_h4_framer framer;
	uint32_t packets;
	uint64_t bytes;
	uint32_t strays;
	uint32_t strays_logged;
	const char *source;             /* TX only: "tty" or "kctl", from the packet's first byte */
	bool synthetic;                 /* this way only traces a shim answer, not controller bytes */
};

static struct bt_h4_way g_h4_tx, g_h4_rx;
static uint32_t g_h4_ibs_count[3], g_h4_ibs_logged[3];
static const char *const g_h4_ibs_name[3] = { "WAKE_IND", "SLEEP_IND", "WAKE_ACK" };

/* One line per packet: the direction and source, the type-specific header, the first 16 bytes. */
static void
bt_h4_trace(struct bt_h4_way *way, bool tx)
{
	struct bt_h4_framer *f = &way->framer;
	const char *source = tx && way->source != NULL ? way->source : "?";
	char hex[3 * BT_H4_FIRST + 1];

	bt_hex(hex, sizeof(hex), f->first, f->first_len);
	switch (f->type) {
	case BT_H4_CMD: {
		uint16_t opcode = bt_le16(f->hdr);

		printf("vayu-bt: h4 > %s cmd %#06x len %u: %s\n", source, (unsigned)opcode,
		    (unsigned)f->payload_len, hex);
		break;
	}
	case BT_H4_ACL: {
		uint16_t handle = (uint16_t)(bt_le16(f->hdr) & 0x0fffu);

		if (tx) {
			printf("vayu-bt: h4 > %s acl handle %#x len %u\n", source,
			    (unsigned)handle, (unsigned)f->payload_len);
		} else {
			printf("vayu-bt: h4 < acl handle %#x len %u\n", (unsigned)handle,
			    (unsigned)f->payload_len);
		}
		break;
	}
	case BT_H4_SCO: {
		uint16_t handle = (uint16_t)(bt_le16(f->hdr) & 0x0fffu);

		if (tx) {
			printf("vayu-bt: h4 > %s sco handle %#x len %u\n", source,
			    (unsigned)handle, (unsigned)f->payload_len);
		} else {
			printf("vayu-bt: h4 < sco handle %#x len %u\n", (unsigned)handle,
			    (unsigned)f->payload_len);
		}
		break;
	}
	case BT_H4_EVT: {
		uint8_t code = f->hdr[0];

		printf("vayu-bt: h4 < evt %#04x len %u%s: %s", (unsigned)code,
		    (unsigned)f->payload_len, way->synthetic ? " (synthetic)" : "", hex);
		if (code == 0x0e && f->first_len >= 7) {
			/* num, opcode le16, status (HCI 4.1, Vol 4, Part E, 7.7.14) */
			printf(" for %#06x status %#x", (unsigned)bt_le16(f->first + 4),
			    (unsigned)f->first[6]);
		} else if (code == 0x0f && f->first_len >= 7) {
			/* status, num, opcode le16 (7.7.15) */
			printf(" status %#x for %#06x", (unsigned)f->first[3],
			    (unsigned)bt_le16(f->first + 5));
		}
		printf("\n");
		break;
	}
	default:
		break;
	}
}

static void
bt_h4_remember(struct bt_h4_framer *f, uint8_t byte)
{
	if (f->first_len < BT_H4_FIRST) {
		f->first[f->first_len++] = byte;
	}
}

/* A packet is complete: count it, and trace it or (after the 200th) summarise it. */
static void
bt_h4_done(struct bt_h4_way *way, bool tx)
{
	struct bt_h4_framer *f = &way->framer;
	uint32_t total = 1 + f->hdr_len + f->payload_len;

	f->in_packet = false;
	way->packets++;
	way->bytes += total;
	if (way->packets <= BT_H4_TRACE_PACKETS) {
		bt_h4_trace(way, tx);
	} else if ((way->packets - BT_H4_TRACE_PACKETS) % BT_H4_SUMMARY_EVERY == 0) {
		printf("vayu-bt: h4: packets > %u (%llu bytes) < %u (%llu bytes); strays > %u < %u; "
		    "in-band 0xfd %u, 0xfe %u, 0xfc %u\n",
		    g_h4_tx.packets, (unsigned long long)g_h4_tx.bytes,
		    g_h4_rx.packets, (unsigned long long)g_h4_rx.bytes,
		    g_h4_tx.strays, g_h4_rx.strays,
		    g_h4_ibs_count[0], g_h4_ibs_count[1], g_h4_ibs_count[2]);
	}
}

static void
bt_h4_start(struct bt_h4_way *way, uint8_t type)
{
	struct bt_h4_framer *f = &way->framer;

	f->in_packet = true;
	f->type = type;
	f->hdr_len = type == BT_H4_ACL ? 4 : (type == BT_H4_EVT ? 2 : 3);
	f->hdr_got = 0;
	f->payload_len = 0;
	f->payload_got = 0;
	f->first_len = 0;
	bt_h4_remember(f, type);
}

/* One byte inside a packet: the header first (the lengths live there), then its payload. */
static void
bt_h4_byte(struct bt_h4_way *way, bool tx, uint8_t byte)
{
	struct bt_h4_framer *f = &way->framer;

	bt_h4_remember(f, byte);
	if (f->hdr_got < f->hdr_len) {
		f->hdr[f->hdr_got++] = byte;
		if (f->hdr_got < f->hdr_len) {
			return;
		}
		switch (f->type) {
		case BT_H4_ACL:
			f->payload_len = bt_le16(f->hdr + 2);
			break;
		case BT_H4_EVT:
			f->payload_len = f->hdr[1];
			break;
		default:                /* command and SCO carry one length byte */
			f->payload_len = f->hdr[2];
			break;
		}
		if (f->payload_len != 0) {
			return;
		}
	} else if (++f->payload_got < f->payload_len) {
		return;
	}
	bt_h4_done(way, tx);
}

/*
 * TX: the bytes pass through unchanged; the framer is only for the trace. A byte that is not a
 * packet type at a boundary is still sent, but cannot start a packet: count it and go on. source
 * is "tty" or "kctl" and is remembered at the packet's first byte for the trace line.
 */
static void
bt_h4_tx_byte(uint8_t byte, const char *source)
{
	struct bt_h4_way *way = &g_h4_tx;

	if (!way->framer.in_packet) {
		way->source = source;
		switch (byte) {
		case BT_H4_CMD:
		case BT_H4_ACL:
		case BT_H4_SCO:
		case BT_H4_EVT:
			bt_h4_start(way, byte);
			return;
		default:
			way->strays++;
			if (way->strays_logged < BT_H4_STRAY_LOGGED) {
				way->strays_logged++;
				printf("vayu-bt: h4 tx: byte %#x is not a packet type at a packet "
				    "boundary\n", (unsigned)byte);
			}
			return;
		}
	}
	bt_h4_byte(way, true, byte);
}

static int
bt_h4_ibs_kind(uint8_t byte)
{
	switch (byte) {
	case BT_H4_WAKE_IND:
		return 0;
	case BT_H4_SLEEP_IND:
		return 1;
	case BT_H4_WAKE_ACK:
		return 2;
	default:
		return -1;
	}
}

/*
 * RX: true means the byte belongs to the tty/kctl client. At a packet boundary 0xfd/0xfe/0xfc are
 * the in-band sleep bytes: counted, the first 10 of each logged, never delivered, and 0xfd asks
 * for a 0xfc outside the tty lock. Any other byte that is not a packet type is dropped and
 * counted. Inside a packet every byte is data, even one that looks like 0xfd.
 */
static bool
bt_h4_rx_byte(uint8_t byte, bool *wake_ack)
{
	struct bt_h4_way *way = &g_h4_rx;
	int kind;

	if (!way->framer.in_packet) {
		kind = bt_h4_ibs_kind(byte);
		if (kind >= 0) {
			g_h4_ibs_count[kind]++;
			if (g_h4_ibs_logged[kind] < BT_H4_SLEEP_LOGGED) {
				g_h4_ibs_logged[kind]++;
				printf("vayu-bt: h4 rx: in-band sleep byte %#x (%s), not delivered\n",
				    (unsigned)byte, g_h4_ibs_name[kind]);
			}
			if (byte == BT_H4_WAKE_IND) {
				*wake_ack = true;
			}
			return false;
		}
		switch (byte) {
		case BT_H4_CMD:
		case BT_H4_ACL:
		case BT_H4_SCO:
		case BT_H4_EVT:
			bt_h4_start(way, byte);
			return true;
		default:
			way->strays++;
			if (way->strays_logged < BT_H4_STRAY_LOGGED) {
				way->strays_logged++;
				printf("vayu-bt: h4 rx: unexpected byte %#x at a packet boundary\n",
				    (unsigned)byte);
			}
			return false;
		}
	}
	bt_h4_byte(way, false, byte);
	return true;
}

/* The caller delivers what this returns: the bytes that belong to the tty/kctl client, unchanged. */
static uint32_t
bt_h4_rx_feed(const uint8_t *bytes, uint32_t count, uint8_t *out, bool *wake_ack)
{
	uint32_t kept = 0;

	for (uint32_t i = 0; i < count; i++) {
		if (bt_h4_rx_byte(bytes[i], wake_ack)) {
			out[kept++] = bytes[i];
		}
	}
	return kept;
}

/* ---- the vendor-specific command shim (vayu_bt_vsc) ------------------------------------------- */

#define BT_VSC_PASS             0               /* send the packet to the controller */
#define BT_VSC_CC               1               /* answer with a synthetic Command Complete */
#define BT_VSC_CS               2               /* answer with a synthetic Command Status */
#define BT_VSC_ANSWERED         3               /* bt_vsc_tx_filter: not sent, event queued */
#define BT_VSC_OGF              0x3f            /* vendor-specific OGF: opcode >> 10 */
#define BT_VSC_MAX              2048            /* the outbound reassembly buffer */
#define BT_VSC_MAX_PARAMS       251             /* 255 minus num, opcode and status in a CC */
#define BT_VSC_LOG_EVERY        50              /* one summary per this many vendor commands */
#define BT_VSC_OUT_SIZE         (BT_TTY_IO_SIZE + BT_VSC_MAX)

struct bt_vsc_rule {
	uint16_t opcode;                /* full 16-bit opcode, e.g. 0xfc18 */
	uint8_t action;                 /* BT_VSC_PASS, BT_VSC_CC, BT_VSC_CS */
	uint8_t status;                 /* the status byte to report (0 success, 1 unknown, ...) */
	uint8_t nparams;                /* bytes after the status in a CC, from params[] */
	const uint8_t *params;
};

static struct bt_vsc_rule bt_vsc_rules[] = {
	/*
	 * Filled in from the bt7 trace. The all-zero entry is only a placeholder until then: its
	 * opcode has OGF 0, and the filter looks only at OGF 0x3f, so it can never match.
	 */
	{ .opcode = 0x0000, .action = BT_VSC_PASS, .status = 0, .nparams = 0, .params = NULL },
	/*
	 * Run 150523 (bt7c): bluetoothd's init chain aborts (reason 108 = 0x6c) right after 0x0c31,
	 * because its next stage, 0x0c5b Write_Default_Erroneous_Data_Reporting, is gated on
	 * Supported_Commands octet 18 bit 3, which the Qualcomm controller leaves clear (decompile:
	 * FUN_10010b970 -> FUN_10010c8c4 returns 0x274 unsent; an Unknown Command answer aborts too).
	 * bt_sc_rewrite() sets that bit in the 0x1002 answer and this rule answers 0x0c5b itself.
	 * A standard (non-vendor) opcode is only ever acted on when it is listed here.
	 */
	{ .opcode = 0x0c5b, .action = BT_VSC_CC, .status = 0, .nparams = 0, .params = NULL },
	/*
	 * Run 231946 (, vsc=2): bluetoothd asserted at bcm_vsc.c:642 in its create-multi-adv
	 * reply parser (decompile: FUN_100126250, _BCM_CreateMultiAdvInstParser, sent as opcode 0x148 =
	 * 0xfd48 by FUN_10012d970). With status 0 it needs >= 1 payload byte (our empty CC asserts);
	 * with a non-zero status it skips the stream and calls back with 0xff. Run 205056 (vsc=1) shows
	 * the real Qualcomm answer to 0xfd48 is status 0x01 and bluetoothd did not assert on it: answer
	 * that, so multi-adv is simply unavailable. Mode 2's catch-all (status 0, no params) still
	 * covers every vendor command not listed here.
	 */
	{ .opcode = 0xfd48, .action = BT_VSC_CC, .status = 0x01, .nparams = 0, .params = NULL },
};

#define BT_VSC_RULE_COUNT       (sizeof(bt_vsc_rules) / sizeof(bt_vsc_rules[0]))

static uint32_t g_vsc_mode;             /* vayu_bt_vsc: 0 off, 1 table, 2 answer every VSC */
static uint32_t g_vsc_seen, g_vsc_passed, g_vsc_answered;
static uint32_t g_vsc_logged;           /* per-command vsc lines printed so far */
static bool g_bt_trace;                 /* vayu_bt_trace=1: every per-command vsc line, no cap */
#define BT_VSC_LOG_FIRST        64      /* without vayu_bt_trace: the first 64 decisions, then summaries only */

/*
 * The shim's per-command lines are on a hot path: bluetoothd restarts and resends dozens of vendor
 * commands per session, and each printed line costs loader-log ring space. Without vayu_bt_trace=1
 * only the first BT_VSC_LOG_FIRST decisions are printed; the summary every BT_VSC_LOG_EVERY stays.
 */
static bool
bt_vsc_verbose(void)
{
	if (g_bt_trace) {
		return true;
	}
	if (g_vsc_logged < BT_VSC_LOG_FIRST) {
		g_vsc_logged++;
		if (g_vsc_logged == BT_VSC_LOG_FIRST) {
			printf("vayu-bt: vsc: %u per-command lines printed; only summaries from now on (vayu_bt_trace=1 for all)\n",
			    (unsigned)BT_VSC_LOG_FIRST);
		}
		return true;
	}
	return false;
}
static bool g_vsc_oversize_logged;

/* Flush the "to UART" buffer: the io thread is the only caller. */
static void
bt_vsc_tx_flush(const uint8_t *bytes, uint32_t *count)
{
	if (*count == 0) {
		return;
	}
	if (!bt_tx_quiet(bytes, *count)) {
		printf("vayu-bt: vsc: %u bytes for the UART were not sent\n", (unsigned)*count);
	}
	*count = 0;
}

/* Append to the "to UART" buffer, flushing it first if the next piece would not fit. */
static void
bt_vsc_tx_emit(uint8_t *out, uint32_t *out_len, const uint8_t *bytes, uint32_t count)
{
	if (*out_len + count > BT_VSC_OUT_SIZE) {
		bt_vsc_tx_flush(out, out_len);
	}
	if (count > BT_VSC_OUT_SIZE) {
		/*
		 * A piece larger than the whole buffer cannot come from the io thread's 256-byte
		 * reads; send it rather than lose it.
		 */
		(void)bt_tx_quiet(bytes, count);
		return;
	}
	memcpy(out + *out_len, bytes, count);
	*out_len += count;
}

/*
 * A synthetic answer takes the same track as a controller event: bt_h4_trace() prints it as
 * "h4 < evt ... (synthetic)". A throw-away framer is used so g_h4_rx's packet, if the controller
 * stopped in the middle of one, is not disturbed.
 */
static void
bt_vsc_trace_synthetic(const uint8_t *event, uint32_t count)
{
	struct bt_h4_way way;

	if (count < 2 || event[0] != BT_H4_EVT) {
		return;
	}
	bzero(&way, sizeof(way));
	way.synthetic = true;
	bt_h4_start(&way, event[0]);
	for (uint32_t i = 1; i < count; i++) {
		bt_h4_byte(&way, false, event[i]);
	}
}

static const struct bt_vsc_rule *
bt_vsc_find(uint16_t opcode)
{
	for (uint32_t i = 0; i < BT_VSC_RULE_COUNT; i++) {
		if (bt_vsc_rules[i].opcode == opcode) {
			return &bt_vsc_rules[i];
		}
	}
	return NULL;
}

/*
 * Build and enqueue one synthetic answer. A Command Complete is
 * 04 0e <4+nparams> 01 <op lo> <op hi> <status> <params...>: the length counts the one
 * num_hci_command_packets byte, the two opcode bytes and the status. A Command Status is
 * 04 0f 04 <status> 01 <op lo> <op hi>.
 */
static void
bt_vsc_answer(uint16_t opcode, uint8_t action, uint8_t status, uint8_t nparams,
    const uint8_t *params)
{
	uint8_t event[8 + BT_VSC_MAX_PARAMS];
	uint32_t count;
	bool verbose = bt_vsc_verbose();

	if (action == BT_VSC_CS) {
		event[0] = BT_H4_EVT;
		event[1] = 0x0f;                /* Command Status, HCI 4.1 7.7.15 */
		event[2] = 4;
		event[3] = status;
		event[4] = 1;                   /* num_hci_command_packets */
		event[5] = (uint8_t)(opcode & 0xff);
		event[6] = (uint8_t)(opcode >> 8);
		count = 7;
		if (verbose) printf("vayu-bt: vsc: opcode %#06x answered (CS status %#x)\n",
		    (unsigned)opcode, (unsigned)status);
	} else {
		if (nparams > BT_VSC_MAX_PARAMS) {
			printf("vayu-bt: vsc: opcode %#06x: %u params do not fit a CC, only %u used\n",
			    (unsigned)opcode, (unsigned)nparams, (unsigned)BT_VSC_MAX_PARAMS);
			nparams = BT_VSC_MAX_PARAMS;
		}
		if (nparams != 0 && params == NULL) {
			printf("vayu-bt: vsc: opcode %#06x: rule says %u params but has no array\n",
			    (unsigned)opcode, (unsigned)nparams);
			nparams = 0;
		}
		event[0] = BT_H4_EVT;
		event[1] = 0x0e;                /* Command Complete, 7.7.14 */
		event[2] = (uint8_t)(4 + nparams);
		event[3] = 1;                   /* num_hci_command_packets */
		event[4] = (uint8_t)(opcode & 0xff);
		event[5] = (uint8_t)(opcode >> 8);
		event[6] = status;
		if (nparams != 0) {
			memcpy(event + 7, params, nparams);
		}
		count = 7 + nparams;
		if (verbose) printf("vayu-bt: vsc: opcode %#06x answered (CC status %#x, %u params)\n",
		    (unsigned)opcode, (unsigned)status, (unsigned)nparams);
	}
	if (verbose) {
		bt_vsc_trace_synthetic(event, count);   /* a throw-away framer: not bounded by the 200-packet trace cap */
	}
	bt_deliver_to_host(event, count);
}

/*
 * Called once for each complete H4 packet the io thread reassembles, before anything reaches
 * bt_tx_quiet(). Only a command with OGF 0x3f is considered; every other packet, and every
 * command outside OGF 0x3f, is PASS. In mode 1 an opcode that is not in the table is PASS, so the
 * controller's own answer is traced; in mode 2 it is a Command Complete status 0 with no
 * parameters.
 */
static int
bt_vsc_tx_filter(const uint8_t *pkt, uint32_t len)
{
	const struct bt_vsc_rule *rule;
	uint16_t opcode;
	uint8_t action, status = 0, nparams = 0;
	const uint8_t *params = NULL;

	if (g_vsc_mode == 0 || len < 4 || pkt[0] != BT_H4_CMD) {
		return BT_VSC_PASS;
	}
	opcode = bt_le16(pkt + 1);
	rule = bt_vsc_find(opcode);
	if ((opcode >> 10) != BT_VSC_OGF) {
		/* a standard command: acted on only when the table lists it with an answer */
		if (rule == NULL || (rule->action != BT_VSC_CC && rule->action != BT_VSC_CS) || opcode == 0) {
			return BT_VSC_PASS;
		}
		/* (bt_vsc_answer() prints the answer line under the same cap) */
		bt_vsc_answer(opcode, rule->action, rule->status, rule->nparams, rule->params);
		return BT_VSC_ANSWERED;
	}

	g_vsc_seen++;
	if (rule != NULL) {
		action = rule->action;
		status = rule->status;
		nparams = rule->nparams;
		params = rule->params;
	} else if (g_vsc_mode == 2) {
		action = BT_VSC_CC;
	} else {
		action = BT_VSC_PASS;
	}
	if (action != BT_VSC_CC && action != BT_VSC_CS) {
		action = BT_VSC_PASS;
	}

	if (action == BT_VSC_PASS) {
		g_vsc_passed++;
		if (bt_vsc_verbose()) {
			printf("vayu-bt: vsc: opcode %#06x passed to the chip\n", (unsigned)opcode);
		}
	} else {
		g_vsc_answered++;
		bt_vsc_answer(opcode, action, status, nparams, params);
	}

	if (g_vsc_seen % BT_VSC_LOG_EVERY == 0) {
		printf("vayu-bt: vsc: %u vendor commands: %u passed, %u answered\n",
		    (unsigned)g_vsc_seen, (unsigned)g_vsc_passed, (unsigned)g_vsc_answered);
	}
	return action == BT_VSC_PASS ? BT_VSC_PASS : BT_VSC_ANSWERED;
}

/*
 * The controller's Read_Local_Supported_Commands answer, rewritten in the byte stream: the
 * Command Complete 04 0e 44 01 02 10 00 is followed by the 64 Supported_Commands octets, octet k
 * at event byte 7 + k. With vayu_bt_vsc >= 1, octet 18 gets bit 3 (0x08, Write_Default_Erroneous_
 * Data_Reporting) so bluetoothd sends 0x0c5b, which the table then answers. A matcher that keeps
 * its place across reads, because a 68-byte event can straddle two 256-byte polls.
 */
static const uint8_t bt_sc_prefix[7] = { 0x04, 0x0e, 0x44, 0x01, 0x02, 0x10, 0x00 };
static uint32_t g_sc_match;             /* prefix bytes matched so far */
static int g_sc_pos = -1;               /* index in the event of the last byte seen, -1: not in one */

static void
bt_sc_rewrite(uint8_t *bytes, uint32_t count)
{
	if (g_vsc_mode == 0) {
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t b = bytes[i];

		if (g_sc_pos >= 0) {
			g_sc_pos++;
			if (g_sc_pos == 7 + 18) {
				bytes[i] = (uint8_t)(b | 0x08);
				printf("vayu-bt: vsc: Supported_Commands octet 18 %#04x -> %#04x (0x0c5b advertised)\n",
				    (unsigned)b, (unsigned)bytes[i]);
				g_sc_pos = -1;
			}
			continue;
		}
		if (b == bt_sc_prefix[g_sc_match]) {
			g_sc_match++;
		} else {
			g_sc_match = (b == bt_sc_prefix[0]) ? 1 : 0;
		}
		if (g_sc_match == sizeof(bt_sc_prefix)) {
			g_sc_match = 0;
			g_sc_pos = (int)sizeof(bt_sc_prefix) - 1;
		}
	}
}

/*
 * One outbound source's reassembly: the io thread reads t_outq and the kctl ring in chunks that
 * are not packet-aligned, so every packet is collected until its header's length is satisfied
 * and the filter sees it whole. A packet longer than BT_VSC_MAX (only a large ACL packet can be)
 * is passed unparsed after its header, so memory stays bounded. Separate state for tty and kctl
 * keeps a short packet from one source out of the other's.
 */
struct bt_vsc_tx {
	uint8_t pkt[BT_VSC_MAX];        /* one packet, from its type byte */
	uint32_t got;                   /* bytes in pkt, or bytes seen while streaming */
	uint32_t want;                  /* total packet bytes, once the header is complete */
	bool streaming;                 /* want > BT_VSC_MAX: the rest goes out unparsed */
};

static struct bt_vsc_tx g_vsc_tty, g_vsc_kctl;

static void
bt_vsc_tx_feed(struct bt_vsc_tx *tx, const uint8_t *bytes, uint32_t count,
    const char *source, uint8_t *out, uint32_t *out_len)
{
	for (uint32_t i = 0; i < count; i++) {
		uint8_t byte = bytes[i];

		bt_h4_tx_byte(byte, source);    /* the existing trace, with its source tag */

		if (tx->got == 0) {
			switch (byte) {
			case BT_H4_CMD:
			case BT_H4_ACL:
			case BT_H4_SCO:
			case BT_H4_EVT:
				tx->pkt[0] = byte;
				tx->got = 1;
				tx->want = 0;
				tx->streaming = false;
				break;
			default:
				/* Not a packet type: bt6 sent it, so send it too. */
				bt_vsc_tx_emit(out, out_len, &byte, 1);
				break;
			}
			continue;
		}

		if (tx->streaming) {
			bt_vsc_tx_emit(out, out_len, &byte, 1);
			if (++tx->got == tx->want) {
				tx->got = 0;
				tx->want = 0;
				tx->streaming = false;
			}
			continue;
		}

		tx->pkt[tx->got++] = byte;
		if (tx->want == 0) {
			uint32_t want = 0;

			switch (tx->pkt[0]) {
			case BT_H4_CMD:
				if (tx->got >= 4) {
					want = 4u + tx->pkt[3];
				}
				break;
			case BT_H4_ACL:
				if (tx->got >= 5) {
					want = 5u + bt_le16(tx->pkt + 3);
				}
				break;
			case BT_H4_SCO:
				if (tx->got >= 4) {
					want = 4u + tx->pkt[3];
				}
				break;
			case BT_H4_EVT:
				if (tx->got >= 3) {
					want = 3u + tx->pkt[2];
				}
				break;
			}
			if (want == 0) {
				continue;
			}
			tx->want = want;
			if (want > BT_VSC_MAX) {
				/*
				 * Longer than the reassembly buffer: pass the header bytes and
				 * every further byte without parsing (logged once).
				 */
				if (!g_vsc_oversize_logged) {
					g_vsc_oversize_logged = true;
					printf("vayu-bt: vsc: a packet longer than %u bytes is "
					    "passed unparsed\n", (unsigned)BT_VSC_MAX);
				}
				tx->streaming = true;
				bt_vsc_tx_emit(out, out_len, tx->pkt, tx->got);
			}
		}
		if (tx->got == tx->want) {
			if (!tx->streaming &&
			    bt_vsc_tx_filter(tx->pkt, tx->got) == BT_VSC_PASS) {
				bt_vsc_tx_emit(out, out_len, tx->pkt, tx->got);
			}
			tx->got = 0;
			tx->want = 0;
			tx->streaming = false;
		}
	}
}

/* Reassemble one source's chunk, filter its packets, and send what was passed. */
static void
bt_vsc_tx_send(struct bt_vsc_tx *tx, const uint8_t *bytes, uint32_t count, const char *source)
{
	uint8_t out[BT_VSC_OUT_SIZE];
	uint32_t out_len = 0;

	bt_vsc_tx_feed(tx, bytes, count, source, out, &out_len);
	bt_vsc_tx_flush(out, &out_len);
}

/*
 * Step 9's one thread, and from here on the SE's only user: the probe's thread finishes after
 * bt_finish() and never calls bt_tx()/bt_rx() again. Each turn drains what the tty or the kctl
 * client queued and polls the controller once; a millisecond of delay() when neither moved a
 * byte, so an idle tty costs 1000 turns a second, not a busy spin.
 */
static void
vayu_bt_io_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t in[BT_TTY_IO_SIZE];
	uint8_t out[BT_TTY_IO_SIZE];

	while (!g_tty_io_stop) {
		struct tty *tp = g_tty;
		bool moved = false;
		uint32_t n;

		if (tp != NULL) {
			uint32_t queued;

			tty_lock(tp);
			queued = (uint32_t)q_to_b(&tp->t_outq, in, BT_TTY_IO_SIZE);
			g_tty_tx_wanted = false;
			tty_unlock(tp);
			if (queued != 0) {
				if (g_vsc_mode == 0) {
					for (uint32_t i = 0; i < queued; i++) {
						bt_h4_tx_byte(in[i], "tty");
					}
					if (!bt_tx_quiet(in, queued)) {
						printf("vayu-bt: tty: %u bytes from the tty were not "
						    "sent\n", queued);
					}
				} else {
					bt_vsc_tx_send(&g_vsc_tty, in, queued, "tty");
				}
				/*
				 * As ptcread does after draining t_outq (tty_dev.c:668-671): l_start
				 * is ttwwakeup for TTYDISC and wakes the writers once the queue fell
				 * to its low water mark.
				 */
				tty_lock(tp);
				(*linesw[tp->t_line].l_start)(tp);
				tty_unlock(tp);
				moved = true;
			}
		}

		/* The kctl client's bytes take the same H4 framer and the same SE. */
		{
			uint32_t queued = bt_kctl_tx_drain(in, BT_KCTL_IO_SIZE);

			if (queued != 0) {
				if (g_vsc_mode == 0) {
					for (uint32_t i = 0; i < queued; i++) {
						bt_h4_tx_byte(in[i], "kctl");
					}
					if (!bt_tx_quiet(in, queued)) {
						printf("vayu-bt: kctl: %u bytes from the client "
						    "were not sent\n", queued);
					}
				} else {
					bt_vsc_tx_send(&g_vsc_kctl, in, queued, "kctl");
				}
				moved = true;
			}
		}

		n = bt_rx(in, BT_TTY_IO_SIZE, 0);
		if (n != 0) {
			bool wake_ack = false;
			uint32_t kept = bt_h4_rx_feed(in, n, out, &wake_ack);

			if (kept != 0) {
				bt_sc_rewrite(out, kept);
				bt_deliver_to_host(out, kept);
			}
			if (wake_ack) {
				uint8_t ack = BT_H4_WAKE_ACK;

				(void)bt_tx_quiet(&ack, 1);
			}
			moved = true;
		}
		if (!moved) {
			/*
			 * t_oproc() ran since the queue was read (a write, a flush, TIOCSTART):
			 * look at the queue again instead of sleeping on it.
			 */
			if (g_tty_tx_wanted) {
				g_tty_tx_wanted = false;
				continue;
			}
			delay(BT_TTY_POLL_US);
		}
	}
	thread_terminate(current_thread());
}

/*
 * Step 9: only after "step 8 done" (the controller has its firmware), make the tty and start its
 * thread, and leave everything else alone. Two devfs nodes share one dev_t, so either name opens
 * the same tty. The nodes are 0666 root:wheel because bluetoothd is not root.
 */
static bool
bt_tty_create(void)
{
	dev_t dev;

	g_tty = ttymalloc();
	if (g_tty == NULL) {
		printf("vayu-bt: tty: ttymalloc failed: no tty\n");
		return false;
	}
	g_tty_table[0] = g_tty;
	g_tty->t_line = TTYDISC;
	g_tty->t_oproc = bt_tty_oproc;
	g_tty->t_param = bt_tty_param;
	g_tty->t_timeout = 60;          /* ticks: a dead io thread must not hang a writer or a close */

	g_tty_major = cdevsw_add(-1, &bt_tty_cdevsw);
	if (g_tty_major < 0) {
		printf("vayu-bt: tty: cdevsw_add failed: no tty\n");
		goto fail;
	}
	/*
	 * spec_kqfilter() routes EVFILT_READ to EVFILTID_TTY only with this flag (spec_vnops.c:
	 * 863,872); cdevsw_add() itself leaves cdevsw_flags alone.
	 */
	if (cdevsw_setkqueueok(g_tty_major, &bt_tty_cdevsw, 0) != 0) {
		printf("vayu-bt: tty: cdevsw_setkqueueok failed: EVFILT_READ would not reach the tty\n");
		goto fail;
	}
	dev = makedev(g_tty_major, 0);
	g_tty_cu_node = devfs_make_node(dev, DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth");
	g_tty_uart_node = devfs_make_node(dev, DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666,
	    "uart.bluetooth");
	if (g_tty_cu_node == NULL || g_tty_uart_node == NULL) {
		printf("vayu-bt: tty: devfs_make_node failed: no tty\n");
		goto fail;
	}
	printf("vayu-bt: tty: /dev/cu.bluetooth and /dev/uart.bluetooth on major %d, mode 0666 (made at "
	    "start: bluetoothd may open it before the firmware is in; what it writes waits in t_outq "
	    "until step 9)\n", g_tty_major);
	return true;

fail:
	if (g_tty_cu_node != NULL) {
		devfs_remove(g_tty_cu_node);
		g_tty_cu_node = NULL;
	}
	if (g_tty_uart_node != NULL) {
		devfs_remove(g_tty_uart_node);
		g_tty_uart_node = NULL;
	}
	if (g_tty_major >= 0) {
		(void)cdevsw_remove(g_tty_major, &bt_tty_cdevsw);
		g_tty_major = -1;
	}
	g_tty_table[0] = NULL;
	ttyfree(g_tty);
	g_tty = NULL;
	return false;
}

/*
 * Step 9: only after "step 8 done" (the controller has its firmware), start the io thread on the
 * tty and the kernel control that bt_tty_create()/bt_kctl_create() made when the probe began
 * (bluetoothd starts at load and gives up when /dev/cu.bluetooth is not there), and leave
 * everything else alone. Until this thread runs nothing reads t_outq or the kctl TX ring, and
 * nothing reaches l_rint or the kctl client.
 */
static bool
bt_step9(void)
{
	thread_t thread;

	if (!g_step8_done) {
		printf("vayu-bt: step 9: step 8 did not finish: the tty stays dead\n");
		return false;
	}
	if (g_tty == NULL) {
		printf("vayu-bt: step 9: there is no tty (see the tty line at the start)\n");
		return false;
	}
	if (se_va == 0) {
		printf("vayu-bt: step 9: the SE is not mapped\n");
		return false;
	}
	if (g_baud != BT_TTY_BAUD) {
		printf("vayu-bt: step 9: the UART is at %u baud, not %u\n", g_baud, (unsigned)BT_TTY_BAUD);
		return false;
	}
	printf("vayu-bt: step 9: the H4 tty and \"%s\" for bluetoothd\n", BT_KCTL_NAME);
	if (!g_rx_running || (bt_se_rd(SE_GENI_STATUS) & SE_STATUS_S_ACTIVE) == 0) {
		printf("vayu-bt: step 9: RX is not running: starting it\n");
		if (!bt_start_rx()) {
			return false;
		}
	}
	if (kernel_thread_start(vayu_bt_io_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("vayu-bt: step 9: the io thread cannot be started\n");
		return false;
	}
	thread_deallocate(thread);

	g_step9_ok = true;
	tty_lock(g_tty);
	printf("vayu-bt: step 9: the tty and the kctl own the SE now (tty open now: %s); t_outq and "
	    "the kctl TX ring -> the controller, the controller -> l_rint and the kctl client\n",
	    (g_tty->t_state & TS_ISOPEN) ? "yes" : "no");
	tty_unlock(g_tty);
	if (g_kctl_ctlref != NULL) {
		printf("vayu-bt: step 9 done: bluetoothd with BT_HCI_TRANSPORT=H4:POSIX:%s,115200 "
		    "(or H4 BT_BAUDRATE=%u for the tty)\n", BT_KCTL_NAME, (unsigned)BT_TTY_BAUD);
	} else {
		printf("vayu-bt: step 9 done: bluetoothd with BT_HCI_TRANSPORT=H4 BT_BAUDRATE=%u "
		    "(the kctl is not registered)\n", (unsigned)BT_TTY_BAUD);
	}
	return true;
}

/* ---- the end ----------------------------------------------------------------------------------- */

static void
bt_finish(void)
{
	uint32_t given = 0;

	/*
	 * Step 9's tty and kctl read the SE from their own thread and bluetoothd is about to
	 * connect or open: nothing may be cancelled and no rail may be given back. This is
	 * vayu_bt_keep=1 with a reason.
	 */
	if (g_step9_ok) {
		printf("vayu-bt: step 9: the tty/kctl have the SE: RX and the rails stay (as "
		    "vayu_bt_keep=1)\n");
		return;
	}
	if (g_keep) {
		printf("vayu-bt: kept (vayu_bt_keep=1)\n");
		return;
	}
	if (se_va != 0 && (bt_se_rd(SE_GENI_STATUS) & SE_STATUS_S_ACTIVE) != 0) {
		printf("vayu-bt: giving RX back: cancel the S command\n");
		(void)bt_cancel_s();
	}
	/*
	 * Our votes come from the application processors' RPMh client, the same one UEFI voted from:
	 * an "enable 0" would also take away a vote UEFI may have made (run 044646 gave all three
	 * back by default). So the rails stay on unless vayu_bt_giveback=1 asks for it.
	 */
	if (!g_giveback) {
		for (unsigned i = 0; i < BT_RAIL_COUNT; i++) {
			if (bt_rails[i].voted_on) {
				printf("vayu-bt: %s stays on (vayu_bt_giveback=0)\n", bt_rails[i].id);
			}
		}
		return;
	}
	for (unsigned i = 0; i < BT_RAIL_COUNT; i++) {
		struct bt_rail *r = &bt_rails[i];
		uint32_t addr4, value = 0;

		if (!r->voted_on) {
			continue;
		}
		addr4 = r->addr + 4;
		printf("vayu-bt: give %s back: enable 0 at %#x\n", r->id, addr4);
		if (!vayu_rpmh_send(&addr4, &value, 1, "vayu-bt")) {
			printf("vayu-bt: the vote that gives %s back did not report done\n", r->id);
		}
		given++;
	}
	if (given != 0) {
		printf("vayu-bt: rails we turned on are given back\n");
	} else {
		printf("vayu-bt: no rail was turned on by us\n");
		printf("vayu-bt: rails we turned on are given back\n");
	}
}

/* ---- the probe --------------------------------------------------------------------------------- */

static void
vayu_bt_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t reached = 0;

	printf("vayu-bt: %u step(s) asked of 9, waiting %u s before step 1\n", g_level, g_wait_s);
	if (g_level >= 9) {
		(void)bt_tty_create();
		(void)bt_kctl_create();
	}
	bt_sleep_ms((uint64_t)g_wait_s * 1000);

	if (!bt_step1()) {
		goto end;
	}
	reached = 1;
	if (g_level < 2) {
		goto end;
	}
	if (!bt_step2()) {
		goto end;
	}
	reached = 2;
	if (g_level < 3) {
		goto end;
	}
	if (!bt_step3()) {
		goto end;
	}
	reached = 3;
	if (g_level < 4) {
		goto end;
	}
	if (!bt_step4()) {
		goto end;
	}
	reached = 4;
	if (g_level < 5) {
		goto end;
	}
	if (!bt_step5()) {
		goto end;
	}
	reached = 5;
	if (g_level < 6) {
		goto end;
	}
	if (!bt_step6()) {
		goto end;
	}
	reached = 6;
	if (g_level < 7) {
		goto end;
	}
	if (!bt_step7()) {
		goto end;
	}
	reached = 7;
	if (g_level < 8) {
		goto end;
	}
	if (!bt_step8()) {
		goto end;
	}
	reached = 8;
	if (g_level < 9) {
		goto end;
	}
	if (!bt_step9()) {
		goto end;
	}
	reached = 9;
end:
	bt_finish();
	printf("vayu-bt: done (reached step %u of %u)\n", reached, g_level);
	thread_terminate(current_thread());
}

void
vayu_bt_init(void)
{
	thread_t thread;
	uint32_t level = 0, value;

	if (!PE_parse_boot_argn("vayu_bt", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 9) {
		printf("vayu-bt: only steps 1..9 exist: doing 9 of %u\n", level);
		level = 9;
	}
	g_level = level;

	value = 20;
	if (PE_parse_boot_argn("vayu_bt_wait", &value, sizeof(value))) {
		g_wait_s = value;
	}
	value = 1;
	if (PE_parse_boot_argn("vayu_bt_rails", &value, sizeof(value))) {
		g_rails = value != 0;
	}
	value = 0;
	if (PE_parse_boot_argn("vayu_bt_cts", &value, sizeof(value))) {
		g_cts = value != 0;
	}
	value = 1;
	if (PE_parse_boot_argn("vayu_bt_fw_cts", &value, sizeof(value))) {
		g_fw_cts = value != 0;
	}
	value = 0;
	if (PE_parse_boot_argn("vayu_bt_ibs", &value, sizeof(value))) {
		g_ibs = value != 0;
	}
	value = 0;
	if (PE_parse_boot_argn("vayu_bt_keep", &value, sizeof(value))) {
		g_keep = value != 0;
	}
	value = 0;
	if (PE_parse_boot_argn("vayu_bt_trace", &value, sizeof(value))) {
		g_bt_trace = value != 0;
	}
	value = 0;
	if (PE_parse_boot_argn("vayu_bt_giveback", &value, sizeof(value))) {
		g_giveback = value != 0;
	}
	value = 0;
	if (PE_parse_boot_argn("vayu_bt_vsc", &value, sizeof(value))) {
		if (value > 2) {
			printf("vayu-bt: vayu_bt_vsc=%u: only 0, 1 and 2 exist; using 2\n",
			    (unsigned)value);
			value = 2;
		}
		g_vsc_mode = value;
	}
	if (g_vsc_mode != 0) {
		printf("vayu-bt: vsc: mode %u (%s); vendor opcodes are OGF 0x3f, %u rule(s)\n",
		    (unsigned)g_vsc_mode,
		    g_vsc_mode == 1 ? "table answers, unlisted pass to the chip" :
		    "table answers, unlisted get a CC status 0", (unsigned)BT_VSC_RULE_COUNT);
	}

	if (kernel_thread_start(vayu_bt_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("vayu-bt: the probe's thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}

/* The tee (see the top): the line into the loader's log, then printf's console when interrupts are on. */
#undef printf
static int
bt_printf(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}
