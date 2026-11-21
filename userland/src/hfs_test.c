/*
 * hfs_test: the data volume of init (src/var_hfs.h -- this program runs that code, not a copy of it) in QEMU, on a
 * disk of its own, each boot of it ended hard: what the volume on the phone's flash must survive at every reset.
 * From the idea of Scion-Project's and vayu-dev's hfs_test.c (mount, write, unmount, mount again); here nothing is
 * unmounted and nothing fsynced, and the next boot is the check.
 *
 * Process 1 of a tiny APFS root (/sbin/launchd, /dev, /private/var), started by qemu/run-hfs-test.sh with
 * virtio_blk=2 var_hfs=rvblk0 on the command line, the disk an HFSX volume made by mkfs and nothing else:
 *   - mounts it on /private/var as init does (var_hfs_mount); a new volume gets a small skeleton and its mark
 *     (var_hfs_fill), one that has been booted has its per-boot directories emptied;
 *   - on a volume that has been booted: the file the boot before wrote (mobile/boot-N.bin, BYTES made from N) is
 *     there and whole, the one before that is gone (that boot removed it), "latest" says N, the per-boot junk is
 *     gone, and the boot count (var_hfs_keep) is N + 1;
 *   - writes this boot's: its file, "latest", junk in tmp, and removes the file of the boot before last -- without
 *     fsync or unmount; var_hfs_keep's syncer and HFS's own are what take it to the disk;
 *   - says "hfs_test: boot N: PASS ... written" (or FAIL and what), and waits to be cut off: the script ends QEMU
 *     with SIGKILL some seconds later.
 * The fill's import (var_import=1) is this program too, as /bin/var_store of the root: started as "var_store
 * import" it does what hfs_test_import= says -- ok: a file, exit 0; fail: half a file, exit 1; hang: half a file
 * and it waits to be cut off, as a reset in the middle of the import would. So the script can check that a fill
 * cut short or failed leaves the volume unmarked, that the next boot empties it and fills it again, and that
 * what a whole fill brought stays (qemu/run-hfs-test.sh, SCENARIO=import).
 *
 * After other HakiOS ports (MIT, as this file): oosexgang/hakios-xnu-kernel, branch main, commit 83aceb83817c (its
 * qemu/run-hfs-test.sh and hfs_test.c, also in Scion-Project/scion-kernel, branch main), and telegdyd/ios-vayu, branch vayu-dev, commit
 * 74cfdfa7d258.
 */
#include "mini.h"

#define BYTES           (4u << 20)      /* a file of the cluster layer's size, not a metadata block's */

static int failures;

static void
check(int ok, const char *what)
{
	printf("hfs_test: %s: %s\n", ok ? "ok" : "FAILED", what);
	failures += !ok;
}

static const char *
in_image(const char *path)
{
	return path;
}

/* the value of boot-arg NAME, or NULL (as init's) */
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

#include "var_hfs.h"

static void
fill(uint8_t *p, uint32_t seed)
{
	for (uint32_t i = 0; i < BYTES; i++) {
		seed = seed * 1103515245u + 12345u;
		p[i] = (uint8_t)(seed >> 16);
	}
}

static long
read_all(const char *path, uint8_t *into, size_t size)
{
	long got = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY, 0);

	if (fd < 0) {
		return -1;
	}
	while ((size_t)got < size && (n = read(fd, into + got, size - (size_t)got)) > 0) {
		got += n;
	}
	close(fd);
	return got;
}

static int
write_all(const char *path, const void *data, size_t size)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), ok;

	if (fd < 0) {
		return 0;
	}
	ok = write(fd, data, size) == (ssize_t)size;
	close(fd);
	return ok;
}

/* "var_store import", as var_hfs_fill runs it: what hfs_test_import= asks for */
static int
import_main(void)
{
	const char *how = boot_arg("hfs_test_import");
	int hang = how != NULL && strcmp(how, "hang") == 0, fail = how != NULL && strcmp(how, "fail") == 0;

	printf("hfs_import: %s\n", hang ? "hang" : fail ? "fail" : "ok");
	if (hang || fail) {
		write_all("/private/var/mobile/half", "half", 4);
		sync();
		if (hang) {
			printf("hfs_import: hanging, to be cut off\n");
			for (;;) {
				usleep(1000000);
			}
		}
		return 1;
	}
	return write_all("/private/var/mobile/imported", "imported\n", 9) ? 0 : 1;
}

static char *
name_of(char *buf, const char *dir, long n)
{
	char digits[24];
	int i = 23;

	digits[i] = 0;
	do {
		digits[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n > 0);
	strcpy(buf, dir);
	strcpy(buf + strlen(buf), digits + i);
	strcpy(buf + strlen(buf), ".bin");
	return buf;
}

int
main(int argc, char **argv, char **envp)
{
	static const char *const skeleton[] = { "/private/var/tmp", "/private/var/run", "/private/var/vm",
		                                "/private/var/mobile", "/private/var/mobile/Library" };
	uint8_t *want, *got;
	char path[128], text[32], mode[16];
	long before = 0, len;
	struct stat st;
	int fd;

	(void)envp;
	if (argc > 1 && strcmp(argv[1], "import") == 0) {
		return import_main();
	}
	fd = open("/dev/console", O_RDWR, 0);
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	printf("hfs_test: start\n");
	want = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	got = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (want == MAP_FAILED || got == MAP_FAILED) {
		printf("hfs_test: FAIL: no memory\n");
		for (;;) {
			usleep(1000000);
		}
	}

	var_hfs_mount();
	check(var_volume != VAR_IN_MEMORY, "the volume mounted on /private/var as HFS Plus (var_hfs_mount)");
	if (var_volume == VAR_ON_FLASH_NEW) {
		int made = 1;

		for (unsigned i = 0; i < sizeof(skeleton) / sizeof(skeleton[0]); i++) {
			made = mkdir(skeleton[i], 0755) == 0 && made;
		}
		const char *how = boot_arg("hfs_test_import");
		int before_fill;

		check(made, "a skeleton on the new volume");
		strcpy(mode, how != NULL && strlen(how) < sizeof(mode) ? how : "");
		before_fill = failures;
		var_hfs_fill();
		if (strcmp(mode, "fail") == 0) {
			failures = before_fill;         /* the import's failure is the point of this boot */
			check(stat(VAR_MARK, &st) != 0 && stat(VAR_FILLING, &st) == 0,
			    "a failed import leaves the volume unmarked, its fill begun (VAR_FILLING)");
		} else {
			check(stat(VAR_MARK, &st) == 0 && stat(VAR_FILLING, &st) != 0, "the new volume marked (var_hfs_fill)");
			check(stat("/private/var/mobile/half", &st) != 0, "nothing of a fill cut short or failed is left");
			check(strcmp(mode, "ok") != 0 || stat("/private/var/mobile/imported", &st) == 0,
			    "what the import brought is there");
		}
	} else if (var_volume == VAR_ON_FLASH) {
		len = read_all("/private/var/mobile/latest", (uint8_t *)text, sizeof(text) - 1);
		text[len > 0 ? len : 0] = 0;
		for (char *p = text; *p >= '0' && *p <= '9'; p++) {
			before = before * 10 + (*p - '0');
		}
		printf("hfs_test: the boot before was boot %ld of the volume\n", before);
		check(before > 0, "\"latest\", written by the boot before without fsync, is there");
		fill(want, (uint32_t)before);
		len = read_all(name_of(path, "/private/var/mobile/boot-", before), got, BYTES);
		check(len == (long)BYTES && memcmp(want, got, BYTES) == 0, "the boot before's file, whole and the same");
		check(before < 2 || stat(name_of(path, "/private/var/mobile/boot-", before - 1), &st) != 0,
		    "the file the boot before removed is gone");
		check(stat("/private/var/tmp/junk", &st) != 0 && stat("/private/var/run/junk", &st) != 0,
		    "the per-boot directories emptied");
		check(stat("/private/var/mobile/Library", &st) == 0, "the skeleton of the first boot still there");
		len = read_all(VAR_MARK, (uint8_t *)path, sizeof(path) - 1);
		path[len > 0 ? len : 0] = 0;
		if (len > 0 && memcmp(path, "filled with the skeleton and", 28) == 0) {
			check(stat("/private/var/mobile/imported", &st) == 0, "what the fill imported, still there");
		}
	}

	var_hfs_keep();
	check(var_boots == before + 1, "the volume's boot count is one more than the boot before's");

	/* this boot's, for the next to find: no fsync, no unmount */
	fill(want, (uint32_t)var_boots);
	check(write_all(name_of(path, "/private/var/mobile/boot-", var_boots), want, BYTES), "this boot's file written");
	len = read_all(path, got, BYTES);
	check(len == (long)BYTES && memcmp(want, got, BYTES) == 0, "and read back the same");
	if (var_boots > 1) {
		check(unlink(name_of(path, "/private/var/mobile/boot-", var_boots - 1)) == 0, "the boot before's file removed");
	}
	len = 0;
	for (long n = var_boots; n > 0; n /= 10) {
		len++;
	}
	for (long n = var_boots, i = len; i > 0; n /= 10) {
		text[--i] = (char)('0' + n % 10);
	}
	text[len] = '\n';
	check(write_all("/private/var/mobile/latest.new", text, (size_t)len + 1) &&
	    rename("/private/var/mobile/latest.new", "/private/var/mobile/latest") == 0, "\"latest\" written and renamed");
	check(write_all("/private/var/tmp/junk", "x", 1) && write_all("/private/var/run/junk", "x", 1), "per-boot junk");

	printf("hfs_test: boot %ld: %s; written, nothing synced by hand: now the hard end\n", var_boots,
	    failures == 0 ? "PASS" : "FAIL");
	for (;;) {
		usleep(1000000);
	}
}
