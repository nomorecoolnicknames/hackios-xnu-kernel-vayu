/*
 * The MDP display interfaces of the POCO X3 Pro (vayu, SM8150-AC), bsd/dev/vayu_vsync.c. This
 * is step 1 of the display refresh work: a look, reading only. Nothing here writes to the
 * display controller, and no write helper exists in this file at all; the two INTF blocks are
 * read with 32-bit loads and that is the whole of it.
 *
 * Why: the framebuffer's "vsync" today is a timer at boot-arg fb_hz (MiniIOMobileFramebuffer.cpp
 * :157). The panel really runs from the display controller's timing engine. This step measures
 * that engine so step 2 can tick vsync from it.
 *
 * The facts, from the phone's own sources:
 *
 *   the block   vendor arch/arm64/boot/dts/qcom/sm8150-sde.dtsi:17, reg = <0x0ae00000 0x84208>
 *               ("mdp_phys"). The INTF offsets are :89-92, qcom,sde-intf-off =
 *               <0x6b000 0x6b800 0x6c000 0x6c800>, size 0x2b8, types "dp", "dsi", "dsi", "dp". So
 *               INTF 1 (DSI 0) is at 0x0ae6b800 and INTF 2 (DSI 1) is at 0x0ae6c000. Mainline says
 *               the same: drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_5_0_sm8150.h:299-309, intf_1
 *               base 0x6a800 and intf_2 base 0x6b000 relative to the mdp node at 0x0ae01000. One
 *               mapping, ml_io_map(0x0ae6b000, 0x2000), covers both: INTF 1 at offset 0x800,
 *               INTF 2 at offset 0x1000.
 *   the offsets drivers/gpu/drm/msm/disp/dpu1/dpu_hw_intf.c:17-83: 0x000 TIMING_ENGINE_EN,
 *               0x004 CONFIG, 0x008 HSYNC_CTL (bits 31:16 pulse width, 15:0 hsync period in
 *               pixels), 0x00c VSYNC_PERIOD_F0 (vsync period in pixel clocks = vtotal x htotal),
 *               0x014 VSYNC_PULSE_WIDTH_F0, 0x01c DISPLAY_V_START_F0, 0x024 DISPLAY_V_END_F0,
 *               0x02c ACTIVE_V_START_F0, 0x034 ACTIVE_V_END_F0, 0x03c DISPLAY_HCTL,
 *               0x040 ACTIVE_HCTL, 0x060 CONFIG2, 0x090 PANEL_FORMAT, 0x0a8 FRAME_LINE_COUNT_EN,
 *               0x0ac FRAME_COUNT, 0x0b0 LINE_COUNT, 0x25c MUX, 0x26c STATUS.
 *   the encoding dpu_hw_intf.c ~l.128-140 and :227-228: hsync_period = pulse + back porch + width
 *               + front porch; VSYNC_PERIOD_F0 = vtotal * hsync_period. Mainline sets
 *               FRAME_LINE_COUNT_EN = 0x3 when it programs timing (:241); if it reads 0 here the
 *               counters are not running, and this step only says so. Step 2 may enable it, but
 *               not here.
 *   the panel   vendor dsi-panel-j20s-42-02-0b-lcd-dsc-vid.dtsi:23,65-75: "dsi_video_mode",
 *               120 Hz, 1080x2400, h pulse 12, h back 40, h front 67, v pulse 2, v back 30,
 *               v front 33. With DSC the INTF's horizontal numbers are in compressed pixels, so
 *               htotal is not checked against 1080+119; only vtotal is: 2400 + 2 + 30 + 33 =
 *               2465 lines.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   vayu_vsync=1    a look, reading only: both INTF blocks, their timing registers, a count of
 *                   frames over one second if the counters are running, and eight line counts
 *                   1 ms apart.
 *
 * Values above 1 are reserved for later steps: for now they say so and do step 1. The probe runs
 * in a kernel thread, so a slow read never holds up boot.
 *
 * Step 2 (not here): tick the framebuffer's vsync from FRAME_COUNT changes polled at 1 ms, or
 * from the MDP vsync interrupt; enabling FRAME_LINE_COUNT_EN = 0x3 only if step 1 shows it off.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_vsync.h>
#include <os/atomic_private.h>

/*
 * Every line of this driver also goes into the loader's log (as the torch's and the audio's say() do):
 * a long run's live log is the loader's log and a ring of the kernel's, and the ring fills with what
 * user space logs -- runs 005616 and 022501 kept no printf-only line of this driver at all, while the
 * torch's lines were there. (printf in parentheses below is the kernel's own.)
 */
extern void loader_log_putc(char c);
static int tee_printf(const char *format, ...) __printflike(1, 2);

static int
tee_printf(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		(printf)("%s", line);
	}
	return length;
}
#define printf(...) tee_printf(__VA_ARGS__)

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void IOSleep(unsigned int milliseconds);
extern void delay(int usec);

/* One mapping for both interfaces: sm8150-sde.dtsi:89-92 puts them 0x800 apart past 0x0ae6b000. */
#define INTF_PHYS               0x0ae6b000UL    /* mdp 0x0ae00000 + 0x6b000 (sm8150-sde.dtsi:17,89-92) */
#define INTF_MAP_SIZE           0x2000UL
#define INTF_1_OFFSET           0x800UL         /* 0x0ae6b800: INTF 1, DSI 0 */
#define INTF_2_OFFSET           0x1000UL        /* 0x0ae6c000: INTF 2, DSI 1 */
#define INTF_COUNT              2

/* Inside an INTF, dpu_hw_intf.c:17-83. */
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_CONFIG             0x004
#define INTF_HSYNC_CTL          0x008           /* [31:16] period, [15:0] pulse width, in pixels (dpu_hw_intf.c:167) */
#define INTF_VSYNC_PERIOD_F0    0x00c           /* vtotal x htotal, in pixel clocks (l.227-228) */
#define INTF_VSYNC_PULSE_WIDTH_F0 0x014
#define INTF_DISPLAY_V_START_F0 0x01c           /* this and the next three: pixel clocks from the frame's start (l.128-140) */
#define INTF_DISPLAY_V_END_F0   0x024
#define INTF_ACTIVE_V_START_F0  0x02c
#define INTF_ACTIVE_V_END_F0    0x034
#define INTF_DISPLAY_HCTL       0x03c
#define INTF_ACTIVE_HCTL        0x040
#define INTF_CONFIG2            0x060
#define INTF_PANEL_FORMAT       0x090
#define INTF_FRAME_LINE_COUNT_EN 0x0a8          /* [1] line count, [0] frame count (l.241) */
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0
#define INTF_MUX                0x25c
#define INTF_STATUS             0x26c

#define INTF_TIMING_ENGINE_ON   (1u << 0)       /* TIMING_ENGINE_EN bit 0 */
#define INTF_FRAME_COUNT_EN     (1u << 0)       /* FRAME_LINE_COUNT_EN bit 0, mainline's 0x3 (l.241) */

#define PANEL_VTOTAL            2465u           /* 2400 + 2 + 30 + 33: dsi-panel-j20s-42-02-0b-lcd-dsc-vid.dtsi:65-75 */

/* (the only kind of access this file makes: a 32-bit read; there is no write helper here) */
static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

/*
 * Count frames for a second and lines for eight milliseconds. Only called when the timing engine
 * is on and FRAME_COUNT_EN is set; if it is not, the counters would not move and there would be
 * nothing to count.
 */
static void
measure(unsigned number, vm_offset_t base, uint32_t vtotal)
{
	uint32_t frame0, frame1, frames, line;
	uint64_t start, end, elapsed_ns, elapsed_us, hundredths, lines_per_second;

	frame0 = rd(base + INTF_FRAME_COUNT);
	start = mach_absolute_time();
	IOSleep(1000);
	frame1 = rd(base + INTF_FRAME_COUNT);
	end = mach_absolute_time();

	frames = frame1 - frame0;               /* uint32 wrap is fine */
	absolutetime_to_nanoseconds(end - start, &elapsed_ns);
	elapsed_us = elapsed_ns / 1000;
	if (elapsed_us == 0) {
		elapsed_us = 1;                 /* (a clock this coarse would be news, but do not divide by 0) */
	}
	hundredths = (uint64_t)frames * 100000000ULL / elapsed_us;
	printf("vayu-vsync: INTF%u: %u frames in %llu us = %u.%02u Hz\n", number, frames,
	    (unsigned long long)elapsed_us, (unsigned)(hundredths / 100), (unsigned)(hundredths % 100));

	printf("vayu-vsync: INTF%u lines:", number);
	for (unsigned i = 0; i < 8; i++) {
		if (i != 0) {
			delay(1000);            /* 1 ms between reads */
		}
		line = rd(base + INTF_LINE_COUNT);
		printf(" %u", line);
	}
	printf("\n");

	/* the line rate in lines/s: frames x vtotal lines over the elapsed seconds */
	lines_per_second = (uint64_t)frames * vtotal * 1000000ULL / elapsed_us;
	printf("vayu-vsync: INTF%u line rate %llu lines/s (frames %u x vtotal %u over %llu us)\n",
	    number, (unsigned long long)lines_per_second, frames, vtotal, (unsigned long long)elapsed_us);
}

/* FRAME_COUNT changes within 50 ms (polled every 1 ms) */
static bool
frame_moves(vm_offset_t base)
{
	uint32_t first = rd(base + INTF_FRAME_COUNT);

	for (unsigned ms = 0; ms < 50; ms++) {
		delay(1000);
		if (rd(base + INTF_FRAME_COUNT) != first) {
			return true;
		}
	}
	return false;
}

static void
show_intf(unsigned number, vm_offset_t base)
{
	uint32_t engine_en, config, mux, status;
	uint32_t hsync_ctl, hsync_period, hsync_pulse;
	uint32_t vsync_period, vsync_pulse;
	uint32_t display_v_start, display_v_end, active_v_start, active_v_end;
	uint32_t display_hctl, active_hctl, panel_format, config2;
	uint32_t counters_en, frame, line, vtotal = 0;
	bool engine_on, counters_on;

	engine_en = rd(base + INTF_TIMING_ENGINE_EN);
	config = rd(base + INTF_CONFIG);
	mux = rd(base + INTF_MUX);
	status = rd(base + INTF_STATUS);
	engine_on = (engine_en & INTF_TIMING_ENGINE_ON) != 0;
	printf("vayu-vsync: INTF%u at %#lx: engine %s (TIMING_ENGINE_EN %#x), config %#x, mux %#x, status %#x\n",
	    number, base, engine_on ? "on" : "off", engine_en, config, mux, status);

	hsync_ctl = rd(base + INTF_HSYNC_CTL);
	hsync_period = hsync_ctl >> 16;                 /* hsync_ctl = period << 16 | pulse (dpu_hw_intf.c:167) */
	hsync_pulse = hsync_ctl & 0xffffu;
	vsync_period = rd(base + INTF_VSYNC_PERIOD_F0);
	vsync_pulse = rd(base + INTF_VSYNC_PULSE_WIDTH_F0);
	display_v_start = rd(base + INTF_DISPLAY_V_START_F0);
	display_v_end = rd(base + INTF_DISPLAY_V_END_F0);
	active_v_start = rd(base + INTF_ACTIVE_V_START_F0);
	active_v_end = rd(base + INTF_ACTIVE_V_END_F0);
	display_hctl = rd(base + INTF_DISPLAY_HCTL);
	active_hctl = rd(base + INTF_ACTIVE_HCTL);
	panel_format = rd(base + INTF_PANEL_FORMAT);
	config2 = rd(base + INTF_CONFIG2);
	printf("vayu-vsync: INTF%u hsync_ctl %#x (period %u, pulse %u) vsync_period %u pulse %u "
	    "display_v %u..%u active_v %u..%u display_hctl %#x active_hctl %#x panel_format %#x config2 %#x\n",
	    number, hsync_ctl, hsync_period, hsync_pulse, vsync_period, vsync_pulse,
	    display_v_start, display_v_end, active_v_start, active_v_end,
	    display_hctl, active_hctl, panel_format, config2);

	if (hsync_period != 0) {
		vtotal = vsync_period / hsync_period;
		printf("vayu-vsync: INTF%u vtotal %u lines (the panel's 120 Hz mode has 2465: %s)\n",
		    number, vtotal, vtotal == PANEL_VTOTAL ? "match" : "DIFFERENT");
	}

	counters_en = rd(base + INTF_FRAME_LINE_COUNT_EN);
	frame = rd(base + INTF_FRAME_COUNT);
	line = rd(base + INTF_LINE_COUNT);
	printf("vayu-vsync: INTF%u counters: enable %#x frame %u line %u\n", number, counters_en, frame, line);

	/*
	 * The counters run although FRAME_LINE_COUNT_EN reads 0 (run 164539: frame 19209 at 161.8 s of
	 * uptime, about 120 a second): "on" is FRAME_COUNT moving within 50 ms.
	 */
	counters_on = counters_en != 0 || frame_moves(base);
	if (engine_on && counters_on) {
		measure(number, base, vtotal);
	} else {
		printf("vayu-vsync: INTF%u: not measured (engine off or counters off)\n", number);
	}
}

/* ---- step 2: the panel's frame timing for the framebuffer (reads only) ---------------------------- */

/*
 * The framebuffer's hooks (MiniIOMobileFramebuffer.cpp): set once the timing is known. vayu_vsync_next()
 * reads INTF1's line counter -- where the scan-out is in the frame -- and gives the absolute time the
 * next frame starts; the framebuffer tells iOS of refreshes then, and starts its copy then, so the copy
 * runs down the screen behind the scan-out instead of at a timer's 120 Hz beating against the panel's.
 */
extern uint64_t (*hakios_vsync_next)(uint64_t now);
extern uint64_t (*hakios_vsync_frame)(void);

static vm_offset_t panel_intf;
static uint64_t panel_frame_abs;                /* one frame, in absolute-time units */
static uint32_t panel_vtotal;
static bool panel_armed;

/*
 * A watchdog on the way: every 64th call, FRAME_COUNT must have moved since the last look and the
 * line counter must be in the frame; if not (the timing engine stopped, a panel reset), the hooks
 * answer 0 from then on and the framebuffer goes back to its fb_hz timer.
 */
static uint32_t watch_calls, watch_frame;

uint64_t
vayu_vsync_next(__unused uint64_t asked)
{
	uint32_t line, frame;
	uint64_t now, into;

	if (!os_atomic_load(&panel_armed, acquire)) {
		return 0;
	}
	line = rd(panel_intf + INTF_LINE_COUNT);
	now = mach_absolute_time();                     /* (taken with the line, not before the caller's work) */
	if ((++watch_calls & 63) == 0) {
		frame = rd(panel_intf + INTF_FRAME_COUNT);
		if (frame == watch_frame || line >= panel_vtotal + 16) {
			os_atomic_store(&panel_armed, false, release);
			printf("vayu-vsync: 2: DISARMED: frame %u (was %u), line %u: the framebuffer's timer takes over\n", frame,
			    watch_frame, line);
			return 0;
		}
		watch_frame = frame;
	}
	line = line >= panel_vtotal ? panel_vtotal - 1 : line;
	into = panel_frame_abs * line / panel_vtotal;
	return now - into + panel_frame_abs;            /* (into < one frame: always after now) */
}

uint64_t
vayu_vsync_frame(void)
{
	return os_atomic_load(&panel_armed, acquire) ? panel_frame_abs : 0;
}

/* the next change of FRAME_COUNT (polled every 20 us, at most 50 ms): its time, and the count; false if none */
static bool
frame_edge(vm_offset_t base, uint64_t *when, uint32_t *count)
{
	uint32_t first = rd(base + INTF_FRAME_COUNT), now;

	for (unsigned i = 0; i < 2500; i++) {
		delay(20);
		if ((now = rd(base + INTF_FRAME_COUNT)) != first) {
			*when = mach_absolute_time();
			*count = now;
			return true;
		}
	}
	return false;
}

static void
step2(vm_offset_t base)
{
	uint64_t t0, t1, ns, mhz;
	uint32_t f0, f1, lines[5], vtotal, hsync_period = rd(base + INTF_HSYNC_CTL) >> 16;
	bool varies = false, bounded = true;

	vtotal = hsync_period != 0 ? rd(base + INTF_VSYNC_PERIOD_F0) / hsync_period : 0;
	if (!(rd(base + INTF_TIMING_ENGINE_EN) & INTF_TIMING_ENGINE_ON) || vtotal != PANEL_VTOTAL) {
		printf("vayu-vsync: 2: INTF1 is not the panel's 120 Hz timing (engine %#x, vtotal %u): not armed\n",
		    rd(base + INTF_TIMING_ENGINE_EN), vtotal);
		return;
	}
	if (!frame_edge(base, &t0, &f0)) {
		printf("vayu-vsync: 2: FRAME_COUNT does not move: not armed\n");
		return;
	}
	IOSleep(1000);
	if (!frame_edge(base, &t1, &f1) || f1 == f0) {
		printf("vayu-vsync: 2: FRAME_COUNT stopped: not armed\n");
		return;
	}
	panel_frame_abs = (t1 - t0) / (f1 - f0);
	absolutetime_to_nanoseconds(panel_frame_abs, &ns);
	mhz = ns != 0 ? 1000000000000ULL / ns : 0;            /* millihertz */
	printf("vayu-vsync: 2: the panel's frame is %llu ns (%llu.%03llu Hz) over %u frames\n", ns, mhz / 1000, mhz % 1000,
	    f1 - f0);
	if (mhz < 100000 || mhz > 140000) {
		printf("vayu-vsync: 2: not between 100 and 140 Hz: not armed\n");
		return;
	}
	for (unsigned i = 0; i < 5; i++) {
		lines[i] = rd(base + INTF_LINE_COUNT);
		varies |= i > 0 && lines[i] != lines[i - 1];
		bounded &= lines[i] < vtotal + 16;
		delay(200);
	}
	printf("vayu-vsync: 2: line counter %u %u %u %u %u\n", lines[0], lines[1], lines[2], lines[3], lines[4]);
	if (!varies || !bounded) {
		printf("vayu-vsync: 2: the line counter %s: not armed\n", !varies ? "does not move" : "runs past the frame");
		return;
	}
	panel_intf = base;
	panel_vtotal = vtotal;
	watch_frame = rd(base + INTF_FRAME_COUNT) - 1;
	os_atomic_store(&panel_armed, true, release);
	os_atomic_store(&hakios_vsync_frame, vayu_vsync_frame, release);
	os_atomic_store(&hakios_vsync_next, vayu_vsync_next, release);
	printf("vayu-vsync: 2: armed: the framebuffer's refresh follows the panel\n");
	for (unsigned s = 30; s <= 120; s += 30) {
		uint64_t now, frames_expected;
		uint32_t counted;

		IOSleep(30000);
		now = mach_absolute_time();
		counted = rd(base + INTF_FRAME_COUNT) - f1;
		frames_expected = (now - t1) / panel_frame_abs;
		printf("vayu-vsync: 2: after %u s: %u frames counted, %llu expected\n", s, counted, frames_expected);
	}
}

static void
vayu_vsync_thread(void *parameter, __unused wait_result_t result)
{
	static const unsigned offsets[INTF_COUNT] = { INTF_1_OFFSET, INTF_2_OFFSET };
	uint32_t level = (uint32_t)(uintptr_t)parameter;
	vm_offset_t window;

	printf("vayu-vsync: step 1: a read-only look at the MDP's INTF 1 and 2\n");
	window = ml_io_map(INTF_PHYS, INTF_MAP_SIZE);
	for (unsigned i = 0; i < INTF_COUNT; i++) {
		show_intf(i + 1, window + offsets[i]);
	}
	printf("vayu-vsync: step 1 done, nothing written\n");
	if (level >= 2) {
		step2(window + INTF_1_OFFSET);
	}
	thread_terminate(current_thread());
}

void
vayu_vsync_init(void)
{
	thread_t thread;
	uint32_t level = 0;

	if (!PE_parse_boot_argn("vayu_vsync", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(vayu_vsync_thread, (void *)(uintptr_t)level, &thread) != KERN_SUCCESS) {
		printf("vayu-vsync: the probe's thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}
