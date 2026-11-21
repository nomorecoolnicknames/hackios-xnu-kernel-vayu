/*
 * macOS's view of this port's disks and APFS containers (boot-arg macos_abi=1 only; without it nothing here
 * is made and every function is a no-op).
 *
 * macOS 13.4's launchd runs apfs_boot_util in its boot tasks mount-phase-1 and -2, and it finds the volumes of
 * the boot container by their role in the IOKit registry, as Apple's APFS kext publishes them, and mounts them
 * by their BSD nodes (22F66's apfs_boot_util, arm64e, and APFS.framework of its shared cache):
 *   - the root device is statfs("/")'s f_mntfromname, and it has to read /dev/diskNsM: the Data role is
 *     looked for by "/dev/disk%ds%ds%d" and the volume's index is the M of it;
 *   - APFSVolumeRoleFind(dev, role): the AppleAPFSContainer that is the parent of the volume diskNsM (or the
 *     child of the media diskN), its children of class AppleAPFSVolume, each one's role asked of the
 *     container's user client (IOServiceOpen type 0, IOConnectCallStructMethod selector 9, the fs index of
 *     its location "M" in, the role out), the matching ones' "BSD Name";
 *   - APFSVolumeRole(diskNsM): the volume's "RoleValue", or else selector 9;
 *   - APFSGetVolumeGroupID(diskNsM): the volume's "VolGroupUUID" (all zeros: not in a volume group, which
 *     mount-phase-2 takes for "no Data volume");
 *   - the iBoot System Container's volumes (xART, which is not optional, Preboot, Hardware): an
 *     AppleAPFSContainer whose volumes have a "Role" of one name ("xART", ...) and whose nearest IOMedia
 *     above the AppleAPFSMedia has the "Content Hint" 69646961-6700-11AA-AA11-00306543ECAC;
 *   - mount("apfs", dir, flags, struct apfs_mount_args { char *fspec = "/dev/diskNsM"; ... }).
 * So, the way Apple's storage stack does it, for each block device a driver of this port registers
 * (apfs_media_disk_register: rvblkN, md0, ufs_NAME; nbd0 when that driver is in the tree) an IOMedia with
 * its BSD name, and for each APFS container on one -- found when the APFS driver mounts one of it (the RAM
 * disk as the root; the system volume by init) and then all of the device's containers, which lie back to
 * back -- the nodes of Apple's APFS (iokit/Kernel/HakiosAPFSMedia.cpp) with block and character devices
 * /dev/diskN, /dev/rdiskN (the container) and /dev/diskNsM, /dev/rdiskNsM (volume M - 1). Each of those
 * devices is the container's bytes, as Apple's are: reads go to the disk at the container's offset; they are
 * read only (this port's APFS driver is). A volume is mounted by its node (apfs_vfsops.c), and the APFS
 * driver names a volume it mounted after its node in f_mntfromname, whichever device it was given.
 *
 * Which GPT type a container's space has ("Content Hint"): Apple_APFS (7C3457EF-0000-11AA-AA11-
 * 00306543ECAC), save the one boot-arg apfs_isc=DISK:N names (container N of /dev/DISK, from 0) as the iBoot
 * System Container -- this port's disks have no partition map to say it.
 */

#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <miscfs/apfs/apfs_media.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/buf_internal.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define MEDIA_MAX_DISKS         32
#define MEDIA_MAX_CONTAINERS    32
#define MEDIA_READ_CHUNK        (128 * 1024)

#define GUID_APPLE_APFS         "7C3457EF-0000-11AA-AA11-00306543ECAC"
#define GUID_APPLE_APFS_ISC     "69646961-6700-11AA-AA11-00306543ECAC"

static LCK_GRP_DECLARE(media_lck_grp, "apfs_media");
static LCK_MTX_DECLARE(media_lock, &media_lck_grp);

static struct apfs_media_disk media_disks[MEDIA_MAX_DISKS];
static uint32_t media_ndisks;
static struct apfs_media_container *media_conts[MEDIA_MAX_CONTAINERS];   /* by unit; never taken away */
static uint32_t media_nconts;
static bool media_cont_published[MEDIA_MAX_CONTAINERS];
static dev_t media_probed_devs[MEDIA_MAX_DISKS];
static uint32_t media_nprobed;
static bool media_iokit;
static int media_bmajor = -1, media_cmajor = -1;

const char *
apfs_media_role_name(uint16_t role)
{
	switch (role) {
	case 0x0001: return "System";
	case 0x0002: return "User";
	case 0x0004: return "Recovery";
	case 0x0008: return "VM";
	case 0x0010: return "Preboot";
	case 0x0020: return "Installer";
	case 0x0040: return "Data";
	case 0x0080: return "Baseband";
	case 0x00c0: return "Update";
	case 0x0100: return "xART";
	case 0x0140: return "Hardware";
	case 0x0180: return "Backup";
	case 0x01c0: return "Sidecar";
	case 0x0200: return "Enterprise";
	case 0x0280: return "Prelogin";
	default:     return NULL;   /* none, or one this does not know a name of */
	}
}

/* ------------------------------------------------------------------ the disks */

void
apfs_media_disk_register(const char *name, dev_t bdev, dev_t cdev, uint32_t block_size, uint64_t blocks,
    bool writable)
{
	struct apfs_media_disk *d = NULL;
	bool now;

	if (hakios_macos_abi == 0) {
		return;
	}
	lck_mtx_lock(&media_lock);
	if (media_ndisks < MEDIA_MAX_DISKS) {
		d = &media_disks[media_ndisks];
		strlcpy(d->name, name, sizeof(d->name));
		d->bdev = bdev;
		d->cdev = cdev;
		d->block_size = block_size;
		d->blocks = blocks;
		d->writable = writable;
		os_atomic_store(&media_ndisks, media_ndisks + 1, release);     /* (read without the lock) */
	}
	now = media_iokit;
	lck_mtx_unlock(&media_lock);
	if (d == NULL) {
		printf("apfs_media: no room for disk %s\n", name);
	} else if (now) {
		hakios_iomedia_publish_disk(d);
	}
}

struct apfs_media_disk *
apfs_media_disk_find(dev_t bdev)
{
	uint32_t n = os_atomic_load(&media_ndisks, acquire);

	for (uint32_t i = 0; i < n; i++) {
		if (media_disks[i].bdev == bdev) {
			return &media_disks[i];
		}
	}
	return NULL;
}

const char *
apfs_media_disk_name(dev_t under)
{
	struct apfs_media_disk *d = apfs_media_disk_find(under);

	return d == NULL ? NULL : d->name;
}

/* ------------------------------------------------------------------ the containers' devices */

static struct apfs_media_container *
media_lookup(dev_t dev, uint32_t *slice)
{
	uint32_t unit = (uint32_t)minor(dev) >> 8, s = (uint32_t)minor(dev) & 0xff;
	struct apfs_media_container *c;

	if (media_bmajor < 0 || (major(dev) != media_bmajor && major(dev) != media_cmajor) ||
	    unit >= os_atomic_load(&media_nconts, acquire)) {
		return NULL;
	}
	c = media_conts[unit];
	if (s != 0) {
		uint32_t i;

		for (i = 0; i < c->nvols && c->vols[i].index != s - 1; i++) {
		}
		if (i == c->nvols) {
			return NULL;
		}
	}
	if (slice != NULL) {
		*slice = s;
	}
	return c;
}

bool
apfs_media_is_volume_dev(dev_t dev, dev_t *under, uint32_t *container, int *fsindex)
{
	uint32_t s;
	struct apfs_media_container *c = media_lookup(dev, &s);

	if (c == NULL) {
		return false;
	}
	if (under != NULL) {
		*under = c->under;
	}
	if (container != NULL) {
		*container = c->index;
	}
	if (fsindex != NULL) {
		*fsindex = s == 0 ? -1 : (int)s - 1;
	}
	return true;
}

static int
media_open(dev_t dev, int flags, __unused int devtype, struct proc *p)
{
	struct apfs_media_container *c = media_lookup(dev, NULL);

	if (c == NULL) {
		return ENXIO;
	}
	if (flags & FWRITE) {
		return EROFS;
	}
	return (*bdevsw[major(c->under)].d_open)(c->under, FREAD, S_IFBLK, p);
}

static int
media_close(dev_t dev, __unused int flags, __unused int devtype, struct proc *p)
{
	struct apfs_media_container *c = media_lookup(dev, NULL);

	if (c == NULL) {
		return ENXIO;
	}
	return (*bdevsw[major(c->under)].d_close)(c->under, FREAD, S_IFBLK, p);
}

static void
media_iodone(buf_t io, void *arg)
{
	buf_t bp = arg;
	uint32_t short_by = buf_count(bp) - buf_count(io);

	buf_seterror(bp, buf_error(io));
	buf_setresid(bp, buf_resid(io) + short_by);
	buf_free(io);
	buf_biodone(bp);
}

/* A piece of the container: the same piece of the disk, at the container's offset. */
static void
media_strategy(buf_t bp)
{
	struct apfs_media_container *c = media_lookup(buf_device(bp), NULL);
	uint64_t off;
	uint32_t n;
	buf_t io;

	if (c == NULL || !(buf_flags(bp) & B_READ)) {
		buf_seterror(bp, c == NULL ? ENXIO : EROFS);
		buf_biodone(bp);
		return;
	}
	off = (uint64_t)buf_blkno(bp) * c->block_size;
	if (off >= c->size) {
		if (off != c->size) {
			buf_seterror(bp, EINVAL);
		}
		buf_setresid(bp, buf_count(bp));       /* at the end: nothing more, not an error */
		buf_biodone(bp);
		return;
	}
	n = buf_count(bp);
	if (off + n > c->size) {
		n = (uint32_t)(c->size - off);
	}
	io = buf_clone(bp, 0, (int)n, media_iodone, bp);
	if (io == NULL) {
		buf_seterror(bp, ENOMEM);
		buf_biodone(bp);
		return;
	}
	io->b_dev = c->under;
	buf_setblkno(io, (daddr64_t)((c->base + off) / c->block_size));
	buf_setlblkno(io, (daddr64_t)((c->base + off) / c->block_size));
	(*bdevsw[major(c->under)].d_strategy)(io);
}

static int
media_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	struct apfs_media_container *c = media_lookup(dev, NULL);

	if (c == NULL) {
		return ENXIO;
	}
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = c->block_size;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == c->block_size ? 0 : ENOTSUP;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = c->size / c->block_size;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETFEATURES:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBYTECOUNTREAD:
		*(uint64_t *)data = MEDIA_READ_CHUNK;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
media_psize(dev_t dev)
{
	struct apfs_media_container *c = media_lookup(dev, NULL);

	return c == NULL ? -1 : (int)MIN(c->size / c->block_size, (uint64_t)INT_MAX);
}

/* read(2) of /dev/rdiskN[sM]: through memory of the kernel's, whatever the disk's own read(2) does */
static int
media_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct apfs_media_container *c = media_lookup(dev, NULL);
	char *kbuf;
	int error = 0;

	if (c == NULL) {
		return ENXIO;
	}
	if (uio_offset(uio) < 0 || (uio_offset(uio) % c->block_size) != 0) {
		return EINVAL;
	}
	kbuf = kalloc_data(MEDIA_READ_CHUNK, Z_WAITOK);
	if (kbuf == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < c->size) {
		uint64_t off = (uint64_t)uio_offset(uio);
		uint64_t n = MIN((uint64_t)uio_resid(uio), MIN((uint64_t)MEDIA_READ_CHUNK, c->size - off));
		buf_t bp;

		n -= n % c->block_size;
		if (n == 0) {
			error = EINVAL;         /* less than a block asked for */
			break;
		}
		bp = buf_alloc(NULLVP);
		if (bp == NULL) {
			error = ENOMEM;
			break;
		}
		buf_reset(bp, B_READ);
		bp->b_dev = c->under;
		buf_setblkno(bp, (daddr64_t)((c->base + off) / c->block_size));
		buf_setlblkno(bp, (daddr64_t)((c->base + off) / c->block_size));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)kbuf);
		(*bdevsw[major(c->under)].d_strategy)(bp);
		error = (int)buf_biowait(bp);
		if (error == 0) {
			uint64_t got = n - buf_resid(bp);

			error = uiomove(kbuf, (int)got, uio);
			if (error == 0 && got < n) {
				buf_free(bp);
				break;          /* the disk's end */
			}
		}
		buf_free(bp);
	}
	kfree_data(kbuf, MEDIA_READ_CHUNK);
	return error;
}

static int
media_write(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return EROFS;
}

static const struct bdevsw media_bdevsw = {
	.d_open     = media_open,
	.d_close    = media_close,
	.d_strategy = media_strategy,
	.d_ioctl    = media_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = media_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw media_cdevsw = {
	.d_open       = media_open,
	.d_close      = media_close,
	.d_read       = media_read,
	.d_write      = media_write,
	.d_ioctl      = media_ioctl,
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

/* ------------------------------------------------------------------ publishing */

bool
apfs_media_probed(dev_t under)
{
	bool got = false;

	lck_mtx_lock(&media_lock);
	for (uint32_t i = 0; i < media_nprobed && !got; i++) {
		got = media_probed_devs[i] == under;
	}
	lck_mtx_unlock(&media_lock);
	return got;
}

/* Is container `index` of the disk the one boot-arg apfs_isc=DISK:N names? */
static bool
media_is_isc(const char *disk, uint32_t index)
{
	char arg[48];
	char *colon;

	if (disk == NULL || !PE_parse_boot_arg_str("apfs_isc", arg, sizeof(arg))) {
		return false;
	}
	colon = strchr(arg, ':');
	if (colon != NULL) {
		*colon = '\0';
	}
	return strcmp(arg, disk) == 0 && (colon == NULL ? 0 : (uint32_t)strtoul(colon + 1, NULL, 10)) == index;
}

/*
 * The containers the APFS driver found on one disk (all of them, in their order on it): units, the devices
 * and their nodes, and the registry's nodes once the registry is up. Once a disk.
 */
void
apfs_media_publish(const struct apfs_media_container *found, uint32_t count)
{
	struct apfs_media_container *made[MEDIA_MAX_CONTAINERS];
	uint32_t nmade = 0;
	bool now;

	if (hakios_macos_abi == 0 || count == 0) {
		return;
	}
	lck_mtx_lock(&media_lock);
	for (uint32_t i = 0; i < media_nprobed; i++) {
		if (media_probed_devs[i] == found[0].under) {
			lck_mtx_unlock(&media_lock);
			return;
		}
	}
	if (media_nprobed < MEDIA_MAX_DISKS) {
		media_probed_devs[media_nprobed++] = found[0].under;
	}
	if (media_bmajor < 0) {
		media_bmajor = bdevsw_add(-1, &media_bdevsw);
		media_cmajor = media_bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &media_cdevsw, media_bmajor);
		if (media_cmajor < 0) {
			printf("apfs_media: no device numbers to be had\n");
			media_bmajor = -1;
			lck_mtx_unlock(&media_lock);
			return;
		}
	}
	for (uint32_t i = 0; i < count && media_nconts < MEDIA_MAX_CONTAINERS; i++) {
		struct apfs_media_container *c = kalloc_type(struct apfs_media_container, Z_WAITOK | Z_ZERO);

		if (c == NULL) {
			break;
		}
		*c = found[i];
		c->unit = media_nconts;
		c->alone = count == 1 && c->base == 0;
		strlcpy(c->content_hint, media_is_isc(apfs_media_disk_name(c->under), c->index) ?
		    GUID_APPLE_APFS_ISC : GUID_APPLE_APFS, sizeof(c->content_hint));
		media_conts[c->unit] = c;
		os_atomic_store(&media_nconts, c->unit + 1, release);
		made[nmade++] = c;
	}
	now = media_iokit;
	for (uint32_t i = 0; i < nmade && now; i++) {
		media_cont_published[made[i]->unit] = true;
	}
	lck_mtx_unlock(&media_lock);

	for (uint32_t i = 0; i < nmade; i++) {
		struct apfs_media_container *c = made[i];
		const char *iscs = strcmp(c->content_hint, GUID_APPLE_APFS_ISC) == 0 ? ", the iBoot System Container" : "";

		(void)devfs_make_node(makedev(media_bmajor, c->unit << 8), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640,
		    "disk%u", c->unit);
		(void)devfs_make_node(makedev(media_cmajor, c->unit << 8), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640,
		    "rdisk%u", c->unit);
		printf("apfs_media: /dev/disk%u: container %u of %s (at %llu MB, %llu MB)%s, %u volumes\n", c->unit,
		    c->index, c->under_name, (unsigned long long)(c->base >> 20), (unsigned long long)(c->size >> 20), iscs,
		    c->nvols);
		for (uint32_t v = 0; v < c->nvols; v++) {
			const char *rn = apfs_media_role_name(c->vols[v].role);
			uint32_t slice = c->vols[v].index + 1;

			(void)devfs_make_node(makedev(media_bmajor, (c->unit << 8) | slice), DEVFS_BLOCK, UID_ROOT,
			    GID_OPERATOR, 0640, "disk%us%u", c->unit, slice);
			(void)devfs_make_node(makedev(media_cmajor, (c->unit << 8) | slice), DEVFS_CHAR, UID_ROOT,
			    GID_OPERATOR, 0640, "rdisk%us%u", c->unit, slice);
			printf("apfs_media:   /dev/disk%us%u \"%s\", role %s (0x%x)\n", c->unit, slice, c->vols[v].name,
			    rn != NULL ? rn : "none", c->vols[v].role);
		}
		if (now) {
			hakios_iomedia_publish_container(c, media_bmajor, media_cmajor);
		}
	}
}

/* The name of the node of volume `fsindex` of the disk's container `container`, if it has one. */
bool
apfs_media_volume_name(dev_t under, uint32_t container, uint32_t fsindex, char *buf, size_t len)
{
	uint32_t n = os_atomic_load(&media_nconts, acquire);

	for (uint32_t u = 0; u < n; u++) {
		struct apfs_media_container *c = media_conts[u];

		if (c->under != under || c->index != container) {
			continue;
		}
		for (uint32_t v = 0; v < c->nvols; v++) {
			if (c->vols[v].index == fsindex) {
				snprintf(buf, len, "/dev/disk%us%u", c->unit, fsindex + 1);
				return true;
			}
		}
	}
	return false;
}

/* The platform expert, once the registry is up: what came before it goes in now. */
void
apfs_media_iokit_ready(void)
{
	struct apfs_media_container *late[MEDIA_MAX_CONTAINERS];
	uint32_t ndisks, nlate = 0;

	if (hakios_macos_abi == 0) {
		return;
	}
	lck_mtx_lock(&media_lock);
	if (media_iokit) {
		lck_mtx_unlock(&media_lock);
		return;
	}
	media_iokit = true;
	ndisks = media_ndisks;
	for (uint32_t u = 0; u < media_nconts; u++) {
		if (!media_cont_published[u]) {
			media_cont_published[u] = true;
			late[nlate++] = media_conts[u];
		}
	}
	lck_mtx_unlock(&media_lock);
	for (uint32_t i = 0; i < ndisks; i++) {
		hakios_iomedia_publish_disk(&media_disks[i]);
	}
	for (uint32_t i = 0; i < nlate; i++) {
		hakios_iomedia_publish_container(late[i], media_bmajor, media_cmajor);
	}
}
