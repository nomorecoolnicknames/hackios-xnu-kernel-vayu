/*
 * FastRPC for the SLPI's sensors PD of the POCO X3 Pro (vayu, SM8150-AC):
 * bsd/dev/vayu_fastrpc.c.
 *
 * The SLPI opens the GLINK channel "fastrpcglink-apps-dsp" (the DT's dsps edge, qcom,intents =
 * <0x64 0x40>: 64 intents of 100 bytes) and expects the application processors to be the
 * FastRPC "AP side": answer as the sensors protection domain, run the reverse listener
 * (adsp_listener handle 3), and serve apps_std from fw/sensors.pak. Only when boot-arg
 * vayu_frpc >= 1; vayu_slpi.c opens the channel and calls vayu_fastrpc_start().
 */
#ifndef _SYS_VAYU_FASTRPC_H_
#define _SYS_VAYU_FASTRPC_H_

#include <stdint.h>

struct vayu_glink;
struct vayu_glink_channel;

/* The channel's input, called on the GLINK edge's thread: copies the answer and wakes the run
 * thread; it never blocks and never sleeps. */
void vayu_fastrpc_input(void *context, const uint8_t *data, uint32_t length);

/* Claim the SMMU's bypass for the sensors' stream IDs and start the FastRPC thread (returns at
 * once). level is vayu_slpi's level, for the log only. */
void vayu_fastrpc_start(struct vayu_glink *g, struct vayu_glink_channel *channel, uint32_t level);

/* Ends the listener loop and lets the next2 invoke return: no locks, no sleeping (a panic's way
 * back to the loader calls this). */
void vayu_fastrpc_quiesce(void);

/* After the SLPI has been stopped: the SMMU entry goes back. A panic leaves it in place -- the
 * DSP is stopped then, so the bypass is harmless. */
void vayu_fastrpc_release(void);

#endif /* _SYS_VAYU_FASTRPC_H_ */
