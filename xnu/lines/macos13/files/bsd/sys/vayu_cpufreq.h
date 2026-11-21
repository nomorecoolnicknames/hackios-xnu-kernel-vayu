/*
 * The processor cores' clock on the POCO X3 Pro (vayu, SM8150-AC), bsd/dev/vayu_cpufreq.c.
 *
 * Both do nothing unless the boot args say cpufreq=1 cpufreq_vayu=1: the addresses and the
 * register layout are that SoC's, and redwood (SM7325) has different ones.
 */
#ifndef _SYS_VAYU_CPUFREQ_H_
#define _SYS_VAYU_CPUFREQ_H_

void vayu_cpufreq_init(void);           /* cpufreq=1 cpufreq_vayu=1: cores and L3 top row; vayu_ddr=1: memory bandwidth */
void vayu_cpufreq_quiesce(void);        /* on the way back to the loader: the row the firmware had left */

#endif /* _SYS_VAYU_CPUFREQ_H_ */
