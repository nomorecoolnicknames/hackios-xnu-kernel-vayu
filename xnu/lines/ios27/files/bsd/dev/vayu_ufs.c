/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * The phone's storage, read only: one block device and one character device for every GPT partition of its UFS
 * (/dev/ufs_NAME, /dev/rufs_NAME), served by the loader (ufs=2). UEFI never exits on this port: Qualcomm's UFSDxe has
 * the controller, its PHY, clocks and power up and stays resident, and the loader serves the kernel from its own core
 * while the kernel runs on the others. It publishes a page (ufs_shared_pa=, sys/vayu_ufs_mailbox.h) with the table of
 * partitions, a request, an answer and a buffer; this driver writes a request -- which partition, which blocks -- and
 * the loader reads them with UFSDxe's ReadBlocks into the buffer. Nothing in this kernel touches the controller.
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/hakios.ufs.c (the 8796 line; its request, its reads, its BCB). Ported to
 * xnu-12377 (K6) as the read-only core of it, in a file of its own (Scion's bsd/dev/hakios.ufs.c is FRANKEL's):
 *  - two steps, as the root needs: before the root (VAYU_PHASE_BEFORE_ROOT) the page is checked, its table copied out
 *    and the block and character majors registered -- enough for rd=ufsroot0 (sweet_ufs_rootdev() below) -- and after
 *    devfs the /dev nodes; each step once;
 *  - nothing writes: no block device and no character device takes a write (EROFS), whatever ufs_write= says. The 8796
 *    line's writes of darwin (ufs_write=1, /dev/ufs_var, var_store, var_fsck), its count of starts (ufs_tries_block),
 *    its native UFS driver (ufs_native, ufs=3: hakios.ufshc.c) and its benches are not ported;
 *  - the one exception, off unless bcb_reboot=1 and the loader offers misc by its identity (loader 2.6): misc's first
 *    block, its command alone -- "boot-recovery" or nothing -- for the USB command port (vayu_ufs_bcb_set);
 *  - the page is read through aligned volatile loads only (a mapping that may be Device memory, K3's vayu_wcomb_safe=1):
 *    the table is copied out once and checked (names, block sizes, sizes) before anything uses it.
 *
 * Boot-args (vayu27=1 for any of it):
 *   ufs=2               the driver (the loader puts ufs_shared_pa= on the command line when asked for ufs=2)
 *   ufsroot=NAME        rd=ufsroot0's partition: exactly one partition of that name, not a whole unit (else the root
 *                       is refused: no default, never Android's userdata)
 *   ufs_selftest=1      after devfs: each unit's GPT read through the loader, its header's and its entries' CRC-32
 *                       checked, the SHA-256 of the unit's first 64 KiB; and the refusal of a write, both devices
 *   bcb_reboot=1        misc's command may be written (above)
 *
 * One request at a time (the page has one): requests are serialized by a mutex and split into pieces of the buffer's
 * size. The kernel writes only the request's cache lines and the loader only the answer's. The data buffer, which the
 * loader's reads fill, is mapped cacheable and the part an answer covers is invalidated before it is copied out.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/crc.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_storage.h>
#include <sys/vayu_ufs_mailbox.h>
#include <vm/memory_types.h>

extern void IOSleep(unsigned int milliseconds);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);   /* arm/machine_routines.h */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define UFS_PAGE           0x4000UL     /* the kernel's page (16K): what a mapping is rounded to */
#define UFS_TIMEOUT_MS     20000        /* the loader answers in well under a second; this is for a dead loader */
#define UFS_BUFFER_MAX     (16u << 20)

static volatile struct loader_block *ufs_page;
static struct loader_block_part ufs_table[BLOCK_MAX_PARTS];     /* the page's table, copied out and checked */
static uint8_t *ufs_buffer;
static uint32_t ufs_buffer_size, ufs_parts, ufs_seq;
static uint32_t ufs_bsize[BLOCK_MAX_PARTS];     /* what each device is counted in (spec_open sets 512) */
static int ufs_bmajor = -1, ufs_cmajor = -1;
static bool ufs_ready, ufs_nodes, ufs_dead;
static uint64_t ufs_requests, ufs_errors, ufs_bytes;
static int ufs_bcb_part = -1;

static LCK_GRP_DECLARE(ufs_lck_grp, "vayu-ufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);         /* the page: one request at a time */
static LCK_MTX_DECLARE(ufs_bcb_lock, &ufs_lck_grp);     /* ufs_bcb_block */

/* ---- the page ---------------------------------------------------------------------------------- */

static uint64_t
ufs_part_bytes(uint32_t part)
{
	return ufs_table[part].blocks * ufs_table[part].block_size;
}

/*
 * Native blocks [lba, lba + count) of partition part, one request of at most the buffer's size: read into dst, or
 * (BLOCK_OP_WRITE, misc's command only) written from src. Called with ufs_lock held.
 */
static int
ufs_request(uint32_t op, uint32_t part, uint64_t lba, uint64_t count, uint8_t *dst, const uint8_t *src)
{
	uint64_t bytes = count * ufs_table[part].block_size, started, waited_ms = 0;
	uint32_t seq = ++ufs_seq;
	int64_t status;

	if (ufs_dead) {
		return EIO;
	}
	if (op == BLOCK_OP_WRITE) {
		/* the data into the buffer and out of this core's cache before the request is raised */
		memcpy(ufs_buffer, src, (size_t)bytes);
		FlushPoC_DcacheRegion((vm_offset_t)ufs_buffer, (size_t)bytes);
	}
	ufs_page->ask.part = part;
	ufs_page->ask.lba = lba;
	ufs_page->ask.count = count;
	ufs_page->ask.op = op;
	__asm__ volatile ("dmb sy" ::: "memory");
	ufs_page->ask.request = seq;
	__asm__ volatile ("dmb sy" ::: "memory");

	started = mach_absolute_time();
	while (ufs_page->answer.answered != seq) {
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		if (ns < 2000 * NSEC_PER_USEC) {
			__builtin_arm_yield();          /* a small request is answered in tens of microseconds */
			continue;
		}
		IOSleep(1);
		if (++waited_ms > UFS_TIMEOUT_MS) {
			printf("ufs: no answer from the loader to request %u in %u s: the storage is off from now on\n",
			    seq, UFS_TIMEOUT_MS / 1000);
			ufs_dead = true;
			ufs_errors++;
			return EIO;
		}
	}
	__asm__ volatile ("dmb sy" ::: "memory");
	status = ufs_page->answer.status;
	if (status != 0) {
		printf("ufs: %s: blocks %llu..%llu: the loader's %s failed (status %llx)\n", ufs_table[part].name, lba,
		    lba + count - 1, op == BLOCK_OP_READ ? "read" : "write", (uint64_t)status);
		ufs_errors++;
		return EIO;
	}
	if (op != BLOCK_OP_READ) {
		return 0;               /* the loader has flushed, read the block back and found it the same */
	}
	/* whatever this core may hold of the buffer from before (a prefetch) goes before the copy */
	FlushPoC_DcacheRegion((vm_offset_t)ufs_buffer, (size_t)bytes);
	memcpy(dst, ufs_buffer, (size_t)bytes);
	ufs_requests++;
	ufs_bytes += bytes;
	return 0;
}

/* Bytes [offset, offset + len) of partition part into dst, whatever their alignment: the native blocks around them. */
static int
ufs_read(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	uint64_t nbs = ufs_table[part].block_size, per = ufs_buffer_size / nbs;
	static uint8_t *bounce;
	int error = 0;

	lck_mtx_lock(&ufs_lock);
	if (bounce == NULL) {
		bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
		if (bounce == NULL) {
			lck_mtx_unlock(&ufs_lock);
			return ENOMEM;
		}
	}
	while (len > 0 && error == 0) {
		uint64_t first = offset / nbs, skip = offset % nbs;
		uint64_t count = (skip + len + nbs - 1) / nbs, take;

		if (count > per) {
			count = per;
		}
		take = count * nbs - skip;
		if (take > len) {
			take = len;
		}
		if (skip == 0 && take == count * nbs) {
			error = ufs_request(BLOCK_OP_READ, part, first, count, dst, NULL);
		} else {
			error = ufs_request(BLOCK_OP_READ, part, first, count, bounce, NULL);
			if (error == 0) {
				memcpy(dst, bounce + skip, (size_t)take);
			}
		}
		offset += take;
		dst += take;
		len -= take;
	}
	lck_mtx_unlock(&ufs_lock);
	return error;
}

static uint32_t
ufs_entry(const char *name)
{
	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp(ufs_table[i].name, name) == 0) {
			return i;
		}
	}
	return BLOCK_MAX_PARTS;
}

/* ---- the devices ------------------------------------------------------------------------------- */

static bool
ufs_dev_part(dev_t dev, uint32_t *part)
{
	uint32_t m = (uint32_t)minor(dev);

	if (!ufs_ready || m >= ufs_parts || (ufs_table[m].flags & BLOCK_PART_UNIT)) {
		return false;
	}
	*part = m;
	return true;
}

static int
ufs_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	uint32_t part;

	if (!ufs_dev_part(dev, &part)) {
		return ENXIO;
	}
	if (flags & FWRITE) {
		return EROFS;           /* nothing of this driver writes */
	}
	return 0;
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
ufs_strategy(buf_t bp)
{
	uint32_t m = (uint32_t)minor(buf_device(bp)), part;
	uint64_t offset, len, size;
	caddr_t va = 0;
	int error;

	if (!ufs_dev_part(buf_device(bp), &part)) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if ((buf_flags(bp) & B_READ) == 0) {
		buf_seterror(bp, EROFS);
		buf_biodone(bp);
		return;
	}
	size = ufs_part_bytes(part);
	offset = (uint64_t)buf_blkno(bp) * ufs_bsize[m];
	len = buf_count(bp);
	if (offset >= size) {
		buf_seterror(bp, offset == size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (len > size - offset) {
		len = size - offset;
	}
	/* buf_map(): a cluster-layer buffer carries a UPL, not a kernel address, until it is mapped */
	if (buf_map(bp, &va) != 0 || va == 0) {
		buf_seterror(bp, EIO);
		buf_biodone(bp);
		return;
	}
	error = ufs_read(part, offset, (uint8_t *)va, len);
	buf_unmap(bp);
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	uint32_t m = (uint32_t)minor(dev), part, native;

	if (!ufs_dev_part(dev, &part)) {
		return ENXIO;
	}
	native = ufs_table[part].block_size;
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = ufs_bsize[m];
		return 0;
	case DKIOCSETBLOCKSIZE:
		/* spec_open() sets 512 and back, and takes a refusal for a broken device; reads are by byte here */
		if (*(uint32_t *)data < 512 || *(uint32_t *)data > native || (*(uint32_t *)data & (*(uint32_t *)data - 1)) != 0) {
			return EINVAL;
		}
		ufs_bsize[m] = *(uint32_t *)data;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = native;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_part_bytes(part) / ufs_bsize[m];
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = ufs_buffer_size / ufs_bsize[m];
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_psize(dev_t dev)
{
	uint32_t part;

	return ufs_dev_part(dev, &part) ? (int)ufs_bsize[minor(dev)] : -1;
}

/* /dev/rufs_NAME: a bufferful at a time into the kernel's memory and out with uiomove (not physio: PAN) */
static int
ufs_cread(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t part;
	uint64_t size;
	uint8_t *bounce;
	int error = 0;

	if (!ufs_dev_part(dev, &part)) {
		return ENXIO;
	}
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	size = ufs_part_bytes(part);
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t at = (uint64_t)uio_offset(uio), want = (uint64_t)uio_resid(uio);

		if (want > ufs_buffer_size) {
			want = ufs_buffer_size;
		}
		if (want > size - at) {
			want = size - at;
		}
		error = ufs_read(part, at, bounce, want);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)want, uio);
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

static int
ufs_cwrite(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return EROFS;
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_open,
	.d_close      = ufs_close,
	.d_read       = ufs_cread,
	.d_write      = ufs_cwrite,
	.d_ioctl      = ufs_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/* ---- for the kernel's own users (as the 8796 line's hakios_ufs_*: the modem's rmtfs, firmware) ------------- */

bool
hakios_ufs_part(const char *name, uint32_t *part, uint64_t *bytes)
{
	uint32_t index;

	if (!ufs_ready || name == NULL) {
		return false;
	}
	index = ufs_entry(name);
	if (index >= ufs_parts || (ufs_table[index].flags & BLOCK_PART_UNIT)) {
		return false;
	}
	*part = index;
	*bytes = ufs_part_bytes(index);
	return true;
}

int
hakios_ufs_part_read(uint32_t part, uint64_t offset, void *dst, uint64_t len)
{
	uint64_t bytes;

	if (!ufs_ready || part >= ufs_parts || (ufs_table[part].flags & BLOCK_PART_UNIT)) {
		return ENXIO;
	}
	bytes = ufs_part_bytes(part);
	if (offset > bytes || len > bytes - offset) {
		return EINVAL;
	}
	return len == 0 ? 0 : ufs_read(part, offset, dst, len);
}

bool
vayu_ufs_dev(dev_t dev)
{
	return ufs_ready && ((ufs_bmajor >= 0 && major(dev) == ufs_bmajor) || (ufs_cmajor >= 0 && major(dev) == ufs_cmajor));
}

/* ---- the root: rd=ufsroot0 (iokit/bsddev/IOKitBSDInit.cpp asks; sys/vayu_12377_abi.h) ------------------------ */

int
sweet_ufs_rootdev(dev_t *dev)
{
	char name[sizeof(ufs_table[0].name)] = "";
	uint32_t index = BLOCK_MAX_PARTS, found = 0;

	if (!vayu27_enabled()) {
		printf("VAYU: rd=ufsroot0: the UFS root is a vayu27=1 path: refused\n");
		return ENODEV;
	}
	if (!ufs_ready) {
		printf("VAYU: rd=ufsroot0: no UFS mailbox in this run (ufs=2, and the loader's ufs_shared_pa=): refused\n");
		return ENODEV;
	}
	if (!PE_parse_boot_arg_str("ufsroot", name, sizeof(name)) || name[0] == 0) {
		printf("VAYU: rd=ufsroot0 without ufsroot=NAME: no partition is taken by default: refused\n");
		return EINVAL;
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp(ufs_table[i].name, name) == 0 && !(ufs_table[i].flags & BLOCK_PART_UNIT)) {
			index = i;
			found++;
		}
	}
	if (found != 1) {
		printf("VAYU: rd=ufsroot0: %u partitions named \"%s\" (exactly one wanted): refused\n", found, name);
		return found == 0 ? ENOENT : EEXIST;
	}
	if (ufs_table[index].block_size != 4096 || ufs_part_bytes(index) < (1u << 20)) {
		printf("VAYU: rd=ufsroot0: %s has blocks of %u, %llu bytes: not a root's partition: refused\n", name,
		    ufs_table[index].block_size, ufs_part_bytes(index));
		return EINVAL;
	}
	*dev = makedev(ufs_bmajor, index);
	printf("VAYU: rd=ufsroot0 is %s, %llu MB, read only through the loader\n", name, ufs_part_bytes(index) >> 20);
	return 0;
}

/* ---- misc's boot control block (bcb_reboot=1, loader 2.6) ----------------------------------------------------- */

#define BCB_BYTES          4096
#define BCB_COMMAND        32
#define BCB_STATUS_AT      32
#define BCB_RECOVERY_AT    64
#define BCB_RECOVERY_BYTES 768

static uint8_t ufs_bcb_block[BCB_BYTES] __attribute__((aligned(64)));  /* under ufs_bcb_lock */
static const char ufs_bcb_recovery[] = "boot-recovery";

static int
ufs_bcb_misc(void)
{
	uint32_t misc = ufs_entry("misc");

	return ufs_ready && misc < ufs_parts && ufs_table[misc].block_size == BCB_BYTES ? (int)misc : -1;
}

int
vayu_ufs_bcb_get(char *out, size_t size)
{
	int misc = ufs_bcb_misc(), error;

	if (size == 0) {
		return EINVAL;
	}
	out[0] = 0;
	if (misc < 0) {
		return ENODEV;
	}
	lck_mtx_lock(&ufs_bcb_lock);
	error = ufs_read((uint32_t)misc, 0, ufs_bcb_block, BCB_BYTES);
	if (error == 0) {
		size_t n = 0;

		for (; n < BCB_COMMAND && n + 1 < size && ufs_bcb_block[n] != 0; n++) {
			out[n] = ufs_bcb_block[n] >= 0x20 && ufs_bcb_block[n] < 0x7f ? (char)ufs_bcb_block[n] : '.';
		}
		out[n] = 0;
	}
	lck_mtx_unlock(&ufs_bcb_lock);
	return error;
}

static void
ufs_bcb_text(const uint8_t *p, size_t n, char *out, size_t size)
{
	size_t i;

	for (i = 0; i < n && i + 1 < size && p[i] != 0; i++) {
		out[i] = p[i] == '\n' ? '|' : p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '.';
	}
	out[i] = 0;
}

int
vayu_ufs_bcb_fields(char *status, size_t ssize, char *recovery, size_t rsize, uint32_t *dirty)
{
	int misc = ufs_bcb_misc(), error;

	if (ssize == 0 || rsize == 0) {
		return EINVAL;
	}
	status[0] = recovery[0] = 0;
	*dirty = 0;
	if (misc < 0) {
		return ENODEV;
	}
	lck_mtx_lock(&ufs_bcb_lock);
	error = ufs_read((uint32_t)misc, 0, ufs_bcb_block, BCB_BYTES);
	if (error == 0) {
		ufs_bcb_text(ufs_bcb_block + BCB_STATUS_AT, BCB_RECOVERY_AT - BCB_STATUS_AT, status, ssize);
		ufs_bcb_text(ufs_bcb_block + BCB_RECOVERY_AT, BCB_RECOVERY_BYTES, recovery, rsize);
		for (unsigned i = BCB_COMMAND; i < BCB_BYTES; i++) {
			*dirty += ufs_bcb_block[i] != 0;
		}
	}
	lck_mtx_unlock(&ufs_bcb_lock);
	return error;
}

/* misc's command set to "boot-recovery" (recovery) or cleared, the rest of its block as it is; read back here too */
int
vayu_ufs_bcb_set(bool recovery)
{
	int error;

	if (ufs_bcb_part < 0) {
		printf("bcb: misc takes no write this run (bcb_reboot=1, and a loader 2.6 that offers it)\n");
		return EPERM;
	}
	lck_mtx_lock(&ufs_bcb_lock);
	error = ufs_read((uint32_t)ufs_bcb_part, 0, ufs_bcb_block, BCB_BYTES);
	if (error == 0) {
		for (unsigned i = 0; i < BCB_COMMAND; i++) {
			ufs_bcb_block[i] = recovery && i < sizeof(ufs_bcb_recovery) - 1 ? (uint8_t)ufs_bcb_recovery[i] : 0;
		}
		lck_mtx_lock(&ufs_lock);
		error = ufs_request(BLOCK_OP_WRITE, (uint32_t)ufs_bcb_part, 0, 1, NULL, ufs_bcb_block);
		lck_mtx_unlock(&ufs_lock);
	}
	if (error == 0) {
		uint8_t want[BCB_COMMAND];

		memcpy(want, ufs_bcb_block, sizeof(want));
		error = ufs_read((uint32_t)ufs_bcb_part, 0, ufs_bcb_block, BCB_BYTES);
		if (error == 0 && memcmp(want, ufs_bcb_block, sizeof(want)) != 0) {
			error = EIO;
		}
	}
	lck_mtx_unlock(&ufs_bcb_lock);
	printf("bcb: misc's command %s: %s (%d)\n", recovery ? "set to \"boot-recovery\"" : "cleared",
	    error == 0 ? "written, read back the same" : "NOT written", error);
	return error;
}

/* ---- ufs_selftest=1 ------------------------------------------------------------------------------------------ */

/* "EFI PART": the GPT header of a unit (its LBA 1), its own CRC-32 and its entries', as UEFI checks them */
static void
ufs_selftest_unit(uint32_t part, uint8_t *buffer, size_t size)
{
	uint32_t bs = ufs_table[part].block_size, hcrc, hsize, entries, esize, ecrc, want;
	uint64_t elba;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	char hex[2 * SHA256_DIGEST_LENGTH + 1];
	SHA256_CTX ctx;

	if (bs > size / 2 || ufs_read(part, 0, buffer, size) != 0) {
		printf("ufs: selftest: %s: not read\n", ufs_table[part].name);
		return;
	}
	SHA256_Init(&ctx);
	SHA256_Update(&ctx, buffer, size);
	SHA256_Final(digest, &ctx);
	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
	if (memcmp(buffer + bs, "EFI PART", 8) != 0) {
		printf("ufs: selftest: %s: no GPT header at LBA 1; first %zu bytes sha256 %s\n", ufs_table[part].name, size, hex);
		return;
	}
	memcpy(&hsize, buffer + bs + 12, 4);
	memcpy(&hcrc, buffer + bs + 16, 4);
	memcpy(&elba, buffer + bs + 72, 8);
	memcpy(&entries, buffer + bs + 80, 4);
	memcpy(&esize, buffer + bs + 84, 4);
	memcpy(&ecrc, buffer + bs + 88, 4);
	if (hsize < 92 || hsize > bs) {
		printf("ufs: selftest: %s: GPT header of %u bytes: FAIL\n", ufs_table[part].name, hsize);
		return;
	}
	bzero(buffer + bs + 16, 4);
	want = crc32(0, buffer + bs, hsize);
	memcpy(buffer + bs + 16, &hcrc, 4);
	printf("ufs: selftest: %s: GPT header crc32 %08x, computed %08x: %s\n", ufs_table[part].name, hcrc, want,
	    want == hcrc ? "ok" : "FAIL");
	if ((uint64_t)entries * esize <= size && elba * bs + (uint64_t)entries * esize <= ufs_part_bytes(part) &&
	    ufs_read(part, elba * bs, buffer, (uint64_t)entries * esize) == 0) {
		want = crc32(0, buffer, (size_t)entries * esize);
		printf("ufs: selftest: %s: %u entries of %u at LBA %llu, crc32 %08x, computed %08x: %s\n", ufs_table[part].name,
		    entries, esize, elba, ecrc, want, want == ecrc ? "ok" : "FAIL");
	} else {
		printf("ufs: selftest: %s: %u entries of %u at LBA %llu: not read\n", ufs_table[part].name, entries, esize, elba);
	}
	printf("ufs: selftest: %s: its first %zu bytes sha256 %s\n", ufs_table[part].name, size, hex);
}

static void
ufs_selftest(void)
{
	size_t size = 64 * 1024;
	uint8_t *buffer = kalloc_data(size, Z_WAITOK);
	uint32_t first = BLOCK_MAX_PARTS;
	int berr, cerr;

	if (buffer == NULL) {
		return;
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (ufs_table[i].flags & BLOCK_PART_UNIT) {
			ufs_selftest_unit(i, buffer, size);
		} else if (first == BLOCK_MAX_PARTS) {
			first = i;
		}
	}
	if (first != BLOCK_MAX_PARTS) {
		berr = ufs_open(makedev(ufs_bmajor, first), FREAD | FWRITE, S_IFBLK, NULL);
		cerr = ufs_open(makedev(ufs_cmajor, first), FREAD | FWRITE, S_IFCHR, NULL);
		printf("ufs: selftest: an open of %s for writing: block %d, character %d: %s\n", ufs_table[first].name, berr,
		    cerr, berr == EROFS && cerr == EROFS ? "refused, ok" : "FAIL");
	}
	printf("ufs: selftest: %llu requests, %llu bytes, %llu errors\n", ufs_requests, ufs_bytes, ufs_errors);
	kfree_data(buffer, size);
}

/* ---- the two steps ------------------------------------------------------------------------------------------- */

/* the page's table, copied out a byte at a time (the mapping may be Device memory), and checked */
static bool
ufs_table_copy(void)
{
	volatile const uint8_t *from = (volatile const uint8_t *)&ufs_page->part[0];
	uint8_t *to = (uint8_t *)ufs_table;

	for (size_t i = 0; i < sizeof(struct loader_block_part) * ufs_parts; i++) {
		to[i] = from[i];
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		struct loader_block_part *e = &ufs_table[i];
		uint64_t bytes;

		if (strnlen(e->name, sizeof(e->name)) >= sizeof(e->name) || e->name[0] == 0 || e->block_size < 512 ||
		    e->block_size > 65536 || (e->block_size & (e->block_size - 1)) != 0 || e->blocks == 0 ||
		    __builtin_mul_overflow(e->blocks, (uint64_t)e->block_size, &bytes) || e->block_size > ufs_buffer_size) {
			e->name[sizeof(e->name) - 1] = 0;
			printf("ufs: the loader's entry %u (\"%s\", blocks of %u, %llu of them) is not one: no storage\n", i,
			    e->name, e->block_size, e->blocks);
			return false;
		}
		ufs_bsize[i] = e->block_size;
	}
	return true;
}

/* before the root: the page, the table and the majors; no /dev yet */
static void
vayu_ufs_early(void)
{
	uint64_t pa = 0, buffer_pa;
	uint32_t mode = 0;
	vm_offset_t base, buffer;

	if (!PE_parse_boot_argn("ufs", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	if (mode != 2) {
		printf("ufs: ufs=%u: only the loader's service (ufs=2) is in this kernel: no storage\n", mode);
		return;
	}
	if (!PE_parse_boot_argn("ufs_shared_pa", &pa, sizeof(pa)) || pa == 0 || (pa & 7) != 0) {
		printf("ufs: ufs=2 without the loader's ufs_shared_pa=: no storage\n");
		return;
	}
	base = ml_io_map_unmappable((vm_offset_t)(pa & ~(UFS_PAGE - 1)),
	    ((pa & (UFS_PAGE - 1)) + sizeof(struct loader_block) + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_WCOMB);
	if (base == 0) {
		printf("ufs: the loader's page at %llx cannot be mapped\n", pa);
		return;
	}
	ufs_page = (volatile struct loader_block *)(base + (pa & (UFS_PAGE - 1)));
	if (ufs_page->magic != BLOCK_MAGIC || ufs_page->version != BLOCK_VERSION ||
	    ufs_page->size != sizeof(struct loader_block) || ufs_page->parts == 0 || ufs_page->parts > BLOCK_MAX_PARTS) {
		printf("ufs: the loader's page at %llx is not one this kernel knows (magic %llx, version %u, size %u of %zu, "
		    "%u partitions): no storage\n", pa, ufs_page->magic, ufs_page->version, ufs_page->size,
		    sizeof(struct loader_block), ufs_page->parts);
		ufs_page = NULL;
		return;
	}
	ufs_parts = ufs_page->parts;
	ufs_buffer_size = ufs_page->buffer_size;
	buffer_pa = ufs_page->buffer_pa;
	if (ufs_buffer_size < 4096 || ufs_buffer_size > UFS_BUFFER_MAX || (ufs_buffer_size & 4095) != 0 ||
	    buffer_pa == 0 || (buffer_pa & 63) != 0) {
		printf("ufs: the loader's buffer (%u bytes at %llx) is not one: no storage\n", ufs_buffer_size, buffer_pa);
		ufs_page = NULL;
		return;
	}
	if (!ufs_table_copy()) {
		ufs_page = NULL;
		return;
	}
	buffer = ml_io_map_unmappable((vm_offset_t)(buffer_pa & ~(UFS_PAGE - 1)),
	    ((buffer_pa & (UFS_PAGE - 1)) + ufs_buffer_size + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_DEFAULT);
	if (buffer == 0) {
		printf("ufs: the loader's buffer at %llx cannot be mapped: no storage\n", buffer_pa);
		ufs_page = NULL;
		return;
	}
	ufs_buffer = (uint8_t *)(buffer + (buffer_pa & (UFS_PAGE - 1)));
	ufs_seq = ufs_page->answer.answered;      /* a loader that served an earlier kernel counts on from there */
	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	ufs_cmajor = ufs_bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor);
	if (ufs_cmajor < 0) {
		printf("ufs: no device numbers to be had\n");
		return;
	}
	ufs_ready = true;
	printf("ufs: the loader's page at %llx: %u entries, a %u KB buffer; block major %d, read only\n", pa, ufs_parts,
	    ufs_buffer_size >> 10, ufs_bmajor);
}

VAYU_HOOK(VAYU_PHASE_BEFORE_ROOT, ufs, vayu_ufs_early);

/* after devfs (bsd/dev/vayu_storage.c): the nodes, misc's write if asked, the selftest */
void
vayu_ufs_init(void)
{
	uint32_t made = 0, on = 0;

	if (!ufs_ready || ufs_nodes) {
		return;
	}
	ufs_nodes = true;
	for (uint32_t i = 0; i < ufs_parts; i++) {
		char name[48], suffix[8] = "";

		if (ufs_table[i].flags & BLOCK_PART_UNIT) {
			continue;               /* a whole unit: not a device of its own */
		}
		/* a name may be on more than one unit (vayu: bk44 and others): the second gets _uN after it */
		snprintf(name, sizeof(name), "ufs_%s", ufs_table[i].name);
		if (devfs_make_node(makedev(ufs_bmajor, i), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "%s", name) == NULL) {
			snprintf(suffix, sizeof(suffix), "_u%u", (unsigned)ufs_table[i].unit);
			snprintf(name, sizeof(name), "ufs_%s%s", ufs_table[i].name, suffix);
			if (devfs_make_node(makedev(ufs_bmajor, i), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "%s", name) == NULL) {
				printf("ufs: no node for %s\n", name);
				continue;
			}
		}
		snprintf(name, sizeof(name), "rufs_%s%s", ufs_table[i].name, suffix);
		(void)devfs_make_node(makedev(ufs_cmajor, i), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "%s", name);
		made++;
	}
	printf("ufs: %u partitions of the phone's storage as /dev/ufs_NAME and /dev/rufs_NAME, read only, through the "
	    "loader's %u KB buffer\n", made, ufs_buffer_size >> 10);

	if (PE_parse_boot_argn("bcb_reboot", &on, sizeof(on)) && on == 1) {
		uint32_t offered = ufs_page->bcb_part_plus_one;
		const char *why = NULL;

		if (offered == 0 || offered > ufs_parts) {
			why = "the loader offers no misc (a loader 2.6, and its misc identity for this device)";
		} else if (strncmp(ufs_table[offered - 1].name, "misc", 5) != 0) {
			why = "the loader offers another partition";
		} else if (ufs_table[offered - 1].flags & BLOCK_PART_UNIT) {
			why = "it is a whole logical unit, not the partition";
		} else if (ufs_table[offered - 1].block_size != BCB_BYTES || ufs_table[offered - 1].blocks < 2) {
			why = "its blocks are not 4096 bytes";
		} else if (ufs_entry("misc") != offered - 1) {
			why = "another partition of the same name comes first";
		}
		if (why == NULL) {
			ufs_bcb_part = (int)(offered - 1);
			printf("bcb: misc's boot control block may be set to \"boot-recovery\" or cleared (its command alone)\n");
		} else {
			printf("bcb: misc's boot control block is not written: %s\n", why);
		}
	}
	on = 0;
	if (PE_parse_boot_argn("ufs_selftest", &on, sizeof(on)) && on == 1) {
		ufs_selftest();
	}
}
