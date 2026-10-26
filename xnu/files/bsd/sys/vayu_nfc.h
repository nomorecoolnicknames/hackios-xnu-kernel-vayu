/*
 * NFC on the POCO X3 Pro (vayu, SM8150): the one entry point of bsd/dev/vayu_nfc.c.
 *
 * That file looks at the phone's NFC controller, a NXP PN553 at I2C address 0x28 on
 * i2c@a84000 (QUP serial engine 9), and -- behind this port's TLMM exception -- talks NCI
 * to it over two bit-banged pins. Nothing here runs unless boot-arg vayu_nfc=N asks for
 * it; step 1 is read-only, steps 2-4 need vayu_nfc_xnu_tlmm=1.
 *
 * The facts, with their sources:
 *
 *   the chip    NXP PN553. Vendor HAL config libnfc-nxp.conf NXP_NFC_CHIP=0x07
 *               (TheMuppets/proprietary_vendor_xiaomi_vayu, lineage-22.2, :241); its clock
 *               NXP_SYS_CLK_SRC_SEL=0x02 (PLL) at 19.2 MHz (:39, :50).
 *   the node    live DT firmware/vayu/dt/recovery-live.dtb, decompiled live.dts:23428-23440:
 *               nq@28 with qcom,nq-irq GPIO 47, qcom,nq-ven GPIO 102, qcom,nq-firm GPIO 48,
 *               qcom,nq-clkreq GPIO 113. Its bus pins: GPIO 39 SDA and 40 SCL, function
 *               qup9 (qupv3_se9_i2c_active, live.dts:12617-12631).
 *   TLMM        pinctrl-sm8150.c: NORTH tile 0x03900000 (39, 40, 102), EAST tile
 *               0x03500000 (47, 48); cfg = tile + 0x1000 * pin, io = cfg + 4.
 *               cfg [1:0] pull, [5:2] function, [8:6] drive ((n + 1) * 2 mA), [9] output
 *               enable; io bit 0 is the input level, bit 1 the output value.
 *   the vendor  MiCode vayu-r-oss drivers/nfc/nq-nci.c (0c9888aa): IRQ rising edge, read
 *               only while it is high (:219-245, :1479); 1 ms after every write (:331);
 *               power on = FIRM 0, VEN 1, 10 ms each (:637-646).
 *
 * Boot-args: vayu_nfc=N runs steps 1..N and stops at the first that fails; N is 1..4.
 * vayu_nfc_xnu_tlmm=1 allows the pin writes of steps >= 2 (pins 39/40 only, cfg bit 9
 * and io bit 1, given back after every transaction); vayu_nfc_secs=S is step 3's polling
 * time (default 120, max 3600); vayu_nfc_tech=MASK selects NFC-A/B/F/V (default 0x7,
 * bit 3 is ISO15693); vayu_nfc_pause=MS sleeps before step 1 (default 2000).
 *
 * This port's rule still stands: XNU does not write TLMM outside that exception, and the
 * PN553's FIRM pin is never raised (no firmware download, no write to the chip's flash).
 */

#ifndef _SYS_VAYU_NFC_H_
#define _SYS_VAYU_NFC_H_

/*
 * The NFC probe and, with vayu_nfc_xnu_tlmm=1, steps 2-4: one kernel thread that prints
 * what it finds and returns. It leaves every pin as it found it.
 */
extern void vayu_nfc_init(void);

#endif /* _SYS_VAYU_NFC_H_ */
