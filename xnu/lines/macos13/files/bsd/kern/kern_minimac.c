/*
 * Stand-ins for the two closed security policies user space cannot do
 * without: AMFI and the Sandbox. On a device they are kexts that register
 * MAC policies of those names; system software talks to them through
 * __mac_syscall() and relies on what they decide.
 *
 * Neither stand-in is a security mechanism. This kernel runs whatever it is
 * given (see libkern/amfi/miniamfi.c); these only give the answers the iOS
 * user space needs in order to run at all:
 *
 *   AMFI     The binaries of the operating system carry ad hoc signatures
 *            (no certificate chain); on a device AMFI finds their hashes
 *            in the trust cache and flags them as platform binaries.
 *            launchd panics if a system daemon is not one. There is no
 *            trust cache here, so every ad hoc signed binary is taken for
 *            a platform binary. Unsigned programs (our own) are not.
 *
 *   Sandbox  Every query (may this process look up that service?) is
 *            answered "allowed", and every request to apply a profile
 *            succeeds without applying anything. The one question with
 *            an answer of its own is which container a process is in.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/codesign.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/kpi_socket.h>
#include <sys/priv.h>
#include <sys/proc_internal.h>
#include <sys/ubc_internal.h>
#include <sys/vnode_internal.h>
#include <libkern/libkern.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <security/mac_framework.h>
#include <security/mac_policy.h>
#include <pexpert/pexpert.h>

void minimac_policy_init(void);

static int
miniamfi_vnode_check_signature(__unused struct vnode *vp, __unused struct label *label, __unused cpu_type_t cpu_type,
    struct cs_blob *cs_blob, unsigned int *cs_flags, __unused unsigned int *signer_type, __unused int flags,
    __unused unsigned int platform, __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	if (cs_blob == NULL) {
		return 0;
	}
	/*
	 * Accepting a signature is what marks the process as signed; the
	 * kernel consults the signature of a process only when that is set.
	 */
	*cs_flags |= CS_SIGNED;
	if (*cs_flags & CS_ADHOC) {
		*cs_flags |= CS_PLATFORM_BINARY;
	}
	return 0;
}

/*
 * dyld asks AMFI what it may do for the process it is starting (request 0x5a, "check dyld
 * policy"): the argument is { flags in, pointer to flags out }. Everything is allowed --
 * @paths, the DYLD_* variables, inserted libraries and interposing -- which is what lets
 * our own library be loaded into stock daemons (DYLD_INSERT_LIBRARIES).
 */
#define AMFI_SYSCALL_CHECK_DYLD_POLICY  0x5a
#define AMFI_DYLD_OUTPUT_ALLOW_ALL      0xffULL
/* what the lab's macOS kernel answered macOS 13.4's dyld (macos_abi=1): ten bits, not iOS 16.5's eight */
#define AMFI_DYLD_OUTPUT_ALLOW_ALL_MACOS 0x3ffULL
extern uint32_t hakios_macos_abi;          /* bsd/kern/kern_exec.c: boot-arg macos_abi=1 */

/*
 * Request 0x60, "query boot-arg state" (amfi_interface_query_bootarg_state() in libamfi): the
 * argument points to 64 bits of flags, one for each boot-arg that switches a part of AMFI off
 * (amfi_get_out_of_my_way and its kin). ExtensionKit asks, and unless one of the bits 0x86 is set
 * it wants the signature of the system's table of extension points
 * (/System/Library/xpc/extensions.plist.sig) verified by AMFI's IOKit service. There is no such
 * service here ("Unable to find AMFI"); the table is then not read and LaunchServices knows no
 * extension point: every application extension is refused when the applications are registered
 * ("does not correspond to a known extension point"), pkd cannot make a record for any extension
 * point, and Shortcuts dies on a nil one. Nothing is enforced by this kernel, so that is what it
 * says. (Which of the three bits stands for which boot-arg is not known; ExtensionKit takes any
 * of them.)
 */
#define AMFI_SYSCALL_QUERY_BOOTARG_STATE 0x60
#define AMFI_BOOTARG_STATE_NOT_ENFORCING 0x86ULL

static int
miniamfi_policy_syscall(__unused struct proc *p, int call, user_addr_t arg)
{
	struct {
		uint64_t input_flags;
		user_addr_t output_flags;
	} args;
	uint64_t flags = hakios_macos_abi != 0 ? AMFI_DYLD_OUTPUT_ALLOW_ALL_MACOS : AMFI_DYLD_OUTPUT_ALLOW_ALL;
	uint64_t state = AMFI_BOOTARG_STATE_NOT_ENFORCING;
	int error;

	if (call == AMFI_SYSCALL_QUERY_BOOTARG_STATE) {
		return copyout(&state, arg, sizeof(state));
	}
	if (call != AMFI_SYSCALL_CHECK_DYLD_POLICY) {
		return ENOPOLICY;               /* what user space got before there was a handler */
	}
	error = copyin(arg, &args, sizeof(args));
	if (error == 0) {
		error = copyout(&flags, args.output_flags, sizeof(flags));
	}
	return error;
}

static const struct mac_policy_ops miniamfi_ops = {
	.mpo_vnode_check_signature = miniamfi_vnode_check_signature,
	.mpo_policy_syscall = miniamfi_policy_syscall,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) miniamfi_conf = {
	.mpc_name = "AMFI",
	.mpc_fullname = "Apple Mobile File Integrity (stand-in)",
	.mpc_ops = &miniamfi_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) miniamfi_handle;

/*
 * Request 4, "container path of a process": sandbox_container_path_for_pid() and
 * sandbox_container_path_for_audit_token() of libsystem_sandbox. The argument is
 * { pid, version of the pid, buffer, its size, whether the version is to be compared }.
 *
 * On a device the Sandbox learns the container when the program is executed (it asks
 * containermanagerd) and confines the process to it. Nothing is confined here, but the question
 * is still asked and the answer matters: an application extension hosted by PlugInKit does not
 * run without a container. It gives the buffer a "?" to start with and wants a longer string
 * back, else "plugin has invalid sandbox or container" (-[PKService checkEnvironment:]) -- the
 * end of every Intents extension while this request "succeeded" without writing anything.
 *
 * Whoever starts a program that lives in a container -- RunningBoard an application, pkd an
 * extension -- has had the path from containermanagerd and passes it in the environment as
 * CFFIXED_USER_HOME, which is where Foundation takes the home directory from. So that is the
 * answer: it is looked up among the strings exec left at the top of the stack of the process.
 * A process without the variable has no container, and gets what every process got before:
 * success and an untouched buffer.
 */
#define SANDBOX_SYSCALL_CONTAINER_PATH  4
#define SANDBOX_CONTAINER_VARIABLE      "CFFIXED_USER_HOME="

static int
minisandbox_container_path(user_addr_t arg)
{
	struct {
		int64_t     pid;
		uint64_t    pidversion;
		user_addr_t buffer;
		user_size_t size;
		uint64_t    check_pidversion;
	} args;
	const size_t prefix_len = sizeof(SANDBOX_CONTAINER_VARIABLE) - 1;
	proc_t p;
	task_t task;
	vm_map_t map;
	vm_map_copy_t copy = NULL;
	vm_offset_t strings = 0;
	vm_size_t strings_size;
	user_addr_t user_stack;
	size_t argslen, len;
	const char *s, *end;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	p = proc_find((int)args.pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}
	if (args.check_pidversion && (uint64_t)proc_pidversion(p) != args.pidversion) {
		proc_rele(p);
		return ESRCH;
	}
	user_stack = p->user_stack;
	argslen = p->p_argslen;
	task = proc_task(p);
	if (user_stack == 0 || argslen == 0 || task == TASK_NULL) {
		proc_rele(p);
		return 0;
	}
	/* the same way as sysctl_procargsx(): the task keeps the map alive once the process is let go */
	task_reference(task);
	proc_rele(p);
	map = get_task_map_reference(task);
	task_deallocate(task);
	if (map == VM_MAP_NULL) {
		return 0;
	}
	strings_size = vm_map_round_page(argslen, vm_map_page_mask(map));
	if (kmem_alloc(kernel_map, &strings, strings_size, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		vm_map_deallocate(map);
		return ENOMEM;
	}
	if (vm_map_copyin(map, (vm_map_address_t)(user_stack - strings_size), strings_size, FALSE, &copy) != KERN_SUCCESS) {
		copy = NULL;
	}
	vm_map_deallocate(map);
	if (copy == NULL || vm_map_copy_overwrite(kernel_map, strings, copy, strings_size, FALSE) != KERN_SUCCESS) {
		if (copy != NULL) {
			vm_map_copy_discard(copy);
		}
		kmem_free(kernel_map, strings, strings_size);
		return 0;                       /* the process is on its way out, or has unmapped its own stack */
	}

	end = (const char *)(strings + strings_size);
	/*
	 * (macos_abi=1) A process without a container is told so (ENOENT): libxpc's _xpc_runtime_is_app_sandboxed()
	 * takes success and a buffer that does not start with a NUL -- the buffer it filled with 0xaa itself -- for an
	 * app sandbox, and Security then asked XPCKeychainSandboxCheck for the keychain paths of every daemon
	 * (authorizationhost's builtin:auto-login waited on it for good, QEMU AQUA r13).
	 */
	error = hakios_macos_abi != 0 ? ENOENT : 0;
	for (s = end - argslen; s < end; s += len + 1) {
		len = strnlen(s, end - s);
		if (len > prefix_len && s + len < end && strncmp(s, SANDBOX_CONTAINER_VARIABLE, prefix_len) == 0) {
			s += prefix_len;
			len -= prefix_len;
			error = len < args.size ? copyout(s, args.buffer, len + 1) : ENAMETOOLONG;
			break;
		}
	}
	kmem_free(kernel_map, strings, strings_size);
	return error;
}

/*
 * Request 5, "issue an extension" (boot-arg macos_abi=1 only): sandbox_extension_issue_file() and its kin of
 * libsystem_sandbox, which hand the kernel { class, type, path or name, flags, a 2 KB buffer, ... } and give the
 * buffer back as the token. The stand-in confines nothing and consumes every token, so any well-formed one does; it
 * left the buffer as malloc() gave it, and the "token" was whatever bytes were there, with no end (XPCKeychainSandboxCheck
 * sends its tokens back to the daemon that asked). The token is Apple's shape -- fields separated by ';', the class
 * and the path last -- with no hash nor sentinel worth checking.
 */
#define SANDBOX_SYSCALL_EXTENSION_ISSUE 5
#define SANDBOX_EXTENSION_TOKEN_SIZE    0x800

static int
minisandbox_extension_issue(user_addr_t arg)
{
	struct {
		user_addr_t ext_class;
		uint64_t    type;
		user_addr_t subject;
		uint64_t    flags;
		user_addr_t token;
		uint64_t    pid;
		uint64_t    pidversion;
	} args;
	char ext_class[128], *subject, *token;
	size_t done;
	int error, n;

	if ((error = copyin(arg, &args, sizeof(args))) != 0) {
		return error;
	}
	if (args.token == 0 || args.ext_class == 0) {
		return EINVAL;
	}
	if ((error = copyinstr(args.ext_class, ext_class, sizeof(ext_class), &done)) != 0) {
		return error;
	}
	subject = kalloc_data(MAXPATHLEN, Z_WAITOK | Z_ZERO);
	token = kalloc_data(SANDBOX_EXTENSION_TOKEN_SIZE, Z_WAITOK | Z_ZERO);
	if (subject == NULL || token == NULL) {
		error = ENOMEM;
		goto out;
	}
	if (args.subject != 0 && (error = copyinstr(args.subject, subject, MAXPATHLEN, &done)) != 0) {
		goto out;
	}
	n = snprintf(token, SANDBOX_EXTENSION_TOKEN_SIZE, "0000000000000000000000000000000000000000;00;00000000;00000000;"
	    "00000000;%016llx;%s;01;%08llx;0000000000000000;%s", (unsigned long long)strlen(subject), ext_class,
	    (unsigned long long)args.type, subject);
	error = n >= SANDBOX_EXTENSION_TOKEN_SIZE ? ENAMETOOLONG : copyout(token, args.token, (size_t)n + 1);
out:
	if (subject != NULL) {
		kfree_data(subject, MAXPATHLEN);
	}
	if (token != NULL) {
		kfree_data(token, SANDBOX_EXTENSION_TOKEN_SIZE);
	}
	return error;
}

/* Which request numbers have been seen: one log line each, to learn what user space asks for. */
static uint64_t minisandbox_seen;

static int
minisandbox_policy_syscall(struct proc *p, int call, user_addr_t arg)
{
	if (call >= 0 && call < 64 && (minisandbox_seen & (1ULL << call)) == 0) {
		minisandbox_seen |= 1ULL << call;
		printf("minisandbox: request %d (first seen from %s[%d]): %s\n", call, p->p_comm, proc_getpid(p),
		    call == SANDBOX_SYSCALL_CONTAINER_PATH ? "answered" : "allowed");
	}
	if (call == SANDBOX_SYSCALL_CONTAINER_PATH) {
		return minisandbox_container_path(arg);
	}
	if (call == SANDBOX_SYSCALL_EXTENSION_ISSUE && hakios_macos_abi != 0) {
		return minisandbox_extension_issue(arg);
	}
	return 0;
}

/*
 * security.mac.sandbox.sentinel (boot-arg macos_abi=1 only): the Sandbox kext's string that names the temporary
 * files the sandbox lets a process create beside a file it may write. libsystem_sandbox's _amkrtemp() -- the
 * mkstemp() of Security's AtomicFile, CoreFoundation's and Foundation's atomic writes -- reads it and makes
 * "<path><sentinel>-XXXXXX"; without the sysctl it makes no file at all. securityd's MDS install wrote its databases
 * that way and failed (CSSMERR_DL_MDS_ERROR), securityd aborted, and trustd, LaunchServices and loginwindow waited
 * on it (QEMU AQUA r5, r7). The stand-in confines nothing, so the name only has to be stable for a boot: ".sb-"
 * and eight hex digits, as a Mac's are. Without macos_abi: ENOENT, as before.
 */
#include <sys/sysctl.h>
#include <sys/random.h>

static int
minisandbox_sysctl_sentinel SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	static char sentinel[16];

	if (hakios_macos_abi == 0) {
		return ENOENT;
	}
	if (sentinel[0] == 0) {
		uint32_t r;

		read_random(&r, sizeof(r));
		snprintf(sentinel, sizeof(sentinel), ".sb-%08x", r);
	}
	return SYSCTL_OUT(req, sentinel, strlen(sentinel) + 1);
}

SYSCTL_DECL(_security_mac);
SYSCTL_NODE(_security_mac, OID_AUTO, sandbox, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "Sandbox (stand-in)");
SYSCTL_PROC(_security_mac_sandbox, OID_AUTO, sentinel, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    minisandbox_sysctl_sentinel, "A", "the name the sandbox gives to its temporary files");

/*
 * Privileges for processes that are not root. On a device the sandbox hands them out by
 * entitlement: a privilege is granted to a process whose code signature has the entitlement
 * that goes with it. The table is the same pairing: the names are the entitlements iOS's own
 * programs carry for them (dasd "vm-pressure-level", symptomsd and aggregated
 * com.apple.private.network.statistics, mediaserverd and mDNSResponder
 * com.apple.private.network.socket-delegate -- without which NECP said "does not hold the
 * necessary entitlement to delegate network traffic" -- and so on).
 *
 * That NECP line is worth knowing for what it is NOT: it costs nothing. The macro that prints it
 * (bsd/net/necp.c, NECP_VERIFY_DELEGATION_ENTITLEMENT) only breaks out of the parameter it was
 * reading, so the delegation is dropped and the policy is matched against the caller itself; no
 * error is returned and no connection is refused. A session where trustd prints it four hundred
 * times is a session where trustd's own connections all go through. A stock iPhone prints it too.
 * It is noise, not a wall -- do not go looking for a daemon's missing network behind it.
 *
 *   PRIV_WORK_INTERVAL    is granted to every process, entitlement or not: telling the scheduler
 *                         about the work of drawing a frame. UIKit does it as soon as anything
 *                         scrolls, and takes a refusal for a programming error ("Failed to create
 *                         work interval"): SpringBoard died of it on the first swipe between
 *                         pages of the home screen.
 *
 * Whatever is refused is said once, to learn what there is. Boot-arg privs=0: only
 * PRIV_WORK_INTERVAL, as before.
 */
extern kern_return_t miniamfi_entitlement_boolean(struct proc *proc, const char *name);

static const struct {
	int priv;
	const char *entitlement;
} minisandbox_privileges[] = {
	{ PRIV_PROC_UUID_POLICY, "com.apple.private.kernel.uuid-policy" },
	{ PRIV_GLOBAL_PROC_INFO, "com.apple.private.kernel.global-proc-info" },
	{ PRIV_SYSTEM_OVERRIDE, "com.apple.private.kernel.system-override" },
	{ PRIV_HW_DEBUG_DATA, "com.apple.private.kernel.hw-debug-data" },
	{ PRIV_SELECTIVE_FORCED_IDLE, "com.apple.private.kernel.selective-forced-idle" },
	{ PRIV_DARKBOOT, "com.apple.private.kernel.darkboot" },
	{ PRIV_AUDIO_LATENCY, "com.apple.private.kernel.audio_latency" },
	{ PRIV_KTRACE_BACKGROUND, "com.apple.private.kernel.ktrace-background" },
	{ PRIV_SETPRIORITY_DARWIN_ROLE, "com.apple.private.kernel.setpriority-darwin-role" },
	{ PRIV_PACKAGE_EXTENSIONS, "com.apple.private.kernel.set-package-extensions" },
	{ PRIV_TRIM_ACTIVE_FILE, "com.apple.private.apfs.trim-active-file" },
	{ PRIV_PROC_CPUMON_OVERRIDE, "com.apple.private.kernel.override-cpumon" },
	{ PRIV_VM_PRESSURE, "vm-pressure-level" },
	{ PRIV_VM_JETSAM, "com.apple.private.kernel.jetsam" },
	{ PRIV_NET_PRIVILEGED_TRAFFIC_CLASS, "com.apple.private.network.privileged-traffic-class" },
	{ PRIV_NET_PRIVILEGED_SOCKET_DELEGATE, "com.apple.private.network.socket-delegate" },
	{ PRIV_NET_INTERFACE_CONTROL, "com.apple.private.network.interface-control" },
	{ PRIV_NET_PRIVILEGED_NETWORK_STATISTICS, "com.apple.private.network.statistics" },
	{ PRIV_NET_PRIVILEGED_NECP_POLICIES, "com.apple.private.necp.policies" },
	{ PRIV_NET_RESTRICTED_AWDL, "com.apple.private.network.awdl.restricted" },
	{ PRIV_NET_QOSMARKING_POLICY_OVERRIDE, "com.apple.private.network.qos-marking-policy-override" },
	{ PRIV_NET_RESTRICTED_MULTIPATH_EXTENDED, "com.apple.developer.networking.multipath_extended" },
	{ PRIV_NET_RESTRICTED_ROUTE_NC_READ, "com.apple.developer.networking.route_nc_read" },
	{ PRIV_NET_VALIDATED_RESOLVER, "com.apple.private.validated-resolver" },
	{ PRIV_NET_CUSTOM_PROTOCOL, "com.apple.developer.networking.custom-protocol" },
	{ PRIV_NET_PRIVILEGED_NECP_DROP_ALL_BYPASS, "com.apple.private.necp.drop_all_bypass" },
	{ PRIV_NET_PRIVILEGED_IPSEC_WAKE_PACKET, "com.apple.private.network.ipsec-wake-packet" },
	{ PRIV_NETINET_RESERVEDPORT, "com.apple.private.network.reserved-port" },
	{ PRIV_NETINET_TCP_KA_OFFLOAD, "com.apple.developer.networking.tcp_ka_offload" },
	{ PRIV_SKYWALK_REGISTER_USER_PIPE, "com.apple.private.skywalk.register-user-pipe" },
	{ PRIV_SKYWALK_REGISTER_KERNEL_PIPE, "com.apple.private.skywalk.register-kernel-pipe" },
	{ PRIV_SKYWALK_REGISTER_NET_IF, "com.apple.private.skywalk.register-net-if" },
	{ PRIV_SKYWALK_REGISTER_FLOW_SWITCH, "com.apple.private.skywalk.register-flow-switch" },
	{ PRIV_SKYWALK_OBSERVE_ALL, "com.apple.private.skywalk.observe-all" },
	{ PRIV_SKYWALK_OBSERVE_STATS, "com.apple.private.skywalk.observe-stats" },
	{ PRIV_VFS_OPEN_BY_ID, "com.apple.private.vfs.open-by-id" },
	{ PRIV_VFS_MOVE_DATA_EXTENTS, "com.apple.private.vfs.move-data-extents" },
	{ PRIV_VFS_SNAPSHOT, "com.apple.private.vfs.snapshot" },
	{ PRIV_VFS_DATALESS_RESOLVER, "com.apple.private.vfs.dataless-resolver" },
	{ PRIV_VFS_SETSIZE, "com.apple.private.vfs.setsize" },
};
static TUNABLE(uint32_t, minisandbox_by_entitlement, "privs", 1);

static int
minisandbox_priv_grant(kauth_cred_t cred, int priv)
{
	static int seen[32];
	static unsigned seen_count;
	proc_t proc = current_proc();

	if (priv == PRIV_WORK_INTERVAL) {
		return 0;
	}
	/* (the entitlements are the current process's: only for a question about its own credential) */
	for (unsigned i = 0; minisandbox_by_entitlement && cred == kauth_cred_get() &&
	    i < sizeof(minisandbox_privileges) / sizeof(minisandbox_privileges[0]); i++) {
		if (minisandbox_privileges[i].priv == priv) {
			if (miniamfi_entitlement_boolean(proc, minisandbox_privileges[i].entitlement) == KERN_SUCCESS) {
				return 0;
			}
			break;
		}
	}
	for (unsigned i = 0; i < seen_count; i++) {
		if (seen[i] == priv) {
			return EPERM;
		}
	}
	if (seen_count < sizeof(seen) / sizeof(seen[0])) {
		seen[seen_count++] = priv;
		printf("minisandbox: privilege %d refused to %s[%d] (first time)\n", priv, proc->p_comm,
		    proc_getpid(proc));
	}
	return EPERM;
}

static const struct mac_policy_ops minisandbox_ops = {
	.mpo_policy_syscall = minisandbox_policy_syscall,
	.mpo_priv_grant = minisandbox_priv_grant,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) minisandbox_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "Seatbelt sandbox policy (stand-in)",
	.mpc_ops = &minisandbox_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) minisandbox_handle;

/*
 * Quarantine (boot-arg macos_abi=1 only): on a Mac, Quarantine.kext is not only the file quarantine but also where
 * "responsibility" is kept -- which process answers for another one (TCC asks it of a process that wants a
 * permission, RunningBoard and the crash reports name it). The field is the kernel's own (p_responsible_pid: fork
 * passes it on, NECP and the crash info read it); the kext sets and answers it for libquarantine. Without a policy of
 * that name every such call is ENOPOLICY, and xpcproxy, which tells it before it execs an XPC service who is
 * responsible for the service (responsibility_init), asserts on any answer but 0 or ESRCH: every XPC service of
 * macOS ended "could not exec: assertion failed: 22F66: xpcproxy + 21164: 0x67" (MTLCompilerService,
 * QuickLookUIService, ...).
 *
 * The requests, as 22F66's libquarantine.dylib makes them (arguments: an array of 64-bit words):
 *   0xb4  responsible for pid       { pid, &pid out, &uniqueid out, &path length, path }   (no path is kept: 0)
 *   0xb5  set pid responsible for   { pid (0: the caller), the responsible pid }          (responsibility_init)
 *   0xb7  audit token's process responsible for itself   { pid, pid version }
 *   0xb8  audit token's process responsible for the caller   { pid, pid version }
 *   0xb9  the caller responsible for itself
 *   0xba  hosted path, 0xbb hosted team id (an XPC service that hosts code says whose): taken, not kept
 * A process nobody set is responsible for itself, as one that launchd starts is on a Mac. Setting it for another
 * process is root's.
 *   0xb6  the attribution of an audit token's process: who is responsible for it, and that process's identity, into
 *         the caller's buffers (22 words, below at miniqtn_attribution). responsibility_get_responsible_audit_
 *         token_for_audit_token and responsibility_get_attribution_for_audittoken make it; tccd makes it for every
 *         request ("ERROR: responsibility_get_responsible_audit_token_for_audit_token() ... (#103) Policy not found",
 *         "Failed to create attribution chain from message": phone run aq5, 2026-11-20) -- without it TCC cannot
 *         say whose a request is.
 *   0x54  the caller's own process quarantine (qtn_proc_init_with_self), 0x55 another process's
 *   0x57  quarantine a process (qtn_proc_apply_to_self: { pid, the qtn_proc's words... })
 *         (boot-arg qtn_proc=1 only, AQUA aq16) No process here is quarantined and none is made so: 0x54/0x55
 *         answer ENOATTR, what the kext says of a process without one (libquarantine: QTN_NOT_QUARANTINED), and 0x57
 *         is taken and not kept. iconservicesagent applies a quarantine to itself at its start and exit(1)s when it
 *         cannot ("Failed to enable quarantine."); the Dock waits on it with its main thread
 *         (+[ISIconManager sharedInstance], a synchronous XPC message that is never answered, the agent restarted
 *         without end), so it never gets to order its desktop picture in or draw its own window (QEMU r45, phone
 *         aq13: "exited due to exit(1)" for every iconservicesagent).
 * Everything else (the file quarantine 0x50-0x53, 0x58-0x59) is ENOPOLICY as before, said once each.
 */
#define QTN_RESPONSIBLE_GET             0xb4
#define QTN_RESPONSIBLE_SET_PID         0xb5
#define QTN_RESPONSIBLE_SET_TOKEN_SELF  0xb7
#define QTN_RESPONSIBLE_SET_TOKEN_CALLER 0xb8
#define QTN_RESPONSIBLE_SET_SELF        0xb9
#define QTN_RESPONSIBLE_HOSTED_PATH     0xba
#define QTN_RESPONSIBLE_HOSTED_TEAM     0xbb
#define QTN_ATTRIBUTION                 0xb6
#define QTN_PROC_GET_SELF               0x54
#define QTN_PROC_GET_PID                0x55
#define QTN_PROC_APPLY                  0x57

/*
 * 0xb6, as 22F66's libquarantine fills it (responsibility_get_attribution_for_audittoken). The audit-token variant
 * (responsibility_get_responsible_audit_token_for_audit_token: word 2 the token out, not 0) is another record:
 * { pid, pid version, &token, &a word out, a word } -- the word out is given the responsible process's unique ID
 * here (tccd passes no room for it and reads only the token). Each string comes back with its length counting the NUL (the
 * library packs them one after another into one allocation); a length word is in: the room, out: what was
 * needed; too little room for any of them is ERANGE with every length set (the library grows the entitlements'
 * buffer and asks again). Kept: no hosted path or team (0xba/0xbb are not kept), no entitlements, no user UUID.
 */
enum {
	QA_PID, QA_PIDVERSION, QA_TOKEN, QA_PATH_LEN, QA_PATH, QA_OFFSET, QA_TEAM_LEN, QA_TEAM, QA_SIGNING_LEN, QA_SIGNING,
	QA_USER_UUID, QA_UNKNOWN11, QA_ENT_LEN, QA_ENT, QA_HOSTED_PATH_LEN, QA_HOSTED_PATH, QA_PERSISTENT_ID, QA_PLATFORM,
	QA_SDK, QA_CSFLAGS, QA_HOSTED_TEAM_LEN, QA_HOSTED_TEAM, QA_WORDS
};

/* the process of a pid (and pid version, when one is given): a reference the caller drops */
static proc_t
miniqtn_find(int64_t pid, const uint64_t *pidversion)
{
	proc_t p;

	if (pid <= 0 || pid > PID_MAX) {
		return PROC_NULL;
	}
	p = proc_find((int)pid);
	if (p != PROC_NULL && pidversion != NULL && (uint64_t)(uint32_t)proc_pidversion(p) != (uint32_t)*pidversion) {
		proc_rele(p);
		p = PROC_NULL;
	}
	return p;
}

static int
miniqtn_set(proc_t caller, proc_t target, int64_t responsible)
{
	proc_t rp;

	if (target != caller && !kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	rp = miniqtn_find(responsible, NULL);
	if (rp == PROC_NULL) {
		return ESRCH;           /* xpcproxy takes this one: the client is gone already */
	}
	proc_rele(rp);
	proc_set_responsible_pid(target, (pid_t)responsible);
	return 0;
}

static int
miniqtn_string_out(const uint64_t *args, int len_at, int buf_at, const char *text, bool *short_of_room)
{
	uint64_t room = 0, need = text != NULL && text[0] != '\0' ? strlen(text) + 1 : 0;
	int error;

	if (args[len_at] == 0) {
		return 0;
	}
	if ((error = copyin((user_addr_t)args[len_at], &room, sizeof(room))) != 0) {
		return error;
	}
	if (need != 0 && args[buf_at] != 0 && room >= need) {
		if ((error = copyout(text, (user_addr_t)args[buf_at], (size_t)need)) != 0) {
			return error;
		}
	} else if (need > room) {
		*short_of_room = true;
	}
	return copyout(&need, (user_addr_t)args[len_at], sizeof(need));
}

static int
miniqtn_attribution(user_addr_t arg)
{
	uint64_t args[QA_WORDS] = { 0 }, zero64 = 0, csflags;
	uint32_t token[8], platform = 1 /* PLATFORM_MACOS */, zero32 = 0;
	uint8_t zeros[32] = { 0 };
	proc_t target, rp;
	pid_t rpid;
	kauth_cred_t cred;
	char *path = NULL;
	int len = MAXPATHLEN, error;
	bool short_of_room = false;
	vnode_t vp;

	if ((error = copyin(arg, args, sizeof(args))) != 0) {
		return error;
	}
	target = miniqtn_find((int64_t)args[QA_PID], &args[QA_PIDVERSION]);
	if (target == PROC_NULL) {
		return ESRCH;
	}
	rpid = target->p_responsible_pid > 0 ? target->p_responsible_pid : proc_getpid(target);
	rp = rpid == proc_getpid(target) ? target : miniqtn_find(rpid, NULL);
	if (rp == PROC_NULL) {
		rp = target;                    /* the responsible one is gone: the process answers for itself */
	}
	cred = kauth_cred_proc_ref(rp);
	token[0] = cred->cr_audit.as_aia_p->ai_auid;
	token[1] = kauth_cred_getuid(cred);
	token[2] = kauth_cred_getgid(cred);
	token[3] = kauth_cred_getruid(cred);
	token[4] = kauth_cred_getrgid(cred);
	token[5] = (uint32_t)proc_getpid(rp);
	token[6] = cred->cr_audit.as_aia_p->ai_asid;
	token[7] = (uint32_t)proc_pidversion(rp);
	kauth_cred_unref(&cred);
	csflags = (uint64_t)(uint32_t)proc_getcsflags(rp);
	path = kalloc_data(MAXPATHLEN, Z_WAITOK | Z_ZERO);
	if (path != NULL && (vp = proc_getexecutablevnode(rp)) != NULLVP) {
		if (vn_getpath(vp, path, &len) != 0) {
			path[0] = '\0';
		}
		vnode_put(vp);
	}
	error = 0;
	if (args[QA_TOKEN] != 0) {
		uint64_t uniqueid = proc_uniqueid(rp);

		error = copyout(token, (user_addr_t)args[QA_TOKEN], sizeof(token));
		if (error == 0 && args[QA_PATH_LEN] != 0) {
			error = copyout(&uniqueid, (user_addr_t)args[QA_PATH_LEN], sizeof(uniqueid));
		}
		goto done;
	}
	if (error == 0) {
		error = miniqtn_string_out(args, QA_PATH_LEN, QA_PATH, path, &short_of_room);
	}
	if (error == 0) {
		error = miniqtn_string_out(args, QA_TEAM_LEN, QA_TEAM, csproc_get_teamid(rp), &short_of_room);
	}
	if (error == 0) {
		error = miniqtn_string_out(args, QA_SIGNING_LEN, QA_SIGNING, cs_identity_get(rp), &short_of_room);
	}
	if (error == 0) {
		error = miniqtn_string_out(args, QA_ENT_LEN, QA_ENT, NULL, &short_of_room);
	}
	if (error == 0) {
		error = miniqtn_string_out(args, QA_HOSTED_PATH_LEN, QA_HOSTED_PATH, NULL, &short_of_room);
	}
	if (error == 0) {
		error = miniqtn_string_out(args, QA_HOSTED_TEAM_LEN, QA_HOSTED_TEAM, NULL, &short_of_room);
	}
	if (error == 0 && args[QA_OFFSET] != 0) {
		error = copyout(&zero64, (user_addr_t)args[QA_OFFSET], sizeof(zero64));
	}
	if (error == 0 && args[QA_UNKNOWN11] != 0) {
		error = copyout(&zero64, (user_addr_t)args[QA_UNKNOWN11], sizeof(zero64));
	}
	if (error == 0 && args[QA_USER_UUID] != 0) {
		error = copyout(zeros, (user_addr_t)args[QA_USER_UUID], 16);
	}
	if (error == 0 && args[QA_PERSISTENT_ID] != 0) {
		error = copyout(zeros, (user_addr_t)args[QA_PERSISTENT_ID], sizeof(zeros));
	}
	if (error == 0 && args[QA_PLATFORM] != 0) {
		error = copyout(&platform, (user_addr_t)args[QA_PLATFORM], sizeof(platform));
	}
	if (error == 0 && args[QA_SDK] != 0) {
		error = copyout(&zero32, (user_addr_t)args[QA_SDK], sizeof(zero32));
	}
	if (error == 0 && args[QA_CSFLAGS] != 0) {
		error = copyout(&csflags, (user_addr_t)args[QA_CSFLAGS], sizeof(csflags));
	}
done:
	if (path != NULL) {
		kfree_data(path, MAXPATHLEN);
	}
	if (rp != target) {
		proc_rele(rp);
	}
	proc_rele(target);
	return error != 0 ? error : short_of_room ? ERANGE : 0;
}

static int
miniqtn_policy_syscall(struct proc *p, int call, user_addr_t arg)
{
	static uint64_t seen;
	uint64_t args[5] = { 0 };
	size_t nargs;
	proc_t target, rp;
	int64_t rpid;
	uint64_t uniqueid = 0;
	int error;

	switch (call) {
	case QTN_RESPONSIBLE_GET:
		nargs = 5;
		break;
	case QTN_RESPONSIBLE_SET_PID:
	case QTN_RESPONSIBLE_SET_TOKEN_SELF:
	case QTN_RESPONSIBLE_SET_TOKEN_CALLER:
		nargs = 2;
		break;
	case QTN_RESPONSIBLE_SET_SELF:
		nargs = 0;
		break;
	case QTN_RESPONSIBLE_HOSTED_PATH:
	case QTN_RESPONSIBLE_HOSTED_TEAM:
		return 0;
	case QTN_ATTRIBUTION:
		return miniqtn_attribution(arg);
	case QTN_PROC_GET_SELF:
	case QTN_PROC_GET_PID:
	case QTN_PROC_APPLY: {
		static int qtn_proc = -1;

		if (qtn_proc < 0) {
			int on = 0;

			qtn_proc = PE_parse_boot_argn("qtn_proc", &on, sizeof(on)) && on != 0;
		}
		if (qtn_proc) {
			return call == QTN_PROC_APPLY ? 0 : ENOATTR;
		}
	}
		OS_FALLTHROUGH;
	default:
		if (call >= 0x40 && call < 0x100 && (seen & (1ULL << ((call - 0x40) & 63))) == 0) {
			seen |= 1ULL << ((call - 0x40) & 63);
			printf("miniqtn: request 0x%x (first seen from %s[%d]): no stand-in, ENOPOLICY\n", call, p->p_comm,
			    proc_getpid(p));
		}
		return ENOPOLICY;
	}
	if (nargs != 0 && (error = copyin(arg, args, nargs * sizeof(args[0]))) != 0) {
		return error;
	}

	switch (call) {
	case QTN_RESPONSIBLE_GET:
		target = args[0] == 0 ? proc_self() : miniqtn_find((int64_t)args[0], NULL);
		if (target == PROC_NULL) {
			return ESRCH;
		}
		rpid = target->p_responsible_pid > 0 ? target->p_responsible_pid : proc_getpid(target);
		proc_rele(target);
		if ((rp = miniqtn_find(rpid, NULL)) != PROC_NULL) {
			uniqueid = proc_uniqueid(rp);
			proc_rele(rp);
		}
		error = 0;
		if (args[1] != 0) {
			error = copyout(&rpid, (user_addr_t)args[1], sizeof(rpid));
		}
		if (error == 0 && args[2] != 0) {
			error = copyout(&uniqueid, (user_addr_t)args[2], sizeof(uniqueid));
		}
		if (error == 0 && args[3] != 0) {
			uint64_t none = 0;

			error = copyout(&none, (user_addr_t)args[3], sizeof(none));
		}
		return error;
	case QTN_RESPONSIBLE_SET_PID:
		target = args[0] == 0 ? proc_self() : miniqtn_find((int64_t)args[0], NULL);
		if (target == PROC_NULL) {
			return ESRCH;
		}
		error = miniqtn_set(p, target, (int64_t)args[1]);
		proc_rele(target);
		return error;
	case QTN_RESPONSIBLE_SET_TOKEN_SELF:
		target = miniqtn_find((int64_t)args[0], &args[1]);
		if (target == PROC_NULL) {
			return ESRCH;
		}
		error = miniqtn_set(p, target, (int64_t)args[0]);
		proc_rele(target);
		return error;
	case QTN_RESPONSIBLE_SET_TOKEN_CALLER:
		if ((rp = miniqtn_find((int64_t)args[0], &args[1])) == PROC_NULL) {
			return ESRCH;
		}
		proc_rele(rp);
		return miniqtn_set(p, p, (int64_t)args[0]);
	case QTN_RESPONSIBLE_SET_SELF:
		return miniqtn_set(p, p, proc_getpid(p));
	}
	return ENOPOLICY;
}

static const struct mac_policy_ops miniqtn_ops = {
	.mpo_policy_syscall = miniqtn_policy_syscall,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) miniqtn_conf = {
	.mpc_name = "Quarantine",
	.mpc_fullname = "Quarantine policy: responsibility (stand-in)",
	.mpc_ops = &miniqtn_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) miniqtn_handle;

/* Called from kernel_bootstrap_thread() once the MAC framework is up. */
void
minimac_policy_init(void)
{
	mac_policy_register(&miniamfi_conf, &miniamfi_handle, NULL);
	mac_policy_register(&minisandbox_conf, &minisandbox_handle, NULL);
	if (hakios_macos_abi != 0) {
		mac_policy_register(&miniqtn_conf, &miniqtn_handle, NULL);
	}
}

#if !CONFIG_CSR
/*
 * csrctl() without config_csr (which would also put SIP checks into the kernel's own paths, the shared
 * region's among them): what System Integrity Protection is, for user space. Boot-arg macos_abi=1 only
 * (bsd/kern/kern_exec.c): SIP is off, every check passes and the active configuration is every flag, as
 * the lab's macOS kernel answered (csr_allow_all = 1). macOS's launchd reads it at once ("System Integrity
 * Protection is engaged" otherwise) and with SIP on refuses the event streams of the services outside
 * /System (notifyd's com.apple.notifyd.matching: notifyd then dies at every start). Without macos_abi:
 * ENOSYS, what this syscall was before.
 */
#include <sys/csr.h>
#include <sys/sysproto.h>

int
csrctl(__unused proc_t p, struct csrctl_args *args, __unused int32_t *retval)
{
	csr_config_t value = 0;
	int error;

	if (hakios_macos_abi == 0) {
		return ENOSYS;
	}
	if (args->useraddr == 0 || args->usersize != sizeof(value)) {
		return EINVAL;
	}
	switch (args->op) {
	case CSR_SYSCALL_CHECK:
		error = copyin(args->useraddr, &value, sizeof(value));
		return error;                   /* every mask allowed */
	case CSR_SYSCALL_GET_ACTIVE_CONFIG:
		value = CSR_VALID_FLAGS;
		return copyout(&value, args->useraddr, sizeof(value));
	default:
		return ENOSYS;
	}
}
#endif /* !CONFIG_CSR */
