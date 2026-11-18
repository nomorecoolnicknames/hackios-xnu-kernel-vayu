/*
 * Host tests of bsd/sys/hakios.ufs_area.h: where in darwin bsd/dev/hakios.ufs.c lets a write go. The numbers are
 * vayu's (darwin sda33, 6291707 blocks; ufs_write_first 2097152) and darwin's layout (devices/poco-x3-pro/tools/darwin-layout.py):
 * var_store's slots 2097152.., /dev/ufs_var 3145728 + 2097152, the staging area 5242880 + 32768, its header 5275648.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/hakios.ufs_area.h>

static int checks, failures;

static void
check(bool ok, const char *format, ...)
{
	va_list list;

	checks++;
	if (ok) {
		return;
	}
	failures++;
	printf("FAIL: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

#define DARWIN          6291707ULL
#define WRITE_FIRST     2097152ULL
#define VAR_FIRST       3145728ULL
#define VAR_BLOCKS      2097152ULL
#define STAGE_FIRST     5242880ULL
#define STAGE_BLOCKS    32768ULL
#define STAGE_HEAD      5275648ULL

static int
raw(uint64_t first, uint64_t count, bool var_on)
{
	return ufs_write_area_check(first, count, WRITE_FIRST, DARWIN, var_on, VAR_FIRST, VAR_BLOCKS, false);
}

static int
var(uint64_t first, uint64_t count, bool var_on)
{
	return ufs_write_area_check(first, count, WRITE_FIRST, DARWIN, var_on, VAR_FIRST, VAR_BLOCKS, true);
}

static void
test_inside_overlap(void)
{
	check(ufs_area_inside(0, 0, 0, 0), "an empty area is inside an empty one");
	check(ufs_area_inside(10, 5, 10, 5), "an area is inside itself");
	check(!ufs_area_inside(9, 5, 10, 5), "one block before");
	check(!ufs_area_inside(11, 5, 10, 5), "one block past");
	check(ufs_area_inside(15, 0, 10, 5), "nothing at the end is inside");
	check(!ufs_area_inside(16, 0, 10, 5), "nothing past the end is not");
	check(!ufs_area_inside(UINT64_MAX, 2, 0, UINT64_MAX), "a sum that would wrap does not pass");
	check(!ufs_area_inside(1, UINT64_MAX, 0, UINT64_MAX), "nor a count that would");
	check(ufs_area_inside(0, UINT64_MAX, 0, UINT64_MAX), "the whole of a 64-bit space is inside itself");

	check(!ufs_area_overlap(0, 10, 10, 10), "touching is not overlapping (after)");
	check(!ufs_area_overlap(10, 10, 0, 10), "touching is not overlapping (before)");
	check(ufs_area_overlap(0, 11, 10, 10), "one block in common (after)");
	check(ufs_area_overlap(19, 1, 10, 10), "one block in common (the last)");
	check(ufs_area_overlap(10, 10, 10, 10), "the same area");
	check(ufs_area_overlap(0, 100, 10, 10), "one inside the other");
	check(!ufs_area_overlap(10, 0, 10, 10) && !ufs_area_overlap(10, 10, 15, 0), "an empty area overlaps nothing");
	check(ufs_area_overlap(UINT64_MAX - 1, 1, 0, UINT64_MAX), "the last block of the space, without wrapping");
	check(!ufs_area_overlap(UINT64_MAX - 1, 1, 0, UINT64_MAX - 1), "and one past an area that ends before it");
}

static void
test_var_area(void)
{
	check(ufs_var_area_refusal(VAR_FIRST, VAR_BLOCKS, WRITE_FIRST, DARWIN) == NULL, "the layout's slice is taken");
	check(ufs_var_area_refusal(WRITE_FIRST, DARWIN - WRITE_FIRST, WRITE_FIRST, DARWIN) == NULL,
	    "all of darwin past write_first is taken");
	check(ufs_var_area_refusal(VAR_FIRST, 0, WRITE_FIRST, DARWIN) != NULL, "an empty slice is refused");
	check(ufs_var_area_refusal(WRITE_FIRST - 1, 10, WRITE_FIRST, DARWIN) != NULL,
	    "a slice over the last block before write_first is refused");
	check(ufs_var_area_refusal(0, VAR_BLOCKS, WRITE_FIRST, DARWIN) != NULL, "a slice over the iOS volume is refused");
	check(ufs_var_area_refusal(DARWIN - 10, 11, WRITE_FIRST, DARWIN) != NULL, "a slice one block past darwin is refused");
	check(ufs_var_area_refusal(DARWIN, 1, WRITE_FIRST, DARWIN) != NULL, "a slice past darwin is refused");
	check(ufs_var_area_refusal(UINT64_MAX - 4, 8, WRITE_FIRST, DARWIN) != NULL, "a slice that wraps is refused");
	check(ufs_var_area_refusal(VAR_FIRST, UINT64_MAX, WRITE_FIRST, DARWIN) != NULL, "a huge count is refused");
}

static void
test_writes(void)
{
	/* /dev/rufs_darwin: var_store's slots, the staging area, its header; never below write_first or past darwin */
	check(raw(WRITE_FIRST, 524288, true) == 0, "var_store's slot 0");
	check(raw(WRITE_FIRST + 524288, 524288, true) == 0, "var_store's slot 1, up to the slice");
	check(raw(STAGE_FIRST, STAGE_BLOCKS, true) == 0, "the staging area");
	check(raw(STAGE_HEAD, 1, true) == 0, "the staging header");
	check(raw(DARWIN - 1, 1, true) == 0, "darwin's last block");
	check(raw(DARWIN - 1, 2, true) == EROFS, "past darwin's last block");
	check(raw(DARWIN, 1, true) == EROFS, "past darwin");
	check(raw(WRITE_FIRST - 1, 1, true) == EROFS, "the block before write_first (the layout block)");
	check(raw(0, 1, false) == EROFS, "the iOS volume, without /dev/ufs_var too");
	check(raw(2039808, 2560, false) == EROFS, "the App cryptex");
	check(raw(WRITE_FIRST - 1, 2, true) == EROFS, "a write that begins before write_first and ends past it");
	check(raw(UINT64_MAX, 1, true) == EROFS && raw(WRITE_FIRST, UINT64_MAX, true) == EROFS, "wrapping writes");
	check(raw(WRITE_FIRST, 0, true) == EINVAL, "no blocks at all");

	/* the slice is /dev/ufs_var's while it is on, and only then */
	check(raw(VAR_FIRST, 1, true) == EROFS, "the slice's first block, from /dev/rufs_darwin");
	check(raw(VAR_FIRST + VAR_BLOCKS - 1, 1, true) == EROFS, "the slice's last block, from /dev/rufs_darwin");
	check(raw(VAR_FIRST - 1, 2, true) == EROFS, "a write into the slice from below");
	check(raw(VAR_FIRST + VAR_BLOCKS - 1, 2, true) == EROFS, "a write out of the slice's end");
	check(raw(VAR_FIRST - 1, 1, true) == 0 && raw(VAR_FIRST + VAR_BLOCKS, 1, true) == 0, "the blocks either side");
	check(raw(WRITE_FIRST, DARWIN - WRITE_FIRST, true) == EROFS, "all of darwin at once, the slice in it");
	check(raw(VAR_FIRST, 1, false) == 0, "the slice from /dev/rufs_darwin when there is no /dev/ufs_var");

	check(var(VAR_FIRST, VAR_BLOCKS, true) == 0, "all of the slice from /dev/ufs_var");
	check(var(VAR_FIRST + VAR_BLOCKS - 1, 1, true) == 0, "its last block");
	check(var(VAR_FIRST + VAR_BLOCKS - 1, 2, true) == EROFS, "past its end, into the staging area's side");
	check(var(VAR_FIRST - 1, 1, true) == EROFS, "var_store's last block from /dev/ufs_var");
	check(var(STAGE_HEAD, 1, true) == EROFS, "the staging header from /dev/ufs_var");
	check(var(VAR_FIRST, 1, false) == EROFS, "/dev/ufs_var when it is off");
	check(var(0, 1, true) == EROFS, "the iOS volume from /dev/ufs_var");
	check(ufs_write_area_check(0, 1, 0, DARWIN, true, 0, VAR_BLOCKS, true) == 0,
	    "(the arithmetic alone: a slice at 0 with write_first 0)");
}

/*
 * /dev/ufs_var's writes cut as the kernel cuts them (ufs_area_piece) on a disk of 4096-byte blocks that only takes
 * whole blocks at block boundaries: every write, of any place and length, has to leave the disk as a plain copy
 * would, and nothing else may go to the disk.
 */
#define SIM_BLOCK       4096u
#define SIM_BLOCKS      64u

static uint8_t sim_disk[SIM_BLOCK * SIM_BLOCKS], sim_want[SIM_BLOCK * SIM_BLOCKS], sim_src[SIM_BLOCK * 8];
static int sim_bad;

static void
sim_disk_write(uint64_t at, const uint8_t *p, uint64_t n)
{
	if (at % SIM_BLOCK != 0 || n % SIM_BLOCK != 0 || n == 0 || at + n > sizeof(sim_disk)) {
		sim_bad++;      /* what the loader would refuse */
		return;
	}
	memcpy(sim_disk + at, p, n);
}

static void
sim_var_write(uint64_t at, const uint8_t *src, uint64_t len)
{
	static uint8_t block[SIM_BLOCK];

	while (len > 0) {
		uint64_t in = at % SIM_BLOCK, n;
		bool whole;

		n = ufs_area_piece(at, len, SIM_BLOCK, &whole);
		if (n == 0 || n > len) {
			sim_bad++;
			return;
		}
		if (whole) {
			sim_disk_write(at, src, n);
		} else {
			if (in + n > SIM_BLOCK) {
				sim_bad++;
				return;
			}
			memcpy(block, sim_disk + (at - in), SIM_BLOCK);
			memcpy(block + in, src, n);
			sim_disk_write(at - in, block, SIM_BLOCK);
		}
		at += n;
		src += n;
		len -= n;
	}
}

static void
test_pieces(void)
{
	uint64_t seed = 12345;
	int same = 1;

	for (size_t i = 0; i < sizeof(sim_disk); i++) {
		sim_disk[i] = sim_want[i] = (uint8_t)(i * 7 + 3);
	}
	for (int round = 0; round < 20000; round++) {
		uint64_t at, len;

		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		/* places and lengths of every kind: 512-byte ones, odd ones, whole blocks, across several blocks */
		switch ((seed >> 60) & 3) {
		case 0:
			at = ((seed >> 20) % (SIM_BLOCKS * 8)) * 512;
			len = 512 * (1 + (seed >> 40) % 16);
			break;
		case 1:
			at = ((seed >> 20) % (SIM_BLOCKS - 8)) * SIM_BLOCK;
			len = SIM_BLOCK * (1 + (seed >> 40) % 8);
			break;
		default:
			at = (seed >> 20) % (sizeof(sim_disk) - sizeof(sim_src));
			len = 1 + (seed >> 36) % sizeof(sim_src);
			break;
		}
		if (at + len > sizeof(sim_disk)) {
			continue;
		}
		for (uint64_t i = 0; i < len; i++) {
			sim_src[i] = (uint8_t)(round + i * 13);
		}
		memcpy(sim_want + at, sim_src, len);
		sim_var_write(at, sim_src, len);
		if (memcmp(sim_disk, sim_want, sizeof(sim_disk)) != 0) {
			same = 0;
			printf("FAIL: write of %llu bytes at %llu: the disk differs from a plain copy\n",
			    (unsigned long long)len, (unsigned long long)at);
			break;
		}
	}
	check(same, "20000 writes cut into pieces leave the disk as plain copies would");
	check(sim_bad == 0, "and nothing that is not whole blocks at block boundaries went to the disk (%d)", sim_bad);
	{
		bool whole;

		check(ufs_area_piece(0, 8192, 4096, &whole) == 8192 && whole, "two whole blocks in one piece");
		check(ufs_area_piece(0, 8704, 4096, &whole) == 8192 && whole, "and the 512 bytes after them apart");
		check(ufs_area_piece(512, 512, 4096, &whole) == 512 && !whole, "512 bytes inside a block");
		check(ufs_area_piece(3584, 1024, 4096, &whole) == 512 && !whole, "up to the block's end first");
		check(ufs_area_piece(4096, 100, 4096, &whole) == 100 && !whole, "less than a block at its start");
	}
}

int
main(void)
{
	test_inside_overlap();
	test_var_area();
	test_writes();
	test_pieces();
	printf("ufs-area-test: %d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
