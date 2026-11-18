/*
 * Where in the partition darwin a write may go (bsd/dev/hakios.ufs.c), as arithmetic alone, so that a program on
 * the machine that builds the kernel tries the same tests (tools/ufs-area-test in the port's repository).
 *
 * Blocks are darwin's own, 4096 bytes, counted from its start. Nothing below writes or locks; every test is a
 * subtraction or a comparison, so a huge first block or count from a caller cannot wrap into a small sum and pass.
 *
 *   [0, write_first)                  what the build writes from the computer: the iOS volume, both cryptexes.
 *                                     Never written from here.
 *   [var_first, var_first + var_blocks)
 *                                     /dev/ufs_var's slice (ufs_var_first=, ufs_var_blocks=): the data volume,
 *                                     a filesystem that writes. While it is on, only /dev/ufs_var writes there --
 *                                     a write of /dev/rufs_darwin (var_store, flash_stage) into it is refused, and
 *                                     /dev/ufs_var writes nowhere else.
 *   the rest of darwin past write_first
 *                                     /dev/rufs_darwin's: var_store's slots, the image flash_stage puts there.
 *
 * The areas follow another HakiOS port (APSL 2.0): telegdyd/ios-vayu, branch vayu-dev, commits 274f9827f52f (/dev/ufs_var) and
 * 86984b16b016 (a boot image staged in darwin).
 */
#ifndef _SYS_HAKIOS_UFS_AREA_H_
#define _SYS_HAKIOS_UFS_AREA_H_

#include <stdbool.h>
#include <stdint.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EROFS
#define EROFS 30
#endif

/* [first, first + count) lies inside [lo, lo + n) */
static inline bool
ufs_area_inside(uint64_t first, uint64_t count, uint64_t lo, uint64_t n)
{
	return first >= lo && first - lo <= n && count <= n - (first - lo);
}

/* [a, a + an) and [b, b + bn) have a block in common */
static inline bool
ufs_area_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
	if (an == 0 || bn == 0) {
		return false;
	}
	return a <= b ? b - a < an : a - b < bn;
}

/* Why /dev/ufs_var cannot be darwin's blocks [first, first + blocks); NULL: it can. */
static inline const char *
ufs_var_area_refusal(uint64_t first, uint64_t blocks, uint64_t write_first, uint64_t darwin_blocks)
{
	if (blocks == 0) {
		return "it is empty";
	}
	if (first < write_first) {
		return "it begins before ufs_write_first: the iOS volume and the cryptexes are there";
	}
	if (!ufs_area_inside(first, blocks, 0, darwin_blocks)) {
		return "it does not fit in darwin";
	}
	return NULL;
}

/*
 * A write of darwin's blocks [first, first + count): 0 if it may go, else EROFS (or EINVAL for no blocks at all).
 * from_var: the write is /dev/ufs_var's (the filesystem on the slice), not one of /dev/rufs_darwin's.
 */
static inline int
ufs_write_area_check(uint64_t first, uint64_t count, uint64_t write_first, uint64_t darwin_blocks,
    bool var_on, uint64_t var_first, uint64_t var_blocks, bool from_var)
{
	if (count == 0) {
		return EINVAL;
	}
	if (first < write_first || !ufs_area_inside(first, count, 0, darwin_blocks)) {
		return EROFS;           /* the iOS volume, the cryptexes, and anything past the partition's end: never */
	}
	if (from_var) {
		return var_on && ufs_area_inside(first, count, var_first, var_blocks) ? 0 : EROFS;
	}
	if (var_on && ufs_area_overlap(first, count, var_first, var_blocks)) {
		return EROFS;           /* the data volume's blocks belong to its filesystem alone */
	}
	return 0;
}

/*
 * The next piece of a write of len bytes (len > 0) at byte at of a device counted in blocks of nbs bytes, as
 * /dev/ufs_var's writes are cut (ufs_var_write): its length, and whether it is whole blocks -- written as they are --
 * or lies inside one block, which is read, patched from byte at % nbs on, and written whole.
 */
static inline uint64_t
ufs_area_piece(uint64_t at, uint64_t len, uint64_t nbs, bool *whole)
{
	uint64_t in = at % nbs;

	if (in == 0 && len >= nbs) {
		*whole = true;
		return len - len % nbs;
	}
	*whole = false;
	return nbs - in < len ? nbs - in : len;
}

#endif /* _SYS_HAKIOS_UFS_AREA_H_ */
