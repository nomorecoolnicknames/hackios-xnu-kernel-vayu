/*
 * usbctld: the userland half of usbctl -- the commands a host sends over the USB cable while
 * iOS runs (see bsd/sys/vayu_ctl.h and bsd/dev/vayu_ctl.c).
 *
 * A launchd job (com.hakios.usbctld.plist, root, KeepAlive).  It opens /dev/vayu-ctl, says
 * hello, and waits for one job at a time:
 *
 *   SH    run the command line; the output (stdout and stderr together) goes back as OUT
 *         messages, then DONE with the exit status.  A shell is looked for in the order
 *         -s PATH, /bin/sh, /var/jb/bin/sh, /var/jb/usr/bin/sh, /usr/bin/sh; iOS 16.5's own
 *         root has none, so the built-in minish.c runs it when none of those is there.
 *         Killed after -t seconds (15 by default): status 124, as timeout(1) does.
 *
 *   PULL  read up to arg[1] bytes of the file at arg[0] and send them back as PULLDATA
 *         messages at their absolute offsets, then PULLDONE with the file's size and
 *         whether this was the end.  The kernel holds four megabytes; xl asks for the next
 *         piece with "oem pull PATH OFFSET" until the end.
 *
 *   status: -v prints what it is doing and which shell it chose.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "minish.h"
#include "vayu_ctl.h"

#define PULL_CHUNK      (256 * 1024)    /* one PULLDATA message */
#define SH_CHUNK        (32 * 1024)     /* one OUT message */

static int   device = -1;
static int   device_fd = -1;            /* -d N: a descriptor instead of the device (the host test) */
static int   verbose;
static int   timeout_seconds = 15;
static const char *shell_path;
static const char *shell_tried[] = { "/bin/sh", "/var/jb/bin/sh", "/var/jb/usr/bin/sh", "/usr/bin/sh", NULL };
static const char *search_path =
    "/usr/local/bin:/var/jb/usr/local/bin:/usr/bin:/var/jb/usr/bin:/bin:/var/jb/bin:"
    "/usr/sbin:/var/jb/usr/sbin:/sbin:/var/jb/sbin:/redwood/bin";

static uint8_t send_buffer[PULL_CHUNK + sizeof(struct vayu_ctl_msg)];

static int
write_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len != 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int
send_msg(uint32_t type, uint64_t id, int64_t code, uint64_t cmd, const uint64_t *arg, const void *data,
    uint32_t len)
{
	struct vayu_ctl_msg *m = (struct vayu_ctl_msg *)send_buffer;

	if (len > PULL_CHUNK) {
		len = PULL_CHUNK;
	}
	memset(m, 0, sizeof(*m));
	m->magic = VAYU_CTL_MSG_MAGIC;
	m->type = type;
	m->id = id;
	m->code = code;
	m->cmd = cmd;
	m->len = len;
	if (arg != NULL) {
		for (int i = 0; i < 6; i++) {
			m->arg[i] = arg[i];
		}
	}
	if (len != 0 && data != NULL) {
		memcpy(send_buffer + sizeof(*m), data, len);
	}
	return write_all(device, send_buffer, sizeof(*m) + len);
}

static int64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
note(const char *fmt, ...)
{
	va_list ap;

	if (!verbose) {
		return;
	}
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------------------ shell */

static const char *
find_shell(void)
{
	for (int i = 0; shell_tried[i] != NULL; i++) {
		if (access(shell_tried[i], X_OK) == 0) {
			return shell_tried[i];
		}
	}
	return NULL;
}

static void
collect_output(int fd, char *out, size_t cap, size_t *out_len, int *truncated)
{
	char tmp[8192];
	ssize_t n;

	n = read(fd, tmp, sizeof(tmp));
	if (n > 0) {
		size_t room = *out_len < cap ? cap - *out_len : 0;
		size_t take = (size_t)n < room ? (size_t)n : room;

		if (take != 0) {
			memcpy(out + *out_len, tmp, take);
			*out_len += take;
		}
		if (take < (size_t)n) {
			*truncated = 1;
		}
	} else if (n == 0) {
		*truncated |= 2;                /* end of file, the caller sees it */
	}
}

static void
run_command(uint64_t id, const char *command)
{
	int p[2];
	pid_t pid;
	char out[VAYU_CTL_SH_MAX];
	size_t out_len = 0;
	int truncated = 0, status = 0, timed_out = 0, eof = 0;
	int64_t deadline = now_ms() + (int64_t)timeout_seconds * 1000;

	if (pipe(p) != 0) {
		send_msg(VAYU_CTL_MSG_DONE, id, errno != 0 ? errno : EIO, 0, NULL, NULL, 0);
		return;
	}
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		send_msg(VAYU_CTL_MSG_DONE, id, errno != 0 ? errno : EIO, 0, NULL, NULL, 0);
		return;
	}
	if (pid == 0) {
		int nul;

		setpgid(0, 0);
		close(p[0]);
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[1]);
		if ((nul = open("/dev/null", O_RDONLY)) >= 0) {
			dup2(nul, 0);
			close(nul);
		}
		setenv("PATH", search_path, 1);
		if (shell_path != NULL) {
			execl(shell_path, "sh", "-c", command, (char *)NULL);
			_exit(127);
		} else {
			char *argv[4];

			argv[0] = (char *)"sh";
			argv[1] = (char *)"-c";
			argv[2] = (char *)command;
			argv[3] = NULL;
			_exit(minish_main(3, argv));
		}
	}
	setpgid(pid, pid);
	close(p[1]);
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);

	for (;;) {
		struct pollfd pfd = { .fd = p[0], .events = POLLIN };
		int64_t left = deadline - now_ms();
		int r;

		if (left < 0) {
			left = 0;
		}
		r = poll(&pfd, 1, (int)left);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r > 0) {
			collect_output(p[0], out, sizeof(out), &out_len, &truncated);
			if ((truncated & 2) != 0) {
				eof = 1;
				break;
			}
		}
		if (now_ms() >= deadline) {
			kill(-pid, SIGKILL);
			kill(pid, SIGKILL);
			timed_out = 1;
			break;
		}
	}
	if (!eof) {
		/* what was still in the pipe when the child was killed or the command ended */
		for (int i = 0; i < 64; i++) {
			collect_output(p[0], out, sizeof(out), &out_len, &truncated);
			if ((truncated & 2) != 0) {
				break;
			}
			usleep(5000);
		}
	}
	close(p[0]);
	status = 137 << 8;
	for (int i = 0; i < 200; i++) {
		pid_t w = waitpid(pid, &status, WNOHANG);

		if (w == pid || (w < 0 && errno == ECHILD)) {
			break;
		}
		usleep(5000);
	}
	if (timed_out) {
		status = 124 << 8;
		/*
		 * Say it in the answer itself: an exit status alone is invisible to the host (the
		 * fastboot reply's numbers were all it had before 2.4), and "the command timed out"
		 * is the one sentence a user of "xl sh" needs.
		 */
		if (out_len < sizeof(out)) {
			int n = snprintf(out + out_len, sizeof(out) - out_len,
			    "\nusbctld: the command timed out after %d s\n", timeout_seconds);

			if (n > 0 && (size_t)n < sizeof(out) - out_len) {
				out_len += (size_t)n;
			}
		}
	}
	note("usbctld: sh %s: %u bytes%s, status %d\n", command, (unsigned)out_len,
	    (truncated & 1) ? " (truncated)" : "",
	    WIFEXITED(status) ? WEXITSTATUS(status) : (timed_out ? 124 : 137));

	{
		size_t sent = 0;

		while (sent < out_len) {
			size_t take = out_len - sent > SH_CHUNK ? SH_CHUNK : out_len - sent;

			if (send_msg(VAYU_CTL_MSG_OUT, id, 0, 0, NULL, out + sent, (uint32_t)take) != 0) {
				return;
			}
			sent += take;
		}
	}
	send_msg(VAYU_CTL_MSG_DONE, id, WIFEXITED(status) ? WEXITSTATUS(status) : (timed_out ? 124 : 137),
	    0, NULL, NULL, 0);
}

/* ------------------------------------------------------------------------------- pull */

static void
run_pull(uint64_t id, const char *path, uint64_t offset, uint64_t max)
{
	static uint8_t data[PULL_CHUNK];
	struct stat st;
	uint64_t total, sent = 0;
	int f;
	uint64_t numbers[2];

	if (max == 0 || max > VAYU_CTL_PULL_MAX) {
		max = VAYU_CTL_PULL_MAX;
	}
	f = open(path, O_RDONLY);
	if (f < 0) {
		numbers[0] = 0;
		numbers[1] = 1;
		send_msg(VAYU_CTL_MSG_PULLDONE, id, -1000 - errno, 0, numbers, NULL, 0);
		return;
	}
	if (fstat(f, &st) != 0) {
		int e = errno;

		close(f);
		numbers[0] = 0;
		numbers[1] = 1;
		send_msg(VAYU_CTL_MSG_PULLDONE, id, -1000 - e, 0, numbers, NULL, 0);
		return;
	}
	if (!S_ISREG(st.st_mode)) {
		close(f);
		numbers[0] = 0;
		numbers[1] = 1;
		send_msg(VAYU_CTL_MSG_PULLDONE, id, -1000 - EISDIR, 0, numbers, NULL, 0);
		return;
	}
	total = (uint64_t)st.st_size;
	if (offset > total) {
		offset = total;
	}
	if (lseek(f, (off_t)offset, SEEK_SET) < 0) {
		int e = errno;

		close(f);
		numbers[0] = total;
		numbers[1] = 1;
		send_msg(VAYU_CTL_MSG_PULLDONE, id, -1000 - e, 0, numbers, NULL, 0);
		return;
	}
	while (sent < max) {
		size_t want = (size_t)(max - sent > PULL_CHUNK ? PULL_CHUNK : max - sent);
		ssize_t n = read(f, data, want);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			numbers[0] = total;
			numbers[1] = 1;
			close(f);
			send_msg(VAYU_CTL_MSG_PULLDONE, id, -1000 - errno, 0, numbers, NULL, 0);
			return;
		}
		if (n == 0) {
			break;
		}
		if (send_msg(VAYU_CTL_MSG_PULLDATA, id, 0, offset + sent, NULL, data, (uint32_t)n) != 0) {
			close(f);
			return;
		}
		sent += (uint64_t)n;
	}
	close(f);
	numbers[0] = total;
	numbers[1] = (offset + sent >= total) ? 1 : 0;
	note("usbtld: pull %s: %llu+%llu of %llu\n", path, (unsigned long long)offset,
	    (unsigned long long)sent, (unsigned long long)total);
	(void)send_msg(VAYU_CTL_MSG_PULLDONE, id, 0, 0, numbers, NULL, 0);
}

/* ------------------------------------------------------------------------------- main */

static int
open_device(void)
{
	int fd;

	if (device_fd >= 0) {
		fd = device_fd;
	} else if ((fd = open(VAYU_CTL_DEVICE, O_RDWR)) < 0) {
		return -1;
	}
	device = fd;
	send_msg(VAYU_CTL_MSG_HELLO, 0, 0, 0, NULL, NULL, 0);
	return 0;
}

static int
run_local(const char *command)
{
	char *argv[4];

	if (shell_path != NULL) {
		execl(shell_path, "sh", "-c", command, (char *)NULL);
		return 127;
	}
	argv[0] = (char *)"sh";
	argv[1] = (char *)"-c";
	argv[2] = (char *)command;
	argv[3] = NULL;
	return minish_main(3, argv);
}

int
main(int argc, char **argv)
{
	const char *command = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			timeout_seconds = atoi(argv[++i]);
			if (timeout_seconds < 1) {
				timeout_seconds = 1;
			}
		} else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			shell_path = argv[++i];
			if (strcmp(shell_path, "-") == 0) {
				shell_path = NULL;      /* the built-in minish, even where /bin/sh exists */
			}
		} else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			command = argv[++i];
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			device_fd = atoi(argv[++i]);
		} else {
			fprintf(stderr, "usage: usbctld [-v] [-t SECONDS] [-s SHELL] [-d FD] [-c 'command']\n");
			return 2;
		}
	}
	if (shell_path == NULL) {
		shell_path = find_shell();
	}
	note("usbctld: shell %s, timeout %d s\n", shell_path != NULL ? shell_path : "minish (built in)",
	    timeout_seconds);
	if (command != NULL) {
		return run_local(command);
	}

	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		union {
			struct vayu_ctl_msg msg;
			uint8_t raw[sizeof(struct vayu_ctl_msg) + VAYU_CTL_TEXT_MAX];
		} u;
		ssize_t n;

		if (device < 0 && open_device() != 0) {
			note("usbctld: %s: %s (waiting)\n", VAYU_CTL_DEVICE, strerror(errno));
			sleep(2);
			continue;
		}
		n = read(device, &u.raw, sizeof(u.raw));
		if (n < (ssize_t)sizeof(struct vayu_ctl_msg)) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
				continue;
			}
			/* the device went away (the kernel came back to the loader): open it again later */
			close(device);
			device = -1;
			sleep(1);
			continue;
		}
		if (u.msg.magic != VAYU_CTL_MSG_MAGIC || u.msg.type != VAYU_CTL_MSG_CMD) {
			continue;
		}
		{
			char text[VAYU_CTL_TEXT_MAX + 1];
			uint32_t len = u.msg.len;

			memset(text, 0, sizeof(text));
			if (len > VAYU_CTL_TEXT_MAX) {
				len = VAYU_CTL_TEXT_MAX;
			}
			if (n > (ssize_t)sizeof(struct vayu_ctl_msg)) {
				uint32_t got = (uint32_t)(n - sizeof(struct vayu_ctl_msg));

				if (got > len) {
					got = len;
				}
				memcpy(text, u.raw + sizeof(struct vayu_ctl_msg), got);
			}
			switch (u.msg.cmd) {
			case VAYU_CTL_CMD_SH:
				run_command(u.msg.id, text);
				break;
			case VAYU_CTL_CMD_PULL:
				run_pull(u.msg.id, text, u.msg.arg[0], u.msg.arg[1]);
				break;
			default:
				send_msg(VAYU_CTL_MSG_DONE, u.msg.id, VAYU_CTL_ERR_ARGS, 0, NULL, NULL, 0);
				break;
			}
		}
	}
}
