/*
 * The speakers of the POCO X3 Pro (vayu, SM8150-AC), bsd/dev/vayu_audio.c: two TI smart amplifiers
 * (TAS2562 at 0x4c, TAS2564 at 0x4d) on quaternary MI2S, which the audio DSP drives.
 */
#ifndef _SYS_VAYU_AUDIO_H_
#define _SYS_VAYU_AUDIO_H_

#ifdef KERNEL
void vayu_audio_init(void);             /* boot-arg vayu_audio=N: the first N steps (see vayu_audio.c) */
void vayu_audio_quiesce(void);          /* both amplifiers into hardware shutdown (SDZ low): on every way back to the loader */
#endif

#endif /* _SYS_VAYU_AUDIO_H_ */
