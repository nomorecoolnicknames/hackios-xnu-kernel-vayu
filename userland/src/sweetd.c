/*
 * sweetd: a tiny remote command runner for the phone's iOS userland -- a diagnostic channel for the test bench
 * with nobody at the phone (a run's sysctl, launchctl, an interface's state, /redwood/bin/touch for a tap or a
 * button, a file onto the phone for phone-mode.sh deploy). Not the way iOS is meant to be used: a stand-in until
 * the native services can be driven from the host.
 *
 * Only with sweetd=1 on the kernel's command line (without it the program exits at once, and launchd leaves it
 * be: com.hakios.sweetd.plist restarts it only after a failure). It listens on TCP 5143 bound to en2 alone
 * (IP_BOUND_IF: the kernel's USB network, bsd/dev/vayu_usbnet_ecm.c, 10.11.0.2/24; nothing from Wi-Fi or the
 * cell reaches it), serves peers of 10.11.0.0/24 alone, and runs nothing without the token build.sh put in
 * the image (/redwood/etc/sweetd.token, from userland/sweetd.token: made on the build machine, never in git).
 * One command per connection:
 *
 *   request:  "RUN <token> <timeout-seconds> <argc>\n" then argc lines, one
 *             argument each (argv[0] is an absolute path)
 *   reply:    the command's stdout and stderr, merged, as they come, then
 *             "\n" SWEETD_END " <status>\n" where status is the exit code,
 *             -SIGNAL when killed, or -1000 when it could not be started
 *
 * argv[0] without a slash is one of the built-in commands below instead (iOS has no ls or cat): ls, cat, sysctl,
 * put (a file from the client: "put PATH MODE SIZE", then SIZE bytes after the argument lines), rm, mkdir, kill,
 * sha256 (PATH [BYTES]: of the first BYTES, or all; a device such as /dev/rufs_boot as well as a file).
 *
 * The client is devices/poco-x3-pro/tools/sweet-sh.py (phone-mode.sh sh/put). Clients are served one at a time.
 *
 * From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 80542801 (its userland/src/sweetd.c, from
 * ios-jasmine's ios/sweetd.c, MIT); here bound to en2, behind sweetd=1, the token from the image alone, sha256.
 */
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT            5143
#define MAX_ARGS        64
#define MAX_LINE        4096
#define SWEETD_END      "__SWEETD_END__"
#define TOKEN_IMAGE     "/redwood/etc/sweetd.token"
#define INTERFACE       "en2"

extern char **environ;

static int
read_line(int fd, char *buf, size_t max)
{
	size_t n = 0;
	while (n < max - 1) {
		char c;
		ssize_t r = read(fd, &c, 1);
		if (r <= 0) {
			return -1;
		}
		if (c == '\n') {
			break;
		}
		buf[n++] = c;
	}
	buf[n] = 0;
	return (int)n;
}

static void
send_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t w = write(fd, buf, len);
		if (w <= 0) {
			return;
		}
		buf += w;
		len -= (size_t)w;
	}
}

/* The token from TOKEN_IMAGE (first line, at most 64 characters), or "" when there is none. */
static void
load_token(char *out, size_t max)
{
	out[0] = 0;
	FILE *f = fopen(TOKEN_IMAGE, "r");
	if (f == NULL) {
		return;
	}
	if (fgets(out, (int)max, f) == NULL) {
		out[0] = 0;
	}
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
}

/* Constant-time comparison, so the token cannot be found byte by byte. */
static bool
token_ok(const char *got, const char *want)
{
	size_t n = strlen(want);
	if (n < 16 || strlen(got) != n) {
		return false;
	}
	unsigned char d = 0;
	for (size_t i = 0; i < n; i++) {
		d |= (unsigned char)(got[i] ^ want[i]);
	}
	return d == 0;
}

static int
bi_ls(int cs, const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *e;
	char full[1024], link[1024];

	if (lstat(path, &st) != 0) {
		dprintf(cs, "ls: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (!S_ISDIR(st.st_mode)) {
		dprintf(cs, "%06o %5u %5u %10lld %s\n", st.st_mode, st.st_uid, st.st_gid, (long long)st.st_size, path);
		return 0;
	}
	dir = opendir(path);
	if (dir == NULL) {
		dprintf(cs, "ls: %s: %s\n", path, strerror(errno));
		return 1;
	}
	while ((e = readdir(dir)) != NULL) {
		ssize_t n;

		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		if (lstat(full, &st) != 0) {
			dprintf(cs, "?????? %s: %s\n", e->d_name, strerror(errno));
			continue;
		}
		dprintf(cs, "%06o %5u %5u %10lld %s", st.st_mode, st.st_uid, st.st_gid, (long long)st.st_size, e->d_name);
		if (S_ISLNK(st.st_mode) && (n = readlink(full, link, sizeof(link) - 1)) >= 0) {
			link[n] = 0;
			dprintf(cs, " -> %s", link);
		}
		dprintf(cs, "\n");
	}
	closedir(dir);
	return 0;
}

static int
bi_cat(int cs, const char *path)
{
	char buf[16384];
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		dprintf(cs, "cat: %s: %s\n", path, strerror(errno));
		return 1;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		send_all(cs, buf, (size_t)n);
	}
	close(fd);
	return n < 0 ? 1 : 0;
}

static int
bi_sysctl(int cs, const char *name)
{
	unsigned char buf[4096];
	size_t len = sizeof(buf) - 1;
	bool text = true;

	if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
		dprintf(cs, "sysctl: %s: %s\n", name, strerror(errno));
		return 1;
	}
	for (size_t i = 0; i < len; i++) {
		if ((buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\t' && !(buf[i] == 0 && i == len - 1)) || buf[i] > 0x7e) {
			text = false;
		}
	}
	if (len == 4 && !text) {
		dprintf(cs, "%s: %d\n", name, *(int *)buf);
	} else if (len == 8 && !text) {
		dprintf(cs, "%s: %lld\n", name, *(long long *)buf);
	} else if (text) {
		buf[len] = 0;
		dprintf(cs, "%s: %s\n", name, (char *)buf);
	} else {
		dprintf(cs, "%s: %zu bytes:", name, len);
		for (size_t i = 0; i < len && i < 256; i++) {
			dprintf(cs, " %02x", buf[i]);
		}
		dprintf(cs, "\n");
	}
	return 0;
}

/* "put PATH MODE SIZE": SIZE bytes from the client after the argument lines, into PATH (made, or emptied) */
static int
bi_put(int cs, const char *path, const char *mode, const char *size)
{
	long long left = strtoll(size, NULL, 10), got = 0;
	char buf[16384];
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)strtol(mode, NULL, 8));
	bool ok = fd >= 0;

	if (!ok) {
		dprintf(cs, "put: %s: %s\n", path, strerror(errno));
	}
	while (left > 0) {
		ssize_t n = read(cs, buf, left < (long long)sizeof(buf) ? (size_t)left : sizeof(buf));

		if (n <= 0) {
			dprintf(cs, "put: the client stopped after %lld bytes\n", got);
			ok = false;
			break;
		}
		if (fd >= 0 && write(fd, buf, (size_t)n) != n) {
			dprintf(cs, "put: %s: %s\n", path, strerror(errno));
			close(fd);
			fd = -1;
			ok = false;
		}
		left -= n;
		got += n;
	}
	if (fd >= 0) {
		fchmod(fd, (mode_t)strtol(mode, NULL, 8));
		if (fsync(fd) != 0 || close(fd) != 0) {
			ok = false;
		}
	}
	if (ok) {
		dprintf(cs, "put: %s: %lld bytes\n", path, got);
	}
	return ok ? 0 : 1;
}

/* "sha256 PATH [BYTES]": the sum of the first BYTES of PATH (all of it without), and how many were read */
static int
bi_sha256(int cs, const char *path, const char *limit)
{
	static unsigned char buf[1 << 20];
	unsigned char md[CC_SHA256_DIGEST_LENGTH];
	long long want = limit != NULL ? strtoll(limit, NULL, 10) : -1, got = 0;
	CC_SHA256_CTX ctx;
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		dprintf(cs, "sha256: %s: %s\n", path, strerror(errno));
		return 1;
	}
	CC_SHA256_Init(&ctx);
	while (want < 0 || got < want) {
		size_t k = want < 0 || want - got > (long long)sizeof(buf) ? sizeof(buf) : (size_t)(want - got);

		n = read(fd, buf, k);
		if (n <= 0) {
			break;
		}
		CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
		got += n;
	}
	close(fd);
	if (n < 0 || (want >= 0 && got != want)) {
		dprintf(cs, "sha256: %s: %lld bytes read, then %s\n", path, got, n < 0 ? strerror(errno) : "its end");
		return 1;
	}
	CC_SHA256_Final(md, &ctx);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
		dprintf(cs, "%02x", md[i]);
	}
	dprintf(cs, "  %lld %s\n", got, path);
	return 0;
}

/* A built-in command (argv[0] has no slash): its status, or -2 when there is no such built-in. */
static int
builtin(int cs, int argc, char **argv)
{
	int status = 0;

	if (strcmp(argv[0], "ls") == 0) {
		if (argc == 1) {
			return bi_ls(cs, "/");
		}
		for (int i = 1; i < argc; i++) {
			if (argc > 2) {
				dprintf(cs, "%s:\n", argv[i]);
			}
			status |= bi_ls(cs, argv[i]);
		}
		return status;
	}
	if (strcmp(argv[0], "cat") == 0) {
		for (int i = 1; i < argc; i++) {
			status |= bi_cat(cs, argv[i]);
		}
		return status;
	}
	if (strcmp(argv[0], "sysctl") == 0) {
		for (int i = 1; i < argc; i++) {
			status |= bi_sysctl(cs, argv[i]);
		}
		return status;
	}
	if (strcmp(argv[0], "put") == 0) {
		if (argc != 4) {
			dprintf(cs, "put: put PATH MODE SIZE\n");
			return 2;
		}
		return bi_put(cs, argv[1], argv[2], argv[3]);
	}
	if (strcmp(argv[0], "rm") == 0 || strcmp(argv[0], "mkdir") == 0) {
		bool rm = argv[0][0] == 'r';

		for (int i = 1; i < argc; i++) {
			if ((rm ? (unlink(argv[i]) != 0 && rmdir(argv[i]) != 0) : mkdir(argv[i], 0755) != 0)) {
				dprintf(cs, "%s: %s: %s\n", argv[0], argv[i], strerror(errno));
				status = 1;
			}
		}
		return status;
	}
	if (strcmp(argv[0], "sha256") == 0) {
		if (argc != 2 && argc != 3) {
			dprintf(cs, "sha256: sha256 PATH [BYTES]\n");
			return 2;
		}
		return bi_sha256(cs, argv[1], argc == 3 ? argv[2] : NULL);
	}
	if (strcmp(argv[0], "kill") == 0) {
		int sig = SIGTERM, i = 1;

		if (argc > 2 && argv[1][0] == '-') {
			sig = atoi(argv[1] + 1);
			i = 2;
		}
		for (; i < argc; i++) {
			if (kill(atoi(argv[i]), sig) != 0) {
				dprintf(cs, "kill: %s: %s\n", argv[i], strerror(errno));
				status = 1;
			}
		}
		return status;
	}
	return -2;
}

static void
serve(int cs)
{
	char line[MAX_LINE], token[80], want[80];
	char *argv[MAX_ARGS + 1];
	int timeout = 0, argc = 0;

	load_token(want, sizeof(want));
	if (read_line(cs, line, sizeof(line)) < 0 || sscanf(line, "RUN %79s %d %d", token, &timeout, &argc) != 3 ||
	    argc < 1 || argc > MAX_ARGS) {
		dprintf(cs, "bad request\n%s -1000\n", SWEETD_END);
		return;
	}
	if (!token_ok(token, want)) {
		dprintf(cs, "sweetd: wrong or missing token\n%s -1000\n", SWEETD_END);
		return;
	}
	for (int i = 0; i < argc; i++) {
		if (read_line(cs, line, sizeof(line)) < 0) {
			return;
		}
		argv[i] = strdup(line);
	}
	argv[argc] = NULL;

	if (strchr(argv[0], '/') == NULL) {
		int status = builtin(cs, argc, argv);

		if (status == -2) {
			dprintf(cs, "sweetd: no built-in %s (ls cat sysctl put rm mkdir kill sha256); a program wants its path\n",
			    argv[0]);
			status = -1000;
		}
		dprintf(cs, "\n%s %d\n", SWEETD_END, status);
		for (int i = 0; i < argc; i++) {
			free(argv[i]);
		}
		return;
	}

	int out[2];
	pid_t pid = -1;
	int status = -1000;
	if (pipe(out) == 0) {
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
		posix_spawn_file_actions_adddup2(&fa, out[1], 1);
		posix_spawn_file_actions_adddup2(&fa, out[1], 2);
		posix_spawn_file_actions_addclose(&fa, out[0]);
		int err = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
		posix_spawn_file_actions_destroy(&fa);
		close(out[1]);
		if (err != 0) {
			dprintf(cs, "sweetd: cannot start %s: %s\n", argv[0], strerror(err));
			pid = -1;
		}
		time_t deadline = time(NULL) + (timeout > 0 ? timeout : 3600);
		bool killed = false;
		for (;;) {
			struct pollfd p = { .fd = out[0], .events = POLLIN };
			int r = poll(&p, 1, 500);
			if (r > 0) {
				char buf[4096];
				ssize_t n = read(out[0], buf, sizeof(buf));
				if (n <= 0) {
					break;
				}
				send_all(cs, buf, (size_t)n);
			}
			if (pid > 0 && !killed && time(NULL) > deadline) {
				kill(pid, SIGKILL);
				killed = true;
				dprintf(cs, "\nsweetd: timeout after %d s, killed\n", timeout);
			}
			if (pid <= 0 && r == 0) {
				break;
			}
		}
		close(out[0]);
		if (pid > 0) {
			int st = 0;
			if (waitpid(pid, &st, 0) == pid) {
				status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? -WTERMSIG(st) : -1000;
			}
		}
	}
	dprintf(cs, "\n%s %d\n", SWEETD_END, status);
	for (int i = 0; i < argc; i++) {
		free(argv[i]);
	}
}

/* sweetd=1 among the kernel's arguments (a word of its own) */
static bool
asked_for(void)
{
	static char args[4096];
	size_t len = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return false;
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	for (char *p = strtok(args, " \t"); p != NULL; p = strtok(NULL, " \t")) {
		if (strcmp(p, "sweetd=1") == 0) {
			return true;
		}
	}
	return false;
}

int
main(void)
{
	struct sockaddr_in a = { .sin_len = sizeof(a), .sin_family = AF_INET, .sin_port = htons(PORT),
		                 .sin_addr.s_addr = htonl(INADDR_ANY) };
	struct timeval patience = { .tv_sec = 30 };
	char want[80];
	int ls = -1, on = 1;
	unsigned index;

	if (!asked_for()) {
		return 0;       /* (no sweetd=1: nothing listens; the job's KeepAlive lets a clean exit be) */
	}
	load_token(want, sizeof(want));
	if (strlen(want) < 16) {
		fprintf(stderr, "sweetd: no token in the image (%s): nothing would run; not listening\n", TOKEN_IMAGE);
		return 0;
	}
	signal(SIGPIPE, SIG_IGN);
	/* en2 comes with the kernel's USB network, some seconds into the boot: until then, and until it binds, wait */
	for (;;) {
		index = if_nametoindex(INTERFACE);
		if (index != 0 && (ls = socket(AF_INET, SOCK_STREAM, 0)) >= 0) {
			setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
			if (setsockopt(ls, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) == 0 &&
			    bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(ls, 4) == 0) {
				break;
			}
			close(ls);
		}
		sleep(2);
	}
	fprintf(stderr, "sweetd: listening on TCP %d of %s alone (sweetd=1)\n", PORT, INTERFACE);
	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int cs = accept(ls, (struct sockaddr *)&peer, &plen);
		if (cs < 0) {
			continue;
		}
		/* (a client that says nothing, or went away with the cable, frees the next one in 30 s) */
		setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
		setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof(patience));
		/* the host at the other end of the cable only */
		uint32_t pa = ntohl(peer.sin_addr.s_addr);
		if (peer.sin_family == AF_INET && (pa >> 8) == 0x0a0b00) {
			serve(cs);
		}
		close(cs);
	}
}
