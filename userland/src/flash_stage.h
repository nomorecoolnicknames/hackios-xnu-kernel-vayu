/*
 * What flash_stage (src/flash_stage.c) and the loader agree on, as arithmetic alone, so that a program on the
 * machine that builds the images tries the same tests (tools/flash-stage-test).
 *
 * Where the image goes is darwin's layout, not a number of this file's: the loader reads the layout block that
 * the installer wrote (devices/poco-x3-pro/tools/darwin-layout.py) and puts it on the kernel's
 * command line as ufs_stage_first= (darwin's block the image begins at), ufs_stage_blocks= (how many it may take)
 * and ufs_stage_head= (the block of the header). Without them -- a phone without the layout -- nothing is staged.
 *
 * The header, in the first bytes of block ufs_stage_head, the rest of the block zero (vayu-dev's loader 2.8 reads
 * the first 32 bytes the same): "XNUFLASH", the first block, the image's length in bytes, FNV-1a 64 of those bytes;
 * then "FORBOOT!", the length of `boot` and FNV-1a 64 of the whole of it as it is when the image is staged; little
 * endian. The loader takes a stage only if first_block is ufs_stage_first, the bytes fit there and in `boot`, the
 * sum is right, the image begins with "ANDROID!" and `boot` is still the one it was staged against (a stage left
 * behind is not written by the next image that happens to say flash_stage=1: that one clears it); it clears the
 * header before it writes `boot`. A header without "FORBOOT!" (this file's first form) it leaves alone.
 *
 * flash_stage_target=recovery on the command line (the multiboot: HakiOS in the recovery partition, Android in
 * boot): the same with the recovery in boot's place -- the image summed against /dev/rufs_recovery, the header tied
 * to it by "FORRECV!" instead of "FORBOOT!". The loader then writes the recovery and never boot, and an image whose
 * target is boot clears a "FORRECV!" header (as one whose target is the recovery clears a "FORBOOT!" one).
 *
 * From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commit 86984b16b016 (its flash_stage.c).
 */
#ifndef FLASH_STAGE_H
#define FLASH_STAGE_H

#include <stddef.h>
#include <stdint.h>

#define FLASH_STAGE_BLOCK       4096
#define FLASH_STAGE_MAGIC       "XNUFLASH"
#define FLASH_STAGE_BOOT_MAGIC  "FORBOOT!"
#define FLASH_STAGE_RECOVERY_MAGIC "FORRECV!"
#define FLASH_STAGE_MIN_BYTES   4096                    /* an Android boot image's header page at least */

struct flash_stage_head {
	char magic[8];                  /* FLASH_STAGE_MAGIC, no terminating zero */
	uint64_t first_block;
	uint64_t bytes;
	uint64_t fnv;
	char boot_magic[8];             /* FLASH_STAGE_BOOT_MAGIC, or FLASH_STAGE_RECOVERY_MAGIC */
	uint64_t boot_bytes;            /* `boot`'s length when the image was staged */
	uint64_t boot_fnv;              /* FNV-1a 64 of all of `boot` then */
};

struct flash_stage_layout {
	uint64_t first, blocks, head;   /* ufs_stage_first, ufs_stage_blocks, ufs_stage_head */
	uint64_t write_first;           /* ufs_write_first, 0 if not given */
	uint64_t var_first, var_blocks; /* ufs_var_first, ufs_var_blocks, 0 if not given */
};

static inline uint64_t
flash_stage_fnv(uint64_t h, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

#define FLASH_STAGE_FNV_START   0xcbf29ce484222325ULL

/*
 * A number of the command line: decimal, or hex after 0x. What the kernel's PE_parse_boot_argn reads otherwise
 * (octal after a leading 0, binary after 0b, a sign) is not taken for one: the two would not agree on it.
 */
static inline int
flash_stage_number(const char *p, size_t len, uint64_t *value)
{
	uint64_t v = 0;
	size_t i = 0;
	unsigned base = 10;

	if (len > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		i = 2;
	} else if (len > 1 && p[0] == '0') {
		return 0;
	}
	if (i == len) {
		return 0;
	}
	for (; i < len; i++) {
		char c = p[i];
		unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
		    c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;

		if (d >= base || v > (UINT64_MAX - d) / base) {
			return 0;
		}
		v = v * base + d;
	}
	*value = v;
	return 1;
}

/*
 * NAME=number in ARGS (the kernel's command line): 1 and the value, 0 if it is not there, -1 if it is there more than
 * once with two values or is not a number (the kernel takes the last of two, a reader of kern.bootargs the first).
 */
static inline int
flash_stage_arg(const char *args, const char *name, uint64_t *value)
{
	size_t n = 0;
	int found = 0;

	while (name[n] != 0) {
		n++;
	}
	for (const char *p = args; p != NULL && *p != 0;) {
		const char *end;
		uint64_t v;
		size_t i;

		while (*p == ' ' || *p == '\t') {
			p++;
		}
		for (end = p; *end != 0 && *end != ' ' && *end != '\t'; end++) {
		}
		for (i = 0; i < n && p + i < end && p[i] == name[i]; i++) {
		}
		if (i == n && p + n < end && p[n] == '=') {
			if (!flash_stage_number(p + n + 1, (size_t)(end - p) - n - 1, &v) || (found && v != *value)) {
				return -1;
			}
			*value = v;
			found = 1;
		}
		p = end;
	}
	return found;
}

/*
 * Which partition a stage is for: flash_stage_target=boot or =recovery on the command line, boot when there is none.
 * NULL and *recovery, or why not (another value, or two of them: the kernel takes the last, a reader the first).
 */
static inline const char *
flash_stage_target(const char *args, int *recovery)
{
	static const char key[] = "flash_stage_target=";
	int found = 0;

	*recovery = 0;
	for (const char *p = args; p != NULL && *p != 0;) {
		const char *end;
		size_t i;

		while (*p == ' ' || *p == '\t') {
			p++;
		}
		for (end = p; *end != 0 && *end != ' ' && *end != '\t'; end++) {
		}
		for (i = 0; i < sizeof(key) - 1 && p + i < end && p[i] == key[i]; i++) {
		}
		if (i == sizeof(key) - 1) {
			const char *v = p + i;
			size_t n = (size_t)(end - v);
			int r;

			if (n == 4 && v[0] == 'b' && v[1] == 'o' && v[2] == 'o' && v[3] == 't') {
				r = 0;
			} else if (n == 8 && v[0] == 'r' && v[1] == 'e' && v[2] == 'c' && v[3] == 'o' && v[4] == 'v' && v[5] == 'e' &&
			    v[6] == 'r' && v[7] == 'y') {
				r = 1;
			} else {
				return "flash_stage_target= is neither boot nor recovery";
			}
			if (found && r != *recovery) {
				return "flash_stage_target= is on the command line twice with two values";
			}
			*recovery = r;
			found = 1;
		}
		p = end;
	}
	return NULL;
}

/* [a, a + an) and [b, b + bn) have a block in common */
static inline int
flash_stage_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
	if (an == 0 || bn == 0) {
		return 0;
	}
	return a <= b ? b - a < an : a - b < bn;
}

/* The layout from the command line: NULL, or why there is none to stage into. */
static inline const char *
flash_stage_layout(const char *args, struct flash_stage_layout *l)
{
	int a = flash_stage_arg(args, "ufs_stage_first", &l->first);
	int b = flash_stage_arg(args, "ufs_stage_blocks", &l->blocks);
	int c = flash_stage_arg(args, "ufs_stage_head", &l->head);

	if (a < 0 || b < 0 || c < 0) {
		return "ufs_stage_first/blocks/head are on the command line twice with two values, or not numbers";
	}
	if (a == 0 || b == 0 || c == 0) {
		return "no ufs_stage_first/blocks/head on the command line: the loader read no layout of darwin";
	}
	l->write_first = l->var_first = l->var_blocks = 0;
	if (flash_stage_arg(args, "ufs_write_first", &l->write_first) < 0 ||
	    flash_stage_arg(args, "ufs_var_first", &l->var_first) < 0 ||
	    flash_stage_arg(args, "ufs_var_blocks", &l->var_blocks) < 0) {
		return "ufs_write_first or ufs_var_* are on the command line twice with two values";
	}
	if (l->blocks == 0 || l->blocks > UINT64_MAX / FLASH_STAGE_BLOCK ||
	    l->first > UINT64_MAX / FLASH_STAGE_BLOCK - l->blocks ||
	    l->head > UINT64_MAX / FLASH_STAGE_BLOCK - 1) {
		return "the staging area is empty or past any partition";
	}
	if (l->write_first == 0 || l->first < l->write_first || l->head < l->write_first) {
		return "the staging area is not past ufs_write_first (or there is none: nothing of darwin is written)";
	}
	if (flash_stage_overlap(l->head, 1, l->first, l->blocks)) {
		return "the header's block is inside the staging area";
	}
	if (flash_stage_overlap(l->first, l->blocks, l->var_first, l->var_blocks) ||
	    flash_stage_overlap(l->head, 1, l->var_first, l->var_blocks)) {
		return "the staging area is inside /dev/ufs_var's slice (the data volume)";
	}
	return NULL;
}

/* The image: NULL, or why the loader would not take it. boot_bytes: the size of `boot`, 0 if not known. */
static inline const char *
flash_stage_image(const uint8_t *img, uint64_t bytes, const struct flash_stage_layout *l, uint64_t boot_bytes)
{
	if (bytes < FLASH_STAGE_MIN_BYTES) {
		return "it is too short to be a boot image";
	}
	if (bytes > l->blocks * FLASH_STAGE_BLOCK) {
		return "it does not fit in the staging area";
	}
	if (boot_bytes != 0 && bytes > boot_bytes) {
		return "it does not fit in boot";
	}
	for (int i = 0; i < 8; i++) {
		if (img[i] != (uint8_t)"ANDROID!"[i]) {
			return "it is not an Android boot image (no ANDROID! at its start)";
		}
	}
	return NULL;
}

/* The header block for an image staged against a boot of boot_bytes whose sum is boot_fnv (recovery: against the
 * recovery, so summed): FLASH_STAGE_BLOCK bytes at out, zero but for the header. */
static inline void
flash_stage_head_block_for(uint8_t *out, const struct flash_stage_layout *l, const uint8_t *img, uint64_t bytes,
    uint64_t boot_bytes, uint64_t boot_fnv, int recovery)
{
	const char *tie = recovery ? FLASH_STAGE_RECOVERY_MAGIC : FLASH_STAGE_BOOT_MAGIC;
	struct flash_stage_head h;

	for (size_t i = 0; i < FLASH_STAGE_BLOCK; i++) {
		out[i] = 0;
	}
	for (int i = 0; i < 8; i++) {
		h.magic[i] = FLASH_STAGE_MAGIC[i];
	}
	h.first_block = l->first;
	h.bytes = bytes;
	h.fnv = flash_stage_fnv(FLASH_STAGE_FNV_START, img, (size_t)bytes);
	for (int i = 0; i < 8; i++) {
		h.boot_magic[i] = tie[i];
	}
	h.boot_bytes = boot_bytes;
	h.boot_fnv = boot_fnv;
	for (size_t i = 0; i < sizeof(h); i++) {
		out[i] = ((const uint8_t *)&h)[i];
	}
}

static inline void
flash_stage_head_block(uint8_t *out, const struct flash_stage_layout *l, const uint8_t *img, uint64_t bytes,
    uint64_t boot_bytes, uint64_t boot_fnv)
{
	flash_stage_head_block_for(out, l, img, bytes, boot_bytes, boot_fnv, 0);
}

/* A header block as the loader reads it: 1 and the header if it holds a stage of this layout, else 0. */
static inline int
flash_stage_head_read(const uint8_t *block, const struct flash_stage_layout *l, struct flash_stage_head *h)
{
	for (size_t i = 0; i < sizeof(*h); i++) {
		((uint8_t *)h)[i] = block[i];
	}
	for (int i = 0; i < 8; i++) {
		if (h->magic[i] != FLASH_STAGE_MAGIC[i]) {
			return 0;
		}
	}
	return h->first_block == l->first && h->bytes >= FLASH_STAGE_MIN_BYTES && h->bytes <= l->blocks * FLASH_STAGE_BLOCK;
}

/* A stage's header (flash_stage_head_read) tied to a boot (recovery: to the recovery): 1 if staged against
 * boot_bytes summing to boot_fnv, 0 if against another, -1 if against none (the first form: the loader leaves it
 * alone), -2 if tied to the other partition (the loader clears it, writes nothing) */
static inline int
flash_stage_head_for_target(const struct flash_stage_head *h, uint64_t boot_bytes, uint64_t boot_fnv, int recovery)
{
	const char *tie = recovery ? FLASH_STAGE_RECOVERY_MAGIC : FLASH_STAGE_BOOT_MAGIC;
	const char *other = recovery ? FLASH_STAGE_BOOT_MAGIC : FLASH_STAGE_RECOVERY_MAGIC;
	int mine = 1, theirs = 1;

	for (int i = 0; i < 8; i++) {
		mine &= h->boot_magic[i] == tie[i];
		theirs &= h->boot_magic[i] == other[i];
	}
	if (!mine) {
		return theirs ? -2 : -1;
	}
	return h->boot_bytes == boot_bytes && h->boot_fnv == boot_fnv;
}

static inline int
flash_stage_head_for(const struct flash_stage_head *h, uint64_t boot_bytes, uint64_t boot_fnv)
{
	return flash_stage_head_for_target(h, boot_bytes, boot_fnv, 0);
}

#endif /* FLASH_STAGE_H */
