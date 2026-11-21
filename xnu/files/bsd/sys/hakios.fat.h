/*
 * A read-only reader of a FAT16/FAT32 volume, for firmware that the phone keeps in FAT partitions of its
 * own: Qualcomm's "modem" (NON-HLOS: image/modem.mdt, image/modem.bNN, image/wlanmdsp.mbn,
 * image/modem_pr/...) and "bluetooth" (BTFM: image/crbtfw21.tlv, image/crnv21.bin). bsd/dev/hakios.image.c
 * reads them through the storage's read-only path (bsd/dev/hakios.ufs.c); tools/fat-test of the port reads
 * a copy of the partition with the same code on the build machine.
 *
 * Nothing here writes, allocates or sleeps: the caller gives the reading function, a scratch buffer of at
 * least one cluster, and room for the whole first FAT (hakios_fat_open says how much).
 */
#ifndef _SYS_HAKIOS_FAT_H_
#define _SYS_HAKIOS_FAT_H_

#ifdef KERNEL
#include <sys/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define HAKIOS_FAT_MAX_FAT      (8u << 20)      /* the most of a FAT kept in memory (vayu's modem: 45 KB) */
#define HAKIOS_FAT_MAX_CLUSTER  (64u << 10)
#define HAKIOS_FAT_PATH_MAX     192

struct hakios_fat {
	/* given by the caller */
	int (*read)(void *ctx, uint64_t offset, void *buf, uint64_t len);       /* 0 or an errno */
	void *ctx;
	uint64_t size;                  /* bytes of the partition */

	/* hakios_fat_open */
	uint32_t type;                  /* 16 or 32 */
	uint32_t sector;                /* bytes per sector */
	uint32_t cluster;               /* bytes per cluster */
	uint32_t clusters;              /* data clusters: numbered 2 .. clusters + 1 */
	uint64_t fat_at, data_at;       /* byte offsets of the first FAT and of cluster 2 */
	uint64_t root_at;               /* FAT16: the fixed root directory */
	uint32_t root_bytes;            /* FAT16: its size */
	uint32_t root_cluster;          /* FAT32: the root directory's first cluster */
	uint32_t fat_bytes;             /* what hakios_fat_load wants room for */

	/* hakios_fat_load */
	const uint8_t *fat;
};

struct hakios_fat_file {
	uint32_t first;                 /* first cluster (0: empty) */
	uint32_t size;                  /* bytes (a directory: 0) */
	int directory;
};

int hakios_fat_open(struct hakios_fat *fs);
int hakios_fat_load(struct hakios_fat *fs, uint8_t *fat, uint32_t fat_bytes);   /* fat_bytes >= fs->fat_bytes */
/* "image/modem_pr/mcfg/x.mbn": components compared without regard to ASCII case; "." and ".." are refused. */
int hakios_fat_lookup(struct hakios_fat *fs, const char *path, uint8_t *scratch, uint32_t scratch_size,
    struct hakios_fat_file *file);
int hakios_fat_read(struct hakios_fat *fs, const struct hakios_fat_file *file, uint8_t *dst, uint64_t len);   /* the whole file: len == file->size */
int hakios_fat_path_ok(const char *path);       /* 1: relative, no empty, "." or ".." component, no control characters */

#endif /* _SYS_HAKIOS_FAT_H_ */
