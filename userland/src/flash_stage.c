/*
 * flash_stage: a new boot image for vayu's `boot`, from iOS (vayu-dev's userland/src/flash_stage.c, loader 2.8:
 * deploys do not go through the loader's USB mode, which hangs after a forced restart).
 *
 * iOS never writes `boot` itself: it may only write darwin past ufs_write_first (bsd/dev/hakios.ufs.c), and the
 * loader on core 0, which makes those writes, refuses anything else. So the image is staged in darwin and the
 * loader writes it at its next start, when the image now in `boot` says flash_stage=1. Where in darwin is the
 * layout's (src/flash_stage.h): ufs_stage_first= / ufs_stage_blocks= / ufs_stage_head=, put on the command line by
 * the loader. Without them, or without flash_stage=1, nothing is staged -- a stage the loader would not take now
 * could be taken by some later image that does.
 *
 *   flash_stage show           the layout, `boot`'s sum, and what the header block holds (writes nothing)
 *   flash_stage stage IMAGE    IMAGE (an Android boot image, "ANDROID!") into the staging area, read back; then the
 *                              header, tied to `boot` as it is now (its length and FNV-1a 64: the loader writes the
 *                              image only over that boot, and clears a header staged against another). The old
 *                              header is cleared first, so a stage that does not finish is never taken. Reboot
 *                              after this.
 *   flash_stage cancel         the header cleared: the next start is an ordinary one
 *   flash_stage verify IMAGE   does `boot` (/dev/rufs_boot, read only) begin with IMAGE's bytes?
 *
 * flash_stage_target=recovery on the command line (an image in the recovery partition: the multiboot, Android in
 * boot): `boot` above is the recovery throughout -- /dev/rufs_recovery summed and compared, the header tied to it
 * ("FORRECV!", flash_stage.h) -- and boot is never what a stage is for. show says which.
 *
 * Writes go through /dev/rufs_darwin like var_store's, under the same claim (kern.hakios.var_busy) and the same
 * kernel lock; the kernel refuses them inside /dev/ufs_var's slice, and flash_stage.h refuses a layout that puts the
 * staging area there. Exit 0 on success.
 *
 * From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commit 86984b16b016 (its flash_stage.c).
 */
#include "mini.h"
#include "flash_stage.h"

#define DARWIN          "/dev/rufs_darwin"
static const char *BOOT = "/dev/rufs_boot";     /* the target: /dev/rufs_recovery with flash_stage_target=recovery */
static const char *target_name = "boot";
static int target_recovery;
#define CHUNK           (4u << 20)              /* the loader's buffer: one request a write */
#define DKIOCGETBLOCKSIZE       0x40046418UL    /* _IOR('d', 24, uint32_t) */
#define DKIOCGETBLOCKCOUNT      0x40086419UL    /* _IOR('d', 25, uint64_t) */

static struct flash_stage_layout layout;
static uint8_t *chunk;

static uint64_t boot_bytes(void);

/* `boot`'s length and FNV-1a 64 of all of it, as the loader sums it before it takes a stage; 0 when it reads */
static int
boot_sum(uint64_t *bytes, uint64_t *fnv)
{
	uint64_t at = 0, total = boot_bytes();
	int fd = open(BOOT, O_RDONLY, 0);

	*bytes = total;
	*fnv = FLASH_STAGE_FNV_START;
	if (fd < 0 || total == 0) {
		printf("flash_stage: %s: %s\n", BOOT, fd < 0 ? strerror(errno) : "its size is not known");
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	while (at < total) {
		uint64_t k = total - at < CHUNK ? total - at : CHUNK;

		if (pread(fd, chunk, (size_t)k, (off_t)at) != (ssize_t)k) {
			printf("flash_stage: %s: read at byte %llu: %s\n", BOOT, (unsigned long long)at, strerror(errno));
			close(fd);
			return -1;
		}
		*fnv = flash_stage_fnv(*fnv, chunk, (size_t)k);
		at += k;
	}
	close(fd);
	return 0;
}

static int
claim(int on)
{
	int v = on;

	if (sysctlbyname("kern.hakios.var_busy", NULL, NULL, &v, sizeof(v)) != 0) {
		printf("flash_stage: kern.hakios.var_busy=%d: %s\n", on, strerror(errno));
		return -1;
	}
	return 0;
}

static const char *
bootargs(void)
{
	static char args[4096];
	size_t len = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return "";
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	return args;
}

/* `boot`'s size in bytes, from its device; 0 if it cannot be told */
static uint64_t
boot_bytes(void)
{
	uint64_t count = 0;
	uint32_t size = 0;
	int fd = open(BOOT, O_RDONLY, 0);

	if (fd < 0) {
		return 0;
	}
	if (mini_syscall(SYS_ioctl, fd, (long)DKIOCGETBLOCKCOUNT, (long)&count, 0, 0, 0) < 0 ||
	    mini_syscall(SYS_ioctl, fd, (long)DKIOCGETBLOCKSIZE, (long)&size, 0, 0, 0) < 0) {
		count = 0;
	}
	close(fd);
	return count * size;
}

static uint8_t *
load(const char *path, uint64_t *bytes, uint64_t *rounded)
{
	struct stat st;
	uint8_t *buf;
	uint64_t got = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY, 0);

	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
		printf("flash_stage: %s: %s\n", path, fd < 0 ? strerror(errno) : "empty");
		return NULL;
	}
	*bytes = (uint64_t)st.st_size;
	*rounded = (*bytes + FLASH_STAGE_BLOCK - 1) / FLASH_STAGE_BLOCK * FLASH_STAGE_BLOCK;
	buf = mmap(NULL, (size_t)*rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (buf == MAP_FAILED) {
		printf("flash_stage: no memory for %llu bytes\n", (unsigned long long)*rounded);
		close(fd);
		return NULL;
	}
	while (got < *bytes && (n = read(fd, buf + got, (size_t)(*bytes - got))) > 0) {
		got += (uint64_t)n;
	}
	close(fd);
	if (got != *bytes) {
		printf("flash_stage: %s: could not read it whole\n", path);
		return NULL;
	}
	return buf;
}

static int
put(int fd, const uint8_t *p, uint64_t n, uint64_t at)
{
	for (uint64_t done = 0; done < n;) {
		uint64_t k = n - done < CHUNK ? n - done : CHUNK;
		ssize_t w = pwrite(fd, p + done, (size_t)k, (off_t)(at + done));

		if (w != (ssize_t)k) {
			printf("flash_stage: write at byte %llu of darwin: %s\n", (unsigned long long)(at + done),
			    w < 0 ? strerror(errno) : "short");
			return -1;
		}
		done += k;
	}
	return 0;
}

static int
same(int fd, const uint8_t *p, uint64_t n, uint64_t at, const char *what)
{
	for (uint64_t done = 0; done < n;) {
		uint64_t k = n - done < CHUNK ? n - done : CHUNK;

		if (pread(fd, chunk, (size_t)k, (off_t)(at + done)) != (ssize_t)k) {
			printf("flash_stage: %s: read at byte %llu: %s\n", what, (unsigned long long)(at + done), strerror(errno));
			return -1;
		}
		if (memcmp(chunk, p + done, (size_t)k) != 0) {
			printf("flash_stage: %s: DIFFERENT in the %llu bytes from %llu\n", what, (unsigned long long)k,
			    (unsigned long long)done);
			return -1;
		}
		done += k;
	}
	return 0;
}

/* the header block: this image's against that boot, or with img NULL zeros (no stage); written, synced, read back */
static int
write_head(int fd, const uint8_t *img, uint64_t bytes, uint64_t boot_len, uint64_t boot_fnv)
{
	static uint8_t block[FLASH_STAGE_BLOCK];
	uint64_t at = layout.head * FLASH_STAGE_BLOCK;

	if (img != NULL) {
		flash_stage_head_block_for(block, &layout, img, bytes, boot_len, boot_fnv, target_recovery);
	} else {
		memset(block, 0, sizeof(block));
	}
	return put(fd, block, sizeof(block), at) == 0 && fsync(fd) == 0 && same(fd, block, sizeof(block), at, "header") == 0 ?
	       0 : -1;
}

static int
show(void)
{
	static uint8_t block[FLASH_STAGE_BLOCK];
	struct flash_stage_head h;
	uint64_t boot_len = 0, boot_fnv = 0;
	int fd = open(DARWIN, O_RDONLY, 0), bound;

	if (boot_sum(&boot_len, &boot_fnv) != 0) {
		boot_len = 0;
	}
	printf("flash_stage: staging area darwin blocks %llu..%llu, header in block %llu; target %s (%s), %llu bytes, "
	    "fnv %016llx\n", (unsigned long long)layout.first, (unsigned long long)(layout.first + layout.blocks - 1),
	    (unsigned long long)layout.head, target_name, BOOT, (unsigned long long)boot_len, (unsigned long long)boot_fnv);
	if (fd < 0 || pread(fd, block, sizeof(block), (off_t)(layout.head * FLASH_STAGE_BLOCK)) != (ssize_t)sizeof(block)) {
		printf("flash_stage: the header block: %s\n", strerror(errno));
		return 1;
	}
	close(fd);
	if (flash_stage_head_read(block, &layout, &h)) {
		bound = boot_len != 0 ? flash_stage_head_for_target(&h, boot_len, boot_fnv, target_recovery) : -3;
		printf("flash_stage: staged: %llu bytes, fnv %016llx: %s\n", (unsigned long long)h.bytes,
		    (unsigned long long)h.fnv, bound == 1 ? (target_recovery ? "against this recovery: the loader writes the "
		    "recovery at the next start (with flash_stage=1)" : "against this boot: the loader writes boot at the next "
		    "start (with flash_stage=1)") : bound == 0 ? "against another boot: the loader clears it, writes nothing" :
		    bound == -1 ? "tied to no boot (an older flash_stage): the loader leaves it alone" :
		    bound == -2 ? "for the other partition (flash_stage_target): the loader clears it, writes nothing" :
		    "boot could not be summed");
	} else {
		printf("flash_stage: nothing staged\n");
	}
	return 0;
}

int
main(int argc, char **argv, char **envp)
{
	const char *what = argc > 1 ? argv[1] : "show", *why, *args = bootargs();
	uint64_t bytes = 0, rounded = 0, on = 0, boot_len = 0, boot_fnv = 0;
	uint8_t *img;
	int fd;

	(void)envp;
	chunk = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (chunk == MAP_FAILED) {
		printf("flash_stage: no memory\n");
		return 1;
	}
	if ((why = flash_stage_target(args, &target_recovery)) != NULL) {
		printf("flash_stage: %s: nothing is done\n", why);
		return 1;
	}
	if (target_recovery) {
		BOOT = "/dev/rufs_recovery";
		target_name = "recovery";
	}
	if (strcmp(what, "verify") == 0) {
		if (argc != 3 || (img = load(argv[2], &bytes, &rounded)) == NULL) {
			return argc != 3 ? 2 : 1;
		}
		fd = open(BOOT, O_RDONLY, 0);
		if (fd < 0) {
			printf("flash_stage: %s: %s\n", BOOT, strerror(errno));
			return 1;
		}
		if (same(fd, img, bytes, 0, "boot") != 0) {
			return 1;
		}
		printf("flash_stage: %s holds %s (%llu bytes, fnv %016llx)\n", target_name, argv[2], (unsigned long long)bytes,
		    (unsigned long long)flash_stage_fnv(FLASH_STAGE_FNV_START, img, (size_t)bytes));
		return 0;
	}
	if ((why = flash_stage_layout(args, &layout)) != NULL) {
		printf("flash_stage: %s\n", why);
		return 1;
	}
	if (strcmp(what, "show") == 0) {
		return show();
	}
	if (strcmp(what, "cancel") == 0) {
		fd = open(DARWIN, O_RDWR, 0);
		if (fd < 0 || claim(1) != 0 || write_head(fd, NULL, 0, 0, 0) != 0) {
			printf("flash_stage: cancel: FAILED (%s)\n", strerror(errno));
			(void)claim(0);
			return 1;
		}
		(void)claim(0);
		printf("flash_stage: cancelled: the next start is an ordinary one\n");
		return 0;
	}
	if (argc != 3 || strcmp(what, "stage") != 0) {
		printf("usage: flash_stage show | stage IMAGE | cancel | verify IMAGE\n");
		return 2;
	}
	if (flash_stage_arg(args, "flash_stage", &on) != 1 || on != 1) {
		printf("flash_stage: this image does not say flash_stage=1: its loader would not take a stage now, and a "
		    "later one might take it unasked; nothing is staged\n");
		return 1;
	}
	if ((img = load(argv[2], &bytes, &rounded)) == NULL) {
		return 1;
	}
	if ((why = flash_stage_image(img, bytes, &layout, boot_bytes())) != NULL) {
		printf("flash_stage: %s: %s\n", argv[2], why);
		return 1;
	}
	if (boot_sum(&boot_len, &boot_fnv) != 0) {
		printf("flash_stage: boot could not be summed: nothing is staged\n");
		return 1;
	}
	fd = open(DARWIN, O_RDWR, 0);
	if (fd < 0) {
		printf("flash_stage: %s: %s\n", DARWIN, strerror(errno));
		return 1;
	}
	if (claim(1) != 0) {
		return 1;
	}
	/* the header off first: a stage that does not finish is never taken */
	if (write_head(fd, NULL, 0, 0, 0) != 0 || put(fd, img, rounded, layout.first * FLASH_STAGE_BLOCK) != 0 ||
	    fsync(fd) != 0 || same(fd, img, rounded, layout.first * FLASH_STAGE_BLOCK, "staged image") != 0 ||
	    write_head(fd, img, bytes, boot_len, boot_fnv) != 0) {
		printf("flash_stage: stage: FAILED; nothing will be flashed\n");
		(void)write_head(fd, NULL, 0, 0, 0);
		(void)claim(0);
		return 1;
	}
	(void)claim(0);
	printf("flash_stage: staged %s: %llu bytes at darwin block %llu, fnv %016llx, against %s's %llu bytes of fnv "
	    "%016llx; the loader writes %s at the next start\n", argv[2], (unsigned long long)bytes,
	    (unsigned long long)layout.first, (unsigned long long)flash_stage_fnv(FLASH_STAGE_FNV_START, img, (size_t)bytes),
	    target_name, (unsigned long long)boot_len, (unsigned long long)boot_fnv, target_name);
	return 0;
}
