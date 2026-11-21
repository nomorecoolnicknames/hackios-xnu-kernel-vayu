/*
 * HakiOS: what HFS's kext had in its C++ part (hfs_iokit.cpp), for HFS built into the kernel.
 *
 * The kext registered the filesystem from its IOService's start(); built in, it is an entry of the static table
 * (bsd/vfs/vfs_conf.c) and hfs_init() does the rest.  The IOKit questions -- is the medium ejectable, which media
 * carry a given content UUID, the platform's serial number -- have no use for a data volume on the phone's own
 * flash, and the keys of content protection are AppleKeyStore's, which this kernel does not have: a volume made
 * without content protection never asks for them.
 *
 * From other HakiOS ports (APSL 2.0, as this file): oosexgang/hakios-xnu-kernel, branch main, commits 83aceb83817c
 * and 7ef54e774d32 (HFS Plus built in; also in Scion-Project/scion-kernel, branch main), as telegdyd/ios-vayu, branch vayu-dev,
 * commits 74cfdfa7d258 and ec38881a572c (the ejectable data volume) carry them.
 */
#include <sys/types.h>
#include <sys/errno.h>
#include <stdbool.h>
#include <mach/kern_return.h>
#include "hfs.h"
#include "hfs_iokit.h"
/*
 * var_hfs_trace=1 (hakios_hfs_trace, hakios_hfs_note): bsd/dev/hakios.ufs.c, beside kern.hakios's other names -- a
 * sysctl declared here would be HFS's own, which this kernel registers by hand.
 */

#ifndef panic_on_assert
bool panic_on_assert = false;
#endif

/*
 * vayu: the data volume on darwin's slice (/dev/ufs_var) is "ejectable" for HFS's sake -- that is what turns on
 * its own syncer (hfs_mountfs: HFS_RUN_SYNCER), which commits the journal 0.1-5 s after a change. This kernel has no
 * periodic sync of its own (sync_thread runs on power events only), so without it a change waited in memory until
 * something else flushed it, and a hard reset lost it. So is QEMU's disk (/dev/rvblk0, which writes only with
 * virtio_blk=2), on which the same volume is tried first (qemu/run-hfs-test.sh). Any other device is not.
 */
int
hfs_is_ejectable(const char *cdev_name)
{
	return cdev_name != NULL && (strcmp(cdev_name, "/dev/ufs_var") == 0 || strcmp(cdev_name, "/dev/rvblk0") == 0);
}

void
hfs_iterate_media_with_content(__unused const char *content_uuid_cstring,
    __unused int (*func)(const char *bsd_name, const char *uuid_str, void *arg), __unused void *arg)
{
}

kern_return_t
hfs_get_platform_serial_number(__unused char *serial_number_str, __unused uint32_t len)
{
	return KERN_FAILURE;
}

#if CONFIG_PROTECT
int
hfs_unwrap_key(__unused aks_cred_t access, __unused const aks_wrapped_key_t wrapped_key_in,
    __unused aks_raw_key_t key_out)
{
	return ENOTSUP;
}

int
hfs_rewrap_key(__unused aks_cred_t access, __unused cp_key_class_t dp_class,
    __unused const aks_wrapped_key_t wrapped_key_in, __unused aks_wrapped_key_t wrapped_key_out)
{
	return ENOTSUP;
}

int
hfs_new_key(__unused aks_cred_t access, __unused cp_key_class_t dp_class, __unused aks_raw_key_t key_out,
    __unused aks_wrapped_key_t wrapped_key_out)
{
	return ENOTSUP;
}

int
hfs_backup_key(__unused aks_cred_t access, __unused const aks_wrapped_key_t wrapped_key_in,
    __unused aks_wrapped_key_t wrapped_key_out)
{
	return ENOTSUP;
}
#endif /* CONFIG_PROTECT */
