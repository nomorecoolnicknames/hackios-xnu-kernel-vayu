/*
 * The kernel's FAT reader (bsd/dev/hakios.fat.c) on this machine, against an image file:
 *   test_fat paths                     the path checks alone
 *   test_fat IMAGE PATH OUT            PATH of IMAGE (a FAT16/32 volume) into the file OUT
 *   test_fat IMAGE PATH -              the same, expected to be refused (exit 0 if it is)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/hakios.fat.h>

static FILE *image;
static unsigned long reads;

static int
image_read(void *ctx, uint64_t offset, void *buf, uint64_t len)
{
	(void)ctx;
	reads++;
	if (fseeko(image, (off_t)offset, SEEK_SET) != 0 || fread(buf, 1, len, image) != len) {
		return EIO;
	}
	return 0;
}

static int
paths(void)
{
	static const char *good[] = { "image/modem.mdt", "image/modem_pr/mcfg/configs/mcfg_sw/generic/x.mbn", "a", "a.b/c" };
	static const char *bad[] = { "", "/image/modem.mdt", "image//x", "image/", "../x", "image/../x", "image/./x", ".", "..",
		"a b", "a\\b", "a\x01", "a\x7f", "\xc3\xa9" };
	int failed = 0;

	for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
		if (!hakios_fat_path_ok(good[i])) {
			printf("FAIL: \"%s\" refused\n", good[i]);
			failed = 1;
		}
	}
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		if (hakios_fat_path_ok(bad[i])) {
			printf("FAIL: \"%s\" taken\n", bad[i]);
			failed = 1;
		}
	}
	{
		char longest[HAKIOS_FAT_PATH_MAX + 2];

		memset(longest, 'a', sizeof(longest) - 1);
		longest[sizeof(longest) - 1] = 0;
		if (hakios_fat_path_ok(longest)) {
			printf("FAIL: a path of %zu characters taken\n", strlen(longest));
			failed = 1;
		}
	}
	printf("paths: %s\n", failed ? "FAIL" : "ok");
	return failed;
}

int
main(int argc, char **argv)
{
	struct hakios_fat fs = { .read = image_read };
	struct hakios_fat_file file;
	uint8_t *fat, *scratch, *data;
	int error;

	if (argc == 2 && strcmp(argv[1], "paths") == 0) {
		return paths();
	}
	if (argc != 4) {
		fprintf(stderr, "usage: test_fat paths | test_fat IMAGE PATH OUT|-\n");
		return 2;
	}
	image = fopen(argv[1], "rb");
	if (image == NULL || fseeko(image, 0, SEEK_END) != 0) {
		perror(argv[1]);
		return 2;
	}
	fs.size = (uint64_t)ftello(image);
	error = hakios_fat_open(&fs);
	if (error != 0) {
		printf("%s: not opened: %d\n", argv[1], error);
		return 1;
	}
	fat = malloc(fs.fat_bytes);
	scratch = malloc(fs.cluster);
	if (fat == NULL || scratch == NULL || hakios_fat_load(&fs, fat, fs.fat_bytes) != 0) {
		printf("%s: FAT not loaded\n", argv[1]);
		return 1;
	}
	error = hakios_fat_lookup(&fs, argv[2], scratch, fs.cluster, &file);
	if (strcmp(argv[3], "-") == 0) {
		free(fat);
		free(scratch);
		fclose(image);
		if (error == 0 && !file.directory) {
			printf("FAIL: %s found (%u bytes)\n", argv[2], file.size);
			return 1;
		}
		return 0;
	}
	if (error != 0 || file.directory) {
		printf("FAIL: %s: %s %d\n", argv[2], error ? "not found" : "a directory", error);
		free(fat);
		free(scratch);
		fclose(image);
		return 1;
	}
	data = malloc(file.size ? file.size : 1);
	error = hakios_fat_read(&fs, &file, data, file.size);
	if (error != 0) {
		printf("FAIL: %s: read %d\n", argv[2], error);
		return 1;
	}
	FILE *out = fopen(argv[3], "wb");
	if (out == NULL || fwrite(data, 1, file.size, out) != file.size || fclose(out) != 0) {
		perror(argv[3]);
		return 2;
	}
	free(data);
	free(fat);
	free(scratch);
	fclose(image);
	(void)reads;
	return 0;
}
