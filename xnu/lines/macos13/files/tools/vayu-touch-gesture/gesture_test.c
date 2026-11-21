/*
 * Tests of the touch-to-pointer gesture state machine in iokit/Kernel/VayuTouchGesture.h, the one
 * iokit/Kernel/VayuTouchPointer.cpp (boot-arg macos_hid=1) runs in the kernel.
 *
 * Every expected number below is worked out by hand from the header's rules (panel 1080x2400,
 * pixels = fraction * (size - 1) / 65535 rounded down, 16.16 = fraction * 65536 / 65535 rounded
 * down), not taken from a run of the code under test.
 *
 * What is NOT tested here: whether macOS's event system takes these reports as the gestures they
 * are meant to be (the phone run does that), and the IOHIDEvent bytes (Apple's IOHIDFamily code).
 *
 * tools/vayu-touch-gesture/run.sh compiles and runs this on the build machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "VayuTouchGesture.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); failures++; } } while (0)

static struct vtg_config
config(uint32_t rotation, bool relative)
{
	struct vtg_config c = { 1080, 2400, rotation, relative, 40, 500000, 300000, 24 };

	return c;
}

/* the reports of one call against the expected ones */
static void
expect(const char *what, const struct vtg_out *out, int n, const struct vtg_out *want, int wanted)
{
	CHECK(n == wanted, "%s: %d reports, want %d", what, n, wanted);
	for (int i = 0; i < n && i < wanted; i++) {
		CHECK(out[i].kind == want[i].kind && out[i].buttons == want[i].buttons && out[i].a == want[i].a &&
		    out[i].b == want[i].b, "%s: report %d is %d/%u %d,%d, want %d/%u %d,%d", what, i, out[i].kind,
		    out[i].buttons, out[i].a, out[i].b, want[i].kind, want[i].buttons, want[i].a, want[i].b);
	}
}

#define T(st, type, f, x, y, t, ...) do { struct vtg_out o_[VTG_OUT_MAX]; const struct vtg_out w_[] = { __VA_ARGS__ }; \
	int n_ = vtg_touch(st, type, f, x, y, t, o_); \
	expect(#type " " #f " " #x "," #y " at " #t, o_, n_, w_, (int)(sizeof(w_) / sizeof(w_[0])) - 1); } while (0)
#define TICK(st, t, ...) do { struct vtg_out o_[VTG_OUT_MAX]; const struct vtg_out w_[] = { __VA_ARGS__ }; \
	int n_ = vtg_tick(st, t, o_); \
	expect("tick at " #t, o_, n_, w_, (int)(sizeof(w_) / sizeof(w_[0])) - 1); } while (0)
#define END { 0, 0, 0, 0 }
#define ABS(b, x, y) { VTG_ABS, b, x, y }
#define REL(b, x, y) { VTG_REL, b, x, y }
#define SCROLL(x, y) { VTG_SCROLL, 0, x, y }

static void
test_place(void)
{
	struct vtg_config c;
	int32_t a, b;

	/* the panel's top-left, top-right and a middle point for every rotation */
	c = config(0, false);
	vtg_place(&c, 0, 0, &a, &b);            CHECK(a == 0 && b == 0, "rot 0 top-left -> %d,%d", a, b);
	vtg_place(&c, 65535, 0, &a, &b);        CHECK(a == 65536 && b == 0, "rot 0 top-right -> %d,%d", a, b);
	c = config(90, false);
	vtg_place(&c, 65535, 0, &a, &b);        CHECK(a == 0 && b == 0, "rot 90 panel top-right is the desktop's top-left -> %d,%d", a, b);
	vtg_place(&c, 0, 0, &a, &b);            CHECK(a == 0 && b == 65536, "rot 90 panel top-left -> %d,%d", a, b);
	vtg_place(&c, 65535, 65535, &a, &b);    CHECK(a == 65536 && b == 0, "rot 90 panel bottom-right -> %d,%d", a, b);
	vtg_place(&c, 32768, 32768, &a, &b);    CHECK(a == 32768 && b == 32767, "rot 90 middle -> %d,%d", a, b);
	c = config(180, false);
	vtg_place(&c, 0, 0, &a, &b);            CHECK(a == 65536 && b == 65536, "rot 180 top-left -> %d,%d", a, b);
	c = config(270, false);
	vtg_place(&c, 0, 0, &a, &b);            CHECK(a == 65536 && b == 0, "rot 270 panel top-left is the desktop's top-right -> %d,%d", a, b);
	vtg_place(&c, 0, 65535, &a, &b);        CHECK(a == 0 && b == 0, "rot 270 panel bottom-left -> %d,%d", a, b);

	/* a movement toward the panel's right edge is upward on a desktop turned 90 */
	c = config(90, false);
	vtg_turn(&c, 10, 0, &a, &b);            CHECK(a == 0 && b == -10, "rot 90 turn -> %d,%d", a, b);
	vtg_turn(&c, 0, 10, &a, &b);            CHECK(a == 10 && b == 0, "rot 90 turn y -> %d,%d", a, b);
	c = config(270, false);
	vtg_turn(&c, 10, 0, &a, &b);            CHECK(a == 0 && b == 10, "rot 270 turn -> %d,%d", a, b);
}

static void
test_tap_drag_long(void)
{
	struct vtg_state st;
	struct vtg_config c = config(90, false);

	vtg_init(&st, &c);
	/* tap in the middle: the pointer goes there, then button 1 down and up there */
	T(&st, VTG_DOWN, 0, 32768, 32768, 1000, ABS(0, 32768, 32767), END);
	CHECK(vtg_deadline(&st) == 501000, "deadline %llu", (unsigned long long)vtg_deadline(&st));
	T(&st, VTG_UP, 0, 32768, 32768, 80000, ABS(1, 32768, 32767), ABS(0, 32768, 32767), END);
	CHECK(st.state == VTG_IDLE && vtg_deadline(&st) == 0, "after a tap: state %d", st.state);

	/* a wobble within the slop (8 and 18 px) is still a tap, clicked where the finger landed */
	T(&st, VTG_DOWN, 0, 20000, 20000, 1000000, ABS(0, 20000, 45535), END);
	T(&st, VTG_MOVE, 0, 20500, 20500, 1010000, END);
	T(&st, VTG_UP, 0, 20500, 20500, 1100000, ABS(1, 20000, 45535), ABS(0, 20000, 45535), END);

	/* a drag: 66 px across the panel is past the slop; button 1 goes down where it landed */
	T(&st, VTG_DOWN, 0, 20000, 20000, 2000000, ABS(0, 20000, 45535), END);
	T(&st, VTG_MOVE, 0, 20500, 20500, 2010000, END);
	T(&st, VTG_MOVE, 0, 24000, 24000, 2020000, ABS(1, 20000, 45535), ABS(1, 24000, 41535), END);
	CHECK(st.state == VTG_DRAG && vtg_deadline(&st) == 0, "dragging: state %d", st.state);
	T(&st, VTG_MOVE, 0, 28000, 28000, 2030000, ABS(1, 28000, 37535), END);
	TICK(&st, 2600000, END);                /* no long press while dragging */
	T(&st, VTG_UP, 0, 28000, 28000, 2040000, ABS(0, 28000, 37535), END);
	CHECK(st.state == VTG_IDLE, "after a drag: state %d", st.state);

	/* held still: a secondary click at the deadline, then nothing until it lifts */
	T(&st, VTG_DOWN, 0, 40000, 40000, 3000000, ABS(0, 40000, 25535), END);
	TICK(&st, 3499999, END);
	TICK(&st, 3500000, ABS(2, 40000, 25535), ABS(0, 40000, 25535), END);
	CHECK(st.state == VTG_SPENT && vtg_deadline(&st) == 0, "after a long press: state %d", st.state);
	T(&st, VTG_MOVE, 0, 50000, 50000, 3600000, END);
	T(&st, VTG_UP, 0, 50000, 50000, 3700000, END);
	CHECK(st.state == VTG_IDLE, "after the long press lifted: state %d", st.state);
}

static void
test_two_fingers(void)
{
	struct vtg_state st;
	struct vtg_config c = config(90, false);

	vtg_init(&st, &c);
	/*
	 * Scroll. The first finger moves 66 px toward the panel's right edge twice (493 -> 559 -> 625):
	 * upward on the turned desktop, dv = -66 each time. -66 / 24 = -2 lines (18 px kept), then
	 * -84 / 24 = -3 (12 kept); natural scrolling gives them out as +2 and +3.
	 */
	T(&st, VTG_DOWN, 0, 30000, 30000, 1000, ABS(0, 30000, 35535), END);
	T(&st, VTG_DOWN, 1, 36000, 30000, 21000, END);
	CHECK(st.state == VTG_SCROLLING && vtg_deadline(&st) == 0, "two fingers: state %d", st.state);
	T(&st, VTG_MOVE, 1, 40000, 30000, 31000, END);  /* the second finger does not steer */
	T(&st, VTG_MOVE, 0, 34000, 30000, 51000, SCROLL(0, 2), END);
	T(&st, VTG_MOVE, 0, 38000, 30000, 81000, SCROLL(0, 3), END);
	T(&st, VTG_UP, 0, 38000, 30000, 111000, END);
	CHECK(st.primary == 1, "the finger left scrolls on: primary %d", st.primary);
	T(&st, VTG_UP, 1, 40000, 30000, 121000, END);  /* it scrolled: no click */
	CHECK(st.state == VTG_IDLE, "after a scroll: state %d", st.state);

	/* a two-finger tap: a secondary click where the first finger landed */
	T(&st, VTG_DOWN, 0, 50000, 50000, 1000000, ABS(0, 50000, 15535), END);
	T(&st, VTG_DOWN, 1, 52000, 50000, 1020000, END);
	T(&st, VTG_UP, 0, 50000, 50000, 1080000, END);
	T(&st, VTG_UP, 1, 52000, 50000, 1090000, ABS(2, 50000, 15535), ABS(0, 50000, 15535), END);

	/* two fingers held past long_us without scrolling: no click at all */
	T(&st, VTG_DOWN, 0, 50000, 50000, 2000000, ABS(0, 50000, 15535), END);
	T(&st, VTG_DOWN, 1, 52000, 50000, 2020000, END);
	TICK(&st, 2600000, END);
	T(&st, VTG_UP, 0, 50000, 50000, 2700000, END);
	T(&st, VTG_UP, 1, 52000, 50000, 2710000, END);

	/* scroll_px < 0 turns the direction round */
	c.scroll_px = -24;
	vtg_init(&st, &c);
	T(&st, VTG_DOWN, 0, 30000, 30000, 1000, ABS(0, 30000, 35535), END);
	T(&st, VTG_DOWN, 1, 36000, 30000, 21000, END);
	T(&st, VTG_MOVE, 0, 34000, 30000, 51000, SCROLL(0, -2), END);

	/* upright (rotation 0): a finger going up the panel (-146 px) is 6 lines of vertical scroll */
	c = config(0, false);
	vtg_init(&st, &c);
	T(&st, VTG_DOWN, 0, 30000, 34000, 1000, ABS(0, 30000, 34000), END);
	T(&st, VTG_DOWN, 1, 36000, 34000, 21000, END);
	T(&st, VTG_MOVE, 0, 30000, 30000, 51000, SCROLL(0, 6), END);
}

static void
test_trackpad(void)
{
	struct vtg_state st;
	struct vtg_config c = config(90, true);

	vtg_init(&st, &c);
	/* a finger landing says nothing; moving 66 px across the panel is 66 px up the desktop */
	T(&st, VTG_DOWN, 0, 20000, 20000, 1000, END);
	T(&st, VTG_MOVE, 0, 24000, 20000, 11000, REL(0, 0, -66), END);
	T(&st, VTG_MOVE, 0, 24000, 22000, 21000, REL(0, 73, 0), END);  /* 805 - 732 px down the panel */
	T(&st, VTG_MOVE, 0, 24000, 22000, 22000, END);                  /* no movement, no report */
	T(&st, VTG_UP, 0, 24000, 22000, 31000, END);

	/* a tap clicks where the pointer is; a touch 200 ms later that moves drags */
	T(&st, VTG_DOWN, 0, 30000, 30000, 1000000, END);
	T(&st, VTG_UP, 0, 30000, 30000, 1080000, REL(1, 0, 0), REL(0, 0, 0), END);
	T(&st, VTG_DOWN, 0, 30000, 30000, 1280000, END);
	CHECK(st.armed, "a touch 200 ms after a tap is armed");
	T(&st, VTG_MOVE, 0, 34000, 30000, 1300000, REL(1, 0, 0), REL(1, 0, -66), END);
	T(&st, VTG_MOVE, 0, 38000, 30000, 1310000, REL(1, 0, -66), END);
	T(&st, VTG_UP, 0, 38000, 30000, 1320000, REL(0, 0, 0), END);

	/* ... but not 400 ms later */
	T(&st, VTG_DOWN, 0, 30000, 30000, 2000000, END);
	T(&st, VTG_UP, 0, 30000, 30000, 2080000, REL(1, 0, 0), REL(0, 0, 0), END);
	T(&st, VTG_DOWN, 0, 30000, 30000, 2480000, END);
	CHECK(!st.armed, "a touch 400 ms after a tap is not armed");
	T(&st, VTG_MOVE, 0, 34000, 30000, 2500000, REL(0, 0, -66), END);
	T(&st, VTG_UP, 0, 34000, 30000, 2510000, END);

	/* a long press is a secondary click here too */
	T(&st, VTG_DOWN, 0, 30000, 30000, 3000000, END);
	TICK(&st, 3500000, REL(2, 0, 0), REL(0, 0, 0), END);
	T(&st, VTG_UP, 0, 30000, 30000, 3600000, END);
}

static void
test_odd_input(void)
{
	struct vtg_state st;
	struct vtg_config c = config(0, false);

	vtg_init(&st, &c);
	T(&st, VTG_MOVE, 3, 100, 100, 1000, END);       /* a finger never seen to land */
	T(&st, VTG_UP, 3, 100, 100, 2000, END);
	T(&st, VTG_DOWN, 10, 100, 100, 3000, END);      /* out of range */
	CHECK(st.state == VTG_IDLE && st.down_mask == 0, "odd input changed the state: %d", st.state);
	/* a second landing of the same finger is a movement, and x/y over 65535 are the edge */
	T(&st, VTG_DOWN, 0, 70000, 0, 4000, ABS(0, 65536, 0), END);
	T(&st, VTG_DOWN, 0, 70000, 0, 5000, END);
	CHECK(st.state == VTG_PENDING && st.down_mask == 1, "repeated landing: state %d mask %x", st.state, st.down_mask);
	T(&st, VTG_UP, 0, 70000, 0, 6000, ABS(1, 65536, 0), ABS(0, 65536, 0), END);
}

int
main(void)
{
	test_place();
	test_tap_drag_long();
	test_two_fingers();
	test_trackpad();
	test_odd_input();
	if (failures != 0) {
		printf("vayu-touch-gesture: %d FAILED\n", failures);
		return 1;
	}
	printf("vayu-touch-gesture: all passed\n");
	return 0;
}
