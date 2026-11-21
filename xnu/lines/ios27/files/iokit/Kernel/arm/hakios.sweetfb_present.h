/* SPDX-License-Identifier: MIT-0 */
/*
 * The framebuffer stand-in's present state machine for sweetfb_nowait=1 (docs/ios27/PRESENT.md, "The tick that does not
 * sleep"): which frame a vsync tick shows, without the tick ever sleeping for the GPU. Plain C over integers, no kernel: the
 * kernel's iokit/Kernel/arm/SweetFramebuffer.cpp (the xnu fork, STARLTE) calls it under its fLock, and
 * tools/sweetfb_present_test.c drives it on a host through every order of swaps, GPU completions, ticks and display off/on.
 *
 * A frame (one SwapEnd) is recorded with the number of the last GPU chain queued when it came: it is finished once the GPU
 * queue's "done" number (written by the queue's worker when it retires a chain, bsd/dev/starlte_gpu.c) has reached it.
 *
 *   recorded --(a newer swap before it is shown)--> replaced: released at once
 *   recorded --(a tick: done >= its seq)----------> shown: taken by that tick, presented, released
 *   recorded --(a tick: not done)-----------------> stays recorded (the tick only counts it; the vsync callback still goes out)
 *   recorded --(waited SFP_IDLE_TICKS ticks with the GPU queue empty, or SFP_MAX_TICKS ticks)--> taken by that tick for the
 *            old path: the tick sleeps for its chains as without sweetfb_nowait, then presents it (a "fallback", counted;
 *            with the GPU idle it is a logic error and logged once)
 *
 * At most one frame is recorded at a time, so a frame is released exactly once: by the swap that replaces it, or after the
 * tick that takes it. NOT run on a phone when written (18 Nov 2026).
 */
#ifndef HAKIOS_SWEETFB_PRESENT_H
#define HAKIOS_SWEETFB_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#define SFP_IDLE_TICKS  4       /* ticks a recorded frame may wait with nothing on the GPU before the self-check acts */
#define SFP_MAX_TICKS   30      /* ticks (half a second) a recorded frame may wait at all before the old path takes it */

enum sfp_action {
	SFP_NOTHING = 0,        /* nothing recorded: the tick only sends the vsync callback */
	SFP_WAIT,               /* a frame recorded, its chains not done: shown by a later tick */
	SFP_SHOW,               /* take the recorded frame: its chains are done */
	SFP_FALLBACK,           /* take the recorded frame and sleep for its chains (the old path) */
};

struct sfp {
	bool     recorded;      /* a frame is waiting to be shown (the framebuffer's fDirty) */
	uint32_t id;            /* its swap id (0: a redraw of a frame already shown and released) */
	uint64_t seq;           /* the last GPU chain queued when it came (0: none to wait for) */
	uint64_t last_seq;      /* the newest frame's seq, for a redraw */
	uint32_t waited;        /* ticks it has waited */
	/* counters (the caller prints and clears them) */
	uint32_t deferred;      /* ticks that left a frame for a later one */
	uint32_t replaced;      /* frames a newer swap replaced before they were shown */
	uint32_t shown;         /* frames ticks took as done */
	uint32_t fallbacks;     /* frames taken for the old path after SFP_MAX_TICKS (the GPU slow) */
	uint32_t idle_fallbacks;/* ... after SFP_IDLE_TICKS with the GPU queue empty: a logic error */
	bool     warned;        /* the logic error has been logged once */
};

/* A swap: the new frame is recorded. Returns the swap id of the frame it replaced, to be released (0: none). */
static inline uint32_t
sfp_swap(struct sfp *s, uint32_t id, uint64_t seq)
{
	uint32_t replaced = s->recorded ? s->id : 0;
	if (s->recorded) {
		s->replaced++;
	}
	s->recorded = true;
	s->id = id;
	s->seq = seq;
	s->last_seq = seq;
	s->waited = 0;
	return replaced;
}

/* The display back on: the newest frame drawn again (it was shown and released already: nothing to release for it). */
static inline void
sfp_redraw(struct sfp *s)
{
	if (s->recorded) {
		return;                 /* (a frame not yet shown is newer: it is the one to draw) */
	}
	s->recorded = true;
	s->id = 0;
	s->seq = s->last_seq;
	s->waited = 0;
}

static inline bool
sfp_done(uint64_t seq, uint64_t done)
{
	return seq == 0 || done >= seq;
}

/*
 * A tick that may show a frame. `done`: the GPU queue's done number now; `gpu_idle`: nothing queued or running. On SFP_SHOW
 * and SFP_FALLBACK the frame is no longer recorded: *id and *seq say which it was (the caller presents it, then releases *id
 * when not 0).
 */
static inline enum sfp_action
sfp_tick(struct sfp *s, uint64_t done, bool gpu_idle, uint32_t *id, uint64_t *seq)
{
	*id = 0;
	*seq = 0;
	if (!s->recorded) {
		return SFP_NOTHING;
	}
	enum sfp_action act;
	if (sfp_done(s->seq, done)) {
		act = SFP_SHOW;
		s->shown++;
	} else if (gpu_idle && s->waited >= SFP_IDLE_TICKS) {
		act = SFP_FALLBACK;
		s->idle_fallbacks++;
	} else if (s->waited >= SFP_MAX_TICKS) {
		act = SFP_FALLBACK;
		s->fallbacks++;
	} else {
		s->waited++;
		s->deferred++;
		return SFP_WAIT;
	}
	*id = s->id;
	*seq = s->seq;
	s->recorded = false;
	s->id = 0;
	s->seq = 0;
	s->waited = 0;
	return act;
}

#endif /* HAKIOS_SWEETFB_PRESENT_H */
