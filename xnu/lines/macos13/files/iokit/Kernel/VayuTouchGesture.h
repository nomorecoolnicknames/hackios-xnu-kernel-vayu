/*
 * The touch screen as a pointer for macOS: the gesture state machine of iokit/Kernel/VayuTouchPointer.cpp.
 *
 * Plain C with no kernel calls, so that the host test (tools/tests/vayu_touch_gesture/) runs the same
 * code. Input is what the touch drivers post to bsd/dev/hakios.input.c: down/move/up of finger N at
 * x, y in 0..65535 of the panel held upright (vayu: 1080x2400), with a time in microseconds. Output is
 * pointer reports in the desktop's orientation:
 *
 *   VTG_ABS     a, b = the pointer's place, 16.16 fractions of the desktop (0..0x10000); buttons
 *   VTG_REL     a, b = movement in panel pixels turned with the desktop; buttons
 *   VTG_SCROLL  a, b = horizontal, vertical lines (as IOHIDEvent::scrollEvent takes them)
 *
 * Absolute mode (the default; macOS-uke's UkeTouchPointer, an absolute tablet):
 *   one finger lands      the pointer goes there, no button
 *   ... and lifts         a click there (button 1 down, up), if it moved less than slop_px and
 *                         was lifted before long_us
 *   ... and moves         past slop_px: button 1 goes down where it landed and follows the finger
 *                         (a drag) until it lifts
 *   ... and stays         long_us without moving past slop_px: a secondary click (button 2) there;
 *                         the rest of the touch is ignored
 *   a second finger       while the first has not decided yet: two-finger scroll, by the first
 *                         finger's movement, one line per scroll_px panel pixels, so that the
 *                         content follows the fingers with macOS's natural scrolling
 *                         (scroll_px < 0 turns it round); lifted without scrolling and before
 *                         long_us: a secondary click
 * Trackpad mode (relative): the same, but a finger moves the pointer by its movement instead of
 * placing it; a tap clicks where the pointer is; a touch within double_us of a tap that then moves
 * drags (button 1 held), as a Mac trackpad's tap-to-drag.
 *
 * rotation is the angle, clockwise, by which the desktop is turned on the upright panel: 90 means
 * the desktop's top edge runs along the panel's right edge (the phone held with that edge up).
 */
#ifndef _VAYU_TOUCH_GESTURE_H_
#define _VAYU_TOUCH_GESTURE_H_

#include <stdint.h>
#include <stdbool.h>

#define VTG_FINGERS     10
#define VTG_OUT_MAX     4

enum { VTG_ABS = 1, VTG_REL = 2, VTG_SCROLL = 3 };
enum { VTG_DOWN = 1, VTG_MOVE = 2, VTG_UP = 3 };       /* = HAKIOS_TOUCH_* */
enum { VTG_IDLE, VTG_PENDING, VTG_DRAG, VTG_MOVING, VTG_SCROLLING, VTG_SPENT };

struct vtg_config {
	uint32_t panel_w, panel_h;      /* panel pixels, upright */
	uint32_t rotation;              /* 0, 90, 180, 270 */
	bool relative;                  /* trackpad mode */
	int32_t slop_px;                /* travel below this is a finger held still */
	uint64_t long_us;               /* a still finger this long is a secondary click */
	uint64_t double_us;             /* trackpad: a touch this soon after a tap drags */
	int32_t scroll_px;              /* panel pixels per scroll line; < 0 reverses */
};

struct vtg_out {
	uint8_t kind;                   /* VTG_ABS, VTG_REL, VTG_SCROLL */
	uint8_t buttons;                /* bit 0 primary, bit 1 secondary */
	int32_t a, b;
};

struct vtg_state {
	struct vtg_config cfg;
	int state;
	uint32_t down_mask;
	int primary;                    /* the finger that steers, -1 none */
	uint32_t x[VTG_FINGERS], y[VTG_FINGERS];        /* each finger's last place, 0..65535 */
	uint32_t down_x, down_y;        /* where the primary finger landed */
	uint64_t down_us, tap_end_us;
	int32_t travel;                 /* the primary finger's largest distance from its landing, px */
	int32_t acc_u, acc_v;           /* scroll movement not yet given out as lines, px */
	bool scrolled;                  /* a line was given out in this touch */
	bool armed;                     /* trackpad: this touch began just after a tap */
	uint8_t buttons;
};

static inline void
vtg_init(struct vtg_state *st, const struct vtg_config *cfg)
{
	struct vtg_state zero = { 0 };

	*st = zero;
	st->cfg = *cfg;
	if (st->cfg.panel_w < 2) {
		st->cfg.panel_w = 2;
	}
	if (st->cfg.panel_h < 2) {
		st->cfg.panel_h = 2;
	}
	if (st->cfg.scroll_px == 0) {
		st->cfg.scroll_px = 24;
	}
	st->cfg.rotation %= 360;
	st->primary = -1;
}

/* 0..65535 of the panel -> 16.16 of the desktop */
static inline void
vtg_place(const struct vtg_config *cfg, uint32_t x, uint32_t y, int32_t *a, int32_t *b)
{
	uint32_t u, v;

	switch (cfg->rotation) {
	case 90:  u = y; v = 65535 - x; break;
	case 180: u = 65535 - x; v = 65535 - y; break;
	case 270: u = 65535 - y; v = x; break;
	default:  u = x; v = y; break;
	}
	*a = (int32_t)(((uint64_t)u << 16) / 65535);
	*b = (int32_t)(((uint64_t)v << 16) / 65535);
}

static inline int32_t
vtg_px(uint32_t fraction, uint32_t pixels)
{
	return (int32_t)((uint64_t)fraction * (pixels - 1) / 65535);
}

/* a movement on the panel, in panel pixels, turned into the desktop's directions */
static inline void
vtg_turn(const struct vtg_config *cfg, int32_t dx, int32_t dy, int32_t *du, int32_t *dv)
{
	switch (cfg->rotation) {
	case 90:  *du = dy; *dv = -dx; break;
	case 180: *du = -dx; *dv = -dy; break;
	case 270: *du = -dy; *dv = dx; break;
	default:  *du = dx; *dv = dy; break;
	}
}

static inline void
vtg_delta(const struct vtg_state *st, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, int32_t *du, int32_t *dv)
{
	int32_t dx = vtg_px(x1, st->cfg.panel_w) - vtg_px(x0, st->cfg.panel_w);
	int32_t dy = vtg_px(y1, st->cfg.panel_h) - vtg_px(y0, st->cfg.panel_h);

	vtg_turn(&st->cfg, dx, dy, du, dv);
}

static inline int
vtg_emit(struct vtg_out *out, int n, uint8_t kind, uint8_t buttons, int32_t a, int32_t b)
{
	if (n < VTG_OUT_MAX) {
		out[n].kind = kind;
		out[n].buttons = buttons;
		out[n].a = a;
		out[n].b = b;
		n++;
	}
	return n;
}

/* the pointer at the place of x, y (absolute) or not moved (trackpad) with these buttons */
static inline int
vtg_at(struct vtg_state *st, struct vtg_out *out, int n, uint32_t x, uint32_t y, uint8_t buttons)
{
	int32_t a, b;

	st->buttons = buttons;
	if (st->cfg.relative) {
		return vtg_emit(out, n, VTG_REL, buttons, 0, 0);
	}
	vtg_place(&st->cfg, x, y, &a, &b);
	return vtg_emit(out, n, VTG_ABS, buttons, a, b);
}

/* button `which` pressed and let go where the primary finger landed */
static inline int
vtg_click(struct vtg_state *st, struct vtg_out *out, int n, uint8_t which)
{
	n = vtg_at(st, out, n, st->down_x, st->down_y, which);
	return vtg_at(st, out, n, st->down_x, st->down_y, 0);
}

static inline int
vtg_lowest(uint32_t mask)
{
	for (int i = 0; i < VTG_FINGERS; i++) {
		if (mask & (1u << i)) {
			return i;
		}
	}
	return -1;
}

/* One sample (type VTG_DOWN/MOVE/UP); returns how many reports were written to out. */
static inline int
vtg_touch(struct vtg_state *st, uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t now_us,
    struct vtg_out *out)
{
	int n = 0;
	uint32_t bit;
	uint32_t ox, oy;
	int32_t du, dv;

	if (finger >= VTG_FINGERS) {
		return 0;
	}
	bit = 1u << finger;
	x = x > 65535 ? 65535 : x;
	y = y > 65535 ? 65535 : y;
	if (type == VTG_DOWN && (st->down_mask & bit)) {
		type = VTG_MOVE;                /* a repeated landing is a movement */
	}
	if (type != VTG_DOWN && !(st->down_mask & bit)) {
		return 0;                       /* nothing known about this finger */
	}
	ox = st->x[finger];
	oy = st->y[finger];
	st->x[finger] = x;
	st->y[finger] = y;

	if (type == VTG_DOWN) {
		st->down_mask |= bit;
		switch (st->state) {
		case VTG_IDLE:
			st->primary = (int)finger;
			st->down_x = x;
			st->down_y = y;
			st->down_us = now_us;
			st->travel = 0;
			st->acc_u = st->acc_v = 0;
			st->scrolled = false;
			st->armed = st->cfg.relative && st->tap_end_us != 0 && now_us - st->tap_end_us <= st->cfg.double_us;
			st->tap_end_us = 0;
			st->state = VTG_PENDING;
			if (!st->cfg.relative) {
				n = vtg_at(st, out, n, x, y, 0);
			}
			break;
		case VTG_PENDING:
			st->state = VTG_SCROLLING;
			st->acc_u = st->acc_v = 0;
			break;
		default:
			break;                  /* a drag, a scroll or a spent touch goes on as it is */
		}
		return n;
	}

	if (type == VTG_MOVE) {
		if ((int)finger != st->primary) {
			return 0;               /* only the primary finger steers and scrolls */
		}
		switch (st->state) {
		case VTG_PENDING: {
			int32_t tx = vtg_px(x, st->cfg.panel_w) - vtg_px(st->down_x, st->cfg.panel_w);
			int32_t ty = vtg_px(y, st->cfg.panel_h) - vtg_px(st->down_y, st->cfg.panel_h);
			int32_t t = (tx < 0 ? -tx : tx) > (ty < 0 ? -ty : ty) ? (tx < 0 ? -tx : tx) : (ty < 0 ? -ty : ty);

			if (t > st->travel) {
				st->travel = t;
			}
			if (st->travel <= st->cfg.slop_px) {
				break;          /* still a finger held still: the pointer stays put */
			}
			if (!st->cfg.relative) {
				st->state = VTG_DRAG;
				n = vtg_at(st, out, n, st->down_x, st->down_y, 1);
				n = vtg_at(st, out, n, x, y, 1);
				break;
			}
			vtg_delta(st, st->down_x, st->down_y, x, y, &du, &dv);
			if (st->armed) {
				st->state = VTG_DRAG;
				st->buttons = 1;
				n = vtg_emit(out, n, VTG_REL, 1, 0, 0);
				n = vtg_emit(out, n, VTG_REL, 1, du, dv);
			} else {
				st->state = VTG_MOVING;
				n = vtg_emit(out, n, VTG_REL, 0, du, dv);
			}
			break;
		}
		case VTG_DRAG:
		case VTG_MOVING:
			if (!st->cfg.relative) {
				n = vtg_at(st, out, n, x, y, st->buttons);
			} else {
				vtg_delta(st, ox, oy, x, y, &du, &dv);
				if (du != 0 || dv != 0) {
					n = vtg_emit(out, n, VTG_REL, st->buttons, du, dv);
				}
			}
			break;
		case VTG_SCROLLING: {
			int32_t step = st->cfg.scroll_px < 0 ? -st->cfg.scroll_px : st->cfg.scroll_px;
			int32_t lu, lv;

			vtg_delta(st, ox, oy, x, y, &du, &dv);
			st->acc_u += du;
			st->acc_v += dv;
			st->travel += (du < 0 ? -du : du) + (dv < 0 ? -dv : dv);
			lu = st->acc_u / step;
			lv = st->acc_v / step;
			if (lu != 0 || lv != 0) {
				st->acc_u -= lu * step;
				st->acc_v -= lv * step;
				st->scrolled = true;
				/* natural scrolling: a finger going up shows what is further down */
				if (st->cfg.scroll_px > 0) {
					lu = -lu;
					lv = -lv;
				}
				n = vtg_emit(out, n, VTG_SCROLL, 0, lu, lv);
			}
			break;
		}
		default:
			break;
		}
		return n;
	}

	/* VTG_UP */
	st->down_mask &= ~bit;
	switch (st->state) {
	case VTG_PENDING:
		/* the only finger, lifted before it moved or was held long: a click */
		n = vtg_click(st, out, n, 1);
		if (st->cfg.relative && !st->armed) {
			st->tap_end_us = now_us;
		}
		st->state = VTG_IDLE;
		st->primary = -1;
		return n;
	case VTG_DRAG:
	case VTG_MOVING:
		if ((int)finger == st->primary) {
			if (st->buttons != 0) {
				n = vtg_at(st, out, n, x, y, 0);
			}
			st->state = st->down_mask ? VTG_SPENT : VTG_IDLE;
			st->primary = -1;
		}
		break;
	case VTG_SCROLLING:
		if (st->down_mask == 0) {
			if (!st->scrolled && st->travel <= st->cfg.slop_px && now_us - st->down_us < st->cfg.long_us) {
				n = vtg_click(st, out, n, 2);   /* a two-finger tap */
			}
			st->state = VTG_IDLE;
			st->primary = -1;
		} else if ((int)finger == st->primary) {
			st->primary = vtg_lowest(st->down_mask);    /* the finger left on the glass scrolls on */
		}
		break;
	case VTG_SPENT:
		if (st->down_mask == 0) {
			st->state = VTG_IDLE;
			st->primary = -1;
		}
		break;
	default:
		break;
	}
	return n;
}

/* When vtg_tick() has something to do (microseconds, as now_us), or 0. */
static inline uint64_t
vtg_deadline(const struct vtg_state *st)
{
	return st->state == VTG_PENDING ? st->down_us + st->cfg.long_us : 0;
}

/* The passing of time: a finger held still long enough is a secondary click. */
static inline int
vtg_tick(struct vtg_state *st, uint64_t now_us, struct vtg_out *out)
{
	int n = 0;

	if (st->state == VTG_PENDING && now_us >= st->down_us + st->cfg.long_us) {
		n = vtg_click(st, out, n, 2);
		st->state = VTG_SPENT;
	}
	return n;
}

#endif /* _VAYU_TOUCH_GESTURE_H_ */
