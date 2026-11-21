/*
 * The disks and APFS containers of this port as macOS's storage stack shows them (boot-arg macos_abi=1 only):
 * IOKit nodes for the block devices, and for each APFS container on one the nodes Apple's APFS publishes --
 * AppleAPFSMedia /dev/diskN, AppleAPFSContainer, AppleAPFSVolume /dev/diskNsM with its role -- with the
 * /dev/diskN and /dev/diskNsM block and character devices they name. See apfs_media.c.
 *
 * Shared by the BSD side (apfs_media.c, the APFS driver, the block drivers) and the IOKit side
 * (iokit/Kernel/HakiosAPFSMedia.cpp).
 */
#ifndef _MISCFS_APFS_APFS_MEDIA_H_
#define _MISCFS_APFS_APFS_MEDIA_H_

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

__BEGIN_DECLS

#define APFS_MEDIA_NAMELEN      32
#define APFS_MEDIA_VOLNAMELEN   256
#define APFS_MEDIA_MAX_VOLS     16      /* the volumes of one container this publishes (APFS allows 100) */

/* A block device of a driver of this port (rvblkN, md0, ufs_NAME, nbd0). */
struct apfs_media_disk {
	char     name[APFS_MEDIA_NAMELEN];      /* its BSD name: /dev/NAME */
	dev_t    bdev, cdev;                    /* cdev NODEV when it has none */
	uint32_t block_size;
	uint64_t blocks;
	bool     writable;
	void    *iokit;                         /* its IOMedia, once published (HakiosAPFSMedia.cpp) */
};

/* One volume of a published container. */
struct apfs_media_vol {
	uint32_t index;                         /* fs index in the container: /dev/diskNs<index + 1> */
	uint16_t role;                          /* apfs_role of its superblock */
	uint8_t  uuid[16];
	uint8_t  group[16];                     /* apfs_volume_group_id: zeros, not in a volume group */
	char     name[APFS_MEDIA_VOLNAMELEN];
};

/* An APFS container on a disk: /dev/diskN, and /dev/diskNsM for its volumes. */
struct apfs_media_container {
	uint32_t unit;                          /* N of /dev/diskN */
	dev_t    under;                         /* the disk's block device */
	char     under_name[APFS_MEDIA_NAMELEN];
	uint32_t index;                         /* which container of the disk, from 0 (they lie back to back) */
	uint64_t base;                          /* where on the disk it starts, bytes */
	uint64_t size;                          /* nx_block_count * nx_block_size */
	uint32_t block_size;                    /* the disk's */
	uint32_t nx_block_size;
	uint8_t  uuid[16];                      /* nx_uuid */
	char     content_hint[40];              /* the GPT type of the space it is in: Apple_APFS, or iSC */
	bool     alone;                         /* the disk's only container, from its start: the disk is its space */
	uint32_t nvols;
	struct apfs_media_vol vols[APFS_MEDIA_MAX_VOLS];
};

/* Block drivers: a disk has come (macos_abi=1: an IOMedia for it; otherwise nothing). */
void apfs_media_disk_register(const char *name, dev_t bdev, dev_t cdev, uint32_t block_size, uint64_t blocks,
    bool writable);

/* The APFS driver. */
bool apfs_media_is_volume_dev(dev_t dev, dev_t *under, uint32_t *container, int *fsindex);
void apfs_media_publish(const struct apfs_media_container *c, uint32_t count);
bool apfs_media_probed(dev_t under);
bool apfs_media_volume_name(dev_t under, uint32_t container, uint32_t fsindex, char *buf, size_t len);
const char *apfs_media_disk_name(dev_t under);
struct apfs_media_disk *apfs_media_disk_find(dev_t bdev);
const char *apfs_media_role_name(uint16_t role);

/* The IOKit side (HakiosAPFSMedia.cpp): called with no lock held. */
void hakios_iomedia_publish_disk(struct apfs_media_disk *d);
void hakios_iomedia_publish_container(const struct apfs_media_container *c, int bmajor, int cmajor);
void apfs_media_iokit_ready(void);      /* the platform expert: the registry is up, publish what waited */

extern uint32_t hakios_macos_abi;       /* bsd/kern/kern_exec.c: boot-arg macos_abi=1 */

__END_DECLS

#endif /* _MISCFS_APFS_APFS_MEDIA_H_ */
