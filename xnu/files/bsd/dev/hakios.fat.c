/*
 * A read-only FAT16/FAT32 reader (sys/hakios.fat.h): the boot sector, the first FAT kept whole, directories
 * with their long names (VFAT), and a file read whole, a run of adjacent clusters at a time.
 *
 * Facts: the layout is Microsoft's "FAT: General Overview of On-Disk Format" (FAT32 File System
 * Specification 1.03): the BPB fields at the offsets read below, the cluster count deciding FAT12/16/32
 * (< 4085, < 65525, else), the long-name entries (attribute 0x0F, ordinal in bits 0-5 of byte 0, 0x40 on
 * the last, 13 UCS-2 characters at bytes 1, 14 and 28, the short name's checksum at byte 13). vayu's
 * "modem" and "bluetooth" partitions are FAT16 with 4096-byte sectors and 4 sectors to a cluster
 * (`file` and mtools on copies of the partitions, 18.11.2026); a long name is what holds
 * the lower case there ("modem.mdt" next to a short "MODEM.MDT").
 *
 * Every number read from the volume is checked before it is used as an offset: the clusters against
 * the count, a chain against looping (no more links than there are clusters), the areas and every read
 * against the partition's size. Nothing is written, nothing is allocated here.
 */
#include <sys/hakios.fat.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#include <sys/errno.h>
#else
#include <errno.h>
#include <string.h>
#endif
#ifndef EFTYPE
#define EFTYPE EINVAL                   /* (a host without BSD's "inappropriate file type") */
#endif

static uint32_t
le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t
le32(const uint8_t *p)
{
	return le16(p) | le16(p + 2) << 16;
}

static int
power_of_two(uint32_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

int
hakios_fat_open(struct hakios_fat *fs)
{
	uint8_t b[512];
	uint32_t bps, spc, reserved, fats, root_entries, total, fat_sectors, root_sectors;
	uint64_t meta;
	int error;

	if (fs->read == NULL || fs->size < sizeof(b)) {
		return EINVAL;
	}
	error = fs->read(fs->ctx, 0, b, sizeof(b));
	if (error != 0) {
		return error;
	}
	if (b[510] != 0x55 || b[511] != 0xaa || (b[0] != 0xeb && b[0] != 0xe9)) {
		return EFTYPE;
	}
	bps = le16(b + 11);
	spc = b[13];
	reserved = le16(b + 14);
	fats = b[16];
	root_entries = le16(b + 17);
	total = le16(b + 19) != 0 ? le16(b + 19) : le32(b + 32);
	fat_sectors = le16(b + 22) != 0 ? le16(b + 22) : le32(b + 36);
	if (bps < 512 || bps > 4096 || !power_of_two(bps) || !power_of_two(spc) || bps * spc > HAKIOS_FAT_MAX_CLUSTER ||
	    reserved == 0 || fats == 0 || fats > 4 || fat_sectors == 0 || total == 0) {
		return EFTYPE;
	}
	root_sectors = (root_entries * 32 + bps - 1) / bps;
	meta = (uint64_t)reserved + (uint64_t)fats * fat_sectors + root_sectors;
	if (meta >= total) {
		return EFTYPE;
	}
	fs->sector = bps;
	fs->cluster = bps * spc;
	fs->clusters = (uint32_t)(total - meta) / spc;
	if (fs->clusters < 4085) {
		return ENOTSUP;                         /* FAT12: none of the phone's partitions */
	}
	fs->type = fs->clusters < 65525 ? 16 : 32;
	if ((fs->type == 16) != (root_entries != 0) || (fs->type == 16 && le16(b + 22) == 0)) {
		return EFTYPE;
	}
	fs->fat_at = (uint64_t)reserved * bps;
	fs->root_at = ((uint64_t)reserved + (uint64_t)fats * fat_sectors) * bps;
	fs->root_bytes = root_entries * 32;
	fs->data_at = meta * bps;
	fs->root_cluster = fs->type == 32 ? le32(b + 44) & 0x0fffffff : 0;
	if (fs->type == 32 && (fs->root_cluster < 2 || fs->root_cluster > fs->clusters + 1)) {
		return EFTYPE;
	}
	if (((uint64_t)fs->clusters + 2) * (fs->type / 8) > HAKIOS_FAT_MAX_FAT) {
		return EFBIG;
	}
	fs->fat_bytes = (fs->clusters + 2) * (fs->type / 8);
	if (fs->fat_bytes > (uint64_t)fat_sectors * bps) {
		return EFTYPE;
	}
	/*
	 * A volume may say it is bigger than its partition: vayu's "bluetooth" says 64 MB in 1 MB (its files are all in
	 * the first). What is read must be in the partition; the clusters past it are refused when a file reaches them.
	 */
	if (fs->fat_at + fs->fat_bytes > fs->size || fs->root_at + fs->root_bytes > fs->size || fs->data_at > fs->size) {
		return EFTYPE;
	}
	fs->fat = NULL;
	return 0;
}

int
hakios_fat_load(struct hakios_fat *fs, uint8_t *fat, uint32_t fat_bytes)
{
	int error;

	if (fs->fat_bytes == 0 || fat_bytes < fs->fat_bytes) {
		return EINVAL;
	}
	error = fs->read(fs->ctx, fs->fat_at, fat, fs->fat_bytes);
	if (error == 0) {
		fs->fat = fat;
	}
	return error;
}

/* The link after cluster c: another cluster (2 .. clusters + 1), 1 at the chain's end, 0 if it is broken. */
static uint32_t
fat_next(const struct hakios_fat *fs, uint32_t c)
{
	uint32_t next;

	if (c < 2 || c > fs->clusters + 1) {
		return 0;
	}
	if (fs->type == 16) {
		next = le16(fs->fat + 2 * c);
		if (next >= 0xfff8) {
			return 1;
		}
	} else {
		next = le32(fs->fat + 4 * c) & 0x0fffffff;
		if (next >= 0x0ffffff8) {
			return 1;
		}
	}
	return next >= 2 && next <= fs->clusters + 1 ? next : 0;
}

static uint64_t
cluster_at(const struct hakios_fat *fs, uint32_t c)
{
	return fs->data_at + (uint64_t)(c - 2) * fs->cluster;
}

static char
lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int
same_name(const char *a, const char *b, size_t b_len)
{
	size_t i;

	for (i = 0; i < b_len; i++) {
		if (a[i] == 0 || lower(a[i]) != lower(b[i])) {
			return 0;
		}
	}
	return a[i] == 0;
}

int
hakios_fat_path_ok(const char *path)
{
	size_t length = 0, component = 0;
	const char *start = path;

	if (path == NULL || *path == 0 || *path == '/') {
		return 0;
	}
	for (const char *c = path;; c++, length++) {
		if (length >= HAKIOS_FAT_PATH_MAX) {
			return 0;
		}
		if (*c == '/' || *c == 0) {
			component = (size_t)(c - start);
			if (component == 0 || component > 255 || (component == 1 && start[0] == '.') ||
			    (component == 2 && start[0] == '.' && start[1] == '.')) {
				return 0;
			}
			if (*c == 0) {
				return 1;
			}
			start = c + 1;
			continue;
		}
		if ((unsigned char)*c <= 0x20 || (unsigned char)*c >= 0x7f || *c == '\\') {
			return 0;
		}
	}
}

/* The long name being gathered, in ASCII (any other character is 0x7f, which no wanted name has). */
struct long_name {
	char text[20 * 13 + 1];
	uint32_t want;                  /* the ordinal the next entry must have; 0: none being gathered */
	uint8_t sum;
	int valid;
};

static void
long_name_part(struct long_name *ln, const uint8_t *e)
{
	static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	uint32_t ordinal = e[0] & 0x3f;

	if (e[0] & 0x40) {
		ln->valid = ordinal >= 1 && ordinal <= 20;
		ln->want = ordinal;
		ln->sum = e[13];
		memset(ln->text, 0, sizeof(ln->text));
	} else if (!ln->valid || ordinal != ln->want || e[13] != ln->sum) {
		ln->valid = 0;
	}
	if (!ln->valid) {
		return;
	}
	for (uint32_t i = 0; i < 13; i++) {
		uint32_t u = le16(e + at[i]);
		char *to = &ln->text[(ordinal - 1) * 13 + i];

		if (u == 0 || u == 0xffff) {
			break;
		}
		*to = u < 0x80 ? (char)u : 0x7f;
	}
	ln->want = ordinal - 1;
}

static void
short_name(const uint8_t *e, char out[13])
{
	unsigned n = 0;

	for (unsigned i = 0; i < 8 && e[i] != ' '; i++) {
		out[n++] = (char)(i == 0 && e[0] == 0x05 ? 0xe5 : e[i]);
	}
	if (e[8] != ' ') {
		out[n++] = '.';
		for (unsigned i = 8; i < 11 && e[i] != ' '; i++) {
			out[n++] = (char)e[i];
		}
	}
	out[n] = 0;
}

/*
 * One block of directory entries looked through for name: 1 if found (*file filled), -1 at the
 * directory's end, 0 to go on.
 */
static int
entries_find(const uint8_t *entries, uint32_t bytes, struct long_name *ln, const char *name, size_t name_len,
    uint32_t *skip, const struct hakios_fat *fs, struct hakios_fat_file *file)
{
	for (uint32_t at = 0; at + 32 <= bytes; at += 32) {
		const uint8_t *e = entries + at;
		uint8_t sum = 0;
		char brief[13];
		int match;

		if (e[0] == 0) {
			return -1;
		}
		if (e[0] == 0xe5) {
			ln->valid = 0;
			continue;
		}
		if ((e[11] & 0x3f) == 0x0f) {
			long_name_part(ln, e);
			continue;
		}
		if (e[11] & 0x08) {
			ln->valid = 0;                  /* the volume's label */
			continue;
		}
		for (unsigned i = 0; i < 11; i++) {
			sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + e[i]);
		}
		short_name(e, brief);
		match = same_name(brief, name, name_len);
		if (!match && ln->valid && ln->want == 0 && ln->sum == sum) {
			match = same_name(ln->text, name, name_len);
		}
		ln->valid = 0;
		if (match && *skip > 0) {
			(*skip)--;                      /* an earlier one of the same name, already tried */
			continue;
		}
		if (match) {
			file->first = le16(e + 26) | (fs->type == 32 ? le16(e + 20) << 16 : 0);
			file->size = le32(e + 28);
			file->directory = (e[11] & 0x10) != 0;
			return 1;
		}
	}
	return 0;
}

static int
directory_find(struct hakios_fat *fs, const struct hakios_fat_file *dir, const char *name, size_t name_len,
    uint32_t skip, uint8_t *scratch, uint32_t scratch_size, struct hakios_fat_file *file)
{
	struct long_name ln;
	int found = 0, error;

	memset(&ln, 0, sizeof(ln));
	if (dir->first == 0 && fs->type == 16) {
		uint32_t chunk = scratch_size & ~31u;   /* the fixed root of FAT16 */

		if (chunk == 0) {
			return EINVAL;
		}
		for (uint32_t off = 0; off < fs->root_bytes && found == 0; off += chunk) {
			uint32_t n = fs->root_bytes - off < chunk ? fs->root_bytes - off : chunk;

			error = fs->read(fs->ctx, fs->root_at + off, scratch, n);
			if (error != 0) {
				return error;
			}
			found = entries_find(scratch, n, &ln, name, name_len, &skip, fs, file);
		}
	} else {
		uint32_t c = dir->first == 0 ? fs->root_cluster : dir->first, steps = 0;

		if (scratch_size < fs->cluster) {
			return EINVAL;
		}
		while (found == 0) {
			if (c < 2 || c > fs->clusters + 1 || steps++ > fs->clusters || cluster_at(fs, c) + fs->cluster > fs->size) {
				return EIO;
			}
			error = fs->read(fs->ctx, cluster_at(fs, c), scratch, fs->cluster);
			if (error != 0) {
				return error;
			}
			found = entries_find(scratch, fs->cluster, &ln, name, name_len, &skip, fs, file);
			if (found == 0) {
				c = fat_next(fs, c);
				if (c == 1) {
					found = -1;
				} else if (c == 0) {
					return EIO;
				}
			}
		}
	}
	return found == 1 ? 0 : ENOENT;
}

/*
 * path below directory dir. A directory may hold two entries of one name: vayu's modem partition has two
 * "sm8150.p" in modem_pr/mcfg/configs/mcfg_sw/generic/common/default/, each with other files below it (mtools
 * shows both). So every entry of the name is tried in turn, as many as *budget directory searches allow.
 */
static int
lookup_below(struct hakios_fat *fs, const struct hakios_fat_file *dir, const char *path, uint8_t *scratch,
    uint32_t scratch_size, struct hakios_fat_file *file, uint32_t *budget)
{
	const char *slash = strchr(path, '/');
	size_t n = slash != NULL ? (size_t)(slash - path) : strlen(path);
	int error = ENOENT;

	for (uint32_t skip = 0; *budget > 0; skip++) {
		struct hakios_fat_file next;

		(*budget)--;
		error = directory_find(fs, dir, path, n, skip, scratch, scratch_size, &next);
		if (error != 0) {
			return error;                   /* (ENOENT: no more of that name) */
		}
		if (next.directory && next.first == 0 && fs->type == 32) {
			next.first = fs->root_cluster;  /* ".." of a first-level directory says 0 for the root */
		}
		if (slash == NULL) {
			if (next.directory) {
				error = EISDIR;
				continue;
			}
			*file = next;
			return 0;
		}
		if (!next.directory) {
			error = ENOTDIR;
			continue;
		}
		if (next.first == 0 && fs->type == 16) {
			error = ENOENT;                 /* a ".." back to the root, or a broken entry: never followed down */
			continue;
		}
		error = lookup_below(fs, &next, slash + 1, scratch, scratch_size, file, budget);
		if (error == 0) {
			return 0;
		}
	}
	return error == 0 ? ENOENT : error;
}

int
hakios_fat_lookup(struct hakios_fat *fs, const char *path, uint8_t *scratch, uint32_t scratch_size,
    struct hakios_fat_file *file)
{
	struct hakios_fat_file root = { .first = 0, .size = 0, .directory = 1 };
	uint32_t budget = 64;                   /* directory searches for one lookup: vayu's deepest path needs 12 */

	if (fs->fat == NULL || !hakios_fat_path_ok(path)) {
		return EINVAL;
	}
	return lookup_below(fs, &root, path, scratch, scratch_size, file, &budget);
}

int
hakios_fat_read(struct hakios_fat *fs, const struct hakios_fat_file *file, uint8_t *dst, uint64_t len)
{
	uint64_t left = file->size;
	uint32_t c = file->first, used = 0;

	if (fs->fat == NULL || file->directory || len != file->size) {
		return EINVAL;
	}
	if (left == 0) {
		return 0;
	}
	if ((uint64_t)fs->clusters * fs->cluster < left) {
		return EIO;
	}
	while (left > 0) {
		uint32_t start = c, run = 1, next;
		uint64_t bytes;
		int error;

		if (c < 2 || c > fs->clusters + 1) {
			return EIO;
		}
		/* adjacent clusters are one read: the storage path splits it as it must */
		while ((uint64_t)run * fs->cluster < left && (next = fat_next(fs, c)) == c + 1) {
			c = next;
			run++;
		}
		used += run;
		if (used > fs->clusters) {
			return EIO;                     /* a chain that loops */
		}
		bytes = (uint64_t)run * fs->cluster < left ? (uint64_t)run * fs->cluster : left;
		if (cluster_at(fs, start) + bytes > fs->size) {
			return EIO;
		}
		error = fs->read(fs->ctx, cluster_at(fs, start), dst, bytes);
		if (error != 0) {
			return error;
		}
		dst += bytes;
		left -= bytes;
		if (left > 0) {
			c = fat_next(fs, c);
			if (c < 2) {
				return EIO;             /* the chain ends (or breaks) before the file does */
			}
		}
	}
	return 0;
}
