/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * VAYU (POCO X3 Pro): the way back to the loader, and the loader's log.
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) osfmk/arm64/loader_return.c (the 8796 line of this port; its loader_log_copy after
 * telegdyd/ios-vayu, branch vayu-dev, commit 66d2e6bcb3c0, MIT). Ported to xnu-12377 by K1 (plans/ios27-macos):
 *  - the 8796 drivers' quiesce/stop calls are a registry now (loader_quiesce_register, loader_stop_register in
 *    sys/vayu_loader.h): each port registers its own, so this file names no driver;
 *  - the console reaches the log through sweet_log_append() (osfmk/console/serial_console.c _cnputs calls it on
 *    every SWEET board): everything printf()/IOLog()/os_log put on the console, quiet boot or not;
 *  - the TTBR0 mapping of the loader's stub is made with a 4K granule of its own (TCR_EL1.TG0), so the 16K kernel
 *    (__ARM_16K_PG__, VAYU.h) returns the same way the 4K one did;
 *  - a panic comes here through sweet_panic_hook (osfmk/kern/debug.c) and kdp_machine_reboot_type().
 *
 * On a phone without a serial port the loader is the only way out for the kernel log. The XNU loader (xnuloader,
 * hakios repository) leaves UEFI resident and patches the physical address of its return stub into the slot
 * "XNULDRST" of start.s. When the kernel has nothing more to do (panic, a timed return, the host's "stop") it collects
 * its log, the message buffer and the panic buffer, maps the stub 1:1 through TTBR0 and jumps to it. The loader then
 * restores the UEFI environment and writes the log to storage (logdump). A loader that watches from another core
 * (bootcpu=N) reads the log through loader_live below even when the kernel never comes back.
 *
 * Only valid while the kernel does not drive hardware that UEFI also uses.
 */
#include <mach/mach_types.h>
#include <machine/atomic.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_VAYU)

/*
 * The console log handed back to the loader. A long session writes more than fits, and both ends matter: the start
 * of the boot is kept as it is, and of everything after it the most recent part (a ring); room is left for what is
 * appended on the way out (the kernel message buffer, a panic's text). loader_log_linearize() puts the pieces in
 * order before returning. 8 MB: 512 KB from the boot's start, a 6.5 MB ring of the newest, 1 MB left free (the 8796
 * line's numbers; the loader stages up to 9 MB).
 */
#define LOADER_LOG_SIZE (8 * 1024 * 1024)
#define LOADER_LOG_HEAD (512 * 1024)
#define LOADER_LOG_TAIL (1024 * 1024)
#define LOADER_LOG_RING (LOADER_LOG_SIZE - LOADER_LOG_HEAD - LOADER_LOG_TAIL)

/* The prototypes of sys/vayu_loader.h (osfmk does not see bsd/sys): keep the two the same. */
void loader_log_putc(char c);
void loader_log_line(const char *line);
void loader_log_write(const char *s, int size);
void loader_return(void);
void loader_checkpoint(uint64_t n);
bool loader_present(void);
uint32_t loader_log_copy(uint64_t *pos, char *out, uint32_t max);
void loader_live_set_vctl(uint64_t mbox_pa, uint64_t window_pa, uint64_t window_len);
uint64_t loader_live_usb_take(void);
void loader_live_usb_take_set(uint64_t value);
uint64_t loader_live_usb_given(void);
void loader_live_stop_set(void);
int loader_quiesce_register(const char *name, void (*fn)(void));
int loader_stop_register(const char *name, void (*fn)(void), uint32_t extra_ms, uint32_t flags);
int loader_before_end_register(void (*fn)(const char *why));
void loader_set_stop_others(bool (*fn)(void));
void hakios_memory_line(char *buf, size_t len);
void sweet_log_append(const char *c, int size);
#define LOADER_STOP_TIMED_ONLY  0x1     /* (sys/vayu_loader.h) only on loader_return_after's way out */

extern void IOSleep(unsigned int milliseconds);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);

/* Also referenced from the early exception vector in start.s. */
char     loader_log[LOADER_LOG_SIZE];
uint32_t loader_log_len;
static bool     loader_return_entered;

/* One L1 table and one L2 table of 4K granule map the single 2 MB block of the stub (see loader_jump). */
static uint64_t loader_l1[512] __attribute__((aligned(4096)));
static uint64_t loader_l2[512] __attribute__((aligned(4096)));

static uint32_t loader_log_ring_at;      /* where the next character goes in the ring */
static bool     loader_log_wrapped;
static bool     loader_log_linear;       /* the pieces have been put in order: only appending from now on */
static uint64_t loader_log_total;        /* characters ever put in: the live log's positions (loader_log_copy) */

/*
 * Characters come from every processor, and from interrupt handlers on top of whatever was writing one. The lock is
 * of the simplest kind, good from the first line of C code to the last of a panic; whoever cannot have it for a long
 * while (its holder is gone: a panic inside this very function) goes ahead without it.
 */
static uint32_t loader_log_lock;

static void loader_log_put_locked(char c);

static inline uint64_t
loader_log_lock_take(bool *locked)
{
	uint64_t daif = __builtin_arm_rsr64("DAIF");

	__builtin_arm_wsr("DAIFSet", DAIFSC_IRQF | DAIFSC_FIQF);
	*locked = false;
	for (uint32_t tries = 0; !*locked && tries < 10000000; tries++) {
		*locked = os_atomic_cmpxchg(&loader_log_lock, 0, 1, acquire);
	}
	return daif;
}

static inline void
loader_log_lock_give(bool locked, uint64_t daif)
{
	if (locked) {
		os_atomic_store(&loader_log_lock, 0, release);
	}
	__builtin_arm_wsr64("DAIF", daif);
}

void
loader_log_putc(char c)
{
	bool locked;
	uint64_t daif;

	if (c == '\r') {
		return;
	}
	daif = loader_log_lock_take(&locked);
	loader_log_put_locked(c);
	loader_log_lock_give(locked, daif);
}

/*
 * A whole line at once, under the lock: lines written from several processors at the same time are not mixed up.
 */
void
loader_log_line(const char *line)
{
	bool locked;
	uint64_t daif = loader_log_lock_take(&locked);

	for (; *line != 0; line++) {
		if (*line != '\r') {
			loader_log_put_locked(*line);
		}
	}
	loader_log_lock_give(locked, daif);
}

void
loader_log_write(const char *s, int size)
{
	bool locked;
	uint64_t daif;

	if (s == NULL || size <= 0) {
		return;
	}
	daif = loader_log_lock_take(&locked);
	for (int i = 0; i < size; i++) {
		if (s[i] != '\r' && s[i] != 0) {
			loader_log_put_locked(s[i]);
		}
	}
	loader_log_lock_give(locked, daif);
}

/*
 * Scion's SWEET console hook (osfmk/console/serial_console.c, _cnputs: every character the console writes, before the
 * quiet boot's mute and disableConsoleOutput): on VAYU the loader's log. It replaces the no-op of bsd/dev/vayu_stubs.c.
 */
void
sweet_log_append(const char *c, int size)
{
	loader_log_write(c, size);
}

static void
loader_log_put_locked(char c)
{
	loader_log_total++;
	if (loader_log_linear || loader_log_len < LOADER_LOG_HEAD) {
		if (loader_log_len < LOADER_LOG_SIZE) {
			loader_log[loader_log_len++] = c;
		}
	} else {
		loader_log[LOADER_LOG_HEAD + loader_log_ring_at] = c;
		if (++loader_log_ring_at == LOADER_LOG_RING) {
			loader_log_ring_at = 0;
			loader_log_wrapped = true;
		}
		if (!loader_log_wrapped) {
			loader_log_len = LOADER_LOG_HEAD + loader_log_ring_at;
		}
	}
}

static void
loader_log_reverse(char *p, uint32_t n)
{
	for (uint32_t i = 0, j = n; i + 1 < j; i++, j--) {
		char t = p[i];

		p[i] = p[j - 1];
		p[j - 1] = t;
	}
}

/* Oldest first: the head, a note about what was dropped, then the ring from its oldest character. */
static void
loader_log_linearize(void)
{
	static const char note[] = "\n\n[... the log between the first 512 KB and the last 6.5 MB was dropped ...]\n\n";

	if (loader_log_linear) {
		return;
	}
	loader_log_linear = true;
	if (!loader_log_wrapped) {
		return;
	}
	/* the ring rotated in place so that its oldest character comes first: three reversals, no second buffer */
	loader_log_reverse(loader_log + LOADER_LOG_HEAD, loader_log_ring_at);
	loader_log_reverse(loader_log + LOADER_LOG_HEAD + loader_log_ring_at, LOADER_LOG_RING - loader_log_ring_at);
	loader_log_reverse(loader_log + LOADER_LOG_HEAD, LOADER_LOG_RING);
	/* the note takes the place of the oldest characters of the ring */
	__builtin_memcpy(loader_log + LOADER_LOG_HEAD, note, sizeof(note) - 1);
	loader_log_len = LOADER_LOG_HEAD + LOADER_LOG_RING;
}

/*
 * The live log (a USB or network reader): characters from *pos on, at most max, *pos moved on. Positions count every
 * character ever put in: the head stays as it is, the ring keeps the last LOADER_LOG_RING of the rest; a reader that
 * fell further behind than the ring (less a margin for the writer) skips ahead. No lock: a character torn by a writer
 * on another processor is the worst that can happen, and the reader must never wait for one.
 */
uint32_t
loader_log_copy(uint64_t *pos, char *out, uint32_t max)
{
	uint64_t total = os_atomic_load(&loader_log_total, relaxed);
	uint32_t n = 0;

	if (loader_log_linear) {
		return 0;               /* (the run is ending: the pieces are being put in order) */
	}
	if (*pos >= LOADER_LOG_HEAD && total > *pos && total - *pos > LOADER_LOG_RING - 65536) {
		*pos = total - (LOADER_LOG_RING - 65536);
	}
	while (n < max && *pos < total) {
		out[n++] = *pos < LOADER_LOG_HEAD ? loader_log[*pos] :
		    loader_log[LOADER_LOG_HEAD + (*pos - LOADER_LOG_HEAD) % LOADER_LOG_RING];
		(*pos)++;
	}
	return n;
}

/*
 * Patched by the loader: { magic, PA of its return stub, checkpoint to stop at, PA - VA }. See start.s. The loader finds
 * it by its magic, an aligned "XNULDRST" (and "XNULIVE!" for loader_live): no string of the kernel may hold either word.
 */
extern uint64_t loader_stub_slot[4];

bool
loader_present(void)
{
	return loader_stub_slot[1] != 0;
}

/*
 * For a loader that watches from another processor core while the kernel runs (bootcpu=N: it starts the kernel on
 * core N and stays on its own): a sign of life, and where the log is, so that the log of a kernel that hangs -- or
 * dies before it has said a word through any other channel -- is not lost with it. The loader finds this by its first
 * word ("XNULIVE!") in the kernel it has loaded. The addresses are the kernel's virtual ones, filled in by the linker
 * and so valid from the first instruction; the loader knows the distance to the physical ones. The layout is the
 * loader's struct kernel_live (xnuloader/loader.c): do not reorder.
 */
struct loader_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every HV_WDT_PET_SECONDS while the kernel is alive */
	const char *log;                /* loader_log */
	const uint32_t *log_len;        /* how much of it is in use */
	const uint32_t *log_ring_at;    /* where the ring part is being written */
	const bool *log_wrapped;        /* the ring has gone round */
	uint64_t log_head, log_ring;    /* sizes of the two parts */
	const uint64_t *recording;      /* loader_recording: { physical address, length } of a screen recording */
	volatile uint64_t stop;         /* the loader's to write: the host has asked for the run to end */
	uint64_t magic2;                /* "XNULIVE2": what follows is there (a loader may meet an older kernel) */
	const uint64_t *sound;          /* loader_sound: the same for a recording of the sound */
	uint64_t magic3;                /* "XNULIVE3": the kernel has what follows */
	volatile uint64_t usb_take;     /* ours: 1 = this kernel wants the USB controller (usb_handover=1) */
	volatile uint64_t usb_given;    /* the loader's: 1 = it no longer touches USB in this run */
	uint64_t magic4;                /* "XNULIVE4": the three fields below are there */
	volatile uint64_t vctl_pa;      /* physical address of the usbctl mailbox (0: none) */
	volatile uint64_t vctl_window_pa;
	volatile uint64_t vctl_window_len;
};

/* A screen recording / a sound recording a driver made: { physical address, bytes, kernel address }. */
uint64_t loader_recording[3];
uint64_t loader_sound[3];

struct loader_live loader_live = {
	.magic = 0x214556494c554e58ULL, .log = loader_log, .log_len = &loader_log_len, .log_ring_at = &loader_log_ring_at,
	.log_wrapped = &loader_log_wrapped, .log_head = LOADER_LOG_HEAD, .log_ring = LOADER_LOG_RING,
	.recording = loader_recording,
	.magic2 = 0x324556494c554e58ULL, .sound = loader_sound,
	.magic3 = 0x334556494c554e58ULL, .usb_take = 0, .usb_given = 0,
	.magic4 = 0x344556494c554e58ULL, .vctl_pa = 0, .vctl_window_pa = 0, .vctl_window_len = 0,
};

void
loader_live_set_vctl(uint64_t mbox_pa, uint64_t window_pa, uint64_t window_len)
{
	loader_live.vctl_window_pa = window_pa;
	loader_live.vctl_window_len = window_len;
	loader_live.vctl_pa = mbox_pa;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The USB handover (usb_handover=1): the kernel writes usb_take = 1, the loader stops touching the controller and
 * writes usb_given = 1, and only then does the kernel program it.
 */
uint64_t
loader_live_usb_take(void)
{
	return loader_live.usb_take;
}

void
loader_live_usb_take_set(uint64_t value)
{
	loader_live.usb_take = value;
}

uint64_t
loader_live_usb_given(void)
{
	return loader_live.usb_given;
}

/* The end of the run as the host's "stop" asks for it, asked from this side (a driver's command port). */
void
loader_live_stop_set(void)
{
	loader_live.stop = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * What the ports hang on the way back. Quiesce functions run on every way back, a panic's too, with interrupts off
 * and possibly nothing else of the kernel working: they put their hardware the way UEFI expects it (a DMA engine
 * stopped, a controller's list given back). Stop functions run only on the orderly ways out (a timed return, the
 * host's stop), each in a thread of its own when stop_deadline= is set. Both in the order of registration; the
 * stops before the quiesces.
 */
#define LOADER_HOOKS_MAX        32

struct loader_hook {
	const char *name;
	void (*fn)(void);
	uint32_t extra_ms;
	uint32_t flags;
};

static struct loader_hook loader_quiesces[LOADER_HOOKS_MAX];
static uint32_t loader_quiesces_count;
static struct loader_hook loader_stops_list[LOADER_HOOKS_MAX];
static uint32_t loader_stops_count;
static void (*loader_before_end[LOADER_HOOKS_MAX])(const char *why);
static uint32_t loader_before_end_count;
static bool (*loader_stop_others_fn)(void);

static int
loader_hook_add(struct loader_hook *list, uint32_t *count, const char *name, void (*fn)(void), uint32_t extra_ms,
    uint32_t flags)
{
	uint32_t index;

	if (fn == NULL) {
		return -1;
	}
	index = os_atomic_inc_orig(count, relaxed);
	if (index >= LOADER_HOOKS_MAX) {
		os_atomic_dec(count, relaxed);
		printf("loader: no room for %s (%u hooks at most)\n", name, LOADER_HOOKS_MAX);
		return -1;
	}
	list[index].name = name;
	list[index].extra_ms = extra_ms;
	list[index].flags = flags;
	os_atomic_thread_fence(release);
	list[index].fn = fn;
	return 0;
}

int
loader_quiesce_register(const char *name, void (*fn)(void))
{
	return loader_hook_add(loader_quiesces, &loader_quiesces_count, name, fn, 0, 0);
}

int
loader_stop_register(const char *name, void (*fn)(void), uint32_t extra_ms, uint32_t flags)
{
	return loader_hook_add(loader_stops_list, &loader_stops_count, name, fn, extra_ms, flags);
}

int
loader_before_end_register(void (*fn)(const char *why))
{
	uint32_t index = os_atomic_inc_orig(&loader_before_end_count, relaxed);

	if (fn == NULL || index >= LOADER_HOOKS_MAX) {
		os_atomic_dec(&loader_before_end_count, relaxed);
		return -1;
	}
	loader_before_end[index] = fn;
	return 0;
}

/*
 * The processors other than the one going back: whoever starts them (osfmk/arm64/psci_smp.c, K24) says how they are
 * stopped, and whether every one of them is confirmed off. It runs on every way back, a panic's too (the debugger's
 * cross call only parks the others where they are: they still own kernel memory and the GIC). false: the loader does
 * not get the machine back (loader_fail_closed).
 */
void
loader_set_stop_others(bool (*fn)(void))
{
	loader_stop_others_fn = fn;
}

/* Valid from the first instruction of C code: does not depend on gVirtBase/gPhysBase being set up. */
static inline uint64_t
static_kvtophys(const void *va)
{
	return (uint64_t)va + loader_stub_slot[3];
}

static inline void
loader_clean_range(uintptr_t start, uintptr_t len)
{
	for (uintptr_t p = start & ~63UL; p < start + len; p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
}

/*
 * K2b: the log written back to RAM as it is now. A phone that hangs is reset by the hypervisor's watchdog; DRAM keeps
 * what it holds over that reset, the caches do not. A recovery started after it (misc's boot control block set to
 * "boot-recovery" before the run) can then read the log out of RAM by its loader_live ("XNULIVE!"): with this done
 * every heartbeat (10 s) and after each line of the SMP way back, what was said before a hang is there. Any context,
 * no lock: a line being written meanwhile may be half there.
 */
void loader_log_sync(void);
void
loader_log_sync(void)
{
	loader_clean_range((uintptr_t)loader_log, sizeof(loader_log));
	loader_clean_range((uintptr_t)&loader_log_len, sizeof(loader_log_len));
	loader_clean_range((uintptr_t)&loader_log_ring_at, sizeof(loader_log_ring_at));
	loader_clean_range((uintptr_t)&loader_log_wrapped, sizeof(loader_log_wrapped));
	loader_clean_range((uintptr_t)&loader_live, sizeof(loader_live));
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * Jump to the loader's return stub. reason: 0 = orderly end, 2 = checkpoint. Returns only if no loader is listening.
 *
 * The stub runs at its physical address and turns the MMU off itself; until then it is reached through TTBR0, mapped
 * 1:1 here. The kernel's TTBR1 has a 16K granule, but TCR_EL1.TG0 is TTBR0's own: TTBR0 gets a 4K granule and a 39-bit
 * space (T0SZ 25), walked from one L1 table to one L2 table, whose 2 MB block holds the stub. The kernel text this
 * runs from stays where it is (TTBR1).
 */
#define LOADER_TCR_T0SZ_MASK    0x3fULL
#define LOADER_TCR_EPD0         (1ULL << 7)
#define LOADER_TCR_IRGN0_MASK   (3ULL << 8)
#define LOADER_TCR_ORGN0_MASK   (3ULL << 10)
#define LOADER_TCR_SH0_MASK     (3ULL << 12)
#define LOADER_TCR_TG0_MASK     (3ULL << 14)
#define LOADER_TCR_E0PD0        (1ULL << 55)
#define LOADER_TCR_T0_4K_39BIT  (25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12))  /* T0SZ 25, WBWA walks, inner shareable, TG0 4K */

/* 4K-granule descriptors (the same bit positions for every granule; written out so no 16K macro slips in) */
#define LOADER_TTE_TABLE        0x3ULL
#define LOADER_TTE_BLOCK        0x1ULL
#define LOADER_TTE_AF           (1ULL << 10)
#define LOADER_TTE_SH_INNER     (3ULL << 8)
#define LOADER_TTE_ATTRINDX(i)  ((uint64_t)(i) << 2)

static void
loader_jump(uint64_t reason, uint64_t code)
{
	uint64_t stub = loader_stub_slot[1];
	uint64_t tcr;
	uint32_t own_core = 0;

	if (stub == 0 || loader_return_entered) {
		return;
	}
	if ((stub >> 39) != 0) {
		return;                 /* (the loader lives below 512 GB: anything else is not its stub) */
	}
	loader_return_entered = true;

	/*
	 * With a core of its own (bootcpu=) the kernel does not go back into the loader's code on this processor: the stub
	 * leaves the log where the loader's core finds it and has the firmware switch this one off. It must go with nothing
	 * pending: the timer masked, this processor's interrupt group 1 off.
	 */
	if (PE_state.initialized && PE_parse_boot_argn("bootcpu", &own_core, sizeof(own_core))) {
		__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
		__builtin_arm_wsr64("S3_0_C12_C12_7", 0);       /* ICC_IGRPEN1_EL1 */
		__builtin_arm_isb(ISB_SY);
	}

	loader_l1[(stub >> 30) & 0x1ff] = static_kvtophys(loader_l2) | LOADER_TTE_TABLE;
	loader_l2[(stub >> 21) & 0x1ff] = (stub & ~0x1fffffULL) | LOADER_TTE_BLOCK | LOADER_TTE_AF | LOADER_TTE_SH_INNER |
	    LOADER_TTE_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);

	/*
	 * The stub turns the MMU off, and on real hardware dirty cache lines do not survive that: write the log and the
	 * tables back to RAM first.
	 */
	loader_clean_range((uintptr_t)loader_l1, sizeof(loader_l1));
	loader_clean_range((uintptr_t)loader_l2, sizeof(loader_l2));
	loader_clean_range((uintptr_t)loader_log, loader_log_len);
	/* ... and a screen recording, a sound recording, and the notes of where they are */
	loader_clean_range((uintptr_t)loader_recording[2], (uintptr_t)loader_recording[1]);
	loader_clean_range((uintptr_t)loader_recording, sizeof(loader_recording));
	loader_clean_range((uintptr_t)loader_sound[2], (uintptr_t)loader_sound[1]);
	loader_clean_range((uintptr_t)loader_sound, sizeof(loader_sound));
	__asm__ volatile ("dsb sy" ::: "memory");

	tcr = __builtin_arm_rsr64("TCR_EL1");
	tcr &= ~(LOADER_TCR_T0SZ_MASK | LOADER_TCR_EPD0 | LOADER_TCR_IRGN0_MASK | LOADER_TCR_ORGN0_MASK | LOADER_TCR_SH0_MASK |
	    LOADER_TCR_TG0_MASK | LOADER_TCR_E0PD0);
	tcr |= LOADER_TCR_T0_4K_39BIT;

	register uint64_t r_ttbr0 __asm__("x0") = static_kvtophys(loader_l1);
	register uint64_t r_log __asm__("x2") = static_kvtophys(loader_log);
	register uint64_t r_len __asm__("x3") = loader_log_len;
	register uint64_t r_tcr __asm__("x4") = tcr;
	register uint64_t r_stub __asm__("x5") = stub;
	register uint64_t r_reason __asm__("x6") = reason;
	register uint64_t r_code __asm__("x7") = code;

	__asm__ volatile (
                "msr DAIFSet, #0xf\n"
                "msr TTBR0_EL1, x0\n"
                "msr TCR_EL1, x4\n"
                "isb\n"
                "tlbi vmalle1\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, x2\n"
                "mov x1, x3\n"
                "mov x2, x6\n"
                "mov x3, x7\n"
                "br x5\n"
                : : "r" (r_ttbr0), "r" (r_log), "r" (r_len), "r" (r_tcr), "r" (r_stub), "r" (r_reason), "r" (r_code));
	__builtin_unreachable();
}

/* bsd/sys/msgbuf.h (not includable from osfmk) */
struct msgbuf {
	int   msg_magic;
	int   msg_size;
	int   msg_bufx;         /* write pointer */
	int   msg_bufr;         /* read pointer */
	char *msg_bufc;         /* buffer */
};
extern struct msgbuf *msgbufp;

static void
loader_log_puts(const char *s)
{
	loader_log_line(s);
}

/* Everything printf()/IOLog()/os_log wrote ends up in the kernel message buffer (what dmesg shows). */
static void
loader_log_append_msgbuf(void)
{
	struct msgbuf *mb = msgbufp;

	if (mb == NULL || mb->msg_bufc == NULL || mb->msg_size <= 0 || mb->msg_bufx < 0 || mb->msg_bufx > mb->msg_size) {
		return;
	}
	loader_log_puts("\n--- kernel message buffer ---\n");
	/* oldest data first: [bufx, size) is only meaningful once the buffer has wrapped */
	for (int pass = 0; pass < 2; pass++) {
		int from = pass == 0 ? mb->msg_bufx : 0;
		int to = pass == 0 ? mb->msg_size : mb->msg_bufx;
		for (int i = from; i < to; i++) {
			if (mb->msg_bufc[i] != 0) {
				loader_log_putc(mb->msg_bufc[i]);
			}
		}
	}
}

/*
 * A CPU of the kernel may still run (or one that might is not accounted for): giving the loader the memory and the
 * core back would let it run under the loader. Stay here instead, interrupts off, the log written back to RAM for a
 * loader that reads it after a reset (bootcpu=: the loader's own core notices the kernel's never comes back).
 */
static void __attribute__((noreturn))
loader_fail_closed(void)
{
	loader_log_line("\nloader: NOT going back: a processor of the kernel is not confirmed off\n");
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	loader_clean_range((uintptr_t)loader_log, loader_log_len);
	loader_clean_range((uintptr_t)&loader_log_len, sizeof(loader_log_len));
	__asm__ volatile ("dsb sy" ::: "memory");
	for (;;) {
		__builtin_arm_wfe();
	}
}

/* every other processor off, or not back at all */
static void
loader_stop_others_or_stay(void)
{
	bool (*fn)(void) = loader_stop_others_fn;

	if (fn != NULL && !fn()) {
		loader_fail_closed();
	}
}

/*
 * The end of the run: the hardware the ports registered put back the way UEFI expects it, the log and the panic
 * buffer collected, and the loader's stub. Nothing here may need interrupts, locks or printf() (a panic calls this).
 */
void
loader_return(void)
{
	if (!loader_present() || loader_return_entered) {
		return;
	}
	loader_log_puts("\nloader: the end of the run is here\n");
	loader_stop_others_or_stay();
	for (uint32_t i = 0; i < loader_quiesces_count && i < LOADER_HOOKS_MAX; i++) {
		void (*fn)(void) = loader_quiesces[i].fn;

		if (fn != NULL) {
			fn();
		}
	}
	/*
	 * A panic's text is first collected in the panic buffer and only later flushed to the console; if the console
	 * itself is what failed, that never happens. Append the buffer so the loader gets it either way.
	 */
	loader_log_linearize();
	loader_log_append_msgbuf();
	if (debug_buf_base != NULL && debug_buf_ptr > debug_buf_base &&
	    (size_t)(debug_buf_ptr - debug_buf_base) < 256 * 1024) {
		loader_log_puts("\n--- panic buffer ---\n");
		loader_log_write(debug_buf_base, (int)(debug_buf_ptr - debug_buf_base));
	}
	loader_jump(0, 0);
}

/* Bring-up aid: stop here if the loader asked for checkpoint n (payload entry "checkpoint", xnuloader). */
void
loader_checkpoint(uint64_t n)
{
	if (loader_stub_slot[2] == n) {
		char line[64];

		snprintf(line, sizeof(line), "\nloader: checkpoint %llu: back to the loader\n", n);
		loader_log_line(line);
		loader_stop_others_or_stay();
		loader_jump(2, n);
	}
}

/*
 * stop_deadline=SECS (0, the default: none): each stop function on the orderly way out runs in a thread of its own,
 * and this one waits for it SECS seconds (plus the function's own extra_ms) at most. One that has not finished by
 * then is left where it is: the log says which, and the way out goes on. stop_test_hang=1 (a test): one more step
 * that never finishes, to see the deadline taken.
 */
#define LOADER_STOP_JOBS        16

struct loader_stop_job {
	void (*fn)(void);
	const char *name;
	volatile uint32_t done;
};

static struct loader_stop_job loader_stop_jobs[LOADER_STOP_JOBS];      /* never reused: a hung one keeps its own */
static uint32_t loader_stop_jobs_used;

static void
loader_stop_job_thread(void *arg, __unused wait_result_t wr)
{
	struct loader_stop_job *job = arg;

	job->fn();
	os_atomic_store(&job->done, 1, release);
	thread_wakeup((event_t)&job->done);
}

static void
loader_stop_test_hang(void)
{
	for (;;) {
		IOSleep(60 * 1000);
	}
}

static void
loader_stop_bounded(const char *name, void (*fn)(void), uint32_t extra_ms)
{
	uint32_t seconds = 0, index;
	struct loader_stop_job *job;
	thread_t thread;
	uint64_t deadline;

	if (!PE_parse_boot_argn("stop_deadline", &seconds, sizeof(seconds)) || seconds == 0) {
		fn();
		return;
	}
	index = os_atomic_inc_orig(&loader_stop_jobs_used, relaxed);
	if (index >= LOADER_STOP_JOBS) {
		fn();
		return;
	}
	job = &loader_stop_jobs[index];
	job->fn = fn;
	job->name = name;
	if (kernel_thread_start(loader_stop_job_thread, job, &thread) != KERN_SUCCESS) {
		printf("loader: %s: no thread for it: called here, without a deadline\n", name);
		fn();
		return;
	}
	thread_deallocate(thread);
	clock_interval_to_deadline((uint64_t)seconds * 1000 + extra_ms, NSEC_PER_MSEC, &deadline);
	while (!os_atomic_load(&job->done, acquire)) {
		assert_wait_deadline((event_t)&job->done, THREAD_UNINT, deadline);
		if (os_atomic_load(&job->done, acquire)) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT && !os_atomic_load(&job->done, acquire)) {
			printf("loader: %s did NOT finish in %u ms (stop_deadline=%u): left as it is, the way out goes on\n",
			    name, seconds * 1000 + extra_ms, seconds);
			return;
		}
	}
}

/* What the orderly ways out stop while there is a thread to wait in (timed: loader_return_after's way). */
static void
loader_stops(bool timed)
{
	uint32_t hang = 0;

	if (PE_parse_boot_argn("stop_test_hang", &hang, sizeof(hang)) && hang == 1) {
		printf("loader: stop_test_hang=1: a step that never finishes (a test of stop_deadline)\n");
		loader_stop_bounded("stop_test_hang", loader_stop_test_hang, 0);
	}
	for (uint32_t i = 0; i < loader_stops_count && i < LOADER_HOOKS_MAX; i++) {
		struct loader_hook *h = &loader_stops_list[i];
		void (*fn)(void) = h->fn;

		if (fn == NULL || ((h->flags & LOADER_STOP_TIMED_ONLY) && !timed)) {
			continue;
		}
		loader_stop_bounded(h->name, fn, h->extra_ms);
	}
}

/*
 * loader_return_after=<seconds>: hand control back to the loader after that long, whatever the kernel is doing. This
 * is how the log gets out when the kernel does not panic but simply waits (e.g. for a root device). return_panic=1
 * makes a panic of it (a test of the panic's way back).
 */
static void
loader_return_timer(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint32_t as_panic = 0;

	loader_log_puts("\n--- loader_return_after expired ---\n");
	PE_parse_boot_argn("return_panic", &as_panic, sizeof(as_panic));
	if (as_panic) {
		panic("return_panic: the panic that was asked for, on cpu %d", cpu_number());
	}
	loader_stops(true);
	loader_return();
}

/*
 * The ports that want to know the end is near (a data volume saved while the run is still whole) are told this long
 * before loader_return_after is up: var_save_ask=SECONDS (default 60, 0: not before the end).
 */
#define LOADER_BEFORE_END_LEAD_SECONDS  60

static void
loader_before_end_timer(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	for (uint32_t i = 0; i < loader_before_end_count && i < LOADER_HOOKS_MAX; i++) {
		void (*fn)(const char *) = loader_before_end[i];

		if (fn != NULL) {
			fn("the run's time is nearly up");
		}
	}
}

__startup_func
static void
loader_return_timer_init(void)
{
	uint32_t seconds = 0, lead = LOADER_BEFORE_END_LEAD_SECONDS;
	uint64_t deadline;

	if (!PE_parse_boot_argn("loader_return_after", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	thread_call_t call = thread_call_allocate(loader_return_timer, NULL);
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
	PE_parse_boot_argn("var_save_ask", &lead, sizeof(lead));
	if (lead != 0 && seconds > lead + 20) {
		thread_call_t ask = thread_call_allocate(loader_before_end_timer, NULL);

		clock_interval_to_deadline(seconds - lead, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(ask, deadline);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_return_timer_init);

/*
 * A loader that watches from another core also listens to the host, and passes on a request to end the run ("xl
 * stop") by setting loader_live.stop. Look once a second.
 */
static thread_call_t loader_stop_call;

static void
loader_stop_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (loader_live.stop) {
		loader_log_puts("\n--- the host asked for the end of the run: back to the loader ---\n");
		loader_stops(false);
		loader_return();
	}
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(loader_stop_call, deadline);
}

__startup_func
static void
loader_stop_poll_init(void)
{
	if (!loader_present()) {
		return;
	}
	loader_stop_call = thread_call_allocate(loader_stop_poll, NULL);
	loader_stop_poll(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_stop_poll_init);

/*
 * hv_wdt_pet=1: the loader has armed the Qualcomm hypervisor's virtual watchdog (it resets the phone when a kernel
 * hangs, so that test runs need nobody at the device). Its timeout is at most 65 seconds; feed it from a timer so that
 * a healthy kernel can run for as long as it likes. The same timer is the heartbeat a watching loader looks at.
 * hb_test_stop=SECS (a test): the heartbeat and the feed stop that far into the run, as in a kernel that hung.
 */
#define VIRT_WDT_PET            0x86000007u
#define HV_WDT_PET_SECONDS      10
#define MEMORY_REPORT_BEATS     6               /* a line about memory every so many heartbeats */

static thread_call_t hv_wdt_call;
static uint32_t hv_wdt_enabled;
static uint32_t hb_test_stop;

extern unsigned int vm_page_free_count, vm_page_wire_count, vm_page_pageable_external_count,
    vm_page_pageable_internal_count;
extern uint32_t memorystatus_available_pages;
extern uint32_t c_segment_pages_compressed;

/* Where the memory is, in megabytes: for reading a session's log afterwards. */
void
hakios_memory_line(char *buf, size_t len)
{
#define MB(pages)       ((unsigned int)(((uint64_t)(pages) * PAGE_SIZE) >> 20))
	snprintf(buf, len, "memory: free %u, files %u, programs %u, wired %u, compressor holding %u MB; available %u MB\n",
	    MB(vm_page_free_count), MB(vm_page_pageable_external_count), MB(vm_page_pageable_internal_count),
	    MB(vm_page_wire_count), MB(c_segment_pages_compressed), MB(memorystatus_available_pages));
#undef MB
}

static void
hv_wdt_pet(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (hv_wdt_enabled) {
		register uint64_t x0 __asm__("x0") = VIRT_WDT_PET;
		register uint64_t x1 __asm__("x1") = 0;
		register uint64_t x2 __asm__("x2") = 0;
		register uint64_t x3 __asm__("x3") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	if (++loader_live.heartbeat % MEMORY_REPORT_BEATS == 0) {
		char line[256];

		hakios_memory_line(line, sizeof(line));
		printf("%s", line);
	}
	{
		/* K2b: the arm64e traps per CPU (sleh.c), when they moved; then the log back to RAM */
		extern void vayu_pacemu_beat(uint64_t beat);
		vayu_pacemu_beat(loader_live.heartbeat);
		loader_log_sync();
	}
	if (hb_test_stop != 0 && loader_live.heartbeat * HV_WDT_PET_SECONDS >= hb_test_stop) {
		printf("hb_test_stop=%u: the heartbeat stops here (beat %llu): a watching loader calls the kernel hung "
		    "in 50 s\n", hb_test_stop, loader_live.heartbeat);
		return;
	}
	clock_interval_to_deadline(HV_WDT_PET_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(hv_wdt_call, deadline);
}

__startup_func
static void
hv_wdt_pet_init(void)
{
	PE_parse_boot_argn("hv_wdt_pet", &hv_wdt_enabled, sizeof(hv_wdt_enabled));
	PE_parse_boot_argn("hb_test_stop", &hb_test_stop, sizeof(hb_test_stop));
	if (!loader_present()) {
		hv_wdt_enabled = 0;     /* (no loader armed a watchdog: QEMU without ours, another boot path) */
		return;
	}
	hv_wdt_call = thread_call_allocate(hv_wdt_pet, NULL);
	hv_wdt_pet(NULL, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hv_wdt_pet_init);

/* What this kernel says about the loader once the console is up: is anybody listening. */
__startup_func
static void
loader_return_announce(void)
{
	if (loader_present()) {
		printf("VAYU: the loader is listening (return stub at 0x%llx, checkpoint %llu); log 0x%llx, %u bytes so far\n",
		    loader_stub_slot[1], loader_stub_slot[2], static_kvtophys(loader_log), loader_log_len);
	} else {
		printf("VAYU: no loader patched the return slot (start.s): panics and halts stay here\n");
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, loader_return_announce);

#endif /* defined(ARM64_BOARD_CONFIG_VAYU) */
