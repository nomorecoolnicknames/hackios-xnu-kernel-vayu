/*
 * A file of one of this port's RAM disks, by path, for the drivers that want one before there is
 * a root or without asking it: the GPU's firmware (hakios.adreno.c), the Wi-Fi DSP's
 * (hakios.wifi.c), the audio DSP's (hakios.adsp.c) -- "fw/NAME" of the image that travels with
 * the kernel (md1; md0 when there is nothing else) or of the resident one (md0).
 *
 * The image is an APFS container (tools/apfs/mkapfs.py: what userland/build.sh makes).  A file
 * stored as it is in one piece is used where it lies in the RAM disk's memory, as the drivers
 * always did (build.sh has mkapfs.py store fw/ that way: --plain fw).
 * Any other APFS file is read out once by apfs_lib -- the same code as the volume's when
 * mounted, compressed or not -- into memory of its own, and kept: firmware is asked for once
 * or twice a session and its driver keeps it while the phone runs, so nothing gives it back.
 *
 * Firmware that is not in the RAM disks (the boot partition's payload is at most 63 MB: the modem's
 * 77 MB of modem.mdt/modem.bNN, wlanmdsp.mbn and the MCFG tree modem_pr/ do not fit) is looked for
 * next, each behind its own boot-arg (both off by default: the RAM disks alone, as before):
 *
 *   fw_part=1  in the phone's own firmware partitions, read only: fw/NAME is image/NAME of "modem"
 *              (NON-HLOS: the modem, wlanmdsp.mbn, modem_pr/, also adsp/cdsp/slpi and bdwlan) and
 *              then of "bluetooth" (BTFM: crbtfw21.tlv, crnv21.bin), FAT16 volumes both
 *              (bsd/dev/hakios.fat.c), through the storage's read-only path (bsd/dev/hakios.ufs.c,
 *              ufs=2). This is where the phone's Android took them from (/vendor/firmware_mnt and
 *              /vendor/bt_firmware are these two partitions), so the files are the ones this very
 *              phone was shipped and runs with; nothing is copied anywhere, nothing is written. They
 *              are there as soon as the storage is (before any root), and stay there when
 *              /private/var is lost.
 *   fw_var=1   in /private/var/hakios/<dir>/NAME of the running root (there once init has mounted it):
 *              the place of another HakiOS port (APSL 2.0, as this file): telegdyd/ios-vayu, branch
 *              vayu-dev, drivers/storage/hakios.image.c (var_file), where Wi-Fi of a flash boot
 *              reached a scan (its NOTES.md, run wifi8). Filled from the computer.
 *
 * hakios_image_file() keeps what it reads from those as it keeps the APFS copies (TFTP, BT, the
 * DSPs ask for theirs more than once). hakios_image_file_once() is for the one reader of the
 * large ones, vayu_pas.c: a file not in a RAM disk is read into memory that the caller gives back
 * once the image is written (77 MB that would otherwise stay wired while the phone runs).
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <miscfs/apfs/apfs_lib.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.fat.h>
#include <sys/hakios.input.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>

extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
const uint8_t *hakios_image_file_once(const char *dir, const char *name, uint64_t *size, uint64_t *allocated);

#define IMAGE_FILE_MAX          (256ull * 1024 * 1024)
#define IMAGE_PATH_MAX          (HAKIOS_FAT_PATH_MAX + 8)       /* "/fw/" and a path of the modem's MCFG tree */
#define VAR_DISK                (-1)            /* kept[].disk of a file of /private/var/hakios/<dir>/ */
#define PART_DISK               (-2)            /* kept[].disk of a file of the firmware partitions */

/* ---- APFS ---- */

struct mem_dev {
	const uint8_t *base;
	uint64_t       length;
};

static int
mem_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct mem_dev *m = ctx;

	if (off > m->length || len > m->length - off) {
		return EIO;
	}
	memcpy(buf, m->base + off, len);
	return 0;
}

static LCK_GRP_DECLARE(image_lck_grp, "hakios_image");
static LCK_MTX_DECLARE(image_lock, &image_lck_grp);    /* kept[]: the drivers start beside each other */
static struct {
	int       disk;
	char      path[IMAGE_PATH_MAX];
	uint8_t  *data;
	uint64_t  size;
} kept[128];

static const uint8_t *
apfs_file(int disk, const uint8_t *base, uint64_t length, const char *dir, const char *name, uint64_t *size)
{
	struct mem_dev m = { .base = base, .length = length };
	struct apfs_dev dev = { .ctx = &m, .size = length, .read = mem_read, .read_data = mem_read };
	struct apfs_nx *nx = NULL;
	struct apfs_vol *vol = NULL;
	struct apfs_inode ino;
	struct apfs_cfile cf;
	char path[IMAGE_PATH_MAX];
	uint64_t id = 0, fsize = 0, off = 0;
	uint8_t dt = 0, *data = NULL;
	uint32_t i;
	int error;

	if (length < 4096 || memcmp(base + 32, "NXSB", 4) != 0) {
		return NULL;
	}
	if (snprintf(path, sizeof(path), "/%s/%s", dir, name) >= (int)sizeof(path)) {
		return NULL;                    /* (never a shortened name: two files would be one in kept[]) */
	}
	lck_mtx_lock(&image_lock);
	for (i = 0; i < sizeof(kept) / sizeof(kept[0]) && kept[i].data != NULL; i++) {
		if (kept[i].disk == disk && strcmp(kept[i].path, path) == 0) {
			*size = kept[i].size;
			lck_mtx_unlock(&image_lock);
			return kept[i].data;
		}
	}
	nx = kalloc_type(struct apfs_nx, Z_WAITOK | Z_ZERO);
	vol = kalloc_type(struct apfs_vol, Z_WAITOK | Z_ZERO);
	if (nx == NULL || vol == NULL) {
		goto out;
	}
	if (apfs_nx_open(nx, &dev) != 0) {
		kfree_type(struct apfs_nx, nx);
		nx = NULL;
		goto out;
	}
	error = ENOENT;
	for (uint32_t v = 0; v < APFS_NX_MAX_FILE_SYSTEMS && error != 0; v++) {
		if (nx->fs_oid[v] != 0 && apfs_vol_open(nx, v, vol) == 0) {
			error = apfs_path_lookup(vol, path, &id, &dt);
		}
	}
	if (error == 0) {
		error = apfs_inode_get(vol, id, &ino);
	}
	if (error == 0 && (ino.mode & APFS_S_IFMT) != APFS_S_IFREG) {
		error = EISDIR;
	}
	if (error == 0) {
		error = apfs_inode_size(vol, &ino, &fsize);
	}
	if (error == 0 && (fsize == 0 || fsize > IMAGE_FILE_MAX)) {
		error = EFBIG;
	}
	if (error == 0) {
		uint64_t addr = 0;

		/* in one piece as it is: where it lies, no copy */
		if (apfs_file_in_place(vol, &ino, &addr) == 0 && addr <= length / nx->block_size &&
		    fsize <= length - addr * nx->block_size) {
			apfs_nx_close(nx);
			*size = fsize;
			data = __DECONST(uint8_t *, base) + addr * nx->block_size;
			goto out;
		}
		if (i == sizeof(kept) / sizeof(kept[0])) {
			printf("hakios_image: %s of /dev/md%d: kept %lu files already, no room for a copy of another\n", path, disk,
			    (unsigned long)i);
			error = ENOSPC;
		}
	}
	if (error == 0) {
		data = kalloc_data((vm_size_t)fsize, Z_WAITOK);
		error = data == NULL ? ENOMEM : 0;
	}
	memset(&cf, 0, sizeof(cf));
	cf.held = APFS_NO_PIECE;
	while (error == 0 && off < fsize) {
		size_t got = 0;

		error = apfs_read_file(vol, &ino, &cf, off, data + off, (size_t)(fsize - off), &got);
		if (error == 0 && got == 0) {
			error = EIO;
		}
		off += got;
	}
	apfs_cfile_fini(&cf);
	apfs_nx_close(nx);
	if (error == 0) {
		kept[i].disk = disk;
		strlcpy(kept[i].path, path, sizeof(kept[i].path));
		kept[i].size = fsize;
		kept[i].data = data;            /* (last: a reader of the table sees all of it or none) */
		*size = fsize;
	} else if (data != NULL) {
		kfree_data(data, (vm_size_t)fsize);
		data = NULL;
	}
out:
	lck_mtx_unlock(&image_lock);
	if (vol != NULL) {
		kfree_type(struct apfs_vol, vol);
	}
	if (nx != NULL) {
		kfree_type(struct apfs_nx, nx);
	}
	return data;
}

/* ---- what is not in a RAM disk: kept[] ---- */

static const uint8_t *
kept_find(int disk, const char *key, uint64_t *size)
{
	const uint8_t *data = NULL;

	lck_mtx_lock(&image_lock);
	for (uint32_t i = 0; i < sizeof(kept) / sizeof(kept[0]) && kept[i].data != NULL; i++) {
		if (kept[i].disk == disk && strcmp(kept[i].path, key) == 0) {
			*size = kept[i].size;
			data = kept[i].data;
			break;
		}
	}
	lck_mtx_unlock(&image_lock);
	return data;
}

/* data (of size bytes, ours) kept under key; the copy another driver kept meanwhile wins. NULL: no room. */
static const uint8_t *
kept_add(int disk, const char *key, uint8_t *data, uint64_t *size)
{
	uint32_t i;

	lck_mtx_lock(&image_lock);
	for (i = 0; i < sizeof(kept) / sizeof(kept[0]) && kept[i].data != NULL; i++) {
		if (kept[i].disk == disk && strcmp(kept[i].path, key) == 0) {
			break;
		}
	}
	if (i == sizeof(kept) / sizeof(kept[0])) {
		lck_mtx_unlock(&image_lock);
		printf("hakios_image: %s: kept %lu files already, no room for another\n", key, (unsigned long)i);
		kfree_data(data, (vm_size_t)*size);
		return NULL;
	}
	if (kept[i].data != NULL) {
		lck_mtx_unlock(&image_lock);
		kfree_data(data, (vm_size_t)*size);
		*size = kept[i].size;
		return kept[i].data;
	}
	kept[i].disk = disk;
	strlcpy(kept[i].path, key, sizeof(kept[i].path));
	kept[i].size = *size;
	kept[i].data = data;            /* (last: a reader of the table sees all of it or none) */
	lck_mtx_unlock(&image_lock);
	return data;
}

static bool
boot_arg_on(const char *name, int *cached)
{
	if (*cached < 0) {
		uint32_t value = 0;

		*cached = PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
	}
	return *cached > 0;
}

/* ---- the phone's firmware partitions (fw_part=1) ---- */

static const char *const part_names[] = { "modem", "bluetooth" };
static struct part_volume {
	bool tried, open;
	uint32_t index;
	struct hakios_fat fs;
	uint8_t *fat, *scratch;
} part_volume[sizeof(part_names) / sizeof(part_names[0])];
static LCK_MTX_DECLARE(part_lock, &image_lck_grp);     /* part_volume[] and its scratch buffers */
static int fw_part = -1;

static int
part_read(void *ctx, uint64_t offset, void *buf, uint64_t len)
{
	struct part_volume *v = ctx;

	return hakios_ufs_part_read(v->index, offset, buf, len);
}

/* With part_lock held. Not there yet (no storage yet): false, and asked again next time. */
static bool
part_open(struct part_volume *v, const char *name)
{
	uint64_t bytes = 0;
	int error;

	if (v->open || v->tried) {
		return v->open;
	}
	if (!hakios_ufs_part(name, &v->index, &bytes)) {
		return false;
	}
	v->tried = true;
	memset(&v->fs, 0, sizeof(v->fs));
	v->fs.read = part_read;
	v->fs.ctx = v;
	v->fs.size = bytes;
	error = hakios_fat_open(&v->fs);
	if (error == 0) {
		v->fat = kalloc_data(v->fs.fat_bytes, Z_WAITOK);
		v->scratch = kalloc_data(v->fs.cluster, Z_WAITOK);
		error = v->fat == NULL || v->scratch == NULL ? ENOMEM : hakios_fat_load(&v->fs, v->fat, v->fs.fat_bytes);
	}
	if (error != 0) {
		printf("hakios_image: the %s partition (%llu bytes) is not read for firmware: %d\n", name, bytes, error);
		if (v->fat != NULL) {
			kfree_data(v->fat, v->fs.fat_bytes);
		}
		if (v->scratch != NULL) {
			kfree_data(v->scratch, v->fs.cluster);
		}
		v->fat = v->scratch = NULL;
		return false;
	}
	v->open = true;
	printf("hakios_image: firmware from the %s partition, read only: FAT%u, %u clusters of %u bytes\n", name,
	    v->fs.type, v->fs.clusters, v->fs.cluster);
	return true;
}

/* fw/NAME as image/NAME of the firmware partitions, read whole into memory of the caller's. */
static uint8_t *
part_file(const char *dir, const char *name, uint64_t *size)
{
	struct hakios_fat_file file;
	char path[IMAGE_PATH_MAX];
	uint8_t *data = NULL;

	if (strcmp(dir, "fw") != 0 || !boot_arg_on("fw_part", &fw_part) ||
	    snprintf(path, sizeof(path), "image/%s", name) >= (int)sizeof(path) || !hakios_fat_path_ok(path)) {
		return NULL;
	}
	lck_mtx_lock(&part_lock);
	for (uint32_t p = 0; p < sizeof(part_names) / sizeof(part_names[0]) && data == NULL; p++) {
		struct part_volume *v = &part_volume[p];
		int error;

		if (!part_open(v, part_names[p]) ||
		    hakios_fat_lookup(&v->fs, path, v->scratch, v->fs.cluster, &file) != 0) {
			continue;
		}
		if (file.size == 0 || file.size > IMAGE_FILE_MAX) {
			continue;
		}
		data = kalloc_data(file.size, Z_WAITOK);
		if (data == NULL) {
			break;
		}
		error = hakios_fat_read(&v->fs, &file, data, file.size);
		if (error != 0) {
			printf("hakios_image: %s of the %s partition: read error %d\n", path, part_names[p], error);
			kfree_data(data, file.size);
			data = NULL;
			continue;
		}
		*size = file.size;
	}
	lck_mtx_unlock(&part_lock);
	return data;
}

/* ---- /private/var/hakios/<dir>/ (fw_var=1; telegdyd/ios-vayu vayu-dev, hakios.image.c var_file) ---- */

static int fw_var = -1;

static uint8_t *
var_file(const char *dir, const char *name, uint64_t *size)
{
	char path[IMAGE_PATH_MAX + 32];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (!boot_arg_on("fw_var", &fw_var) || rootvnode == NULLVP || !hakios_fat_path_ok(name) ||
	    snprintf(path, sizeof(path), "/private/var/hakios/%s/%s", dir, name) >= (int)sizeof(path)) {
		return NULL;                    /* (also no "." or ".." in it, no root yet) */
	}
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_vtype(vnode) != VREG || vnode_getattr(vnode, &attributes, context) != 0 ||
	    attributes.va_data_size == 0 || attributes.va_data_size > IMAGE_FILE_MAX) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data((vm_size_t)attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("hakios_image: %s: read error %d, %d bytes left\n", path, error, left);
		kfree_data(data, (vm_size_t)attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

/* ---- the drivers' ---- */

static const uint8_t *
ram_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	for (int disk = 1; disk >= 0; disk--) {
		vm_offset_t base = 0;
		uint64_t length = 0;
		const uint8_t *data;
		dev_t dev = mdevlookup(disk);

		if (dev == (dev_t)-1 || mdevgetkernelrange(dev, &base, &length) != 0) {
			continue;
		}
		data = apfs_file(disk, (const uint8_t *)base, length, dir, name, size);
		if (data != NULL) {
			if (disk_out != NULL) {
				*disk_out = disk;
			}
			return data;
		}
	}
	return NULL;
}

/* Not in a RAM disk: the partitions, then /private/var. *disk: PART_DISK or VAR_DISK. Memory of the caller's. */
static uint8_t *
flash_file(const char *dir, const char *name, uint64_t *size, int *disk)
{
	uint8_t *data = part_file(dir, name, size);

	*disk = PART_DISK;
	if (data == NULL) {
		data = var_file(dir, name, size);
		*disk = VAR_DISK;
	}
	return data;
}

const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	const uint8_t *data = ram_file(dir, name, size, disk_out);
	char key[IMAGE_PATH_MAX];
	uint8_t *ours;
	int disk;

	if (data != NULL || snprintf(key, sizeof(key), "/%s/%s", dir, name) >= (int)sizeof(key)) {
		return data;
	}
	for (disk = PART_DISK; disk <= VAR_DISK; disk++) {
		data = kept_find(disk, key, size);
		if (data != NULL) {
			if (disk_out != NULL) {
				*disk_out = disk;
			}
			return data;
		}
	}
	ours = flash_file(dir, name, size, &disk);
	if (ours == NULL) {
		return NULL;
	}
	data = kept_add(disk, key, ours, size);
	if (data != NULL && disk_out != NULL) {
		*disk_out = disk;
	}
	return data;
}

const uint8_t *
hakios_image_file_once(const char *dir, const char *name, uint64_t *size, uint64_t *allocated)
{
	const uint8_t *data = ram_file(dir, name, size, NULL);
	char key[IMAGE_PATH_MAX];
	int disk;

	*allocated = 0;
	if (data != NULL || snprintf(key, sizeof(key), "/%s/%s", dir, name) >= (int)sizeof(key)) {
		return data;
	}
	for (disk = PART_DISK; disk <= VAR_DISK; disk++) {
		data = kept_find(disk, key, size);      /* (another driver's copy, kept: used, not given back) */
		if (data != NULL) {
			return data;
		}
	}
	data = flash_file(dir, name, size, &disk);
	if (data != NULL) {
		*allocated = *size;
	}
	return data;
}
