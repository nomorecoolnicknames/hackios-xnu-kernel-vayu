/*
 * The screen off beyond its backlight (vayu): bsd/dev/vayu_display_power.c.
 */
#ifndef _SYS_VAYU_DISPLAY_POWER_H_
#define _SYS_VAYU_DISPLAY_POWER_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS
void     vayu_display_power_state(bool on);     /* iOS's RequestPowerChange, after the backlight */
void     vayu_display_power_wake(void);         /* before the backlight is lit (vayu_wled_power(true)) */
uint32_t vayu_display_touch_hz(void);           /* touch_off_hz while the display is off, else 0 */
bool     vayu_display_is_off(void);
__END_DECLS

#endif /* _SYS_VAYU_DISPLAY_POWER_H_ */
