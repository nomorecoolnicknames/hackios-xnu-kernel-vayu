/*
 * /private/var as HFS Plus on the phone's flash, for init (src/init.c) and for its test in QEMU (src/hfs_test.c),
 * which is why it is a file of its own: the test runs this code, not a copy of it.
 *
 *   var_hfs=1     /private/var is HFS Plus on /dev/ufs_var -- the slice of darwin that the kernel lets a filesystem
 *                 write (bsd/dev/hakios.ufs.c: ufs_write=1 and ufs_var_first= / ufs_var_blocks=, which the loader
 *                 puts on the command line from darwin's layout) -- instead of a memory filesystem: what iOS writes
 *                 stays, as on an iPhone. var_hfs=NAME: /dev/NAME instead (QEMU's disk, rvblk0). If it does not
 *                 mount, /private/var is a memory filesystem for this boot, as without var_hfs, and with
 *                 var_store=1 the store of the last Shut Down is put back into it as it always was.
 *   var_import=1  a new volume (made empty on the computer: devices/poco-x3-pro/tools/var-volume.sh) gets the newest
 *                 good save of var_store over its skeleton -- the way the data of the memory filesystem's days moves
 *                 onto the flash. Once: the mark says what was done, and a volume once filled is never filled again;
 *                 var_store's slots are left as they are (and with var_hfs asked, var_store never saves again).
 *   var_fsck=1    the volume mended before it is mounted, by iOS's own fsck_hfs on /dev/rufs_var (the kernel makes
 *                 that node with the same argument): when its header says the kernel found it inconsistent, and
 *                 when it does not mount -- then fsck_hfs -fy, and the mount once more.
 *   var_hfs_trace=1  the kernel says each step of the mount (bsd/hfs/hfs_builtin.c) and keeps the lines in
 *                 kern.hakios.hfs_trace; a mount that fails has them said here as well (diagnosis only).
 *   var_hfs_strict=1  a volume asked for that does not mount is the end of the boot: no iOS on a memory filesystem
 *                 that would look like a phone reset to nothing. The kernel's command port (usbnet_cmd=1) and the
 *                 buttons still reach the recovery.
 *
 * Before the mount the volume's header is read and said: whether the last boot unmounted it (a Shut Down or a
 * restart from iOS: reboot(2) unmounts every filesystem, bsd/kern/kern_shutdown.c) or left it mounted (a hard
 * reset, a hang, the end of a run through the loader), and how much of the journal is still to be replayed. HFS
 * Plus replays it itself at the mount: what a hard reset cut in two is either all there or not at all.
 *
 * A fill is made whole or made again: VAR_FILLING is written before the import and goes when the mark is written,
 * so a volume found with it and without the mark is one whose fill was cut short (a reset in the middle) or failed;
 * it is emptied and filled again from the store, which is still there -- what the boot that failed wrote after it
 * goes with it, so that two generations are never mixed. VAR_FILL_TRIES fills at most; after that the volume is
 * marked as it is and kept.
 *
 * The mark (VAR_MARK) is written when a new volume has been filled, and its being there is what makes every later
 * boot keep the volume as it is: only what one boot can mean (VAR_PER_BOOT, as var_store leaves it out) is emptied,
 * and of the skeleton only what is missing is made. Every boot of the volume counts itself in VAR_BOOTS, written
 * without fsync: that the next boot sees it one higher proves that a write made after the start reached the flash
 * without an unmount. And a process of init's calls sync() every VAR_SYNC_SECONDS for the whole run: this kernel has
 * no periodic sync of its own, and a hard reset (the buttons, a hang, a panic) unmounts nothing. HFS's journal keeps
 * what is on the flash consistent at every moment; this syncer and HFS's own (bsd/hfs/hfs_builtin.c,
 * hfs_is_ejectable) bound how many seconds of writes a hard reset can lose.
 *
 * Needs from its includer: boot_arg(), check(), in_image(), and libmini.
 *
 * From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commits 274f9827f52f (/private/var on
 * HFS Plus, var_hfs=1) and ec38881a572c (the volume written out within seconds).
 */
#ifndef VAR_HFS_H
#define VAR_HFS_H

#ifndef EEXIST
#define EEXIST 17
#endif

#define VAR_MARK                "/private/var/.hakios-var-volume"
#define VAR_FILLING             "/private/var/.hakios-var-filling"
#define VAR_BOOTS               "/private/var/.hakios-var-boots"
#define VAR_SYNC_SECONDS        3
#define VAR_FILL_TRIES          3
#define VAR_FSCK                "/System/Library/Filesystems/hfs.fs/fsck_hfs"

enum { VAR_IN_MEMORY, VAR_ON_FLASH, VAR_ON_FLASH_NEW };
static int var_volume = VAR_IN_MEMORY;
static int var_hfs_failed;      /* asked for, and it did not mount: /private/var is a memory filesystem this boot */
static long var_boots;
static long var_fill_tries;     /* fills of this volume begun before this boot's (VAR_FILLING) */

/* struct hfs_mount_args as user space passes it (bsd/hfs/hfs_mount.h) */
struct var_hfs_args {
	const char *fspec;
	uint32_t uid, gid;
	uint16_t mask, pad;
	uint32_t encoding;
	int tz[2];
	int flags, journal_tbuffer_size, journal_flags, journal_disable;
};

/* what only the boot that made it can mean: emptied at every boot of the volume (var_store.c leaves it out) */
static const char *const VAR_PER_BOOT[] = { "/private/var/tmp", "/private/var/run", "/private/var/vm" };

/*
 * What is in a directory, gone; the directory stays. Links are removed, not followed. A directory is read from its
 * start again after every bufferful removed (a removal moves what follows it), until a reading removes nothing.
 */
static int
var_empty_tree(const char *path, int depth)
{
	char buf[2048], child[512];
	struct stat st;
	int ok = 1;

	if (depth > 32) {
		return 0;
	}
	for (;;) {
		off_t position = 0;
		ssize_t n;
		int removed = 0, fd = open(path, O_RDONLY | O_DIRECTORY, 0);

		if (fd < 0) {
			return ok;
		}
		n = getdirentries(fd, buf, sizeof(buf), &position);
		close(fd);
		for (ssize_t at = 0; at < n;) {
			struct dirent *de = (struct dirent *)(void *)(buf + at);
			const char *name = de->d_name;

			if (de->d_reclen == 0) {
				break;
			}
			at += de->d_reclen;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strlen(path) + strlen(name) + 2 > sizeof(child)) {
				continue;
			}
			strcpy(child, path);
			strcpy(child + strlen(child), "/");
			strcpy(child + strlen(child), name);
			if (lstat(child, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR ?
			    var_empty_tree(child, depth + 1) && rmdir(child) == 0 : unlink(child) == 0) {
				removed++;
			} else {
				ok = 0;
			}
		}
		if (removed == 0) {
			return ok;
		}
	}
}

static uint32_t
var_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static long
var_read_number(const char *path)
{
	char text[32];
	long value = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY, 0);

	if (fd < 0) {
		return -1;
	}
	n = read(fd, text, sizeof(text) - 1);
	close(fd);
	text[n > 0 ? n : 0] = 0;
	for (char *p = text; *p >= '0' && *p <= '9'; p++) {
		value = value * 10 + (*p - '0');
	}
	return value;
}

/* PATH: "WHAT" and a number, or "filled with WHAT" (the mark), fsynced */
static void
var_write_line(const char *path, const char *what, long number)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		if (number >= 0) {
			dprintf(fd, "%ld\n", number);
		} else {
			dprintf(fd, "filled with %s\n", what);
		}
		fsync(fd);
		close(fd);
	}
}

/*
 * The volume's header (bsd/hfs/hfs_format.h, HFSPlusVolumeHeader at byte 1024, big-endian) and its journal's
 * (bsd/hfs/hfs_journal.h, journal_header, in the writer's byte order), read before the mount and said. 1: it is
 * HFS Plus and the kernel did not mark it inconsistent; 2: inconsistent; 0: no HFS Plus there (or unreadable).
 */
static int
var_hfs_header(const char *dev)
{
	static uint8_t block[4096];
	const uint8_t *h = block + 1024;
	uint32_t attributes, journal_block, block_size;
	uint16_t signature;
	int fd = open(dev, O_RDONLY, 0), result;

	if (fd < 0 || pread(fd, block, sizeof(block), 0) != (ssize_t)sizeof(block)) {
		printf("  %s: its header does not read: %s\n", dev, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return 0;
	}
	signature = (uint16_t)(h[0] << 8 | h[1]);
	if (signature != 0x482b && signature != 0x4858) {
		close(fd);
		printf("  %s: no HFS Plus volume there (signature %04x): not written yet? (var-volume.sh, darwin-install.sh var)\n",
		    dev, signature);
		return 0;
	}
	attributes = var_be32(h + 4);
	journal_block = var_be32(h + 12);
	block_size = var_be32(h + 40);
	result = attributes & (1u << 14) ? 2 : 1;
	printf("  the volume's header: %s, %u MB of %u MB free, last mounted by \"%c%c%c%c\"; %s%s\n",
	    signature == 0x4858 ? "HFSX" : "HFS Plus",
	    (unsigned)((uint64_t)var_be32(h + 48) * block_size >> 20), (unsigned)((uint64_t)var_be32(h + 44) * block_size >> 20),
	    h[8] >= 32 && h[8] < 127 ? h[8] : '?', h[9] >= 32 && h[9] < 127 ? h[9] : '?',
	    h[10] >= 32 && h[10] < 127 ? h[10] : '?', h[11] >= 32 && h[11] < 127 ? h[11] : '?',
	    attributes & (1u << 8) ? "the last boot unmounted it (clean)" :
	    "the last boot left it mounted (a hard end: the journal makes it whole)",
	    result == 2 ? "; the kernel found it INCONSISTENT" : "");
	if ((attributes & (1u << 13)) != 0 && journal_block != 0 && block_size >= 4096 && block_size % 4096 == 0 &&
	    pread(fd, block, sizeof(block), (off_t)journal_block * block_size) == (ssize_t)sizeof(block)) {
		uint64_t at = (uint64_t)var_be32(block + 36) << 32 | var_be32(block + 40);

		if ((var_be32(block) & 1) != 0 && at % 4096 == 0 && pread(fd, block, sizeof(block), (off_t)at) ==
		    (ssize_t)sizeof(block)) {
			uint32_t magic;
			uint64_t start, end, size;

			memcpy(&magic, block, 4);
			memcpy(&start, block + 8, 8);
			memcpy(&end, block + 16, 8);
			memcpy(&size, block + 24, 8);
			if (magic == 0x784c4e4a) {      /* written big-endian (mkfs on the computer) */
				start = __builtin_bswap64(start);
				end = __builtin_bswap64(end);
				size = __builtin_bswap64(size);
				magic = 0x4a4e4c78;
			}
			if (magic != 0x4a4e4c78) {
				printf("  the journal's header: not a journal (%08x)\n", magic);
			} else if (start == end) {
				printf("  the journal: empty, nothing to replay (%llu KB)\n", (unsigned long long)(size >> 10));
			} else {
				printf("  the journal: %llu KB of transactions to replay at the mount\n",
				    (unsigned long long)((end >= start ? end - start : size - start + end) >> 10));
			}
		}
	}
	close(fd);
	return result;
}

/* The kernel's counters of the storage (bsd/dev/hakios.ufs.c), for what a mount that failed did to it */
static const char *const VAR_COUNTERS[] = { "ufs_requests", "ufs_errors", "ufs_writes", "ufs_write_errors",
	                                    "ufs_write_refused", "ufs_var_writes", "ufs_var_patched" };
static uint64_t var_counted[sizeof(VAR_COUNTERS) / sizeof(VAR_COUNTERS[0])];

static void
var_count(int say)
{
	for (unsigned i = 0; i < sizeof(VAR_COUNTERS) / sizeof(VAR_COUNTERS[0]); i++) {
		char name[48];
		uint64_t now = 0;
		size_t len = sizeof(now);

		strcpy(name, "kern.hakios.");
		strcpy(name + 12, VAR_COUNTERS[i]);
		if (sysctlbyname(name, &now, &len, NULL, 0) != 0) {
			now = 0;
		}
		if (say && now != var_counted[i]) {
			printf("  the storage during the mount: %s +%llu\n", VAR_COUNTERS[i],
			    (unsigned long long)(now - var_counted[i]));
		}
		var_counted[i] = now;
	}
}

/*
 * A mount that failed (diagnosis only, nothing written): with var_hfs_trace=1 the kernel's own steps of it; the kernel's counters of the storage across it -- an
 * error or a refusal of the loader's says the I/O failed, none says HFS itself refused -- and the same volume
 * mounted read-only and given back: if that mounts, what failed was a write.
 */
static void
var_hfs_why(struct var_hfs_args *args)
{
	static char trace[4100];
	size_t len = sizeof(trace) - 1;

	var_count(1);
	/* var_hfs_trace=1: the kernel's own account of the mount (bsd/hfs/hfs_builtin.c, kern.hakios.hfs_trace) */
	if (sysctlbyname("kern.hakios.hfs_trace", trace, &len, NULL, 0) == 0 && len > 0) {
		trace[len < sizeof(trace) ? len : sizeof(trace) - 1] = 0;
		for (char *line = trace; *line;) {
			char *end = strchr(line, '\n');

			if (end != NULL) {
				*end = 0;
			}
			printf("  kernel: %s\n", line);
			if (end == NULL) {
				break;
			}
			line = end + 1;
		}
	} else if (boot_arg("var_hfs_trace") != NULL) {
		printf("  kern.hakios.hfs_trace: %s (%lu)\n", strerror(errno), (unsigned long)len);
	}
	if (mount("hfs", "/private/var", MNT_RDONLY, args) == 0) {
		printf("  read-only it mounts (so a write failed): given back for now\n");
		unmount("/private/var", 0);
	} else {
		printf("  read-only it does not mount either: %s\n", strerror(errno));
	}
	var_count(1);
}

/* var_fsck=1: iOS's own fsck_hfs, mending the volume through /dev/rufs_var. Its exit status, or -1. */
static int var_hfs_run(char *const argv[]);

static int
var_hfs_fsck(const char *name, const char *why)
{
	const char *fsck = boot_arg("var_fsck");
	char raw[64];
	struct stat st;
	int status;

	if (fsck == NULL || strcmp(fsck, "1") != 0) {
		return -1;
	}
	if (stat(VAR_FSCK, &st) != 0) {
		printf("  %s, and there is no %s to mend it with\n", why, VAR_FSCK);
		return -1;
	}
	strcpy(raw, "/dev/r");
	strcpy(raw + 6, name);         /* ufs_var: /dev/rufs_var */
	printf("  %s: fsck_hfs -fy %s (var_fsck=1)\n", why, raw);
	{
		char *argv[] = { (char *)VAR_FSCK, (char *)"-fy", raw, NULL };

		status = var_hfs_run(argv);
	}
	printf("  fsck_hfs: %s (%d)\n", status == 0 ? "the volume is whole, or mended" : "it could not mend it", status);
	return status;
}

/*
 * var_hfs=...: the volume on /private/var, and what it is (var_volume). Nothing asked, or no volume: VAR_IN_MEMORY,
 * and the caller mounts its memory filesystem as ever.
 */
static int
var_hfs_mount(void)
{
	const char *asked = boot_arg("var_hfs");
	char dev[64], type[20];
	struct var_hfs_args args;
	struct stat st;

	type[0] = 0;
	if (asked == NULL || strcmp(asked, "0") == 0 || strlen(asked) > 40 || strchr(asked, '/') != NULL) {
		return var_volume;
	}
	strcpy(dev, "/dev/");
	strcpy(dev + 5, strcmp(asked, "1") == 0 ? "ufs_var" : asked);
	memset(&args, 0, sizeof(args));
	args.fspec = dev;
	var_hfs_failed = 1;
	if (var_hfs_header(dev) == 2) {
		var_hfs_fsck(dev + 5, "the kernel marked the volume inconsistent");
	}
	var_count(0);
	if (mount("hfs", "/private/var", 0, &args) != 0) {
		int error = errno;

		printf("  HFS Plus on %s: the mount failed: %s (%d)\n", dev, strerror(error), error);
		var_hfs_why(&args);

		if (var_hfs_fsck(dev + 5, "the volume does not mount") < 0 || mount("hfs", "/private/var", 0, &args) != 0) {
			const char *store = boot_arg("var_store");

			printf("  HFS Plus on %s: %s: a memory filesystem instead, for this boot only%s\n", dev, strerror(error),
			    store != NULL && strcmp(store, "1") == 0 ? " (var_store's newest save read into it, and never saved)" : "");
			return var_volume;
		}
	}
	if (fs_type("/private/var", type, sizeof(type)) != 0 || strcmp(type, "hfs") != 0) {
		printf("  %s mounted, but /private/var is not HFS Plus after it (%s)\n", dev, type);
		return var_volume;
	}
	var_hfs_failed = 0;
	var_volume = stat(VAR_MARK, &st) == 0 ? VAR_ON_FLASH : VAR_ON_FLASH_NEW;
	var_fill_tries = var_volume == VAR_ON_FLASH_NEW ? var_read_number(VAR_FILLING) : -1;
	printf("  /private/var: HFS Plus on %s, %s\n", dev,
	    var_volume == VAR_ON_FLASH ? "as the last boot left it" :
	    var_fill_tries < 0 ? "new: it is filled now, once" :
	    "its fill was cut short or failed: it is emptied and filled again");
	if (var_fill_tries >= 0) {
		/* (the journal's own files and HFS's private directories stay: they are not to be removed) */
		var_empty_tree("/private/var", 0);
		printf("  the volume emptied after %ld fill%s that did not finish\n", var_fill_tries,
		    var_fill_tries == 1 ? "" : "s");
	}
	if (var_volume == VAR_ON_FLASH) {
		for (unsigned i = 0; i < sizeof(VAR_PER_BOOT) / sizeof(VAR_PER_BOOT[0]); i++) {
			if (!var_empty_tree(VAR_PER_BOOT[i], 0)) {
				printf("  %s: not all of it could be removed\n", VAR_PER_BOOT[i]);
			}
		}
		/* the package prefix is grafted anew at every boot (init.c): the last boot's link or tree goes */
		if (lstat("/private/var/jb", &st) == 0) {
			if ((st.st_mode & S_IFMT) == S_IFDIR) {
				var_empty_tree("/private/var/jb", 0);
				rmdir("/private/var/jb");
			} else {
				unlink("/private/var/jb");
			}
		}
	}
	return var_volume;
}

/* A program of this image's, waited for: its exit status, or -1 if it did not run to an end. */
static int
var_hfs_run(char *const argv[])
{
	int status = 0;
	pid_t pid = fork();

	if (pid == 0) {
		execve(argv[0], argv, environ);
		printf("  %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (pid < 0 || wait4(pid, &status, 0, NULL) != pid || !WIFEXITED(status)) {
		return -1;
	}
	return WEXITSTATUS(status);
}

/*
 * A new volume, its skeleton made: with var_import=1 the last var_store save over it (var_store import), and then
 * the mark, which says what went in. The fill is begun by writing VAR_FILLING (the fills begun so far) and ended by
 * the mark and its removal; an import that failed leaves the volume unmarked, and the next boot empties it and fills
 * it again (var_hfs_mount), VAR_FILL_TRIES times at most. "No save" and "no save that checks out" are not failures:
 * there is nothing to bring, and the volume is marked with the skeleton alone.
 */
static void
var_hfs_fill(void)
{
	const char *import = boot_arg("var_import");
	const char *what = "the skeleton";
	long tries = var_fill_tries < 0 ? 1 : var_fill_tries + 1;
	struct stat st;
	int done = 1, failed = 0;

	if (var_volume != VAR_ON_FLASH_NEW) {
		return;
	}
	if (import != NULL && strcmp(import, "1") == 0) {
		char *argv[] = { (char *)in_image("/bin/var_store"), (char *)"import", NULL };
		int status;

		printf("\n-- the data volume of the memory filesystem's days, onto the flash (var_import=1, fill %ld of at most %d) --\n",
		    tries, VAR_FILL_TRIES);
		var_write_line(VAR_FILLING, NULL, tries);
		sync();
		status = var_hfs_run(argv);
		what = status == 0 ? "the skeleton and var_store's newest save" :
		    status == 3 ? "the skeleton; var_store had no save" :
		    status == 4 ? "the skeleton; NOTHING of var_store's saves checked out" :
		    "the skeleton; var_store import FAILED";
		failed = !(status == 0 || status == 3 || status == 4);
		done = !failed || tries >= VAR_FILL_TRIES;
		check(status == 0 || status == 3, "var_store's newest save onto the volume on the flash");
	}
	if (!done) {
		sync();
		printf("  the volume on the flash: NOT marked (%s): the next boot empties it and fills it again (fill %ld of %d)\n",
		    what, tries, VAR_FILL_TRIES);
		return;
	}
	var_write_line(VAR_MARK, what, -1);
	if (stat(VAR_MARK, &st) == 0) {
		unlink(VAR_FILLING);
	}
	printf("  the volume on the flash: filled with %s%s\n", what,
	    failed ? ", given up after the last fill: kept as it is" : "");
	check(stat(VAR_MARK, &st) == 0, "mark the data volume as filled");
	sync();
}

/* Every boot of the volume: its count, and the syncer. */
static void
var_hfs_keep(void)
{
	int fd;
	pid_t pid;

	if (var_volume == VAR_IN_MEMORY) {
		return;
	}
	var_boots = var_read_number(VAR_BOOTS);
	var_boots = var_boots < 0 ? 1 : var_boots + 1;
	fd = open(VAR_BOOTS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		dprintf(fd, "%ld\n", var_boots);
		close(fd);
	}
	printf("  the data volume on the flash: boot %ld of it\n", var_boots);
	pid = fork();
	if (pid == 0) {
		for (;;) {
			usleep(VAR_SYNC_SECONDS * 1000000);
			sync();
		}
	}
	check(pid > 0, "the data volume's syncer (sync() every few seconds)");
}

#endif /* VAR_HFS_H */
