/*
 * The speakers of the POCO X3 Pro (vayu, SM8150-AC), bsd/dev/vayu_audio.c: two TI smart amplifiers
 * (TAS2562 at 0x4c, TAS2564 at 0x4d) on quaternary MI2S, which the audio DSP drives.
 */
#ifndef _SYS_VAYU_AUDIO_H_
#define _SYS_VAYU_AUDIO_H_

#ifdef KERNEL
void vayu_audio_init(void);             /* boot-arg vayu_audio=N: the first N steps (see vayu_audio.c) */
void vayu_audio_quiesce(void);          /* both amplifiers into hardware shutdown (SDZ low): on every way back to the loader */

/*
 * vayu_call_audio=1: the modem's QMI VOICE client (bsd/dev/vayu_voice.c) owns the q6voice path
 * (vayu_voice=2|3 in vayu_audio.c).  It asks for it when a call goes into CONVERSATION and for the
 * teardown when the call is over.  Both calls only leave a word for vayu_audio.c's own thread: the
 * path's bring-up takes seconds and the QMI client's indication path must not wait for it (the END
 * indication is the one that has to take the path down).
 *
 * `path_mode' matches the vayu_voice=2 (a VoLTE call, "default volte voice") and =3 (a CS/CSFB
 * call, "default modem voice") steps; the audio side keeps the codec (vayu_jack>=2) and the DSP up
 * for vayu_call_audio_secs so that the request can be served whenever the call comes.
 */
void vayu_audio_call_audio_start(uint32_t path_mode);
void vayu_audio_call_audio_stop(void);
bool vayu_audio_call_audio_up(void);    /* the path is up (a failed bring-up is retried by the caller) */
#endif

#endif /* _SYS_VAYU_AUDIO_H_ */
