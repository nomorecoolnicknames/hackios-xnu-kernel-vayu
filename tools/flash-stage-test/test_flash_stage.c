/*
 * Host tests of userland/src/flash_stage.h: the layout flash_stage takes from the kernel's command line, the image
 * it is willing to stage, and the header block -- as vayu-dev's loader 2.8 reads it (standalone_staged_flash):
 * "XNUFLASH", u64 first block, u64 bytes, u64 FNV-1a 64, little endian, the rest of the block zero.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash_stage.h"

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

/* darwin's layout (devices/poco-x3-pro/tools/darwin-layout.py) as the loader writes it, in front of an image's own arguments */
#define LOADER_ARGS "ufs_darwin_first=0x3548000 ufs_darwin_blocks=0x6000fb ufs_write_first=0x200000 " \
	"ufs_var_first=0x300000 ufs_var_blocks=0x200000 ufs_stage_first=0x500000 ufs_stage_blocks=0x8000 " \
	"ufs_stage_head=0x508000"

static const char *
layout_of(const char *args, struct flash_stage_layout *l)
{
	memset(l, 0xa5, sizeof(*l));
	return flash_stage_layout(args, l);
}

static void
test_args(void)
{
	uint64_t v = 7;

	check(flash_stage_arg("a=1 b=0x10 c=010", "b", &v) == 1 && v == 16, "hex");
	check(flash_stage_arg("a=1 b=0x10 c=010", "c", &v) == -1, "a leading zero (the kernel's octal) is no number");
	check(flash_stage_arg("a=0 b=0x0", "a", &v) == 1 && v == 0 && flash_stage_arg("b=0x0", "b", &v) == 1, "zero");
	check(flash_stage_arg("a=-1", "a", &v) == -1 && flash_stage_arg("a=0b1", "a", &v) == -1, "a sign, 0b: no number");
	check(flash_stage_arg("a=1 b=2", "x", &v) == 0, "not there");
	check(flash_stage_arg("ab=1 a=2", "a", &v) == 1 && v == 2, "a longer name that begins the same is another");
	check(flash_stage_arg("a=3 a=3", "a", &v) == 1 && v == 3, "twice, the same");
	check(flash_stage_arg("a=3 a=0x3", "a", &v) == 1 && v == 3, "twice, the same number written two ways");
	check(flash_stage_arg("a=3 a=4", "a", &v) == -1, "twice, two values");
	check(flash_stage_arg("a=x", "a", &v) == -1 && flash_stage_arg("a=", "a", &v) == -1 &&
	    flash_stage_arg("a=0x", "a", &v) == -1, "not a number");
	check(flash_stage_arg("a=18446744073709551616", "a", &v) == -1, "past 64 bits");
	check(flash_stage_arg("a=0xffffffffffffffff", "a", &v) == 1 && v == UINT64_MAX, "64 bits");
	check(flash_stage_arg("  a=5\tb=6 ", "b", &v) == 1 && v == 6, "spaces and tabs");
	check(flash_stage_arg("a", "a", &v) == 0, "a name without a value is not a number's");
	check(flash_stage_arg("", "a", &v) == 0 && flash_stage_arg(NULL, "a", &v) == 0, "nothing at all");
}

static void
test_layout(void)
{
	struct flash_stage_layout l;

	check(layout_of(LOADER_ARGS " rd=md0 flash_stage=1", &l) == NULL, "the loader's layout is taken");
	check(l.first == 5242880 && l.blocks == 32768 && l.head == 5275648 && l.write_first == 2097152 &&
	    l.var_first == 3145728 && l.var_blocks == 2097152, "and read right");
	check(layout_of(LOADER_ARGS " ufs_stage_first=5242880", &l) == NULL, "the image's own, the same, also");
	check(layout_of(LOADER_ARGS " ufs_stage_first=2621440", &l) != NULL, "the image's own, another: refused");
	check(layout_of(LOADER_ARGS " ufs_var_first=0x280000", &l) != NULL, "another /dev/ufs_var: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x500000 ufs_stage_blocks=0x8000", &l) != NULL,
	    "no header block: refused");
	check(layout_of("rd=md0 var_hfs=1", &l) != NULL, "no layout at all (a phone before the installer): refused");
	check(layout_of("ufs_stage_first=0x500000 ufs_stage_blocks=0x8000 ufs_stage_head=0x508000", &l) != NULL,
	    "no ufs_write_first (darwin takes no writes): refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x100000 ufs_stage_blocks=0x8000 "
	    "ufs_stage_head=0x508000", &l) != NULL, "a staging area over the iOS volume: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x500000 ufs_stage_blocks=0x8000 "
	    "ufs_stage_head=0x1fffff", &l) != NULL, "a header below write_first: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x500000 ufs_stage_blocks=0x8000 "
	    "ufs_stage_head=0x507fff", &l) != NULL, "the header on the area's last block: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x500000 ufs_stage_blocks=0x8000 "
	    "ufs_stage_head=0x500000", &l) != NULL, "the header on its first: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x500000 ufs_stage_blocks=0 ufs_stage_head=0x508000",
	    &l) != NULL, "an empty area: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x500000 ufs_stage_blocks=0xffffffffffffffff "
	    "ufs_stage_head=0x400000", &l) != NULL, "a huge area: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_var_first=0x300000 ufs_var_blocks=0x200000 "
	    "ufs_stage_first=0x4ff000 ufs_stage_blocks=0x8000 ufs_stage_head=0x508000", &l) != NULL,
	    "a staging area over the end of /dev/ufs_var: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_var_first=0x300000 ufs_var_blocks=0x200000 "
	    "ufs_stage_first=0x500000 ufs_stage_blocks=0x8000 ufs_stage_head=0x3fffff", &l) != NULL,
	    "a header inside /dev/ufs_var: refused");
	check(layout_of("ufs_write_first=0x200000 ufs_stage_first=0x280000 ufs_stage_blocks=0x8000 "
	    "ufs_stage_head=0x2fffff", &l) == NULL, "without /dev/ufs_var anything past write_first (vayu-dev's)");
}

static void
test_image_and_head(void)
{
	struct flash_stage_layout l;
	struct flash_stage_head h;
	static uint8_t img[1 << 20], block[FLASH_STAGE_BLOCK];
	uint64_t want = FLASH_STAGE_FNV_START;

	layout_of(LOADER_ARGS, &l);
	memcpy(img, "ANDROID!", 8);
	for (size_t i = 8; i < sizeof(img); i++) {
		img[i] = (uint8_t)(i * 31 + (i >> 8));
	}
	check(flash_stage_image(img, sizeof(img), &l, 134217728) == NULL, "an image of 1 MB is taken");
	check(flash_stage_image(img, 4095, &l, 0) != NULL, "a short one is not");
	check(flash_stage_image(img, 4096, &l, 0) == NULL, "one page is");
	check(flash_stage_image(img, 32768ULL * 4096 + 1, &l, 0) != NULL, "one byte past the staging area is not");
	check(flash_stage_image(img, sizeof(img), &l, sizeof(img) - 1) != NULL, "one byte past boot is not");
	img[0] = 'X';
	check(flash_stage_image(img, sizeof(img), &l, 0) != NULL, "no ANDROID! at its start: not");
	img[0] = 'A';

	/* the header: byte for byte what the loader reads */
	flash_stage_head_block(block, &l, img, 12345);
	for (size_t i = 0; i < 12345; i++) {
		want = (want ^ img[i]) * 0x100000001b3ULL;
	}
	check(memcmp(block, "XNUFLASH", 8) == 0, "magic");
	check(block[8] == 0x00 && block[9] == 0x00 && block[10] == 0x50 && block[15] == 0, "first block, little endian");
	check(block[16] == (12345 & 0xff) && block[17] == (12345 >> 8) && block[18] == 0, "bytes, little endian");
	{
		uint64_t got = 0;

		for (int i = 7; i >= 0; i--) {
			got = got << 8 | block[24 + i];
		}
		check(got == want, "FNV-1a 64 of exactly the image's bytes");
	}
	{
		int zero = 1;

		for (size_t i = 32; i < sizeof(block); i++) {
			zero = zero && block[i] == 0;
		}
		check(zero, "the rest of the block zero");
	}
	check(flash_stage_head_read(block, &l, &h) && h.bytes == 12345 && h.fnv == want, "read back as a stage");
	block[0] = 'Y';
	check(!flash_stage_head_read(block, &l, &h), "a block without the magic is no stage");
	memset(block, 0, sizeof(block));
	check(!flash_stage_head_read(block, &l, &h), "a zero block (cancelled) is no stage");
	flash_stage_head_block(block, &l, img, 4096);
	l.first++;
	check(!flash_stage_head_read(block, &l, &h), "a stage of another area is none of this one");
	check(sizeof(struct flash_stage_head) == 32, "the header is 32 bytes");
}

int
main(void)
{
	test_args();
	test_layout();
	test_image_and_head();
	printf("flash-stage-test: %d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
