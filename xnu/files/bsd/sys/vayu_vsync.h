/*
 * The display controller's timing engine, bsd/dev/vayu_vsync.c: where the panel's refresh is
 * really counted. Step 1 only reads; see that file for the facts and the boot-arg.
 */
#ifndef _SYS_VAYU_VSYNC_H_
#define _SYS_VAYU_VSYNC_H_

#include <stdint.h>

void vayu_vsync_init(void);             /* vayu_vsync=1: a look at INTF 1/2; =2: and the framebuffer follows the panel */
uint64_t vayu_vsync_next(uint64_t now); /* the absolute time of the panel's next frame start; 0 until step 2 armed */
uint64_t vayu_vsync_frame(void);        /* one frame in absolute-time units; 0 until step 2 armed */

#endif /* _SYS_VAYU_VSYNC_H_ */
