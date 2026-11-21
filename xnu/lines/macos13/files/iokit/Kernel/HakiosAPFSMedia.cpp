/*
 * The registry's side of bsd/miscfs/apfs/apfs_media.c (boot-arg macos_abi=1 only): this port's disks and
 * APFS containers as macOS's storage stack and Apple's APFS kext show them, for macOS 13.4's apfs_boot_util
 * (launchd's boot tasks mount-phase-1/2), APFS.framework and diskmanagementd. For each disk a driver
 * registers, and below it for each APFS container on it:
 *
 *   IOMedia "<disk> Media"                 BSD Name <disk> (rvblk1, md0, ...), Whole, Size, Writable, ...
 *     IOMedia "<disk> container <i>"       the container's space: Content Hint its GPT type (Apple_APFS,
 *                                          or the iBoot System Container's), Base, Size, Partition ID -- or,
 *                                          for a disk that is one container, the disk's IOMedia itself
 *       AppleAPFSContainerScheme
 *         AppleAPFSMedia                   BSD Name diskN: the container, Content EF57347C-...
 *           AppleAPFSContainer             UUID; its user client (type 0): selector 9, a volume's role
 *             AppleAPFSVolume <name>@M     BSD Name diskNsM, RoleValue, Role (one name), UUID, VolGroupUUID
 *
 * What reads which (22F66): apfs_boot_util, the volumes' Role and the Content Hint above them (the iBoot
 * System Container's xART, Preboot, Hardware), their BSD Name; APFS.framework's APFSVolumeRoleFind, the
 * container's children of class AppleAPFSVolume, their location (the fs index + 1) and selector 9;
 * APFSVolumeRole, RoleValue; APFSGetVolumeGroupID, VolGroupUUID. The classes have the names of Apple's
 * because those are what is matched (IOServiceMatching, IOObjectConformsTo); they hold no more than what is
 * listed. Nothing here writes: a role is not set (selector 9's set refused), the media are not writable.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <uuid/uuid.h>
#include <sys/proc.h>
#include <os/atomic_private.h>
#include "MiniDrivers.h"

#include "../../bsd/miscfs/apfs/apfs_media.h"

#define CONTENT_APFS_CONTAINER  "EF57347C-0000-11AA-AA11-00306543ECAC"
#define CONTENT_APFS_VOLUME     "41504653-0000-11AA-AA11-00306543ECAC"

class IOMedia : public IOService {
	OSDeclareDefaultStructors(IOMedia)
};

class AppleAPFSMedia : public IOMedia {
	OSDeclareDefaultStructors(AppleAPFSMedia)
};

class AppleAPFSVolume : public IOMedia {
	OSDeclareDefaultStructors(AppleAPFSVolume)
};

class AppleAPFSContainerScheme : public IOService {
	OSDeclareDefaultStructors(AppleAPFSContainerScheme)
};

class AppleAPFSContainer : public IOService {
	OSDeclareDefaultStructors(AppleAPFSContainer)

public:
	const struct apfs_media_container * fContainer;

	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties, IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

class AppleAPFSUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleAPFSUserClient)

public:
	const struct apfs_media_container * fContainer;

	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMedia, IOService)
OSDefineMetaClassAndStructors(AppleAPFSMedia, IOMedia)
OSDefineMetaClassAndStructors(AppleAPFSVolume, IOMedia)
OSDefineMetaClassAndStructors(AppleAPFSContainerScheme, IOService)
OSDefineMetaClassAndStructors(AppleAPFSContainer, IOService)
OSDefineMetaClassAndStructors(AppleAPFSUserClient, IOUserClient)

/* ------------------------------------------------------------------ the user client */

/*
 * Selector 9 (APFS.framework's APFSVolumeRoleFind and APFSVolumeRole, 22F66): in, 8 bytes { u32 fs index;
 * u16 0xffff to set the role, else 0; u16 the role to set }; out, 2 bytes: the volume's role.
 */
IOReturn
AppleAPFSUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	static uint32_t told;
	char name[32];

	if (selector == 9 && args->structureInputSize >= 4 && args->structureInput != NULL) {
		const uint8_t * in = (const uint8_t *) args->structureInput;
		uint32_t index;

		memcpy(&index, in, sizeof(index));
		if (args->structureInputSize >= 8 && in[4] == 0xff && in[5] == 0xff) {
			return kIOReturnNotWritable;    /* setting a role: this port's APFS does not write */
		}
		for (uint32_t i = 0; i < fContainer->nvols; i++) {
			if (fContainer->vols[i].index != index) {
				continue;
			}
			if (args->structureOutput == NULL || args->structureOutputSize < 2) {
				return kIOReturnBadArgument;
			}
			memcpy(args->structureOutput, &fContainer->vols[i].role, 2);
			args->structureOutputSize = 2;
			return kIOReturnSuccess;
		}
		return kIOReturnNotFound;
	}
	if (selector >= 32 || !(os_atomic_or_orig(&told, 1u << selector, relaxed) & (1u << selector))) {
		proc_selfname(name, sizeof(name));
		IOLog("AppleAPFSContainer: %s[%d] selector %u (scalars %u in, struct %u in, %u out): not served\n", name,
		    proc_selfpid(), selector, (unsigned) args->scalarInputCount, (unsigned) args->structureInputSize,
		    (unsigned) args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

IOReturn
AppleAPFSUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleAPFSContainer::newUserClient(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties, IOUserClient ** handler)
{
	AppleAPFSUserClient * client;

	if (type != 0) {
		IOLog("AppleAPFSContainer: open of type %u refused (0 only)\n", (unsigned) type);
		return kIOReturnUnsupported;
	}
	client = new AppleAPFSUserClient;
	if (client == NULL) {
		return kIOReturnNoMemory;
	}
	client->fContainer = fContainer;
	if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(this)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ publishing */

static void
setNumber(IOService * s, const char * key, uint64_t value, unsigned bits)
{
	OSNumber * n = OSNumber::withNumber((unsigned long long) value, bits);

	if (n != NULL) {
		s->setProperty(key, n);
		n->release();
	}
}

static void
setUUID(IOService * s, const char * key, const uint8_t * uuid)
{
	uuid_string_t str;

	uuid_unparse_upper(uuid, str);
	s->setProperty(key, str);
}

/* attach, start, publish; false (and the object let go of) if any of it fails */
static bool
publish(IOService * s, IOService * provider)
{
	if (!s->attach(provider)) {
		return false;
	}
	if (!s->start(provider)) {
		s->detach(provider);
		return false;
	}
	s->registerService();
	return true;
}

static void
mediaProperties(IOService * m, const char * bsdName, int bmajor, uint32_t minor, uint32_t unit, uint64_t size,
    uint32_t blockSize, bool whole, bool leaf, bool writable, const char * content)
{
	if (bsdName != NULL) {
		m->setProperty("BSD Name", bsdName);
		setNumber(m, "BSD Major", (uint32_t) bmajor, 32);
		setNumber(m, "BSD Minor", minor, 32);
		setNumber(m, "BSD Unit", unit, 32);
	}
	setNumber(m, "Size", size, 64);
	setNumber(m, "Preferred Block Size", blockSize, 64);
	m->setProperty("Whole", whole);
	m->setProperty("Leaf", leaf);
	m->setProperty("Writable", writable);
	m->setProperty("Removable", false);
	m->setProperty("Ejectable", false);
	m->setProperty("Open", false);
	m->setProperty("Content", content);
}

extern "C" void
hakios_iomedia_publish_disk(struct apfs_media_disk * d)
{
	IOService * platform = IOService::getPlatform();
	IOMedia * m;
	char name[64];

	if (platform == NULL || d->iokit != NULL) {
		return;
	}
	m = new IOMedia;
	if (m == NULL || !m->init()) {
		OSSafeReleaseNULL(m);
		return;
	}
	snprintf(name, sizeof(name), "%s Media", d->name);
	m->setName(name);
	mediaProperties(m, d->name, major(d->bdev), (uint32_t) minor(d->bdev), (uint32_t) minor(d->bdev),
	    d->blocks * d->block_size, d->block_size, true, false, d->writable, "");
	m->setProperty("Content Hint", "");
	if (!publish(m, platform)) {
		m->release();
		return;
	}
	d->iokit = m;           /* kept: the disks are never taken away */
	IOLog("HakiosAPFSMedia: IOMedia for /dev/%s (%llu MB%s)\n", d->name,
	    (unsigned long long) (d->blocks * d->block_size >> 20), d->writable ? ", writable" : "");
}

extern "C" void
hakios_iomedia_publish_container(const struct apfs_media_container * c, int bmajor, int cmajor)
{
	struct apfs_media_disk * d = apfs_media_disk_find(c->under);
	IOService * above = d != NULL && d->iokit != NULL ? (IOService *) d->iokit : IOService::getPlatform();
	IOMedia * part = NULL;
	AppleAPFSContainerScheme * scheme = NULL;
	AppleAPFSMedia * media = NULL;
	AppleAPFSContainer * cont = NULL;
	char name[64];
	uint32_t nvol = 0;

	(void) cmajor;
	if (above == NULL) {
		return;
	}
	if (c->alone && d != NULL && d->iokit != NULL) {
		/* the disk is the container's space, as a whole disk with an APFS container and no partition map is */
		part = (IOMedia *) d->iokit;
		part->retain();
		part->setProperty("Content Hint", c->content_hint);
		part->setProperty("Content", c->content_hint);
	} else {
		/* one of several containers back to back: its space, a node of its own (no BSD node: no device is it) */
		part = new IOMedia;
		if (part == NULL || !part->init()) {
			goto fail;
		}
		snprintf(name, sizeof(name), "%s container %u", c->under_name, c->index);
		part->setName(name);
		mediaProperties(part, NULL, 0, 0, 0, c->size, c->block_size, false, false, false, c->content_hint);
		part->setProperty("Content Hint", c->content_hint);
		setNumber(part, "Base", c->base, 64);
		setNumber(part, "Partition ID", c->index + 1, 32);
		if (!publish(part, above)) {
			goto fail;
		}
	}

	scheme = new AppleAPFSContainerScheme;
	if (scheme == NULL || !scheme->init() || !publish(scheme, part)) {
		goto fail;
	}

	media = new AppleAPFSMedia;
	if (media == NULL || !media->init()) {
		goto fail;
	}
	snprintf(name, sizeof(name), "disk%u", c->unit);
	media->setName("AppleAPFSMedia");
	mediaProperties(media, name, bmajor, c->unit << 8, c->unit, c->size, c->nx_block_size, true, false, false,
	    CONTENT_APFS_CONTAINER);
	if (!publish(media, scheme)) {
		goto fail;
	}

	cont = new AppleAPFSContainer;
	if (cont == NULL || !cont->init()) {
		goto fail;
	}
	cont->fContainer = c;
	setUUID(cont, "UUID", c->uuid);
	setNumber(cont, "ContainerBlockSize", c->nx_block_size, 32);
	if (!publish(cont, media)) {
		goto fail;
	}

	for (uint32_t i = 0; i < c->nvols; i++) {
		const struct apfs_media_vol * v = &c->vols[i];
		const char * rn = apfs_media_role_name(v->role);
		AppleAPFSVolume * vol = new AppleAPFSVolume;
		char loc[12];

		if (vol == NULL || !vol->init()) {
			OSSafeReleaseNULL(vol);
			continue;
		}
		snprintf(name, sizeof(name), "disk%us%u", c->unit, v->index + 1);
		snprintf(loc, sizeof(loc), "%u", v->index + 1);
		vol->setName(v->name[0] != '\0' ? v->name : name);
		vol->setLocation(loc);
		mediaProperties(vol, name, bmajor, (c->unit << 8) | (v->index + 1), c->unit, c->size, c->nx_block_size,
		    false, true, false, CONTENT_APFS_VOLUME);
		setNumber(vol, "RoleValue", v->role, 16);
		if (rn != NULL) {
			OSString * s = OSString::withCString(rn);
			OSArray * a = s != NULL ? OSArray::withObjects((const OSObject **) &s, 1, 1) : NULL;

			if (a != NULL) {
				vol->setProperty("Role", a);
			}
			OSSafeReleaseNULL(a);
			OSSafeReleaseNULL(s);
		}
		setUUID(vol, "UUID", v->uuid);
		setUUID(vol, "VolGroupUUID", v->group);
		if (publish(vol, cont)) {
			nvol++;
		}
		vol->release();
	}
	IOLog("HakiosAPFSMedia: %s container %u: AppleAPFSMedia disk%u, AppleAPFSContainer, %u AppleAPFSVolume (%s)\n",
	    c->under_name, c->index, c->unit, nvol, c->content_hint);
	part->release();
	scheme->release();
	media->release();
	cont->release();
	return;

fail:
	IOLog("HakiosAPFSMedia: %s container %u: the registry's nodes could not be made\n", c->under_name, c->index);
	OSSafeReleaseNULL(part);
	OSSafeReleaseNULL(scheme);
	OSSafeReleaseNULL(media);
	OSSafeReleaseNULL(cont);
}
