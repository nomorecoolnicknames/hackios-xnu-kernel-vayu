/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/sys/vayu_wled.h; K5 (xnu-12377): started by its VAYU_HOOK, + the quiesce.
 */
/*
 * The panel's backlight of the POCO X3 Pro (vayu): the PM8150L's WLED, bsd/dev/vayu_wled.c.
 */
#ifndef _SYS_VAYU_WLED_H_
#define _SYS_VAYU_WLED_H_

#include <stdbool.h>
#include <stdint.h>

/* vayu27=1 vayu_wled=1: a look; =2: writes allowed (then SweetFramebuffer's hakios_display_power/brightness) */
bool vayu_wled_power(bool on);                  /* the WLED module on or off (vayu_wled=2); true if done */
bool vayu_wled_brightness(uint32_t permille);   /* 1..1000 of the modulator's full scale (vayu_wled=2) */
void vayu_wled_quiesce(void);                   /* the module bit as found, on the way to the loader */

#endif /* _SYS_VAYU_WLED_H_ */
