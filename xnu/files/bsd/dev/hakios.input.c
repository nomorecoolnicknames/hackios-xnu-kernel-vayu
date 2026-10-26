/*
 * /dev/hakios-touch: a queue of touch samples (see sys/hakios.input.h).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <os/atomic_private.h>
#include <sys/vayu_cpufreq.h>
#include <sys/vayu_vsync.h>
#include <sys/vayu_bt.h>
#include <sys/vayu_camera.h>
#include <sys/vayu_venus.h>
#include <sys/vayu_nfc.h>
#include <sys/vayu_wled.h>
#include <sys/vayu_mdp.h>
#include <sys/vayu_ir.h>
#include <sys/vayu_usbnet.h>
#include <pexpert/pexpert.h>

#define TOUCH_QUEUE     512             /* records; a power of two */

/*
 * Boot-arg touch_lat=1: for measuring how long a touch takes to reach the panel, a line for every
 * sample posted ("tl: p TYPE FINGER X Y at US", the time in microseconds of mach_absolute_time),
 * to be matched with the window server's lines about reading it (userland/src/inject.c), drawing
 * (eagl.c) and the display driver's about the frame (MiniIOMobileFramebuffer.cpp): see
 * tools/latency.py.
 */
static uint32_t touch_lat = 2;          /* 2: not yet looked up */

extern void loader_log_line(const char *line);

/*
 * A "tl:" line, for the kernel's drivers: into the loader's log as it is written -- the kernel's own
 * log keeps only the last of a run, and a live look at the loader's (XL_FOLLOW) then misses nothing.
 */
void hakios_tl(const char *format, ...) __printflike(1, 2);

void
hakios_tl(const char *format, ...)
{
	char line[192];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static bool
latency_lines(void)
{
	if (touch_lat == 2) {
		uint32_t on = 0;

		touch_lat = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	return touch_lat != 0;
}

static LCK_GRP_DECLARE(touch_lck_grp, "hakios-touch");
static LCK_MTX_DECLARE(touch_lock, &touch_lck_grp);
static struct hakios_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head, touch_tail;        /* read from head, add at tail */

/*
 * The way out when nothing else is at hand: five fingers held on the screen for a second and a
 * half end the run -- control goes back to the loader, as when loader_return_after expires.
 * Only a driver's fingers count, not samples a program writes to the device.
 */
#define EXIT_FINGERS    5
#define EXIT_HOLD_MS    1500

extern void loader_return(void);

static thread_call_t exit_call;
static uint32_t fingers_down;           /* one bit for each finger a driver says is down */

static void
exit_hold_elapsed(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	printf("\n--- %d fingers held on the screen for %d ms: back to the loader ---\n", EXIT_FINGERS, EXIT_HOLD_MS);
	hakios_adreno_stop();
	loader_return();
}

/* With touch_lock held. */
static void
count_fingers(uint32_t type, uint32_t finger)
{
	int before = __builtin_popcount(fingers_down), now;
	uint64_t deadline;

	if (finger >= 32 || exit_call == NULL || (type != HAKIOS_TOUCH_DOWN && type != HAKIOS_TOUCH_UP)) {
		return;
	}
	if (type == HAKIOS_TOUCH_DOWN) {
		fingers_down |= 1u << finger;
	} else {
		fingers_down &= ~(1u << finger);
	}
	now = __builtin_popcount(fingers_down);
	if (now >= EXIT_FINGERS && before < EXIT_FINGERS) {
		clock_interval_to_deadline(EXIT_HOLD_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(exit_call, deadline);
	} else if (now < EXIT_FINGERS && before >= EXIT_FINGERS) {
		thread_call_cancel(exit_call);
	}
}

static void
touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time, bool from_driver)
{
	struct hakios_touch *t;

	lck_mtx_lock(&touch_lock);
	if (touch_tail - touch_head == TOUCH_QUEUE) {
		touch_head++;                   /* nobody reads: drop the oldest */
	}
	t = &touch_queue[touch_tail++ % TOUCH_QUEUE];
	t->time = time != 0 ? time : mach_absolute_time();
	if (latency_lines()) {
		uint64_t ns;

		absolutetime_to_nanoseconds(t->time, &ns);
		hakios_tl("tl: p %u %u %u %u at %llu\n", type, finger, x, y, ns / 1000);
	}
	t->type = type;
	t->finger = finger;
	t->x = x > 65535 ? 65535 : x;
	t->y = y > 65535 ? 65535 : y;
	if (from_driver) {
		count_fingers(type, finger);
	}
	lck_mtx_unlock(&touch_lock);
	wakeup(&touch_queue);
}

extern uint32_t hakios_display_crop[6];        /* MiniIOMobileFramebuffer.cpp, fb_fit=1 */

/*
 * How long the screen has been left alone, for fb_120_idle=1 (iokit/Kernel/MiniIOMobileFramebuffer.cpp: 120 Hz only
 * while touched): the time of the last touch or button event, and the fingers that are down -- a finger held still
 * posts nothing (the driver drops samples that did not move), but it is a touch all the same.
 */
static volatile uint64_t activity_abs;
static volatile uint32_t activity_down;

uint64_t
hakios_touch_idle_abs(void)
{
	uint64_t last = os_atomic_load(&activity_abs, relaxed), now = mach_absolute_time();

	if (os_atomic_load(&activity_down, relaxed) != 0) {
		return 0;
	}
	return last == 0 || now < last ? ~0ULL : now - last;
}

void
hakios_touch_post_at(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, uint64_t time)
{
	uint32_t panel_h = os_atomic_load(&hakios_display_crop[5], acquire);

	os_atomic_store(&activity_abs, mach_absolute_time(), relaxed);
	if (finger < 32 && type == HAKIOS_TOUCH_DOWN) {
		os_atomic_or(&activity_down, 1u << finger, relaxed);
	} else if (finger < 32 && type == HAKIOS_TOUCH_UP) {
		os_atomic_andnot(&activity_down, 1u << finger, relaxed);
	}

	/*
	 * With fb_fit=1 the panel shows the picture's middle pixel for pixel: a touch at a place on the
	 * panel is at that place plus the cut-off edge in the picture, not at the same fraction of it.
	 */
	if (panel_h != 0 && type != HAKIOS_BUTTON_DOWN && type != HAKIOS_BUTTON_UP) {
		uint64_t px = (uint64_t)x * hakios_display_crop[4] / 65535, py = (uint64_t)y * panel_h / 65535;

		x = (uint32_t)((px + hakios_display_crop[0]) * 65535 / hakios_display_crop[2]);
		y = (uint32_t)((py + hakios_display_crop[1]) * 65535 / hakios_display_crop[3]);
	}
	touch_post(type, finger, x, y, time, true);
}

void
hakios_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	hakios_touch_post_at(type, finger, x, y, mach_absolute_time());
}

static int
touch_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0, moved = 0;

	lck_mtx_lock(&touch_lock);
	while (touch_head == touch_tail) {
		error = msleep(&touch_queue, &touch_lock, PZERO | PCATCH, "hakios-touch", NULL);
		if (error) {
			lck_mtx_unlock(&touch_lock);
			return error;
		}
	}
	while (touch_head != touch_tail && uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		t = touch_queue[touch_head++ % TOUCH_QUEUE];
		lck_mtx_unlock(&touch_lock);
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		lck_mtx_lock(&touch_lock);
		if (error) {
			break;
		}
		moved++;
	}
	lck_mtx_unlock(&touch_lock);
	return moved ? 0 : error;
}

static int
touch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_touch t;
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		if (error) {
			break;
		}
		if (t.type < HAKIOS_TOUCH_DOWN || t.type > HAKIOS_BUTTON_UP) {
			return EINVAL;
		}
		touch_post(t.type, t.finger, t.x, t.y, 0, false);
	}
	return error;
}

static const struct cdevsw touch_cdevsw = {
	.d_open = touch_open, .d_close = touch_close, .d_read = touch_read, .d_write = touch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_input_init(void)
{
	int major = cdevsw_add(-1, &touch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-touch") == NULL) {
		printf("hakios-touch: cannot create the device\n");
	}
	exit_call = thread_call_allocate(exit_hold_elapsed, NULL);
	hakios_cpufreq_init();
	vayu_cpufreq_init();            /* boot-args cpufreq=1 cpufreq_vayu=1: SM8150 OSM, L3, memory bandwidth */
	vayu_vsync_init();              /* boot-arg vayu_vsync=1: a read-only look at the display controller's timing engine */
	vayu_bt_init();                 /* boot-arg vayu_bt=1: a read-only look at the Bluetooth UART (QUP wrapper 2 SE3) */
	vayu_camera_init();             /* boot-arg vayu_camera=1: a read-only look at the camera clocks, GDSC and sensor pins */
	vayu_venus_init();              /* boot-arg vayu_venus=1: the video encoder's M1 (power, PAS, HFI SYS_INIT) */
	vayu_nfc_init();                /* boot-arg vayu_nfc=N: the PN553 NFC controller, steps 1-4 (see bsd/dev/vayu_nfc.c) */
	vayu_wled_init();               /* boot-arg vayu_wled=1: the backlight looked at; =2: off/on with iOS's display power */
	vayu_mdp_init();                /* boot-arg vayu_mdp=1: the display controller as UEFI left it, read only */
	vayu_usbnet_init();             /* boot-arg vayu_usbnet=1: a read-only look at the USB controller as the loader's UEFI runs it */
	vayu_ir_init();                 /* boot-arg vayu_ir=1: the IR blaster (QUP0 SE1 SPI), /dev/vayu-ir */
	hakios_virtio_input_init();
	hakios_goodix_init();
	hakios_buttons_init();
	hakios_battery_init();
	hakios_ufs_init();
	hakios_net_init();
	hakios_adreno_init();
	vayu_adreno_init();             /* boot-args gpu=1 vayu_gpu=1: the Adreno 640 of vayu */
	hakios_wifi_init();
	hakios_adsp_init();
	hakios_mpss_init();
}
