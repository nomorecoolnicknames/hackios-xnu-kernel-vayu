/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU diagnostics: what the threads of a few named processes are waiting for, every N seconds, in the log (the
 * console, so the loader's log and the live log on 5140 too). For a user space that stops making system calls while
 * alive (backboardd in QEMU, U3G1): which Mach port each thread waits on, whose it is, and both stacks.
 *
 * The kernel's own stackshot does the collecting (stack_snapshot_from_kernel, kern/kern_stackshot.c: the same
 * records spindump and tailspin read), one per process: KCDATA, thread wait info, the turnstile chains, the port
 * labels -- which carry launchd's service names where the port has one. This file only reads the records back and
 * prints them:
 *   VAYU STACKSHOT: <proc>[pid] #k: N threads
 *   VAYU STACKSHOT: <proc>[pid] tid T "name": state S (run/wait/...), wait event E, continuation C
 *   VAYU STACKSHOT: <proc>[pid] tid T waits: <type> owner O context X [port label "service"] flags F
 *   VAYU STACKSHOT: <proc>[pid] tid T turnstile: inheritor X hops H flags F [port label "service"]
 *   VAYU STACKSHOT: <proc>[pid] tid T kernel: frame frame ...   (return addresses, unslid by nothing: as captured)
 *   VAYU STACKSHOT: <proc>[pid] tid T user: frame frame ...
 *
 * Boot-args (vayu27=1 for the hook):
 *   stackshot_proc=NAME[,NAME...]   the processes, by p_comm (at most 8 names, 128 characters)
 *   stackshot_every=S               every S seconds (default 30, at least 5)
 *   stackshot_count=N               N rounds, then the thread ends (default 40)
 *   stackshot_delay=S               the first round S seconds after the hook (default 30)
 * Nothing is written anywhere but the log; the stackshot itself stops the system for the time it takes (milliseconds
 * for one process).
 */
#include <kern/block_hint.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/kcdata.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vnode.h>
#include <mach/shared_region.h>
#include <mach-o/loader.h>
#include <vm/vm_map_xnu.h>

#pragma clang diagnostic error "-Wuninitialized"
#pragma clang diagnostic error "-Wsometimes-uninitialized"

extern void IOSleep(unsigned int milliseconds);

/* osfmk/kern/thread.h's thread states (MACH_KERNEL_PRIVATE there), as the stackshot records them */
#define SS_TH_WAIT      0x01
#define SS_TH_RUN       0x04
#define SS_TH_UNINT     0x08

#define SS_NAMES        8
#define SS_BUFFER       (1024u * 1024)
#define SS_FRAMES_K     24
#define SS_FRAMES_U     40
#define SS_WAITS        128
#define SS_LABELS       64

static char ss_names[SS_NAMES][MAXCOMLEN + 1];
static unsigned ss_name_count;
static uint32_t ss_every = 30, ss_count = 40, ss_delay = 30;

struct ss_found {
	int pids[SS_NAMES * 2];
	unsigned n;
};

static int
ss_match(proc_t p, void *arg)
{
	struct ss_found *found = arg;
	char name[MAXCOMLEN + 1];

	proc_name(proc_pid(p), name, sizeof(name));
	for (unsigned i = 0; i < ss_name_count && found->n < SS_NAMES * 2; i++) {
		if (strncmp(name, ss_names[i], MAXCOMLEN) == 0) {
			found->pids[found->n++] = proc_pid(p);
			break;
		}
	}
	return PROC_RETURNED;
}

static const char *
ss_wait_type(uint8_t type)
{
	switch (type) {
	case kThreadWaitNone: return "none";
	case kThreadWaitKernelMutex: return "kernel mutex";
	case kThreadWaitPortReceive: return "mach_msg receive";
	case kThreadWaitPortSetReceive: return "mach_msg receive (port set)";
	case kThreadWaitPortSend: return "mach_msg send (queue full)";
	case kThreadWaitPortSendInTransit: return "mach_msg send (in transit)";
	case kThreadWaitSemaphore: return "semaphore";
	case kThreadWaitKernelRWLockRead: return "rw lock read";
	case kThreadWaitKernelRWLockWrite: return "rw lock write";
	case kThreadWaitKernelRWLockUpgrade: return "rw lock upgrade";
	case kThreadWaitUserLock: return "ulock";
	case kThreadWaitPThreadMutex: return "pthread mutex";
	case kThreadWaitPThreadRWLockRead: return "pthread rwlock read";
	case kThreadWaitPThreadRWLockWrite: return "pthread rwlock write";
	case kThreadWaitPThreadCondVar: return "pthread condvar";
	case kThreadWaitParkedWorkQueue: return "parked workqueue";
	case kThreadWaitWorkloopSyncWait: return "workloop sync wait";
	case kThreadWaitOnProcess: return "wait4";
	case kThreadWaitSleepWithInheritor: return "sleep with inheritor";
	case kThreadWaitEventlink: return "eventlink";
	case kThreadWaitCompressor: return "compressor";
	default: return "other";
	}
}

/* what a wait's owner is (kern_stackshot.c stackshot_thread_wait_owner_info, ipc_port.c kdp_mqueue_*_find_owner) */
static void
ss_owner(char *out, size_t size, uint8_t type, uint64_t owner)
{
	switch (owner) {
	case 0: snprintf(out, size, "owner unknown"); return;
	case STACKSHOT_WAITOWNER_KERNEL: snprintf(out, size, "the kernel's port"); return;
	case STACKSHOT_WAITOWNER_PORT_LOCKED: snprintf(out, size, "the port was locked"); return;
	case STACKSHOT_WAITOWNER_PSET_LOCKED: snprintf(out, size, "the port set was locked"); return;
	case STACKSHOT_WAITOWNER_INTRANSIT: snprintf(out, size, "the port is in transit"); return;
	case STACKSHOT_WAITOWNER_MTXSPIN: snprintf(out, size, "a spinning mutex"); return;
	case STACKSHOT_WAITOWNER_THREQUESTED: snprintf(out, size, "waiting for a worker thread"); return;
	case STACKSHOT_WAITOWNER_SUSPENDED: snprintf(out, size, "the workloop is suspended"); return;
	default: break;
	}
	switch (type) {
	case kThreadWaitPortReceive:
		snprintf(out, size, "port name %#llx in the receiving task's space", owner);
		break;
	case kThreadWaitPortSend:
		snprintf(out, size, "the port's receiver is pid %llu", owner);
		break;
	case kThreadWaitPortSendInTransit:
		snprintf(out, size, "in transit to port %#llx", owner);
		break;
	case kThreadWaitOnProcess:
		snprintf(out, size, "pid %llu", owner);
		break;
	default:
		snprintf(out, size, "owner thread %llu", owner);
		break;
	}
}

static struct {
	int16_t id;
	char name[64];
} ss_labels[SS_LABELS];
static unsigned ss_label_count;

static const char *
ss_label(int16_t id)
{
	if (id == STACKSHOT_PORTLABELID_NONE) {
		return NULL;
	}
	for (unsigned i = 0; i < ss_label_count; i++) {
		if (ss_labels[i].id == id) {
			return ss_labels[i].name;
		}
	}
	return id == STACKSHOT_PORTLABELID_MISSING ? "(label not kept: no room)" : "(label without a name)";
}

/*
 * "kernel 0: a b c ..." of a uint64_t array, at most `most` of them, 8 to a line (a console line, and the loader's,
 * stays short enough not to be cut or mixed with another's)
 */
static void
ss_frames(const char *who, int pid, uint64_t tid, const char *what, const uint64_t *f, uint32_t n, uint32_t most)
{
	for (uint32_t first = 0; first < n && first < most; first += 8) {
		char line[8 * 19 + 1];
		size_t at = 0;

		line[0] = 0;
		for (uint32_t i = first; i < first + 8 && i < n && i < most; i++) {
			at += (size_t)snprintf(line + at, sizeof(line) - at, " %llx", f[i]);
		}
		printf("VAYU STACKSHOT: %s[%d] tid %llu %s %u:%s%s\n", who, pid, tid, what, first, line,
		    first + 8 >= most && n > most ? " ..." : "");
	}
}

extern uint64_t sweet_sample_slide(task_t task, uint64_t *mh);  /* osfmk/kern/exception.c: shared cache slide, header */
extern vm_map_t get_task_map(task_t);

static void
ss_uuid(char out[37], const uint8_t u[16])
{
	snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[0], u[1], u[2], u[3], u[4],
	    u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/*
 * To symbolicate the frames: the main image's path, where its header is and its slide (the header's address less
 * its __TEXT's vmaddr, read from the header in the process's memory), and the shared cache's slide as the task has it.
 */
static void
ss_image(const char *who, int pid)
{
	proc_t p = proc_find(pid);
	char path[MAXPATHLEN] = "?";
	uint64_t mh = 0, cache_slide = 0, text = 0;
	uint8_t header[4096];
	vnode_t vp;
	int len = sizeof(path);

	if (p == PROC_NULL) {
		return;
	}
	if ((vp = proc_getexecutablevnode(p)) != NULLVP) {
		if (vn_getpath(vp, path, &len) != 0) {
			strlcpy(path, "?", sizeof(path));
		}
		vnode_put(vp);
	}
	cache_slide = sweet_sample_slide(proc_task(p), &mh);
	if (mh != 0 && vm_map_read_user(get_task_map(proc_task(p)), mh, header, sizeof(header)) == KERN_SUCCESS) {
		const struct mach_header_64 *h = (const struct mach_header_64 *)header;
		size_t at = sizeof(*h);

		for (uint32_t i = 0; h->magic == MH_MAGIC_64 && i < h->ncmds && at + sizeof(struct load_command) <= sizeof(header);
		    i++) {
			const struct load_command *lc = (const struct load_command *)(header + at);

			if (lc->cmdsize < sizeof(*lc) || at + lc->cmdsize > sizeof(header)) {
				break;
			}
			if (lc->cmd == LC_SEGMENT_64 && strncmp(((const struct segment_command_64 *)lc)->segname, "__TEXT", 16) == 0) {
				text = ((const struct segment_command_64 *)lc)->vmaddr;
				break;
			}
			at += lc->cmdsize;
		}
	}
	proc_rele(p);
	if (text != 0) {
		printf("VAYU STACKSHOT: %s[%d] image %s: header %#llx, __TEXT %#llx, slide %#llx; shared cache slide %#llx\n", who,
		    pid, path, mh, text, mh - text, cache_slide);
	} else {
		printf("VAYU STACKSHOT: %s[%d] image %s: header %#llx (its __TEXT not read); shared cache slide %#llx\n", who, pid,
		    path, mh, cache_slide);
	}
}

/* one process's stackshot, read back and printed */
static void
ss_one(int pid, uint32_t round, uint8_t *buf)
{
	unsigned bytes = 0;
	kern_return_t kr;
	char who[MAXCOMLEN + 1] = "?";
	uint64_t tid = 0;
	unsigned threads = 0;
	const thread_waitinfo_v2_t *waits[SS_WAITS];
	uint32_t wait_size[SS_WAITS];
	const thread_turnstileinfo_v2_t *tss[SS_WAITS];
	uint32_t ts_size[SS_WAITS];
	unsigned wn = 0, tn = 0, caches = 0, images = 0;
	int16_t label_id = 0;

	kr = stack_snapshot_from_kernel(pid, buf, SS_BUFFER, STACKSHOT_KCDATA_FORMAT | STACKSHOT_THREAD_WAITINFO |
	    STACKSHOT_NO_IO_STATS | STACKSHOT_DISABLE_LATENCY_INFO | STACKSHOT_TRYLOCK | STACKSHOT_SAVE_LOADINFO, 0, 0, &bytes);
	if (kr != KERN_SUCCESS || bytes == 0) {
		printf("VAYU STACKSHOT: pid %d #%u: stackshot failed: %d\n", pid, round, kr);
		return;
	}
	proc_name(pid, who, sizeof(who));
	ss_label_count = 0;
	if (pid > 0) {
		ss_image(who, pid);
	}

	kcdata_iter_t it = kcdata_iter(buf, bytes);
	if (!kcdata_iter_valid(it) || it.item->type != KCDATA_BUFFER_BEGIN_STACKSHOT) {
		printf("VAYU STACKSHOT: %s[%d] #%u: not a stackshot buffer (%u bytes)\n", who, pid, round, bytes);
		return;
	}
	it = kcdata_iter_next(it);
	KCDATA_ITER_FOREACH(it) {
		uint32_t type = kcdata_iter_type(it);
		void *p = kcdata_iter_payload(it);
		uint32_t size = kcdata_iter_size(it), count = 0, each = 0;

		if (type == KCDATA_TYPE_ARRAY) {        /* (an array says its elements' type apart) */
			if (!kcdata_iter_array_valid(it)) {
				continue;
			}
			type = kcdata_iter_array_elem_type(it);
			count = kcdata_iter_array_elem_count(it);
			each = kcdata_iter_array_elem_size(it);
		}

		switch (type) {
		case KCDATA_TYPE_CONTAINER_BEGIN: {
			uint32_t what = size >= 4 ? *(uint32_t *)p : 0;
			if (what == STACKSHOT_KCCONTAINER_THREAD) {
				tid = it.item->flags;
				threads++;
			} else if (what == STACKSHOT_KCCONTAINER_PORTLABEL) {
				label_id = (int16_t)it.item->flags;
			}
			break;
		}
		case STACKSHOT_KCTYPE_THREAD_SNAPSHOT:
			if (size >= sizeof(struct thread_snapshot_v2)) {
				const struct thread_snapshot_v2 *t = p;
				printf("VAYU STACKSHOT: %s[%d] tid %llu: state %#x (%s%s%s), wait event %#llx, continuation %#llx\n",
				    who, pid, t->ths_thread_id, t->ths_state, (t->ths_state & SS_TH_WAIT) ? "wait " : "",
				    (t->ths_state & SS_TH_RUN) ? "run " : "", (t->ths_state & SS_TH_UNINT) ? "unint" : "",
				    t->ths_wait_event, t->ths_continuation);
			}
			break;
		case STACKSHOT_KCTYPE_THREAD_NAME:
			printf("VAYU STACKSHOT: %s[%d] tid %llu: name \"%.*s\"\n", who, pid, tid, (int)strnlen(p, size), (const char *)p);
			break;
		case STACKSHOT_KCTYPE_KERN_STACKLR64:
		case STACKSHOT_KCTYPE_USER_STACKLR64:
			if (count != 0 && each == sizeof(uint64_t)) {
				ss_frames(who, pid, tid, type == STACKSHOT_KCTYPE_KERN_STACKLR64 ? "kernel" : "user", p, count,
				    type == STACKSHOT_KCTYPE_KERN_STACKLR64 ? SS_FRAMES_K : SS_FRAMES_U);
			}
			break;
		case STACKSHOT_KCTYPE_THREAD_WAITINFO:
			if (count != 0) {
				for (uint32_t i = 0; i < count && wn < SS_WAITS && each >= sizeof(thread_waitinfo_t); i++) {
					waits[wn] = (const thread_waitinfo_v2_t *)((const uint8_t *)p + (size_t)each * i);
					wait_size[wn++] = each;
				}
			}
			break;
		case STACKSHOT_KCTYPE_THREAD_TURNSTILEINFO:
			if (count != 0) {
				for (uint32_t i = 0; i < count && tn < SS_WAITS && each >= offsetof(thread_turnstileinfo_v2_t, portlabel_id);
				    i++) {
					tss[tn] = (const thread_turnstileinfo_v2_t *)((const uint8_t *)p + (size_t)each * i);
					ts_size[tn++] = each;
				}
			}
			break;
		case STACKSHOT_KCTYPE_SHAREDCACHE_LOADINFO:
		case STACKSHOT_KCTYPE_SHAREDCACHE_INFO:
			if (size >= sizeof(struct dyld_shared_cache_loadinfo) && caches < 2) {
				const struct dyld_shared_cache_loadinfo *c = p;
				static const uint8_t none[16];
				char u[37];

				if (memcmp(c->sharedCacheUUID, none, sizeof(none)) == 0) {
					break;          /* (no shared cache: a static program, QEMU's tests) */
				}
				caches++;
				ss_uuid(u, c->sharedCacheUUID);
				printf("VAYU STACKSHOT: shared cache %s: slide %#llx, slid base %#llx, first mapping %#llx%s\n", u,
				    c->sharedCacheSlide, c->sharedCacheUnreliableSlidBaseAddress, c->sharedCacheSlidFirstMapping,
				    type == STACKSHOT_KCTYPE_SHAREDCACHE_INFO ? " (the system's)" : "");
			}
			break;
		case KCDATA_TYPE_LIBRARY_LOADINFO64:
			/* the images not in the shared cache (the main one, dyld, bundles such as HakiosMetal's) */
			for (uint32_t i = 0; i < count && each >= sizeof(struct dyld_uuid_info_64); i++) {
				const struct dyld_uuid_info_64 *d =
				    (const struct dyld_uuid_info_64 *)((const uint8_t *)p + (size_t)each * i);
				char u[37];

				if (d->imageLoadAddress >= SHARED_REGION_BASE_ARM64 &&
				    d->imageLoadAddress < SHARED_REGION_BASE_ARM64 + SHARED_REGION_SIZE_ARM64) {
					continue;
				}
				if (images++ < 16) {
					ss_uuid(u, d->imageUUID);
					printf("VAYU STACKSHOT: %s[%d] image at %#llx: uuid %s (not in the shared cache)\n", who, pid,
					    d->imageLoadAddress, u);
				}
			}
			break;
		case STACKSHOT_KCTYPE_PORTLABEL_NAME:
			if (ss_label_count < SS_LABELS) {
				ss_labels[ss_label_count].id = label_id;
				strlcpy(ss_labels[ss_label_count].name, p, MIN(size + 1, sizeof(ss_labels[0].name)));
				ss_label_count++;
			}
			break;
		default:
			break;
		}
	}
	for (unsigned i = 0; i < wn; i++) {
		const thread_waitinfo_v2_t *w = waits[i];
		int16_t id = wait_size[i] >= sizeof(thread_waitinfo_v2_t) ? w->portlabel_id : 0;
		uint32_t flags = wait_size[i] >= sizeof(thread_waitinfo_v2_t) ? w->wait_flags : 0;
		const char *name = ss_label(id);
		char owner[80];

		ss_owner(owner, sizeof(owner), w->wait_type, w->owner);
		printf("VAYU STACKSHOT: %s[%d] tid %llu waits: %s (%u), %s, object %#llx%s%s%s%s%s\n", who, pid,
		    w->waiter, ss_wait_type(w->wait_type), w->wait_type, owner, w->context, name ? ", port label \"" : "",
		    name ? name : "", name ? "\"" : "", (flags & STACKSHOT_WAITINFO_FLAGS_SPECIALREPLY) ? ", a special reply port" : "",
		    (flags & STACKSHOT_WAITINFO_FLAGS_BOOTSTRAP) ? ", the bootstrap port" : "");
	}
	for (unsigned i = 0; i < tn; i++) {
		const thread_turnstileinfo_v2_t *t = tss[i];
		const char *name = ts_size[i] >= sizeof(thread_turnstileinfo_v2_t) ? ss_label(t->portlabel_id) : NULL;

		uint64_t f = t->turnstile_flags;

		printf("VAYU STACKSHOT: %s[%d] tid %llu turnstile: %s %llu (%#llx), %u hops, flags %#llx%s%s%s%s\n", who, pid,
		    t->waiter, (f & STACKSHOT_TURNSTILE_STATUS_THREAD) ? "the thread holding it is tid" :
		    (f & STACKSHOT_TURNSTILE_STATUS_BLOCKED_ON_TASK) ? "the task holding it is pid" :
		    (f & STACKSHOT_TURNSTILE_STATUS_WORKQUEUE) ? "a workqueue, at" : "the end of the chain unknown,",
		    t->turnstile_context, t->turnstile_context, t->number_of_hops, f,
		    (f & STACKSHOT_TURNSTILE_STATUS_LOCKED_WAITQ) ? " (a waitq was locked)" : "", name ? ", port label \"" : "",
		    name ? name : "", name ? "\"" : "");
	}
	printf("VAYU STACKSHOT: %s[%d] #%u: %u threads, %u waits, %u turnstiles, %u port labels, %u bytes%s\n", who, pid, round,
	    threads, wn, tn, ss_label_count, bytes, KCDATA_ITER_FOREACH_FAILED(it) ? " (the records end early)" : "");
}

static void
ss_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint8_t *buf = kalloc_data(SS_BUFFER, Z_WAITOK);

	if (buf == NULL) {
		printf("VAYU STACKSHOT: no %u KB for the records\n", SS_BUFFER >> 10);
		return;
	}
	IOSleep(ss_delay * 1000);
	for (uint32_t round = 1; round <= ss_count; round++) {
		struct ss_found found = { .n = 0 };

		proc_iterate(PROC_ALLPROCLIST, ss_match, &found, NULL, NULL);
		if (found.n == 0) {
			printf("VAYU STACKSHOT: #%u: none of the processes named is running\n", round);
		}
		for (unsigned i = 0; i < found.n; i++) {
			ss_one(found.pids[i], round, buf);
		}
		IOSleep(ss_every * 1000);
	}
	kfree_data(buf, SS_BUFFER);
}

static void
vayu_stackshot_start(void)
{
	char list[128] = "";
	thread_t thread;

	if (!PE_parse_boot_arg_str("stackshot_proc", list, sizeof(list)) || list[0] == 0) {
		return;
	}
	for (char *c = list, *start = list;; c++) {
		if (*c == ',' || *c == 0) {
			bool end = *c == 0;

			*c = 0;
			if (*start != 0 && ss_name_count < SS_NAMES) {
				strlcpy(ss_names[ss_name_count++], start, sizeof(ss_names[0]));
			}
			if (end) {
				break;
			}
			start = c + 1;
		}
	}
	PE_parse_boot_argn("stackshot_every", &ss_every, sizeof(ss_every));
	PE_parse_boot_argn("stackshot_count", &ss_count, sizeof(ss_count));
	PE_parse_boot_argn("stackshot_delay", &ss_delay, sizeof(ss_delay));
	ss_every = ss_every < 5 ? 5 : ss_every > 3600 ? 3600 : ss_every;
	ss_delay = ss_delay > 3600 ? 3600 : ss_delay;
	ss_count = ss_count > 10000 ? 10000 : ss_count;
	printf("VAYU STACKSHOT: %u process name(s) (%s...), every %u s after %u s, %u rounds\n", ss_name_count, ss_names[0],
	    ss_every, ss_delay, ss_count);
	if (ss_name_count != 0 && kernel_thread_start(ss_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

VAYU_HOOK(VAYU_PHASE_AFTER_DEVFS, stackshot, vayu_stackshot_start);
