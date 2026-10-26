/*
 * The camera on the POCO X3 Pro (vayu, SM8150-AC): Phase 1's step 1 (a read-only survey of the
 * camera's clocks, pins, rails and PMICs), step 2 (one sensor of the module table powered up,
 * probed, its chip ID read, and powered down again), step 3 (one frame of the CSID's test
 * pattern through IFE-lite 0 into memory, with no sensor at all), step 4 (one raw frame of
 * either camera: the rear IMX582 or the front S5K3T2, chosen by vayu_camera_cam, through its
 * vendor tables, its CSIPHY, CSID-lite 0's RDI 0, IFE-lite 0's WM0 and memory), step 5:
 * /dev/hakios-camera for iOS's camera userland, fed by a synthetic colour chart (no hardware
 * at all), and step 6: the same node and ABI fed by the real cameras.
 *
 * Step 1 writes no configuration register: every access is a 32-bit load, except the PMIC
 * arbiter's read-command word in its observer channel -- the same store vayu_haptic.c makes and
 * the only way to read a PMIC register. It prints the GCC camera clocks, the camcc titan_top
 * GDSC and all four MCLKs, the 22 camera pins, the RPMh command-database addresses of the rails,
 * and a read-only survey of the PMIC regulators (type, status, vset, mode, enable).
 *
 * Step 2 is the first part of this file that writes anything, and only these registers:
 *   - camcc: the titan_top GDSCR (0xc1bc) and, per MCLKn, the RCG command/cfg and the CBCR;
 *   - TLMM: cfg bit 9 (output enable) and io bit 1 (output level) of the pins listed below,
 *     after the loader has set them up (the maintainers' narrow exception, 2026-10-24);
 *   - the PMIC rails, through RPMh only (vayu_rpmh_send(): one enable vote per rail).
 * No sensor register is ever written: only an address-phase probe and a register read.
 *
 * What step 2 does, per sensor, in order 0..4 of the table below:
 *   1. its pins are checked, read-only: reset, standby, VDIG-EN and VANA-EN must be function 0
 *      with OE 1, SDA/SCL function 0 with pull-up, MCLK function 1; otherwise it is skipped;
 *   2. the module's power-up sequence (VIO, VANA, VDIG, CUSTOM1, CUSTOM2, MCLK, RESET, STANDBY),
 *      one line per step;
 *   3. an I2C probe (start, address, ACK, stop), then, if acknowledged, one register read;
 *   4. the power-down sequence, always, undoing only the steps that were done;
 *   5. after all the sensors: the titan_top GDSC off, if this run turned it on, and a summary.
 *
 * Step 3 is the first part of this file that drives the camera subsystem proper, and it does so
 * without a sensor: CSID-lite 0's own test pattern generator stands in for one. It writes
 *   - camcc: the titan_top GDSCR (0xc1bc), the five RCGs that carry a frame (slow_ahb 0x7058,
 *     camnoc_axi 0xc170, cphy_rx 0xa064, ife_lite_0_csid 0xc020, ife_lite_0 0xc004) and their seven
 *     branches, and, only with vayu_camera_clk=1, PLL0 (Trion, camcc + 0x0) with its configuration;
 *   - GCC: the camera's two AXI branches (0xb030, 0xb034);
 *   - the apps SMMU (0x15000000): one free match register and one free context, put back as found;
 *   - CSID-lite 0 (0x0acc8000) and IFE-lite 0 (0x0acc4000), with redwood's values.
 * No TLMM pin, no CCI transaction, no rail vote, no PMIC and no CSIPHY is touched: the test pattern
 * needs none of them, and vayu_camera_xnu_tlmm is not needed.
 *
 * The frame goes to two frames' worth of contiguous kernel memory behind the SMMU's IOVA
 * 0x40000000, with a 4 KB header ("RWCAM001") that the loader hands to the host
 * (loader_recording), and WM0's done bit (bus IRQ status1 0x2060 bit 0) is polled every 10 ms for
 * 2 s. The frame is then unpacked as MIPI RAW10 (four pixels in five bytes) and its histogram is
 * printed. The frame memory is kept after the teardown: the loader's is the only way out.
 *
 * Step 4 is the first part that talks to a sensor through its own register tables, for either
 * camera of the phone (vayu_camera_cam=0, the default, is the rear IMX582; 1 is the front
 * S5K3T2). It writes
 *   - the same rails and camcc/GCC clocks as steps 2 and 3, plus camcc's csiNphytimer RCG
 *     (0x6028 for CSIPHY1, 0x604c for CSIPHY2; PLL0 even /2 = 300 MHz) and its two branches
 *     (csiphyN's CBCR 0x6044/0x6068, the timer's 0x6040/0x6064), always with PLL0 up:
 *     vayu_camera_clk is forced to 1 for step 4;
 *   - PM8150L L3 (ldoc3, 1.2 V), the CSIPHY's mipi-csi-vdd, as a shared rail: voted on once and
 *     never off, because the DSI PLL uses it too;
 *   - TLMM's outputs of the selected sensor and its CCI pins, then I2C to the module's four
 *     vendor blocks from fw/camera-imx582.tab or fw/camera-s5k3t2.tab (init, the mode block --
 *     res3/res2 -- on, off). TLMM exception 3's amendment of 2026-10-25 (maintainers; amendment 2 of
 *     the same day for the S5K3T2): only those four blocks of the selected module, off is
 *     0x0100 = 0, and no exposure, gain, OTP or EEPROM register is ever written;
 *   - CSIPHY1 (0x0ac66000) or CSIPHY2 (0x0ac67000), both qcom,csiphy-v1.1, with the v1.1
 *     sequence of ref/cam_csiphy_1_1_hwreg.h: reset, common, the five lane positions, the
 *     interrupt masks; the sequence is the same for every CSIPHY instance;
 *   - CSID-lite 0's RX from PHY 1 or 2 (four lanes, lane assignment 0x3210, VC 0, RAW10) and its
 *     RDI 0, IFE-lite 0's WM0 into the frames' memory behind the SMMU.
 * The rear frame is 1920x1080 RAW10 (stride 2400), its res3: 4 lanes, 60.08 fps; the front's is
 * 1280x720 RAW10 (stride 1600), its res2: 4 lanes, 120.41 fps. WM0's done bit is polled every
 * 10 ms for vayu_camera_frames_ms; the teardown always runs, with the sensor's off block before
 * its power-down. The CSIPHY's settle count is vayu_camera_settle (default 0x20, a HYPOTHESIS:
 * the module's value is not known; 0x20 at a 300 MHz timer is about 107 ns, inside the D-PHY's
 * 85 ns + 6 UI .. 145 ns + 10 UI).
 *
 * Step 5 is the ABI of sys/hakios.camera.h on vayu: a character device /dev/hakios-camera
 * whose reads hand iOS the newest ready frame and whose writes take exposure, sensor and focus
 * commands. The frames are a synthetic colour chart (the reference's fake_line()/fake_fill():
 * eight bars and a grey ramp in RAW10, in the sensor's Bayer order, as bright as the exposure
 * asked for), made in a kernel thread at vayu_camera_fake_fps (default 10, 1..30): sensor 1 is
 * the rear IMX582 at 1920x1080, sensor 2 the front S5K3T2 at 1280x720. There is no sensor, no
 * CSIPHY, no CSID, no IFE, no SMMU, no TLMM, no I2C, no rail and no RPMh access in step 5. The
 * per-sensor timing and gain values in cam_fake_sensors[] are HYPOTHESES for this synthetic
 * source only; step 6's real ones are in cam_real_sensors[]. Three places of contiguous kernel
 * memory hold the ring. A session starts with the first read, ends with the last close, a
 * second without a read, or a sensor command; its log is one line at the start and one at the
 * end, never one per frame.
 *
 * Step 6 is step 5's node and ABI with the synthetic source replaced by the real cameras: ABI
 * sensor 1 is the rear IMX582 in cam_streams[0]'s mode (res3), sensor 2 the front S5K3T2 in
 * cam_streams[1]'s (res2). A session is step 4's bring-up -- the tab load, the rails, the GDSC,
 * PLL0, the clocks, ldoc3, the pins, the module's power-up, the chip ID (MATCH required), the
 * vendor init and one mode block, then 0x0340/0x0202/0x0204 ALWAYS with the camera row's 4x
 * frame and its exposure and gain (the fixed exposure: ~15.02 fps rear, ~30.10 front), the
 * SMMU the whole ring, CSID-lite 0 with RDI 0 running, CSIPHY, IFE-lite 0 and the vendor on
 * block -- and then a continuous capture loop. The ring is step 5's three places, the whole of
 * it mapped in the apps SMMU: place p is IOVA FRAME_ADDRESSES + p * place_bytes, no header in
 * front, so the frame bytes start at the place. Every frame: the oldest FREE place is marked
 * WRITING, WM0_IMAGE_ADDR gets its IOVA, WM0 is on, IFE_REG_UPDATE is 0x2 and the bus's status1
 * bit 0 is cleared; the loop sleeps 2 ms at a time for it, at most 3 frame times + 100 ms,
 * invalidates the place's cache lines, marks it READY with the next sequence and the times and
 * wakes the readers. Every 100 frames one line; an SMMU fault or three consecutive frame
 * timeouts ends the session. The session ends after 1 s without reads, the last close, a
 * SENSOR command or a failure: the vendor off block and its 30 ms, then step 4's teardown order,
 * then one line with the counts. A failure tears everything down and makes the session's reads
 * EIO, but not the node: one later session may retry, and the second failed session ends it.
 * Step 1 is NOT run for 6 (as for 5), except GCC and camcc are mapped and g_camcc_ok is set from
 * camera_ahb exactly as step 1 does; vayu_camera_xnu_tlmm=1 is required (as for step 4) and
 * vayu_camera_clk is forced to 1 (the CSIPHY timer runs from PLL0). EXPOSURE commands are
 * accepted and ignored (logged once per session), FOCUS is ignored. Step 1-5 behaviour and log
 * lines are unchanged: cam_stream_up()/cam_stream_down() are step 4's bring-up and teardown
 * factored out, and the node, ring, read, write and session code is step 5's with the frame source
 * pluggable.
 *
 * The sensor table (module powerSetting; vendor sensor modules
 * com.qti.sensormodule.vayu_*.bin, TheMuppets proprietary_vendor_xiaomi_vayu @56e4960; the DT
 * index is the node @N):
 *
 *   i  name      bus SDA/SCL  addr7  id reg  id      MCLK  reset  standby  vdig  vana  VANA   VDIG  CUSTOM2
 *   0  imx582    19/20        0x10   0x0016  0x0582  1     30     -        59    -     smpa5  smpc8 bobc1
 *   1  ov02b1b   33/34        0x3c   0x0203  0x002b  2     23     125      -     94    bobc1  -     -
 *   2  s5k3t2    17/18        0x10   0x0000  0x3142  0     54     -        118   -     bobc1  smpc8 -
 *   3  ov8856    33/34        0x36   0x300b  0x885a  3     28     -        -     94    bobc1  smpc8 -
 *   4  hi259     33/34        0x38   0x04    0xe1    3     27     84       -     94    bobc1  -     -
 *
 * (The two register-address and data sizes are 2/2 for 0-3 and 1/1 for hi259, as the modules say.)
 * TLMM exception 3 (maintainers, 2026-10-24, the bring-up notes) leaves out 27, 94 and 125,
 * which have a second owner in the phone's live DT: hi259 (reset 27) is not probed; ov02b1b runs
 * without its standby (125) and VANA-EN (94), and with the ACK probe only (its 2-byte id register
 * 0x0203 would be a register write on an 8-bit OV02B); ov8856 runs without VANA-EN (94).
 *
 * Boot-args:
 *   vayu_camera=1             step 1 only (reads).
 *   vayu_camera=2             step 1, then step 2.
 *   vayu_camera=3             step 1, then step 3 (step 2 is NOT run; no TLMM is written and no
 *                             sensor is talked to).
 *   vayu_camera=4             step 1, then step 4 (one raw frame of the camera chosen by
 *                             vayu_camera_cam).
 *   vayu_camera=5             /dev/hakios-camera only: step 1 is NOT run, only the node and the
 *                             synthetic frames for iOS's camera userland.
 *   vayu_camera=6             /dev/hakios-camera fed by the real cameras: step 1 is NOT run
 *                             (GCC and camcc are mapped as step 1 does); needs
 *                             vayu_camera_xnu_tlmm=1; forces vayu_camera_clk to 1.
 *   vayu_camera>6             "only steps 1-6 exist", and step 6 is done.
 *   vayu_camera_fake_fps=N    step 5's frames a second, default 10 (1..30).
 *   vayu_camera_exposure=N    step 4: 0x0202 (coarse integration, lines) <- N before stream-on; off
 *   vayu_camera_gain=N        when 0 / absent (default). 0x0204 (analog gain) <- N likewise. Needs the
 *                             the maintainers' ruling (volatile SMIA registers, outside the vendor blocks;
 *                             granted 2026-10-25).
 *   vayu_camera_af=1          step 6: the rear DW9800 lens motor (GPIO 24 VAF, 0x0c: 0x02, 0x03/0x04
 *                             only; amendment 3); frames report its position, FOCUS moves it.
 *   vayu_camera_ae=1          step 4: the camera row's 0x0340 frame length (4x the mode's), 0x0202
 *                             exposure (frame - 48 lines) and 0x0204 gain (4x) before stream-on;
 *                             exposure/gain boot-args override the row's. Volatile registers: the
 *                             power-down after the frame resets them.
 *   vayu_camera_skip_ms=N     step 4: RDI 0 stays halted for N ms after stream-on (default 500), so
 *                             the frame written is not the dark first one.
 *   vayu_camera_cam=0|1|2     step 4's camera: 0 = rear IMX582 (default), 1 = front S5K3T2, 2 = the
 *                             rear and then the front in one boot, each with its whole teardown;
 *                             anything else says so and uses 0.
 *   vayu_camera_clk=1         camcc's PLL0 (Trion) up and step 3's RCGs at their svs rates; default
 *                             0: every camera RCG from XO at 19.2 MHz, no PLL. Steps 4 and 6 force
 *                             it to 1 and say so: their CSIPHY timer needs PLL0's even output.
 *   vayu_camera_settle=N      steps 4 and 6's CSIPHY settle count, default 0x20.
 *   vayu_camera_frames_ms=N   step 4: how long WM0's done bit is polled, default 2000 ms. Step 6
 *                             does not use it: its wait is 3 frame times + 100 ms.
 *   vayu_camera_tpg_w=W       the test pattern's width, default 640 (16..4096).
 *   vayu_camera_tpg_h=H       its height, default 480 (16..4096).
 *   vayu_camera_sensors=MASK  bit i = sensor i of the table, default 0x1; step 2 runs 0..4 in order.
 *   vayu_camera_xnu_tlmm=1    required for steps 2, 4 and 6 (TLMM writes). Without it the step
 *                             says so and does nothing; step 1 still runs.
 *   vayu_camera_shared=0      do not vote the shared rails at all (default 1: vote them on once,
 *                             never off -- see below).
 *   vayu_camera_pause=MS      time between the "about to write" line and the first write of step 2,
 *                             3 or 4, default 2200 ms, so the loader has the log before anything
 *                             changes. Step 6 uses 0 ms (the app is already waiting for frames).
 *   vayu_camera_hold=MS       how long a step-2 sensor stays powered after its ID read, default 0.
 *
 * The safety rules of this file:
 *   - a shared rail (smpa5, smpc8, bobc1, ldoc3) is voted on at most once per run and is never
 *     voted off: VANA of one sensor is VDIG of another, smpc8 is UFS VCCQ too, bobc1 feeds the
 *     audio, ldoc3 also feeds the DSI PLL;
 *   - only ldoc1, the cameras' own XOB regulator, is voted both ways;
 *   - TLMM is written only for the 6 output and 8 CCI pins of exception 3, only after the loader set them up,
 *     and only in cfg bit 9 (OE) and io bit 1 (level) -- never function, pull or drive;
 *   - every check is a read before the matching write, the first write waits for the pause, and
 *     the power-down of a sensor always runs, whether its probe worked or not;
 *   - step 3 writes no TLMM, no sensor and no rail: its effects are camcc, GCC, the SMMU and the two
 *     camera blocks, all of them read back and taken back in the reverse order (the test pattern
 *     first), and the frame memory is kept for the loader;
 *   - step 4 writes a sensor only through the selected module's vendor init, one mode block and
 *     its stream-on/off, from fw/camera-imx582.tab or fw/camera-s5k3t2.tab (TLMM exception 3's
 *     amendment, maintainers 2026-10-25 for the IMX582; amendment 2 of the same day for the S5K3T2):
 *     every byte acknowledged, a NACK stops the block, and the off block is written before any
 *     power-down once init was attempted; no other sensor register is touched;
 *   - PLL0 is refused if it runs already, an SMMU match register that already covers the IFEs'
 *     streams is left alone, and every loop of steps 3 and 4 is bounded (1 ms for PLL0's lock,
 *     500 us for an RCG or a branch, 10 ms for the SMMU's TLB, vayu_camera_frames_ms for the
 *     frame, 100 ms for a table delay);
 *   - step 5 touches no hardware: its ring is its only memory, its frames are made by the CPU,
 *     and its lock is its own; vayu_camera_quiesce() has nothing of step 5 to take down;
 *   - step 6 reuses step 4's sensor rules unchanged (the vendor blocks, the off block, the ID
 *     MATCH) and adds only 0x0340/0x0202/0x0204, from the selected camera row, at every session
 *     start (the fixed exposure); every wait is bounded (2 ms polls, 3 frame times + 100 ms per
 *     frame, 3 timeouts, 1 s of idle, 1 ms for PLL0's lock as step 4), the ring's places are
 *     re-armed one at a time, and a failure tears the session down without touching the node's
 *     memory: the first failure is retried once, the second ends the node.
 *
 * The hardware facts, with their sources:
 *
 *   GCC         base 0x00100000, 0x80000 bytes (as vayu_bt.c). The camera AHB CBCR is at 0xb008
 *               and the camera XO CBCR at 0xb044, both CLK_IS_CRITICAL (mainline
 *               drivers/clk/qcom/gcc-sm8150.c:1252-1257, :1295). CBCR bit 0 is enable, bit 31
 *               "clock off". camcc's registers sit behind the camera AHB, so they are only
 *               touched when 0xb008 reads enabled or not off; otherwise a read could hang.
 *   camcc       base 0x0ad00000, 0x10000 bytes (mainline arch/arm64/boot/dts/qcom/
 *               sm8150.dtsi:3766-3768). The titan_top GDSC is at 0xc1bc
 *               (drivers/clk/qcom/camcc-sm8150.c:1883-1891): bit 0 SW_COLLAPSE (1 = off request),
 *               bit 31 PWR_ON. MCLKn's RCG command is 0x5004 + 0x20*n (vendor camcc-sm8150.c:
 *               1181, :1201, :1221, :1241), with cfg at +4, M at +8, N at +0xc, D at +0x10
 *               (mnd_width 8, hid_width 5, parent map 1: source 0 = BI_TCXO, 19.2 MHz;
 *               :1172-1178). MCLKn's CBCR is 0x501c + 0x20*n (:2191, :2209, :2227, :2245).
 *               RCG cfg: [4:0] src_div (= 2*div - 1, so 1 for /1), [10:8] src_sel, [13:12] mode
 *               (0 = bypass M/N); RCG cmd: bit 0 UPDATE, bit 31 ROOT_OFF.
 *   GCC (more)  step 3's two camera AXI branches: camera_hf_axi CBCR 0xb030, camera_sf_axi CBCR
 *               0xb034 (gcc-sm8150.c:1684-1703).
 *   camcc RCGs  step 3's five: slow_ahb cmd 0x7058, camnoc_axi 0xc170, cphy_rx 0xa064,
 *               ife_lite_0_csid 0xc020, ife_lite_0 0xc004; cfg at +4. Parent map 0 is 0 XO
 *               (19.2 MHz), 1 PLL0 main, 2 PLL0 even, 3 PLL0 odd (vendor camcc-sm8150.c:91-97);
 *               the tables that give each its source and divider are :544-552, :625-629, :770-776,
 *               :971-978, :1290-1294. Their branches: cpas_ahb 0xc168, camnoc_axi 0xc18c,
 *               ife_lite_0_csid 0xc038, ife_lite_0_cphy_rx 0xc040, ife_lite_0 0xc01c.
 *   camcc (4)   step 4's csiNphytimer RCG command 0x6028 (CSIPHY1) or 0x604c (CSIPHY2):
 *               source 2 (PLL0 even, 600 MHz), field 3 (/2) = 300 MHz (camcc-sm8150.c:651-656,
 *               :678, :698); its branch CBCR is 0x6040 (:1511) or 0x6064 (:1529), and
 *               csiphy1/2's is 0x6044 (:1583) or 0x6068 (:1601).
 *   CSIPHY1/2   0x0ac66000 / 0x0ac67000, 0x1000 bytes, qcom,csiphy-v1.1 (vendor
 *               sm8150-camera.dtsi:47-72, :75-100). Both PHYs' mipi-csi-vdd is PM8150L L3
 *               (ldoc3, 1.2 V, shared with the DSI PLL). The v1.1 register tables are
 *               ref/cam_csiphy_1_1_hwreg.h and are the same for every instance; the core's order
 *               is reset, common, per-lane, irq (cam_csiphy_core.c), and the down path is the
 *               reset again. CSID's RX phy select is the CSIPHY's index (INFERENCE:
 *               camera_v3 cam_ife_csid_core.c:831).
 *   PLL0        Trion at camcc + 0x0 (camcc-sm8150.c:44-65, :83-99): 19.2 MHz * (0x3e + 0x8000/2^16)
 *               = 1200 MHz, its even output /2 = 600, its odd one /3 = 400. The configuration is
 *               the phone's v2 one (camcc-sm8150.c:179-191). Its register offsets and the three
 *               sequences are ref/clk-alpha-pll.c's TRION defines (:77-99), configure (:827-917),
 *               enable (:919-983) and disable (:985-1019); the lock is PLL_MODE bit 31 and the
 *               vendor's own wait is 100 x 1 us (:183-215), so 1 ms is this file's bound.
 *   CSID/IFE    CSID-lite 0 at 0x0acc8000, IFE-lite 0 at 0x0acc4000; every offset below is
 *               cam_ife_csid_lite17x.h's and cam_vfe_lite17x.h's, the same as redwood's. WM0's
 *               done bit is bus IRQ status1 0x2060 bit 0; IFE_REG_UPDATE 0x4ac takes 0x2 for RDI 0.
 *   SMMU        the apps SMMU at 0x15000000 (vayu_audio_smmu_925-1160.c:15), the layout read from
 *               IDR0/IDR1 as that code reads it: match registers IDR0[7:0], contexts IDR1[7:0], the
 *               page size IDR1[31] and 2 << IDR1[30:28] pages before the context banks. The IFEs'
 *               eight stream IDs are 0x840 under mask 0x620 (vendor sm8150-camera.dtsi:325-331), so
 *               one match register covers all eight. IOVA 0x40000000 is inside the DT's IFE pool
 *               (0x7400000 + 0xd8c00000).
 *   step 6      step 5's ring, the whole of it mapped in that SMMU: place p's IOVA is
 *               FRAME_ADDRESSES + p * place_bytes (place_bytes is round_page of the largest
 *               sensor's stride * height), and the frame's first byte is the place's first byte
 *               (no header). WM0_IMAGE_ADDR is written per frame, WM0_FRAME_INC stays 0 so a late
 *               frame overwrites the place it just finished rather than running into the next
 *               one, and IFE_REG_UPDATE 0x2 (0x4ac) makes the new address take. The camera row's
 *               0x0340 frame length is 4x the mode's (1262*4 = 5048 rear, 992*4 = 3968 front),
 *               its 0x0202 exposure the frame minus 48 lines (5000 / 3920), its 0x0204 gain 0x300
 *               (Sony's own code for 4x: 1024 - 1024/4) / 0x80 (Samsung's 0x20 unity: 4x), the
 *               same numbers as vayu_camera_ae=1; the ABI header reports that 4x linearly, 4 *
 *               gain_unity (0x400 rear, 0x80 front). The header's line_ns is 1e9 / (fps *
 *               frame_lines): 13190 ns rear (15.02 fps), 8375 ns front (30.10 fps).
 *   CAMNOC/CPAS step 3 programs nothing there: HYPOTHESIS, their reset defaults pass one small
 *               frame, and the run's log (show_paths()) is what will say.
 *   TLMM        cfg at tile + 0x1000 * pin: [1:0] pull (0 none, 1 down, 3 up), [5:2] function,
 *               [8:6] drive ((n + 1) * 2 mA), [9] OE; io at cfg + 4: bit 0 in, bit 1 out. Tiles
 *               NORTH 0x03900000, EAST 0x03500000, SOUTH 0x03d00000, WEST 0x03100000
 *               (drivers/pinctrl/qcom/pinctrl-sm8150.c:28-31, TLMM base 0x03000000). One 0x1000
 *               page per pin, mapped once and kept.
 *               NORTH 13-16 MCLK0-3 (function 1 = cam_mclk), 17-20 and 31-34 CCI (function 1 =
 *               cci_i2c), 84 hi259 standby, 94 VANA-EN (ov02b1b/ov8856/hi259); EAST 23 ov02b1b
 *               reset, 27 hi259 reset, 28 ov8856 reset, 30 imx582 reset; SOUTH 54 s5k3t2 reset,
 *               59 imx582 VDIG-EN, 118 s5k3t2 VDIG-EN; WEST 125 ov02b1b standby.
 *               Roles: 17/18 CCI0-M0 SDA/SCL, 19/20 CCI0-M1 SDA/SCL, 31/32 CCI1-M0 SDA/SCL,
 *               33/34 CCI1-M1 SDA/SCL (vendor vayu-sm8150-camera-sensor-mtp.dtsi:156-503).
 *   rails       vendor sm8150-regulator.dtsi, with the RPMh resource names of the DT:
 *               ldoc1 = PM8150L L1, 1.8 V, a qcom,rpmh-xob-regulator (:712-716), used only by the
 *               cameras, so exclusive here; smpa5 = PM8150 S5 = VANA of imx582; smpc8 = PM8150L S8
 *               = VDIG, also UFS VCCQ, so shared; bobc1 = BOB, shared with the audio; ldoc3 =
 *               PM8150L L3, 1.2 V, the CSIPHY's mipi-csi-vdd, shared with the DSI PLL, so shared
 *               too (voted 1 once and never off). The enable vote is the value 1 (on) or 0 (off)
 *               at vayu_rpmh_address(id) + 4 (vendor rpmh-regulator.c:95-101). Voltage is not
 *               voted: the DT values stand.
 *   I2C         bit-banged on the CCI pins, as vayu_audio_i2c_part.c does it on the QUP pins:
 *               100 kHz (5 us per half bit), clock stretching up to 100 half bits, a 5 ms
 *               deadline per transaction, the pins taken and given back exactly as found.
 *
 * Like vayu_haptic.c, the PMIC arbiter's read path is copied from bsd/dev/hakios.torch.c and
 * serialised with the buttons' driver (hakios_spmi_lock()/hakios_spmi_unlock()). The probe runs
 * in a kernel thread, so a slow register does not hold up boot.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/conf.h>
#include <sys/hakios.camera.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vayu_camera.h>
#include <sys/vayu_pas.h>

/*
 * Every line also goes into the loader's log as it is said (loader_log_putc, as vayu_audio.c's say()
 * does), so the camera's lines survive a run whose USB stream dies before the kernel returns
 * (a test run 220922: none of them came through with plain printf). Not for the quiesce: it uses
 * loader_log_line(), which is safe with interrupts off.
 */
extern void loader_log_putc(char c);
static int cam_printf(const char *format, ...) __printflike(1, 2);
#define printf cam_printf
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern void loader_log_line(const char *line);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void IOSleep(unsigned int ms);
extern void delay(int us);
/* (step 3's frame memory and the SMMU's lock: as in hakios_pipeline_1853-2720.c and vayu_audio_smmu_925-1160.c) */
extern uint64_t kvtophys(vm_offset_t va);
extern uint64_t loader_recording[3];
extern lck_mtx_t hakios_smmu_lock;
extern void loader_log_line(const char *line);   /* osfmk/arm64/loader_return.c: safe with interrupts off */
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);
/* (the module's own table, from the firmware directory: a pointer into the loaded image) */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
/* (sys/vayu_pas.h declares these two as they are added to vayu_pas.c; repeated so this file reads alone.) */
extern uint32_t vayu_rpmh_address(const char *name);
extern bool vayu_rpmh_send(const uint32_t addresses[], const uint32_t values[], unsigned count, const char *who);

/* ---- the blocks ------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL    /* vayu_bt.c:66 (GCC_PHYS) */
#define GCC_SIZE                0x80000UL       /* vayu_bt.c:67 */

#define CAMCC_PHYS              0x0ad00000UL    /* mainline sm8150.dtsi:3766-3768 */
#define CAMCC_SIZE              0x10000UL       /* its "reg" there: 0xad00000, 0x10000 */

#define TLMM_PIN_SIZE           0x1000UL        /* ctl_reg = 0x1000 * id (pinctrl-sm8150.c) */

/*
 * The tiles (pinctrl-sm8150.c:28-31, TLMM base 0x03000000). Every pin is its own 0x1000 page;
 * the table below uses these constants and keeps the mapped page per pin.
 */
#define TLMM_NORTH_PHYS         0x03900000UL
#define TLMM_EAST_PHYS          0x03500000UL
#define TLMM_SOUTH_PHYS         0x03D00000UL
#define TLMM_WEST_PHYS          0x03100000UL

/* ---- GCC: the camera's two critical CBCRs ------------------------------------------------------ */

#define GCC_CAMERA_AHB_CBCR     0xb008          /* gcc-sm8150.c:1252-1257, CLK_IS_CRITICAL */
#define GCC_CAMERA_XO_CBCR      0xb044          /* gcc-sm8150.c:1295, CLK_IS_CRITICAL */

#define GCC_CBCR_ENABLE         (1u << 0)       /* CBCR bit 0: enable */
#define GCC_CBCR_OFF            (1u << 31)      /* CBCR bit 31: clock off */

/* ---- camcc: titan_top GDSC and the four MCLKs -------------------------------------------------- */

#define CAMCC_TITAN_TOP_GDSC    0xc1bc          /* camcc-sm8150.c:1883-1891 */
#define CAMCC_GDSCR_SW_COLLAPSE (1u << 0)       /* GDSCR bit 0: 1 = off request */
#define CAMCC_GDSCR_POWER_ON    (1u << 31)      /* GDSCR bit 31: power on */

#define CAMCC_MCLK_CMD(n)       (0x5004u + 0x20u * (n)) /* vendor :1181, :1201, :1221, :1241 */
#define CAMCC_MCLK_CFG(n)       (CAMCC_MCLK_CMD(n) + 0x4u)
#define CAMCC_MCLK_M(n)         (CAMCC_MCLK_CMD(n) + 0x8u)
#define CAMCC_MCLK_N(n)         (CAMCC_MCLK_CMD(n) + 0xcu)
#define CAMCC_MCLK_D(n)         (CAMCC_MCLK_CMD(n) + 0x10u)
#define CAMCC_MCLK_CBCR(n)      (0x501cu + 0x20u * (n)) /* vendor :2191, :2209, :2227, :2245 */

#define CAMCC_CMD_UPDATE        (1u << 0)       /* RCG cmd bit 0: update */
#define CAMCC_CMD_ROOT_OFF      (1u << 31)      /* RCG cmd bit 31: root off */
#define CAMCC_CFG_SRC_MASK      (0x7u << 8)     /* RCG cfg source [10:8] */
#define CAMCC_CFG_SRC_SHFT      8
#define CAMCC_CFG_DIV_MASK      0x1fu           /* RCG cfg divider [4:0] */

/*
 * One MCLK from XO at 19.2 MHz: src_div [4:0] = 2 * div - 1 = 1 for /1, src_sel [10:8] = 0
 * (BI_TCXO, the only parent: map 1), mode [13:12] = 0 (bypass M/N). 19.2 MHz is
 * F(19200000, P_BI_TCXO, 1, 0, 0) in camcc-sm8150.c:1172-1178.
 */
#define CAMCC_MCLK_CFG_XO_19M2  1u

/* ---- TLMM pin registers, as vayu_bt.c decodes them --------------------------------------------- */

#define TLMM_CFG_PULL_MASK      0x3u            /* cfg [1:0] pull: 0 none, 1 down, 3 up */
#define TLMM_CFG_FUNC_SHFT      2               /* cfg [5:2] function */
#define TLMM_CFG_FUNC_MASK      0xfu
#define TLMM_CFG_DRIVE_SHFT     6               /* cfg [8:6] drive: (n + 1) * 2 mA */
#define TLMM_CFG_DRIVE_MASK     0x7u
#define TLMM_CFG_OE_SHFT        9               /* cfg [9] output enable */
#define TLMM_CFG_OE             (1u << TLMM_CFG_OE_SHFT)
#define TLMM_IO_IN              (1u << 0)       /* io [0] input level */
#define TLMM_IO_OUT             (1u << 1)       /* io [1] output level */

/* ---- reading and writing ----------------------------------------------------------------------- */

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

/* (only the registers named in the comment at the top: step 2's camcc/TLMM/PMIC ones, step 3's cameras' ones) */
static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static const char *
cam_clock_state(uint32_t cbcr)
{
	return (cbcr & GCC_CBCR_OFF) ? "OFF" : "on";
}

/* ---- the state of the run ---------------------------------------------------------------------- */

static uint32_t g_level, g_sensor_mask = 0x1, g_xnu_tlmm, g_shared = 1, g_pause_ms = 2200, g_hold_ms;
static uint32_t g_cam;                  /* vayu_camera_cam: 0 = rear IMX582, 1 = front S5K3T2 (2: both, g_both) */
static bool g_both;                     /* vayu_camera_cam=2: step 4 for the rear, then for the front */
static uint32_t g_clk;                  /* vayu_camera_clk: 0 = every camera RCG from XO, 1 = PLL0 and the svs rates */
static uint32_t g_settle = 0x20;        /* vayu_camera_settle: step 4's CSIPHY settle count (a HYPOTHESIS) */
static uint32_t g_frames_ms = 2000;     /* vayu_camera_frames_ms: step 4's wait for WM0's done */
static uint32_t g_exposure, g_gain;     /* vayu_camera_exposure / vayu_camera_gain: 0 = not written (the default) */
static uint32_t g_ae;                   /* vayu_camera_ae=1: the camera row's long frame, exposure and gain */
static uint32_t g_af;                   /* vayu_camera_af=1: step 6 drives the rear DW9800 lens motor (amendment 3) */
static uint32_t g_skip_ms = 500;        /* vayu_camera_skip_ms: step 4 lets the sensor run this long before RDI 0 takes a frame */
static bool g_rdi_hold;                 /* step 4: csid_config() leaves RDI 0 halted; the step resumes it after the skip */
static uint32_t g_tpg_w = 640, g_tpg_h = 480;   /* vayu_camera_tpg_w / _h: the test pattern's size */
static uint32_t g_fake_fps = 10;        /* vayu_camera_fake_fps: step 5's synthetic frames a second */
static vm_offset_t gcc_va, camcc_va;
static bool g_camcc_ok;                 /* step 1's verdict: camcc may be read and written */
static bool g_gdsc_ours;                /* this run turned the titan_top GDSC on: turn it off */
static uint32_t g_mclk_on_mask;         /* the MCLKs this run left running */
static volatile bool g_sensor_powered;  /* a step-2 or step-4 sensor is powered: the quiesce must act */
static volatile bool g_pipeline_running; /* step 3 or 4: while the paths run, the quiesce must act */
static bool g_csiphy_running;           /* step 4's CSIPHY was brought up: csiphy_down() resets it */
static uint32_t g_raised;               /* pin-table bits of the output pins driven high */
static bool g_custom1_said;             /* the "no custom1 supply" note, once per run */

/* ---- the pins, with their roles ---------------------------------------------------------------- */

struct cam_pin {
	uint32_t pin;
	const char *name;
	vm_offset_t phys;
	const char *fn1;        /* function 1's meaning here, or NULL when it is not a camera function */
};

#define CAM_PIN_COUNT   (sizeof(cam_pins) / sizeof(cam_pins[0]))

/*
 * All 22 camera pins (the Phase 1 plan's §1). The roles are from vendor
 * vayu-sm8150-camera-sensor-mtp.dtsi:156-503; the CCI and MCLK functions are the SoC's
 * (pinctrl-sm8150.c). The table is the allow-list for every TLMM write in this file.
 */
static const struct cam_pin cam_pins[] = {
	/* GPIO 13-16: MCLK0-3, function 1 = cam_mclk; north tile */
	{ 13, "MCLK0",        TLMM_NORTH_PHYS + 0x1000UL * 13, " (cam_mclk)" },
	{ 14, "MCLK1",        TLMM_NORTH_PHYS + 0x1000UL * 14, " (cam_mclk)" },
	{ 15, "MCLK2",        TLMM_NORTH_PHYS + 0x1000UL * 15, " (cam_mclk)" },
	{ 16, "MCLK3",        TLMM_NORTH_PHYS + 0x1000UL * 16, " (cam_mclk)" },
	/* GPIO 17-20, 31-34: CCI0-M0, CCI0-M1, CCI1-M0, CCI1-M1, function 1 = cci_i2c; north tile */
	{ 17, "CCI0-M0 SDA",  TLMM_NORTH_PHYS + 0x1000UL * 17, " (cci_i2c)" },
	{ 18, "CCI0-M0 SCL",  TLMM_NORTH_PHYS + 0x1000UL * 18, " (cci_i2c)" },
	{ 19, "CCI0-M1 SDA",  TLMM_NORTH_PHYS + 0x1000UL * 19, " (cci_i2c)" },
	{ 20, "CCI0-M1 SCL",  TLMM_NORTH_PHYS + 0x1000UL * 20, " (cci_i2c)" },
	{ 31, "CCI1-M0 SDA",  TLMM_NORTH_PHYS + 0x1000UL * 31, " (cci_i2c)" },
	{ 32, "CCI1-M0 SCL",  TLMM_NORTH_PHYS + 0x1000UL * 32, " (cci_i2c)" },
	{ 33, "CCI1-M1 SDA",  TLMM_NORTH_PHYS + 0x1000UL * 33, " (cci_i2c)" },
	{ 34, "CCI1-M1 SCL",  TLMM_NORTH_PHYS + 0x1000UL * 34, " (cci_i2c)" },
	/* GPIO 84: hi259 standby; north tile */
	{ 84, "hi259 STANDBY", TLMM_NORTH_PHYS + 0x1000UL * 84, NULL },
	/* GPIO 94: VANA enable of ov02b1b, ov8856 and hi259; north tile */
	{ 94, "VANA-EN",      TLMM_NORTH_PHYS + 0x1000UL * 94, NULL },
	/* GPIO 23: ov02b1b reset; east tile */
	{ 23, "ov02b1b RESET", TLMM_EAST_PHYS + 0x1000UL * 23, NULL },
	/* GPIO 27: hi259 reset; east tile */
	{ 27, "hi259 RESET",  TLMM_EAST_PHYS + 0x1000UL * 27, NULL },
	/* GPIO 28: ov8856 reset; east tile */
	{ 28, "ov8856 RESET", TLMM_EAST_PHYS + 0x1000UL * 28, NULL },
	/* GPIO 30: imx582 reset; east tile */
	{ 30, "imx582 RESET", TLMM_EAST_PHYS + 0x1000UL * 30, NULL },
	/* GPIO 54: s5k3t2 reset; south tile */
	{ 54, "s5k3t2 RESET", TLMM_SOUTH_PHYS + 0x1000UL * 54, NULL },
	/* GPIO 59: imx582 VDIG enable; south tile */
	{ 59, "imx582 VDIG-EN", TLMM_SOUTH_PHYS + 0x1000UL * 59, NULL },
	/* GPIO 118: s5k3t2 VDIG enable; south tile */
	{ 118, "s5k3t2 VDIG-EN", TLMM_SOUTH_PHYS + 0x1000UL * 118, NULL },
	/* GPIO 125: ov02b1b standby; west tile */
	{ 125, "ov02b1b STANDBY", TLMM_WEST_PHYS + 0x1000UL * 125, NULL },
	/* GPIO 24: the rear actuator's VAF enable (actuator_rear_regulator, 2.8 V from BOB:
	 * vayu-sm8150-camera-sensor-mtp.dtsi:56-66); east tile. TLMM exception 3, amendment 3. */
	{ 24, "actuator VAF-EN", TLMM_EAST_PHYS + 0x1000UL * 24, NULL },
};

/* Each pin's 0x1000 page, mapped once and kept (cam_pin_page()); also what the quiesce walks. */
static vm_offset_t cam_pin_va[CAM_PIN_COUNT];

/*
 * The output pins of TLMM exception 3 (the bring-up notes): the only ones cam_gpio_set() may
 * drive (the I2C pins use cam_line()). 27 (hi259 reset), 84 (hi259 standby), 94 (VANA-EN) and 125
 * (ov02b1b standby) are read in step 1 but never written: 27, 94 and 125 have a second owner in the
 * phone's live DT (panel ESD irq, msm_tspp tsif1, aqt1000), and the loader leaves 84 alone.
 */
static bool
cam_pin_is_output(uint32_t pin)
{
	switch (pin) {
	case 23:
	case 28:
	case 30:
	case 54:
	case 59:
	case 118:
	case 24:        /* (amendment 3: the actuator's VAF enable, only with vayu_camera_af=1) */
		return true;
	}
	return false;
}

static int
cam_pin_index(uint32_t pin)
{
	for (unsigned i = 0; i < CAM_PIN_COUNT; i++) {
		if (cam_pins[i].pin == pin) {
			return (int)i;
		}
	}
	return -1;
}

/*
 * A pin's 0x1000 page, mapped once and kept (22 pages at most). 0 if it cannot be mapped; the
 * caller says so.
 */
static vm_offset_t
cam_pin_page(uint32_t pin)
{
	int index = cam_pin_index(pin);
	vm_offset_t page;

	if (index < 0) {
		return 0;
	}
	if (cam_pin_va[index] != 0) {
		return cam_pin_va[index];
	}
	page = ml_io_map(cam_pins[index].phys, TLMM_PIN_SIZE);
	if (page != 0) {
		cam_pin_va[index] = page;
	}
	return page;
}

static unsigned
cam_pin_level(uint32_t pin)
{
	vm_offset_t page = cam_pin_page(pin);

	return page == 0 ? 1 : (rd(page + 4) & TLMM_IO_IN);
}

/* ---- the sensor table -------------------------------------------------------------------------- */

enum cam_step_kind {
	CAM_VIO,
	CAM_VANA,
	CAM_VDIG,
	CAM_CUSTOM1,
	CAM_CUSTOM2,
	CAM_MCLK,
	CAM_RESET,
	CAM_STANDBY,
};

struct cam_step {
	uint8_t kind;
	uint8_t value;          /* 1 = on/high, 0 = off/low */
	uint16_t delay_ms;      /* IOSleep after the step, as the module's powerSetting says */
};

struct cam_sensor {
	const char *name;
	uint8_t sda, scl;
	uint8_t addr7;
	uint8_t reg_bytes, data_bytes;
	uint16_t id_reg;
	uint16_t id;
	uint8_t mclk;
	int reset, standby, vdig_gpio, vana_gpio;  /* -1: not there */
	const char *vana_rail, *vdig_rail, *custom2_rail;
	const struct cam_step *up, *down;
	unsigned up_count, down_count;
	bool probe_only;        /* the ACK probe only: the id read could write a register (see the table) */
	const char *skip;       /* not probed at all, and why */
};

/* module powerSetting: com.qti.sensormodule.vayu_imx582.bin (DT node @0, vendor dtsi:156-202) */
static const struct cam_step imx582_up[] = {
	{ CAM_CUSTOM2, 1, 5 }, { CAM_VANA, 1, 0 }, { CAM_CUSTOM1, 1, 0 }, { CAM_VDIG, 1, 0 },
	{ CAM_VIO, 1, 0 }, { CAM_MCLK, 1, 10 }, { CAM_RESET, 1, 2 },
};
static const struct cam_step imx582_down[] = {
	{ CAM_RESET, 0, 2 }, { CAM_MCLK, 0, 0 }, { CAM_VIO, 0, 0 }, { CAM_VDIG, 0, 0 },
	{ CAM_CUSTOM1, 0, 0 }, { CAM_VANA, 0, 20 }, { CAM_CUSTOM2, 0, 0 },
};

/* com.qti.sensormodule.vayu_ov02b1b.bin (DT node @1) */
static const struct cam_step ov02b1b_up[] = {
	{ CAM_RESET, 0, 1 }, { CAM_VIO, 1, 1 }, { CAM_VANA, 1, 5 }, { CAM_STANDBY, 1, 1 },
	{ CAM_MCLK, 1, 4 }, { CAM_RESET, 1, 9 },
};
static const struct cam_step ov02b1b_down[] = {
	{ CAM_RESET, 0, 1 }, { CAM_MCLK, 0, 1 }, { CAM_STANDBY, 0, 0 }, { CAM_VANA, 0, 0 },
	{ CAM_VIO, 0, 0 },
};

/* com.qti.sensormodule.vayu_s5k3t2.bin (DT node @2) */
static const struct cam_step s5k3t2_up[] = {
	{ CAM_STANDBY, 0, 1 }, { CAM_VDIG, 1, 0 }, { CAM_VIO, 1, 0 }, { CAM_MCLK, 1, 1 },
	{ CAM_RESET, 1, 1 },
};
static const struct cam_step s5k3t2_down[] = {
	{ CAM_RESET, 0, 1 }, { CAM_MCLK, 0, 1 }, { CAM_VIO, 0, 0 }, { CAM_VDIG, 0, 0 },
	{ CAM_STANDBY, 1, 1 },
};

/* com.qti.sensormodule.vayu_ov8856.bin (DT node @3) */
static const struct cam_step ov8856_up[] = {
	{ CAM_CUSTOM1, 1, 5 }, { CAM_VANA, 1, 0 }, { CAM_VIO, 1, 0 }, { CAM_VDIG, 1, 0 },
	{ CAM_MCLK, 1, 10 }, { CAM_RESET, 1, 2 },
};
static const struct cam_step ov8856_down[] = {
	{ CAM_RESET, 0, 1 }, { CAM_MCLK, 0, 1 }, { CAM_VDIG, 0, 0 }, { CAM_VIO, 0, 0 },
	{ CAM_VANA, 0, 20 }, { CAM_CUSTOM1, 0, 0 },
};

/* com.qti.sensormodule.vayu_hi259.bin (DT node @4) */
static const struct cam_step hi259_up[] = {
	{ CAM_RESET, 0, 3 }, { CAM_STANDBY, 0, 3 }, { CAM_VIO, 1, 0 }, { CAM_VANA, 1, 10 },
	{ CAM_MCLK, 1, 2 }, { CAM_STANDBY, 1, 5 }, { CAM_RESET, 1, 10 },
};
static const struct cam_step hi259_down[] = {
	{ CAM_RESET, 0, 1 }, { CAM_MCLK, 0, 1 }, { CAM_STANDBY, 0, 1 }, { CAM_VANA, 0, 1 },
	{ CAM_VIO, 0, 0 },
};

/*
 * The module table. The DT index is the node @N of vendor vayu-sm8150-camera-sensor-mtp.dtsi;
 * the values are the modules' (TheMuppets proprietary_vendor_xiaomi_vayu @56e4960).
 */
static const struct cam_sensor cam_sensors[] = {
	{
		.name = "imx582", .sda = 19, .scl = 20, .addr7 = 0x10,
		.reg_bytes = 2, .data_bytes = 2, .id_reg = 0x0016, .id = 0x0582, .mclk = 1,
		.reset = 30, .standby = -1, .vdig_gpio = 59, .vana_gpio = -1,
		.vana_rail = "smpa5", .vdig_rail = "smpc8", .custom2_rail = "bobc1",
		.up = imx582_up, .down = imx582_down,
		.up_count = sizeof(imx582_up) / sizeof(imx582_up[0]),
		.down_count = sizeof(imx582_down) / sizeof(imx582_down[0]),
	},
	{
		.name = "ov02b1b", .sda = 33, .scl = 34, .addr7 = 0x3c,
		.reg_bytes = 2, .data_bytes = 2, .id_reg = 0x0203, .id = 0x002b, .mclk = 2,
		/* standby 125 and VANA-EN 94 are outside TLMM exception 3: those steps are skipped */
		.reset = 23, .standby = -1, .vdig_gpio = -1, .vana_gpio = -1,
		.vana_rail = "bobc1", .vdig_rail = NULL, .custom2_rail = NULL,
		.up = ov02b1b_up, .down = ov02b1b_down,
		.up_count = sizeof(ov02b1b_up) / sizeof(ov02b1b_up[0]),
		.down_count = sizeof(ov02b1b_down) / sizeof(ov02b1b_down[0]),
		/*
		 * HYPOTHESIS: the module's 2-byte register address 0x0203 does not fit the OV02B family's
		 * 8-bit registers; sent to an 8-bit device its second byte would be a register write.
		 */
		.probe_only = true,
	},
	{
		.name = "s5k3t2", .sda = 17, .scl = 18, .addr7 = 0x10,
		.reg_bytes = 2, .data_bytes = 2, .id_reg = 0x0000, .id = 0x3142, .mclk = 0,
		.reset = 54, .standby = -1, .vdig_gpio = 118, .vana_gpio = -1,
		.vana_rail = "bobc1", .vdig_rail = "smpc8", .custom2_rail = NULL,
		.up = s5k3t2_up, .down = s5k3t2_down,
		.up_count = sizeof(s5k3t2_up) / sizeof(s5k3t2_up[0]),
		.down_count = sizeof(s5k3t2_down) / sizeof(s5k3t2_down[0]),
	},
	{
		.name = "ov8856", .sda = 33, .scl = 34, .addr7 = 0x36,
		.reg_bytes = 2, .data_bytes = 2, .id_reg = 0x300b, .id = 0x885a, .mclk = 3,
		.reset = 28, .standby = -1, .vdig_gpio = -1, .vana_gpio = -1,   /* VANA-EN 94: outside the exception */
		.vana_rail = "bobc1", .vdig_rail = "smpc8", .custom2_rail = NULL,
		.up = ov8856_up, .down = ov8856_down,
		.up_count = sizeof(ov8856_up) / sizeof(ov8856_up[0]),
		.down_count = sizeof(ov8856_down) / sizeof(ov8856_down[0]),
	},
	{
		.name = "hi259", .sda = 33, .scl = 34, .addr7 = 0x38,
		.reg_bytes = 1, .data_bytes = 1, .id_reg = 0x04, .id = 0xe1, .mclk = 3,
		.reset = 27, .standby = 84, .vdig_gpio = -1, .vana_gpio = 94,
		.vana_rail = "bobc1", .vdig_rail = NULL, .custom2_rail = NULL,
		.up = hi259_up, .down = hi259_down,
		.up_count = sizeof(hi259_up) / sizeof(hi259_up[0]),
		.down_count = sizeof(hi259_down) / sizeof(hi259_down[0]),
		.skip = "its reset (gpio 27) has a second owner in the DT: outside TLMM exception 3",
	},
};

/* ---- step 4's per-camera configuration --------------------------------------------------------- */

/*
 * One row per camera that step 4 can stream. The rows differ in the sensor, its table and mode
 * block, the frame size and rate, and which CSIPHY and camcc PHY clocks carry it; both use four
 * lanes and lane assignment 0x3210. Sources: the modules' streamConfiguration (the modes), vendor
 * sm8150-camera.dtsi:47-100 (CSIPHY1/2), camcc-sm8150.c:678/:698, :1511/:1529 and :1583/:1601
 * (the timers and their branches). CSID's phy_sel is the CSIPHY's index (INFERENCE: camera_v3
 * cam_ife_csid_core.c:831).
 */
struct cam_stream_cfg {
	const char *name;               /* the summary label and the log prefix: imx582 / s5k3t2 */
	unsigned sensor;                /* index into cam_sensors[] */
	const char *tab;                /* the table in fw/ (built on the host, not in git) */
	const char *mode;               /* the mode block's name: res3 / res2 */
	uint32_t width, height;
	const char *fps;                /* text only */
	unsigned lanes;
	uint32_t lane_cfg;
	unsigned csiphy;                /* 1 or 2 */
	vm_offset_t csiphy_phys;
	unsigned phy_sel;               /* CSID's RX phy select */
	uint32_t timer_cmd;             /* csiNphytimer's RCG command */
	uint32_t timer_cbcr;            /* csiNphytimer's branch */
	uint32_t phy_cbcr;              /* csiphyN's branch */
	uint32_t bayer;                 /* the frame header's: 0 RGGB */
	const char *header;             /* the header's sensor name: IMX582 / S5K3T2 */
	/*
	 * vayu_camera_ae=1 (the maintainers' ruling 2026-10-25: 0x0202/0x0204/0x0340 before stream-on): a
	 * long frame, a long exposure and a moderate gain, ~16x the light of the vendor mode, so one
	 * run tells "the scene is dark" from "the exposure is the problem".
	 */
	uint16_t ae_frame_lines;        /* 0x0340: 4x the mode's (IMX582 res3 0x04ee, S5K3T2 res2 0x03e0) */
	uint16_t ae_lines;              /* 0x0202: frame lines - 48 (the vendor's own margin in res3: 0x04ee - 0x04be) */
	uint16_t ae_gain;               /* 0x0204: 4x (Sony 1024 - 1024/4 = 0x300; Samsung 0x20 per 1x = 0x80: HYPOTHESIS) */
};

static const struct cam_stream_cfg cam_streams[] = {
	{
		.name = "imx582", .sensor = 0, .tab = "camera-imx582.tab", .mode = "res3",
		.width = 1920, .height = 1080, .fps = "60.08", .lanes = 4, .lane_cfg = 0x3210,
		.csiphy = 1, .csiphy_phys = 0x0ac66000UL, .phy_sel = 1,
		.timer_cmd = 0x6028, .timer_cbcr = 0x6040, .phy_cbcr = 0x6044,
		.bayer = 0, .header = "IMX582",       /* RGGB: colorFilterArrangement 3 in CamX's order (tools/camsensor.py:142) */
		.ae_frame_lines = 0x13b8, .ae_lines = 0x1388, .ae_gain = 0x300,
	},
	{
		.name = "s5k3t2", .sensor = 2, .tab = "camera-s5k3t2.tab", .mode = "res2",
		.width = 1280, .height = 720, .fps = "120.41", .lanes = 4, .lane_cfg = 0x3210,
		.csiphy = 2, .csiphy_phys = 0x0ac67000UL, .phy_sel = 2,
		.timer_cmd = 0x604c, .timer_cbcr = 0x6064, .phy_cbcr = 0x6068,
		.bayer = 2, .header = "S5K3T2",       /* GBRG: the module's colorFilterArrangement 1 in CamX's order (0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB: tools/camsensor.py:31-32, :142); a tester's magenta selfie with RGGB (a test run) */
		.ae_frame_lines = 0x0f80, .ae_lines = 0x0f50, .ae_gain = 0x80,
	},
};

/* The row vayu_camera_cam selected; set before step 4 and used by the table's log prefixes. */
static const struct cam_stream_cfg *g_stream = &cam_streams[0];

/* ---- the rails --------------------------------------------------------------------------------- */

enum cam_rail_kind {
	CAM_RAIL_VIO,           /* ldoc1, the cameras' own XOB regulator: exclusive */
	CAM_RAIL_VANA,          /* per sensor (smpa5 for imx582, bobc1 for the rest) */
	CAM_RAIL_VDIG,          /* smpc8, shared with UFS VCCQ */
	CAM_RAIL_BOB,           /* bobc1, shared with the audio */
	CAM_RAIL_CSI,           /* ldoc3, the CSIPHY's 1.2 V, shared with the DSI PLL */
	CAM_RAIL_COUNT,
};

struct cam_rail {
	const char *id;
	bool shared;            /* true: vote 1 once, never 0 */
	uint32_t addr;          /* vayu_rpmh_address(id), looked up at the start of step 2 */
	bool voted_on;
};

static struct cam_rail cam_rails[CAM_RAIL_COUNT] = {
	[CAM_RAIL_VIO] = { .id = "ldoc1", .shared = false },   /* PM8150L L1, 1.8 V (vendor sm8150-regulator.dtsi:712-716) */
	[CAM_RAIL_VANA] = { .id = "smpa5", .shared = true },   /* PM8150 S5: VANA of imx582 */
	[CAM_RAIL_VDIG] = { .id = "smpc8", .shared = true },   /* PM8150L S8: VDIG, and UFS VCCQ */
	[CAM_RAIL_BOB] = { .id = "bobc1", .shared = true },    /* BOB: shared with the audio */
	[CAM_RAIL_CSI] = { .id = "ldoc3", .shared = true },    /* PM8150L L3, 1.2 V: the CSIPHY's, and the DSI PLL's */
};

static struct cam_rail *
cam_rail_find(const char *id)
{
	if (id == NULL) {
		return NULL;
	}
	for (unsigned i = 0; i < CAM_RAIL_COUNT; i++) {
		if (strcmp(cam_rails[i].id, id) == 0) {
			return &cam_rails[i];
		}
	}
	return NULL;
}

/*
 * The enable vote at vayu_rpmh_address(id) + 4: 1 on, 0 off (vendor rpmh-regulator.c:95-101).
 * A shared rail is only ever voted on, once per run; an "off" for one says so and does nothing.
 * Fills `detail' with the short reason the step line shows.
 */
static bool
cam_rail_vote(struct cam_rail *r, unsigned on, char *detail, size_t size)
{
	uint32_t addr4, value = on ? 1u : 0u;

	if (r == NULL) {
		snprintf(detail, size, "no rail in the DT: skipped");
		return false;
	}
	if (r->shared) {
		if (!on) {
			snprintf(detail, size, "%s is shared: left on", r->id);
			return true;
		}
		if (g_shared == 0) {
			snprintf(detail, size, "%s: shared, not voted", r->id);
			return true;
		}
		if (r->voted_on) {
			snprintf(detail, size, "%s shared: voted 1 already", r->id);
			return true;
		}
		addr4 = r->addr + 4;
		if (!vayu_rpmh_send(&addr4, &value, 1, "vayu-camera")) {
			snprintf(detail, size, "%s shared: vote 1 NOT DONE", r->id);
			return false;
		}
		r->voted_on = true;
		snprintf(detail, size, "%s shared: voted 1", r->id);
		return true;
	}
	addr4 = r->addr + 4;
	if (!vayu_rpmh_send(&addr4, &value, 1, "vayu-camera")) {
		snprintf(detail, size, "%s voted %u: NOT DONE", r->id, value);
		return false;
	}
	snprintf(detail, size, "%s voted %u: done", r->id, value);
	return true;
}

/* ---- small text helpers ------------------------------------------------------------------------ */

static void cam_append(char *buf, size_t size, const char *format, ...) __printflike(3, 4);

static void
cam_append(char *buf, size_t size, const char *format, ...)
{
	size_t at = strlen(buf);
	va_list list;

	if (at + 1 >= size) {
		return;
	}
	va_start(list, format);
	vsnprintf(buf + at, size - at, format, list);
	va_end(list);
}

/* ", " between the items of one step line. */
static const char *
cam_sep(const char *buf)
{
	return buf[0] != 0 ? ", " : "";
}

static void
cam_bytes(char *buf, size_t size, const uint8_t *data, unsigned count)
{
	buf[0] = 0;
	for (unsigned i = 0; i < count; i++) {
		cam_append(buf, size, "%s%02x", i != 0 ? " " : "", data[i]);
	}
}

static const char *
cam_step_name(uint8_t kind)
{
	switch (kind) {
	case CAM_VIO: return "VIO";
	case CAM_VANA: return "VANA";
	case CAM_VDIG: return "VDIG";
	case CAM_CUSTOM1: return "CUSTOM1";
	case CAM_CUSTOM2: return "CUSTOM2";
	case CAM_MCLK: return "MCLK";
	case CAM_RESET: return "RESET";
	case CAM_STANDBY: return "STANDBY";
	}
	return "?";
}

/* ---- TLMM output pins -------------------------------------------------------------------------- */

/*
 * io bit 1 only, read-modify-write. Refuses any pin that is not one of the outputs above.
 * The io read back is in `detail' for the step line.
 */
static bool
cam_gpio_set(uint32_t pin, unsigned high, char *detail, size_t size)
{
	vm_offset_t page;
	uint32_t io;
	int index;

	if (!cam_pin_is_output(pin)) {
		snprintf(detail, size, "gpio%u is not an output this file may drive: refused", pin);
		return false;
	}
	page = cam_pin_page(pin);
	if (page == 0) {
		snprintf(detail, size, "gpio%u cannot be mapped: not touched", pin);
		return false;
	}
	io = rd(page + 4);
	io = (io & ~TLMM_IO_OUT) | (high ? TLMM_IO_OUT : 0u);
	wr(page + 4, io);
	index = cam_pin_index(pin);
	if (index >= 0) {
		if (high) {
			g_raised |= 1u << index;
		} else {
			g_raised &= ~(1u << index);
		}
	}
	snprintf(detail, size, "gpio%u -> %u (io reads %#x)", pin, high ? 1 : 0, rd(page + 4));
	return true;
}

/* ---- I2C by hand, as vayu_audio_i2c_part.c does it (hakios.speaker.c) ------------------------- */

struct cam_bus {
	unsigned sda, scl;
	vm_offset_t sda_pg, scl_pg;
};

#define CAM_HALF_BIT_US         5               /* 100 kHz at most */
#define CAM_STRETCH_HALF_BITS   100u
#define CAM_I2C_TIMEOUT_US      20000           /* 20 ms per transaction (a stuck clock stretch, not preemption: see below) */

/* The two pins as they were found at bus_take(): cfg and io exactly, given back at the end. */
static uint32_t i2c_taken_cfg[2], i2c_taken_io[2];
static uint64_t i2c_deadline;
static bool i2c_late;

static void
cam_half_bit(void)
{
	delay(CAM_HALF_BIT_US);
}

static bool
cam_i2c_overdue(void)
{
	if (!i2c_late && mach_absolute_time() > i2c_deadline) {
		i2c_late = true;
	}
	return i2c_late;
}

/*
 * A line is driven low (io bit 1 = 0, then OE set) or let go (OE cleared, the bus's pull-up raises
 * it). Only cfg bit 9 and io bit 1 change: every other cfg bit stays as read at bus_take().
 */
static void
cam_line(struct cam_bus *b, unsigned pin, bool high)
{
	vm_offset_t page = (pin == b->sda) ? b->sda_pg : b->scl_pg;
	uint32_t cfg, io;

	cfg = rd(page);
	if (high) {
		cfg &= ~TLMM_CFG_OE;
		wr(page, cfg);
	} else {
		io = rd(page + 4);
		wr(page + 4, io & ~TLMM_IO_OUT);
		cfg |= TLMM_CFG_OE;
		wr(page, cfg);
	}
}

static void
cam_bus_give_back(struct cam_bus *b)
{
	wr(b->sda_pg + 4, i2c_taken_io[0] & TLMM_IO_OUT);
	wr(b->scl_pg + 4, i2c_taken_io[1] & TLMM_IO_OUT);
	wr(b->sda_pg, i2c_taken_cfg[0]);
	wr(b->scl_pg, i2c_taken_cfg[1]);
}

static void
cam_i2c_begin(void)
{
	uint64_t limit;

	nanoseconds_to_absolutetime(CAM_I2C_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &limit);
	i2c_deadline = mach_absolute_time() + limit;
	i2c_late = false;
}

/* False if the bus is not at rest (both lines must read high after being let go). */
static bool
cam_bus_take(struct cam_bus *b)
{
	i2c_taken_cfg[0] = rd(b->sda_pg);
	i2c_taken_cfg[1] = rd(b->scl_pg);
	i2c_taken_io[0] = rd(b->sda_pg + 4);
	i2c_taken_io[1] = rd(b->scl_pg + 4);
	cam_line(b, b->sda, true);
	cam_line(b, b->scl, true);
	cam_half_bit();
	if (cam_pin_level(b->scl) != 0 && cam_pin_level(b->sda) == 0) {
		/*
		 * A device holds SDA low: a transaction cut off in the middle of a byte (a test run 231946:
		 * the S5K3T2's bus read SDA 0 after a timed-out read). Up to nine clocks let it shift the
		 * rest of its byte out and see a NACK; then a STOP (the I2C specification's bus clear).
		 */
		unsigned clocks = 0;

		while (clocks < 9 && cam_pin_level(b->sda) == 0) {
			cam_line(b, b->scl, false);
			cam_half_bit();
			cam_line(b, b->scl, true);
			cam_half_bit();
			clocks++;
		}
		cam_line(b, b->sda, false);
		cam_half_bit();
		cam_line(b, b->scl, true);
		cam_half_bit();
		cam_line(b, b->sda, true);
		cam_half_bit();
		printf("vayu-camera: i2c gpio%u/%u: SDA was held low; bus clear after %u clocks: SDA %u\n",
		    b->sda, b->scl, clocks, cam_pin_level(b->sda));
	}
	if (cam_pin_level(b->sda) == 0 || cam_pin_level(b->scl) == 0) {
		printf("vayu-camera: i2c gpio%u/%u not at rest (SDA %u, SCL %u): not driven\n",
		    b->sda, b->scl, cam_pin_level(b->sda), cam_pin_level(b->scl));
		cam_bus_give_back(b);
		return false;
	}
	return true;
}

static void
cam_i2c_start(struct cam_bus *b)
{
	cam_line(b, b->sda, true);
	cam_line(b, b->scl, true);
	cam_half_bit();
	cam_line(b, b->sda, false);
	cam_half_bit();
	cam_line(b, b->scl, false);
	cam_half_bit();
}

static void
cam_i2c_stop(struct cam_bus *b)
{
	cam_line(b, b->sda, false);
	cam_half_bit();
	cam_line(b, b->scl, true);
	cam_half_bit();
	cam_line(b, b->sda, true);
	cam_half_bit();
}

/* One bit, with clock stretching (at most 100 half bits) and the transaction's deadline. */
static bool
cam_i2c_bit(struct cam_bus *b, bool out)
{
	bool in;

	if (cam_i2c_overdue()) {
		return true;            /* reads as "not acknowledged": the transaction ends */
	}
	cam_line(b, b->sda, out);
	cam_half_bit();
	cam_line(b, b->scl, true);
	for (unsigned stretched = 0; cam_pin_level(b->scl) == 0 && stretched < CAM_STRETCH_HALF_BITS; stretched++) {
		cam_half_bit();
	}
	cam_half_bit();
	in = cam_pin_level(b->sda);
	cam_line(b, b->scl, false);
	return in;
}

static bool
cam_i2c_write(struct cam_bus *b, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		cam_i2c_bit(b, (byte >> bit) & 1);
	}
	return !cam_i2c_bit(b, true);
}

static uint8_t
cam_i2c_read(struct cam_bus *b, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | (cam_i2c_bit(b, true) ? 1 : 0));
	}
	cam_i2c_bit(b, !more);          /* ACK all but the last byte, NACK the last */
	return byte;
}

/* The one line after a transaction: both pins' cfg and io, read back. */
static void
cam_i2c_show_back(struct cam_bus *b)
{
	printf("vayu-camera: i2c gpio%u/%u given back: cfg %#x/%#x io %#x/%#x\n",
	    b->sda, b->scl, rd(b->sda_pg), rd(b->scl_pg), rd(b->sda_pg + 4), rd(b->scl_pg + 4));
}

/* (a) START, address << 1 | 0, the ACK bit, STOP. No register pointer is written. */
static bool
cam_i2c_probe_raw(struct cam_bus *b, uint8_t addr7)
{
	bool acked;

	cam_i2c_begin();
	if (!cam_bus_take(b)) {
		return false;
	}
	cam_i2c_start(b);
	acked = cam_i2c_write(b, (uint8_t)(addr7 << 1));
	cam_i2c_stop(b);
	cam_bus_give_back(b);
	if (i2c_late) {
		printf("vayu-camera: i2c probe of %#x timed out (5 ms)\n", addr7);
	}
	cam_i2c_show_back(b);
	return acked && !i2c_late;
}

/*
 * (b) START, address W, the register address (big-endian, 1 or 2 bytes), repeated START,
 * address R, the data bytes (ACK all but the last), STOP.
 */
static bool
cam_i2c_read_id_raw(struct cam_bus *b, uint8_t addr7, uint16_t reg, unsigned reg_bytes,
    uint8_t *data, unsigned data_bytes)
{
	bool answered = true;

	cam_i2c_begin();
	if (!cam_bus_take(b)) {
		return false;
	}
	cam_i2c_start(b);
	answered = cam_i2c_write(b, (uint8_t)(addr7 << 1));
	for (unsigned i = reg_bytes; answered && i > 0; i--) {
		answered = cam_i2c_write(b, (uint8_t)((reg >> (8 * (i - 1))) & 0xff));
	}
	if (answered) {
		cam_i2c_start(b);
		answered = cam_i2c_write(b, (uint8_t)(addr7 << 1 | 1));
	}
	for (unsigned i = 0; answered && i < data_bytes; i++) {
		data[i] = cam_i2c_read(b, i + 1 < data_bytes);
	}
	cam_i2c_stop(b);
	cam_bus_give_back(b);
	if (i2c_late) {
		printf("vayu-camera: i2c read of %#x reg %#x timed out (5 ms)\n", addr7, reg);
	}
	cam_i2c_show_back(b);
	return answered && !i2c_late;
}

/*
 * (c) START, address W, the register address (big-endian, 1 or 2 bytes), the value (big-endian,
 * 1 or 2 bytes), STOP. Every byte must be acknowledged; a NACK (or the 5 ms deadline) ends the
 * transaction at once. The whole transaction is one bus_take()/give_back() pair, as the reads are.
 */
static bool
cam_i2c_write_reg_raw(struct cam_bus *b, uint8_t addr7, uint16_t reg, unsigned reg_bytes,
    uint16_t value, unsigned value_bytes)
{
	bool answered = true;

	cam_i2c_begin();
	if (!cam_bus_take(b)) {
		return false;
	}
	cam_i2c_start(b);
	answered = cam_i2c_write(b, (uint8_t)(addr7 << 1));
	for (unsigned i = reg_bytes; answered && i > 0; i--) {
		answered = cam_i2c_write(b, (uint8_t)((reg >> (8 * (i - 1))) & 0xff));
	}
	for (unsigned i = value_bytes; answered && i > 0; i--) {
		answered = cam_i2c_write(b, (uint8_t)((value >> (8 * (i - 1))) & 0xff));
	}
	cam_i2c_stop(b);
	cam_bus_give_back(b);
	if (i2c_late) {
		printf("vayu-camera: i2c write of %#x reg %#x timed out (5 ms)\n", addr7, reg);
	}
	if (!answered || i2c_late) {
		cam_i2c_show_back(b);
	}
	return answered && !i2c_late;
}

/*
 * Each transaction runs with this core's interrupts off (about 0.1-0.6 ms at 100 kHz): the bit-banged
 * clock is ours, so a thread preempted in the middle of a byte only stretches it -- but the deadline
 * then cut the transaction off mid-byte (a test run, 8 busy cores: "i2c write of 0x10 reg
 * 0x61a1 timed out (5 ms)" at the IMX582's init entry 260, and the S5K3T2's ID read). With interrupts
 * off nothing preempts it; the deadline is left for a device that stretches the clock for too long.
 */
static bool
cam_i2c_probe(struct cam_bus *b, uint8_t addr7)
{
	boolean_t was = ml_set_interrupts_enabled(FALSE);
	bool ok = cam_i2c_probe_raw(b, addr7);

	(void)ml_set_interrupts_enabled(was);
	return ok;
}

static bool
cam_i2c_read_id(struct cam_bus *b, uint8_t addr7, uint16_t reg, unsigned reg_bytes, uint8_t *data, unsigned data_bytes)
{
	boolean_t was = ml_set_interrupts_enabled(FALSE);
	bool ok = cam_i2c_read_id_raw(b, addr7, reg, reg_bytes, data, data_bytes);

	(void)ml_set_interrupts_enabled(was);
	return ok;
}

static bool
cam_i2c_write_reg(struct cam_bus *b, uint8_t addr7, uint16_t reg, unsigned reg_bytes, uint16_t value, unsigned value_bytes)
{
	boolean_t was = ml_set_interrupts_enabled(FALSE);
	bool ok = cam_i2c_write_reg_raw(b, addr7, reg, reg_bytes, value, value_bytes);

	(void)ml_set_interrupts_enabled(was);
	return ok;
}

/* ---- camcc: titan_top GDSC and the MCLKs ------------------------------------------------------- */

/* Bounded by time and by a count, never unbounded. */
static bool
cam_wait_bits(vm_offset_t base, uint32_t offset, uint32_t mask, bool set, unsigned us, uint32_t *last)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t value = 0;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &interval);
	for (unsigned tries = 0; tries < 100000; tries++) {
		value = rd(base + offset);
		if (set ? (value & mask) == mask : (value & mask) == 0) {
			*last = value;
			return true;
		}
		if (mach_absolute_time() - start >= interval) {
			break;
		}
		delay(10);
	}
	*last = value;
	return false;
}

static uint32_t
cam_gdsc_us(uint64_t since)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
	return (uint32_t)(ns / NSEC_PER_USEC);
}

static bool
cam_gdsc_on(void)
{
	uint32_t value, after;
	uint64_t start;

	if (camcc_va == 0) {
		printf("vayu-camera: camcc is not mapped: the titan_top GDSC is not turned on\n");
		return false;
	}
	value = rd(camcc_va + CAMCC_TITAN_TOP_GDSC);
	if ((value & CAMCC_GDSCR_POWER_ON) != 0) {
		printf("vayu-camera: camcc titan_top gdsc was on (0xc1bc = %#x)\n", value);
		return true;
	}
	wr(camcc_va + CAMCC_TITAN_TOP_GDSC, value & ~CAMCC_GDSCR_SW_COLLAPSE);
	start = mach_absolute_time();
	if (!cam_wait_bits(camcc_va, CAMCC_TITAN_TOP_GDSC, CAMCC_GDSCR_POWER_ON, true, 5000, &after)) {
		printf("vayu-camera: titan_top gdsc DID NOT COME ON (%#x)\n", after);
		return false;
	}
	g_gdsc_ours = true;
	printf("vayu-camera: titan_top gdsc on after %u us (0xc1bc = %#x)\n", cam_gdsc_us(start), after);
	return true;
}

static void
cam_gdsc_off(void)
{
	uint32_t value, after;
	uint64_t start;

	if (!g_gdsc_ours || camcc_va == 0) {
		return;
	}
	value = rd(camcc_va + CAMCC_TITAN_TOP_GDSC);
	wr(camcc_va + CAMCC_TITAN_TOP_GDSC, value | CAMCC_GDSCR_SW_COLLAPSE);
	start = mach_absolute_time();
	if (!cam_wait_bits(camcc_va, CAMCC_TITAN_TOP_GDSC, CAMCC_GDSCR_POWER_ON, false, 5000, &after)) {
		printf("vayu-camera: titan_top gdsc DID NOT COLLAPSE (%#x)\n", after);
	} else {
		printf("vayu-camera: titan_top gdsc off after %u us (0xc1bc = %#x)\n", cam_gdsc_us(start), after);
	}
	g_gdsc_ours = false;
}

/*
 * MCLKn on: cfg first (divider /1, source XO, bypass M/N), then the RCG update, then the branch.
 * Each poll is bounded at 500 us; a failure aborts the sensor that asked for the clock.
 */
static bool
cam_mclk_on(unsigned n, char *detail, size_t size)
{
	uint32_t cmd, cfg_reg, cbcr, value;

	if (n > 3 || camcc_va == 0) {
		snprintf(detail, size, "mclk%u: no such clock or camcc not mapped", n);
		return false;
	}
	if (g_mclk_on_mask & (1u << n)) {
		snprintf(detail, size, "mclk%u already running", n);
		return true;
	}
	cmd = CAMCC_MCLK_CMD(n);
	cfg_reg = CAMCC_MCLK_CFG(n);
	cbcr = CAMCC_MCLK_CBCR(n);

	wr(camcc_va + cfg_reg, CAMCC_MCLK_CFG_XO_19M2);
	value = rd(camcc_va + cmd);
	wr(camcc_va + cmd, value | CAMCC_CMD_UPDATE);
	if (!cam_wait_bits(camcc_va, cmd, CAMCC_CMD_UPDATE, false, 500, &value)) {
		printf("vayu-camera: mclk%u rcg did not update (cmd %#x)\n", n, value);
		return false;
	}
	value = rd(camcc_va + cbcr);
	wr(camcc_va + cbcr, value | GCC_CBCR_ENABLE);
	if (!cam_wait_bits(camcc_va, cbcr, GCC_CBCR_OFF, false, 500, &value)) {
		printf("vayu-camera: mclk%u branch DID NOT RUN (cbcr %#x)\n", n, value);
		return false;
	}
	snprintf(detail, size, "mclk%u cmd %#x cfg %#x cbcr %#x", n, rd(camcc_va + cmd),
	    rd(camcc_va + cfg_reg), rd(camcc_va + cbcr));
	g_mclk_on_mask |= 1u << n;
	return true;
}

/* MCLKn off: the branch only; the RCG is left as it is. */
static void
cam_mclk_off(unsigned n, char *detail, size_t size)
{
	uint32_t cbcr, value;

	if (n > 3 || camcc_va == 0) {
		snprintf(detail, size, "mclk%u: no such clock or camcc not mapped", n);
		return;
	}
	cbcr = CAMCC_MCLK_CBCR(n);
	value = rd(camcc_va + cbcr);
	wr(camcc_va + cbcr, value & ~GCC_CBCR_ENABLE);
	if (!cam_wait_bits(camcc_va, cbcr, GCC_CBCR_OFF, true, 500, &value)) {
		printf("vayu-camera: mclk%u branch did not stop (cbcr %#x)\n", n, value);
	}
	snprintf(detail, size, "mclk%u cbcr %#x", n, rd(camcc_va + cbcr));
	g_mclk_on_mask &= ~(1u << n);
}

/* ---- step 2: one sensor's power sequence ------------------------------------------------------- */

struct cam_power {
	bool vio, vana, vdig, custom2, mclk, reset, standby;
};

/*
 * One step of the module's sequence: one line, and the delay that follows it. `up' says whether
 * this is the power-up or the power-down pass; a down step whose resource was never turned on is
 * skipped silently ("only undo what was done"). False only when the sensor must not go on.
 */
static bool
cam_step_exec(const struct cam_sensor *s, const struct cam_step *st, bool up,
    struct cam_power *done, char *detail, size_t size)
{
	const char *state;
	char what[16];
	bool ok = true;

	detail[0] = 0;
	if (!up) {
		switch (st->kind) {
		case CAM_VIO: if (!done->vio) return true; break;
		case CAM_VANA: if (!done->vana) return true; break;
		case CAM_VDIG: if (!done->vdig) return true; break;
		case CAM_CUSTOM1: return true;
		case CAM_CUSTOM2: if (!done->custom2) return true; break;
		case CAM_MCLK: if (!done->mclk) return true; break;
		case CAM_RESET: if (!done->reset) return true; break;
		case CAM_STANDBY: if (!done->standby) return true; break;
		}
	}

	switch (st->kind) {
	case CAM_VIO:
		ok = cam_rail_vote(cam_rail_find("ldoc1"), st->value, detail, size);
		if (up) {
			done->vio = true;   /* even a vote not seen DONE is voted off again */
		}
		break;
	case CAM_VANA: {
		struct cam_rail *r;
		char one[80];
		bool acted = false;

		if (s->vana_gpio >= 0) {
			ok = cam_gpio_set(s->vana_gpio, st->value, one, sizeof(one));
			acted = true;
			cam_append(detail, size, "%s", one);
		}
		if (ok && s->vana_rail != NULL) {
			r = cam_rail_find(s->vana_rail);
			ok = cam_rail_vote(r, st->value, one, sizeof(one));
			acted = true;
			cam_append(detail, size, "%s%s", cam_sep(detail), one);
		}
		if (s->vana_gpio < 0 && s->vana_rail == NULL) {
			cam_append(detail, size, "no VANA pin or supply in the DT: skipped");
		}
		/*
		 * Even a step that failed after the pin was driven must be undone: `acted', not
		 * `ok', is what the power-down pass looks at.
		 */
		if (up && acted) {
			done->vana = true;
		}
		break;
	}
	case CAM_VDIG: {
		struct cam_rail *r;
		char one[80];
		bool acted = false;

		if (s->vdig_rail != NULL) {
			r = cam_rail_find(s->vdig_rail);
			ok = cam_rail_vote(r, st->value, one, sizeof(one));
			acted = true;
			cam_append(detail, size, "%s", one);
		}
		if (ok && s->vdig_gpio >= 0) {
			ok = cam_gpio_set(s->vdig_gpio, st->value, one, sizeof(one));
			acted = true;
			cam_append(detail, size, "%s%s", cam_sep(detail), one);
		}
		if (s->vdig_rail == NULL && s->vdig_gpio < 0) {
			cam_append(detail, size, "no VDIG pin or supply in the DT: skipped");
		}
		if (up && acted) {
			done->vdig = true;
		}
		break;
	}
	case CAM_CUSTOM1:
		/*
		 * No supply of any of these five is CUSTOM1 in the vendor DT: the step is kept in the
		 * sequence (and in the log) and does nothing. The long note is said once per run.
		 */
		if (!g_custom1_said) {
			cam_append(detail, size, "no custom1 supply in the DT: skipped");
			g_custom1_said = true;
		} else {
			cam_append(detail, size, "no custom1 supply in the DT");
		}
		break;
	case CAM_CUSTOM2:
		if (s->custom2_rail != NULL) {
			char one[80];

			ok = cam_rail_vote(cam_rail_find(s->custom2_rail), st->value, one, sizeof(one));
			cam_append(detail, size, "%s", one);
			if (up) {
				done->custom2 = true;
			}
		} else {
			cam_append(detail, size, "no CUSTOM2 supply in the DT: skipped");
		}
		break;
	case CAM_MCLK:
		if (up) {
			ok = cam_mclk_on(s->mclk, detail, size);
			done->mclk = true;  /* a branch enabled but not seen running is switched off again */
		} else {
			cam_mclk_off(s->mclk, detail, size);
			done->mclk = false;
		}
		break;
	case CAM_RESET:
		if (s->reset < 0) {
			cam_append(detail, size, "no reset pin");
		} else {
			ok = cam_gpio_set((uint32_t)s->reset, st->value, detail, size);
			if (ok) {
				done->reset = true;
			}
		}
		break;
	case CAM_STANDBY:
		if (s->standby < 0) {
			cam_append(detail, size, "no standby pin");
		} else {
			ok = cam_gpio_set((uint32_t)s->standby, st->value, detail, size);
			if (ok) {
				done->standby = true;
			}
		}
		break;
	}

	state = (st->kind == CAM_RESET || st->kind == CAM_STANDBY) ? (st->value ? "high" : "low") :
	    (st->value ? "on" : "off");
	if (st->kind == CAM_MCLK) {
		snprintf(what, sizeof(what), "MCLK%u", s->mclk);
	} else {
		snprintf(what, sizeof(what), "%s", cam_step_name(st->kind));
	}
	printf("vayu-camera: %s: %s: %s %s (%s)\n", s->name, up ? "up" : "down", what, state, detail);

	if (ok && st->delay_ms != 0) {
		IOSleep(st->delay_ms);
	}
	return ok;
}

/* ---- step 2: the pin check --------------------------------------------------------------------- */

static bool
cam_pin_complain(const struct cam_sensor *s, uint32_t pin, uint32_t cfg)
{
	printf("vayu-camera: %s: gpio%u is not set up by the loader (cfg %#x): "
	    "needs vayu_cam_pins=1; sensor skipped\n", s->name, pin, cfg);
	return false;
}

/*
 * Read-only: the outputs the sensor uses must be function 0 with OE 1, SDA/SCL function 0 with
 * pull-up, and its MCLK function 1. Nothing is repaired here; the loader's vayu_cam_pins=1 does.
 */
static bool
cam_sensor_pins_ok(const struct cam_sensor *s)
{
	int outputs[] = { s->reset, s->standby, s->vdig_gpio, s->vana_gpio };
	vm_offset_t page;
	uint32_t cfg;

	for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
		if (outputs[i] < 0) {
			continue;
		}
		page = cam_pin_page((uint32_t)outputs[i]);
		if (page == 0) {
			return cam_pin_complain(s, (uint32_t)outputs[i], 0);
		}
		cfg = rd(page);
		if (((cfg >> TLMM_CFG_FUNC_SHFT) & TLMM_CFG_FUNC_MASK) != 0 ||
		    ((cfg >> TLMM_CFG_OE_SHFT) & 1) == 0) {
			return cam_pin_complain(s, (uint32_t)outputs[i], cfg);
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		uint32_t pin = i == 0 ? s->sda : s->scl;

		page = cam_pin_page(pin);
		if (page == 0) {
			return cam_pin_complain(s, pin, 0);
		}
		cfg = rd(page);
		if (((cfg >> TLMM_CFG_FUNC_SHFT) & TLMM_CFG_FUNC_MASK) != 0 ||
		    (cfg & TLMM_CFG_PULL_MASK) != 3) {
			return cam_pin_complain(s, pin, cfg);
		}
	}
	page = cam_pin_page(13u + s->mclk);         /* GPIO 13-16: MCLK0-3 */
	if (page == 0) {
		return cam_pin_complain(s, 13u + s->mclk, 0);
	}
	cfg = rd(page);
	if (((cfg >> TLMM_CFG_FUNC_SHFT) & TLMM_CFG_FUNC_MASK) != 1) {
		return cam_pin_complain(s, 13u + s->mclk, cfg);
	}
	return true;
}

/* ---- step 2: one sensor ------------------------------------------------------------------------ */

/*
 * After a power-down, whatever its sequence said: every output pin of this run still high goes low
 * (TLMM exception 3: the outputs end low on every path; s5k3t2's sequence ends with STANDBY high).
 */
static void
cam_outputs_low(const struct cam_sensor *s)
{
	char detail[80];

	for (unsigned i = 0; i < CAM_PIN_COUNT; i++) {
		if ((g_raised & (1u << i)) == 0) {
			continue;
		}
		(void)cam_gpio_set(cam_pins[i].pin, 0, detail, sizeof(detail));
		printf("vayu-camera: %s: left high after the power-down: %s\n", s->name, detail);
	}
}

/*
 * The module's power-up, shared by steps 2 and 4: one line per step, and from the first step on
 * g_sensor_powered says that the quiesce has something to stop. The steps that were done stay in
 * `done' for the power-down.
 */
static bool
cam_sensor_power_up(const struct cam_sensor *s, struct cam_power *done)
{
	char detail[192];
	bool ok = true;

	g_sensor_powered = true;
	for (unsigned i = 0; i < s->up_count; i++) {
		if (!cam_step_exec(s, &s->up[i], true, done, detail, sizeof(detail))) {
			ok = false;
			break;
		}
	}
	return ok;
}

/* The module's power-down, shared by steps 2 and 4: only what was done, the outputs low last. */
static void
cam_sensor_power_down(const struct cam_sensor *s, struct cam_power *done)
{
	char detail[192];

	for (unsigned i = 0; i < s->down_count; i++) {
		(void)cam_step_exec(s, &s->down[i], false, done, detail, sizeof(detail));
	}
	cam_outputs_low(s);
	g_sensor_powered = false;
}

static void
cam_sensor_run(const struct cam_sensor *s, unsigned *answered, unsigned *matched)
{
	struct cam_power done = { 0 };
	struct cam_bus bus;
	uint8_t data[2] = { 0, 0 };
	char bytes[32];
	uint32_t id = 0;
	bool up_ok;

	if (s->skip != NULL) {
		printf("vayu-camera: %s: not probed: %s\n", s->name, s->skip);
		return;
	}
	if (!cam_sensor_pins_ok(s)) {
		return;
	}

	bus.sda = s->sda;
	bus.scl = s->scl;
	bus.sda_pg = cam_pin_page(s->sda);
	bus.scl_pg = cam_pin_page(s->scl);
	if (bus.sda_pg == 0 || bus.scl_pg == 0) {
		printf("vayu-camera: %s: the I2C pins cannot be mapped: sensor skipped\n", s->name);
		return;
	}

	up_ok = cam_sensor_power_up(s, &done);

	if (up_ok) {
		bool acked = cam_i2c_probe(&bus, s->addr7);

		printf("vayu-camera: %s: %#x %s on gpio%u/%u\n", s->name, s->addr7,
		    acked ? "ACK" : "NO ACK", s->sda, s->scl);
		if (acked) {
			(*answered)++;
			if (s->probe_only) {
				printf("vayu-camera: %s: the id is not read (probe only: its register address is "
				    "not trusted)\n", s->name);
			} else if (cam_i2c_read_id(&bus, s->addr7, s->id_reg, s->reg_bytes, data, s->data_bytes)) {
				for (unsigned i = 0; i < s->data_bytes; i++) {
					id = (id << 8) | data[i];
				}
				if (id == s->id) {
					(*matched)++;
				}
				cam_bytes(bytes, sizeof(bytes), data, s->data_bytes);
				printf("vayu-camera: %s: chip id %#x at %#x (expected %#x): %s (bytes %s)\n",
				    s->name, id, s->id_reg, s->id, id == s->id ? "MATCH" : "MISMATCH", bytes);
			} else {
				printf("vayu-camera: %s: the id at %#x was not read\n", s->name, s->id_reg);
			}
		} else {
			printf("vayu-camera: %s: nobody at %#x (SDA %u, SCL %u)\n", s->name, s->addr7,
			    cam_pin_level(s->sda), cam_pin_level(s->scl));
		}
		if (g_hold_ms != 0) {
			IOSleep(g_hold_ms);
		}
	} else {
		printf("vayu-camera: %s: power-up did not finish: powering down what was done\n", s->name);
	}

	cam_sensor_power_down(s, &done);
}

/* ---- step 2: the rails and the whole run ------------------------------------------------------- */

/*
 * One address per rail, from the command database only; any zero stops the step (`what').
 * with_csi adds ldoc3, which only step 4 uses: step 2's set is exactly the four rails it always had.
 */
static bool
cam_rails_lookup(const char *what, bool with_csi)
{
	bool ok = true;

	for (unsigned i = 0; i < CAM_RAIL_COUNT; i++) {
		if (i == CAM_RAIL_CSI && !with_csi) {
			continue;
		}
		cam_rails[i].addr = vayu_rpmh_address(cam_rails[i].id);
		if (cam_rails[i].addr == 0) {
			printf("vayu-camera: no cmd-db address for %s: %s not done\n", cam_rails[i].id, what);
			ok = false;
		}
	}
	return ok;
}

static void
cam_step2(void)
{
	unsigned selected = 0, answered = 0, matched = 0;

	if (!g_xnu_tlmm) {
		printf("vayu-camera: step 2 needs vayu_camera_xnu_tlmm=1 (TLMM writes): not done\n");
		return;
	}
	if (!g_camcc_ok || camcc_va == 0) {
		printf("vayu-camera: the camera AHB clock was not enabled in step 1: "
		    "step 2 not done\n");
		return;
	}
	if (!cam_rails_lookup("step 2", false)) {
		return;
	}

	/* Nothing has been written up to here; the loader's log is about to be the only witness. */
	printf("vayu-camera: about to write (rails, camcc, pins) in %u ms\n", g_pause_ms);
	if (g_pause_ms != 0) {
		IOSleep(g_pause_ms);
	}

	if (!cam_gdsc_on()) {
		printf("vayu-camera: step 2 not done: the titan_top GDSC did not come on\n");
		return;
	}

	for (unsigned i = 0; i < sizeof(cam_sensors) / sizeof(cam_sensors[0]); i++) {
		if ((g_sensor_mask & (1u << i)) == 0) {
			continue;
		}
		selected++;
		cam_sensor_run(&cam_sensors[i], &answered, &matched);
	}

	cam_gdsc_off();
	printf("vayu-camera: step 2 done: %u of %u sensors answered, %u ids matched\n",
	    answered, selected, matched);
}

/* ---- step 3: the camera's clocks (PLL0, the RCGs and their branches) ---------------------------- */

/*
 * The registers step 3 writes, with their sources. Nothing here belongs to a sensor: the CSID's own
 * test pattern generator stands in for one, so no CSIPHY, no CCI, no rail vote and no TLMM write
 * happens in this step.
 *
 * GCC: the camera's two AXI branches (gcc-sm8150.c:1684-1703). camcc: the five RCGs that carry the
 * frame and their seven branches. camcc's parent map 0 is 0 XO (19.2 MHz), 1 PLL0 main, 2 PLL0
 * even, 3 PLL0 odd (vendor camcc-sm8150.c:91-97), and the tables that give each RCG its source and
 * divider are :544-552, :625-629, :770-776, :971-978, :1290-1294:
 *
 *   name               cmd      from XO: src, field   from PLL0 (svs): src, field   rate
 *   slow_ahb           0x7058   0, 1                 2, 14                    80 MHz (600 / 7.5)
 *   camnoc_axi         0xc170   0, 1                 2, 7                    150 MHz (600 / 4)
 *   cphy_rx            0xa064   0, 1                 3, 1                    400 MHz (400 / 1)
 *   ife_lite_0_csid    0xc020   0, 1                 3, 1                    400 MHz
 *   ife_lite_0         0xc004   0, 1                 3, 1                    400 MHz
 *
 * cfg (cmd + 4) = source << 8 | (2 * div - 1), mode [13:12] = 0 (bypass M/N); cmd bit 0 UPDATE,
 * bit 31 ROOT_OFF. From XO a /1 is the field 1, so its cfg is just 0x1.
 */
#define GCC_CAMERA_HF_AXI_CBCR  0xb030          /* gcc-sm8150.c:1684-1703 */
#define GCC_CAMERA_SF_AXI_CBCR  0xb034

#define CAMCC_CFG_MASK          0x371fu         /* [4:0] src_div, [10:8] src_sel, [13:12] mode */

struct cam_rcg {
	const char *name;
	uint32_t cmd;
	uint32_t xo_cfg;        /* source 0 (XO) and the divider's field, mode 0 */
	uint32_t svs_cfg;       /* source 2 (PLL0 even) or 3 (PLL0 odd) */
	uint32_t found_cfg;     /* what the cfg read before this run wrote it: put back by clocks_down() */
	bool set;
	bool with_phy;          /* step 4 only: the CSIPHY's clocks (step 3 leaves them alone) */
};

static struct cam_rcg cam_rcgs[] = {
	{ "slow_ahb 80 MHz",          0x7058, 0x001u, 2u << 8 | 14u },
	{ "camnoc_axi 150 MHz",       0xc170, 0x001u, 2u << 8 | 7u },
	{ "cphy_rx 400 MHz",          0xa064, 0x001u, 3u << 8 | 1u },
	{ "ife_lite_0_csid 400 MHz",  0xc020, 0x001u, 3u << 8 | 1u },
	{ "ife_lite_0 400 MHz",       0xc004, 0x001u, 3u << 8 | 1u },
	/* csi1phytimer: source 2 (PLL0 even, 600 MHz), field 3 (/2) = 300 MHz (camcc-sm8150.c:651-656, :678) */
	{ .name = "csi1phytimer 300 MHz", .cmd = 0x6028, .xo_cfg = 0x001u, .svs_cfg = 2u << 8 | 3u, .with_phy = true },
};

struct cam_branch {
	const char *name;
	bool gcc;               /* the register is in GCC (gcc_va), not in camcc (camcc_va) */
	uint32_t cbcr;
	bool set;               /* this run turned it on: clocks_down() turns it off again */
	bool with_phy;          /* step 4 only */
	bool is_phy;            /* step 4: the CSIPHY's own branch, not its timer's */
};

static struct cam_branch cam_branches[] = {
	{ "gcc camera_hf_axi",  true,  GCC_CAMERA_HF_AXI_CBCR },
	{ "gcc camera_sf_axi",  true,  GCC_CAMERA_SF_AXI_CBCR },
	{ "cpas_ahb",           false, 0xc168 },
	{ "camnoc_axi",         false, 0xc18c },
	{ "ife_lite_0_csid",    false, 0xc038 },
	{ "ife_lite_0_cphy_rx", false, 0xc040 },
	{ "ife_lite_0",         false, 0xc01c },
	/* step 4's: csiphy1/2's CBCR 0x6044/0x6068 (camcc-sm8150.c:1583, :1601), csi1/2phytimer's
	 * 0x6040/0x6064 (:1511, :1529); cam_step4_clocks() fills them from the camera's row. */
	{ .name = "csiphy1", .cbcr = 0x6044, .with_phy = true, .is_phy = true },
	{ .name = "csi1phytimer", .cbcr = 0x6040, .with_phy = true },
};

/*
 * camcc's PLL0 (Trion, at camcc + 0x0): 19.2 MHz * (0x3e + 0x8000/2^16) = 1200 MHz, its even
 * output /2 = 600 MHz and its odd one /3 = 400 MHz (camcc-sm8150.c:44-65, :67-99). The
 * configuration is the phone's v2 one (camcc-sm8150.c:179-191); the offsets are clk-alpha-pll.c's
 * TRION defines (:77-99) and the three sequences are clk_trion_pll_configure() (:827-917),
 * clk_trion_pll_enable() (:919-983) and clk_trion_pll_disable() (:985-1019).
 */
#define CAMCC_PLL0_MODE             0x00        /* PLL_MODE */
#define CAMCC_PLL0_L_VAL            0x04
#define CAMCC_PLL0_CAL_L_VAL        0x08        /* TRION_PLL_CAL_L_VAL */
#define CAMCC_PLL0_USER_CTL         0x0c        /* TRION_PLL_USER_CTL */
#define CAMCC_PLL0_USER_CTL_U       0x10
#define CAMCC_PLL0_USER_CTL_U1      0x14
#define CAMCC_PLL0_CONFIG_CTL       0x18
#define CAMCC_PLL0_CONFIG_CTL_U     0x1c
#define CAMCC_PLL0_CONFIG_CTL_U1    0x20
#define CAMCC_PLL0_TEST_CTL         0x24
#define CAMCC_PLL0_TEST_CTL_U       0x28
#define CAMCC_PLL0_TEST_CTL_U1      0x2c
#define CAMCC_PLL0_OPMODE           0x38        /* TRION_PLL_OPMODE */
#define CAMCC_PLL0_ALPHA_VAL        0x40        /* TRION_PLL_ALPHA_VAL */
#define CAMCC_PLL0_CAL_VAL          0x44        /* TRION_PLL_CAL_VAL, clk-alpha-pll.c:92 */

#define CAMCC_PLL0_OUTCTRL          (1u << 0)   /* PLL_MODE bit 0: the global output (PLL_OUTCTRL) */
#define CAMCC_PLL0_RESET_N          (1u << 2)   /* PLL_MODE bit 2 (PLL_RESET_N) */
#define CAMCC_PLL0_HW_UPDATE_BYPASS (1u << 23)  /* PLL_MODE bit 23 (TRION_PLL_HW_UPDATE_LOGIC_BYPASS) */
#define CAMCC_PLL0_LOCKED           (1u << 31)  /* PLL_MODE bit 31 (PLL_LOCK_DET) */
#define CAMCC_PLL0_STANDBY          0u          /* TRION_PLL_STANDBY */
#define CAMCC_PLL0_RUN              1u          /* TRION_PLL_RUN */
#define CAMCC_PLL0_OUT_MASK         0x7u        /* USER_CTL [2:0] (TRION_PLL_OUT_MASK) */

static const struct {
	uint32_t offset;
	uint32_t value;
} cam_pll0_config[] = {                         /* camcc-sm8150.c:179-191, the v2 values */
	{ CAMCC_PLL0_L_VAL,            0x3e },
	{ CAMCC_PLL0_CAL_L_VAL,        CAMCC_PLL0_CAL_VAL },
	{ CAMCC_PLL0_ALPHA_VAL,        0x8000 },
	{ CAMCC_PLL0_CONFIG_CTL,       0x20485699 },
	{ CAMCC_PLL0_CONFIG_CTL_U,     0x2267 },
	{ CAMCC_PLL0_CONFIG_CTL_U1,    0x24 },
	{ CAMCC_PLL0_TEST_CTL,         0 },
	{ CAMCC_PLL0_TEST_CTL_U,       0 },
	{ CAMCC_PLL0_TEST_CTL_U1,      0x20 },
	{ CAMCC_PLL0_USER_CTL,         0x3100 },
	{ CAMCC_PLL0_USER_CTL_U,       0x805 },
	{ CAMCC_PLL0_USER_CTL_U1,      0xd0 },
};

static bool g_pll0_ours;                /* this run brought PLL0 up: PLL0 goes down again */

/* A GCC, camcc or SMMU register: written, then read back under `check' (redwood's camcc_any()). */
static bool
cam_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		printf("vayu-camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		return false;
	}
	return true;
}

/* redwood's smmu_put(): the SMMU's own name for a write that is read back (the read-back is cam_put()'s). */
static bool
smmu_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, uint32_t check)
{
	return cam_put(base, what, offset, value, check);
}

/* A CSID or IFE register: written, a barrier, then read back whole where the register keeps its value. */
static bool
cam_blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	__asm__ volatile ("dsb sy");
	if (check && rd(base + offset) != value) {
		printf("vayu-camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		return false;
	}
	return true;
}

/*
 * PLL0 up, in clk_trion_pll_configure()'s order (:827-917) and then its enable (:919-983):
 * configuration, hardware update bypass, output off, standby, out of reset, run, the lock at most
 * 1 ms, the three outputs, the global output. A PLL that somebody else has running is refused: this
 * file does not touch what it did not start.
 */
static bool
pll0_up(void)
{
	uint32_t value;
	bool ok = true;

	if (camcc_va == 0) {
		printf("vayu-camera: camcc is not mapped: PLL0 not started\n");
		return false;
	}
	value = rd(camcc_va + CAMCC_PLL0_MODE);
	if ((value & CAMCC_PLL0_OUTCTRL) != 0 || (rd(camcc_va + CAMCC_PLL0_OPMODE) & CAMCC_PLL0_RUN) != 0) {
		printf("vayu-camera: camcc PLL0 is running already (mode %#x, opmode %#x): somebody else's, refused\n",
		    value, rd(camcc_va + CAMCC_PLL0_OPMODE));
		return false;
	}
	g_pll0_ours = true;                     /* (from here on, any failure takes it down again) */
	for (unsigned i = 0; ok && i < sizeof(cam_pll0_config) / sizeof(cam_pll0_config[0]); i++) {
		ok = cam_put(camcc_va, "pll0", cam_pll0_config[i].offset, cam_pll0_config[i].value, ~0u);
	}
	if (ok) {
		value = rd(camcc_va + CAMCC_PLL0_MODE);
		ok = cam_put(camcc_va, "pll0 mode", CAMCC_PLL0_MODE, value | CAMCC_PLL0_HW_UPDATE_BYPASS,
		    CAMCC_PLL0_HW_UPDATE_BYPASS) &&
		    cam_put(camcc_va, "pll0 mode", CAMCC_PLL0_MODE,
		    rd(camcc_va + CAMCC_PLL0_MODE) & ~CAMCC_PLL0_OUTCTRL, CAMCC_PLL0_OUTCTRL) &&
		    cam_put(camcc_va, "pll0 opmode", CAMCC_PLL0_OPMODE, CAMCC_PLL0_STANDBY, ~0u) &&
		    cam_put(camcc_va, "pll0 mode", CAMCC_PLL0_MODE,
		    rd(camcc_va + CAMCC_PLL0_MODE) | CAMCC_PLL0_RESET_N, CAMCC_PLL0_RESET_N);
	}
	if (ok) {
		ok = cam_put(camcc_va, "pll0 opmode", CAMCC_PLL0_OPMODE, CAMCC_PLL0_RUN, ~0u);
	}
	if (ok && !cam_wait_bits(camcc_va, CAMCC_PLL0_MODE, CAMCC_PLL0_LOCKED, true, 1000, &value)) {
		printf("vayu-camera: camcc PLL0 does not lock (mode %#x)\n", value);
		ok = false;
	}
	if (ok) {
		ok = cam_put(camcc_va, "pll0 user_ctl", CAMCC_PLL0_USER_CTL,
		    rd(camcc_va + CAMCC_PLL0_USER_CTL) | CAMCC_PLL0_OUT_MASK, CAMCC_PLL0_OUT_MASK) &&
		    cam_put(camcc_va, "pll0 mode", CAMCC_PLL0_MODE,
		    rd(camcc_va + CAMCC_PLL0_MODE) | CAMCC_PLL0_OUTCTRL, CAMCC_PLL0_OUTCTRL);
	}
	if (!ok) {
		return false;
	}
	printf("vayu-camera: camcc PLL0 locked at 1200 MHz (mode %#x, opmode %#x, user_ctl %#x)\n",
	    rd(camcc_va + CAMCC_PLL0_MODE), rd(camcc_va + CAMCC_PLL0_OPMODE), rd(camcc_va + CAMCC_PLL0_USER_CTL));
	return true;
}

/* PLL0 down, in clk_trion_pll_disable()'s order (:985-1019), and only if this run started it. */
static void
pll0_down(void)
{
	if (!g_pll0_ours || camcc_va == 0) {
		return;
	}
	(void)cam_put(camcc_va, "pll0 mode", CAMCC_PLL0_MODE,
	    rd(camcc_va + CAMCC_PLL0_MODE) & ~CAMCC_PLL0_OUTCTRL, CAMCC_PLL0_OUTCTRL);
	(void)cam_put(camcc_va, "pll0 user_ctl", CAMCC_PLL0_USER_CTL,
	    rd(camcc_va + CAMCC_PLL0_USER_CTL) & ~CAMCC_PLL0_OUT_MASK, CAMCC_PLL0_OUT_MASK);
	(void)cam_put(camcc_va, "pll0 opmode", CAMCC_PLL0_OPMODE, CAMCC_PLL0_STANDBY, ~0u);
	(void)cam_put(camcc_va, "pll0 mode", CAMCC_PLL0_MODE,
	    rd(camcc_va + CAMCC_PLL0_MODE) | CAMCC_PLL0_RESET_N, CAMCC_PLL0_RESET_N);
	g_pll0_ours = false;
	printf("vayu-camera: camcc PLL0 off (mode %#x, opmode %#x)\n",
	    rd(camcc_va + CAMCC_PLL0_MODE), rd(camcc_va + CAMCC_PLL0_OPMODE));
}

/*
 * Step 4's three PHY clocks, from the selected camera's row: the csiNphytimer RCG's command and
 * the two branches' CBCRs. The names carry the CSIPHY's number so the log lines say csi1/csi2.
 * Called before clocks_up(true); clocks_down() then puts back what this set.
 */
static bool
cam_step4_clocks(const struct cam_stream_cfg *cfg, const char *step)
{
	static char rcg_name[32], phy_name[16], timer_name[16];
	bool rcg = false, phy = false, timer = false;

	snprintf(rcg_name, sizeof(rcg_name), "csi%uphytimer 300 MHz", cfg->csiphy);
	snprintf(phy_name, sizeof(phy_name), "csiphy%u", cfg->csiphy);
	snprintf(timer_name, sizeof(timer_name), "csi%uphytimer", cfg->csiphy);
	for (unsigned i = 0; i < sizeof(cam_rcgs) / sizeof(cam_rcgs[0]); i++) {
		if (!cam_rcgs[i].with_phy) {
			continue;
		}
		cam_rcgs[i].name = rcg_name;
		cam_rcgs[i].cmd = cfg->timer_cmd;
		rcg = true;
	}
	for (unsigned i = 0; i < sizeof(cam_branches) / sizeof(cam_branches[0]); i++) {
		if (!cam_branches[i].with_phy) {
			continue;
		}
		if (cam_branches[i].is_phy) {
			cam_branches[i].name = phy_name;
			cam_branches[i].cbcr = cfg->phy_cbcr;
			phy = true;
		} else {
			cam_branches[i].name = timer_name;
			cam_branches[i].cbcr = cfg->timer_cbcr;
			timer = true;
		}
	}
	if (!rcg || !phy || !timer) {
		printf("vayu-camera: %s: the clock table has no csi%uphytimer/csiphy%u entries\n",
		    step, cfg->csiphy, cfg->csiphy);
		return false;
	}
	return true;
}

/*
 * The RCGs first (cfg, update, the cfg read back), then the branches. A branch that reads enabled
 * already is left on, as redwood leaves it: somebody else's clock is not this file's to stop.
 * with_phy adds step 4's CSIPHY clocks after cam_step4_clocks() set them from the selected camera;
 * step 3's set and its log lines stay exactly as they were.
 */
static bool
clocks_up(bool with_phy)
{
	uint32_t value, want;
	unsigned rcgs = 0, branches = 0;

	if (gcc_va == 0 || camcc_va == 0) {
		printf("vayu-camera: GCC or camcc is not mapped: no clocks\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(cam_rcgs) / sizeof(cam_rcgs[0]); i++) {
		struct cam_rcg *r = &cam_rcgs[i];
		uint32_t cfg = r->cmd + 4;

		if (r->with_phy && !with_phy) {
			continue;
		}
		rcgs++;
		r->found_cfg = rd(camcc_va + cfg);
		want = (r->found_cfg & ~CAMCC_CFG_MASK) | (g_clk ? r->svs_cfg : r->xo_cfg);
		if (!cam_put(camcc_va, r->name, cfg, want, CAMCC_CFG_MASK)) {
			return false;
		}
		r->set = true;
		wr(camcc_va + r->cmd, rd(camcc_va + r->cmd) | CAMCC_CMD_UPDATE);
		if (!cam_wait_bits(camcc_va, r->cmd, CAMCC_CMD_UPDATE, false, 500, &value)) {
			printf("vayu-camera: the RCG %s does not take its configuration (cmd %#x)\n", r->name, value);
			return false;
		}
		printf("vayu-camera: rcg %s: cmd %#x cfg %#x -> %#x (source %u, divider field %u)\n", r->name,
		    r->cmd, r->found_cfg, rd(camcc_va + cfg),
		    (rd(camcc_va + cfg) & CAMCC_CFG_SRC_MASK) >> CAMCC_CFG_SRC_SHFT, rd(camcc_va + cfg) & CAMCC_CFG_DIV_MASK);
	}
	for (unsigned i = 0; i < sizeof(cam_branches) / sizeof(cam_branches[0]); i++) {
		struct cam_branch *b = &cam_branches[i];
		vm_offset_t base = b->gcc ? gcc_va : camcc_va;

		if (b->with_phy && !with_phy) {
			continue;
		}
		branches++;
		value = rd(base + b->cbcr);
		if ((value & GCC_CBCR_ENABLE) != 0) {
			printf("vayu-camera: branch %s: cbcr %#x = %#x (on already: left on)\n", b->name, b->cbcr, value);
			continue;
		}
		wr(base + b->cbcr, value | GCC_CBCR_ENABLE);
		if (!cam_wait_bits(base, b->cbcr, GCC_CBCR_ENABLE, true, 500, &value)) {
			printf("vayu-camera: the branch %s does not take its enable (cbcr %#x = %#x)\n",
			    b->name, b->cbcr, value);
			return false;
		}
		b->set = true;          /* (an enable that took: the teardown must turn it off even if it does not come on) */
		if (!cam_wait_bits(base, b->cbcr, GCC_CBCR_OFF, false, 500, &value)) {
			printf("vayu-camera: the branch %s does not come on (cbcr %#x = %#x)\n", b->name, b->cbcr, value);
			return false;
		}
		printf("vayu-camera: branch %s: cbcr %#x -> %#x (on)\n", b->name, b->cbcr, value);
	}
	printf("vayu-camera: the camera's clocks on (%u RCGs, %u branches) from %s\n",
	    rcgs, branches, g_clk ? "camcc PLL0" : "XO");
	return true;
}

/* Our branches off first, then every RCG's cfg put back as it was found and updated. */
static void
clocks_down(void)
{
	uint32_t value;

	for (int i = (int)(sizeof(cam_branches) / sizeof(cam_branches[0])) - 1; i >= 0; i--) {
		struct cam_branch *b = &cam_branches[i];
		vm_offset_t base;

		if (!b->set) {
			continue;
		}
		base = b->gcc ? gcc_va : camcc_va;
		wr(base + b->cbcr, rd(base + b->cbcr) & ~GCC_CBCR_ENABLE);
		printf("vayu-camera: branch %s off: cbcr %#x = %#x\n", b->name, b->cbcr, rd(base + b->cbcr));
		b->set = false;
	}
	for (int i = (int)(sizeof(cam_rcgs) / sizeof(cam_rcgs[0])) - 1; i >= 0; i--) {
		struct cam_rcg *r = &cam_rcgs[i];

		if (!r->set) {
			continue;
		}
		wr(camcc_va + r->cmd + 4, r->found_cfg);
		wr(camcc_va + r->cmd, rd(camcc_va + r->cmd) | CAMCC_CMD_UPDATE);
		(void)cam_wait_bits(camcc_va, r->cmd, CAMCC_CMD_UPDATE, false, 500, &value);
		printf("vayu-camera: rcg %s put back: cfg %#x = %#x\n", r->name, r->cmd + 4, rd(camcc_va + r->cmd + 4));
		r->set = false;
	}
	printf("vayu-camera: the camera's clocks off\n");
}

/* ---- step 3: the frames' memory ---------------------------------------------------------------- */

#define FRAME_ADDRESSES         0x40000000ull   /* the IFE's addresses for it (inside the DT's IFE pool) */
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2

/* What the loader hands to the host (tools/camraw.py reads it). */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, packed (four pixels in five bytes) */
	uint32_t        bayer;          /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        frames;         /* how many were written whole */
	uint32_t        frame_bytes;    /* each, from CAM_HEADER_BYTES on */
	uint32_t        source;         /* 0: the CSID's test pattern, 1: the sensor */
	char            sensor[16];
	uint32_t        last;           /* which of the frames' places holds the last one written */
};

static vm_offset_t g_frames;
static uint64_t g_frames_bytes;

/*
 * The two frames and their header, contiguous (the IFE writes 4 KB headers and rows as they come),
 * handed to the loader; what the processor may have in its caches is written out and invalidated
 * first, since the IFE writes past the cache.
 */
static bool
frames_alloc(uint32_t width, uint32_t height, uint32_t stride, uint32_t source, const char *sensor)
{
	struct cam_header *h;
	uint32_t frame_bytes = stride * height;

	g_frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes);
	if (kmem_alloc_contig(kernel_map, &g_frames, g_frames_bytes, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("vayu-camera: no memory for %u frames (%llu KB)\n", CAM_FRAMES,
		    (unsigned long long)(g_frames_bytes >> 10));
		g_frames = 0;
		g_frames_bytes = 0;
		return false;
	}
	h = (struct cam_header *)g_frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	/*
	 * 0: RGGB. The CSID test pattern's own order, and HYPOTHESIS for the IMX582: its module's
	 * colorFilterArrangement is 3, which CamX's table (3, 2, 1, 0) turns into RGGB.
	 */
	h->bayer = 0;
	h->frame_bytes = frame_bytes;
	h->source = source;
	strlcpy(h->sensor, sensor, sizeof(h->sensor));
	for (vm_offset_t line = g_frames; line < g_frames + g_frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(g_frames);
	loader_recording[1] = g_frames_bytes;
	loader_recording[2] = (uint64_t)g_frames;
	printf("vayu-camera: %llu KB of memory for %u frames at %#llx, handed to the loader for the host\n",
	    (unsigned long long)(g_frames_bytes >> 10), CAM_FRAMES, (unsigned long long)loader_recording[0]);
	return true;
}

/* ---- step 3: the apps SMMU --------------------------------------------------------------------- */

/*
 * The apps SMMU at 0x15000000: the block vayu_audio_smmu_925-1160.c drives for the DSP, and its
 * layout is read from IDR0/IDR1 as that code reads it -- a free match register, a free context,
 * and "IT DID NOT TAKE" when a write does not read back (the hypervisor may own the block). Where
 * redwood's copy assumed a 1 MB layout, this one follows vayu's. The IFEs' eight stream IDs are
 * 0x840 under mask 0x620 (vendor sm8150-camera.dtsi:325-331), so one match register covers them.
 */
#define SMMU_PHYS               0x15000000UL    /* dt: apps-smmu@15000000 (vayu_audio_smmu_925-1160.c:15) */
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))               /* (in the second page) */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))       /* (in the second page) */
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

#define IFE_STREAM              0x840
#define IFE_STREAM_MASK         0x620

static vm_offset_t g_smmu_va, g_smmu_second, g_smmu_context, g_smmu_tables;
static uint32_t g_smmu_match = ~0u, g_smmu_context_n, g_smmu_old_smr, g_smmu_old_s2cr, g_smmu_old_sctlr;
static bool g_smmu_on, g_smmu_bad, g_smmu_said;

/* Reads only: is one of the IFEs' streams matched already, and which match register and context are free. */
static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts, page, pages;

	if (g_smmu_va == 0) {
		g_smmu_va = ml_io_map(SMMU_PHYS, 0x2000);
		if (g_smmu_va == 0) {
			printf("vayu-camera: the SMMU cannot be mapped at %#x\n", (unsigned)SMMU_PHYS);
			return false;
		}
	}
	scr0 = rd(g_smmu_va + SMMU_SCR0);
	idr0 = rd(g_smmu_va + SMMU_IDR0);
	idr1 = rd(g_smmu_va + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	printf("vayu-camera: smmu: scr0 %#x idr0 %#x idr1 %#x: %u match registers, %u contexts\n",
	    scr0, idr0, idr1, matches, contexts);
	if ((scr0 & (1u << 3)) != 0) {
		printf("vayu-camera: smmu: 16-bit stream numbers (EXIDS), which this file is not written for\n");
		return false;
	}
	page = (idr1 >> 31) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	g_smmu_match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(g_smmu_va + SMMU_SMR(n)), s2cr = rd(g_smmu_va + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

		if ((smr >> 31) != 0) {
			used[s2cr & 0xff] = true;
			if (((stream ^ IFE_STREAM) & ~mask) == 0) {
				printf("vayu-camera: smmu: match register %u covers the IFEs' streams already "
				    "(smr %#x, s2cr %#x): somebody's, left alone\n", n, smr, s2cr);
				return false;
			}
		} else if (g_smmu_match == ~0u) {
			g_smmu_match = n;
			g_smmu_old_smr = smr;
			g_smmu_old_s2cr = s2cr;
		}
	}
	for (g_smmu_context_n = 0; g_smmu_context_n < contexts && used[g_smmu_context_n]; g_smmu_context_n++) {
	}
	if (g_smmu_match == ~0u || g_smmu_context_n >= contexts) {
		printf("vayu-camera: smmu: no match register or no context left\n");
		return false;
	}
	g_smmu_second = ml_io_map(SMMU_PHYS + page, 0x1000);
	g_smmu_context = ml_io_map(SMMU_PHYS + (uint64_t)(pages + g_smmu_context_n) * page, 0x1000);
	if (g_smmu_second == 0 || g_smmu_context == 0) {
		printf("vayu-camera: smmu: match register %u and context %u cannot be mapped\n",
		    g_smmu_match, g_smmu_context_n);
		return false;
	}
	g_smmu_old_sctlr = rd(g_smmu_context + SMMU_CB_SCTLR);
	printf("vayu-camera: smmu: free: match register %u (smr %#x, s2cr %#x), context %u at %#x (sctlr %#x)\n",
	    g_smmu_match, g_smmu_old_smr, g_smmu_old_s2cr, g_smmu_context_n,
	    (unsigned)(SMMU_PHYS + (pages + g_smmu_context_n) * page), g_smmu_old_sctlr);
	if ((g_smmu_old_sctlr & 1) != 0) {
		printf("vayu-camera: smmu: context %u is on although no stream uses it: somebody's, left alone\n",
		    g_smmu_context_n);
		return false;
	}
	return true;
}

/*
 * `bytes' of kernel memory at `base' behind FRAME_ADDRESSES for the IFEs' streams, through the
 * free context with page tables of ours: the context first, then the match register, as vayu's
 * audio code does it -- and the "IT DID NOT TAKE" check over the four registers that matter.
 * Step 3 and step 4 pass g_frames/g_frames_bytes; step 6 passes its whole ring.
 */
static bool
smmu_up(vm_offset_t base, uint64_t bytes)
{
	uint64_t *root, *second, *third, table_root;
	uint32_t thirds = (uint32_t)((bytes + 0x1fffff) >> 21), pages = (uint32_t)(bytes >> 12);
	uint32_t value, sees = 1u << 31 | (uint32_t)IFE_STREAM_MASK << 16 | IFE_STREAM;

	if (bytes == 0 || base == 0) {
		printf("vayu-camera: smmu: no frame memory to map\n");
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	if (kmem_alloc(kernel_map, &g_smmu_tables, (2 + thirds) * 4096,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("vayu-camera: smmu: no memory for the page tables\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	root = (uint64_t *)g_smmu_tables;
	second = (uint64_t *)(g_smmu_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		third = (uint64_t *)(g_smmu_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(base + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	for (vm_offset_t line = g_smmu_tables; line < g_smmu_tables + (2 + thirds) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(g_smmu_tables) | (uint64_t)g_smmu_context_n << 48;
	printf("vayu-camera: smmu: context %u gets our tables for %llu KB at %#llx; match register %u sends "
	    "streams %#x/%#x through it\n", g_smmu_context_n, (unsigned long long)(bytes >> 10), FRAME_ADDRESSES,
	    g_smmu_match, IFE_STREAM, IFE_STREAM_MASK);
	if (!smmu_put(g_smmu_context, "smmu sctlr", SMMU_CB_SCTLR, 0, ~0u) ||
	    !smmu_put(g_smmu_second, "smmu cba2r", SMMU_CBA2R(g_smmu_context_n), SMMU_CBA2R_64_BIT, ~0u) ||
	    !smmu_put(g_smmu_second, "smmu cbar", SMMU_CBAR(g_smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) ||
	    /* (TCR2 is checked on its own bits only: the rest are RES0) */
	    !smmu_put(g_smmu_context, "smmu tcr2", SMMU_CB_TCR2, SMMU_TCR2, SMMU_TCR2) ||
	    !smmu_put(g_smmu_context, "smmu tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) ||
	    !smmu_put(g_smmu_context, "smmu ttbr0", SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) ||
	    !smmu_put(g_smmu_context, "smmu ttbr0 high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) ||
	    !smmu_put(g_smmu_context, "smmu ttbr1", SMMU_CB_TTBR1, 0, ~0u) ||
	    !smmu_put(g_smmu_context, "smmu ttbr1 high", SMMU_CB_TTBR1 + 4, 0, ~0u) ||
	    !smmu_put(g_smmu_context, "smmu mair0", SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) ||
	    !smmu_put(g_smmu_context, "smmu mair1", SMMU_CB_MAIR1, 0, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	wr(g_smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(g_smmu_context + SMMU_CB_TLBIASID, g_smmu_context_n);
	wr(g_smmu_context + SMMU_CB_TLBSYNC, 1);
	if (!cam_wait_bits(g_smmu_context, SMMU_CB_TLBSTATUS, 1, false, 10000, &value)) {
		printf("vayu-camera: smmu: it does not finish forgetting old translations (tlbstatus %#x)\n", value);
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	if (!smmu_put(g_smmu_context, "smmu sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	g_smmu_on = true;
	if (!smmu_put(g_smmu_va, "smmu s2cr", SMMU_S2CR(g_smmu_match), g_smmu_context_n, 0x300ff) ||
	    !smmu_put(g_smmu_va, "smmu smr", SMMU_SMR(g_smmu_match), sees, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	if (rd(g_smmu_va + SMMU_SMR(g_smmu_match)) != sees ||
	    (rd(g_smmu_va + SMMU_S2CR(g_smmu_match)) & 0x300ff) != g_smmu_context_n ||
	    (rd(g_smmu_context + SMMU_CB_SCTLR) & 1) == 0 ||
	    rd(g_smmu_context + SMMU_CB_TTBR0) != (uint32_t)table_root) {
		printf("vayu-camera: smmu: IT DID NOT TAKE WHAT WAS WRITTEN (smr %#x s2cr %#x sctlr %#x ttbr0 %#x): "
		    "the hypervisor may own it\n", rd(g_smmu_va + SMMU_SMR(g_smmu_match)),
		    rd(g_smmu_va + SMMU_S2CR(g_smmu_match)), rd(g_smmu_context + SMMU_CB_SCTLR),
		    rd(g_smmu_context + SMMU_CB_TTBR0));
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	printf("vayu-camera: smmu: streams %#x/%#x go through context %u with our page tables\n",
	    IFE_STREAM, IFE_STREAM_MASK, g_smmu_context_n);
	return true;
}

/* The "nothing" line once, a fault at any time (the teardown asks too). */
static bool
smmu_faults(void)
{
	uint32_t status, global;

	if (!g_smmu_on) {
		return false;
	}
	status = rd(g_smmu_context + SMMU_CB_FSR);
	global = rd(g_smmu_va + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		if (!g_smmu_said) {
			g_smmu_said = true;
			printf("vayu-camera: smmu: the IFE was refused nothing (context %u, fsr 0, sgfsr 0)\n",
			    g_smmu_context_n);
		}
		return false;
	}
	if (g_smmu_bad) {
		return true;            /* (the teardown asks again: the fault is said once) */
	}
	g_smmu_bad = true;
	printf("vayu-camera: smmu: THE SMMU REFUSED A REQUEST: context %u status %#x address %#llx syndrome %#x; "
	    "global %#x stream %#x\n", g_smmu_context_n, status,
	    (unsigned long long)((uint64_t)rd(g_smmu_context + SMMU_CB_FAR + 4) << 32 |
	    rd(g_smmu_context + SMMU_CB_FAR)),
	    rd(g_smmu_context + SMMU_CB_FSYNR0), global, rd(g_smmu_va + SMMU_SGFSYNR1));
	return true;
}

/* The match register and the context back as they were found (the SCTLR as found: it read off). */
static void
smmu_down(void)
{
	if (!g_smmu_on && g_smmu_match == ~0u) {
		return;
	}
	(void)smmu_faults();
	lck_mtx_lock(&hakios_smmu_lock);
	if (g_smmu_match != ~0u) {
		wr(g_smmu_va + SMMU_SMR(g_smmu_match), g_smmu_old_smr);
		wr(g_smmu_va + SMMU_S2CR(g_smmu_match), g_smmu_old_s2cr);
	}
	if (g_smmu_on) {
		wr(g_smmu_context + SMMU_CB_SCTLR, g_smmu_old_sctlr);
		wr(g_smmu_context + SMMU_CB_TLBIASID, g_smmu_context_n);
		wr(g_smmu_context + SMMU_CB_TLBSYNC, 1);
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	g_smmu_on = false;
	g_smmu_match = ~0u;
	printf("vayu-camera: the SMMU's match register and context put back as they were found\n");
}

/* ---- step 3: CSID-lite 0 and IFE-lite 0 -------------------------------------------------------- */

/*
 * The blocks' registers: cam_ife_csid_lite17x.h's offsets (CSID) and cam_vfe_lite17x.h's (IFE),
 * the same as redwood's. The TPG, RDI 0 and WM0 values are redwood's too; only the RCG settings
 * above and the SMMU are this phone's.
 */
#define CSID_PHYS               0x0acc8000UL
#define IFE_PHYS                0x0acc4000UL

#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_CSI2_RX_ECC        0x164
#define CSID_CSI2_RX_CRC        0x168
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COMMON_GEN_CFG 0x648
#define TPG_VC                  0xa
#define RAW10_DT                0x2b

/*
 * A line's bytes as the write master lays them out: RAW10 (four pixels in five bytes), the line
 * rounded up to 16 bytes, the bus's width (a 4000-pixel line takes 5008, not 5000).
 */
#define RAW10_STRIDE(width)     (((width) * 10 / 8 + 15) & ~15u)

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x0018
#define IFE_IRQ_MASK0           0x005c
#define IFE_IRQ_MASK1           0x0060
#define IFE_IRQ_CLEAR0          0x0064
#define IFE_IRQ_CLEAR1          0x0068
#define IFE_IRQ_STATUS0         0x006c
#define IFE_IRQ_STATUS1         0x0070
#define IFE_IRQ_GLOBAL_CLEAR    0x0058
#define IFE_VIOLATION_STATUS    0x007c
#define IFE_REG_UPDATE          0x04ac
#define BUS_HW_VERSION          0x2000
#define BUS_CGC_OVD             0x200c
#define BUS_IRQ_MASK1           0x2048
#define BUS_IRQ_CLEAR0          0x2050
#define BUS_IRQ_CLEAR1          0x2054
#define BUS_IRQ_CLEAR2          0x2058
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_IRQ_GLOBAL_CLEAR    0x2068
#define BUS_COMP_ERROR          0x206c
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080
#define BUS_ADDR_SYNC_NO_SYNC   0x2084
#define BUS_TEST_CTRL           0x211c
#define WM0_STATUS0             0x2200
#define WM0_STATUS1             0x2204
#define WM0_CFG                 0x2208
#define WM0_IMAGE_ADDR          0x2214
#define WM0_IMAGE_ADDR_OFFSET   0x2218
#define WM0_WIDTH               0x221c
#define WM0_HEIGHT              0x2220
#define WM0_PACKER              0x2224
#define WM0_STRIDE              0x2228
#define WM0_IRQ_SUB_PERIOD      0x2248
#define WM0_IRQ_SUB_PATTERN     0x224c
#define WM0_FRAMEDROP_PERIOD    0x2250
#define WM0_FRAMEDROP_PATTERN   0x2254
#define WM0_FRAME_INC           0x2258
#define WM0_BURST_LIMIT         0x225c

static vm_offset_t g_csid_va, g_ife_va;
static bool g_csid_running, g_ife_running, g_tpg_running;

/* cam_ife_csid_global_reset() and reset_regs(): interrupts masked and cleared, then the registers,
 * then the hardware. */
static bool
csid_reset(void)
{
	uint32_t value;

	cam_blk_put(g_csid_va, "CSID", CSID_CSI2_RX_IRQ_MASK, 0, false);
	for (unsigned r = 0; r < 4; r++) {
		cam_blk_put(g_csid_va, "CSID", CSID_RDI0_IRQ_MASK + 0x10 * r, 0, false);
	}
	cam_blk_put(g_csid_va, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
	cam_blk_put(g_csid_va, "CSID", CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff, false);
	for (unsigned r = 0; r < 4; r++) {
		cam_blk_put(g_csid_va, "CSID", CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff, false);
	}
	cam_blk_put(g_csid_va, "CSID", CSID_IRQ_CMD, 1, false);
	cam_blk_put(g_csid_va, "CSID", CSID_CSI2_RX_CFG1, 0x80, false);
	for (unsigned r = 0; r < 4; r++) {
		cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CFG0 + 0x100 * r, 2, false);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		cam_blk_put(g_csid_va, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		cam_blk_put(g_csid_va, "CSID", CSID_IRQ_CMD, 1, false);
		if (hw) {
			cam_blk_put(g_csid_va, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			cam_blk_put(g_csid_va, "CSID", CSID_IRQ_CMD, 1, false);
		}
		cam_blk_put(g_csid_va, "CSID", CSID_RST_STROBES, hw ? 0x1e : 0x1, false);
		if (!hw) {
			cam_blk_put(g_csid_va, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			cam_blk_put(g_csid_va, "CSID", CSID_IRQ_CMD, 1, false);
		}
		if (!cam_wait_bits(g_csid_va, CSID_TOP_IRQ_STATUS, 1, true, 100000, &value)) {
			printf("vayu-camera: the CSID's %s reset does not finish (top irq %#x)\n",
			    hw ? "hardware" : "registers'", rd(g_csid_va + CSID_TOP_IRQ_STATUS));
			return false;
		}
		cam_blk_put(g_csid_va, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		cam_blk_put(g_csid_va, "CSID", CSID_IRQ_CMD, 1, false);
	}
	printf("vayu-camera: CSID-lite 0 reset (version %#x)\n", rd(g_csid_va + CSID_HW_VERSION));
	return true;
}

/* The RX (from a PHY, or the test pattern generator: `phy' -1) and RDI 0, for RAW10 of vc/dt,
 * written as it comes (packed). */
static bool
csid_config(int phy, unsigned lanes, uint32_t lane_cfg, uint32_t vc, uint32_t width, uint32_t height, bool cphy)
{
	uint32_t cfg0 = vc << 22 | RAW10_DT << 16 | 0u << 27 | 0xfu << 12 | 0u << 10 | 1u << 2 | 3u;

	if (!cam_blk_put(g_csid_va, "CSID", CSID_CSI2_RX_CFG0, (lanes - 1) | lane_cfg << 4 | (uint32_t)cphy << 24 |
	    (uint32_t)(phy < 0 ? 0 : phy & 3) << 20, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	if (phy < 0) {
		/* cam_ife_csid_config_tpg(): one data type, frames without end, blanking 0x3ff lines and
		 * 0x740 pixels, RAW10, colour bars */
		if (!cam_blk_put(g_csid_va, "CSID", CSID_TPG_VC_CFG0, 0u << 16 | 1u << 10 | vc, true) ||
		    !cam_blk_put(g_csid_va, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) ||
		    !cam_blk_put(g_csid_va, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
		    !cam_blk_put(g_csid_va, "CSID", CSID_TPG_DT0_CFG0, width << 16 | height, true) ||
		    !cam_blk_put(g_csid_va, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
		    !cam_blk_put(g_csid_va, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) ||
		    !cam_blk_put(g_csid_va, "CSID", CSID_TPG_COLOR_BARS_CFG, 1u << 5, true) ||
		    !cam_blk_put(g_csid_va, "CSID", CSID_TPG_COMMON_GEN_CFG, 0, true)) {
			return false;
		}
	}
	/* cam_ife_csid_init_config_rdi_path() */
	if (!cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CFG0, cfg0, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	g_csid_running = true;
	/* cam_ife_csid_enable_rdi_path(): resume at the frame's boundary; the reset, overflow and
	 * count errors seen */
	return cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CTRL, g_rdi_hold ? 0 : 1, true) &&
	       cam_blk_put(g_csid_va, "CSID", CSID_RDI0_IRQ_MASK,
	       1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

/* cam_ife_csid_tpg_start(): four lanes' worth; from here on the quiesce has a pipeline to stop. */
static bool
tpg_start(void)
{
	if (!cam_blk_put(g_csid_va, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
		return false;
	}
	g_tpg_running = true;
	g_pipeline_running = true;
	printf("vayu-camera: the CSID's test pattern on (tpg ctrl %#x)\n", rd(g_csid_va + CSID_TPG_CTRL));
	return true;
}

/* The test pattern off first of all: the RDI path below it is about to be halted. */
static void
tpg_stop(void)
{
	if (!g_tpg_running || g_csid_va == 0) {
		return;
	}
	wr(g_csid_va + CSID_TPG_CTRL, 0);
	g_tpg_running = false;
	printf("vayu-camera: the CSID's test pattern off (tpg ctrl %#x)\n", rd(g_csid_va + CSID_TPG_CTRL));
}

/* cam_vfe_top_reset(), cam_vfe_bus_init_hw(), cam_vfe_bus_start_wm() and update_wm() for RDI 0 in frame mode. */
static bool
ife_config(uint64_t address, uint32_t frame_inc)
{
	uint32_t value;

	cam_blk_put(g_ife_va, "IFE", IFE_IRQ_MASK0, 1u << 31, false);
	cam_blk_put(g_ife_va, "IFE", IFE_GLOBAL_RESET, 0x3f9f, false);
	if (!cam_wait_bits(g_ife_va, IFE_IRQ_STATUS0, 1u << 31, true, 100000, &value)) {
		printf("vayu-camera: the IFE does not reset (irq %#x)\n", rd(g_ife_va + IFE_IRQ_STATUS0));
		return false;
	}
	cam_blk_put(g_ife_va, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	cam_blk_put(g_ife_va, "IFE", IFE_IRQ_CLEAR1, 0xffffffff, false);
	cam_blk_put(g_ife_va, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	printf("vayu-camera: IFE-lite 0 reset (version %#x, bus %#x)\n",
	    rd(g_ife_va + IFE_HW_VERSION), rd(g_ife_va + BUS_HW_VERSION));
	if (!cam_blk_put(g_ife_va, "IFE", BUS_ADDR_SYNC_FRAME_HDR, 0, true) ||
	    !cam_blk_put(g_ife_va, "IFE", BUS_CGC_OVD, 0xfffff, false) ||
	    !cam_blk_put(g_ife_va, "IFE", BUS_TEST_CTRL, 0, false) ||
	    !cam_blk_put(g_ife_va, "IFE", BUS_ADDR_SYNC_NO_SYNC, 0xffffff, false) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_BURST_LIMIT, 0xf, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_WIDTH, 0xff01, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_HEIGHT, 0, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_PACKER, 0, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_STRIDE, 0xff01, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_FRAMEDROP_PERIOD, 0, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_FRAMEDROP_PATTERN, 1, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_IRQ_SUB_PERIOD, 0, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_IRQ_SUB_PATTERN, 1, true) ||
	    !cam_blk_put(g_ife_va, "IFE", BUS_IRQ_MASK1, 1, false) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_IMAGE_ADDR, (uint32_t)address, false) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_FRAME_INC, frame_inc, true) ||
	    !cam_blk_put(g_ife_va, "IFE", WM0_CFG, 3, true)) {
		return false;
	}
	g_ife_running = true;
	printf("vayu-camera: ife-lite 0: wm0 at iova %#llx, frame_inc %#x\n", (unsigned long long)address, frame_inc);
	/* the RDI 0 resource's start: its register update */
	return cam_blk_put(g_ife_va, "IFE", IFE_REG_UPDATE, 0x2, false);
}

/* What the blocks say, in one line: the RX's packets, RDI 0's, the IFE's, the bus's and WM0's. */
static void
show_paths(const char *when)
{
	printf("vayu-camera: %s: CSID rx irq %#x, packets %u, ecc %#x, crc %u; RDI0 irq %#x status %#x, bytes %u/%u; "
	    "IFE irq %#x %#x, violation %#x; bus irq %#x %#x %#x, comp error %#x; WM0 status %#x %#x\n", when,
	    rd(g_csid_va + CSID_CSI2_RX_IRQ_STATUS), rd(g_csid_va + CSID_CSI2_RX_PKTS), rd(g_csid_va + CSID_CSI2_RX_ECC),
	    rd(g_csid_va + CSID_CSI2_RX_CRC), rd(g_csid_va + CSID_RDI0_IRQ_STATUS), rd(g_csid_va + CSID_RDI0_STATUS),
	    rd(g_csid_va + CSID_RDI0_BYTES_PING), rd(g_csid_va + CSID_RDI0_BYTES_PONG), rd(g_ife_va + IFE_IRQ_STATUS0),
	    rd(g_ife_va + IFE_IRQ_STATUS1), rd(g_ife_va + IFE_VIOLATION_STATUS), rd(g_ife_va + BUS_IRQ_STATUS0),
	    rd(g_ife_va + BUS_IRQ_STATUS1), rd(g_ife_va + BUS_IRQ_STATUS2), rd(g_ife_va + BUS_COMP_ERROR),
	    rd(g_ife_va + WM0_STATUS0), rd(g_ife_va + WM0_STATUS1));
}

/* The RDI path halted at once and off, then WM0 off and its register update: the blocks' own order. */
static void
paths_down(void)
{
	if (g_csid_running) {
		wr(g_csid_va + CSID_RDI0_IRQ_MASK, 0);
		wr(g_csid_va + CSID_RDI0_CTRL, (rd(g_csid_va + CSID_RDI0_CTRL) & ~3u) | 2);
		wr(g_csid_va + CSID_RDI0_CFG0, rd(g_csid_va + CSID_RDI0_CFG0) & ~(1u << 31));
		wr(g_csid_va + CSID_CSI2_RX_CFG0, 0);
		printf("vayu-camera: csid-lite 0 rdi 0 off (rdi ctrl %#x, rx cfg0 %#x)\n",
		    rd(g_csid_va + CSID_RDI0_CTRL), rd(g_csid_va + CSID_CSI2_RX_CFG0));
		g_csid_running = false;
	}
	if (g_ife_running) {
		wr(g_ife_va + WM0_CFG, 0);
		wr(g_ife_va + IFE_REG_UPDATE, 0x2);
		delay(1000);                    /* (redwood's pause: the write master's last rows) */
		printf("vayu-camera: ife-lite 0 write master 0 off (wm0 cfg %#x)\n", rd(g_ife_va + WM0_CFG));
		g_ife_running = false;
	}
}

/* ---- step 3: what the frame holds -------------------------------------------------------------- */

/*
 * After the teardown: the frame's lines out of the processor's caches (dc civac: the IFE wrote past
 * them), then one line of numbers, then the packed MIPI RAW10 unpacked -- each five bytes are four
 * pixels, byte i (0..3) holds pixel i's bits 9:2 and byte 4 the four low pairs, pixel i in bits
 * 2*i+1:2*i -- into a 16-bin histogram of pixel >> 6, with the pixels' range. Every loop is bounded
 * by the frame's bytes.
 */
static void cam_site_means(const char *who, const uint8_t *frame, uint32_t width, uint32_t height, uint32_t stride);

static void
cam_frame_summary(uint32_t width, uint32_t height, uint32_t stride, const char *label)
{
	char first[48];
	const uint8_t *frame;
	uint32_t bins[16] = { 0 };
	uint32_t bytes, nonzero = 0, lowest = 0x3ff, highest = 0, pixels = 0, hash = 2166136261u;
	unsigned line_bytes = width * 10 / 8;
	uint64_t sum = 0;

	if (g_frames == 0 || g_frames_bytes <= CAM_HEADER_BYTES || stride == 0 || height == 0) {
		printf("vayu-camera: no frame memory: no summary\n");
		return;
	}
	bytes = stride * height;
	if (CAM_HEADER_BYTES + (uint64_t)bytes > g_frames_bytes) {
		bytes = (uint32_t)(g_frames_bytes - CAM_HEADER_BYTES);
	}
	frame = (const uint8_t *)(g_frames + CAM_HEADER_BYTES);
	for (vm_offset_t line = g_frames; line < g_frames + CAM_HEADER_BYTES + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");

	for (uint32_t at = 0; at < bytes; at++) {
		hash = (hash ^ frame[at]) * 16777619u;
		nonzero += frame[at] != 0 ? 1u : 0u;
	}
	cam_bytes(first, sizeof(first), frame, 10);
	printf("vayu-camera: %s frame: %u bytes, %u nonzero, fnv1a32 %#x, first bytes %s\n",
	    label, bytes, nonzero, hash, first);

	for (uint32_t line = 0; line < height && (line + 1) * (uint64_t)stride <= bytes; line++) {
		const uint8_t *p = frame + (uint64_t)line * stride;

		for (unsigned at = 0; at + 5 <= line_bytes; at += 5) {
			for (unsigned i = 0; i < 4; i++) {
				unsigned pixel = (unsigned)p[at + i] << 2 | ((p[at + 4] >> (2 * i)) & 3u);

				bins[pixel >> 6]++;
				lowest = pixel < lowest ? pixel : lowest;
				highest = pixel > highest ? pixel : highest;
				sum += pixel;
				pixels++;
			}
		}
	}
	if (pixels == 0) {
		printf("vayu-camera: the frame has no pixel to unpack\n");
		return;
	}
	printf("vayu-camera: %s histogram (pixel>>6): %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
	    label, bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6], bins[7],
	    bins[8], bins[9], bins[10], bins[11], bins[12], bins[13], bins[14], bins[15]);
	printf("vayu-camera: %s pixels: min %u, max %u, mean %u (%u pixels)\n",
	    label, lowest, highest, (unsigned)(sum / pixels), pixels);
	cam_site_means(label, frame, width, height, stride);
}

/*
 * The mean of each of the 16 sites of a 4x4 tile of a RAW10 frame (rows y % 4, columns x % 4), over every 4th
 * tile row: which colour filter sits where (a 2x2 Bayer repeats every 2; a quad Bayer / Tetracell's 2x2 blocks
 * of one colour show as 4x4 structure). a test run: the front S5K3T2's frames read as magenta, "green 0
 * above black" in mediaserverd's ISP, so its pattern is in doubt. Bounded by the frame.
 */
static void
cam_site_means(const char *who, const uint8_t *frame, uint32_t width, uint32_t height, uint32_t stride)
{
	uint64_t sum[16] = { 0 };
	uint32_t n[16] = { 0 };
	char line[200];
	int at = 0;

	for (uint32_t y = 0; y + 4 <= height; y += 16) {
		for (uint32_t dy = 0; dy < 4; dy++) {
			const uint8_t *row = frame + (uint64_t)(y + dy) * stride;

			for (uint32_t x = 0; x + 4 <= width; x += 4) {
				const uint8_t *g = row + (x / 4) * 5;

				for (uint32_t dx = 0; dx < 4; dx++) {
					unsigned v = (unsigned)g[dx] << 2 | ((g[4] >> (2 * dx)) & 3u);

					sum[dy * 4 + dx] += v;
					n[dy * 4 + dx]++;
				}
			}
		}
	}
	for (int i = 0; i < 16; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, "%s%u", (i % 4) == 0 ? (i ? " / " : "") : " ",
		    n[i] ? (unsigned)(sum[i] / n[i]) : 0);
	}
	printf("vayu-camera: %s: 4x4 site means (rows y%%4, cols x%%4): %s\n", who, line);
}

/* ---- step 3: the whole run --------------------------------------------------------------------- */

/*
 * The teardown of step 3, always, in the order the hardware wants it: the test pattern first, then
 * the paths, the SMMU, the clocks, PLL0 if ours, the GDSC. Every part is guarded by what it set, so
 * a failure at any point takes down exactly what was done. The frame memory is not given back.
 */
static void
cam_step3_down(void)
{
	tpg_stop();
	paths_down();
	smmu_down();
	clocks_down();
	pll0_down();
	cam_gdsc_off();
	g_pipeline_running = false;
}

/*
 * One frame of the CSID's test pattern through RDI 0 and IFE-lite 0 into the frames' memory. False
 * if the frame was not seen; the caller tears everything down either way.
 */
static bool
capture_tpg(uint32_t width, uint32_t height, uint32_t stride, uint32_t frame_bytes)
{
	struct cam_header *h;
	bool done = false;

	if (g_csid_va == 0) {
		g_csid_va = ml_io_map(CSID_PHYS, 0x1000);
		g_ife_va = ml_io_map(IFE_PHYS, 0x4000);
	}
	if (g_csid_va == 0 || g_ife_va == 0) {
		printf("vayu-camera: CSID-lite 0 and IFE-lite 0 cannot be mapped: no test pattern\n");
		return false;
	}
	if (!frames_alloc(width, height, stride, 0, "CSID TPG") || !cam_gdsc_on() || (g_clk && !pll0_up()) ||
	    !clocks_up(false) || !smmu_up(g_frames, g_frames_bytes) || !csid_reset() ||
	    !csid_config(-1, 4u, 0x3210u, TPG_VC, width, height, false) ||
	    !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, frame_bytes)) {
		printf("vayu-camera: step 3: the pipeline was not configured: taking down what was done\n");
		return false;
	}
	show_paths("configured");
	if (!tpg_start()) {
		return false;
	}
	h = (struct cam_header *)g_frames;
	for (unsigned ms = 0; ms < 2000; ms += 10) {
		IOSleep(10);
		if ((rd(g_ife_va + BUS_IRQ_STATUS1) & 1u) != 0) {
			h->frames = 1;
			done = true;
			printf("vayu-camera: write master 0 says its frame is done after %u ms\n", ms + 10);
			break;
		}
	}
	if (!done) {
		printf("vayu-camera: NO FRAME after 2000 ms\n");
	}
	show_paths("after the frame");
	(void)smmu_faults();
	return done;
}

/*
 * Step 3: step 1's camcc, then no sensor at all -- the CSID's test pattern, four virtual lanes, VC
 * 0xa, RAW10 of vayu_camera_tpg_w x vayu_camera_tpg_h, IFE-lite 0's write master 0 into the frames'
 * memory behind the SMMU. The teardown always runs; the frame's summary comes after it.
 */
static void
cam_step3(void)
{
	uint32_t width = g_tpg_w, height = g_tpg_h, stride, frame_bytes;
	bool done;

	if (!g_camcc_ok || camcc_va == 0) {
		printf("vayu-camera: the camera AHB clock was not enabled in step 1: step 3 not done\n");
		return;
	}
	if (width < 16 || width > 4096 || height < 16 || height > 4096) {
		printf("vayu-camera: tpg %ux%u is not a size this file takes (16..4096): using 640x480\n", width, height);
		width = 640;
		height = 480;
	}
	stride = RAW10_STRIDE(width);
	frame_bytes = stride * height;

	printf("vayu-camera: step 3: one CSID test-pattern frame %ux%u RAW10 through IFE-lite 0 (clocks: %s)\n",
	    width, height, g_clk ? "PLL0" : "XO");
	printf("vayu-camera: about to write (camcc, GCC, SMMU, CSID, IFE) in %u ms\n", g_pause_ms);
	if (g_pause_ms != 0) {
		IOSleep(g_pause_ms);
	}

	done = capture_tpg(width, height, stride, frame_bytes);
	cam_step3_down();
	cam_frame_summary(width, height, stride, "tpg");
	printf("vayu-camera: step 3 done: frame %s, smmu faults %s\n",
	    done ? "done" : "NO FRAME", g_smmu_bad ? "SEEN" : "none");
}

/* ---- step 4: the selected camera's vendor table (fw/camera-*.tab) ------------------------------- */

/*
 * The module's own register tables, as devices/poco-x3-pro/tools/camtab.py writes them: "VYCTAB01", a little-endian
 * block count, then per block an 8-byte NUL-padded name and a little-endian entry count, then the
 * entries {u16 address; u16 value; u8 address_bytes; u8 value_bytes; u16 delay_us}, 8 bytes packed.
 * Only init, the mode block, on and off are used (TLMM exception 3's amendment): res3 is the
 * IMX582's lowest-pixel-clock mode (1920x1080 RAW10 at 60.08 fps on four lanes), res2 the S5K3T2's
 * (1280x720 RAW10 at 120.41 fps on four lanes). Values may be one or two bytes, exactly as stored:
 * the S5K3T2's stream-on is one 2-byte write, 0x0100 <- 0x0103.
 */
#define CAM_TAB_MAGIC           "VYCTAB01"
#define CAM_TAB_BLOCK_BYTES     12
#define CAM_TAB_ENTRY_BYTES     8
#define CAM_TAB_MAX_DELAY_US    100000

enum {
	CAM_TAB_INIT,
	CAM_TAB_MODE,
	CAM_TAB_ON,
	CAM_TAB_OFF,
	CAM_TAB_COUNT,
};

struct cam_tab_block {
	char name[9];           /* the 8 bytes, NUL-terminated */
	const uint8_t *entries;
	uint32_t count;
	bool found;
};

static struct cam_tab_block cam_tab[CAM_TAB_COUNT];

static uint16_t
cam_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t
cam_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One block's header and its entries, bounded by the file's `size'. */
static bool
cam_tab_block_at(const uint8_t *data, uint64_t size, uint64_t *at, struct cam_tab_block *block)
{
	if (*at > size || size - *at < CAM_TAB_BLOCK_BYTES) {
		return false;
	}
	memcpy(block->name, data + *at, 8);
	block->name[8] = 0;
	block->count = cam_le32(data + *at + 8);
	*at += CAM_TAB_BLOCK_BYTES;
	if ((uint64_t)block->count > (size - *at) / CAM_TAB_ENTRY_BYTES) {
		return false;
	}
	block->entries = data + *at;
	*at += (uint64_t)block->count * CAM_TAB_ENTRY_BYTES;
	return true;
}

/* Every entry's address and value must fit the number of bytes it says it takes. */
static bool
cam_tab_entries_ok(const struct cam_tab_block *block, const char *step)
{
	for (uint32_t i = 0; i < block->count; i++) {
		const uint8_t *e = block->entries + (uint64_t)i * CAM_TAB_ENTRY_BYTES;
		uint16_t address = cam_le16(e), value = cam_le16(e + 2);
		unsigned address_bytes = e[4], value_bytes = e[5];

		if ((address_bytes != 1 && address_bytes != 2) || (value_bytes != 1 && value_bytes != 2) ||
		    (address_bytes == 1 && address > 0xff) || (value_bytes == 1 && value > 0xff)) {
			printf("vayu-camera: %s tab: block \"%s\" entry %u: address %#x in %u byte(s), "
			    "value %#x in %u byte(s): %s not done\n", g_stream->name, block->name, i,
			    address, address_bytes, value, value_bytes, step);
			return false;
		}
	}
	return true;
}

/* The file and its four blocks (init, <mode>, on, off), before any register is written. */
static bool
cam_tab_load(const char *file, const char *mode_block, const char *step)
{
	const uint8_t *data;
	const char *names[CAM_TAB_COUNT];
	uint64_t size = 0, at;
	uint32_t blocks;
	bool ok = true;

	names[CAM_TAB_INIT] = "init";
	names[CAM_TAB_MODE] = mode_block;
	names[CAM_TAB_ON] = "on";
	names[CAM_TAB_OFF] = "off";
	memset(cam_tab, 0, sizeof(cam_tab));

	data = hakios_image_file("fw", file, &size, NULL);
	if (data == NULL || size < CAM_TAB_BLOCK_BYTES || memcmp(data, CAM_TAB_MAGIC, 8) != 0) {
		printf("vayu-camera: no fw/%s (VYCTAB01): %s not done\n", file, step);
		return false;
	}
	blocks = cam_le32(data + 8);
	if ((uint64_t)blocks > (size - CAM_TAB_BLOCK_BYTES) / CAM_TAB_BLOCK_BYTES) {
		printf("vayu-camera: fw/%s: %u blocks do not fit %llu bytes: %s not done\n",
		    file, blocks, (unsigned long long)size, step);
		return false;
	}
	at = CAM_TAB_BLOCK_BYTES;
	printf("vayu-camera: fw/%s: %llu bytes, %u blocks\n",
	    file, (unsigned long long)size, blocks);
	for (uint32_t b = 0; b < blocks && ok; b++) {
		struct cam_tab_block block = { 0 };

		if (!cam_tab_block_at(data, size, &at, &block)) {
			printf("vayu-camera: fw/%s: block %u is past the file: %s not done\n", file, b, step);
			return false;
		}
		for (unsigned n = 0; n < CAM_TAB_COUNT; n++) {
			if (strcmp(block.name, names[n]) != 0) {
				continue;
			}
			block.found = true;
			if (!cam_tab_entries_ok(&block, step)) {
				return false;
			}
			cam_tab[n] = block;
			printf("vayu-camera: %s tab: \"%s\": %u entries\n", g_stream->name, block.name,
			    block.count);
		}
	}
	for (unsigned n = 0; n < CAM_TAB_COUNT; n++) {
		if (!cam_tab[n].found) {
			printf("vayu-camera: %s tab: no \"%s\" block: %s not done\n", g_stream->name,
			    names[n], step);
			ok = false;
		}
	}
	return ok;
}

/*
 * One block: every byte acknowledged, the entry's delay after it. A NACK ends the block. TLMM
 * exception 3's amendment (maintainers, 2026-10-25; amendment 2 for the S5K3T2) allows these four blocks
 * of the selected module and nothing else.
 */
static bool
cam_tab_write_block(struct cam_bus *bus, const struct cam_sensor *s, const struct cam_tab_block *block)
{
	for (uint32_t i = 0; i < block->count; i++) {
		const uint8_t *e = block->entries + (uint64_t)i * CAM_TAB_ENTRY_BYTES;
		uint16_t address = cam_le16(e), value = cam_le16(e + 2), delay_us = cam_le16(e + 6);

		if (!cam_i2c_write_reg(bus, s->addr7, address, e[4], value, e[5])) {
			printf("vayu-camera: %s: \"%s\": NOT ACKNOWLEDGED at entry %u: %#x <- %#x\n",
			    s->name, block->name, i, address, value);
			return false;
		}
		if (delay_us != 0) {
			delay(delay_us > CAM_TAB_MAX_DELAY_US ? CAM_TAB_MAX_DELAY_US : delay_us);
		}
	}
	printf("vayu-camera: %s: \"%s\": %u writes, all acknowledged\n", s->name, block->name,
	    block->count);
	return true;
}

/* ---- step 4: CSIPHY1 or CSIPHY2 (qcom,csiphy-v1.1) --------------------------------------------- */

#define CSIPHY_SIZE             0x1000UL        /* both 0x0ac66000 and 0x0ac67000 (vendor dtsi:47-100) */

#define CSIPHY_LANE_ENABLE      0xd5u           /* data0 0x01, clock 0x80, data1 0x04, data2 0x10, data3 0x40 */
#define CSIPHY_SETTLE_OFFSET    0x08u           /* the settle count's low byte, in every lane position */

struct csiphy_reg {
	uint16_t offset;
	uint8_t value;
};

struct csiphy_seq {
	uint16_t offset;
	uint8_t value;
	uint8_t delay_ms;
	bool check;             /* a plain RW register: read it back */
};

/* cam_csiphy_core.c's reset, then common (ref/cam_csiphy_1_1_hwreg.h:38-44, :30-36) */
static const struct csiphy_seq csiphy_reset_seq[] = {
	{ 0x814, 0x00, 5, true },
	{ 0x818, 0x00, 0, true },
	{ 0x81c, 0x00, 0, true },
	{ 0x800, 0x01, 1, false },
	{ 0x800, 0x00, 0, false },
};

static const struct csiphy_seq csiphy_common_seq[] = {
	{ 0x814, CSIPHY_LANE_ENABLE, 0, true },
	{ 0x818, 0x01, 0, true },
	{ 0x81c, 0x02, 0, true },
	{ 0x800, 0x01, 1, false },
	{ 0x800, 0x00, 0, false },
};

/* The clock lane's 14 entries (ref/cam_csiphy_1_1_hwreg.h:78-93) and the data lanes' 12 (their
 * +0x28 and +0x0c entries are "do not program" there and are skipped; +0x08 is the settle). */
static const struct csiphy_reg csiphy_clock_lane[] = {
	{ 0x04, 0x0c }, { 0x2c, 0x01 }, { 0x34, 0x0f }, { 0x1c, 0x0a }, { 0x14, 0x60 },
	{ 0x28, 0x04 }, { 0x3c, 0xb8 }, { 0x00, 0x80 }, { 0x08, 0x00 }, { 0x0c, 0xa5 },
	{ 0x10, 0x52 }, { 0x38, 0x1f }, { 0x60, 0x00 }, { 0x64, 0x7f },
};

static const struct csiphy_reg csiphy_data_lane[] = {
	{ 0x04, 0x0c }, { 0x2c, 0x01 }, { 0x34, 0x0f }, { 0x1c, 0x0a }, { 0x14, 0x60 },
	{ 0x3c, 0xb8 }, { 0x00, 0x90 }, { 0x08, 0x00 }, { 0x10, 0x52 }, { 0x38, 0xfe },
	{ 0x60, 0x00 }, { 0x64, 0x7f },
};

/* The five positions in the core's order: data0, clock, data1, data2, data3. */
static const uint16_t csiphy_lane_base[] = { 0x000, 0x700, 0x200, 0x400, 0x600 };

/* ref/cam_csiphy_1_1_hwreg.h:46-58 (0x82c + 4*i) */
static const struct csiphy_reg csiphy_irq_masks[] = {
	{ 0x82c, 0xff }, { 0x830, 0xff }, { 0x834, 0xfb }, { 0x838, 0xff }, { 0x83c, 0x7f },
	{ 0x840, 0xff }, { 0x844, 0xff }, { 0x848, 0xef }, { 0x84c, 0xff }, { 0x850, 0xff },
	{ 0x854, 0xff },
};

/* Each CSIPHY's page, mapped once and kept; g_csiphy_va is the selected one, g_csiphy_n its number. */
static vm_offset_t g_csiphy_pages[3];
static vm_offset_t g_csiphy_va;
static unsigned g_csiphy_n;

static bool
csiphy_seq_write(const struct csiphy_seq *seq, unsigned count, unsigned *writes)
{
	for (unsigned i = 0; i < count; i++) {
		wr(g_csiphy_va + seq[i].offset, seq[i].value);
		(*writes)++;
		__asm__ volatile ("dsb sy");
		if (seq[i].check && rd(g_csiphy_va + seq[i].offset) != seq[i].value) {
			printf("vayu-camera: csiphy%u %#x <- %#x, but it reads %#x (going on)\n", g_csiphy_n,
			    seq[i].offset, seq[i].value, rd(g_csiphy_va + seq[i].offset));
		}
		if (seq[i].delay_ms != 0) {
			IOSleep(seq[i].delay_ms);
		}
	}
	return true;
}

static bool
csiphy_regs_write(const struct csiphy_reg *regs, unsigned count, uint32_t base,
    uint32_t settle, bool settle_here, unsigned *writes)
{
	for (unsigned i = 0; i < count; i++) {
		uint32_t value = (settle_here && regs[i].offset == CSIPHY_SETTLE_OFFSET) ?
		    (settle & 0xff) : regs[i].value;

		wr(g_csiphy_va + base + regs[i].offset, value);
		(*writes)++;
		__asm__ volatile ("dsb sy");
		if (rd(g_csiphy_va + base + regs[i].offset) != value) {
			/* (noted, not fatal: a PHY register may not read back as written; the frame decides) */
			printf("vayu-camera: csiphy%u %#x <- %#x, but it reads %#x (going on)\n", g_csiphy_n,
			    base + regs[i].offset, value, rd(g_csiphy_va + base + regs[i].offset));
		}
	}
	return true;
}

#define CAM_NELEM(a)    (sizeof(a) / sizeof((a)[0]))

/*
 * One CSIPHY's page, mapped once and kept (0 if it cannot be mapped); g_csiphy_n says which.
 */
static vm_offset_t
csiphy_page(const struct cam_stream_cfg *cfg)
{
	if (cfg->csiphy < 1 || cfg->csiphy > 2) {
		return 0;
	}
	if (g_csiphy_pages[cfg->csiphy] == 0) {
		g_csiphy_pages[cfg->csiphy] = ml_io_map(cfg->csiphy_phys, CSIPHY_SIZE);
	}
	return g_csiphy_pages[cfg->csiphy];
}

/*
 * The v1.1 sequence: reset, common, the five lane positions, the interrupt masks. Every plain RW
 * register is read back; 0x800 is a command and is not. The down path is the reset sequence again,
 * with no interrupt handling. Both CSIPHY instances share it.
 */
static bool
csiphy_up(const struct cam_stream_cfg *cfg, uint32_t settle, const char *step)
{
	unsigned writes = 0;
	bool ok;

	g_csiphy_n = cfg->csiphy;
	g_csiphy_va = csiphy_page(cfg);
	if (g_csiphy_va == 0) {
		printf("vayu-camera: CSIPHY%u cannot be mapped at %#x: %s not done\n",
		    cfg->csiphy, (unsigned)cfg->csiphy_phys, step);
		return false;
	}
	g_csiphy_running = true;
	ok = csiphy_seq_write(csiphy_reset_seq, CAM_NELEM(csiphy_reset_seq), &writes) &&
	     csiphy_seq_write(csiphy_common_seq, CAM_NELEM(csiphy_common_seq), &writes);
	for (unsigned l = 0; ok && l < CAM_NELEM(csiphy_lane_base); l++) {
		if (l == 1) {
			ok = csiphy_regs_write(csiphy_clock_lane, CAM_NELEM(csiphy_clock_lane),
			    csiphy_lane_base[l], settle, true, &writes);
		} else {
			ok = csiphy_regs_write(csiphy_data_lane, CAM_NELEM(csiphy_data_lane),
			    csiphy_lane_base[l], settle, true, &writes);
		}
	}
	if (ok) {
		ok = csiphy_regs_write(csiphy_irq_masks, CAM_NELEM(csiphy_irq_masks), 0, 0, false, &writes);
	}
	if (!ok) {
		return false;
	}
	printf("vayu-camera: csiphy%u up: lanes %#x, settle %#x, %u writes\n",
	    g_csiphy_n, CSIPHY_LANE_ENABLE, settle & 0xff, writes);
	return true;
}

static void
csiphy_down(void)
{
	unsigned writes = 0;

	if (!g_csiphy_running || g_csiphy_va == 0) {
		return;
	}
	(void)csiphy_seq_write(csiphy_reset_seq, CAM_NELEM(csiphy_reset_seq), &writes);
	g_csiphy_running = false;
	printf("vayu-camera: csiphy%u down: 0x814 %#x, %u writes\n", g_csiphy_n,
	    rd(g_csiphy_va + 0x814), writes);
}

/* ---- steps 4 and 6: one camera's stream, up and down ------------------------------------------- */

/*
 * Step 5's ring: three places of contiguous kernel memory, each round_page()d to the largest
 * sensor's frame. Step 6's IFE writes into them and cam_stream_up() maps the whole ring, so the
 * variables are declared before it; cam_fake_ring_alloc() (with the node's code below) fills them.
 */
static vm_offset_t cam_fake_ring;
static uint64_t cam_fake_ring_bytes;
static uint32_t cam_fake_place_bytes, cam_fake_frame_bytes;

/* What one session's bring-up did, for its teardown (and for step 4's summary). */
struct cam_stream_state {
	const struct cam_stream_cfg *cfg;
	const struct cam_sensor *s;
	struct cam_power done;
	struct cam_bus bus;
	bool init_tried;        /* the init block was attempted: the off block is due */
	bool teardown;          /* step 4's rule: from frames_alloc() on, a failure tears down */
	bool on;                /* the vendor's on block went out: g_pipeline_running is set */
};

/*
 * Step 4's bring-up, shared by step 6.
 * `ring' is 0 for step 4: its own frames are allocated (a 4 KB header, then two frames) and RDI 0
 * is left halted for vayu_camera_skip_ms. Non-zero is step 6's ring: the whole ring is mapped in
 * the SMMU, the pause is 0, the camera row's 4x frame/exposure/gain are written ALWAYS, and RDI 0
 * runs from the start. `step' is "step 4"/"step 6" for the lines that have one.
 *
 * The caller tears down through cam_stream_down() on any false; st->teardown says whether a
 * teardown is due (false only for the tab and rail refusals, which write nothing).
 */
static bool
cam_stream_up(const struct cam_stream_cfg *cfg, vm_offset_t ring, const char *step,
    struct cam_stream_state *st)
{
	const struct cam_sensor *s = &cam_sensors[cfg->sensor];
	uint8_t data[2] = { 0, 0 };
	char detail[192], bytes[32];
	uint32_t width = cfg->width, height = cfg->height;
	uint32_t stride = RAW10_STRIDE(width), frame_bytes = stride * height;
	uint32_t id = 0;
	bool real = ring != 0;

	memset(st, 0, sizeof(*st));
	st->cfg = cfg;
	st->s = s;
	g_stream = cfg;

	if (!cam_tab_load(cfg->tab, cfg->mode, step) || !cam_rails_lookup(step, true)) {
		return false;
	}
	if (real) {
		printf("vayu-camera: step 6: a %s frame %ux%u RAW10, four lanes, through CSIPHY%u, "
		    "CSID-lite 0 RDI 0 and IFE-lite 0 WM0 (camcc PLL0, settle %#x)\n", cfg->header,
		    width, height, cfg->csiphy, g_settle & 0xff);
	} else {
		printf("vayu-camera: step 4: one %s frame %ux%u RAW10, four lanes, %s fps, through CSIPHY%u, "
		    "CSID-lite 0 RDI 0 and IFE-lite 0 WM0 (camcc PLL0, settle %#x, %u ms for the frame)\n",
		    cfg->header, width, height, cfg->fps, cfg->csiphy, g_settle & 0xff, g_frames_ms);
	}
	printf("vayu-camera: about to write (rails, camcc, TLMM, I2C, CSIPHY, SMMU, CSID, IFE) in %u ms\n",
	    real ? 0 : g_pause_ms);
	if (!real && g_pause_ms != 0) {
		IOSleep(g_pause_ms);
	}
	st->teardown = true;            /* (step 4's failures from here on take down what was done) */

	if (!real) {
		struct cam_header *h;

		if (!frames_alloc(width, height, stride, 1, cfg->header)) {
			printf("vayu-camera: step 4: no frame memory: taking down what was done\n");
			return false;
		}
		h = (struct cam_header *)g_frames;
		h->bayer = cfg->bayer;
		/* frames_alloc() flushed the header before it knew the selected camera's bayer: flush it again. */
		for (vm_offset_t line = g_frames; line < g_frames + CAM_HEADER_BYTES; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
	}
	if (!cam_step4_clocks(cfg, step) || !cam_gdsc_on() || !pll0_up() || !clocks_up(true)) {
		printf("vayu-camera: %s: the clocks did not come up: taking down what was done\n", step);
		return false;
	}
	if (!cam_rail_vote(cam_rail_find("ldoc3"), 1, detail, sizeof(detail))) {
		printf("vayu-camera: %s: ldoc3: %s\n", s->name, detail);
		return false;
	}
	printf("vayu-camera: %s: ldoc3: %s\n", s->name, detail);

	if (!cam_sensor_pins_ok(s)) {
		return false;
	}
	st->bus.sda = s->sda;
	st->bus.scl = s->scl;
	st->bus.sda_pg = cam_pin_page(s->sda);
	st->bus.scl_pg = cam_pin_page(s->scl);
	if (st->bus.sda_pg == 0 || st->bus.scl_pg == 0) {
		printf("vayu-camera: %s: the I2C pins cannot be mapped: %s not done\n", s->name, step);
		return false;
	}
	if (!cam_sensor_power_up(s, &st->done)) {
		printf("vayu-camera: %s: power-up did not finish: powering down what was done\n", s->name);
		return false;
	}

	if (!cam_i2c_read_id(&st->bus, s->addr7, s->id_reg, s->reg_bytes, data, s->data_bytes)) {
		printf("vayu-camera: %s: the id at %#x was not read: %s not done\n", s->name, s->id_reg, step);
		return false;
	}
	for (unsigned i = 0; i < s->data_bytes; i++) {
		id = (id << 8) | data[i];
	}
	cam_bytes(bytes, sizeof(bytes), data, s->data_bytes);
	printf("vayu-camera: %s: chip id %#x at %#x (expected %#x): %s (bytes %s)\n",
	    s->name, id, s->id_reg, s->id, id == s->id ? "MATCH" : "MISMATCH", bytes);
	if (id != s->id) {
		printf("vayu-camera: %s: not the module's sensor: %s not done\n", s->name, step);
		return false;
	}

	st->init_tried = true;
	if (!cam_tab_write_block(&st->bus, s, &cam_tab[CAM_TAB_INIT]) ||
	    !cam_tab_write_block(&st->bus, s, &cam_tab[CAM_TAB_MODE])) {
		printf("vayu-camera: %s: the sensor's init failed: taking down what was done\n", s->name);
		return false;
	}
	/*
	 * 0x0340/0x0202/0x0204, the camera row's values. Step 4 writes them only when asked
	 * (vayu_camera_ae, or an exposure/gain boot-arg); step 6 writes them at every session start,
	 * because its exposure is fixed. Volatile registers: the power-down resets them.
	 */
	if (real) {
		uint32_t frame_lines = cfg->ae_frame_lines, lines = cfg->ae_lines, gain = cfg->ae_gain;
		bool ok = cam_i2c_write_reg(&st->bus, s->addr7, 0x0340, 2, (uint16_t)frame_lines, 2) &&
		    cam_i2c_write_reg(&st->bus, s->addr7, 0x0202, 2, (uint16_t)lines, 2) &&
		    cam_i2c_write_reg(&st->bus, s->addr7, 0x0204, 2, (uint16_t)gain, 2);

		printf("vayu-camera: %s: frame length 0x0340 <- %#x, exposure 0x0202 <- %#x lines, gain 0x0204 <- %#x: %s\n",
		    s->name, frame_lines, lines, gain, ok ? "acknowledged" : "NOT ACKNOWLEDGED");
		if (!ok) {
			return false;
		}
	} else if (g_ae || g_exposure != 0 || g_gain != 0) {
		uint32_t frame_lines = g_ae ? cfg->ae_frame_lines : 0;
		uint32_t lines = g_exposure != 0 ? g_exposure : g_ae ? cfg->ae_lines : 0;
		uint32_t gain = g_gain != 0 ? g_gain : g_ae ? cfg->ae_gain : 0;
		bool ok = true;

		/* integration within the frame: lines <= frame lines - 48 when both are ours */
		if (frame_lines != 0 && lines > frame_lines - 48) {
			lines = frame_lines - 48;
		}
		if (frame_lines != 0) {
			ok = cam_i2c_write_reg(&st->bus, s->addr7, 0x0340, 2, (uint16_t)frame_lines, 2);
		}
		if (ok && lines != 0) {
			ok = cam_i2c_write_reg(&st->bus, s->addr7, 0x0202, 2, (uint16_t)lines, 2);
		}
		if (ok && gain != 0) {
			ok = cam_i2c_write_reg(&st->bus, s->addr7, 0x0204, 2, (uint16_t)gain, 2);
		}
		printf("vayu-camera: %s: frame length 0x0340 <- %#x, exposure 0x0202 <- %#x lines, gain 0x0204 <- %#x: %s\n",
		    s->name, frame_lines, lines, gain, ok ? "acknowledged" : "NOT ACKNOWLEDGED");
		if (!ok) {
			return false;
		}
	}

	if (g_csid_va == 0) {
		g_csid_va = ml_io_map(CSID_PHYS, 0x1000);
		g_ife_va = ml_io_map(IFE_PHYS, 0x4000);
	}
	if (g_csid_va == 0 || g_ife_va == 0) {
		printf("vayu-camera: CSID-lite 0 and IFE-lite 0 cannot be mapped: %s not done\n", step);
		return false;
	}
	/*
	 * csid_config()'s RX cfg0 is (lanes - 1) | lane_cfg << 4 | phy_sel << 20: 0x00132103 for
	 * PHY 1, 0x00232103 for PHY 2; cfg1 is 1 << 6 | 1 = 0x41, and RDI 0's cfg0 carries RAW10's
	 * 0x2b in VC 0. Step 4 leaves RDI 0 halted for the skip; step 6 lets it run from the start.
	 * WM0's frame increment is 0 for the ring: a place is re-armed by hand every frame, so a late
	 * frame overwrites the place it just finished instead of running into the next place.
	 */
	g_rdi_hold = true;              /* (step 4 resumes after its skip, step 6 when its session loop is ready) */
	if (real) {
		/*
		 * From the SMMU on, a quiesce has a pipeline to stop even before the vendor's on block:
		 * g_sensor_powered covers the MCLKs and the pins, this covers WM0 and the SMMU. (Step 4
		 * keeps its old timing: its flag is set after the on block.)
		 */
		g_pipeline_running = true;
	}
	if (!smmu_up(real ? ring : g_frames, real ? cam_fake_ring_bytes : g_frames_bytes) || !csid_reset() ||
	    !csid_config((int)cfg->phy_sel, cfg->lanes, cfg->lane_cfg, 0, width, height, false) ||
	    !csiphy_up(cfg, g_settle, step) ||
	    !ife_config(real ? FRAME_ADDRESSES : FRAME_ADDRESSES + CAM_HEADER_BYTES,
	    real ? 0 : frame_bytes)) {
		printf("vayu-camera: %s: the pipeline was not configured: taking down what was done\n", step);
		return false;
	}
	show_paths("configured");

	if (!cam_tab_write_block(&st->bus, s, &cam_tab[CAM_TAB_ON])) {
		printf("vayu-camera: %s: the stream did not start: taking down what was done\n", s->name);
		return false;
	}
	g_pipeline_running = true;
	st->on = true;
	return true;
}

/*
 * The teardown, always, in the hardware's order: the stream off (the module's off block, 0x0100 =
 * 0, then its 30 ms), the paths, the CSIPHY, the SMMU, the sensor's power-down (ldoc1 off, the
 * outputs low), the clocks, PLL0 if ours, the GDSC. Shared rails stay on. Every part is guarded by
 * what it set, so a failure at any point takes down exactly what was done.
 */
static void
cam_stream_down(struct cam_stream_state *st)
{
	if (st->s == NULL) {
		return;
	}
	if (g_sensor_powered && st->init_tried) {
		(void)cam_tab_write_block(&st->bus, st->s, &cam_tab[CAM_TAB_OFF]);
		IOSleep(30);
	}
	paths_down();
	csiphy_down();
	smmu_down();
	cam_sensor_power_down(st->s, &st->done);
	clocks_down();
	pll0_down();
	cam_gdsc_off();
	g_pipeline_running = false;
	st->on = false;
}

/*
 * Step 4: step 1's camcc, then the selected camera's own tables, its CSIPHY, CSID-lite 0's RDI 0
 * from PHY 1 or 2, IFE-lite 0's WM0 into the frames' memory behind the SMMU. The IMX582's frame
 * is 1920x1080 RAW10 (stride 2400), the S5K3T2's 1280x720 RAW10 (stride 1600). The bring-up is
 * cam_stream_up() (shared with step 6), the teardown cam_stream_down(); the teardown always runs
 * once the bring-up has written anything, and the frame's summary comes after it.
 */
static void
cam_step4(void)
{
	const struct cam_stream_cfg *cfg = &cam_streams[g_cam];
	const struct cam_sensor *s = &cam_sensors[cfg->sensor];
	struct cam_stream_state st;
	struct cam_header *h;
	uint32_t width = cfg->width, height = cfg->height;
	uint32_t stride = RAW10_STRIDE(width);
	uint32_t rx = 0;
	bool done_frame = false;

	g_stream = cfg;
	printf("vayu-camera: step 4: camera %u: %s\n", g_cam, cfg->header);
	if (!g_xnu_tlmm) {
		printf("vayu-camera: step 4 needs vayu_camera_xnu_tlmm=1 (TLMM writes): not done\n");
		return;
	}
	if (!g_camcc_ok || camcc_va == 0) {
		printf("vayu-camera: the camera AHB clock was not enabled in step 1: step 4 not done\n");
		return;
	}
	if (!cam_stream_up(cfg, 0, "step 4", &st)) {
		if (!st.teardown) {
			return;
		}
		goto teardown;
	}
	/*
	 * The first frames after stream-on are dark (cam2b/cam3 on hardware: 194750 and 200614 read
	 * min 62, max 67, mean 64 -- the black level -- 40 ms after "on"). RDI 0 was left halted, so
	 * nothing reaches WM0 while the sensor settles; after vayu_camera_skip_ms it resumes at a
	 * frame's boundary and the frame after that is the one written.
	 */
	if (g_skip_ms != 0) {
		IOSleep(g_skip_ms > 5000 ? 5000 : g_skip_ms);
	}
	wr(g_ife_va + BUS_IRQ_CLEAR1, 1);
	wr(g_ife_va + BUS_IRQ_GLOBAL_CLEAR, 1);
	(void)cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CTRL, 1, true);
	printf("vayu-camera: %s: rdi 0 resumed after %u ms of stream (packets so far %u)\n", s->name,
	    g_skip_ms > 5000 ? 5000 : g_skip_ms, rd(g_csid_va + CSID_CSI2_RX_PKTS));
	h = (struct cam_header *)g_frames;
	for (unsigned waited = 0; waited < g_frames_ms; ) {
		unsigned step = g_frames_ms - waited;

		if (step > 10) {
			step = 10;
		}
		IOSleep(step);
		waited += step;
		if ((rd(g_ife_va + BUS_IRQ_STATUS1) & 1u) != 0) {
			h->frames = 1;
			done_frame = true;
			printf("vayu-camera: write master 0 says its frame is done after %u ms\n", waited);
			break;
		}
	}
	if (!done_frame) {
		printf("vayu-camera: NO FRAME after %u ms\n", g_frames_ms);
	}
	show_paths("after the frame");
	(void)smmu_faults();

teardown:
	rx = g_csid_va != 0 ? rd(g_csid_va + CSID_CSI2_RX_PKTS) : 0;
	cam_stream_down(&st);
	cam_frame_summary(width, height, stride, cfg->name);
	printf("vayu-camera: step 4 done: frame %s, smmu faults %s, rx packets %u\n",
	    done_frame ? "done" : "NO FRAME", g_smmu_bad ? "SEEN" : "none", rx);
}

/* ---- step 1: the read-only survey -------------------------------------------------------------- */

static void
cam_show_pins(void)
{
	for (unsigned i = 0; i < CAM_PIN_COUNT; i++) {
		const struct cam_pin *p = &cam_pins[i];
		vm_offset_t page = cam_pin_page(p->pin);
		uint32_t cfg, io, function, drive;

		if (page == 0) {
			printf("vayu-camera: cannot map gpio%u (%s) at %#lx: the pin is not read\n",
			    p->pin, p->name, (unsigned long)p->phys);
			continue;
		}
		cfg = rd(page);
		io = rd(page + 4);
		function = (cfg >> TLMM_CFG_FUNC_SHFT) & TLMM_CFG_FUNC_MASK;
		drive = (cfg >> TLMM_CFG_DRIVE_SHFT) & TLMM_CFG_DRIVE_MASK;

		printf("vayu-camera: gpio%u (%s): cfg %#x: function %u%s, pull %u, %u mA, oe %u; "
		    "io %#x (in %u)\n", p->pin, p->name, cfg, function,
		    (function == 1 && p->fn1 != NULL) ? p->fn1 : "", cfg & TLMM_CFG_PULL_MASK,
		    (drive + 1) * 2, (cfg >> TLMM_CFG_OE_SHFT) & 1, io, io & TLMM_IO_IN);
	}
}

/* The command database's addresses, a read of the database only (no vote). */
static void
cam_show_cmd_db(void)
{
	static const char *const ids[] = {
		"ldoc1", "ldoc3", "smpa5", "smpc8", "bobc1", "mmcx.lvl", "mx.lvl",
	};

	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		uint32_t address = vayu_rpmh_address(ids[i]);

		if (address == 0) {
			printf("vayu-camera: cmd-db %s: no address\n", ids[i]);
		} else {
			printf("vayu-camera: cmd-db %s = %#x (enable vote at %#x)\n", ids[i], address,
			    address + 4);
		}
	}
}

/* ---- step 1: the PMIC regulator survey (read-only, from vayu_haptic.c) ------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL    /* the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* which system may write through each channel (not read here) */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* reads: 0x80 per channel for our system (the first) */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0               /* who we are to the arbiter: the DT's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* [19:8] slave and peripheral */
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

#define SURVEY_MAX_LINES        160

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral */
	uint16_t        channel;
	bool            found;
};

static vm_offset_t arb_core, arb_observer;

/* (the arbiter one transfer at a time with the buttons' driver) */
static void
lock(void)
{
	hakios_spmi_lock();
}

static void
unlock(void)
{
	hakios_spmi_unlock();
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static const char *spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count);

/* A transfer the arbiter drops now and then is asked for again, twice at most. */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static const char *
spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	const char *problem;
	uint32_t data[2];

	if (!p->found) {
		return "no channel";
	}
	lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	unlock();
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/*
 * Every channel that is a PMIC peripheral (slave 0 or 4, peripheral 0x14..0xaf) is read where
 * the regulator drivers keep type/subtype (0x04), status1 (0x08), vset (0x40, little-endian
 * millivolts), mode (0x45) and en_ctl (0x46). Read-only, whoever owns the channel; at most
 * SURVEY_MAX_LINES lines.
 */
static void
cam_pmic_survey(void)
{
	uint32_t version, channels, lines = 0;

	if (arb_core == 0 || arb_observer == 0) {
		printf("vayu-camera: the PMIC arbiter cannot be mapped: no PMIC survey\n");
		return;
	}
	version = rd(arb_core + ARB_VERSION);
	printf("vayu-camera: PMIC arbiter version %#x\n", version);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("vayu-camera: the arbiter is not version 5: no PMIC survey\n");
		return;
	}
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}

	for (uint32_t n = 0; n < channels && lines < SURVEY_MAX_LINES; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), ppid, sid, per;
		struct peripheral p = { .name = "the survey", .found = true };
		uint8_t type[2], status1, vset[2], mode, en;
		const char *problem = NULL;

		if (map == 0) {
			continue;
		}
		ppid = (map >> 8) & 0xfff;
		sid = ppid >> 8;
		per = ppid & 0xff;
		if ((sid != 0 && sid != 4) || per < 0x14 || per > 0xaf) {
			continue;
		}
		p.id = (uint16_t)ppid;
		p.channel = (uint16_t)n;

		/* type and subtype, status1, vset (little-endian mV), mode, enable */
		problem = spmi_read(&p, 0x04, type, 2);
		if (problem == NULL) {
			problem = spmi_read(&p, 0x08, &status1, 1);
		}
		if (problem == NULL) {
			problem = spmi_read(&p, 0x40, vset, 2);
		}
		if (problem == NULL) {
			problem = spmi_read(&p, 0x45, &mode, 1);
		}
		if (problem == NULL) {
			problem = spmi_read(&p, 0x46, &en, 1);
		}
		if (problem != NULL) {
			printf("vayu-camera: pmic sid %u 0x%02x00: %s\n", sid, per, problem);
			lines++;
			continue;
		}
		printf("vayu-camera: pmic sid %u 0x%02x00: type %u sub %u status1 %u vset %u %u "
		    "(= %u mV) mode %u en %u\n", sid, per, type[0], type[1], status1,
		    vset[0], vset[1], (unsigned)(vset[0] | (vset[1] << 8)), mode, en);
		lines++;
	}
	if (lines == SURVEY_MAX_LINES) {
		printf("vayu-camera: pmic survey stops at %u lines\n", SURVEY_MAX_LINES);
	}
}

/* ---- step 1 ------------------------------------------------------------------------------------ */

/*
 * GCC and camcc, and camera_ahb's verdict: step 1's first part, and the only part of step 1 that
 * step 6 runs. camera_ahb decides whether camcc may be touched at all, so it is read first;
 * camera_xo is a GCC register and is safe either way. True when both are mapped and camcc may be
 * read and written.
 */
static bool
cam_gcc_camcc(void)
{
	uint32_t ahb, xo;

	gcc_va = ml_io_map(GCC_PHYS, GCC_SIZE);
	if (gcc_va == 0) {
		printf("vayu-camera: cannot map GCC at 0x100000: the clocks are not read\n");
		g_camcc_ok = false;
		return false;
	}
	ahb = rd(gcc_va + GCC_CAMERA_AHB_CBCR);
	xo = rd(gcc_va + GCC_CAMERA_XO_CBCR);
	printf("vayu-camera: gcc camera_ahb 0xb008 = %#x (%s), camera_xo 0xb044 = %#x (%s)\n",
	    ahb, cam_clock_state(ahb), xo, cam_clock_state(xo));
	g_camcc_ok = (ahb & GCC_CBCR_ENABLE) != 0 || (ahb & GCC_CBCR_OFF) == 0;
	if (!g_camcc_ok) {
		/* Definitely stopped: an AHB read of camcc could hang the CPU. */
		printf("vayu-camera: camera AHB clock off (0xb008 = %#x): camcc not read\n", ahb);
		return false;
	}
	camcc_va = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	if (camcc_va == 0) {
		printf("vayu-camera: cannot map camcc at 0xad00000: it is not read\n");
		g_camcc_ok = false;
		return false;
	}
	return true;
}

static void
cam_step1(void)
{
	uint32_t gdsc, cmd, cfg, cbcr;

	printf("vayu-camera: step 1: a read-only look at the camera's clocks, pins, rails and PMICs\n");

	if (cam_gcc_camcc()) {
		gdsc = rd(camcc_va + CAMCC_TITAN_TOP_GDSC);
		cmd = rd(camcc_va + CAMCC_MCLK_CMD(1));
		cfg = rd(camcc_va + CAMCC_MCLK_CFG(1));
		cbcr = rd(camcc_va + CAMCC_MCLK_CBCR(1));

		printf("vayu-camera: camcc titan_top gdsc 0xc1bc = %#x (%s)\n", gdsc,
		    (gdsc & CAMCC_GDSCR_POWER_ON) ? "power on" : "POWER OFF");
		printf("vayu-camera: camcc mclk1 rcg cmd %#x cfg %#x (source %u, "
		    "divider %u, root %s), cbcr 0x503c = %#x (%s)\n", cmd, cfg,
		    (cfg & CAMCC_CFG_SRC_MASK) >> CAMCC_CFG_SRC_SHFT,
		    cfg & CAMCC_CFG_DIV_MASK, (cmd & CAMCC_CMD_ROOT_OFF) ? "off" : "on",
		    cbcr, cam_clock_state(cbcr));

		for (unsigned n = 0; n < 4; n++) {
			uint32_t c = CAMCC_MCLK_CMD(n), g = CAMCC_MCLK_CFG(n);
			uint32_t b = CAMCC_MCLK_CBCR(n);
			uint32_t v = rd(camcc_va + c), f = rd(camcc_va + g), r = rd(camcc_va + b);

			printf("vayu-camera: camcc mclk%u rcg cmd %#x cfg %#x "
			    "(source %u, divider %u, root %s), cbcr %#x = %#x (%s)\n", n, v, f,
			    (f & CAMCC_CFG_SRC_MASK) >> CAMCC_CFG_SRC_SHFT,
			    f & CAMCC_CFG_DIV_MASK, (v & CAMCC_CMD_ROOT_OFF) ? "off" : "on",
			    b, r, cam_clock_state(r));
		}
	}

	cam_show_pins();
	cam_show_cmd_db();

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	cam_pmic_survey();

	printf("vayu-camera: step 1 done, nothing written\n");
}

/* ---- steps 5 and 6: /dev/hakios-camera, its ring and its two frame sources --------------------- */

#define CAM_FAKE_RING           3
#define CAM_FAKE_IDLE_MS        1000
#define CAM_FAKE_FIRST_MS       15000
#define CAM_FAKE_STREAM_MS      3000
#define CAM_REAL_POLL_MS        2               /* step 6's poll of WM0's done bit */

enum { CAM_PLACE_FREE, CAM_PLACE_WRITING, CAM_PLACE_READY, CAM_PLACE_READING };

/* what a frame hook produced, for the place it is marked READY with */
struct cam_frame_out {
	uint32_t        exposure_lines, gain, frame_lines;
	int             place;          /* the place the frame really went to (step 6: may differ from the one asked) */
};

/* what a frame hook returns: a frame, a quiet stop, a retry (a timeout), or a session failure */
enum { CAM_FRAME_STOP = 0, CAM_FRAME_DONE = 1, CAM_FRAME_RETRY = 2, CAM_FRAME_FAIL = -1 };

/*
 * One row per sensor of the ABI, for both sources. 1 is the rear IMX582, 2 the front S5K3T2.
 * cam_fake_sensors[] is step 5's synthetic source: its frame lengths are the modes' shortest
 * (1e9 / (1262 * 13190) = 60.08 frames a second rear, 1e9 / (830 * 10000) = 120.48 front), the
 * margins leave room for the sensor's own blanking, and gain_unity/gain_max are the sensor's 1x
 * and maximum register values. Those values are HYPOTHESES for the synthetic source only.
 * cam_real_sensors[] is step 6's: the header describes the camera row's 4x frame (5048 lines rear,
 * 3968 front), whose line times are 13190 ns (15.02 fps) and 8375 ns (30.10 fps); the gain is in
 * the sensors' linear scale (Sony 0x100 = 1x, 0x400 = 4x; Samsung 0x20 = 1x, 0x80 = 4x), and the
 * margin is the row's own 48 lines. stream is the cam_streams[] row the source brings up.
 */
struct cam_abi_sensor {
	uint32_t        sensor;         /* the frame header's sensor number: 1 rear, 2 front */
	const char      *name;
	uint32_t        width, height;
	uint32_t        bayer;
	uint32_t        black;
	uint32_t        line_ns;
	uint32_t        frame_lines_min;
	uint32_t        frame_lines_max;
	uint32_t        margin;
	uint32_t        gain_unity;
	uint32_t        gain_max;
	const struct cam_stream_cfg *stream;    /* step 6 only: the camera row to bring up */
};

static const struct cam_abi_sensor cam_fake_sensors[] = {
	{ 1, "IMX582", 1920, 1080, 0, 64, 13190, 1262, 65535, 22, 0x100, 0x3c0, NULL },
	{ 2, "S5K3T2", 1280, 720,  2, 64, 10000,  830, 65535,  8, 0x020, 0x200, NULL },
};
#define CAM_FAKE_SENSOR_COUNT   (sizeof(cam_fake_sensors) / sizeof(cam_fake_sensors[0]))

static const struct cam_abi_sensor cam_real_sensors[] = {
	/* the IMX582's res3 frame made 4x long: 4 * 1262 lines at 13190 ns; 0x0204 = 0x300 (Sony's 4x code) */
	{ 1, "IMX582", 1920, 1080, 0, 64, 13190, 0x13b8, 0x13b8, 48, 0x100, 0x1000, &cam_streams[0] },    /* gain 1x..16x (module max 64x: sensorDriverData +0x150 = 64.0, INFERENCE) */
	/* the S5K3T2's res2 frame made 4x long: 4 * 992 lines at 8375 ns; 0x0204 = 0x80 (4x of 0x20) */
	{ 2, "S5K3T2", 1280, 720,  2, 64,  8375, 0x0f80, 0x0f80, 48, 0x020, 0x200, &cam_streams[1] },     /* gain 1x..16x (module max 16x: sensorDriverData +0x150 = 16.0, INFERENCE) */
};
#define CAM_REAL_SENSOR_COUNT   (sizeof(cam_real_sensors) / sizeof(cam_real_sensors[0]))

/*
 * The frame source: step 5's synthetic chart or step 6's real sensor. The node, the ring, the
 * reads, the writes and the session loop around these hooks are one piece of code.
 * start() brings the source up and prints its own start line; frame() produces one frame into the
 * place (already WRITING) and returns one of CAM_FRAME_*; stop() tears the source down.
 */
struct cam_source {
	const char      *step;          /* "step 5" / "step 6" in the session lines */
	bool            fixed_exposure; /* step 6: EXPOSURE goes to cam_real_ae(), not to the synthetic exposure model */
	const struct cam_abi_sensor *(*find)(uint32_t sensor);
	bool            (*start)(const struct cam_abi_sensor *s, uint32_t lines, uint32_t gain,
	                    uint32_t frame_lines);
	int             (*frame)(const struct cam_abi_sensor *s, int place, struct cam_frame_out *out);
	void            (*stop)(void);
};

static const struct cam_source *cam_fake_source;

/* the bars' linear reflectance, R, G, B in thousandths (the reference's chart) */
static const uint16_t cam_fake_bars[8][3] = {
	{ 900, 900, 900 }, { 730, 550, 300 }, { 600, 50, 50 }, { 750, 650, 50 },
	{ 80, 450, 100 }, { 50, 500, 600 }, { 50, 100, 550 }, { 180, 180, 180 },
};

static LCK_GRP_DECLARE(cam_fake_lck_grp, "vayu-camera");
static LCK_MTX_DECLARE(cam_fake_lock, &cam_fake_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
} cam_fake_place[CAM_FAKE_RING];
static const struct cam_abi_sensor *cam_fake_now;
static volatile bool cam_fake_open, cam_fake_streaming, cam_fake_wanted;
static volatile bool cam_fake_broken;
static bool cam_fake_failed;                    /* this session failed: its reads get EIO */
static const char *cam_fake_failed_why = "a fault";   /* the failed session's reason, for its line */
static bool cam_fake_exposure_said;             /* step 6's "ignored" line, once per session */
static uint32_t cam_fake_failures;              /* failed sessions: the second ends the node */
static uint32_t cam_fake_opens, cam_fake_readers, cam_fake_generation;
static uint64_t cam_fake_last_read, cam_fake_frames_done, cam_fake_frames_given, cam_fake_reads;
static uint64_t cam_fake_given_sequence;
static struct { uint32_t lines, gain, frame_lines; bool pending; } cam_fake_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } cam_fake_at, cam_fake_next;
static volatile uint32_t cam_fake_switch_to;    /* the sensor a write asked for: 1 or 2, 0 none */

static uint64_t
cam_fake_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static const struct cam_abi_sensor *
cam_fake_find(uint32_t sensor)
{
	for (unsigned i = 0; i < CAM_FAKE_SENSOR_COUNT; i++) {
		if (cam_fake_sensors[i].sensor == sensor) {
			return &cam_fake_sensors[i];
		}
	}
	return NULL;
}

/* exposure_lines, gain, frame_lines within what the sensor takes (0: the shortest that holds the exposure) */
static void
cam_fake_limits(const struct cam_abi_sensor *s, uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = s->frame_lines_min, longest = s->frame_lines_max, margin = s->margin;

	*gain = *gain < s->gain_unity ? s->gain_unity : *gain > s->gain_max ? s->gain_max : *gain;
	*lines = *lines < 4 ? 4 : *lines > longest - margin ? longest - margin : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + margin > shortest ? *lines + margin : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > longest ? longest : *frame_lines;
	if (*lines > *frame_lines - margin) {
		*lines = *frame_lines - margin;
	}
}

/*
 * The ring: three places in one piece of contiguous kernel memory, as step 3's frames are. The
 * largest sensor's frame sets the place's size, so a switch between the two needs no new memory;
 * the ring stays for the rest of the run (nothing of it is handed to the loader). Step 5 sizes it
 * from its synthetic table, step 6 from the real one; both tables agree on the two sizes.
 */
static bool
cam_fake_ring_alloc(const struct cam_abi_sensor *rows, unsigned count, const char *step)
{
	uint32_t largest = 0;

	for (unsigned i = 0; i < count; i++) {
		uint32_t bytes = RAW10_STRIDE(rows[i].width) * rows[i].height;

		largest = bytes > largest ? bytes : largest;
	}
	cam_fake_place_bytes = round_page(largest);
	cam_fake_ring_bytes = (uint64_t)CAM_FAKE_RING * cam_fake_place_bytes;
	if (kmem_alloc_contig(kernel_map, &cam_fake_ring, cam_fake_ring_bytes, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("vayu-camera: %s: no memory for %u frames (%llu KB)\n", step, CAM_FAKE_RING,
		    (unsigned long long)(cam_fake_ring_bytes >> 10));
		cam_fake_ring = 0;
		cam_fake_ring_bytes = 0;
		return false;
	}
	for (vm_offset_t line = cam_fake_ring; line < cam_fake_ring + cam_fake_ring_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	printf("vayu-camera: %s: %llu KB of memory for %u frames\n", step,
	    (unsigned long long)(cam_fake_ring_bytes >> 10), CAM_FAKE_RING);
	return true;
}

/* the sensor selection, its first exposure: a room's, until the program asks for its own */
static bool
cam_fake_select(uint32_t sensor)
{
	const struct cam_abi_sensor *s = cam_fake_source->find(sensor);

	if (s == NULL) {
		return false;
	}
	lck_mtx_lock(&cam_fake_lock);
	cam_fake_now = s;
	cam_fake_ask.lines = s->frame_lines_min - s->margin;
	cam_fake_ask.gain = s->gain_unity;
	cam_fake_ask.frame_lines = 0;
	cam_fake_ask.pending = false;
	lck_mtx_unlock(&cam_fake_lock);
	return true;
}

/* one line of the chart, RAW10: `ramp' the grey ramp's, else the bars'; `odd' the second line of the cells */
static void
cam_fake_line(uint8_t *out, uint32_t width, bool odd, bool ramp, uint64_t light, const struct cam_abi_sensor *s)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };
	int r = red[s->bayer & 3], b = blue[s->bayer & 3];
	uint32_t v[4];

	for (uint32_t x = 0; x < width; x += 4) {
		for (int i = 0; i < 4; i++) {
			uint32_t px = x + (uint32_t)i, site = (odd ? 2u : 0u) + (px & 1);
			uint32_t c = site == (uint32_t)r ? 0 : site == (uint32_t)b ? 2 : 1;
			uint64_t refl = ramp ? (uint64_t)px * 1000 / width : cam_fake_bars[px * 8 / width][c];
			/* (2/15 of a 10-bit step per thousandth of reflectance and line at 1x, and the black level) */
			uint64_t level = s->black + refl * light * 2 / 15000;

			v[i] = level > 1023 ? 1023 : (uint32_t)level;
		}
		uint8_t *g = out + (x / 4) * 5;

		g[0] = (uint8_t)(v[0] >> 2);
		g[1] = (uint8_t)(v[1] >> 2);
		g[2] = (uint8_t)(v[2] >> 2);
		g[3] = (uint8_t)(v[3] >> 2);
		g[4] = (uint8_t)((v[0] & 3) | (v[1] & 3) << 2 | (v[2] & 3) << 4 | (v[3] & 3) << 6);
	}
}

/* place p filled with the chart as an exposure of `lines' at `gain' shows it */
static void
cam_fake_fill(int p, const struct cam_abi_sensor *s, uint32_t lines, uint32_t gain)
{
	uint8_t *at = (uint8_t *)(cam_fake_ring + (uint64_t)p * cam_fake_place_bytes);
	uint32_t width = s->width, height = s->height, stride = RAW10_STRIDE(width), ramp_from = height * 3 / 4 & ~1u;
	uint64_t light = (uint64_t)lines * gain / s->gain_unity;

	cam_fake_line(at, width, false, false, light, s);
	cam_fake_line(at + stride, width, true, false, light, s);
	cam_fake_line(at + (uint64_t)ramp_from * stride, width, false, true, light, s);
	cam_fake_line(at + (uint64_t)(ramp_from + 1) * stride, width, true, true, light, s);
	for (uint32_t y = 2; y < height; y++) {
		if (y != ramp_from && y != ramp_from + 1) {
			memcpy(at + (uint64_t)y * stride, at + (uint64_t)((y & 1) + (y >= ramp_from ? ramp_from : 0)) * stride, stride);
		}
	}
}

/* ---- step 5: the synthetic source --------------------------------------------------------------- */

/* the exposure each place was last drawn at (the chart is only redrawn when it changes) */
static uint32_t cam_fake_filled_lines[CAM_FAKE_RING], cam_fake_filled_gain[CAM_FAKE_RING];

static bool
cam_fake_start(const struct cam_abi_sensor *s, uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	for (int i = 0; i < CAM_FAKE_RING; i++) {
		cam_fake_filled_lines[i] = 0;
		cam_fake_filled_gain[i] = 0;
	}
	printf("vayu-camera: step 5: session started on sensor %u (%s): %ux%u RAW10, %u fps, exposure %u lines, "
	    "gain %#x, frame %u lines\n", s->sensor, s->name, s->width, s->height, g_fake_fps, lines, gain,
	    frame_lines);
	return true;
}

/* the chart for this place, if its exposure changed; the session loop marks it READY */
static int
cam_fake_frame(const struct cam_abi_sensor *s, int p, struct cam_frame_out *out)
{
	if (cam_fake_filled_lines[p] != out->exposure_lines || cam_fake_filled_gain[p] != out->gain) {
		cam_fake_fill(p, s, out->exposure_lines, out->gain);
		cam_fake_filled_lines[p] = out->exposure_lines;
		cam_fake_filled_gain[p] = out->gain;
	}
	return CAM_FRAME_DONE;
}

static void
cam_fake_stop(void)
{
}

static const struct cam_source cam_fake_synthetic = {
	.step = "step 5",
	.fixed_exposure = false,
	.find = cam_fake_find,
	.start = cam_fake_start,
	.frame = cam_fake_frame,
	.stop = cam_fake_stop,
};

/* ---- step 6: the real source -------------------------------------------------------------------- */

static struct cam_stream_state cam_real_stream;
static uint64_t cam_real_frame_ns;
static uint32_t cam_real_timeout_ms, cam_real_timeouts;
#define CAM_RDI0_SOF            (1u << 27)      /* IFE irq status 0: RDI 0's start of frame (hakios.camera.c RDI0_SOF; seen as 0x8000220) */
static int cam_real_writing, cam_real_next;     /* the place the frame in flight goes to / the one given for the next */
static uint32_t cam_real_late;                  /* SOFs with no address waiting */
/*
 * AE (the maintainers' ruling 2026-10-25: 0x0202 / 0x0204 / 0x0340): the ISP in mediaserverd asks for an exposure in lines and a
 * LINEAR gain (gain_unity = 1x); the session thread writes it between frames. The frame length stays the row's 4x value.
 */
static struct { uint32_t lines, gain; bool pending; } cam_real_ask;
static uint32_t cam_real_lines, cam_real_gain;  /* what the sensor has now (linear gain) */
static uint32_t cam_real_ae_writes;
static void cam_af_up(void);
static void cam_af_frame(void);

static const struct cam_abi_sensor *
cam_real_find(uint32_t sensor)
{
	for (unsigned i = 0; i < CAM_REAL_SENSOR_COUNT; i++) {
		if (cam_real_sensors[i].sensor == sensor) {
			return &cam_real_sensors[i];
		}
	}
	return NULL;
}

static bool
cam_real_start(const struct cam_abi_sensor *s, uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	const struct cam_stream_cfg *cfg = s->stream;

	(void)lines;
	(void)gain;
	(void)frame_lines;
	memset(&cam_real_stream, 0, sizeof(cam_real_stream));
	cam_real_timeouts = 0;
	/*
	 * The session's own SMMU verdict: a previous session's fault must not damn this one. The
	 * hardware status registers are still read as they are (smmu_faults() asks them each frame).
	 */
	g_smmu_bad = false;
	g_smmu_said = false;
	if (cfg == NULL || cam_fake_ring == 0 || cam_fake_ring_bytes == 0) {
		printf("vayu-camera: step 6: sensor %u has no camera row or the ring is not there\n", s->sensor);
		return false;
	}
	cam_real_frame_ns = (uint64_t)s->line_ns * s->frame_lines_min;
	cam_real_timeout_ms = (uint32_t)((3 * cam_real_frame_ns + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC) + 100;
	if (!cam_stream_up(cfg, cam_fake_ring, "step 6", &cam_real_stream)) {
		printf("vayu-camera: step 6: session on sensor %u (%s): the bring-up failed: the session ends\n",
		    s->sensor, s->name);
		return false;
	}
	/* ife_config() gave WM0 place 0's address (FRAME_ADDRESSES): it is taken at the first SOF */
	cam_real_writing = 0;
	cam_real_next = 0;
	cam_real_late = 0;
	cam_real_lines = cfg->ae_lines;
	cam_real_gain = 4u * s->gain_unity;
	cam_real_ask.pending = false;
	cam_real_ae_writes = 0;
	cam_af_up();
	/* RDI 0 was configured halted: with the statuses clear it resumes at a frame's boundary, so the
	 * first SOF the loop sees is the first frame's, and that frame takes place 0 */
	wr(g_ife_va + IFE_IRQ_CLEAR0, CAM_RDI0_SOF);
	wr(g_ife_va + IFE_IRQ_GLOBAL_CLEAR, 1);
	wr(g_ife_va + BUS_IRQ_CLEAR1, 1);
	wr(g_ife_va + BUS_IRQ_GLOBAL_CLEAR, 1);
	(void)cam_blk_put(g_csid_va, "CSID", CSID_RDI0_CTRL, 1, true);
	printf("vayu-camera: step 6: session started on sensor %u (%s): %ux%u RAW10, line %u ns, frame %u lines, "
	    "exposure %u lines, 0x0204 gain %#x, %u ms timeout\n", s->sensor, s->name, s->width, s->height,
	    s->line_ns, s->frame_lines_min, cfg->ae_lines, cfg->ae_gain, cam_real_timeout_ms);
	return true;
}

/*
 * One frame from WM0: the place's IOVA in WM0_IMAGE_ADDR, WM0 on, the IFE's register update and
 * the bus's status1 bit 0 cleared; then the done bit is polled every 2 ms for 3 frame times +
 * 100 ms. The place's cache lines are invalidated once the IFE has written past them. A timeout
 * asks for another place; the third in a row fails the session, as an SMMU fault does.
 *
 * The header's exposure/gain are the fixed ones this session wrote, but the gain is the ABI's
 * LINEAR one, not the sensor's code: the row's fixed gain is 4x, and 0x0204 took the sensor's
 * own code (IMX582 0x300, S5K3T2 0x80) while the ABI says 4 * gain_unity (0x400 / 0x80).
 */
/*
 * A pending AE request written to the sensor, between two frames (the frame that follows takes it,
 * or the one after: the header says what the sensor has from now on). Linear gain to the sensor's
 * code: the IMX582's analog gain is Sony's code = 1024 - 1024 / gain (gain 1x..16x: code 0..960); the
 * S5K3T2's is linear with 0x20 = 1x (HYPOTHESIS, as cam6). Only 0x0202 and 0x0204 are written.
 */
static void
cam_real_ae(const struct cam_abi_sensor *s)
{
	const struct cam_stream_cfg *cfg = s->stream;
	uint32_t lines, gain, code;
	bool ok;

	lck_mtx_lock(&cam_fake_lock);
	if (!cam_real_ask.pending) {
		lck_mtx_unlock(&cam_fake_lock);
		return;
	}
	lines = cam_real_ask.lines;
	gain = cam_real_ask.gain;
	cam_real_ask.pending = false;
	lck_mtx_unlock(&cam_fake_lock);
	if (lines < 4) {
		lines = 4;
	} else if (lines > (uint32_t)cfg->ae_frame_lines - 48) {
		lines = cfg->ae_frame_lines - 48;
	}
	if (gain < s->gain_unity) {
		gain = s->gain_unity;
	} else if (gain > s->gain_max) {
		gain = s->gain_max;
	}
	if (lines == cam_real_lines && gain == cam_real_gain) {
		return;
	}
	code = cfg == &cam_streams[0] ? 1024u - (1024u * s->gain_unity) / gain : gain;
	ok = cam_i2c_write_reg(&cam_real_stream.bus, cam_real_stream.s->addr7, 0x0202, 2, (uint16_t)lines, 2) &&
	    cam_i2c_write_reg(&cam_real_stream.bus, cam_real_stream.s->addr7, 0x0204, 2, (uint16_t)code, 2);
	if (!ok) {
		printf("vayu-camera: step 6: AE write (%u lines, gain code %#x) NOT ACKNOWLEDGED\n", lines, code);
		return;
	}
	cam_real_lines = lines;
	cam_real_gain = gain;
	if (cam_real_ae_writes++ < 3) {
		printf("vayu-camera: step 6: AE: exposure %u lines, gain %#x (code %#x)\n", lines, gain, code);
	}
}

static int
cam_real_frame(const struct cam_abi_sensor *s, int p, struct cam_frame_out *out)
{
	const struct cam_stream_cfg *cfg = s->stream;
	uint64_t start;
	vm_offset_t at;

	(void)p;
	out->exposure_lines = cam_real_lines;
	out->gain = cam_real_gain;      /* (linear: gain_unity = 1x; the sensor's code is written by cam_real_ae()) */
	out->frame_lines = cfg->ae_frame_lines;

	/*
	 * redwood's ios_session() scheme: WM0's address and the IFE's register update are taken at the
	 * NEXT start of frame. So at each SOF the frame now starting goes to the place given at the
	 * previous SOF (cam_real_next -> cam_real_writing), and the address for the frame after it is
	 * given at once (a whole frame's time to do it in). WM0's done then completes cam_real_writing.
	 * (cam8 01:54 programmed the place and waited for the frame already in flight: that frame went
	 * to the previous place, so readers got stale or half-overwritten frames: a tester's "jittery".)
	 */
	start = cam_fake_ms();
	for (;;) {
		uint32_t top = rd(g_ife_va + IFE_IRQ_STATUS0), bus = rd(g_ife_va + BUS_IRQ_STATUS1);

		if (top & CAM_RDI0_SOF) {
			int writing, next = -1;

			wr(g_ife_va + IFE_IRQ_CLEAR0, CAM_RDI0_SOF);
			wr(g_ife_va + IFE_IRQ_GLOBAL_CLEAR, 1);
			lck_mtx_lock(&cam_fake_lock);
			if (cam_real_next >= 0) {
				cam_real_writing = cam_real_next;
			} else {
				cam_real_late++;        /* no address was waiting: this frame goes where the last one went */
			}
			writing = cam_real_writing;
			cam_fake_place[writing].state = CAM_PLACE_WRITING;
			for (int i = 1; i < CAM_FAKE_RING; i++) {
				int c = (writing + i) % CAM_FAKE_RING;

				if (cam_fake_place[c].state != CAM_PLACE_READING &&
				    (next < 0 || cam_fake_place[c].sequence < cam_fake_place[next].sequence)) {
					next = c;
				}
			}
			cam_real_next = next;
			lck_mtx_unlock(&cam_fake_lock);
			if (next >= 0) {
				wr(g_ife_va + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + (uint64_t)next * cam_fake_place_bytes));
				wr(g_ife_va + IFE_REG_UPDATE, 0x2);
			}
		}
		if (bus & 1u) {
			wr(g_ife_va + BUS_IRQ_CLEAR1, 1);
			wr(g_ife_va + BUS_IRQ_GLOBAL_CLEAR, 1);
			break;
		}
		if (!cam_fake_open || cam_fake_switch_to != 0 || cam_fake_broken) {
			return CAM_FRAME_STOP;
		}
		if (cam_fake_ms() - start >= cam_real_timeout_ms) {
			cam_real_timeouts++;
			printf("vayu-camera: step 6: no frame in %u ms (%u timeout(s) in a row)\n",
			    cam_real_timeout_ms, cam_real_timeouts);
			if (cam_real_timeouts >= 3) {
				printf("vayu-camera: step 6: three frame timeouts in a row: the session ends\n");
				cam_fake_failed_why = "three frame timeouts";
				return CAM_FRAME_FAIL;
			}
			return CAM_FRAME_RETRY;
		}
		IOSleep(1);
	}
	cam_real_timeouts = 0;
	out->place = cam_real_writing;
	at = cam_fake_ring + (uint64_t)cam_real_writing * cam_fake_place_bytes;
	for (vm_offset_t line = at; line < at + cam_fake_frame_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	if (smmu_faults()) {
		printf("vayu-camera: step 6: the SMMU refused a request: the session ends\n");
		cam_fake_failed_why = "the SMMU refused a request";
		return CAM_FRAME_FAIL;
	}
	cam_real_ae(s);
	cam_af_frame();
	if ((cam_fake_frames_done + 1) % 100 == 0) {
		cam_site_means(s->name, (const uint8_t *)at, s->width, s->height, RAW10_STRIDE(s->width));
		printf("vayu-camera: step 6: exposure %u lines, gain %#x (unity %#x), %u AE writes\n", cam_real_lines,
		    cam_real_gain, s->gain_unity, cam_real_ae_writes);
		printf("vayu-camera: step 6: %llu frames done, %llu given, %u late arms, rx packets %u, CSID bytes %u, "
		    "smmu faults %s\n", (unsigned long long)(cam_fake_frames_done + 1),
		    (unsigned long long)cam_fake_frames_given, cam_real_late, rd(g_csid_va + CSID_CSI2_RX_PKTS),
		    rd(g_csid_va + CSID_RDI0_BYTES_PING), g_smmu_bad ? "SEEN" : "none");
	}
	return CAM_FRAME_DONE;
}

/*
 * The rear lens motor, a DW9800 VCM (module strings "vayu_imx582_dw9800_ofilm"; DT actuator_rear on CCI0 M1) — TLMM exception
 * 3, amendment 3 (maintainers, 2026-10-26): GPIO 24 (VAF 2.8 V) high, an ACK probe at 0x0c (no ACK: stop, no address scan), then
 * only register 0x02 (control: bit 0 power-down) and 0x03/0x04 (the 10-bit position, written as one 2-byte value at 0x03). On
 * exit: the lens to 0 in steps (not to knock it against its stop), 0x02 = 1 (power-down), VAF off. mediaserverd's own contrast
 * AF (camera_device.c's af_frame) drives the position through FOCUS commands once the frames report one.
 */
#define CAM_AF_ADDR             0x0c
#define CAM_AF_CONTROL          0x02
#define CAM_AF_POSITION         0x03
#define CAM_AF_START            300             /* a middle position to start from (redwood's DW9800 starts at 0x12c) */
static uint32_t cam_af_pos = HAKIOS_CAMERA_NO_FOCUS;   /* the lens's position, or NO_FOCUS while the motor is off */
static struct { uint32_t pos; bool pending; } cam_af_ask;
static uint32_t cam_af_moves;

static void
cam_af_vaf(bool on)
{
	char detail[80];

	(void)cam_gpio_set(24, on ? 1 : 0, detail, sizeof(detail));
	printf("vayu-camera: imx582: actuator VAF %s (%s)\n", on ? "on" : "off", detail);
}

static void
cam_af_up(void)
{
	struct cam_bus *bus = &cam_real_stream.bus;
	vm_offset_t page = cam_pin_page(24);
	uint32_t cfg;

	cam_af_pos = HAKIOS_CAMERA_NO_FOCUS;
	cam_af_ask.pending = false;
	cam_af_moves = 0;
	if (!g_af || cam_real_stream.cfg != &cam_streams[0]) {
		return;                 /* (the front S5K3T2 has no actuator: fixed focus) */
	}
	cfg = page != 0 ? rd(page) : 0;
	if (page == 0 || ((cfg >> TLMM_CFG_FUNC_SHFT) & TLMM_CFG_FUNC_MASK) != 0 || ((cfg >> TLMM_CFG_OE_SHFT) & 1) == 0) {
		printf("vayu-camera: imx582: gpio24 (actuator VAF) is not set up by the loader (cfg %#x): no autofocus\n", cfg);
		return;
	}
	cam_af_vaf(true);
	IOSleep(2);
	if (!cam_i2c_probe(bus, CAM_AF_ADDR)) {
		printf("vayu-camera: imx582: no lens motor at %#x (no ACK): no autofocus in this session\n", CAM_AF_ADDR);
		cam_af_vaf(false);
		return;
	}
	if (!cam_i2c_write_reg(bus, CAM_AF_ADDR, CAM_AF_CONTROL, 1, 0x00, 1)) {
		printf("vayu-camera: imx582: the lens motor did not take power-on (0x02 <- 0): no autofocus\n");
		cam_af_vaf(false);
		return;
	}
	IOSleep(1);
	if (!cam_i2c_write_reg(bus, CAM_AF_ADDR, CAM_AF_POSITION, 1, CAM_AF_START, 2)) {
		printf("vayu-camera: imx582: the lens motor did not take its first position: no autofocus\n");
		(void)cam_i2c_write_reg(bus, CAM_AF_ADDR, CAM_AF_CONTROL, 1, 0x01, 1);
		cam_af_vaf(false);
		return;
	}
	cam_af_pos = CAM_AF_START;
	printf("vayu-camera: imx582: lens motor DW9800 at %#x up, the lens at %u\n", CAM_AF_ADDR, cam_af_pos);
}

/* A pending FOCUS command, between frames (the session thread). */
static void
cam_af_frame(void)
{
	uint32_t pos;

	if (cam_af_pos == HAKIOS_CAMERA_NO_FOCUS) {
		return;
	}
	lck_mtx_lock(&cam_fake_lock);
	pos = cam_af_ask.pos;
	if (!cam_af_ask.pending) {
		lck_mtx_unlock(&cam_fake_lock);
		return;
	}
	cam_af_ask.pending = false;
	lck_mtx_unlock(&cam_fake_lock);
	pos = pos > 1023 ? 1023 : pos;
	if (pos == cam_af_pos) {
		return;
	}
	if (cam_i2c_write_reg(&cam_real_stream.bus, CAM_AF_ADDR, CAM_AF_POSITION, 1, (uint16_t)pos, 2)) {
		if (cam_af_moves++ < 5) {
			printf("vayu-camera: imx582: the lens from %u to %u\n", cam_af_pos, pos);
		}
		cam_af_pos = pos;
	}
}

static void
cam_af_down(void)
{
	if (cam_af_pos != HAKIOS_CAMERA_NO_FOCUS) {
		while (cam_af_pos > 0) {
			uint32_t next = cam_af_pos > 48 ? cam_af_pos - 48 : 0;

			if (!cam_i2c_write_reg(&cam_real_stream.bus, CAM_AF_ADDR, CAM_AF_POSITION, 1, (uint16_t)next, 2)) {
				break;
			}
			cam_af_pos = next;
			IOSleep(10);
		}
		(void)cam_i2c_write_reg(&cam_real_stream.bus, CAM_AF_ADDR, CAM_AF_CONTROL, 1, 0x01, 1);
		printf("vayu-camera: imx582: lens motor down (lens at %u, %u moves)\n", cam_af_pos, cam_af_moves);
		cam_af_pos = HAKIOS_CAMERA_NO_FOCUS;
		cam_af_vaf(false);
	}
}

static void
cam_real_stop(void)
{
	if (cam_real_stream.cfg == NULL) {
		return;
	}
	cam_af_down();
	cam_stream_down(&cam_real_stream);
}

static const struct cam_source cam_fake_real = {
	.step = "step 6",
	.fixed_exposure = true,
	.find = cam_real_find,
	.start = cam_real_start,
	.frame = cam_real_frame,
	.stop = cam_real_stop,
};

/* ---- steps 5 and 6: the session loop, one frame source at a time -------------------------------- */

/*
 * One session: the ring's places are reset, the source brings its hardware (or chart) up, and the
 * loop fills places until the last close, a sensor command, 1 s without reads, a failure or the
 * node's end. Step 5's exposure-change bookkeeping (from two frames on) is kept exactly as it
 * was; step 6's source takes EXPOSURE through cam_real_ask (cam_real_ae()), so this `ask' never becomes pending.
 * A failed session makes its reads EIO and is counted: the second one ends the node.
 */
static void
cam_session(const struct cam_abi_sensor *s, const struct cam_source *src)
{
	uint32_t lines, gain, frame_lines, failures;
	const char *why;
	uint64_t made, given, reads;
	bool failed = false, broken_now;

	lck_mtx_lock(&cam_fake_lock);
	cam_fake_frame_bytes = RAW10_STRIDE(s->width) * s->height;
	/*
	 * A read of the last session may still be copying out of the ring (the memory stays). Give it
	 * a bounded moment before the places are used again; the generation keeps an old read from
	 * writing a state of the new session back.
	 */
	for (unsigned waited = 0; cam_fake_readers != 0 && waited < 1000; waited += 10) {
		lck_mtx_unlock(&cam_fake_lock);
		IOSleep(10);
		lck_mtx_lock(&cam_fake_lock);
	}
	cam_fake_generation++;
	cam_fake_frames_done = cam_fake_frames_given = cam_fake_reads = 0;
	cam_fake_given_sequence = 0;
	for (int i = 0; i < CAM_FAKE_RING; i++) {
		cam_fake_place[i].state = CAM_PLACE_FREE;
		cam_fake_place[i].sequence = 0;
	}
	lines = cam_fake_ask.lines;
	gain = cam_fake_ask.gain;
	frame_lines = cam_fake_ask.frame_lines;
	cam_fake_ask.pending = false;
	cam_fake_limits(s, &lines, &gain, &frame_lines);
	cam_fake_at.lines = cam_fake_next.lines = lines;
	cam_fake_at.gain = cam_fake_next.gain = gain;
	cam_fake_at.frame_lines = cam_fake_next.frame_lines = frame_lines;
	cam_fake_next.from = 0;
	cam_fake_streaming = true;
	cam_fake_wanted = false;
	cam_fake_failed = false;
	cam_fake_failed_why = "a fault";
	cam_fake_exposure_said = false;
	cam_fake_last_read = cam_fake_ms();
	lck_mtx_unlock(&cam_fake_lock);

	if (!src->start(s, lines, gain, frame_lines)) {
		failed = true;
		cam_fake_failed_why = "the bring-up failed";
	}

	while (!failed && cam_fake_open && cam_fake_switch_to == 0 && !cam_fake_broken) {
		struct cam_frame_out out;
		int p = -1, rc;

		lck_mtx_lock(&cam_fake_lock);
		if (cam_fake_ms() - cam_fake_last_read > CAM_FAKE_IDLE_MS) {
			lck_mtx_unlock(&cam_fake_lock);
			break;
		}
		/* (this frame's number is frames_done + 1: a change asked for at frame N starts at N + 2) */
		if (!src->fixed_exposure && cam_fake_next.from != 0 &&
		    cam_fake_frames_done + 1 >= cam_fake_next.from) {
			cam_fake_at = cam_fake_next;
			cam_fake_next.from = 0;
		}
		for (int i = 0; i < CAM_FAKE_RING; i++) {
			if (cam_fake_place[i].state != CAM_PLACE_READING &&
			    (p < 0 || cam_fake_place[i].sequence < cam_fake_place[p].sequence)) {
				p = i;
			}
		}
		if (p >= 0) {
			cam_fake_place[p].state = CAM_PLACE_WRITING;
		}
		out.exposure_lines = cam_fake_at.lines;
		out.gain = cam_fake_at.gain;
		out.frame_lines = cam_fake_at.frame_lines;
		lck_mtx_unlock(&cam_fake_lock);
		if (p < 0) {
			IOSleep(src->fixed_exposure ? CAM_REAL_POLL_MS : 1000 / g_fake_fps);
			continue;
		}
		out.place = p;
		rc = src->frame(s, p, &out);
		lck_mtx_lock(&cam_fake_lock);
		if (rc == CAM_FRAME_DONE && out.place != p && out.place >= 0 && out.place < CAM_FAKE_RING) {
			/* (step 6: the frame went to the place latched at its SOF; the one asked for stays free) */
			cam_fake_place[p].state = CAM_PLACE_FREE;
			p = out.place;
		}
		if (rc == CAM_FRAME_DONE) {
			cam_fake_place[p].state = CAM_PLACE_READY;
			cam_fake_place[p].sequence = ++cam_fake_frames_done;
			cam_fake_place[p].time = mach_absolute_time();
			cam_fake_place[p].exposure_lines = out.exposure_lines;
			cam_fake_place[p].gain = out.gain;
			cam_fake_place[p].frame_lines = out.frame_lines;
			if (!src->fixed_exposure && cam_fake_ask.pending) {
				lines = cam_fake_ask.lines;
				gain = cam_fake_ask.gain;
				frame_lines = cam_fake_ask.frame_lines;
				cam_fake_ask.pending = false;
				cam_fake_limits(s, &lines, &gain, &frame_lines);
				cam_fake_next.lines = lines;
				cam_fake_next.gain = gain;
				cam_fake_next.frame_lines = frame_lines;
				cam_fake_next.from = cam_fake_frames_done + 2;   /* (the exposure from two frames on) */
			}
		} else {
			cam_fake_place[p].state = CAM_PLACE_FREE;
		}
		lck_mtx_unlock(&cam_fake_lock);
		if (rc == CAM_FRAME_DONE) {
			wakeup(&cam_fake_frames_done);
			if (!src->fixed_exposure) {
				IOSleep(1000 / g_fake_fps);
			}
		} else if (rc == CAM_FRAME_RETRY) {
			IOSleep(CAM_REAL_POLL_MS);
		} else if (rc == CAM_FRAME_FAIL) {
			failed = true;
			break;
		} else {
			break;          /* (closed or another sensor asked for: the loop's condition says which) */
		}
	}

	src->stop();
	lck_mtx_lock(&cam_fake_lock);
	cam_fake_streaming = false;
	for (int i = 0; i < CAM_FAKE_RING; i++) {
		cam_fake_place[i].state = CAM_PLACE_FREE;
	}
	made = cam_fake_frames_done;
	given = cam_fake_frames_given;
	reads = cam_fake_reads;
	if (failed) {
		cam_fake_failed = true;
		cam_fake_failures++;
		if (cam_fake_failures >= 2) {
			cam_fake_broken = true;
		}
	} else {
		/* a session that ended normally proves the source works: the next failure may retry */
		cam_fake_failures = 0;
	}
	failures = cam_fake_failures;
	broken_now = cam_fake_broken;
	why = !cam_fake_open ? "closed" : cam_fake_switch_to != 0 ? "another sensor asked for" :
	    failed ? cam_fake_failed_why : cam_fake_broken ? "a fault" : "nobody reads";
	lck_mtx_unlock(&cam_fake_lock);
	wakeup(&cam_fake_frames_done);
	printf("vayu-camera: %s: session stopped (%s) on sensor %u (%s): %llu frames made, %llu frames given, "
	    "%llu reads\n", src->step, why, s->sensor, s->name, (unsigned long long)made,
	    (unsigned long long)given, (unsigned long long)reads);
	if (failed) {
		printf("vayu-camera: %s: the failed session is %u of 2; its reads get EIO; %s\n", src->step,
		    failures, broken_now ? "the node ends" : "one more session may try");
	}
}

static int
cam_fake_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&cam_fake_lock);
	if (cam_fake_broken) {
		lck_mtx_unlock(&cam_fake_lock);
		return EIO;
	}
	cam_fake_opens++;
	cam_fake_open = true;
	lck_mtx_unlock(&cam_fake_lock);
	return 0;
}

/*
 * The cdevsw's close is the last close (XNU calls it when the device's use count drops to zero),
 * so the opens counted are forgotten here and the session ends; the source thread sees the flag.
 */
static int
cam_fake_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&cam_fake_lock);
	cam_fake_opens = 0;
	cam_fake_open = false;
	cam_fake_wanted = false;
	wakeup(&cam_fake_open);
	lck_mtx_unlock(&cam_fake_lock);
	return 0;
}

static int
cam_fake_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_frame f;
	const struct cam_abi_sensor *s;
	uint64_t since = cam_fake_ms();
	uint32_t generation, frame_bytes;
	int b = -1, error = 0;

	lck_mtx_lock(&cam_fake_lock);
	if (cam_fake_broken || cam_fake_now == NULL) {
		lck_mtx_unlock(&cam_fake_lock);
		return EIO;
	}
	/*
	 * If the last session failed and is over, this read is the first of the retry: clear the
	 * session's EIO and let the server try once more (cam_fake_failures counts the failures, and
	 * the second one closes the node). A read that arrives during the failed session itself sees
	 * the flag below and gets EIO.
	 */
	if (!cam_fake_streaming && !cam_fake_wanted) {
		cam_fake_failed = false;
	}
	cam_fake_last_read = cam_fake_ms();
	cam_fake_reads++;
	if (!cam_fake_streaming && !cam_fake_wanted) {
		cam_fake_wanted = true;
		wakeup(&cam_fake_open);
	}
	for (;;) {
		if (cam_fake_broken || cam_fake_failed) {
			lck_mtx_unlock(&cam_fake_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < CAM_FAKE_RING; i++) {
			if (cam_fake_place[i].state == CAM_PLACE_READY && cam_fake_place[i].sequence > cam_fake_given_sequence &&
			    (b < 0 || cam_fake_place[i].sequence > cam_fake_place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (cam_fake_ms() - since >
		    (cam_fake_streaming && cam_fake_frames_done != 0 ? CAM_FAKE_STREAM_MS : CAM_FAKE_FIRST_MS)) {
			lck_mtx_unlock(&cam_fake_lock);
			return EAGAIN;
		}
		if (!cam_fake_streaming && !cam_fake_wanted) {
			cam_fake_wanted = true;         /* (the frames stopped meanwhile: another sensor) */
			wakeup(&cam_fake_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };

		error = msleep(&cam_fake_frames_done, &cam_fake_lock, PZERO | PCATCH, "vayu-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&cam_fake_lock);
			return error;
		}
		cam_fake_last_read = cam_fake_ms();
	}
	cam_fake_place[b].state = CAM_PLACE_READING;
	generation = cam_fake_generation;
	cam_fake_readers++;
	cam_fake_given_sequence = cam_fake_place[b].sequence;
	cam_fake_frames_given++;
	s = cam_fake_now;
	frame_bytes = cam_fake_frame_bytes;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.header_bytes = (uint32_t)sizeof(f);
	f.width = s->width;
	f.height = s->height;
	f.stride = RAW10_STRIDE(s->width);
	f.format = 1;
	f.bayer = s->bayer;
	f.black = s->black;
	f.sequence = cam_fake_place[b].sequence;
	f.time = cam_fake_place[b].time;
	f.exposure_lines = cam_fake_place[b].exposure_lines;
	f.line_ns = s->line_ns;
	f.gain = cam_fake_place[b].gain;
	f.gain_unity = s->gain_unity;
	f.gain_max = s->gain_max;
	f.frame_lines = cam_fake_place[b].frame_lines;
	f.frame_lines_min = s->frame_lines_min;
	f.frame_lines_max = s->frame_lines_max;
	f.margin = s->margin;
	f.sensor = s->sensor;
	f.focus = cam_fake_source == &cam_fake_real ? cam_af_pos : HAKIOS_CAMERA_NO_FOCUS;
	lck_mtx_unlock(&cam_fake_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		vm_offset_t at = cam_fake_ring + (uint64_t)b * cam_fake_place_bytes;
		uint32_t n = (uint32_t)MIN((uint64_t)frame_bytes, (uint64_t)uio_resid(uio));

		/* (step 5's chart is the CPU's; step 6 invalidated the place when WM0 was done) */
		error = uiomove((caddr_t)at, (int)n, uio);
	}
	lck_mtx_lock(&cam_fake_lock);
	if (generation == cam_fake_generation && cam_fake_place[b].state == CAM_PLACE_READING) {
		cam_fake_place[b].state = CAM_PLACE_READY;
	}
	if (cam_fake_readers != 0) {
		cam_fake_readers--;
	}
	cam_fake_last_read = cam_fake_ms();
	lck_mtx_unlock(&cam_fake_lock);
	return error;
}

static int
cam_fake_dev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_command c;
	uint32_t lines, gain, frame_lines;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	if (c.command == HAKIOS_CAMERA_EXPOSURE) {
		lines = c.a;
		gain = c.b;
		frame_lines = c.c;
		lck_mtx_lock(&cam_fake_lock);
		if (cam_fake_source->fixed_exposure) {
			/* step 6: the session thread writes it between frames (cam_real_ae()); frame lines stay fixed */
			cam_real_ask.lines = lines;
			cam_real_ask.gain = gain;
			cam_real_ask.pending = true;
			lck_mtx_unlock(&cam_fake_lock);
			return 0;
		}
		if (cam_fake_now != NULL) {
			cam_fake_limits(cam_fake_now, &lines, &gain, &frame_lines);
		}
		cam_fake_ask.lines = lines;
		cam_fake_ask.gain = gain;
		cam_fake_ask.frame_lines = frame_lines;
		cam_fake_ask.pending = true;
		lck_mtx_unlock(&cam_fake_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_SENSOR) {
		if (c.a != 1 && c.a != 2) {
			return EINVAL;
		}
		lck_mtx_lock(&cam_fake_lock);
		cam_fake_switch_to = c.a;       /* (b, the mode, is ignored: each sensor has one) */
		lck_mtx_unlock(&cam_fake_lock);
		wakeup(&cam_fake_open);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_FOCUS) {
		/* step 6 with the lens motor up: applied between frames (cam_af_frame()); otherwise ignored (NO_FOCUS) */
		lck_mtx_lock(&cam_fake_lock);
		cam_af_ask.pos = c.a;
		cam_af_ask.pending = true;
		lck_mtx_unlock(&cam_fake_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_KEEP) {
		return EINVAL;                  /* (no picture for the host in steps 5 and 6) */
	}
	return EINVAL;
}

static const struct cdevsw cam_fake_cdevsw = {
	.d_open = cam_fake_dev_open, .d_close = cam_fake_dev_close, .d_read = cam_fake_dev_read,
	.d_write = cam_fake_dev_write, .d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset,
	.d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool cam_fake_dev_made;

static void
cam_fake_dev_make(void)
{
	int major = cdevsw_add(-1, &cam_fake_cdevsw);

	cam_fake_dev_made = major >= 0 &&
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-camera") != NULL;
}

/* a session whenever a read wants frames, another sensor whenever one is asked for, until a fault */
static void
cam_fake_serve(void)
{
	if (!cam_fake_dev_made) {
		return;
	}
	while (!cam_fake_broken) {
		lck_mtx_lock(&cam_fake_lock);
		while (!cam_fake_broken && !(cam_fake_open && cam_fake_wanted) && cam_fake_switch_to == 0) {
			(void)msleep(&cam_fake_open, &cam_fake_lock, PZERO, "vayu-camera-idle", NULL);
		}
		uint32_t to = cam_fake_switch_to;

		cam_fake_switch_to = 0;
		lck_mtx_unlock(&cam_fake_lock);
		if (cam_fake_broken) {
			break;
		}
		if (to != 0) {
			if (!cam_fake_select(to)) {
				printf("vayu-camera: %s: sensor %u is not one of the two\n", cam_fake_source->step, to);
				cam_fake_broken = true;
				break;
			}
			continue;
		}
		lck_mtx_lock(&cam_fake_lock);
		cam_fake_wanted = false;
		lck_mtx_unlock(&cam_fake_lock);
		cam_session(cam_fake_now, cam_fake_source);
	}
	printf("vayu-camera: %s: /dev/hakios-camera: no more frames in this run\n", cam_fake_source->step);
}

/*
 * Step 5: /dev/hakios-camera with the synthetic chart. No step 1 (nothing is read from the
 * camera's blocks), no TLMM, no I2C, no rail, no RPMh: only the ring and the device node.
 */
static void
cam_step5(void)
{
	cam_fake_source = &cam_fake_synthetic;
	if (!cam_fake_select(1)) {
		printf("vayu-camera: step 5: no sensor table\n");
		return;
	}
	cam_fake_dev_make();
	printf("vayu-camera: step 5: /dev/hakios-camera (synthetic frames, %u fps)\n", g_fake_fps);
	if (!cam_fake_dev_made) {
		printf("vayu-camera: step 5: /dev/hakios-camera cannot be made\n");
		return;
	}
	if (!cam_fake_ring_alloc(cam_fake_sensors, CAM_FAKE_SENSOR_COUNT, "step 5")) {
		cam_fake_broken = true;
		printf("vayu-camera: step 5: no ring: reads will say EIO\n");
		return;
	}
	cam_fake_serve();
}

/*
 * Step 6: /dev/hakios-camera fed by the real cameras. Step 1 is not run: only GCC and camcc are
 * mapped, from camera_ahb exactly as step 1 does (cam_gcc_camcc()). The node, the ring and the
 * session code are step 5's; each session is cam_stream_up() (step 4's bring-up, with the ring as
 * the IFE's target and RDI 0 running) and cam_stream_down().
 */
static void
cam_step6(void)
{
	cam_fake_source = &cam_fake_real;
	printf("vayu-camera: step 6: /dev/hakios-camera, fed by the real sensors (1 = rear IMX582, "
	    "2 = front S5K3T2)\n");
	if (!g_xnu_tlmm) {
		printf("vayu-camera: step 6 needs vayu_camera_xnu_tlmm=1 (TLMM writes): not done\n");
		return;
	}
	if (!cam_gcc_camcc() || !g_camcc_ok || camcc_va == 0) {
		printf("vayu-camera: the camera AHB clock was not enabled: step 6 not done\n");
		return;
	}
	if (!cam_fake_select(1)) {
		printf("vayu-camera: step 6: no sensor table\n");
		return;
	}
	cam_fake_dev_make();
	if (!cam_fake_dev_made) {
		printf("vayu-camera: step 6: /dev/hakios-camera cannot be made\n");
		return;
	}
	if (!cam_fake_ring_alloc(cam_real_sensors, CAM_REAL_SENSOR_COUNT, "step 6")) {
		cam_fake_broken = true;
		printf("vayu-camera: step 6: no ring: reads will say EIO\n");
		return;
	}
	cam_fake_serve();
}

/* ---- quiesce (loader return, panic path) -------------------------------------------------------- */

/*
 * Called on the way back to the loader when a step-2, step-4 or step-6 sensor is still powered:
 * the MCLKs this run left running are stopped and the output pins it raised are lowered (the reset
 * goes low). And when step 3, 4 or 6 has the pipeline running: the test pattern off (nothing for
 * step 6: TPG is not its), WM0 off, the SMMU's match register and S2CR back to the values found
 * and the context switched off. No I2C, no RPMh, no lock, no sleep, no polling, one line each.
 * Step 5 has no hardware: there is nothing here for it. Step 6's sessions leave the same flags as
 * step 4's (cam_sensor_power_up() sets g_sensor_powered, cam_stream_up() sets g_pipeline_running),
 * so this takes them down too.
 */
void
vayu_camera_quiesce(void)
{
	if (g_sensor_powered) {
		if (camcc_va != 0) {
			for (unsigned n = 0; n < 4; n++) {
				uint32_t cbcr;

				if ((g_mclk_on_mask & (1u << n)) == 0) {
					continue;
				}
				cbcr = CAMCC_MCLK_CBCR(n);
				wr(camcc_va + cbcr, rd(camcc_va + cbcr) & ~GCC_CBCR_ENABLE);
			}
			g_mclk_on_mask = 0;
		}
		for (unsigned i = 0; i < CAM_PIN_COUNT; i++) {
			if ((g_raised & (1u << i)) == 0 || cam_pin_va[i] == 0) {
				continue;
			}
			wr(cam_pin_va[i] + 4, rd(cam_pin_va[i] + 4) & ~TLMM_IO_OUT);
		}
		g_raised = 0;
		g_sensor_powered = false;
		loader_log_line("vayu-camera: quiesced\n");   /* (not printf: interrupts are off here) */
	}
	if (g_pipeline_running) {
		if (g_csid_va != 0) {
			wr(g_csid_va + CSID_TPG_CTRL, 0);
		}
		if (g_ife_va != 0) {
			wr(g_ife_va + WM0_CFG, 0);
		}
		if (g_smmu_va != 0 && g_smmu_match != ~0u) {
			wr(g_smmu_va + SMMU_SMR(g_smmu_match), g_smmu_old_smr);
			wr(g_smmu_va + SMMU_S2CR(g_smmu_match), g_smmu_old_s2cr);
		}
		if (g_smmu_context != 0) {
			wr(g_smmu_context + SMMU_CB_SCTLR, 0);
		}
		g_pipeline_running = false;
		loader_log_line("vayu-camera: quiesced (paths off, SMMU put back)\n");
	}
}

/* ---- the start --------------------------------------------------------------------------------- */

static void
vayu_camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (g_level == 5) {
		/* step 5 is the node and its synthetic source only: step 1 is not run */
		cam_step5();
		thread_terminate(current_thread());
		return;
	}
	if (g_level == 6) {
		/* step 6 is the same node fed by the real cameras: step 1 is not run either */
		cam_step6();
		thread_terminate(current_thread());
		return;
	}
	cam_step1();
	if (g_level == 2) {
		cam_step2();
	} else if (g_level == 3) {
		cam_step3();
	} else if (g_level >= 4) {
		if (g_both) {
			/* vayu_camera_cam=2: the rear, then the front, each with its whole teardown */
			g_cam = 0;
			cam_step4();
			g_smmu_bad = g_smmu_said = false;
			g_cam = 1;
			cam_step4();
		} else {
			cam_step4();
		}
	}
	thread_terminate(current_thread());
}

void
vayu_camera_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("vayu_camera", &g_level, sizeof(g_level)) || g_level == 0) {
		return;
	}
	if (g_level > 6) {
		printf("vayu-camera: only steps 1-6 exist\n");
		g_level = 6;
	}
	PE_parse_boot_argn("vayu_camera_sensors", &g_sensor_mask, sizeof(g_sensor_mask));
	PE_parse_boot_argn("vayu_camera_xnu_tlmm", &g_xnu_tlmm, sizeof(g_xnu_tlmm));
	PE_parse_boot_argn("vayu_camera_shared", &g_shared, sizeof(g_shared));
	PE_parse_boot_argn("vayu_camera_pause", &g_pause_ms, sizeof(g_pause_ms));
	PE_parse_boot_argn("vayu_camera_hold", &g_hold_ms, sizeof(g_hold_ms));
	PE_parse_boot_argn("vayu_camera_clk", &g_clk, sizeof(g_clk));
	PE_parse_boot_argn("vayu_camera_tpg_w", &g_tpg_w, sizeof(g_tpg_w));
	PE_parse_boot_argn("vayu_camera_tpg_h", &g_tpg_h, sizeof(g_tpg_h));
	PE_parse_boot_argn("vayu_camera_settle", &g_settle, sizeof(g_settle));
	PE_parse_boot_argn("vayu_camera_frames_ms", &g_frames_ms, sizeof(g_frames_ms));
	PE_parse_boot_argn("vayu_camera_skip_ms", &g_skip_ms, sizeof(g_skip_ms));
	PE_parse_boot_argn("vayu_camera_exposure", &g_exposure, sizeof(g_exposure));
	PE_parse_boot_argn("vayu_camera_gain", &g_gain, sizeof(g_gain));
	PE_parse_boot_argn("vayu_camera_ae", &g_ae, sizeof(g_ae));
	PE_parse_boot_argn("vayu_camera_af", &g_af, sizeof(g_af));
	g_exposure &= 0xffff;
	g_gain &= 0xffff;
	PE_parse_boot_argn("vayu_camera_fake_fps", &g_fake_fps, sizeof(g_fake_fps));
	if (g_fake_fps < 1) {
		g_fake_fps = 1;
	} else if (g_fake_fps > 30) {
		g_fake_fps = 30;
	}
	PE_parse_boot_argn("vayu_camera_cam", &g_cam, sizeof(g_cam));
	if (g_cam == 2) {
		g_both = true;
		g_cam = 0;
	} else if (g_cam > 1) {
		printf("vayu-camera: vayu_camera_cam=%u is not 0 (rear IMX582), 1 (front S5K3T2) or 2 (both): using 0\n",
		    g_cam);
		g_cam = 0;
	}
	g_stream = &cam_streams[g_cam];

	if (g_level == 4 || g_level == 6) {
		/*
		 * Step 4's and step 6's CSIPHY timer runs from PLL0's even output: it cannot use XO.
		 * (Step 4's line is unchanged; step 6 says its own step number.)
		 */
		g_clk = 1;
		printf("vayu-camera: step %u: vayu_camera_clk forced to 1 (camcc PLL0)\n", g_level);
	}
	if (kernel_thread_start(vayu_camera_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("vayu-camera: the probe's thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}

/* The tee (see the top): printf's text, then the same bytes into the loader's log. */
#undef printf
static int
cam_printf(const char *format, ...)
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
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		printf("%s", line);
	}
	return length;
}
