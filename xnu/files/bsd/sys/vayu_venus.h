/*
 * The hardware video encoder of the POCO X3 Pro (vayu, SM8150-AC): Venus / IRIS1,
 * bsd/dev/vayu_venus.c.
 *
 * vayu_venus_init() starts a kernel thread.  Nothing at all is touched unless boot-arg
 * vayu_venus=1 asks for it.  Milestone M1 does, in this order, and only logs:
 *
 *   1. the shared RPMh votes "mmcx.lvl" and "mx.lvl" (raised once, never given back: they
 *      are the whole multimedia island's, as the display's and the camera's are), and the
 *      video path's four RPMh BCM votes -- MM3 (the two video masters), SH0 (LLCC), MC0
 *      (DDR) and ACV (the DDR corner) -- which nothing in this file can replace: without
 *      them the ARM9 cannot fetch its image.  MM3 is the video's own and is given back;
 *      SH0/MC0/ACV are the whole system's memory path and are only ever raised here;
 *   2. GCC's video AHB/XO/AXI branch clocks, video_cc's XO branch, the mvsc GDSC and (only
 *      if vayu_venus_gdsc asks for it) the mvs0/mvs1 GDSCs with the wrapper's MMCC
 *      power-control handshake.  mvs0/mvs1 are hardware-triggered on this phone
 *      ("qcom,support-hw-trigger"): HW_CONTROL is cleared to switch them and set again
 *      afterwards, as the vendor GDSC driver does;
 *   3. video_cc's PLL0 (Trion, 2 x the wanted iris rate) and the iris_clk_src RCG, then
 *      the iris AHB and mvsc core branches;
 *   4. venus.mdt + venus.bNN through vayu_pas.c's PAS loader (INIT_IMAGE, MEM_SETUP, the
 *      image written and read back), then PAS AUTH_AND_RESET (pas-id 9);
 *   5. the three HFI queues and the SFR, 38 MB of contiguous memory behind our own apps-
 *      SMMU context (SID 0x2300 / mask 0x60 as this phone's live DT says, IOVA
 *      0x25800000), uncached on our side;
 *   6. UC_REGION_ADDR/SIZE, CPU_CS_SCIACMDARG1/2, SFR_ADDR, VIDC_CTRL_INIT, the host-to-
 *      firmware doorbell, and a bounded poll for HFI_MSG_SYS_INIT;
 *   7. HFI debug config, the codec power-plane property, the firmware image version and a
 *      SYS_PING; the version string is logged if the firmware answers;
 *   8. everything down again in reverse (PAS shutdown first, then the SMMU, the queues,
 *      the clocks, PLL0 and the GDSCs, then the video NoC's MM3 vote; SH0/MC0/ACV are left
 *      up).  The two RPMh rails are shared and stay up.
 *
 * The firmware-to-host interrupt is MASKED in the wrapper for M1 (WRAPPER_INTR_MASK = 0)
 * and the message queue is polled: no GIC handler exists yet, and a level line left
 * asserted by the firmware on an unhandled SPI is a boot hazard.  M2 will install the
 * GIC SPI 174 handler and unmask A2HVCODEC.
 *
 * Boot-args (nothing at all unless vayu_venus is at least 1):
 *   vayu_venus=1        M1 (levels above 1 say only M1 exists, and M1 is done)
 *   vayu_venus_wait=MS  the SYS_INIT/version/ping poll, default 3000 ms (100..30000)
 *   vayu_venus_secs=S   kept up after M1 succeeds, for late messages, default 2 (0..30)
 *   vayu_venus_rails=0  no RPMh vote at all (default 1: mmcx.lvl and mx.lvl)
 *   vayu_venus_level=N  that vote's RPMh level, default 384 (TURBO)
 *   vayu_venus_bcm=0    no video NoC/DDR BCM votes (default 1: MM3, SH0, MC0, ACV)
 *   vayu_venus_rate=N   iris clock in MHz, default 200 (200/225/300/365/432/480)
 *   vayu_venus_xo=1     no PLL0: the iris RCG from XO at 19.2 MHz (a diagnostic)
 *   vayu_venus_gdsc=M   bit 0 mvsc (default), bit 1 mvs0, bit 2 mvs1
 *   vayu_venus_debug=N  HFI debug config, default 0x18 (ERROR | FATAL)
 *
 * What M1 does NOT do: no HFI session, no NV12/bitstream buffer, no VideoToolbox device,
 * no secure (content-protection) context, no GIC handler, and nothing is left powered up
 * when the thread ends.
 *
 * vayu_venus_quiesce() is for the loader-return/panic path: it masks the firmware's
 * interrupt, shuts the firmware down through TrustZone, turns our SMMU context off and
 * puts it back, cuts the branches/GDSCs (HW_CONTROL first) and gives the MM3 vote back --
 * no lock, no sleep, no polling beyond what the SMC/RPMh calls need, no memory freed, one
 * line each.  It does nothing when M1 never started the firmware.
 *
 * The packets and the queue table are sys/vayu_venus_hfi.h; tools/venus-hfi tests them.
 */
#ifndef _SYS_VAYU_VENUS_H_
#define _SYS_VAYU_VENUS_H_

#ifdef KERNEL
void vayu_venus_init(void);
void vayu_venus_quiesce(void);
#endif

#endif /* _SYS_VAYU_VENUS_H_ */
