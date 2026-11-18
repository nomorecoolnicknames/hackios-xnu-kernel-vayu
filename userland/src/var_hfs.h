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
 *                 var_store's slots are left as they are.
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
#define VAR_BOOTS               "/private/var/.hakios-var-boots"
#define VAR_SYNC_SECONDS        3

enum { VAR_IN_MEMORY, VAR_ON_FLASH, VAR_ON_FLASH_NEW };
static int var_volume = VAR_IN_MEMORY;
static long var_boots;

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
	if (mount("hfs", "/private/var", 0, &args) != 0) {
		printf("  HFS Plus on %s: %s: a memory filesystem instead, for this boot only\n", dev, strerror(errno));
		return var_volume;
	}
	if (fs_type("/private/var", type, sizeof(type)) != 0 || strcmp(type, "hfs") != 0) {
		printf("  %s mounted, but /private/var is not HFS Plus after it (%s)\n", dev, type);
		return var_volume;
	}
	var_volume = stat(VAR_MARK, &st) == 0 ? VAR_ON_FLASH : VAR_ON_FLASH_NEW;
	printf("  /private/var: HFS Plus on %s, %s\n", dev,
	    var_volume == VAR_ON_FLASH ? "as the last boot left it" : "new: it is filled now, once");
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
 * the mark, which says what went in. Whatever the import made of it, the volume is marked: a second fill over what
 * this boot's iOS will have written would mix two generations of the same files, and the store is still there.
 */
static void
var_hfs_fill(void)
{
	const char *import = boot_arg("var_import");
	const char *what = "the skeleton";
	int fd;

	if (var_volume != VAR_ON_FLASH_NEW) {
		return;
	}
	if (import != NULL && strcmp(import, "1") == 0) {
		char *argv[] = { (char *)in_image("/bin/var_store"), (char *)"import", NULL };
		int status;

		printf("\n-- the data volume of the memory filesystem's days, onto the flash (var_import=1) --\n");
		status = var_hfs_run(argv);
		what = status == 0 ? "the skeleton and var_store's newest save" :
		    status == 3 ? "the skeleton; var_store had no save" :
		    status == 4 ? "the skeleton; NOTHING of var_store's saves checked out" :
		    "the skeleton; var_store import FAILED";
		check(status == 0 || status == 3, "var_store's newest save onto the volume on the flash");
	}
	fd = open(VAR_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		dprintf(fd, "filled with %s\n", what);
		fsync(fd);
		close(fd);
	}
	printf("  the volume on the flash: filled with %s\n", what);
	check(fd >= 0, "mark the data volume as filled");
	sync();
}

/* Every boot of the volume: its count, and the syncer. */
static void
var_hfs_keep(void)
{
	char text[32];
	ssize_t n;
	int fd;
	pid_t pid;

	if (var_volume == VAR_IN_MEMORY) {
		return;
	}
	var_boots = 0;
	fd = open(VAR_BOOTS, O_RDONLY, 0);
	if (fd >= 0) {
		n = read(fd, text, sizeof(text) - 1);
		text[n > 0 ? n : 0] = 0;
		for (char *p = text; *p >= '0' && *p <= '9'; p++) {
			var_boots = var_boots * 10 + (*p - '0');
		}
		close(fd);
	}
	var_boots++;
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
