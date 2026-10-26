/*
 * libbt.dylib: keep bluetoothd's unsupported antenna VSC out of the controller conversation.
 *
 * vayu's Bluetooth transport and HCI bring-up are in the kernel.  The remaining stall is in the
 * stock bluetoothd: its BCM_4357 profile marks BD_VSC_OLYMPIC_ANTENNA_PREF_B supported, sends HCI
 * 0xfce2 with Olympic sub-opcode 0x15, then waits 15 seconds for an answer this controller does not
 * provide and restarts the stack (reason=3602).
 *
 * The support set is one byte per VSC id in the controller object: an array at object+168, id 140
 * (0x8c) is BD_VSC_OLYMPIC_ANTENNA_PREF_B, so the byte lives at object+308 (0x134).  The profile
 * initializer sets it with exactly one instruction in the whole 8.9 MB image (sha256
 * a315f41598a7d6271cafbdc11204944f3e75e6d10cd6375b322aafc34aef6889):
 *
 *     strb w8, [x19, #308]   ->   strb wzr, [x19, #308]
 *     68 d2 04 39                  7f d2 04 39
 *
 * The gate that consumes the byte is at 0x100282df8: it does
 * `ldrb w8,[x20+w19+168]; cbz -> os_log("VSC %s, not supported"); return false`, so a zero byte
 * takes bluetoothd's own unsupported path (no send, no 15 s wait, no restart), and the generic
 * sender (0x10025b5d4) returns an error to its caller instead of queueing the command.  The
 * preceding `mov w8,#1` must stay: the next store uses w8 for a different VSC.
 *
 * WHY THIS PATCH SCANS INSTEAD OF USING AN UNSLID ADDRESS
 * ------------------------------------------------------
 * The first two attempts computed the site as `_dyld_get_image_header(0) + 0x28330c`.  For the
 * binary above that arithmetic is right for any slide: __TEXT is at vmaddr 0x100000000, fileoff 0,
 * filesize 0x7f4000, so the byte at file offset 0x28330c is at header + 0x28330c.  Yet on the
 * phone the read returned 0 (direct) or KERN_INVALID_ADDRESS (vm_read_overwrite).  A fixed
 * `header + offset` cannot survive that: either image 0 was not this file, or the page was not in
 * the task's map at that address.  So this version
 *
 *   1. resolves the image from the task's own Mach-O header and takes the slide from the header
 *      itself (`slide = header - __TEXT.vmaddr`), cross-checked against task_info(TASK_DYLD_INFO)
 *      and the dyld all_image_infos list;
 *   2. finds the instruction by a 24-byte byte pattern inside the image's __text section, not by
 *      an unslid address, and patches whatever address the pattern is actually at;
 *   3. if the pattern is not in the main image, scans the other dyld images' __text and, as a last
 *      resort, every executable region of the task -- so a cache image, a renamed binary or an
 *      unexpected slide still lands on the instruction;
 *   4. if nothing is found, prints the image path, base, slide, __text range and the VM region at
 *      the address the old code used, so the next run says *why* instead of "word 0".
 *
 * Only the one store changes.  This is a private, copy-on-write change to one instruction in the
 * running process; Apple's file on disk and the rest of the capability table are untouched.  If
 * the pattern is not seen exactly once, nothing is changed and the run must not be described as
 * patched.  Removing the injection line reverts it on the next boot.
 */
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern const char *getprogname(void);

/*
 * mach_vm_read_overwrite() is a MIG routine of libsystem_kernel, but the iPhoneOS SDK ships
 * <mach/mach_vm.h> as an #error stub, so the prototype is declared here.  It is the 64-bit
 * sibling of vm_read_overwrite() and goes through a different trap path, which is why it is
 * tried first: the earlier run's vm_read_overwrite() answered KERN_INVALID_ADDRESS for an
 * address the direct read could reach.
 */
extern kern_return_t mach_vm_read_overwrite(vm_map_t target_task, mach_vm_address_t address,
    mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *outsize);

/* The expected site in this exact bluetoothd, as a file offset inside __TEXT (informational). */
#define BT_VSC_TEXT_OFFSET     0x28330cUL

/*
 * 24 bytes of __text around the store, as they are in bluetoothd 20F66 (file offset 0x2832fc):
 *
 *   54000040  b.eq        +8
 *   52800015  mov         w21, #0
 *   39034675  strb        w21, [x19, #209]
 *   52800028  mov         w8, #1
 *   3904d268  strb        w8,  [x19, #308]     <- the one instruction that changes
 *   39030a68  strb        w8,  [x19, #194]
 *
 * The four-word prefix is the search key; the two words after it prove the context.  The whole
 * 24-byte sequence occurs exactly once in the 8.9 MB file (checked with the sha256 above).
 */
static const uint32_t bt_vsc_signature[6] = {
	0x54000040, 0x52800015, 0x39034675, 0x52800028, 0x3904d268, 0x39030a68,
};
#define BT_VSC_SIG_WORDS        6
#define BT_VSC_SIG_PATCH        4       /* index of the strb inside bt_vsc_signature */
#define BT_VSC_SIG_KEY_WORDS    4       /* words that do not change when patched */
#define BT_VSC_STORE_BEFORE     0x3904d268U     /* strb w8,  [x19,#308] */
#define BT_VSC_STORE_AFTER      0x3904d27fU     /* strb wzr, [x19,#308] */

#define BT_READ_TRIES           300     /* 300 * 100 ms = at most 30 s */
#define BT_READ_USLEEP          100000
#define BT_SCAN_CHUNK           (64 * 1024)
#define BT_SCAN_ALL_EVERY       50      /* full-image / all-region scans: every 5 s */
#define BT_SCAN_MAX_BYTES       (512UL * 1024 * 1024)   /* bound for the all-region scan */

struct bt_text {
	uint64_t base;                          /* runtime address of the __text section */
	uint64_t size;
};

static vm_address_t g_patch_site;               /* runtime address of the strb, 0 if unknown */
static bool g_reported_map;
static bool g_reported_infos;

static const char *
bt_image_name(uint32_t index)
{
	const char *name = _dyld_get_image_name(index);

	return name != NULL ? name : "(no path)";
}

static void
say(const char *format, ...)
{
	char line[320];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	(void)write(STDERR_FILENO, line, (size_t)n);
}

/* ------------------------------------------------------------------ reading mapped bytes ------ */

/* Ask the VM for the bytes: works for a text page that has not been executed yet. */
static bool
bt_read_vm(vm_address_t address, void *buffer, size_t length)
{
	mach_vm_size_t got = 0;
	kern_return_t kr;

	kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address,
	    (mach_vm_size_t)length, (mach_vm_address_t)(uintptr_t)buffer, &got);
	if (kr == KERN_SUCCESS && got == length) {
		return true;
	}
	got = 0;
	kr = vm_read_overwrite(mach_task_self(), address, (vm_size_t)length,
	    (vm_address_t)(uintptr_t)buffer, &got);
	return kr == KERN_SUCCESS && got == length;
}

/*
 * Last resort: read it as the process would.  A hole in the map raises SIGSEGV/SIGBUS here; the
 * guard turns that into "not readable" and the scan steps over the hole.  The handlers are
 * installed only for the duration of the read and the previous ones are put back.
 */
static sigjmp_buf g_fault_jmp;
static volatile sig_atomic_t g_fault_armed;

static void
bt_fault_handler(int sig)
{
	if (g_fault_armed) {
		g_fault_armed = 0;
		siglongjmp(g_fault_jmp, sig);
	}
	/* Not ours: put the default back and let it happen again. */
	signal(sig, SIG_DFL);
	raise(sig);
}

static bool
bt_read_direct(vm_address_t address, void *buffer, size_t length)
{
	struct sigaction segv, bus, old_segv, old_bus;
	volatile bool ok = false;

	memset(&segv, 0, sizeof(segv));
	segv.sa_handler = bt_fault_handler;
	sigemptyset(&segv.sa_mask);
	bus = segv;
	if (sigaction(SIGSEGV, &segv, &old_segv) != 0) {
		return false;
	}
	if (sigaction(SIGBUS, &bus, &old_bus) != 0) {
		(void)sigaction(SIGSEGV, &old_segv, NULL);
		return false;
	}
	if (sigsetjmp(g_fault_jmp, 1) == 0) {
		g_fault_armed = 1;
		memcpy(buffer, (const void *)address, length);
		ok = true;
	}
	g_fault_armed = 0;
	(void)sigaction(SIGSEGV, &old_segv, NULL);
	(void)sigaction(SIGBUS, &old_bus, NULL);
	return ok;
}

static bool
bt_read(vm_address_t address, void *buffer, size_t length)
{
	return bt_read_vm(address, buffer, length) || bt_read_direct(address, buffer, length);
}

/* ------------------------------------------------------------------ the image's __text -------- */

/*
 * Walk the Mach-O load commands at `header` and return the __text section plus the __TEXT
 * segment's vmaddr.  The slide is then `header - text_vmaddr`, which is the definition dyld uses
 * for an image, so no other source of the slide is needed (task_info is only a cross-check).
 */
static bool
bt_image_text(const struct mach_header_64 *header, struct bt_text *text, uint64_t *text_vmaddr)
{
	const uint8_t *cmd;
	uint32_t ncmds;

	if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) {
		return false;
	}
	cmd = (const uint8_t *)(header + 1);
	ncmds = header->ncmds;
	for (uint32_t i = 0; i < ncmds; i++) {
		const struct load_command *lc = (const struct load_command *)cmd;

		if (lc->cmdsize < sizeof(*lc) || (const uint8_t *)lc + lc->cmdsize < cmd) {
			return false;
		}
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;

			if (strncmp(seg->segname, "__TEXT", sizeof(seg->segname)) == 0) {
				*text_vmaddr = seg->vmaddr;
				const struct section_64 *sect = (const struct section_64 *)(seg + 1);

				for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
					if (strncmp(sect->sectname, "__text", sizeof(sect->sectname)) == 0 &&
					    strncmp(sect->segname, "__TEXT", sizeof(sect->segname)) == 0) {
						text->base = sect->addr;
						text->size = sect->size;
						return text->size != 0;
					}
				}
			}
		}
		cmd += lc->cmdsize;
	}
	return false;
}

/* ------------------------------------------------------------------ scanning ------------------ */

/*
 * Look for the 24-byte sequence in one range.  Returns the runtime address of the strb (the word
 * at index 4), or 0.  *patched is set when the range already holds the patched instruction.
 * A hole in the range (a page not mapped) is skipped, not fatal: the scan continues after it.
 */
static vm_address_t
bt_scan_range(uint64_t start, uint64_t size, bool *patched, bool verbose)
{
	uint8_t *buffer = malloc(BT_SCAN_CHUNK + sizeof(bt_vsc_signature));
	uint64_t done = 0;
	uint32_t holes = 0;

	if (buffer == NULL || size < sizeof(bt_vsc_signature)) {
		free(buffer);
		return 0;
	}
	while (done < size) {
		size_t want = (size_t)((size - done) < BT_SCAN_CHUNK ? (size - done) : BT_SCAN_CHUNK);
		size_t have = want;

		if (!bt_read((vm_address_t)(start + done), buffer, want)) {
			/* step over one page and try again: this chunk is a hole */
			holes++;
			done += 4096;
			continue;
		}
		for (size_t off = 0; off + sizeof(bt_vsc_signature) <= have; off += 4) {
			const uint32_t *w = (const uint32_t *)(buffer + off);
			bool match = true;

			for (unsigned k = 0; k < BT_VSC_SIG_KEY_WORDS; k++) {
				if (w[k] != bt_vsc_signature[k]) {
					match = false;
					break;
				}
			}
			if (!match) {
				continue;
			}
			if (w[5] != bt_vsc_signature[5]) {
				continue;
			}
			if (w[4] != BT_VSC_STORE_BEFORE && w[4] != BT_VSC_STORE_AFTER) {
				continue;
			}
			*patched = (w[4] == BT_VSC_STORE_AFTER);
			free(buffer);
			return (vm_address_t)(start + done + off + BT_VSC_SIG_PATCH * 4);
		}
		if (have <= sizeof(bt_vsc_signature) - 4) {
			break;                  /* the tail is shorter than the overlap */
		}
		/* overlap by the signature so a match across the chunk edge is not lost */
		done += have - (sizeof(bt_vsc_signature) - 4);
	}
	if (verbose && holes != 0) {
		say("vayu-bt: scan of %#llx+%#llx skipped %u unmapped page(s)\n",
		    (unsigned long long)start, (unsigned long long)size, (unsigned)holes);
	}
	free(buffer);
	return 0;
}

/* The main executable first: that is where the instruction is in every build we have seen. */
static vm_address_t
bt_scan_main_image(bool *patched, bool verbose, struct bt_text *out_text, uint64_t *out_vmaddr)
{
	const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
	struct bt_text text;
	uint64_t text_vmaddr = 0;
	intptr_t slide;
	vm_address_t site;

	if (header == NULL || !bt_image_text(header, &text, &text_vmaddr)) {
		if (verbose) {
			say("vayu-bt: image 0 is not a usable arm64 Mach-O (header %p); "
			    "the pattern scan will use the other images\n", (const void *)header);
		}
		return 0;
	}
	slide = (intptr_t)header - (intptr_t)text_vmaddr;
	if (out_text != NULL) {
		out_text->base = text.base + (uint64_t)slide;
		out_text->size = text.size;
	}
	if (out_vmaddr != NULL) {
		*out_vmaddr = text_vmaddr;
	}
	if (verbose) {
		say("vayu-bt: image 0 %s base %p (__TEXT vmaddr %#llx, slide %#lx), __text %#llx+%#llx; "
		    "the old fixed address was %p\n", bt_image_name(0), (const void *)header,
		    (unsigned long long)text_vmaddr, (unsigned long)slide,
		    (unsigned long long)(text.base + (uint64_t)slide),
		    (unsigned long long)text.size,
		    (void *)((vm_address_t)header + BT_VSC_TEXT_OFFSET));
	}
	site = bt_scan_range(text.base + (uint64_t)slide, text.size, patched, verbose);
	if (site == 0 && verbose) {
		/* say what is actually mapped where the previous version looked */
		vm_address_t query = (vm_address_t)header + BT_VSC_TEXT_OFFSET;
		vm_size_t region_size = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;
		kern_return_t kr = vm_region_64(mach_task_self(), &query, &region_size,
		    VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);

		if (kr == KERN_SUCCESS) {
			say("vayu-bt: no pattern in __text; at %p the VM has a region %#llx+%#llx "
			    "prot %#x\n", (void *)((vm_address_t)header + BT_VSC_TEXT_OFFSET),
			    (unsigned long long)query, (unsigned long long)region_size,
			    (unsigned)info.protection);
		} else {
			say("vayu-bt: no pattern in __text; at %p vm_region says %d "
			    "(KERN_INVALID_ADDRESS is 1)\n",
			    (void *)((vm_address_t)header + BT_VSC_TEXT_OFFSET), (int)kr);
		}
	}
	return site;
}

/* Any dyld image: covers "the target is in a dylib in the shared cache" without assuming it. */
static vm_address_t
bt_scan_images(bool *patched, bool verbose)
{
	uint32_t count = _dyld_image_count();

	for (uint32_t i = 1; i < count; i++) {
		const struct mach_header_64 *header =
		    (const struct mach_header_64 *)_dyld_get_image_header(i);
		struct bt_text text;
		uint64_t vmaddr = 0;
		intptr_t slide;
		vm_address_t site;

		if (header == NULL || !bt_image_text(header, &text, &vmaddr)) {
			continue;
		}
		slide = (intptr_t)header - (intptr_t)vmaddr;
		site = bt_scan_range(text.base + (uint64_t)slide, text.size, patched, false);
		if (site != 0) {
			if (verbose) {
				say("vayu-bt: pattern found in image %u (%s) at %p\n", i,
				    bt_image_name(i), (void *)site);
			}
			return site;
		}
	}
	return 0;
}

/* Every executable region: the last resort when no image header describes the code. */
static vm_address_t
bt_scan_exec_regions(bool *patched, bool verbose)
{
	vm_address_t address = 0;
	uint64_t scanned = 0;
	unsigned regions = 0;

	for (;;) {
		vm_size_t size = 0;
		vm_region_submap_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		natural_t depth = 0;
		kern_return_t kr = vm_region_recurse_64(mach_task_self(), &address, &size, &depth,
		    (vm_region_recurse_info_t)&info, &count);

		if (kr != KERN_SUCCESS) {
			break;
		}
		if (size == 0) {
			break;
		}
		if ((info.protection & VM_PROT_EXECUTE) != 0) {
			bool local = false;
			vm_address_t site = bt_scan_range(address, size, &local, false);

			regions++;
			scanned += size;
			if (site != 0) {
				*patched = local;
				if (verbose) {
					say("vayu-bt: pattern found in executable region %#llx+%#llx "
					    "at %p\n", (unsigned long long)address,
					    (unsigned long long)size, (void *)site);
				}
				return site;
			}
			if (scanned > BT_SCAN_MAX_BYTES) {
				if (verbose) {
					say("vayu-bt: all-region scan stopped after %llu bytes in %u "
					    "regions\n", (unsigned long long)scanned, regions);
				}
				break;
			}
		}
		address += size;
	}
	return 0;
}

/* ------------------------------------------------------------------ patching ------------------ */

static void
bt_restore_text(vm_address_t page, size_t page_size)
{
	kern_return_t kr = vm_protect(mach_task_self(), page, (vm_size_t)page_size, FALSE,
	    VM_PROT_READ | VM_PROT_EXECUTE);

	if (kr != KERN_SUCCESS) {
		say("vayu-bt: cannot restore text protection (%d)\n", kr);
	}
}

static bool
bt_patch(vm_address_t site)
{
	vm_address_t page = site & ~(vm_address_t)(getpagesize() - 1);
	size_t page_size = (size_t)getpagesize();
	kern_return_t kr;
	uint32_t word = 0;

	kr = vm_protect(mach_task_self(), page, (vm_size_t)page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		/* the same request through the 64-bit entry point, then plain mprotect */
		kern_return_t kr64 = mach_vm_protect(mach_task_self(), (mach_vm_address_t)page,
		    (mach_vm_size_t)page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

		if (kr64 != KERN_SUCCESS &&
		    mprotect((void *)page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
			say("vayu-bt: cannot make the text page %p private/writable "
			    "(vm_protect %d, mach_vm_protect %d); VSC table unchanged\n",
			    (void *)page, (int)kr, (int)kr64);
			return false;
		}
	}
	*(volatile uint32_t *)site = BT_VSC_STORE_AFTER;
	sys_icache_invalidate((void *)site, sizeof(uint32_t));
	bt_restore_text(page, page_size);

	if (bt_read(site, &word, sizeof(word)) && word == BT_VSC_STORE_AFTER) {
		say("vayu-bt: BD_VSC_OLYMPIC_ANTENNA_PREF_B marked unsupported in this process "
		    "at %p; bluetoothd will not send its antenna VSC or wait for it\n", (void *)site);
		return true;
	}
	say("vayu-bt: wrote %p but read back %#x, not %#x; VSC table may be unchanged\n",
	    (void *)site, (unsigned)word, (unsigned)BT_VSC_STORE_AFTER);
	return false;
}

/* ------------------------------------------------------------------ diagnostics --------------- */

/*
 * One block that answers, on the next hardware run, what the two failed runs could not: which
 * image is image 0, where dyld says its images are, and what the VM has at the address the old
 * code used.
 */
static void
bt_report_infos(void)
{
	task_dyld_info_data_t info;
	mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
	kern_return_t kr;

	if (g_reported_infos) {
		return;
	}
	kr = task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&info, &count);
	if (kr != KERN_SUCCESS) {
		say("vayu-bt: task_info(TASK_DYLD_INFO) failed: %d\n", (int)kr);
		return;
	}
	g_reported_infos = true;
	say("vayu-bt: dyld all_image_infos %#llx (%u bytes); image 0 \"%s\" at %p\n",
	    (unsigned long long)info.all_image_info_addr, (unsigned)info.all_image_info_size,
	    bt_image_name(0), _dyld_get_image_header(0));
	if (info.all_image_info_addr != 0 && info.all_image_info_size >= sizeof(struct dyld_all_image_infos)) {
		const struct dyld_all_image_infos *all = (const struct dyld_all_image_infos *)
		    (uintptr_t)info.all_image_info_addr;

		say("vayu-bt: dyld %u, %u image(s), main %p (%s), dyld %p%s%s\n",
		    (unsigned)all->version, (unsigned)all->infoArrayCount,
		    all->infoArray != NULL ? all->infoArray[0].imageLoadAddress : NULL,
		    all->infoArray != NULL && all->infoArray[0].imageFilePath != NULL ?
		    all->infoArray[0].imageFilePath : "?",
		    (const void *)all->dyldImageLoadAddress,
		    all->errorMessage != NULL ? ", error: " : "",
		    all->errorMessage != NULL ? all->errorMessage : "");
	}
}

/* ------------------------------------------------------------------ the worker ----------------- */

static bool
bt_try_patch(bool full_scan, bool verbose)
{
	bool patched = false;
	vm_address_t site;

	if (g_patch_site != 0) {
		uint32_t word = 0;

		if (bt_read(g_patch_site, &word, sizeof(word)) && word == BT_VSC_STORE_AFTER) {
			if (verbose) {
				say("vayu-bt: ANTENNA_PREF_B already unsupported (runtime patch present)\n");
			}
			return true;
		}
		if (bt_read(g_patch_site, &word, sizeof(word)) && word == BT_VSC_STORE_BEFORE) {
			return bt_patch(g_patch_site);
		}
		g_patch_site = 0;                       /* the site went away: find it again */
	}
	site = bt_scan_main_image(&patched, verbose, NULL, NULL);
	if (site == 0 && full_scan) {
		site = bt_scan_images(&patched, verbose);
	}
	if (site == 0 && full_scan) {
		site = bt_scan_exec_regions(&patched, verbose);
	}
	if (site == 0) {
		return false;
	}
	if (patched) {
		g_patch_site = site;
		say("vayu-bt: ANTENNA_PREF_B already unsupported at %p (runtime patch present)\n",
		    (void *)site);
		return true;
	}
	g_patch_site = site;
	return bt_patch(site);
}

static void *
bt_worker(void *unused)
{
	(void)unused;
	for (unsigned attempt = 0; attempt < BT_READ_TRIES; attempt++) {
		bool full = (attempt % BT_SCAN_ALL_EVERY) == 0;

		if (bt_try_patch(full, attempt == 0)) {
			return NULL;
		}
		if (attempt == 0) {
			say("vayu-bt: the antenna VSC store is not in image 0 yet; "
			    "retrying for at most 30 s\n");
			bt_report_infos();
		}
		usleep(BT_READ_USLEEP);
	}
	if (!g_reported_map) {
		g_reported_map = true;
		say("vayu-bt: exact instruction not found in 30 s; VSC table left unchanged\n");
		bt_report_infos();
		(void)bt_scan_main_image(&(bool){ false }, true, NULL, NULL);   /* prints the map */
	}
	return NULL;
}

__attribute__((constructor)) static void
bt_vsc_init(void)
{
	const char *name = getprogname();
	const char *image = _dyld_get_image_name(0);
	pthread_t thread;
	int error;

	if (name == NULL || strcmp(name, "bluetoothd") != 0) {
		return;
	}
	if (image != NULL && strstr(image, "bluetoothd") == NULL) {
		/*
		 * Keep going -- the process may be renamed -- but say so: this is the one line that
		 * would have explained the earlier "word 0" runs if image 0 had been something else.
		 */
		say("vayu-bt: process is bluetoothd but image 0 is \"%s\"; "
		    "the VSC store is looked up by pattern\n", image);
	}
	error = pthread_create(&thread, NULL, bt_worker, NULL);
	if (error != 0) {
		say("vayu-bt: cannot start the text reader (%d); VSC table unchanged\n", error);
		return;
	}
	pthread_detach(thread);
}
