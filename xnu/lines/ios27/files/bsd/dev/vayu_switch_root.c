/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * sysctl kern.hakios_switch_root="INCOMING OUTGOING": the volume mounted on INCOMING becomes the root, and what was
 * the root until now is mounted on OUTGOING inside it (a path without a leading slash, a directory there). devfs stays
 * on /dev (vfs_switch_root() keeps it there). The two-step root of the NBD disk: the kernel starts from a small RAM
 * root, its process 1 mounts the volume of /dev/nbd0 (HFS Plus/HFSX through Scion's sweet_hfs; APFS once this tree
 * has it), puts in place what the system needs and asks for this. Process 1 only, root only, once, and only with
 * vayu27=1; reading it says whether it was done ("1") or not ("0").
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/hakios.apfs_check.c (hakios_sysctl_switch_root, the 8796 line's; the rest
 * of that file, the APFS checker, is not ported). vfs_switch_root() of xnu-12377 (bsd/vfs/vfs_subr.c) is 8796's with
 * VMOUNTEDHERE for VMOUNT: the same three arguments and the same semantics.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/vayu_12377_abi.h>
#include <pexpert/pexpert.h>

static int vayu_root_switched;

/*
 * vayu_hide_ramdisk=1 (default 0): once the root is switched, kern.bootargs (bsd/kern/kern_sysctl.c) shows the boot line
 * without its rd= words. iOS 27's launchd takes the system for a restore environment -- "Restore environment
 * starting.", none of the boot tasks run -- when kern.bootargs has "rd=md0" in it, or when SystemVersion.plist's
 * ReleaseType says Restore, Ramdisk or RamDisk (/sbin/launchd 24A446 at 0x100049fa0: sysctlbyname("kern.bootargs"),
 * strstr "rd=md0", else the ReleaseType read by its SystemVersion reader, strstr "Restore", "Ramdisk", "RamDisk"; it
 * reads nothing of /chosen or kern.ramdisk). After the switch the RAM disk is not the root any more: the line as
 * launchd sees it says so. The kernel's own line (PE_parse_boot_argn) is not changed.
 */
static char vayu_bootargs_after[PE_BOOT_ARGS_LENGTH];
static int vayu_bootargs_hidden;

const char *vayu_bootargs_shown(void);
const char *
vayu_bootargs_shown(void)
{
	return __atomic_load_n(&vayu_bootargs_hidden, __ATOMIC_ACQUIRE) ? vayu_bootargs_after : PE_boot_args();
}

static void
vayu_hide_ramdisk(void)
{
	const char *from = PE_boot_args();
	size_t at = 0;
	uint32_t on = 0;
	unsigned dropped = 0;

	if (!PE_parse_boot_argn("vayu_hide_ramdisk", &on, sizeof(on)) || on == 0) {
		return;
	}
	while (*from != '\0') {
		const char *end;
		size_t n;

		while (*from == ' ' || *from == '\t') {
			from++;
		}
		for (end = from; *end != '\0' && *end != ' ' && *end != '\t'; end++) {
		}
		n = (size_t)(end - from);
		if (n == 0) {
			break;
		}
		if (n >= 3 && strncmp(from, "rd=", 3) == 0) {
			dropped++;
		} else if (at + n + 2 <= sizeof(vayu_bootargs_after)) {
			if (at != 0) {
				vayu_bootargs_after[at++] = ' ';
			}
			memcpy(vayu_bootargs_after + at, from, n);
			at += n;
		}
		from = end;
	}
	vayu_bootargs_after[at] = '\0';
	__atomic_store_n(&vayu_bootargs_hidden, 1, __ATOMIC_RELEASE);
	printf("hakios: vayu_hide_ramdisk=1: kern.bootargs without its %u rd= word(s) from now on\n", dropped);
}

static int
vayu_sysctl_switch_root SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char buf[512], *outgoing;
	int error;

	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, vayu_root_switched ? "1" : "0", 2);
	}
	if (!vayu27_enabled()) {
		return ENOTSUP;
	}
	if (proc_getpid(current_proc()) != 1 || suser(kauth_cred_get(), NULL) != 0) {
		return EPERM;
	}
	if (vayu_root_switched) {
		return EBUSY;
	}
	if (req->newlen == 0 || req->newlen >= sizeof(buf)) {
		return EINVAL;
	}
	error = SYSCTL_IN(req, buf, req->newlen);
	if (error) {
		return error;
	}
	buf[req->newlen] = '\0';
	outgoing = strchr(buf, ' ');
	if (outgoing == NULL || buf[0] != '/' || outgoing[1] == '\0' || outgoing[1] == '/') {
		return EINVAL;
	}
	*outgoing++ = '\0';
	error = vfs_switch_root(buf, outgoing, 0);
	printf("hakios: %s is the root now, the one before it on /%s: %d\n", buf, outgoing, error);
	if (error == 0) {
		vayu_root_switched = 1;
		vayu_hide_ramdisk();
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, hakios_switch_root, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, vayu_sysctl_switch_root, "A",
    "write \"INCOMING OUTGOING\": the volume on INCOMING becomes the root (process 1, once, vayu27=1)");
