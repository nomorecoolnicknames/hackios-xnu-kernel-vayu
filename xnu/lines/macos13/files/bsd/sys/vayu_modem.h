/*
 * The modem of the POCO X3 Pro (vayu, SM8150-AC): a probe, bsd/dev/vayu_modem.c.
 * vayu_modem_init() is called where vayu_audio_init() is; vayu_modem_quiesce() on a panic's way
 * back to the loader, as vayu_audio_quiesce() is.
 */
#ifndef _SYS_VAYU_MODEM_H_
#define _SYS_VAYU_MODEM_H_

#ifdef KERNEL
void vayu_modem_init(void);
void vayu_modem_quiesce(void);
#endif

#endif /* _SYS_VAYU_MODEM_H_ */
