/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * The UFS block service's page (ufs=2): the loader publishes it at ufs_shared_pa= and serves the kernel's requests
 * from its own core with Qualcomm's UFSDxe (xnuloader/loader.c, "UFS block service", struct loader_block). The layout
 * is the loader's, field for field; tools/vayu/tests/k6/ufs-abi-test.sh compiles this header and the loader's
 * definition side by side and compares every sizeof and offsetof.
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/hakios.ufs.c (its copy of the struct), against xnuloader12377 d86847d3
 * xnuloader/loader.c:1878-1988. Pure C: the kernel (bsd/dev/vayu_ufs.c) and a host test include it.
 */
#ifndef _SYS_VAYU_UFS_MAILBOX_H_
#define _SYS_VAYU_UFS_MAILBOX_H_

#include <stdint.h>

#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u
#define BLOCK_OP_WRITE_FAST 2u
#define BLOCK_OP_FLUSH     3u
#define BLOCK_CAN_FAST     1u

struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	uint32_t write_part_plus_one;          /* ufs_write=1: darwin (index + 1), 0: none */
	uint32_t write_caps;
	uint64_t write_first;
	uint32_t bcb_part_plus_one;            /* bcb_reboot=1 (2.6): misc (index + 1), 0: none */
	uint32_t bcb_reserved;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/* ufs_native= (the kernel's own transfer list): not used by this kernel (ufs=3/native is not ported) */
	struct {
		uint64_t hc_pa;
		uint64_t area_pa;
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the loader's page must stay the size it was: what is before ask has to fit in its padding");
_Static_assert(sizeof(struct loader_block_part) == 64, "struct loader_block_part is the loader's");

#endif /* _SYS_VAYU_UFS_MAILBOX_H_ */
