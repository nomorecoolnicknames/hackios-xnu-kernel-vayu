/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * nbd27_test: the kernel's /dev/nbd0 and /dev/nbd1 (bsd/dev/vayu_nbd.c, vayu27=1 nbd=1) seen from user space, as
 * process 1 of mockfs (the ramdisk is this one program, /sbin/launchd; mockfs has /dev, so devfs is there). Run by
 * tools/vayu/tests/k6/run-nbd27.sh in QEMU against nbd-server.py on the host; on the phone it can stand in for
 * launchd (it holds pid 1 for nbd_test_hold= seconds).
 *
 * Origin: the repository's (627bbf46) userland/src/nbd_test.c (its SHA-256 and boot_arg(); libmini). No file system
 * is mounted (mockfs has no directory to mount on); the steps, each said "ok" or "FAILED":
 *   - nbd_test_expect=nxio: the open must fail with ENXIO (an export the server does not have), and that is all;
 *   - open /dev/rnbd0 (it waits for the first connection), read its first nbd_test_mb MiB (default 100): SHA-256;
 *   - the disk is read only without nbd_rw=1: an open for writing must fail with EROFS; with nbd_rw=1
 *     nbd_test_write=1 (the server with --cow) 4 KiB are written at 8 KiB (within the 64 KiB the kernel checks on a
 *     reconnection) and at 2 MiB, and read back;
 *   - its size (DKIOCGETBLOCKCOUNT x DKIOCGETBLOCKSIZE), and a read at the end: 0 bytes;
 *   - nbd_test_pause=S: "nbd_test: pause" and S seconds of sleep (the script changes the server meanwhile), then the
 *     first nbd_test_mb2 MiB (default 8) again: nbd_test_after=same (default) wants the same SHA-256 as the same
 *     bytes before, =eio wants an I/O error (the kernel refused another export: nbd_giveup= ends its wait);
 *   - nbd1=1: /dev/rnbd1's first nbd_test_mb1 MiB (default 8): SHA-256;
 *   - nbd_test_root=1 (a RAM root of HFS Plus with /flash, not mockfs): the export, an HFS Plus volume, mounted read only
 *     from /dev/nbd0 on /flash, /flash/test.bin's SHA-256; then kern.hakios_switch_root "/flash old": the volume is the
 *     root, /test.bin the same SHA-256, the RAM root under /old (its /old/sbin/launchd), a second switch refused (EBUSY);
 *   - nbd_test_hold=S: pid 1 stays S seconds more (a line every 30 s);
 * then "nbd_test: PASS" or "FAIL" and process 1 ends (the kernel panics on that: initproc exited).
 */
#include "mini.h"

#define CHUNK   (1u << 20)
#define EROFS_  30
#define ENXIO_  6
#define EIO_    5

#define DKIOCGETBLOCKSIZE       0x40046418UL    /* _IOR('d', 24, uint32_t) */
#define DKIOCGETBLOCKCOUNT      0x40086419UL    /* _IOR('d', 25, uint64_t) */
#define DKIOCSYNCHRONIZECACHE   0x20006416UL    /* _IO('d', 22): the driver's NBD_CMD_FLUSH */

static unsigned char chunk[CHUNK];
static int failures;

static void
check(int ok, const char *what)
{
	printf("nbd_test: %s: %s\n", ok ? "ok" : "FAILED", what);
	failures += !ok;
}

static const char *
boot_arg(const char *name)
{
	static char args[4096], value[64];
	size_t len = sizeof(args) - 1, n = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return NULL;
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	for (char *p = args; *p;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, name, n) == 0 && p[n] == '=') {
			size_t i = 0;

			for (p += n + 1; *p && *p != ' ' && i < sizeof(value) - 1; p++) {
				value[i++] = *p;
			}
			value[i] = 0;
			return value;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return NULL;
}

static unsigned long
number(const char *text, unsigned long otherwise)
{
	unsigned long n = 0;

	if (text == NULL || *text == 0) {
		return otherwise;
	}
	for (; *text >= '0' && *text <= '9'; text++) {
		n = n * 10 + (unsigned long)(*text - '0');
	}
	return n;
}

/* ---- SHA-256 (FIPS 180-4), as nbd_test.c's ---- */

struct sha256 {
	uint32_t h[8];
	uint64_t length;
	unsigned char block[64];
	unsigned used;
};

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n)       (((x) >> (n)) | ((x) << (32 - (n))))

static void
sha256_block(struct sha256 *s, const unsigned char *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;

	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

		h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
	}
	s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void
sha256_init(struct sha256 *s)
{
	static const uint32_t h0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(s->h, h0, sizeof(h0));
	s->length = 0;
	s->used = 0;
}

static void
sha256_update(struct sha256 *s, const unsigned char *p, size_t n)
{
	s->length += n;
	while (n > 0) {
		if (s->used == 0 && n >= 64) {
			sha256_block(s, p);
			p += 64, n -= 64;
			continue;
		}
		s->block[s->used++] = *p++;
		n--;
		if (s->used == 64) {
			sha256_block(s, s->block);
			s->used = 0;
		}
	}
}

static void
sha256_final(struct sha256 *s, char *text)
{
	uint64_t bits = s->length * 8;
	unsigned char pad = 0x80, zero = 0, length[8];

	sha256_update(s, &pad, 1);
	while (s->used != 56) {
		sha256_update(s, &zero, 1);
	}
	for (int i = 0; i < 8; i++) {
		length[i] = (unsigned char)(bits >> (56 - 8 * i));
	}
	sha256_update(s, length, 8);
	for (int i = 0; i < 32; i++) {
		static const char hex[] = "0123456789abcdef";
		unsigned char byte = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i % 4)));

		text[2 * i] = hex[byte >> 4];
		text[2 * i + 1] = hex[byte & 15];
	}
	text[64] = 0;
}

static long
now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* the first `most` bytes of an open device through the sum; how many were read, -errno on an error */
static long
sum_fd(int fd, unsigned long most, char *text)
{
	struct sha256 s;
	unsigned long total = 0;

	sha256_init(&s);
	while (total < most) {
		size_t want = most - total < CHUNK ? (size_t)(most - total) : CHUNK;
		ssize_t n = pread(fd, chunk, want, (off_t)total);

		if (n < 0) {
			printf("nbd_test: read at %lu: %s\n", total, strerror(errno));
			return -errno;
		}
		if (n == 0) {
			break;
		}
		sha256_update(&s, chunk, (size_t)n);
		total += (unsigned long)n;
	}
	sha256_final(&s, text);
	return (long)total;
}

/* one device's first mb MiB: "nbd_test: NAME raw BYTES sha256 SUM in MS ms (KB/s)"; the count, or -errno */
static long
raw_read(int fd, const char *name, unsigned long mb, char *sum)
{
	long started = now_ms(), got, ms;

	got = sum_fd(fd, mb << 20, sum);
	ms = now_ms() - started;
	if (got >= 0) {
		printf("nbd_test: %s raw %ld bytes sha256 %s in %ld ms (%ld KB/s)\n", name, got, sum, ms,
		    ms > 0 ? got / ms * 1000 / 1024 : 0);
	}
	return got;
}

static int
dk_ioctl(int fd, unsigned long request, void *data)
{
	return (int)mini_syscall(SYS_ioctl, fd, (long)request, (long)data, 0, 0, 0);
}

static void
nbd0(void)
{
	unsigned long mb = number(boot_arg("nbd_test_mb"), 100), mb2 = number(boot_arg("nbd_test_mb2"), 8);
	unsigned long pause = number(boot_arg("nbd_test_pause"), 0);
	char after_text[16] = "";
	const char *after = boot_arg("nbd_test_after");
	int rw = number(boot_arg("nbd_rw"), 0) != 0;
	char sum[65], sum2[65], before2[65];
	uint32_t bsize = 0;
	uint64_t blocks = 0;
	long started = now_ms(), got;
	int fd, wfd;

	if (after != NULL) {
		strcpy(after_text, after);      /* (boot_arg's answer is its own buffer: the next call overwrites it) */
		after = after_text;
	}
	fd = open("/dev/rnbd0", O_RDONLY, 0);       /* (waits for the first connection: nbd_wait) */
	check(fd >= 0, "open /dev/rnbd0");
	if (fd < 0) {
		printf("nbd_test: /dev/rnbd0: %s\n", strerror(errno));
		return;
	}
	printf("nbd_test: /dev/rnbd0 open after %ld ms\n", now_ms() - started);
	got = raw_read(fd, "nbd0", mb, sum);
	check(got == (long)(mb << 20), "the whole of the first MiBs read");

	wfd = open("/dev/rnbd0", O_RDWR, 0);
	if (rw) {
		check(wfd >= 0, "open /dev/rnbd0 for writing (nbd_rw=1)");
	} else {
		check(wfd < 0 && errno == EROFS_, "open /dev/rnbd0 for writing refused with EROFS (no nbd_rw=1)");
	}
	if (wfd >= 0 && number(boot_arg("nbd_test_write"), 0) != 0) {
		static unsigned char pattern[4096], back[4096];
		static const off_t where[2] = { 8192, 2 << 20 };

		for (unsigned i = 0; i < sizeof(pattern); i++) {
			pattern[i] = (unsigned char)(i * 7 + 0x5a);
		}
		for (int w = 0; w < 2; w++) {
			int ok = pwrite(wfd, pattern, sizeof(pattern), where[w]) == (ssize_t)sizeof(pattern) &&
			    pread(fd, back, sizeof(back), where[w]) == (ssize_t)sizeof(back) &&
			    memcmp(back, pattern, sizeof(back)) == 0;

			check(ok, w == 0 ? "4 KiB written at 8 KiB and read back" : "4 KiB written at 2 MiB and read back");
		}
		/* (the server's overlay on its disk before the script restarts it) */
		check(dk_ioctl(wfd, DKIOCSYNCHRONIZECACHE, NULL) == 0, "the writes flushed (DKIOCSYNCHRONIZECACHE)");
	}
	if (wfd >= 0) {
		close(wfd);
	}

	check(dk_ioctl(fd, DKIOCGETBLOCKSIZE, &bsize) == 0 && dk_ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) == 0 &&
	    bsize != 0 && blocks != 0, "its block size and count");
	printf("nbd_test: nbd0 blocks of %u, %llu of them (%llu bytes)\n", bsize, (unsigned long long)blocks,
	    (unsigned long long)blocks * bsize);
	if (bsize != 0 && blocks != 0) {
		check(pread(fd, chunk, 4096, (off_t)(blocks * bsize)) == 0, "a read at the end: 0 bytes");
	}

	if (pause != 0) {
		int want_eio = after != NULL && strcmp(after, "eio") == 0;

		/* (the same first mb2 MiB as the sum to compare with, read now while the server is the first one) */
		got = raw_read(fd, "nbd0-before", mb2, before2);
		check(got == (long)(mb2 << 20), "the first MiBs for the second look");
		printf("nbd_test: pause %lu s\n", pause);
		usleep((unsigned)(pause * 1000000));
		got = raw_read(fd, "nbd0-after", mb2, sum2);
		if (want_eio) {
			check(got == -EIO_, "after the pause: an I/O error (another export, refused)");
		} else {
			check(got == (long)(mb2 << 20) && strcmp(sum2, before2) == 0, "after the pause: the same bytes");
		}
	}
	close(fd);
}

/* struct hfs_mount_args as user space passes it (bsd/hfs/hfs_mount.h; nbd_test.c's) */
struct hfs_args {
	const char *fspec;
	uint32_t uid, gid;
	uint16_t mask, pad;
	uint32_t encoding;
	int tz[2];
	int flags, journal_tbuffer_size, journal_flags, journal_disable;
};

static long
file_sum(const char *path, char *sum)
{
	struct stat st;
	long got;
	int fd;

	if (stat(path, &st) != 0 || (fd = open(path, O_RDONLY, 0)) < 0) {
		printf("nbd_test: %s: %s\n", path, strerror(errno));
		return -1;
	}
	got = sum_fd(fd, (unsigned long)st.st_size, sum);
	close(fd);
	if (got >= 0) {
		printf("nbd_test: file %s %ld bytes sha256 %s\n", path, got, sum);
	}
	return got == (long)st.st_size ? got : -1;
}

static void
root_switch(void)
{
	static const char request[] = "/flash old";
	struct hfs_args args;
	struct stat st;
	char before[65], after[65], text[64];
	int ok, fd;
	long n;

	memset(&args, 0, sizeof(args));
	args.fspec = "/dev/nbd0";
	ok = mount("hfs", "/flash", MNT_RDONLY, &args) == 0;
	if (!ok) {
		printf("nbd_test: mount hfs /dev/nbd0 on /flash: %s (%d)\n", strerror(errno), errno);
	}
	check(ok, "HFS Plus from /dev/nbd0 on /flash, read only");
	if (!ok) {
		return;
	}
	check(file_sum("/flash/test.bin", before) > 0, "/flash/test.bin read, all of it");
	ok = sysctlbyname("kern.hakios_switch_root", NULL, NULL, request, sizeof(request) - 1) == 0;
	if (!ok) {
		printf("nbd_test: kern.hakios_switch_root: %s (%d)\n", strerror(errno), errno);
	}
	check(ok, "kern.hakios_switch_root \"/flash old\"");
	if (!ok) {
		return;
	}
	check(file_sum("/test.bin", after) > 0 && strcmp(before, after) == 0, "/test.bin on the new root: the same bytes");
	memset(text, 0, sizeof(text));
	fd = open("/marker.txt", O_RDONLY, 0);
	n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
	if (fd >= 0) {
		close(fd);
	}
	printf("nbd_test: /marker.txt: %s", n > 0 ? text : "(none)\n");
	check(n > 0 && strncmp(text, "the NBD root", 12) == 0, "/marker.txt is the export's");
	check(stat("/old/sbin/launchd", &st) == 0, "the RAM root under /old (/old/sbin/launchd)");
	check(stat("/dev/rnbd0", &st) == 0, "devfs still on /dev");
	ok = sysctlbyname("kern.hakios_switch_root", NULL, NULL, request, sizeof(request) - 1) != 0 && errno == 16;
	check(ok, "a second switch refused (EBUSY)");
	if (number(boot_arg("vayu_hide_ramdisk"), 0) != 0) {
		/* K8 (U3G1): iOS 27's launchd takes "rd=md0" in kern.bootargs for a restore environment */
		static char args[4096];
		size_t len = sizeof(args) - 1;
		int has_rd = 0;

		ok = sysctlbyname("kern.bootargs", args, &len, NULL, 0) == 0;
		args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
		for (char *a = args; ok && *a; a++) {
			if ((a == args || a[-1] == ' ') && strncmp(a, "rd=", 3) == 0) {
				has_rd = 1;
			}
		}
		printf("nbd_test: kern.bootargs after the switch: %s\n", ok ? args : "?");
		check(ok && !has_rd, "kern.bootargs without rd= after the switch (vayu_hide_ramdisk=1)");
	}
}

/*
 * nbd_test_kill=1 (K8, vayu_exitlog=1): a child killed by this process with SIGKILL, and one that sends itself
 * SIGKILL: the kernel's VAYU SIG / VAYU EXIT lines say who sent it.
 */
static void
kill_test(void)
{
	int status = 0;
	pid_t child = fork();

	if (child == 0) {
		for (;;) {
			usleep(1000000);
		}
	}
	usleep(300000);
	check(child > 0 && mini_syscall(SYS_kill, child, 9, 1, 0, 0, 0) == 0, "kill(child, SIGKILL)");
	check(child > 0 && wait4(child, &status, 0, NULL) == child && (status & 0x7f) == 9, "the child ended on SIGKILL");
	child = fork();
	if (child == 0) {
		mini_syscall(SYS_kill, getpid(), 9, 1, 0, 0, 0);
		for (;;) {
			usleep(1000000);
		}
	}
	status = 0;
	check(child > 0 && wait4(child, &status, 0, NULL) == child && (status & 0x7f) == 9,
	    "a child that sent itself SIGKILL ended on it");
}

static void
nbd1(void)
{
	unsigned long mb = number(boot_arg("nbd_test_mb1"), 8);
	char sum[65];
	long got;
	int fd = open("/dev/rnbd1", O_RDONLY, 0);

	check(fd >= 0, "open /dev/rnbd1");
	if (fd < 0) {
		printf("nbd_test: /dev/rnbd1: %s\n", strerror(errno));
		return;
	}
	got = raw_read(fd, "nbd1", mb, sum);
	check(got == (long)(mb << 20), "the whole of nbd1's first MiBs read");
	close(fd);
}

int
main(int argc, char **argv, char **envp)
{
	int fd = open("/dev/console", O_RDWR, 0);
	const char *expect = boot_arg("nbd_test_expect");
	char expect_text[16] = "";
	unsigned long hold;

	if (expect != NULL) {
		strcpy(expect_text, expect);
		expect = expect_text;
	}
	hold = number(boot_arg("nbd_test_hold"), 0);

	(void)argc, (void)argv, (void)envp;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	printf("nbd_test: process %d (12377)\n", getpid());
	if (number(boot_arg("nbd"), 0) == 0) {
		printf("nbd_test: no nbd=1: nothing to read\n");
	} else if (expect != NULL && strcmp(expect, "nxio") == 0) {
		int nfd = open("/dev/rnbd0", O_RDONLY, 0);

		check(nfd < 0 && errno == ENXIO_, "open /dev/rnbd0 refused with ENXIO (no such export)");
		if (nfd >= 0) {
			close(nfd);
		}
	} else {
		nbd0();
		if (number(boot_arg("nbd1"), 0) != 0) {
			nbd1();
		}
		if (number(boot_arg("nbd_test_root"), 0) != 0) {
			root_switch();
		}
	}
	if (number(boot_arg("nbd_test_kill"), 0) != 0) {
		kill_test();
	}
	printf("nbd_test: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
	for (unsigned long waited = 0; waited < hold; waited += 30) {
		usleep(30 * 1000000);
		printf("nbd_test: holding pid 1, %lu of %lu s\n", waited + 30, hold);
	}
	sync();
	return failures != 0;
}
