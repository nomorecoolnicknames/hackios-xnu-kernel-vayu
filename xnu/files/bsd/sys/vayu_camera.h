/*
 * The camera on the POCO X3 Pro (vayu, SM8150-AC): the entry points for Phase 1, steps 1-6.
 *
 * vayu_camera_init() starts a kernel thread. With boot-arg vayu_camera=1 the thread does step 1
 * only: a read-only survey of the camera's clocks, the titan_top GDSC, all four MCLKs, the 22
 * camera pins, the RPMh command-database addresses of the rails, and the PMIC regulators (type,
 * status, vset, mode, enable) over SPMI. With vayu_camera=2 it then does step 2: for each sensor
 * selected by vayu_camera_sensors it checks the pins, runs the module's power-up sequence, probes
 * the I2C address, reads the chip ID, and runs the power-down sequence. With vayu_camera=3 it does
 * step 3 instead of step 2: one frame of the CSID-lite 0's own test pattern, four virtual lanes,
 * VC 0xa, RAW10 of vayu_camera_tpg_w x vayu_camera_tpg_h (default 640x480), through RDI 0 and
 * IFE-lite 0's write master 0 into two frames of kernel memory behind the apps SMMU's IOVA
 * 0x40000000, then the frame's summary. Step 3 writes no TLMM, talks to no sensor and votes no
 * rail: vayu_camera_xnu_tlmm is not needed for it. With vayu_camera_clk=1 the camera's RCGs come
 * from camcc's PLL0 (Trion, 1200 MHz: 600 even, 400 odd) at their svs rates; the default 0 takes
 * every one of them from XO at 19.2 MHz and starts no PLL.
 *
 * With vayu_camera=4 it does step 4 after step 1: one raw frame of the rear IMX582 (the default)
 * or the front S5K3T2, chosen by vayu_camera_cam. The selected sensor's four vendor blocks (init,
 * one mode -- res3 for the IMX582, res2 for the S5K3T2 -- on, off) come from
 * fw/camera-imx582.tab or fw/camera-s5k3t2.tab ("VYCTAB01"); TLMM exception 3's amendment of
 * 2026-10-25 (maintainers; the S5K3T2 requested, see the bring-up notes) allows those four blocks only,
 * with every byte acknowledged, and no exposure, gain, OTP or EEPROM register. The IMX582's frame
 * is 1920x1080 RAW10 (stride 2400) from its res3 (four lanes, 60.08 fps); the S5K3T2's is
 * 1280x720 RAW10 (stride 1600) from its res2 (four lanes, 120.41 fps). Both go through their
 * CSIPHY -- 1 at 0x0ac66000 or 2 at 0x0ac67000, both qcom,csiphy-v1.1, settle count
 * vayu_camera_settle -- CSID-lite 0's RDI 0 from PHY 1 or 2 (lane assignment 0x3210, VC 0), and
 * IFE-lite 0's WM0 into the frames' memory behind the SMMU. Step 4 needs vayu_camera_xnu_tlmm=1
 * and always forces vayu_camera_clk to 1 (its CSIPHY timer runs from PLL0's even output); the
 * CSIPHY's supply ldoc3 is a shared rail, voted on once and never off.
 *
 * With vayu_camera=5 it makes /dev/hakios-camera for iOS's camera userland instead, with a
 * synthetic frame source: no sensor, no CSIPHY, no CSID, no IFE, no SMMU, no TLMM, no I2C and no
 * rail. Sensor 1 is the rear IMX582 (1920x1080), sensor 2 the front S5K3T2 (1280x720); the frames
 * are a colour chart in RAW10 at vayu_camera_fake_fps (default 10, 1..30). Step 1 is not run.
 *
 * With vayu_camera=6 the same node and ABI are fed by the real cameras instead: ABI sensor 1 is
 * the rear IMX582 in step 4's res3 mode, sensor 2 the front S5K3T2 in its res2 mode. A session is
 * step 4's whole bring-up (vendor tables, rails, GDSC, PLL0, clocks, ldoc3, TLMM, I2C, chip ID,
 * init and one mode, CSIPHY, CSID-lite 0's RDI 0, IFE-lite 0, stream-on), with the camera row's
 * 4x frame, exposure and gain (0x0340/0x0202/0x0204: ~15 fps rear, ~30 fps front) written at
 * every start; then IFE-lite 0's WM0 writes frame after frame into step 5's three-place ring, the
 * whole ring mapped in the apps SMMU (place p at FRAME_ADDRESSES + p * place_bytes, no header).
 * WM0 is re-armed per frame and its done bit polled every 2 ms for at most 3 frame times + 100 ms;
 * the place's cache lines are invalidated and its readers woken. The session ends after 1 s
 * without reads, the last close, a sensor command, an SMMU fault or three consecutive frame
 * timeouts, always with the module's off block and step 4's teardown. A failure makes that
 * session's reads EIO; one later session may retry, the second failed session ends the node.
 * Step 1 is not run (only GCC and camcc are mapped, from camera_ahb as step 1 does), and
 * vayu_camera_xnu_tlmm=1 is required. Exposure commands are accepted and ignored (logged once per
 * session), focus is ignored.
 *
 * vayu_camera_quiesce() is for the loader-return/panic path: if a step-2, step-4 or step-6 sensor
 * is still powered it stops the MCLKs this run started and lowers the output pins it raised (the
 * reset goes low); if step 3, 4 or 6 has the pipeline running it stops the test pattern, WM0, and
 * puts the SMMU's match register, S2CR and context back -- no I2C, no RPMh, no lock, no sleep, no
 * polling, one line each. It does nothing when nothing is running.
 *
 * Boot-args:
 *   vayu_camera=1|2|3|4|5|6  step 1; steps 1-2; steps 1 and 3 (no TLMM, no sensor); steps 1 and
 *                             4; step 5 only (the node and the synthetic frames, no step 1); step
 *                             6 only (the same node fed by the real cameras, no step 1).
 *   vayu_camera>6             "only steps 1-6 exist", and step 6 is done.
 *   vayu_camera_fake_fps=N    step 5's frames a second, default 10 (1..30).
 *   vayu_camera_cam=0|1       step 4's camera: 0 = rear IMX582 (default), 1 = front S5K3T2;
 *                             anything else says so and uses 0.
 *   vayu_camera_sensors=MASK  bit i = sensor i of the table below, default 0x1, order 0..4 (step 2).
 *   vayu_camera_xnu_tlmm=1    required for steps 2, 4 and 6 (TLMM writes); without it step 1 runs.
 *   vayu_camera_shared=0      do not vote the shared rails (default 1: vote on once, never off).
 *   vayu_camera_pause=MS      before the first write of step 2, 3 or 4, default 2200 ms (step 6 uses 0).
 *   vayu_camera_hold=MS       stay powered after the step-2 ID read, default 0.
 *   vayu_camera_clk=1         camcc PLL0 up and the svs rates; default 0: XO only (steps 4 and 6 force 1).
 *   vayu_camera_settle=N      steps 4 and 6's CSIPHY settle count, default 0x20 (a HYPOTHESIS).
 *   vayu_camera_frames_ms=N   step 4's wait for WM0's done, default 2000 ms (step 6: 3 frame times + 100 ms).
 *   vayu_camera_tpg_w=W       step 3's test pattern width, default 640 (16..4096).
 *   vayu_camera_tpg_h=H       its height, default 480 (16..4096).
 *
 * The safety rules: a shared rail is never voted off; only ldoc1 is voted both ways; TLMM is
 * written only in cfg bit 9 and io bit 1 of the 22 listed pins, after the loader set them up;
 * every check is a read before the matching write; the power-down always runs. Step 4 writes only
 * the selected module's vendor init, one mode block and stream-on/off, a NACK ends the block, and
 * the off block (0x0100 = 0) is written before any power-down once init was attempted. The
 * IMX582 is covered by the 2026-10-25 amendment; the S5K3T2 is requested (see the bring-up notes); no
 * other sensor register is written; step 6 adds only 0x0340/0x0202/0x0204 from the camera row, at
 * every session start. Steps 3, 4 and 6 are bounded everywhere
 * (1 ms for PLL0's lock, 500 us for an RCG or a branch, 10 ms for the SMMU's TLB,
 * vayu_camera_frames_ms for step 4's frame, 3 frame times + 100 ms for step 6's, 100 ms for a
 * table delay), leave a branch that was on already on, refuse a PLL0 somebody else runs, and leave
 * an SMMU match register that already covers the IFEs' streams alone.
 *
 * The hardware, with its sources:
 *
 *   GCC         base 0x00100000, 0x80000 bytes. The camera AHB CBCR is at 0xb008 and the camera XO
 *               CBCR at 0xb044, both CLK_IS_CRITICAL (mainline gcc-sm8150.c:1252-1257, :1295).
 *               CBCR bit 0 is enable, bit 31 "clock off". camcc is read only when 0xb008 is
 *               enabled or not off: its registers sit behind that clock.
 *   camcc       base 0x0ad00000, 0x10000 bytes (mainline sm8150.dtsi:3766-3768). titan_top GDSC
 *               at 0xc1bc (camcc-sm8150.c:1883-1891): bit 0 SW_COLLAPSE, bit 31 PWR_ON. MCLKn's
 *               RCG command is 0x5004 + 0x20*n (vendor camcc-sm8150.c:1181, :1201, :1221, :1241),
 *               cfg at +4, M at +8, N at +0xc, D at +0x10 (source 0 = BI_TCXO, 19.2 MHz;
 *               :1172-1178); MCLKn's CBCR is 0x501c + 0x20*n (:2191, :2209, :2227, :2245).
 *               RCG cfg [4:0] src_div = 2*div - 1, [10:8] src_sel, [13:12] mode (0 = bypass);
 *               command bit 0 UPDATE, bit 31 ROOT_OFF. Step 4's csiNphytimer RCG is 0x6028
 *               (CSIPHY1) or 0x604c (CSIPHY2), source 2 = PLL0 even, field 3 = /2, 300 MHz
 *               (camcc-sm8150.c:651-656, :678, :698), with its branch CBCR 0x6040/0x6064
 *               (:1511, :1529) and csiphy1/2's 0x6044/0x6068 (:1583, :1601).
 *   CSIPHY1/2   0x0ac66000 / 0x0ac67000, 0x1000 bytes each, qcom,csiphy-v1.1 (vendor
 *               sm8150-camera.dtsi:47-72, :75-100). Both PHYs' mipi-csi-vdd is PM8150L L3 (ldoc3,
 *               1.2 V), shared with the DSI PLL. The v1.1 tables are ref/cam_csiphy_1_1_hwreg.h
 *               (reset, common, lane, irq; the down path is the reset again) and are the same for
 *               every instance; the core's order is cam_csiphy_core.c's. CSID's RX phy select is
 *               the CSIPHY's index (INFERENCE: camera_v3 cam_ife_csid_core.c:831).
 *   TLMM        cfg at tile + 0x1000*pin: [1:0] pull, [5:2] function, [8:6] drive, [9] OE; io at
 *               +4: bit 0 in, bit 1 out. Tiles NORTH 0x03900000, EAST 0x03500000, SOUTH
 *               0x03d00000, WEST 0x03100000 (pinctrl-sm8150.c:28-31). Pins and roles: NORTH
 *               13-16 MCLK0-3, 17-20/31-34 CCI, 84 hi259 standby, 94 VANA-EN; EAST 23/27/28/30
 *               resets of ov02b1b/hi259/ov8856/imx582; SOUTH 54 s5k3t2 reset, 59 imx582 VDIG-EN,
 *               118 s5k3t2 VDIG-EN; WEST 125 ov02b1b standby (vendor
 *               vayu-sm8150-camera-sensor-mtp.dtsi:156-503).
 *   rails       vendor sm8150-regulator.dtsi: ldoc1 = PM8150L L1, 1.8 V, a qcom,rpmh-xob-regulator
 *               used only by the cameras (:712-716), exclusive; smpa5 = PM8150 S5, VANA of imx582;
 *               smpc8 = PM8150L S8, VDIG and UFS VCCQ (shared); bobc1 = BOB, shared with the audio;
 *               ldoc3 = PM8150L L3, 1.2 V, the CSIPHY's mipi-csi-vdd, shared with the DSI PLL (voted
 *               1 once, never off). The enable vote is 1/0 at vayu_rpmh_address(id) + 4 (vendor
 *               rpmh-regulator.c:95-101); voltage is not voted.
 *
 * The module table (vendor sensor modules com.qti.sensormodule.vayu_*.bin, TheMuppets
 * proprietary_vendor_xiaomi_vayu @56e4960; DT node @N):
 *
 *   i  name      bus SDA/SCL  addr7  id reg  id      MCLK  reset  standby  vdig  vana  VANA   VDIG  CUSTOM2
 *   0  imx582    19/20        0x10   0x0016  0x0582  1     30     -        59    -     smpa5  smpc8 bobc1
 *   1  ov02b1b   33/34        0x3c   0x0203  0x002b  2     23     125      -     94    bobc1  -     -
 *   2  s5k3t2    17/18        0x10   0x0000  0x3142  0     54     -        118   -     bobc1  smpc8 -
 *   3  ov8856    33/34        0x36   0x300b  0x885a  3     28     -        -     94    bobc1  smpc8 -
 *   4  hi259     33/34        0x38   0x04    0xe1    3     27     84       -     94    bobc1  -     -
 * (TLMM exception 3 excludes pins 27, 94, 125: hi259 is not probed, ov02b1b is ACK-probed only.)
 *
 * The CCI pins are bit-banged as in vayu_audio_i2c_part.c: 100 kHz, clock stretching, a 5 ms
 * deadline per transaction, the pins given back exactly as taken.
 */

#ifndef _SYS_VAYU_CAMERA_H_
#define _SYS_VAYU_CAMERA_H_

void vayu_camera_init(void);
void vayu_camera_quiesce(void);

#endif /* _SYS_VAYU_CAMERA_H_ */
