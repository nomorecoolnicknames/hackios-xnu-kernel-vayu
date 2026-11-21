/*
 * The sensors DSP (SLPI) of the POCO X3 Pro (vayu, SM8150-AC): a probe, bsd/dev/vayu_slpi.c.
 * vayu_slpi_init() is called where vayu_audio_init() is; vayu_slpi_quiesce() on a panic's way
 * back to the loader, as vayu_audio_quiesce() is.
 */
#ifndef _SYS_VAYU_SLPI_H_
#define _SYS_VAYU_SLPI_H_

#ifdef KERNEL
void vayu_slpi_init(void);
void vayu_slpi_quiesce(void);
#endif

#endif /* _SYS_VAYU_SLPI_H_ */
