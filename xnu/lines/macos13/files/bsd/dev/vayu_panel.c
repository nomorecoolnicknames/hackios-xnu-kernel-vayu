/*
 * The panel of the POCO X3 Pro (vayu): what it really scans at, and where its own frame signal is,
 * bsd/dev/vayu_panel.c. Boot-arg vayu_panel=1. Step 1 of this file reads; see the bottom for what
 * it would take to change the mode (nothing here writes).
 *
 * Why: the framebuffer tells iOS the display is 120 Hz (MiniIOMobileFramebuffer.cpp, fb_120=1) and
 * counts its refreshes itself. That says nothing about the panel. This file asks the hardware:
 *
 *   the timing engine  MDP INTF1 (DSI 0) at 0x0ae6b800: its frame counter (INTF_FRAME_COUNT) is
 *                      clocked by the display controller's own pixel clock, so the number of
 *                      frames it counts in a second IS the rate the panel is scanned at -- no
 *                      timer of ours, no user space. vayu_vsync.c (boot-arg vayu_vsync=1) reads
 *                      the same block; this one keeps the number as the panel's answer.
 *   the mode           INTF1's hsync/vsync registers, against the panel's own 120 Hz timing in the
 *                      phone's live device tree (recovery-live.dtb, node
 *                      qcom,mdss_dsi_j20s_42_02_0b_dsc_video): 1080x2400, h pulse 12, back 40,
 *                      front 67 -> htotal 479 (the panel is DSC 3:1, so the controller counts
 *                      compressed pixels: 1080/3 + 119 = 479), v pulse 2, back 30, front 33 ->
 *                      vtotal 2465. pclk = 479 * 2465 * fps.
 *   the link           DSI0's controller (0x0ae94000, 0x400), its PHY (0x0ae94400, 0x760) and its
 *                      PLL (0x0ae94900, 0x260): DSI_CTRL's VIDEO_MODE_EN/CMD_MODE_EN say which of
 *                      the two ways the panel is driven. A video-mode panel is scanned by the
 *                      stream that arrives, so its rate is INTF's; a command-mode one keeps its
 *                      own picture and refreshes on its own oscillator, and then only TE knows.
 *   TE                 the panel's tearing-effect line, qcom,platform-te-gpio = <&tlmm 8 0> in the
 *                      same node. Read as a pin (TLMM 0x03000000, GPIO 8, the layout vayu_nvt.c
 *                      uses: cfg at +0x1000*n, in/out at +4, bit 0 = in). Edge to edge is the
 *                      panel's own frame; it is the one measurement that owes nothing to the
 *                      display controller.
 *
 * Nothing is written anywhere: every access below is a 32-bit read (and the TE pin is only read,
 * never configured -- if it is not already a GPIO input the line says so and no measurement is
 * made).
 *
 * The lines, all through tee_printf (the loader's log keeps them; see vayu_vsync.c):
 *
 *   panel: 120 Hz mode, TE period 8.333 ms (...)
 *
 * is the answer the display work asked for, and it is printed only when the hardware agrees
 * (INTF frame counter between 100 and 140 Hz). A second line says where TE was found and what it
 * measured, or that it was not there.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_panel.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void IOSleep(unsigned int milliseconds);
extern void delay(int usec);
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
	if (ml_get_interrupts_enabled()) {
		(printf)("%s", line);
	}
	return length;
}
#define printf(...) tee_printf(__VA_ARGS__)

/* the display controller: sm8150-sde.dtsi:17 reg = <0x0ae00000 0x84208>; INTF1 at +0x6b800 */
#define MDP_PHYS                0x0ae01000UL
#define MDP_SIZE                0x90000UL
#define INTF1_OFFSET            0x6b800UL
#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_HSYNC_CTL          0x008
#define INTF_VSYNC_PERIOD_F0    0x00c
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0

/* DSI0: recovery-live.dts:3208 qcom,mdss_dsi_ctrl0@ae94000; PHY :3292 at 0xae94400; PLL :791 at 0xae94900 */
#define DSI_PHYS                0x0ae94000UL
#define DSI_SIZE                0x1000UL
#define DSI_CTRL                0x004           /* BIT(0) enable, BIT(1) video mode, BIT(2) command mode */
#define DSI_STATUS              0x008
#define DSI_VIDEO_MODE_CTRL     0x038           /* (dumped raw: the traffic mode's bits differ by version) */
#define DSI_VIDEO_MODE_ACTIVE_H 0x03c
#define DSI_VIDEO_MODE_ACTIVE_V 0x040
#define DSI_VIDEO_MODE_TOTAL    0x044
#define DSI_PHY_OFFSET          0x400
#define DSI_PLL_OFFSET          0x900

/* the pins: recovery-live.dts:11194 pinctrl@03000000; the layout vayu_nvt.c:146-149 uses */
#define TLMM_PHYS               0x03000000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))
#define GPIO_IO(n)              (GPIO_CFG(n) + 4)
#define TE_PIN                  8               /* qcom,platform-te-gpio = <0x38 0x08 0x00> */

#define PANEL_VTOTAL            2465u           /* 2400 + 2 + 30 + 33: the 120 Hz timing */
#define PANEL_HTOTAL            479u            /* 1080/3 + 12 + 40 + 67 (DSC 3:1) */

static vm_offset_t mdp, dsi, tlmm;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

/*
 * The panel's frame, from the display controller's own counter. Returns the period in nanoseconds
 * (0 if the counter does not move, i.e. the timing engine is off or INTF1 is not the panel's).
 */
static uint64_t
panel_frame_period(unsigned *frames_out)
{
	uint32_t frame0, frame1, frames;
	uint64_t start, end, elapsed_ns, period;

	frame0 = rd(mdp + INTF1_OFFSET + INTF_FRAME_COUNT);
	start = mach_absolute_time();
	IOSleep(1000);
	frame1 = rd(mdp + INTF1_OFFSET + INTF_FRAME_COUNT);
	end = mach_absolute_time();
	frames = frame1 - frame0;
	if (frames_out != NULL) {
		*frames_out = frames;
	}
	if (frames == 0) {
		return 0;
	}
	absolutetime_to_nanoseconds(end - start, &elapsed_ns);
	period = elapsed_ns / frames;
	return period;
}

/* nanoseconds -> "seconds.milliseconds", for a line: 8333281 -> "8.333" ms */
static void
print_ms(uint64_t ns, char *out, size_t size)
{
	snprintf(out, size, "%llu.%03llu", ns / 1000000ULL, (ns / 1000ULL) % 1000ULL);
}

static uint32_t
milli_hz(uint64_t ns)
{
	return ns != 0 ? (uint32_t)(1000000000000ULL / ns) : 0;         /* mHz */
}

/*
 * The panel's own frame signal on TE, if it is a pin we can read. Edge to edge: the smallest and
 * the middle of the periods seen, which is what says whether the panel's own refresh is the same as
 * the controller's. Read-only: the pin's configuration is printed, never written.
 */
static void
te_measure(void)
{
	uint32_t cfg, edges = 0, samples = 0;
	uint64_t last = 0, began, ended, elapsed_ns, now;
	uint32_t smallest = 0xffffffffu, largest = 0;
	bool level, was;

	cfg = rd(GPIO_CFG(TE_PIN));
	if (((cfg >> 2) & 0xf) != 0) {
		printf("panel: TE gpio %u is not a plain GPIO (cfg %#x, function %u): not measured\n",
		    TE_PIN, cfg, (cfg >> 2) & 0xf);
		return;
	}
	was = (rd(GPIO_IO(TE_PIN)) & 1) != 0;
	began = mach_absolute_time();
	/* 150 ms at 20 us: 18 frames of 120 Hz, 9 of 60 */
	for (unsigned i = 0; i < 7500; i++) {
		delay(20);
		level = (rd(GPIO_IO(TE_PIN)) & 1) != 0;
		samples++;
		if (level && !was) {
			now = mach_absolute_time();
			if (last != 0) {
				uint64_t period_ns;
				uint32_t period_us;

				absolutetime_to_nanoseconds(now - last, &period_ns);
				period_us = (uint32_t)(period_ns / 1000);
				if (period_us != 0 && period_us < smallest) {
					smallest = period_us;
				}
				if (period_us > largest) {
					largest = period_us;
				}
				edges++;
			}
			last = now;
		}
		was = level;
	}
	ended = mach_absolute_time();
	absolutetime_to_nanoseconds(ended - began, &elapsed_ns);
	if (edges == 0 || smallest == 0xffffffffu) {
		printf("panel: TE gpio %u (cfg %#x): %u samples in %llu ms, no edge: the panel's TE is not on this pin "
		    "(a video-mode panel need not drive it)\n", TE_PIN, cfg, samples, elapsed_ns / NSEC_PER_MSEC);
		return;
	}
	printf("panel: TE gpio %u (cfg %#x): %u edges, period %u..%u us (%u.%03u..%u.%03u ms)\n", TE_PIN, cfg,
	    edges + 1, smallest, largest, smallest / 1000, smallest % 1000, largest / 1000, largest % 1000);
}

/*
 * What the DSI link is doing. The controller's DSI_CTRL is the one place that says whether the
 * panel is scanned by the stream (video mode) or keeps its own picture (command mode); the rest is
 * dumped raw because the bit names move between the DSI versions and a wrong name is worse than a
 * number.
 */
static void
show_dsi(void)
{
	uint32_t ctrl = rd(dsi + DSI_CTRL), status = rd(dsi + DSI_STATUS);

	printf("panel: DSI0 hw version %#x, DSI_CTRL %#x (enable %u, video mode %u, command mode %u, dma sched %u/%u), "
	    "DSI_STATUS %#x\n", rd(dsi + 0x000), ctrl, ctrl & 1, (ctrl >> 1) & 1, (ctrl >> 2) & 1, (ctrl >> 3) & 1, (ctrl >> 4) & 1,
	    status);
	printf("panel: DSI0 video mode: ctrl %#x active %#x/%#x total %#x\n", rd(dsi + DSI_VIDEO_MODE_CTRL),
	    rd(dsi + DSI_VIDEO_MODE_ACTIVE_H), rd(dsi + DSI_VIDEO_MODE_ACTIVE_V), rd(dsi + DSI_VIDEO_MODE_TOTAL));
	for (uint32_t o = 0; o < 0x50; o += 0x10) {
		printf("panel: DSI0 +%03x: %08x %08x %08x %08x\n", o, rd(dsi + o), rd(dsi + o + 4), rd(dsi + o + 8),
		    rd(dsi + o + 12));
	}
	for (uint32_t o = 0; o < 0x40; o += 0x10) {
		printf("panel: DSI0 phy +%03x: %08x %08x %08x %08x\n", o, rd(dsi + DSI_PHY_OFFSET + o),
		    rd(dsi + DSI_PHY_OFFSET + o + 4), rd(dsi + DSI_PHY_OFFSET + o + 8), rd(dsi + DSI_PHY_OFFSET + o + 12));
	}
	for (uint32_t o = 0; o < 0x40; o += 0x10) {
		printf("panel: DSI0 pll +%03x: %08x %08x %08x %08x\n", o, rd(dsi + DSI_PLL_OFFSET + o),
		    rd(dsi + DSI_PLL_OFFSET + o + 4), rd(dsi + DSI_PLL_OFFSET + o + 8), rd(dsi + DSI_PLL_OFFSET + o + 12));
	}
}

static void
vayu_panel_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t hsync_ctl, hsync_period, vsync_period, vtotal = 0, frames = 0, status;
	uint64_t period_ns;
	char ms[24];
	unsigned level = (unsigned)(uintptr_t)parameter;

	mdp = ml_io_map(MDP_PHYS, MDP_SIZE);
	dsi = ml_io_map(DSI_PHYS, DSI_SIZE);
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (mdp == 0 || dsi == 0 || tlmm == 0) {
		printf("panel: cannot map the display blocks (%#lx %#lx %#lx)\n", mdp, dsi, tlmm);
		thread_terminate(current_thread());
		return;
	}
	/* the user interface is up by now: everything here is what is running, not what was set up */
	IOSleep(20000);

	status = rd(mdp + INTF1_OFFSET + INTF_TIMING_ENGINE_EN);
	hsync_ctl = rd(mdp + INTF1_OFFSET + INTF_HSYNC_CTL);
	hsync_period = hsync_ctl >> 16;
	vsync_period = rd(mdp + INTF1_OFFSET + INTF_VSYNC_PERIOD_F0);
	if (hsync_period != 0) {
		vtotal = vsync_period / hsync_period;
	}
	printf("panel: INTF1 (DSI 0) timing engine %s, htotal %u, vtotal %u (%s the 120 Hz timing's %u/%u)\n",
	    (status & 1) != 0 ? "on" : "off", hsync_period, vtotal,
	    hsync_period == PANEL_HTOTAL && vtotal == PANEL_VTOTAL ? "matches" : "DIFFERENT from",
	    PANEL_HTOTAL, PANEL_VTOTAL);

	period_ns = panel_frame_period(&frames);
	if (period_ns == 0) {
		printf("panel: the frame counter does not move: the timing engine is off, not measured\n");
		show_dsi();
		thread_terminate(current_thread());
		return;
	}
	print_ms(period_ns, ms, sizeof(ms));
	printf("panel: the display controller's own frame counter: %u frames in 1000 ms, period %s ms (%u.%03u Hz)\n",
	    frames, ms, milli_hz(period_ns) / 1000, milli_hz(period_ns) % 1000);
	/* a second look, 3 s later: a counter that is a timer of ours would not stay on the panel's period */
	IOSleep(3000);
	period_ns = panel_frame_period(&frames);
	print_ms(period_ns, ms, sizeof(ms));
	printf("panel: 3 s later: %u frames in 1000 ms, period %s ms (%u.%03u Hz)\n", frames, ms,
	    milli_hz(period_ns) / 1000, milli_hz(period_ns) % 1000);
	if (level >= 2) {
		/* vayu_panel=2: ten one-second counts, so a rate that drifts (or a counter that is ours) shows */
		for (unsigned i = 0; i < 8; i++) {
			IOSleep(1000);
			period_ns = panel_frame_period(&frames);
			print_ms(period_ns, ms, sizeof(ms));
			printf("panel: sample %u: %u frames in 1000 ms, period %s ms (%u.%03u Hz)\n", i + 3, frames, ms,
			    milli_hz(period_ns) / 1000, milli_hz(period_ns) % 1000);
		}
	}
	if (milli_hz(period_ns) >= 118000 && milli_hz(period_ns) <= 122000) {
		show_dsi();
		te_measure();
		print_ms(period_ns, ms, sizeof(ms));
		printf("panel: 120 Hz mode, TE period %s ms (measured: the display controller's frame counter; the panel "
		    "is scanned by the stream %u Hz, not 60)\n", ms, milli_hz(period_ns) / 1000);
	} else if (milli_hz(period_ns) >= 58000 && milli_hz(period_ns) <= 62000) {
		show_dsi();
		te_measure();
		print_ms(period_ns, ms, sizeof(ms));
		printf("panel: 60 Hz mode, TE period %s ms: the panel is scanned at 60; a 120 Hz stream would need the "
		    "timing engine reprogrammed (see REPORT.md)\n", ms);
	} else {
		show_dsi();
		te_measure();
	}
	thread_terminate(current_thread());
}

void
vayu_panel_init(void)
{
	thread_t thread;
	uint32_t level = 0;

	if (!PE_parse_boot_argn("vayu_panel", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(vayu_panel_thread, (void *)(uintptr_t)level, &thread) != KERN_SUCCESS) {
		printf("panel: the probe's thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}

/*
 * If the probe ever comes back 60 Hz, this is the change, in the order it has to be made -- and it
 * is a write to the display controller, so it belongs in a file of its own with its own boot-arg,
 * not here (nothing in this file writes):
 *
 *   1. INTF1: hsync period and vsync period from the panel's 120 Hz timing (this file's PANEL_HTOTAL
 *      and PANEL_VTOTAL: 479 and 2465), then TIMING_ENGINE_EN = 1 again, so the displayed timing
 *      changes at a frame's start and not in the middle of one. dpu_hw_intf.c:227-241.
 *   2. DSI0: DSI_VIDEO_MODE_ACTIVE_H/V and DSI_VIDEO_MODE_TOTAL set to the same numbers, in the
 *      encoding the DSI version uses (the raw dump above is what says which), and the video-mode
 *      DMA schedule bit in DSI_CTRL. msm dsi_host.c dsi_ctrl_config()/dsi_set_timing_db().
 *   3. The DSC block (0xae80000): bit-per-pixel and slice size unchanged -- 3:1 is what makes 120 Hz
 *      fit in the link at all (1080 x 2400 x 120 x 8 bits = 2.49 Gbit/s against 3.4 Gbit/s of
 *      compressed stream at pclk 141.7 MHz).
 *   4. The panel: nothing. The j20s node has one timing (120 Hz) and no DCS command that changes a
 *      frame rate (recovery-live.dts:3065-3074; the only commands are ON, POST-OFF, CABC and HBM),
 *      so a video-mode panel follows the stream and there is no 60 Hz mode to leave.
 */
