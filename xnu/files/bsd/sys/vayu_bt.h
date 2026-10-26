/*
 * Bluetooth's UART on the POCO X3 Pro (vayu, SM8150), Phase 1 steps 1..9.
 *
 * This header declares the one entry point of bsd/dev/vayu_bt.c: the probe that looks at QUP
 * wrapper 2 / SE3 (the Bluetooth UART) read-only, brings the GENI UART up in FIFO mode at 2400
 * 8N1, checks the pins, votes the three Bluetooth rails, sends the QCA power pulses, reads the
 * SoC version, speaks HCI, downloads the controller's firmware at 115200, speaks HCI again, and
 * finally hands the UART to iOS as an H4 tty (/dev/cu.bluetooth).
 *
 * What each step does:
 *   1  read only: the GCC vote and the wrapper/SE3 halts, SE3's RCG and its eight DFS levels,
 *      QUPV3_HW_VER, the SE's GENI/UART registers and FIFO depths, GPIO 43-46, and the four
 *      PMIC regulators (pm8150 S4A/L7A, pm8150l L2C/L11C) through the SPMI arbiter. When the
 *      RCG is not in DFS (the run-031412 case: the DFS word's bit 0 clear), the one rate in
 *      the RCG's CFG/M/N words is computed and stored as level 0.
 *   2  the SE3 clock check, then the vendor geni_se_init / FIFO / port_setup sequence and a
 *      2400 8N1 baud rate from a DFS level of the SE3 RCG; RX is started.
 *   3  read only: all four pins must be function 1 (qup13); CTS is printed.
 *   4  the PMIC readout again; ldoa7, ldoc2 and ldoc11 are voted through RPMh when their
 *      regulator reads off or could not be read at all (the apps arbiter has no channel for
 *      them). A rail whose state is unknown is voted with vayu_bt_rails=1; S4A is only read
 *      and never stops the step.
 *   5  the QCA power pulses: 0xc0 at 2400 baud, 0xfc at 115200, then whatever came back.
 *   6  the version request 01 00 fc 01 19 and the 0xff/00 02 answer (product, patch, rom, soc,
 *      fw_rom and the crbtfw/crnv names).
 *   7  HCI_Reset, Read_Local_Version and Read_BD_ADDR.
 *   8  the QCA firmware download at 115200: the rampatch crbtfw21.tlv (or crbtfw%02x.tlv of
 *      the fw_rom step 6 named) in 243-byte H4 segments, then the patched crnv21.bin, then
 *      HCI_Reset, Read_Local_Version and Read_BD_ADDR again.
 *   9  the tty and the kernel control: one struct tty (ttymalloc) behind two devfs nodes on one
 *      dev_t, /dev/cu.bluetooth and /dev/uart.bluetooth, 0666 root:wheel, and the kernel control
 *      "com.vayu.bt.uart" (made with the tty nodes when the probe starts: a PF_SYSTEM/SOCK_STREAM
 *      kernel control that bluetoothd's H4:POSIX driver connects to; 16 KB each way, no root,
 *      one client). One kernel thread from then on owns the SE: it drains t_outq and the kctl TX
 *      ring into bt_tx_quiet() and feeds bt_rx() through the H4 framer into the line discipline
 *      and to the kctl client. The tty is raw (iflag/oflag/lflag 0, CS8|CREAD|CLOCAL, 115200) and
 *      t_param accepts any speed without touching the UART; the kctl answers TIOCGETA with its
 *      current termios (initially the same raw one) and stores TIOCSETA/TIOCSETAW/TIOCSETAF
 *      without touching it either. The step returns with RX and the rails still running, exactly
 *      as vayu_bt_keep=1 would, because bluetoothd opens the tty (plain H4) or connects the kctl
 *      (H4:POSIX) next. The first 200 HCI packets in each direction are traced (TX says whether
 *      tty or kctl), and 0xfd/0xfe/0xfc at a packet boundary are filtered (0xfd is answered with
 *      0xfc). With vayu_bt_vsc=1|2 the same outbound path first reassembles complete H4 packets
 *      and lets the kernel answer selected vendor opcodes (OGF 0x3f) with a synthetic event;
 *      standard commands still go to the controller unchanged.
 *
 * What userland runs for step 9:
 *
 *     BT_HCI_TRANSPORT=H4:POSIX:com.vayu.bt.uart,115200 bluetoothd
 *
 * That is bluetoothd's own POSIX HCI driver: it looks the kernel control's name up with
 * CTLIOCGINFO, connects a PF_SYSTEM/SOCK_STREAM socket to it, gets and sets the termios with
 * TIOCGETA and TIOCSETA, then speaks plain H4 on the socket. Plain BT_HCI_TRANSPORT=H4
 * BT_BAUDRATE=115200 still goes through the tty. bluetoothd also asks BlueTool to power the
 * controller; a stand-in that answers result = 0 (an XPC dictionary whose "result" key is an
 * int64 0) is enough (FUN_1002722c8).
 *
 * What it may write: SE3 registers inside 0x00c8c000..0x00c8cfff, never offset 0x1000 or above
 * (an SE's 0x2000+ window is TrustZone-only), and RPMh VRM votes for ldoa7, ldoc2 and ldoc11
 * through vayu_rpmh_send(). It writes no TLMM and no GCC register. The SPMI arbiter's read
 * command is the one other store, and it is the only way to read a PMIC register. After step 9
 * the SE is written only by the tty/kctl io thread (through bt_tx_quiet/bt_rx and, for a 0xfd,
 * one 0xfc); the tty's and the kctl's own ioctls only touch memory, never the UART.
 *
 * Boot-args: vayu_bt=N (0/absent: nothing; steps 1..N in order, at most 9, stopping at the
 * first failure); vayu_bt_wait=S seconds before step 1 (default 20); vayu_bt_rails=0|1
 * (default 1: vote a rail that the readout shows off or cannot read); vayu_bt_cts=0|1
 * (default 0: ignore CTS in steps 5..7); vayu_bt_fw_cts=0|1 (default 1: honour CTS for the
 * step 8 download); vayu_bt_ibs=0|1 (default 0: patch the NVM's tag 17 with in-band sleep off
 * and tag 27 with deep sleep off; 1 leaves both on as upstream btqca does);
 * vayu_bt_vsc=0|1|2 (default 0: the outbound vendor-command shim; 1 answers the host commands
 * listed in bt_vsc_rules and passes every other vendor opcode, OGF 0x3f, to the controller;
 * 2 also answers an unlisted vendor opcode with a synthetic Command Complete status 0; a
 * standard command, OGF != 0x3f, always passes to the chip unchanged);
 * vayu_bt_keep=0|1 (default 0: cancel RX; vayu_bt_giveback=1 also gives back the rails this run
 * turned on; 1: leave them; step 9 always leaves them).
 *
 * Run 031412, the first hardware run, found (and this file now assumes):
 *   - SE3's RCG is not in DFS: the DFS word (cmd + 0x14) reads 0x8020, bit 0 clear, and all
 *     eight DFSR/M/N words read 0. The bt2 loader programs the RCG itself to 7 372 800 Hz
 *     (cfg source 6, hid 1, mode 2; M 384, N ~(15625 - 384)). Step 1 therefore reads the one
 *     rate from the RCG's CFG (cmd + 4), M (cmd + 8) and N (cmd + 0xc), stores it as level 0
 *     and marks levels 1..7 invalid.
 *   - The PMIC LDOs are not readable through the apps arbiter: ppids 0x1d, 0x46, 0x441 and
 *     0x44a answer "no channel of the arbiter". Step 4 treats such a rail as unknown and votes
 *     it (with vayu_bt_rails=1); a readout failure is no longer a reason to stop.
 *   - Step 2 worked: the GENI UART came up at 2400 baud with divider 250 from 19.2 MHz; the
 *     FIFO depths are TX 16 words and RX 32 words.
 *
 * The facts, with their sources:
 *
 *   the node    stock vayu.dts:23389-23405 "qcom,qup_uart@0xc8c000", compatible
 *               "qcom,msm-geni-serial-hs", alias hsuart0 (:27591). Its wrapper is
 *               "qcom,qupv3-geni-se" at 0xcc0000 (:23650-23662): QUP wrapper 2, SE3
 *               (the bring-up notes). SE registers: 0xc8c000, 0x4000 bytes.
 *   the GENI    vendor drivers/tty/serial/msm_geni_serial.c and qcom-geni-se.h (vayu-r-oss):
 *               0x040 SE_GENI_STATUS, 0x068 GENI_FW_REVISION_RO (bits 15:8 = protocol: 1 SPI,
 *               2 UART, 3 I2C), 0x07c SE_GENI_CLK_SEL, the FIFO and UART registers 0x258..0x2ac
 *               and 0x600..0xe30, the same offsets as bsd/dev/vayu_qup_spi.c. UART_START_TX
 *               (M opcode 1) and UART_START_READ with UART_PARAM_RFR_OPEN 0x80.
 *   the clocks  GCC at 0x100000 (as vayu_qup_spi.c GCC_PHYS). Vote word 0x52014: bit 1 wrap 2
 *               s_ahb, bit 2 m_ahb (vayu_qup_spi.c:79-81), bit 7 wrap 2 SE3
 *               (gcc-sm8150.c:2453-2458; VAYU_BT_PHASE0.md §1). Wrapper AHB halt registers
 *               0x1e008 and 0x1e004 (vayu_qup_spi.c:84-85), SE3's branch 0x1e4d4
 *               (gcc-sm8150.c:2454), its RCG cmd 0x1e4d8 / cfg 0x1e4dc (:731-732, cfg = cmd + 4
 *               as vayu_qup_spi.c:88), DFS word cmd + 0x14 (as vayu_qup_spi.c:89) and the eight
 *               DFS levels at cmd + 0x1c + 4*i (DFSR), + 0x5c + 4*i (M) and + 0x9c + 4*i (N);
 *               gcc-sm8150.c:4188 and clk-rcg2.c. DFSR: src [10:8], hid [4:0], mode [13:12];
 *               gcc_parent_map_0 has XO 19.2 MHz (0), GPLL0 600 MHz (1) and GPLL0_EVEN 300 MHz
 *               (6); other sources are skipped. Rate = parent, then hid and M/N as clk-rcg2.c.
 *               With DFS off, the same fields come from CFG (cmd + 4), M (cmd + 8) and N
 *               (cmd + 0xc), and that one rate is level 0 (run 031412).
 *               Branch halt bit 31 = the clock is off (vayu_qup_spi.c:92).
 *   the pins    mainline pinctrl-sm8150.c:1356-1359: GPIO 43-46, EAST tile, function 1 =
 *               qup13. Tile base 0x03500000 (sm8150.dtsi:2381-2385), one 0x1000-byte block per
 *               pin (ctl_reg = 0x1000 * id), cfg at +0x0 and io at +0x4, as the other vayu
 *               drivers read the TLMM. GPIO43 CTS, 44 RTS, 45 TX, 46 RX (stock vayu.dts:
 *               11537-11620; the phone's live DT recovery-live.dts:11524-11600), so cfg
 *               0x0352b000..0x0352e000: one 0x4000 mapping at 0x0352b000. Cfg bits as
 *               vayu_audio.c's show_pins() decodes them: 1:0 pull, 5:2 function, 8:6 drive
 *               ((n+1)*2 mA), 9 output enable; io bit 0 = input, bit 1 = output.
 *   the rails   stock vayu.dts and the phone's live DT recovery-live.dts:27906-27918: pm8150
 *               S4A (ppid 0x01d) = vddio 1.8 V, pm8150 L7A (0x046) = the BT vdd-core/xtal rail
 *               1.8 V (RPMh ldoa7), pm8150l L2C (0x441) = vdd-pa 1.304 V (ldoc2), pm8150l L11C
 *               (0x44a) = vdd-ldo 3.312 V (ldoc11). Registers via the SPMI arbiter: 0x04-0x05
 *               type/subtype, 0x08 status1 (bit7 VREG_OK, a hypothesis), 0x40-0x41 vset mV
 *               little-endian, 0x45 mode, 0x46 en_ctl (bit7 enabled). RPMh (vendor
 *               rpmh-regulator.c): cmd-db address +0 mV, +4 enable, +8 mode (PMIC5 LDO 7 = HPM).
 *   the power   upstream hci_qca.c (qca_regulator_init / qca_send_power_pulse), btqca.c
 *               (qca_read_soc_version / qca_soc_ver, qca_tlv_check_data / qca_tlv_send_segment)
 *               and vendor bluetooth-power.c: 0xc0 at 2400, 0xfc at 115200; 0xfd/0xfc/0xfe are
 *               in-band sleep bytes. The version request 01 00 fc 01 19 and its 0xff/00 02
 *               answer. Step 8's rampatch is the TLV type 1 (total_size/data_length/format/
 *               signature/download_mode/product_id/rom_build/patch_version/entry at offsets
 *               4..27) and its data goes out as 01 00 fc (seg + 2) 1e seg; the NVM is the
 *               multi TLV type 4 around a type 2 tag set (tag: id le16, len le16, 8 reserved,
 *               data), with tag 17 patched to stay at 115200 and tag 27 for deep sleep; with
 *               vayu_bt_ibs=0 (the default) the in-band-sleep bit (tag 17 data[0] bit 7) and the
 *               deep-sleep bit (tag 27 data[0] bit 0) are cleared, with 1 they are set as
 *               upstream does.
 *   the tty     iOS 16.5's bluetoothd reads BT_HCI_TRANSPORT (one transport name, e.g.
 *               H4; user space work from its strings), probes access("/dev/cu.bluetooth") and then "/dev/uart.bluetooth",
 *               opens the tty, does tcgetattr / cfmakeraw / cfsetspeed / tcsetattr (TIOCGETA
 *               0x40487413, TIOCSETA 0x80487414) and reads it through EVFILT_READ; it speaks
 *               plain H4 (01 command, 02 ACL, 03 SCO, 04 event). Both names are one dev_t and
 *               one struct tty; step 9's thread is the only SE user from then on.
 *   the kctl    iOS 16.5's bluetoothd POSIX HCI driver (FUN_10006669c) does not open the tty.
 *               With BT_HCI_TRANSPORT=H4:POSIX:<name>,<baud> it makes socket(AF_SYSTEM 32,
 *               SOCK_STREAM 1, SYSPROTO_CONTROL 2), ioctl(CTLIOCGINFO 0xc0644e03,
 *               {ctl_name = name}), connect(sockaddr_ctl{sc_len 32, sc_family 32,
 *               ss_sysaddr 2, sc_id, sc_unit = 0}), getsockopt(2, TIOCGETA 0x40487413,
 *               &termios, &len = 0x48), cfmakeraw/cfsetspeed, setsockopt(2, TIOCSETA
 *               0x80487414, &termios, 0x48), and then reads/writes plain H4 on the socket. The
 *               kernel control is "com.vayu.bt.uart": CTL_FLAG_REG_SOCK_STREAM, 16 KB each way,
 *               no CTL_FLAG_PRIVILEGED (bluetoothd runs as mobile), one client at a time. Its
 *               TIOCGETA/TIOCSETA* only answer and store the termios; the UART stays at 115200.
 *   the blue    bluetoothd asks BlueTool to power the controller before it uses HCI. Until that
 *               daemon exists, a stand-in that answers result = 0 (an XPC dictionary whose
 *               "result" key is an int64 0) is enough (FUN_1002722c8 in the 16.5 decompile).
 *   the kqueue  EVFILT_READ on a character device reaches the tty filter only when
 *               d_type == D_TTY *and* cdevsw_flags[major] has CDEVSW_SELECT_KQUEUE: see
 *               spec_kqfilter (spec_vnops.c:831-885, the flag at :863 and the tty branch at
 *               :872), cdevsw_setkqueueok (bsd_stubs.c:287-305, the only setter of that flag),
 *               and filt_ttyattach (tty.c:3368-3415), which needs the driver's d_select to
 *               selrecord on tp->t_rsel under FREAD|FMARK (ttyselect, tty.c:1532-1593).
 *               Without the flag a plain EVFILT_READ is refused by EVFILTID_SPEC's attach
 *               (spec_vnops.c:3022-3027).
 *
 * Nothing happens unless the boot-arg vayu_bt=1 or above is present. The probe runs in a kernel
 * thread, like vayu_haptic_init(). Every wait has a timeout; on one the registers are printed and
 * the step chain stops. Reading an SE register needs the wrapper's AHB path, so m_ahb's halt bit
 * and s_ahb (clear, or hardware-gated through bit 1) are checked before any SE access; a
 * hardware-gated s_ahb keeps bit 31 set while idle and still counts as running, as the loader
 * and bsd/dev/vayu_qup_spi.c treat it.
 *
 * After step 9 the probe's thread ends but the tty/kctl's io thread stays (a kernel thread,
 * polling once a millisecond when idle), and the rails and RX stay up; the SE is never cancelled.
 *
 * This header's only export is vayu_bt_init().
 */

#ifndef _SYS_VAYU_BT_H_
#define _SYS_VAYU_BT_H_

/*
 * The Bluetooth probe: a no-op without vayu_bt=1, otherwise one thread that runs steps 1..N,
 * prints what each one finds and at the end gives back the rails it turned on (unless
 * vayu_bt_keep=1). Step 9 leaves a tty (/dev/cu.bluetooth, /dev/uart.bluetooth), the kernel
 * control "com.vayu.bt.uart" and the thread that serves both, and at the end of that chain
 * nothing is cancelled or given back.
 */
extern void vayu_bt_init(void);

#endif /* _SYS_VAYU_BT_H_ */
