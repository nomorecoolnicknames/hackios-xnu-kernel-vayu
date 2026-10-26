/*
 * The 3.5 mm jack of the POCO X3 Pro (vayu, SM8150-AC): the WCD934x codec behind the audio
 * DSP's SLIMbus, and the first look at it from here (bsd/dev/vayu_jack.c). Nothing runs unless
 * boot-arg vayu_jack=N asks for it, and only after vayu_audio's step 3 has the DSP and QRTR up.
 */
#ifndef _SYS_VAYU_JACK_H_
#define _SYS_VAYU_JACK_H_

#ifdef KERNEL
#include <stdbool.h>

struct vayu_qrtr;

/* vayu_jack=1: the SLIMbus NGD up, the codec's chip ID read; true when every step was done.
 * vayu_jack=2: the same, then MCLK and the codec's init; everything is left up for the headphone path.
 * vayu_jack=3: the same, then the MBHC armed and its poll thread started (the codec stays up). */
bool vayu_jack_run(struct vayu_qrtr *q);
/* vayu_jack=2 and its init done: vayu_audio's step 4 plays to the headphones (AFE SLIMBUS_6_RX) instead of MI2S. */
bool vayu_jack_ready(void);
/* vayu_jack=3 and its init done: the MBHC polls the jack, so the codec must stay up for the whole run. */
bool vayu_jack_keep(void);
/* The jack as the MBHC sees it: -1 inactive, 0 none, 1 headphones (kern.vayu_jack). */
int vayu_jack_state(void);
/* The route vayu_audio actually took: 0 speaker, 1 headphones (kern.vayu_audio_route). */
void vayu_jack_set_route(int route);
/* After the AFE port 0x400C is started: codec ports 18/19 connected, channels 146/147 active, the HPH path powered. */
bool vayu_jack_path_on(void);
/* Before the AFE port is stopped: the HPH path down, the channels removed (idempotent). */
void vayu_jack_path_off(void);
/* At the end of vayu_audio's run, before the DSP stops: path off, MCLK off, the codec quiet, QMI/QRTR given back. */
void vayu_jack_finish(void);
/* After the AFE port 0x4001 (SLIMBUS_0_TX) is started: the handset microphones (AMIC3 -> DEC7 -> SLIM TX7, AMIC1 -> DEC8
 * -> SLIM TX8, mic biases 3 and 1) powered, codec ports 7/8 connected, channels 135/136 active. Needs vayu_jack_ready(). */
bool vayu_jack_mic_on(void);
/* Before the AFE port 0x4001 is stopped: the microphones' path down, channels 135/136 removed (idempotent). */
void vayu_jack_mic_off(void);
#endif

#endif /* _SYS_VAYU_JACK_H_ */
