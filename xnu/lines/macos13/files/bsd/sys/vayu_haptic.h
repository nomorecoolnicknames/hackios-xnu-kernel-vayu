/*
 * The vibration motor of the POCO X3 Pro (vayu, SM8150-AC): the haptics block of the PM8150B
 * (SPMI slave 3, registers 0xc000), bsd/dev/vayu_haptic.c.
 *
 *   vayu_haptic=1   step 1 only: a read-only look (run 164539)
 *   vayu_haptic=2   step 1, the vendor's LRA setup, and two test pulses
 *                   (vayu_haptic_ms, vayu_haptic_mv)
 *   vayu_haptic=3   step 1, the setup, and iOS's motor at /dev/hakios-haptic
 *                   (vayu_haptic_tap_ms, vayu_haptic_max_ms)
 *
 * Never above 2800 mV; only thirteen registers, every one read back; SC_DET stops the motor.
 */
#ifndef _SYS_VAYU_HAPTIC_H_
#define _SYS_VAYU_HAPTIC_H_

void vayu_haptic_init(void);            /* boot-arg vayu_haptic=1, 2 or 3 */
void vayu_haptic_quiesce(void);         /* at the run's end and on a panic: motor off, step 3's summary */

#endif /* _SYS_VAYU_HAPTIC_H_ */
