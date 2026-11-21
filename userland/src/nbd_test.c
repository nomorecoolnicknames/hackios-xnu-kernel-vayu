/*
 * nbd_test: the kernel's /dev/nbd0 (bsd/dev/vayu_nbd.c, boot-arg nbd=1) in QEMU, against tools/nbd/nbd-server.py
 * on the host. Process 1 of a tiny APFS root (/sbin/launchd, /dev, /mnt), started by qemu/run-nbd-test.sh:
 *   - reads the first nbd_test_mb MiB (default 100) of /dev/rnbd0, the character device, and says their SHA-256
 *     and how fast they came;
 *   - mounts the export from /dev/nbd0 (the block device: the driver's strategy, as a file system's reads go) on
 *     /mnt, read only -- an APFS container as tools/apfs/mkapfs.py makes them
 *     (the way init takes a root: root_apfs=, userland/src/init.c flash_root), or with nbd_test_fs=hfs an HFS Plus
 *     volume -- and says the SHA-256 of /mnt/test.bin, a file the script put there, if there is one;
 *   - nbd_test_write=N (HFS Plus, nbd_rw=1): mounts it writable instead, writes /mnt/boot-N.txt, fsyncs and
 *     unmounts; nbd_test_check=N: the file boot N wrote must be there, whole.
 * The script compares what it says with the image's own sums. Then "nbd_test: PASS" (or FAIL) and process 1
 * ends, which ends the run (the kernel panics on it).
 */
#include "mini.h"

#define CHUNK   (1u << 20)

static unsigned char chunk[CHUNK];
static int failures;
static long read_ms;            /* of the last sum_fd: the time in read() alone, not in the sum */

static void
check(int ok, const char *what)
{
	printf("nbd_test: %s: %s\n", ok ? "ok" : "FAILED", what);
	failures += !ok;
}

/* the value of boot-arg NAME, or NULL (as init's) */
static const char *
boot_arg(const char *name)
{
	static char args[2048], value[64];
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

/* ---- SHA-256 (FIPS 180-4) ---- */

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

/* the sum as 64 hex digits in text[65] */
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

/* the whole of an open file through the sum; its length, -1 on an error */
static long
sum_fd(int fd, unsigned long most, char *text)
{
	struct sha256 s;
	unsigned long total = 0;

	sha256_init(&s);
	read_ms = 0;
	while (total < most) {
		size_t want = most - total < CHUNK ? (size_t)(most - total) : CHUNK;
		long before = now_ms();
		ssize_t n = read(fd, chunk, want);

		read_ms += now_ms() - before;

		if (n < 0) {
			printf("nbd_test: read at %lu: %s\n", total, strerror(errno));
			return -1;
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

/* struct hfs_mount_args as user space passes it (bsd/hfs/hfs_mount.h; as userland/src/var_hfs.h) */
struct hfs_args {
	const char *fspec;
	uint32_t uid, gid;
	uint16_t mask, pad;
	uint32_t encoding;
	int tz[2];
	int flags, journal_tbuffer_size, journal_flags, journal_disable;
};

static void
raw_read(void)
{
	unsigned long mb = number(boot_arg("nbd_test_mb"), 100);
	char sum[65];
	long started, ms, got;
	int fd;

	started = now_ms();
	fd = open("/dev/rnbd0", O_RDONLY, 0);       /* (waits for the first connection: nbd_wait) */
	check(fd >= 0, "open /dev/rnbd0");
	if (fd < 0) {
		printf("nbd_test: /dev/rnbd0: %s\n", strerror(errno));
		return;
	}
	printf("nbd_test: /dev/rnbd0 open after %ld ms\n", now_ms() - started);
	started = now_ms();
	got = sum_fd(fd, mb << 20, sum);
	ms = now_ms() - started;
	close(fd);
	check(got == (long)(mb << 20), "the whole of the first MiBs read");
	printf("nbd_test: raw %ld bytes sha256 %s in %ld ms, %ld of them in read() (%ld KB/s)\n", got, sum, ms,
	    read_ms, read_ms > 0 ? got / read_ms * 1000 / 1024 : 0);
}

static void
mounted(void)
{
	const char *wrote = boot_arg("nbd_test_write"), *wanted = boot_arg("nbd_test_check");
	struct hfs_args args;
	char sum[65], path[64], text[64], back[64];
	long got;
	int fd, ok;

	const char *fs = boot_arg("nbd_test_fs");
	int hfs = fs != NULL && strcmp(fs, "hfs") == 0;
	struct stat st;

	memset(&args, 0, sizeof(args));
	args.fspec = "/dev/nbd0";
	if (hfs) {
		ok = mount("hfs", "/mnt", wrote != NULL ? 0 : MNT_RDONLY, &args) == 0;
	} else {
		ok = mount("apfs", "/mnt", MNT_RDONLY, (void *)"/dev/nbd0") == 0;
	}
	if (!ok) {
		printf("nbd_test: mount %s /dev/nbd0 on /mnt: %s (%d)\n", hfs ? "hfs" : "apfs", strerror(errno), errno);
	}
	check(ok, hfs ? (wrote != NULL ? "HFS Plus from /dev/nbd0 on /mnt, writable" : "HFS Plus from /dev/nbd0 on /mnt, read only") :
	    "APFS from /dev/nbd0 on /mnt, read only");
	if (!ok) {
		return;
	}
	if (stat("/mnt/test.bin", &st) == 0) {
		fd = open("/mnt/test.bin", O_RDONLY, 0);
		got = fd >= 0 ? sum_fd(fd, ~0ul, sum) : -1;
		if (fd >= 0) {
			close(fd);
		}
		check(got == (long)st.st_size && got > 0, "/mnt/test.bin read, all of it");
		if (got > 0) {
			printf("nbd_test: file test.bin %ld bytes sha256 %s\n", got, sum);
		}
	}
	if (wanted != NULL) {
		strcpy(path, "/mnt/boot-");
		strcpy(path + strlen(path), wanted);
		strcpy(path + strlen(path), ".txt");
		strcpy(text, "written by boot ");
		strcpy(text + strlen(text), wanted);
		strcpy(text + strlen(text), "\n");
		memset(back, 0, sizeof(back));
		fd = open(path, O_RDONLY, 0);
		got = fd >= 0 ? read(fd, back, sizeof(back) - 1) : -1;
		if (fd >= 0) {
			close(fd);
		}
		check(got == (long)strlen(text) && strcmp(back, text) == 0, "what the boot before wrote is there, whole");
	}
	if (wrote != NULL) {
		strcpy(path, "/mnt/boot-");
		strcpy(path + strlen(path), wrote);
		strcpy(path + strlen(path), ".txt");
		strcpy(text, "written by boot ");
		strcpy(text + strlen(text), wrote);
		strcpy(text + strlen(text), "\n");
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		ok = fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text) && fsync(fd) == 0;
		if (fd >= 0) {
			close(fd);
		}
		check(ok, "a file written and fsynced on the volume");
	}
	ok = unmount("/mnt", 0) == 0;
	if (!ok) {
		printf("nbd_test: unmount /mnt: %s\n", strerror(errno));
	}
	check(ok, "unmounted");
}

int
main(int argc, char **argv, char **envp)
{
	int fd = open("/dev/console", O_RDWR, 0);

	(void)argc, (void)argv, (void)envp;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	printf("nbd_test: process %d\n", getpid());
	raw_read();
	if (boot_arg("nbd_test_nomount") == NULL) {
		mounted();
	}
	printf("nbd_test: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
	sync();
	return failures != 0;
}
