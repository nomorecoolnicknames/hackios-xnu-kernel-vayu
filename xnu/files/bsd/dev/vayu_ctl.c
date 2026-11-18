/*
 * vayu-ctl: control of a running iOS over USB (k31-usbctl).
 *
 * See bsd/sys/vayu_ctl.h for the shape of the thing.  This file is the kernel end:
 *
 *   - one thread polls the mailbox the loader writes into (struct vayu_ctl_mbox, in this
 *     file's data, published by physical address through osfmk/arm64/loader_return.c) and
 *     answers each request;
 *   - touches and buttons go through hakios_touch_post_synthetic(), the same path a write()
 *     to /dev/hakios-touch takes from /redwood/bin/touch (bsd/dev/hakios.input.c);
 *   - shell commands and file reads are handed to /redwood/bin/usbctld through
 *     /dev/vayu-ctl, a character device with one job in flight: the agent reads a command,
 *     runs it (with its own shell: iOS 16.5's root has none), and writes the output back.
 *
 * The answer's bytes are placed in vctl_window[], four megabytes of this file's data.  The
 * kernel cleans them out of the cache (dc cvac) and gives the loader their physical address;
 * the loader stages them for "fastboot get_staged" exactly as it stages the framebuffer for
 * "oem screenshot".  Nothing here talks to the USB controller: the loader keeps it, which is
 * what makes this work in the middle of a run.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vayu_ctl.h>

extern void IOSleep(unsigned int milliseconds);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_live_set_vctl(uint64_t mbox_pa, uint64_t window_pa, uint64_t window_len);

#define VCTL_POLL_MS            5               /* how often the mailbox is looked at */
#define VCTL_TIMEOUT_SECONDS    20              /* a shell command or one pull chunk */
#define VCTL_JOB_SLICE_MS       100             /* the agent gets the lock back between slices */
#define VCTL_TAP_MS_DEFAULT     80              /* a tap held long enough for a recognizer */
#define VCTL_TAP_MS_MIN         10
#define VCTL_TAP_MS_MAX         2000

/* The loader's request, once the kernel has taken it. */
static struct vayu_ctl_mbox vctl_mbox __attribute__((aligned(4096))) = {
	.magic = VAYU_CTL_MBOX_MAGIC,
};
/* The answer's bytes; physically contiguous like the loader's own log (a mach-o __data array). */
static char vctl_window[VAYU_CTL_PULL_MAX] __attribute__((aligned(4096)));

static lck_grp_t *vctl_lock_group;
static lck_mtx_t *vctl_lock;

static struct {
	int      valid;
	int      delivered;
	uint64_t id;
	uint64_t cmd;
	uint64_t arg[6];
	uint32_t len;
	char     text[VAYU_CTL_TEXT_MAX];
} vctl_job;

static uint64_t vctl_job_next = 1;
static int      vctl_resp_done;
static int64_t  vctl_resp_code;
static uint32_t vctl_resp_len;
static uint64_t vctl_pull_total;
static uint64_t vctl_pull_base;
static uint64_t vctl_pull_max;
static uint32_t vctl_flags;
static int      vctl_agent_seen;
static uint32_t vctl_timeout = VCTL_TIMEOUT_SECONDS;
static uint64_t vctl_commands, vctl_timeouts;
static int      vctl_log = 1;           /* boot-arg usbctl_log=0: no per-command line */

/* Write the answer's bytes back to memory: the loader reads them at their physical address. */
static void
vctl_clean(const void *va, size_t len)
{
	uintptr_t p;

	for (p = (uintptr_t)va & ~63UL; p < (uintptr_t)va + len; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
vctl_barrier(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * Wait for the agent's answer, in slices, so that a loader that gave up (its own timeout is
 * longer) and a new request cannot pin this thread for ever.
 */
static int64_t
vctl_wait_answer(uint64_t seconds)
{
	uint64_t deadline;
	struct timespec slice = { .tv_sec = 0, .tv_nsec = VCTL_JOB_SLICE_MS * 1000000ULL };

	clock_interval_to_deadline((uint32_t)seconds, NSEC_PER_SEC, &deadline);
	while (!vctl_resp_done) {
		if (mach_absolute_time() >= deadline) {
			return VAYU_CTL_ERR_TIMEOUT;
		}
		(void)msleep(&vctl_resp_done, vctl_lock, PRIBIO, "vayu-ctl-answer", &slice);
	}
	return vctl_resp_code;
}

/* Queue one command for usbctld and wait for it.  Called with no lock held. */
static int64_t
vctl_agent_call(uint64_t cmd, const char *text, uint64_t a0, uint64_t a1, uint64_t seconds, uint32_t *out_len)
{
	int64_t code;

	lck_mtx_lock(vctl_lock);
	if (vctl_job.valid) {                   /* one at a time: somebody else is still there */
		lck_mtx_unlock(vctl_lock);
		return VAYU_CTL_ERR_BUSY;
	}
	memset(&vctl_job, 0, sizeof(vctl_job));
	vctl_job.id = vctl_job_next++;
	vctl_job.cmd = cmd;
	vctl_job.arg[0] = a0;
	vctl_job.arg[1] = a1;
	if (text != NULL) {
		strlcpy(vctl_job.text, text, sizeof(vctl_job.text));
		vctl_job.len = (uint32_t)strlen(vctl_job.text) + 1;
	}
	vctl_resp_done = 0;
	vctl_resp_code = VAYU_CTL_OK;
	vctl_resp_len = 0;
	vctl_flags = 0;
	vctl_pull_total = 0;
	vctl_job.valid = 1;
	vctl_job.delivered = 0;
	wakeup(&vctl_job);

	code = vctl_wait_answer(seconds);
	if (code == VAYU_CTL_ERR_TIMEOUT) {
		vctl_timeouts++;
	}
	if (out_len != NULL) {
		*out_len = vctl_resp_len;
	}
	vctl_job.valid = 0;
	lck_mtx_unlock(vctl_lock);
	return code;
}

/* --------------------------------------------------------------------------- touch/keys */

/*
 * A tap as the host means it: down, one more sample at the same place, and up after `ms`.
 * /redwood/bin/touch holds a tap 60 ms; the first run's taps at 60 ms did reach iOS (the
 * screen woke and Settings opened), but a tap that lands while the list is still moving is
 * easy for a recognizer to miss, and iOS's tap recognizers want the finger down for longer
 * than a frame.  80 ms and a second sample is the same gesture with the doubt taken out; the
 * host can ask for more with "oem touch X Y MS".
 */
static void
vctl_tap(uint32_t x, uint32_t y, uint32_t ms)
{
	if (ms < VCTL_TAP_MS_MIN || ms > VCTL_TAP_MS_MAX) {
		ms = VCTL_TAP_MS_DEFAULT;
	}
	hakios_touch_post_synthetic(HAKIOS_TOUCH_DOWN, 0, x, y);
	IOSleep(20);                            /* the down is read before the move */
	hakios_touch_post_synthetic(HAKIOS_TOUCH_MOVE, 0, x, y);
	IOSleep(ms > 20 ? ms - 20 : 60);
	hakios_touch_post_synthetic(HAKIOS_TOUCH_UP, 0, x, y);
}

/* The panel pixels behind a 0..65535 pair, for the log: what the host asked for, in its units. */
static void
vctl_panel_px(uint32_t x, uint32_t y, uint32_t *px, uint32_t *py)
{
	uint32_t w = 0, h = 0;

	hakios_touch_panel_size(&w, &h);
	if (w == 0) {
		w = 1080;
	}
	if (h == 0) {
		h = 2400;
	}
	*px = (uint32_t)((uint64_t)x * w / 65535);
	*py = (uint32_t)((uint64_t)y * h / 65535);
}

static void
vctl_log_touch(const char *what, uint32_t x, uint32_t y, uint32_t ms)
{
	uint32_t px, py;

	if (!vctl_log) {
		return;
	}
	vctl_panel_px(x, y, &px, &py);
	printf("vayu-ctl: %s panel %u %u (%u ms) = %u %u of 65535\n", what, px, py, ms,
	    (unsigned)x, (unsigned)y);
}

/*
 * The same sampling rwtouch.c and touch.c use: a sample every 8 ms (one sleep after another
 * here, not on a clock: the drift over a gesture of a second or two is a millisecond or two,
 * and no window server measures it).
 */
static void
vctl_swipe(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t ms)
{
	uint32_t steps, i;
	int64_t dx = (int64_t)x1 - (int64_t)x0, dy = (int64_t)y1 - (int64_t)y0;

	if (ms == 0) {
		ms = 1;
	}
	steps = ms / 8;
	if (steps == 0) {
		steps = 1;
	}
	vctl_log_touch("swipe from", x0, y0, ms);
	if (vctl_log) {
		uint32_t px, py;

		vctl_panel_px(x1, y1, &px, &py);
		printf("vayu-ctl: swipe to panel %u %u = %u %u of 65535\n", px, py,
		    (unsigned)x1, (unsigned)y1);
	}
	hakios_touch_post_synthetic(HAKIOS_TOUCH_DOWN, 0, x0, y0);
	for (i = 1; i <= steps; i++) {
		IOSleep(8);
		hakios_touch_post_synthetic(HAKIOS_TOUCH_MOVE, 0,
		    (uint32_t)((int64_t)x0 + dx * (int64_t)i / (int64_t)steps),
		    (uint32_t)((int64_t)y0 + dy * (int64_t)i / (int64_t)steps));
	}
	IOSleep(16);                            /* a finger that lifts is still on the panel for a frame */
	hakios_touch_post_synthetic(HAKIOS_TOUCH_UP, 0, x1, y1);
}

static void
vctl_button(uint32_t which, uint32_t ms)
{
	hakios_touch_post_synthetic(HAKIOS_BUTTON_DOWN, which, 0, 0);
	IOSleep(ms != 0 ? ms : 80);
	hakios_touch_post_synthetic(HAKIOS_BUTTON_UP, which, 0, 0);
}

static int64_t
vctl_key(uint32_t key)
{
	uint32_t w = 0, h = 0;

	hakios_touch_panel_size(&w, &h);
	switch (key) {
	case VAYU_CTL_KEY_HOME:
		/* the gesture, not the HID button: an iPhone of this generation has no home button */
		vctl_swipe(w / 2, h > 1 ? h - 1 : h, w / 2, (uint32_t)(h * 55 / 100), 250);
		return VAYU_CTL_OK;
	case VAYU_CTL_KEY_POWER:
		vctl_button(HAKIOS_BUTTON_LOCK, 80);
		return VAYU_CTL_OK;
	case VAYU_CTL_KEY_VOLUP:
		vctl_button(HAKIOS_BUTTON_VOLUME_UP, 80);
		return VAYU_CTL_OK;
	case VAYU_CTL_KEY_VOLDOWN:
		vctl_button(HAKIOS_BUTTON_VOLUME_DOWN, 80);
		return VAYU_CTL_OK;
	case VAYU_CTL_KEY_BHOME:
		vctl_button(HAKIOS_BUTTON_HOME, 80);
		return VAYU_CTL_OK;
	default:
		return VAYU_CTL_ERR_ARGS;
	}
}

/* ------------------------------------------------------------------------------ dispatch */

static void
vctl_dispatch(struct vayu_ctl_mbox *m)
{
	uint32_t len = 0;
	int64_t code = VAYU_CTL_ERR_ARGS;

	vctl_commands++;
	m->data_pa = (uint64_t)kvtophys((vm_offset_t)vctl_window);
	m->data_len = 0;
	m->total = 0;
	m->flags = 0;

	switch (m->cmd) {
	case VAYU_CTL_CMD_TOUCH:
		if (m->arg[0] <= 65535 && m->arg[1] <= 65535) {
			uint32_t ms = (uint32_t)m->arg[2];

			if (ms < VCTL_TAP_MS_MIN || ms > VCTL_TAP_MS_MAX) {
				ms = VCTL_TAP_MS_DEFAULT;
			}
			vctl_log_touch("touch", (uint32_t)m->arg[0], (uint32_t)m->arg[1], ms);
			vctl_tap((uint32_t)m->arg[0], (uint32_t)m->arg[1], ms);
			code = VAYU_CTL_OK;
		}
		break;
	case VAYU_CTL_CMD_SWIPE:
		if (m->arg[0] <= 65535 && m->arg[1] <= 65535 && m->arg[2] <= 65535 && m->arg[3] <= 65535 &&
		    m->arg[4] <= 60000) {
			vctl_swipe((uint32_t)m->arg[0], (uint32_t)m->arg[1], (uint32_t)m->arg[2], (uint32_t)m->arg[3],
			    (uint32_t)m->arg[4]);
			code = VAYU_CTL_OK;
		}
		break;
	case VAYU_CTL_CMD_KEY:
		if (vctl_log) {
			printf("vayu-ctl: key %llu\n", (unsigned long long)m->arg[0]);
		}
		code = vctl_key((uint32_t)m->arg[0]);
		break;
	case VAYU_CTL_CMD_SH:
		m->text[sizeof(m->text) - 1] = 0;
		code = vctl_agent_call(VAYU_CTL_CMD_SH, m->text, 0, 0, vctl_timeout, &len);
		if (code >= 0 && len != 0) {
			vctl_clean(vctl_window, len);
		}
		if (vctl_log) {
			char line[VAYU_CTL_TEXT_MAX];

			strlcpy(line, m->text, sizeof(line));
			printf("vayu-ctl: sh \"%s\" -> %lld, %u bytes%s\n", line, (long long)code, len,
			    (vctl_flags & VAYU_CTL_F_TRUNCATED) ? " (cut)" : "");
		}
		break;
	case VAYU_CTL_CMD_PULL: {
		uint64_t want = m->arg[1];

		m->text[sizeof(m->text) - 1] = 0;
		if (want == 0 || want > VAYU_CTL_PULL_MAX) {
			want = VAYU_CTL_PULL_MAX;
		}
		vctl_pull_base = m->arg[0];
		vctl_pull_max = want;
		code = vctl_agent_call(VAYU_CTL_CMD_PULL, m->text, vctl_pull_base, want, vctl_timeout, &len);
		if (code >= 0 && len != 0) {
			vctl_clean(vctl_window, len);
		}
		m->total = vctl_pull_total;
		if (vctl_log) {
			char line[VAYU_CTL_TEXT_MAX];

			strlcpy(line, m->text, sizeof(line));
			printf("vayu-ctl: pull \"%s\" at %llu -> %lld, %u bytes, total %llu%s\n", line,
			    (unsigned long long)vctl_pull_base, (long long)code, len,
			    (unsigned long long)m->total, (vctl_flags & VAYU_CTL_F_LAST) ? " (last)" : "");
		}
		break;
	}
	case VAYU_CTL_CMD_STATUS:
		code = vctl_agent_seen ? 1 : 0;
		if (vctl_log) {
			printf("vayu-ctl: status: agent %lld\n", (long long)code);
		}
		break;
	default:
		break;
	}
	m->code = (uint64_t)code;
	m->data_len = len;
	m->flags = vctl_flags;
}

static void
vctl_thread(void *parameter, __unused wait_result_t result)
{
	uint64_t last_seq = 0;

	(void)parameter;
	for (;;) {
		struct vayu_ctl_mbox *m = &vctl_mbox;

		/*
		 * The request is the seq: a new one is a command to run, whatever the host did with
		 * the last.  The answer is done_seq = that seq, written after everything else, so a
		 * request the host gave up on (its own timeout) can finish late and be ignored, and
		 * the next one is taken by the next turn of this loop.
		 */
		if (m->seq != last_seq) {
			uint64_t seq = m->seq;

			last_seq = seq;
			m->state = VAYU_CTL_ST_BUSY;
			vctl_barrier();
			vctl_dispatch(m);
			m->done_seq = seq;
			vctl_barrier();
			m->state = VAYU_CTL_ST_DONE;
		}
		IOSleep(VCTL_POLL_MS);
	}
}

/* ------------------------------------------------------------------------- /dev/vayu-ctl */

static int
vctl_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(vctl_lock);
	vctl_agent_seen = 1;
	lck_mtx_unlock(vctl_lock);
	return 0;
}

static int
vctl_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	lck_mtx_lock(vctl_lock);
	vctl_agent_seen = 0;
	lck_mtx_unlock(vctl_lock);
	return 0;
}

static int
vctl_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct vayu_ctl_msg msg;
	char text[VAYU_CTL_TEXT_MAX];
	int error = 0;

	if (uio_resid(uio) < (user_ssize_t)sizeof(msg)) {
		return EINVAL;
	}
	lck_mtx_lock(vctl_lock);
	while (!vctl_job.valid || vctl_job.delivered) {
		error = msleep(&vctl_job, vctl_lock, PRIBIO | PCATCH, "vayu-ctl-read", NULL);
		if (error) {
			lck_mtx_unlock(vctl_lock);
			return error;
		}
	}
	memset(&msg, 0, sizeof(msg));
	msg.magic = VAYU_CTL_MSG_MAGIC;
	msg.type = VAYU_CTL_MSG_CMD;
	msg.id = vctl_job.id;
	msg.cmd = vctl_job.cmd;
	msg.len = vctl_job.len;
	for (int i = 0; i < 6; i++) {
		msg.arg[i] = vctl_job.arg[i];
	}
	memcpy(text, vctl_job.text, sizeof(text));      /* the job may time out while the copy runs */
	vctl_job.delivered = 1;
	lck_mtx_unlock(vctl_lock);

	error = uiomove((caddr_t)&msg, sizeof(msg), uio);
	if (error == 0 && msg.len != 0) {
		error = uiomove((caddr_t)text, (int)msg.len, uio);
	}
	return error;
}

/* Drop payload bytes whose job is over or whose room is gone. */
static int
vctl_drain(struct uio *uio, uint32_t left)
{
	char sink[256];

	while (left != 0) {
		uint32_t n = left > sizeof(sink) ? (uint32_t)sizeof(sink) : left;
		int error = uiomove(sink, (int)n, uio);

		if (error) {
			return error;
		}
		left -= n;
	}
	return 0;
}

static int
vctl_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct vayu_ctl_msg msg;
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(msg)) {
		error = uiomove((caddr_t)&msg, sizeof(msg), uio);
		if (error) {
			break;
		}
		if (msg.magic != VAYU_CTL_MSG_MAGIC || msg.len > VAYU_CTL_PULL_MAX) {
			return EINVAL;
		}
		if (msg.type == VAYU_CTL_MSG_HELLO) {
			lck_mtx_lock(vctl_lock);
			vctl_agent_seen = 1;
			lck_mtx_unlock(vctl_lock);
			continue;
		}
		lck_mtx_lock(vctl_lock);
		if (!vctl_job.valid || msg.id != vctl_job.id) {
			lck_mtx_unlock(vctl_lock);
			if ((error = vctl_drain(uio, msg.len)) != 0) {
				return error;
			}
			continue;
		}
		switch (msg.type) {
		case VAYU_CTL_MSG_OUT: {
			uint32_t room = vctl_resp_len < VAYU_CTL_SH_MAX ? VAYU_CTL_SH_MAX - vctl_resp_len : 0;
			uint32_t take = msg.len < room ? msg.len : room;
			uint32_t at = vctl_resp_len;

			if (take != 0) {
				lck_mtx_unlock(vctl_lock);
				error = uiomove(vctl_window + at, (int)take, uio);
				lck_mtx_lock(vctl_lock);
				if (error) {
					lck_mtx_unlock(vctl_lock);
					return error;
				}
				vctl_resp_len += take;
			}
			if (take < msg.len) {
				vctl_flags |= VAYU_CTL_F_TRUNCATED;
				lck_mtx_unlock(vctl_lock);
				error = vctl_drain(uio, msg.len - take);
				lck_mtx_lock(vctl_lock);
				if (error) {
					lck_mtx_unlock(vctl_lock);
					return error;
				}
			}
			break;
		}
		case VAYU_CTL_MSG_PULLDATA: {
			uint64_t off = msg.cmd;
			uint32_t left = msg.len;

			if (off >= vctl_pull_base && off - vctl_pull_base < vctl_pull_max &&
			    off - vctl_pull_base < VAYU_CTL_PULL_MAX) {
				uint32_t at = (uint32_t)(off - vctl_pull_base);
				uint32_t room = (uint32_t)vctl_pull_max - at;
				uint32_t take = left < room ? left : room;

				if (take > VAYU_CTL_PULL_MAX - at) {
					take = VAYU_CTL_PULL_MAX - at;
				}
				if (take != 0) {
					lck_mtx_unlock(vctl_lock);
					error = uiomove(vctl_window + at, (int)take, uio);
					lck_mtx_lock(vctl_lock);
					if (error) {
						lck_mtx_unlock(vctl_lock);
						return error;
					}
					if (at + take > vctl_resp_len) {
						vctl_resp_len = at + take;
					}
					left -= take;
				}
			}
			if (left != 0) {
				lck_mtx_unlock(vctl_lock);
				error = vctl_drain(uio, left);
				lck_mtx_lock(vctl_lock);
				if (error) {
					lck_mtx_unlock(vctl_lock);
					return error;
				}
			}
			break;
		}
		case VAYU_CTL_MSG_PULLDONE:
			vctl_pull_total = msg.arg[0];
			if (msg.arg[1] != 0) {          /* the agent reached the end of the file */
				vctl_flags |= VAYU_CTL_F_LAST;
			}
			vctl_resp_code = msg.code;
			vctl_resp_done = 1;
			wakeup(&vctl_resp_done);
			break;
		case VAYU_CTL_MSG_DONE:
			vctl_resp_code = msg.code;
			vctl_resp_done = 1;
			wakeup(&vctl_resp_done);
			break;
		default:
			break;
		}
		lck_mtx_unlock(vctl_lock);
	}
	return error;
}

static const struct cdevsw vctl_cdevsw = {
	.d_open = vctl_open, .d_close = vctl_close, .d_read = vctl_read, .d_write = vctl_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
vayu_ctl_init(void)
{
	thread_t thread;
	uint32_t on = 0;
	int major;

	if (PE_parse_boot_argn("usbctl", &on, sizeof(on)) && on == 0) {
		printf("vayu-ctl: usbctl=0: no control channel\n");
		return;
	}
	{
		uint32_t log_on = 1;

		if (PE_parse_boot_argn("usbctl_log", &log_on, sizeof(log_on))) {
			vctl_log = log_on != 0;
		}
	}
	PE_parse_boot_argn("usbctl_timeout", &vctl_timeout, sizeof(vctl_timeout));
	if (vctl_timeout < 1) {
		vctl_timeout = 1;
	}
	if (vctl_timeout > 600) {
		vctl_timeout = 600;
	}
	vctl_lock_group = lck_grp_alloc_init("vayu-ctl", LCK_GRP_ATTR_NULL);
	if (vctl_lock_group == NULL) {
		printf("vayu-ctl: no lock group\n");
		return;
	}
	vctl_lock = lck_mtx_alloc_init(vctl_lock_group, LCK_ATTR_NULL);
	if (vctl_lock == NULL) {
		printf("vayu-ctl: no lock\n");
		return;
	}
	major = cdevsw_add(-1, &vctl_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "vayu-ctl") == NULL) {
		printf("vayu-ctl: cannot create the device\n");
		return;
	}
	loader_live_set_vctl((uint64_t)kvtophys((vm_offset_t)&vctl_mbox),
	    (uint64_t)kvtophys((vm_offset_t)vctl_window), sizeof(vctl_window));
	if (kernel_thread_start(vctl_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("vayu-ctl: the thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
	printf("vayu-ctl: /dev/vayu-ctl, mailbox at %#llx, window %u KB, timeout %u s, log %d\n",
	    (uint64_t)kvtophys((vm_offset_t)&vctl_mbox), (uint64_t)(sizeof(vctl_window) / 1024), vctl_timeout, vctl_log);
}
