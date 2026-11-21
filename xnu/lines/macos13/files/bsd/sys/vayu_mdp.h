/*
 * The display controller as UEFI left it (vayu), bsd/dev/vayu_mdp.c: boot-arg vayu_mdp=1, a look, reading only.
 */
#ifndef _SYS_VAYU_MDP_H_
#define _SYS_VAYU_MDP_H_

#include <stdbool.h>
#include <stdint.h>

void vayu_mdp_init(void);
bool vayu_mdp_scanout(uint64_t phys, uint32_t stride, uint32_t x, uint32_t y); /* fb_zero: VIG0 fetches that surface (crop at x,y); true once the flush is taken */
void vayu_mdp_unscanout(void);         /* the boot framebuffer back, its stride and offset too */
void vayu_mdp_quiesce(void);           /* vayu_mdp=2: the boot framebuffer back on the panel (loader_return.c) */

#endif /* _SYS_VAYU_MDP_H_ */
