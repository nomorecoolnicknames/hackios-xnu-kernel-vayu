/*
 * The panel's backlight of the POCO X3 Pro (vayu): the PM8150L's WLED, bsd/dev/vayu_wled.c.
 */
#ifndef _SYS_VAYU_WLED_H_
#define _SYS_VAYU_WLED_H_

#include <stdbool.h>
#include <stdint.h>

void vayu_wled_init(void);                      /* boot-arg vayu_wled=1: a look; =2: writes allowed */
bool vayu_wled_power(bool on);                  /* the WLED module on or off (vayu_wled=2); true if done */
bool vayu_wled_brightness(uint32_t permille);   /* 1..1000 of the modulator's full scale (vayu_wled=2) */

#endif /* _SYS_VAYU_WLED_H_ */
