/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "identity.h"         /* who iOS shall think we are: from the iPhone's own device tree (mkidentity.py) */

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

#define PAYLOAD_BASE        0xF0000000ULL
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (PAYLOAD_BASE + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x04000000ULL         /* RAM reserved for the payload in the UEFI memory map */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608
#define FULL_CMD_LEN 4096      /* the whole command line, for /chosen hakios-bootargs (loader 1.9) */

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			return bio;
		}
	}
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
/*
 * The staged log: 9 MB from UEFI on first use (loader 1.6: the kernel's log is 8 MB since 2026-10-25), else this 2 MB.
 */
static char out_buf_static[2 * 1024 * 1024];
static char *out_buf = out_buf_static;
static uint64_t out_cap = sizeof(out_buf_static);
#define OUT_BUF_BIG     (9ULL * 1024 * 1024)

static void
out_buf_grow(void)
{
	static int tried;
	uint64_t pa = 0;

	if (tried) {
		return;
	}
	tried = 1;
	if (!EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, OUT_BUF_BIG >> 12, &pa))) {
		out_buf = (char *)(uintptr_t)pa;
		out_cap = OUT_BUF_BIG;
		memset(out_buf, 0, out_cap);
	}
}
/* the storage's reads for the kernel land here (block_poll); also where the log is read back */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf_static;   /* what "upload" sends: normally the log in out_buf (set by compose_logs) */
static uint64_t staged_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len, was = staged_len;

	out_buf_grow();
	staged_data = out_buf;
	/* (what the last staging left is cleared: the partition gets whole blocks, past the end too) */
	memset(out_buf, 0, was + 65536 < out_cap ? was + 65536 : out_cap);
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > out_cap - n - 8192) {
			klog_len = out_cap - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

/*
 * The log the last kernel returned: "oem log" between runs stages it again instead of the loader's
 * log alone. Before, an "oem log" from xl's follow loop (XL_FOLLOW) that came just after the kernel's
 * return replaced the staged loader + kernel log with the loader's ~8 KB, and xl then fetched that as
 * the run's log (runs 175145 and 204935: 1441823 bytes returned, 8 KB fetched).
 */
static const char *returned_klog;
static uint64_t returned_klog_len;

/*
 * The log of the run before this boot (loader 1.8). A run whose log reached only logdump -- the 120 s idle reset after
 * a return the host never fetched, a watchdog, a hard reset -- lost it here: the first thing a freshly started loader
 * wrote to logdump was its own few KB (test run, 2026-10-25: a 339-byte log, "log: 4096 bytes written to logdump").
 * Now, until a kernel is started in this session, logdump is read first and, if it holds a kernel's log, left as it
 * is and kept in memory as well: "oem prevlog" stages it for get_staged.
 */
static char *prev_log;
static uint64_t prev_log_len;
static int prev_log_checked, kernel_started_this_session, logdump_force;  /* force: oem fbtouch writes regardless */

static void
prev_log_check(efi_block_io *bio)
{
	static const char mark[] = "== KERNEL LOG";
	uint64_t bs, blocks, pa = 0, cap = 9ULL * 1024 * 1024, n;

	prev_log_checked = 1;
	if (bio == NULL || kernel_started_this_session) {
		return;
	}
	bs = bio->media->block_size;
	blocks = cap / bs;
	if ((bio->media->last_block + 1) < blocks) {
		blocks = bio->media->last_block + 1;
	}
	if (EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (blocks * bs + 0xfff) >> 12, &pa)) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, blocks * bs, (void *)(uintptr_t)pa))) {
		return;
	}
	for (n = 0; n < blocks * bs && ((char *)(uintptr_t)pa)[n] != 0; n++) {
	}
	for (uint64_t i = 0; i + sizeof(mark) - 1 <= n; i++) {
		if (memcmp((char *)(uintptr_t)pa + i, mark, sizeof(mark) - 1) == 0) {
			prev_log = (char *)(uintptr_t)pa;
			prev_log_len = n;
			print("log: logdump holds the last run's log (%d bytes, a kernel's among it): kept, \"oem prevlog\" stages it\n", n);
			return;
		}
	}
	BS->free_pages(pa, (blocks * bs + 0xfff) >> 12);
}

static void
write_logs(const char *klog, uint64_t klog_len)
{
	returned_klog = klog;
	returned_klog_len = klog_len;
	efi_block_io *bio = find_log_partition();

	if (!prev_log_checked) {
		prev_log_check(bio);
	}
	compose_logs(klog, klog_len);
	if (prev_log != NULL && !kernel_started_this_session && !logdump_force) {
		return;                 /* (logdump keeps the last run's log until a kernel runs in this session) */
	}
	if (bio == NULL) {
		/* No log partition (QEMU): echo the returned kernel log on the console instead. */
		if (klog != NULL) {
			print("\n== KERNEL LOG ==\n");
			for (uint64_t i = 0; i < klog_len; i++) {
				put_char(klog[i]);
			}
			print("\n== END OF LOG ==\n");
		}
		return;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;

	if (len > (bio->media->last_block + 1) * bs) {
		len = (bio->media->last_block + 1) * bs;        /* (a partition smaller than the log: its start only) */
	}
	efi_status ws, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to logdump: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(block_buffer)) {
			rs = bio->read_blocks(bio, bio->media->media_id, 0, len, block_buffer);
			same = !EFI_ERROR(rs) && memcmp(block_buffer, out_buf, len) == 0;
		}
	}
	print("log: %d bytes written to logdump (%x), read back %s (%x)\n", len, ws,
	    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/*
 * Properties copied from the iPhone's device tree (identity.h), but one: the iPhone X's display is a
 * wide-gamut one ("artwork-display-gamut" P3), and with it iOS takes the screen for one that shows
 * extended colour (MobileGestalt's HasExtendedColorDisplay): WebKit then draws a page's tiles into
 * surfaces of ten bits a colour ('w30r', 'b3a8'), which neither the kernel's surfaces nor the window
 * server's renderer here do (eight bits, BGRA), and every page comes out black. Here the screen is
 * what the framebuffer is: sRGB.
 */
static int gamut_p3;    /* boot-arg gamut=p3 (a test): the iPhone's own "P3" left in place */

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut") && !gamut_p3) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* One property of those copied from the iPhone's device tree, by name. */
static void
dt_identity_one(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, name)) {
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
		}
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

/*
 * The port's own partition on a device, "darwin": the one place a write may reach (bsd/dev/hakios.ufs.c,
 * tools/darwin-write.py). Everything about it is checked against this before anything is written; a
 * device without one (none set up yet) gets no write at all. The kernel is handed the same numbers on its
 * command line (ufs_darwin_first=, ufs_darwin_blocks=, ufs_write_first=) and checks the loader's table
 * against them.
 */
struct darwin_identity {
	uint64_t first_lba, last_lba;    /* on its unit, as the GPT has them; blocks of 4096 */
	uint64_t write_first;            /* its first block a write may reach: past what the build writes there */
	uint16_t unit;                   /* the unit it is on, by UEFI's order */
	efi_guid type, uniq;             /* its GPT type and unique GUIDs */
};

/* a partition there is a reference image of, and how many of its bytes the image has (ufs=1, its hash) */
struct ufs_reference {
	const char *name;
	uint64_t bytes;
};

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int hypervisor_wdt;              /* the Qualcomm hypervisor's virtual watchdog is behind SMC */
	int charger;                     /* the UEFI charger stack can be queried on this platform */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* fallback identification: the framebuffer the firmware reports (GOP) */
	const struct darwin_identity *darwin;      /* the port's partition; NULL: none, nothing is ever written */
	const struct ufs_reference *ufs_refs;      /* ufs=1: the partitions hashed, and how far */
	unsigned ufs_nrefs;
};

/*
 * redwood: darwin is the port's own 20 GiB at the end of logical unit 0 (README, "The port's partition");
 * what the build puts there from the computer (tools/darwin-write.py) is the iOS volume at its start and
 * the two cryptexes behind it, so writes begin past both. The references are firmware-update/ of the
 * ROM's fw.zip (tools/ufs-survey.py).
 */
static const struct darwin_identity darwin_redwood = {
	.first_lba = 25374720, .last_lba = 30617594, .write_first = 2042368, .unit = 0,
	.type = { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } },
	.uniq = { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } },
};
static const struct ufs_reference ufs_refs_redwood[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};
/*
 * vayu: darwin is entry 33 of unit 0 (sda33), 24 GiB at the end of the unit, with the GPT entry
 * parsed from the phone's own partition table (type APFS, unique 1c213492-90d4-4921-869e-98ec2fa0497c,
 * LBA 55871232..62162938). Nothing is written from the computer there yet; writes begin at block 2097152
 * (8 GiB), where var_store's two slots are (userland/src/var_store.c STORE_AT), so the first 8 GiB stay
 * free for an iOS volume on flash later. The references are whole partitions of a read-only backup of the
 * phone's own Android; they are hashed with FNV-1a for comparison (ufs=1).
 */
static const struct darwin_identity darwin_vayu = {
	.first_lba = 55871232, .last_lba = 62162938, .write_first = 2097152, .unit = 0,
	.type = { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } },
	.uniq = { 0x1c213492, 0x90d4, 0x4921, { 0x86, 0x9e, 0x98, 0xec, 0x2f, 0xa0, 0x49, 0x7c } },
};
static const struct ufs_reference ufs_refs_vayu[] = {
	{ "bluetooth", 1048576 }, { "abl", 2097152 }, { "aop", 524288 }, { "xbl", 7340032 },
	{ "tz", 4194304 }, { "hyp", 8388608 }, { "dsp", 67108864 }, { "modem", 335544320 },
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1, .hypervisor_wdt = 1,
	.charger = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
	.darwin = &darwin_redwood,
	.ufs_refs = ufs_refs_redwood, .ufs_nrefs = sizeof(ufs_refs_redwood) / sizeof(ufs_refs_redwood[0]),
};

static const struct platform platform_vayu = {
	/* POCO X3 Pro (SM8150-AC / Snapdragon 860). Values from vayu.dtb + vayuPkg MemoryMapLib. */
	.name = "vayu", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1, .hypervisor_wdt = 1,
	.charger = 1,
	.cores = 8, .core_shift = 8,
	/* Cores 0-3 are Cortex-A55s; 4-7 are Cortex-A76s. */
	.small_cores = 0x0f,
	/* FACT: on the phone the loader printed "display 1080 x 2400 at 9c000000"; fallback only. */
	.fb_base = 0x9C000000,
	.darwin = &darwin_vayu,
	.ufs_refs = ufs_refs_vayu, .ufs_nrefs = sizeof(ufs_refs_vayu) / sizeof(ufs_refs_vayu[0]),
};

/*
 * The devices the loader knows. The image normally names one in its payload; the framebuffer base is the
 * fallback for images built before that entry existed. A port to another device adds its description
 * here (devices/poco-x3-pro/README.md lists where each value of vayu's came from);
 * a display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_vayu,
};

/* The image can name its device in the payload ("platform=vayu"), so a wrong
 * or changed framebuffer base cannot silently fall back to QEMU's platform. */
static const struct platform *
platform_by_name(const char *name, uint32_t len)
{
	if (len != 0 && name[len - 1] == '\0') {
		len--;
	}
	for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
		const char *candidate = platforms[i]->name;

		if (str_len(candidate) == len && memcmp(candidate, name, len) == 0) {
			return platforms[i];
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        "  msr daifset, #0xf\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
	uint64_t magic3;                /* "XNULIVE3": the kernel has what follows (USB handover, USB networking) */
	volatile uint64_t usb_take;     /* the kernel writes 1: it wants the USB controller (usb_handover=1) */
	volatile uint64_t usb_given;    /* this loader writes 1: it has stopped touching USB for this run */
};

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;
/*
 * loader_charger=0 on the payload command line: no charger polls while that kernel runs. The
 * 3D stream's loader (charger only on redwood) serves live logs during vayu runs; this branch's
 * (charger on vayu too, from vayu-battery-buttons) did not in runs 165651..T1'. HYPOTHESIS: the
 * UEFI charger calls every 5 s starve the USB side; this switch tests it.
 */
static int charger_paused;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC3      0x334556494c554e58ULL   /* "XNULIVE3" */
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */

static void wdt_pet(void);
static int wdt_armed;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / READ_SYSREG(CNTFRQ_EL0);
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	uint64_t n, room;

	compose_logs(NULL, 0);
	room = out_cap - 8192;
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n + take > room) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);

/* A voltage-only fallback is deliberately conservative; the real QG SOC wins whenever available. */
static int
soc_from_voltage(uint32_t mv)
{
	if (mv <= 3300) return 0;
	if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
	if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
	if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
	if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
	return 100;
}

/* Read-only status query.  No TakeAction call and no charger-limit writes are made here. */
static void
charger_poll(void)
{
	struct qcom_battery_status status;
	uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
	uint8_t present = 0, plugged = 0;
	efi_status s;
	int have_soc = 0, have_voltage = 0;

	if (!charger_enabled) {
		return;
	}
	memset(&status, 0, sizeof(status));

	charger_shared.sequence++;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (CHARGER_EX != NULL) {
		s = CHARGER_EX->get_battery_presence(&present);
		if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
		s = CHARGER_EX->get_charger_presence(&plugged);
		if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
		s = CHARGER_EX->get_battery_voltage(&voltage);
		if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
			have_voltage = 1;
			flags |= LOADER_BATTERY_VOLTAGE_VALID;
			charger_shared.voltage_mv = (int32_t)voltage;
		}
	}
	if (QCOM_CHARGER != NULL) {
		s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
		if (!EFI_ERROR(s)) {
			charger_shared.charger_action = action;
			if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
				charger_shared.voltage_mv = status.battery_voltage_mv;
				have_voltage = 1;
				flags |= LOADER_BATTERY_VOLTAGE_VALID;
			}
			/*
			 * GetChargingAction returns the firmware's state of charge in word [2] of its
			 * output (confirmed by dumping the raw buffer on this device). We use it as-is
			 * EXCEPT when the fuel gauge is clearly not ready yet: right after initialize()
			 * it can report a near-empty SOC while the cell voltage says otherwise, which is
			 * where the old "shows 1% on a half-full battery" came from. A Li-ion cell above
			 * ~3.6 V is never under 5%, so in that case we distrust the gauge and fall back to
			 * the voltage estimate below until it settles.
			 */
			if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
				int vmv = have_voltage ? charger_shared.voltage_mv : 0;
				if (status.state_of_charge < 5 && vmv > 3600) {
					/* fuel gauge not settled; leave it to the voltage fallback */
				} else {
					charger_shared.soc_percent = status.state_of_charge;
					have_soc = 1;
					flags |= LOADER_BATTERY_VALID;
				}
			}
			charger_shared.current_ma = status.charge_current_ma;
			charger_shared.temperature_c = status.battery_temperature_c;
		}
	}
	if (!have_soc && have_voltage) {
		charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
		flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
	}
	if (plugged && charger_shared.soc_percent < 100) {
		flags |= LOADER_BATTERY_CHARGING;
	}
	charger_shared.flags = flags;
	charger_shared.polls++;
	if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
	__asm__ volatile ("dmb ish" ::: "memory");
	charger_shared.sequence++;
	clean_dcache((uint64_t)(uintptr_t)&charger_shared,
	    (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}

/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; PLAT->ufs_refs != NULL && r < PLAT->ufs_nrefs; r++) {
		const char *a = PLAT->ufs_refs[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / READ_SYSREG(CNTFRQ_EL0));
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, PLAT->ufs_refs[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}

/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * bsd/dev/hakios.ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the iOS volume or the cryptexes. The
 * refusals are here, in the kernel (bsd/dev/hakios.ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the iOS volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of iOS lives there
 * and a write is refused before that block. The identity is the whole of darwin-write.py's: the
 * same partition, named the same, of the same size, in the same place, on logical unit 0.
 */
/* (the identity itself is the platform's: struct darwin_identity, PLAT->darwin) */

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start() */
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
#define HC_PA              0x1d84000ULL        /* ufshc@1d84000, "ufs_mem", in this phone's Android device tree */
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x800
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	const struct darwin_identity *d = PLAT->darwin;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (d == NULL) {
		why = "this device has no darwin set up in the loader (struct darwin_identity)";
	} else if (memcmp(&info->type_guid, &d->type, sizeof(d->type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (memcmp(&info->unique_guid, &d->uniq, sizeof(d->uniq)) != 0) {
		why = "its unique GUID is not the one it had";
	} else if (info->start_lba != d->first_lba || info->end_lba != d->last_lba) {
		why = "it does not begin and end where it did";
	} else if (bp->unit != d->unit) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on its logical unit";
	} else if (bp->block_size != 4096 || bp->blocks != d->last_lba - d->first_lba + 1) {
		why = "its blocks are not what they were";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit %d): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, (uint64_t)d->unit, d->write_first);
	return 1;
}

static void
block_start(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE ||
			    EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = dp;
			units[nunits].path_bytes = device_path_bytes(dp);
			units[nunits].lun = device_path_print(nunits, dp);
			nunits++;
		}
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information: the block service is off\n");
		return;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
		if (block_write_requested && block_is_darwin(info, bio, bp)) {
			block_write_part = (int)block_shared.parts;
		}
		block_bios[block_shared.parts++] = bio;
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = PLAT->darwin->write_first;   /* block_is_darwin() said yes: there is one */
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits, nunits, (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	if (part < block_shared.parts && count != 0 && count <= BLOCK_BUFFER_SIZE / block_shared.part[part].block_size &&
	    lba < block_shared.part[part].blocks && count <= block_shared.part[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_shared.part[part].block_size;
		if (!hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if (op == BLOCK_OP_WRITE) {
			if (block_write_part < 0 || PLAT->darwin == NULL || part != (uint32_t)block_write_part ||
			    lba < PLAT->darwin->write_first) {
				/* not darwin, or not past what the build writes from the computer: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of %s: REFUSED\n", count, lba,
					    block_shared.part[part].name);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					bio->flush_blocks(bio);
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of darwin read back DIFFERENT\n",
						    count, lba);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
			/* what the controller wrote is in memory; drop whatever this core held of the buffer */
			flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}

static void
charger_start(void)
{
	static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
	static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
	efi_status ex_status, qcom_status, init_status = 0;

	if (charger_enabled || !PLAT->charger) {
		return;
	}
	ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
	qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
	if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
		/* Re-open the driver's status path after QcomChargerApp handed off to the OS. */
		init_status = QCOM_CHARGER->initialize(1);
	}
	charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
	    (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
	print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
	    (uint64_t)(uintptr_t)&charger_shared);
	charger_poll();
	print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
	    (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
	    (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
	    (uint64_t)charger_shared.charger_action);
}

/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	kernel_started_this_session = 1;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	/* the last argument arrives in that core's x0 */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	if (PLAT->log_other_cpu) {
		flush_log();
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			watching_only = 1;
			daif = READ_SYSREG(DAIF);
			__asm__ volatile ("msr daifset, #0xf");
			group1 = READ_SYSREG(S3_0_C12_C12_7);          /* ICC_IGRPEN1_EL1 */
			__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && !charger_paused && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		if (block_enabled) {
			block_poll();
		}
		usb_poll_command();
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
	running_live = NULL;

	if (watching_only) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (!PLAT->hypervisor_wdt) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

/* ------------------------------------------------------------------------ */
/* vayu audio: the speaker amplifiers' pins, for XNU's bsd/dev/vayu_audio.c. */
/*                                                                          */
/* This port's rule: XNU does not write TLMM, the loader prepares the pins.   */
/* Two TI amplifiers on QUP0 SE7's bus (GPIO 98 data, 99 clock, function    */
/* qup7 = 1): TAS2562 at 0x4c (SDZ GPIO 92, IRQ 91) and TAS2564 at 0x4d     */
/* (SDZ 89, IRQ 90); SDZ is their active-low hardware shutdown. 89-92 are   */
/* in the NORTH tile, 98/99 in the SOUTH one (pinctrl-sm8150.c), registers  */
/* +0x1000 * pin. Only when the payload's command line asks:                */
/*   vayu_audio_pins=1  SDZ 89/92 driven high, pull kept (as the driver's   */
/*                      tas256x_hw_reset() and gpio_direction_output), 20   */
/*                      ms: the chips leave hardware shutdown for software  */
/*                      shutdown, no output stage; XNU may then talk to them */
/*   vayu_audio_pins=2  SDZ 89/92 driven low: both back in hardware shutdown */
/*   vayu_audio_mi2s=1  MI2S 137-140 to qua_mi2s (dt active state); =2 sleep */
/*   vayu_audio_ids=1   (with =1) the chip IDs read here, I2C by hand on    */
/*                      98/99 (taken as GPIOs and given back as found):     */
/*                      page 0x00, book 0x7f, ID 0x7d (0x10/0x20 TAS2562,   */
/*                      0x00 TAS2564). No amplifier register is written.    */
/* Every write is logged with its readback.                                 */
/* ------------------------------------------------------------------------ */

#define AUD_TLMM_NORTH  0x03900000ULL
#define AUD_TLMM_SOUTH  0x03D00000ULL
#define AUD_SDZ_LEFT    89      /* 0x4d */
#define AUD_SDZ_RIGHT   92      /* 0x4c */
#define AUD_SDA         98
#define AUD_SCL         99
#define AUD_CFG_OE      (1u << 9)

static uint64_t
aud_cfg(unsigned pin)
{
	return (pin >= 89 && pin <= 92 ? AUD_TLMM_NORTH : AUD_TLMM_SOUTH) + 0x1000ULL * pin;
}

static inline uint32_t
aud_rd(uint64_t pa)
{
	return *(volatile uint32_t *)(uintptr_t)pa;
}

static inline void
aud_wr(uint64_t pa, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)pa = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
aud_spin_us(uint64_t us)
{
	uint64_t frq = READ_SYSREG(CNTFRQ_EL0), start = READ_SYSREG(CNTPCT_EL0);

	while (READ_SYSREG(CNTPCT_EL0) - start < us * frq / 1000000) {
	}
}

static int
aud_word(const char *cmdline, const char *word)
{
	for (const char *p = cmdline; *p; p++) {
		const char *q = p, *w = word;

		if (p != cmdline && p[-1] != ' ') {
			continue;
		}
		while (*w && *q == *w) {
			q++, w++;
		}
		if (*w == 0 && (*q == 0 || *q == ' ')) {
			return 1;
		}
	}
	return 0;
}

static void
aud_show(const char *when, unsigned pin)
{
	uint32_t cfg = aud_rd(aud_cfg(pin)), io = aud_rd(aud_cfg(pin) + 4);

	print("vayu-audio: %s gpio%d cfg %x io %x (function %d, pull %d, oe %d, reads %d)\n", when, (uint64_t)pin, (uint64_t)cfg,
	    (uint64_t)io, (uint64_t)((cfg >> 2) & 0xf), (uint64_t)(cfg & 3), (uint64_t)((cfg >> 9) & 1), (uint64_t)(io & 1));
}

/* SDZ to a level: output enabled, everything else of the cfg kept. */
static void
aud_sdz(unsigned pin, int high)
{
	uint32_t cfg = aud_rd(aud_cfg(pin));

	aud_wr(aud_cfg(pin) + 4, high ? 2 : 0);
	aud_wr(aud_cfg(pin), cfg | AUD_CFG_OE);
	print("vayu-audio: SDZ gpio%d -> %s: cfg %x (was %x), io %x\n", (uint64_t)pin, high ? "high" : "low",
	    (uint64_t)aud_rd(aud_cfg(pin)), (uint64_t)cfg, (uint64_t)aud_rd(aud_cfg(pin) + 4));
}

/* I2C by hand (bsd/dev/hakios.speaker.c): a line pulled low or let go to the pull-ups. */
static void
aud_line(unsigned pin, int high)
{
	aud_wr(aud_cfg(pin) + 4, 0);
	aud_wr(aud_cfg(pin), 3 | (high ? 0 : AUD_CFG_OE));
}

static int
aud_high(unsigned pin)
{
	return aud_rd(aud_cfg(pin) + 4) & 1;
}

static int
aud_bit(int out)
{
	int in;

	aud_line(AUD_SDA, out);
	aud_spin_us(5);
	aud_line(AUD_SCL, 1);
	for (int stretched = 0; !aud_high(AUD_SCL) && stretched < 100; stretched++) {
		aud_spin_us(5);
	}
	aud_spin_us(5);
	in = aud_high(AUD_SDA);
	aud_line(AUD_SCL, 0);
	return in;
}

static int
aud_byte_out(uint8_t b)
{
	for (int bit = 7; bit >= 0; bit--) {
		aud_bit((b >> bit) & 1);
	}
	return !aud_bit(1);
}

static void
aud_start(void)
{
	aud_line(AUD_SDA, 1);
	aud_line(AUD_SCL, 1);
	aud_spin_us(5);
	aud_line(AUD_SDA, 0);
	aud_spin_us(5);
	aud_line(AUD_SCL, 0);
	aud_spin_us(5);
}

static void
aud_stop(void)
{
	aud_line(AUD_SDA, 0);
	aud_spin_us(5);
	aud_line(AUD_SCL, 1);
	aud_spin_us(5);
	aud_line(AUD_SDA, 1);
	aud_spin_us(5);
}

/* One register read; -1: no answer, -2: the bus not at rest. The two pins are given back as found. */
static int
aud_read(uint8_t address, uint8_t reg)
{
	uint32_t cfg[2] = { aud_rd(aud_cfg(AUD_SDA)), aud_rd(aud_cfg(AUD_SCL)) };
	uint32_t io[2] = { aud_rd(aud_cfg(AUD_SDA) + 4), aud_rd(aud_cfg(AUD_SCL) + 4) };
	int value = -1;

	aud_line(AUD_SDA, 1);
	aud_line(AUD_SCL, 1);
	aud_spin_us(5);
	if (!aud_high(AUD_SDA) || !aud_high(AUD_SCL)) {
		value = -2;
	} else {
		aud_start();
		if (aud_byte_out((uint8_t)(address << 1)) && aud_byte_out(reg)) {
			aud_start();
			if (aud_byte_out((uint8_t)(address << 1 | 1))) {
				value = 0;
				for (int bit = 7; bit >= 0; bit--) {
					value = value << 1 | aud_bit(1);
				}
				aud_bit(1);     /* no acknowledge: the last byte */
			}
		}
		aud_stop();
	}
	aud_wr(aud_cfg(AUD_SDA) + 4, io[0] & 2);
	aud_wr(aud_cfg(AUD_SCL) + 4, io[1] & 2);
	aud_wr(aud_cfg(AUD_SDA), cfg[0]);
	aud_wr(aud_cfg(AUD_SCL), cfg[1]);
	return value;
}

/*
 * The quaternary MI2S pins (pinctrl-sm8150.c: 137-140 SOUTH, qua_mi2s = function 1): vayu_audio_mi2s=1
 * as the phone's device tree's quat_mi2s_active / _sd0_active / _sd1_active (137 bit clock and 138 word
 * select 8 mA, no pull, output-high; 139 data out and 140 data in 8 mA, no pull); =2 as its *_sleep
 * states (GPIO, pull-down, 2 mA, input). The DSP drives them once its port runs.
 */
static void
aud_mi2s(int on)
{
	for (unsigned pin = 137; pin <= 140; pin++) {
		uint32_t cfg = aud_rd(aud_cfg(pin)), want;

		if (on) {
			want = (1u << 2) | (3u << 6) | (pin <= 138 ? AUD_CFG_OE : 0);
			if (pin <= 138) {
				aud_wr(aud_cfg(pin) + 4, 2);
			}
		} else {
			want = 1u;              /* function 0, pull-down, 2 mA, input */
		}
		aud_wr(aud_cfg(pin), want);
		print("vayu-audio: MI2S gpio%d -> cfg %x (was %x), io %x\n", (uint64_t)pin, (uint64_t)aud_rd(aud_cfg(pin)),
		    (uint64_t)cfg, (uint64_t)aud_rd(aud_cfg(pin) + 4));
	}
}

static void
vayu_audio_pins(const char *cmdline)
{
	static const unsigned show[] = { AUD_SDZ_LEFT, 90, 91, AUD_SDZ_RIGHT, AUD_SDA, AUD_SCL };
	static const uint8_t address[2] = { 0x4c, 0x4d };

	for (unsigned i = 0; i < sizeof(show) / sizeof(show[0]); i++) {
		aud_show("before", show[i]);
	}
	if (aud_word(cmdline, "vayu_audio_mi2s=1") || aud_word(cmdline, "vayu_audio_mi2s=2")) {
		aud_mi2s(aud_word(cmdline, "vayu_audio_mi2s=1"));
	}
	if (!aud_word(cmdline, "vayu_audio_pins=1") && !aud_word(cmdline, "vayu_audio_pins=2")) {
		return;
	}
	if (aud_word(cmdline, "vayu_audio_pins=2")) {
		aud_sdz(AUD_SDZ_RIGHT, 0);
		aud_sdz(AUD_SDZ_LEFT, 0);
		return;
	}
	aud_sdz(AUD_SDZ_RIGHT, 1);
	aud_sdz(AUD_SDZ_LEFT, 1);
	aud_spin_us(20000);             /* (tas256x_hw_reset(): 20 ms before the first transfer) */
	aud_show("after 20 ms", AUD_SDZ_RIGHT);
	aud_show("after 20 ms", AUD_SDZ_LEFT);
	if (!aud_word(cmdline, "vayu_audio_ids=1")) {
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		int page = aud_read(address[i], 0x00), book = page >= 0 ? aud_read(address[i], 0x7f) : page;
		int id = page == 0 && book == 0 ? aud_read(address[i], 0x7d) : -3;

		print("vayu-audio: %x: page %x book %x id %x (%s)\n", (uint64_t)address[i], (uint64_t)(uint32_t)page,
		    (uint64_t)(uint32_t)book, (uint64_t)(uint32_t)id, id == 0x10 || id == 0x20 ? "TAS2562" : id == 0 ? "TAS2564" :
		    page == -2 ? "BUS NOT AT REST" : page == -1 ? "NO ANSWER" : id == -3 ? "book/page not 0" : "UNKNOWN");
	}
	aud_show("after", AUD_SDA);
	aud_show("after", AUD_SCL);
}

/* ------------------------------------------------------------------------ */
/* vayu jack: the WCD934x codec's reset pin, for XNU's bsd/dev/vayu_jack.c. */
/*                                                                          */
/* The codec hangs off the audio DSP's SLIMbus; its reset is TLMM GPIO 143  */
/* (dt msm_cdc_pinctrl@143 qcom,cdc-rst-n-gpio = <&tlmm 143>, states        */
/* cdc_reset_sleep / cdc_reset_active: function gpio, drive-strength 2 mA,  */
/* bias-disable, output low / high; recovery-live.dts:13496-13530, 27528),  */
/* SOUTH tile (pinctrl-sm8150.c), GPIO 123 its interrupt. As with the       */
/* amplifiers, XNU does not write TLMM: only when the payload's command     */
/* line asks:                                                              */
/*   vayu_jack_pins=1  reset out of reset: 20 ms low, then high, 20 ms      */
/*                     (wcd9xxx_reset: wcd9xxx-utils.c:715-731); the codec  */
/*                     may then enumerate on the DSP's SLIMbus              */
/*   vayu_jack_pins=2  reset driven low again: the codec back in reset      */
/* Every write is logged with its readback; GPIO 123 is only shown.         */
/* ------------------------------------------------------------------------ */

#define AUD_JACK_RESET  143
#define AUD_JACK_IRQ    123

static void
vayu_jack_pins(const char *cmdline)
{
	uint32_t cfg, was;

	aud_show("before", AUD_JACK_RESET);
	aud_show("before", AUD_JACK_IRQ);

	/* the output level low first, so enabling the output cannot pulse the reset high */
	aud_wr(aud_cfg(AUD_JACK_RESET) + 4, 0);
	/* function 0 (GPIO), pull 0 (bias-disable), drive 0 (2 mA), output; the other bits are kept */
	was = aud_rd(aud_cfg(AUD_JACK_RESET));
	cfg = was & ~((3u << 0) | (0xfu << 2) | (7u << 6));
	cfg |= AUD_CFG_OE;
	aud_wr(aud_cfg(AUD_JACK_RESET), cfg);
	print("vayu-jack: gpio%d -> cfg %x (was %x, function 0, 2 mA, bias-disable, output)\n", (uint64_t)AUD_JACK_RESET,
	    (uint64_t)aud_rd(aud_cfg(AUD_JACK_RESET)), (uint64_t)was);
	aud_show("after cfg, reset low", AUD_JACK_RESET);
	if (aud_word(cmdline, "vayu_jack_pins=1")) {
		aud_spin_us(20000);             /* wcd9xxx_reset(): low 20 ms */
		aud_wr(aud_cfg(AUD_JACK_RESET) + 4, 2);
		aud_show("after reset high", AUD_JACK_RESET);
		aud_spin_us(20000);             /* ... and high 20 ms before the first transfer */
		aud_show("after 20 ms", AUD_JACK_RESET);
	}
}

/* vayu NFC: the PN553's pins for XNU's driver, plus a read-only NCI probe. */
/*                                                                          */
/* NXP PN553 at I2C 0x28 on QUP SE9's bus. From the phone's live DT: SDA    */
/* GPIO39 and SCL GPIO40 (qup9 = function 1), bias-disable (board           */
/* pull-ups), 2 mA, NORTH; IRQ GPIO47 input pull-up 2 mA, high = a packet,  */
/* EAST; FIRM GPIO48 output, LOW in normal mode, pull-up 2 mA, EAST; VEN    */
/* GPIO102 output, HIGH = chip on, pull-up 2 mA, NORTH. CLKREQ GPIO113      */
/* (west tile) is never read or written: writes there wedged this phone.    */
/* TLMM NORTH = 0x03900000 (39, 40, 102), EAST = 0x03500000 (47, 48);       */
/* cfg = tile + 0x1000 * pin, io = cfg + 4; cfg [1:0] pull, [5:2] function, */
/* [8:6] drive ((n+1)*2 mA), [9] output enable; io bit 0 in, bit 1 out.     */
/*   vayu_nfc_pins=1: IRQ input, FIRM low, VEN power-cycled on, 39/40 left  */
/*     as released plain GPIOs (function 0) for XNU.                        */
/*   vayu_nfc_pins=2: FIRM and VEN low, the vendor's power-off sequence.    */
/*   vayu_nfc_probe=1 (with =1): read-only NCI bring-up on 39/40; only      */
/*     CORE_RESET_CMD and CORE_INIT_CMD are sent, no NVM write.             */
/* Every pin-configuration write is logged with its readback (DID NOT TAKE  */
/* on a mismatch); the I2C bit writes are not. FIRM is never raised, 113 is */
/* never touched.                                                           */
/* ------------------------------------------------------------------------ */

#define NFC_TLMM_NORTH  0x03900000ULL
#define NFC_TLMM_EAST   0x03500000ULL
#define NFC_SDA         39
#define NFC_SCL         40
#define NFC_IRQ         47
#define NFC_FIRM        48
#define NFC_VEN         102
#define NFC_CFG_OE      (1u << 9)
#define NFC_CFG         0              /* nfc_reg_wr(): the cfg register */
#define NFC_IO          1              /* nfc_reg_wr(): the io register */

static uint64_t nfc_cfg(unsigned pin)
{
	return (pin == NFC_IRQ || pin == NFC_FIRM ? NFC_TLMM_EAST : NFC_TLMM_NORTH) + 0x1000ULL * pin;
}

static int nfc_high(unsigned pin)
{
	return aud_rd(nfc_cfg(pin) + 4) & 1;
}

static void
nfc_show(const char *when, unsigned pin)
{
	uint32_t cfg = aud_rd(nfc_cfg(pin)), io = aud_rd(nfc_cfg(pin) + 4);

	print("vayu-nfc: %s gpio%d cfg %x io %x (function %d, pull %d, oe %d, drive %d, out %d, in %d)\n", when,
	    (uint64_t)pin, (uint64_t)cfg, (uint64_t)io, (uint64_t)((cfg >> 2) & 0xf), (uint64_t)(cfg & 3),
	    (uint64_t)((cfg >> 9) & 1), (uint64_t)((cfg >> 6) & 7), (uint64_t)((io >> 1) & 1), (uint64_t)(io & 1));
}

/* One register write, its readback, and DID NOT TAKE when the fields differ. */
static void
nfc_reg_wr(const char *what, unsigned pin, int reg, uint32_t want)
{
	uint64_t pa = nfc_cfg(pin) + (reg == NFC_IO ? 4 : 0);
	uint32_t mask = reg == NFC_IO ? 2u : 0x3ffu, back; /* io bit 0 is the input level, not written */

	aud_wr(pa, want);
	back = aud_rd(pa);
	print("vayu-nfc: %s gpio%d %s <- %x, reads %x%s\n", what, (uint64_t)pin, reg == NFC_IO ? "io" : "cfg",
	    (uint64_t)want, (uint64_t)back, (back & mask) == (want & mask) ? "" : " DID NOT TAKE");
}

/* print() has no field width: "%02x" bytes go through a tiny two-digit string. */
static void
nfc_hex_bytes(const uint8_t *p, int len)
{
	static const char digits[] = "0123456789abcdef";
	char text[4] = { ' ', 0, 0, 0 };

	for (int i = 0; i < len; i++) {
		text[1] = digits[p[i] >> 4], text[2] = digits[p[i] & 0xf];
		print("%s", text);
	}
}

static void
nfc_hex_line(const char *dir, const uint8_t *buf, int len)
{
	print("vayu-nfc: %s %d:", dir, (uint64_t)len);
	nfc_hex_bytes(buf, len);
	print("\n");
}

/* The bus pins as step 4 of vayu_nfc_pins() left them: their pull bits are kept. */
static uint32_t nfc_sda_cfg, nfc_scl_cfg;

static void
nfc_line(unsigned pin, int high)
{
	uint32_t cfg = pin == NFC_SDA ? nfc_sda_cfg : nfc_scl_cfg;

	aud_wr(nfc_cfg(pin) + 4, 0);
	aud_wr(nfc_cfg(pin), (cfg & ~NFC_CFG_OE) | (high ? 0 : NFC_CFG_OE));
}

static int
nfc_bit(int out)
{
	int in;

	nfc_line(NFC_SDA, out); aud_spin_us(5);
	nfc_line(NFC_SCL, 1);
	for (int stretched = 0; !nfc_high(NFC_SCL) && stretched < 200; stretched++) aud_spin_us(5);
	aud_spin_us(5);
	in = nfc_high(NFC_SDA);
	nfc_line(NFC_SCL, 0);
	return in;
}

static int
nfc_byte_out(uint8_t b)
{
	for (int bit = 7; bit >= 0; bit--) nfc_bit((b >> bit) & 1);
	return !nfc_bit(1);
}

static int
nfc_byte_in(int ack)
{
	int b = 0;

	for (int bit = 7; bit >= 0; bit--) b = b << 1 | nfc_bit(1);
	nfc_bit(!ack);                  /* 0 drives SDA low (ACK), 1 lets it go (NACK) */
	return b;
}

/* Let both lines go: a transaction only starts with both pulled high. */
static int
nfc_bus_rest(void)
{
	nfc_line(NFC_SDA, 1); nfc_line(NFC_SCL, 1); aud_spin_us(5);
	int sda = nfc_high(NFC_SDA), scl = nfc_high(NFC_SCL);

	if (!sda || !scl) print("vayu-nfc: bus not at rest (sda %d scl %d)\n", (uint64_t)sda, (uint64_t)scl);
	return sda && scl;
}

/* 0 = ACKed, -1 = address NACK, -2 = data NACK, -3 = bus not at rest. */
static int
nfc_i2c_write(const uint8_t *buf, int len)
{
	int ret = nfc_bus_rest() ? 0 : -3;

	if (ret == 0) {
		nfc_line(NFC_SDA, 1); nfc_line(NFC_SCL, 1); aud_spin_us(5);
		nfc_line(NFC_SDA, 0); aud_spin_us(5); nfc_line(NFC_SCL, 0); aud_spin_us(5);
		ret = nfc_byte_out(0x28 << 1) ? 0 : -1;
		for (int i = 0; ret == 0 && i < len; i++) ret = nfc_byte_out(buf[i]) ? 0 : -2;
		nfc_line(NFC_SDA, 0); aud_spin_us(5); nfc_line(NFC_SCL, 1); aud_spin_us(5);
		nfc_line(NFC_SDA, 1); aud_spin_us(5);
	}
	return ret;
}

static int
nfc_i2c_read(uint8_t *buf, int len)
{
	int ret = nfc_bus_rest() ? 0 : -3;

	if (ret == 0) {
		nfc_line(NFC_SDA, 1); nfc_line(NFC_SCL, 1); aud_spin_us(5);
		nfc_line(NFC_SDA, 0); aud_spin_us(5); nfc_line(NFC_SCL, 0); aud_spin_us(5);
		ret = nfc_byte_out(0x28 << 1 | 1) ? 0 : -1;
		for (int i = 0; ret == 0 && i < len; i++) buf[i] = (uint8_t)nfc_byte_in(i != len - 1);
		nfc_line(NFC_SDA, 0); aud_spin_us(5); nfc_line(NFC_SCL, 1); aud_spin_us(5);
		nfc_line(NFC_SDA, 1); aud_spin_us(5);
	}
	return ret;
}

/* The first write after idle may NACK while the PN553 wakes up: three more tries. */
static int
nfc_write_retry(const uint8_t *buf, int len)
{
	int ret = nfc_i2c_write(buf, len);

	for (int retry = 1; ret == -1 && retry <= 3; retry++) {
		print("vayu-nfc: write NACKed, retry %d\n", (uint64_t)retry);
		aud_spin_us(1000);
		ret = nfc_i2c_write(buf, len);
	}
	return ret;
}

static int
nfc_irq_wait(int ms)
{
	uint64_t frq = READ_SYSREG(CNTFRQ_EL0), start = READ_SYSREG(CNTPCT_EL0);
	uint64_t ticks = (uint64_t)ms * frq / 1000;

	for (;;) {
		if (nfc_high(NFC_IRQ)) return 1;
		if (READ_SYSREG(CNTPCT_EL0) - start >= ticks) return 0;
		aud_spin_us(100);
	}
}

/* One NCI packet: header, then its payload; both reads are logged as one line. */
static int
nfc_packet_read(uint8_t *buf, int max)
{
	int ret, len = 3, plen;

	if ((ret = nfc_i2c_read(buf, 3)) < 0) {
		print("vayu-nfc: header read failed (%d)\n", (uint64_t)(uint32_t)ret);
		return ret;
	}
	if ((plen = buf[2]) > 0) {
		if (plen > max - 3) {
			print("vayu-nfc: payload %d clipped to %d\n", (uint64_t)plen, (uint64_t)(max - 3));
			plen = max - 3;
		}
		if ((ret = nfc_i2c_read(buf + 3, plen)) < 0) {
			print("vayu-nfc: payload read failed (%d)\n", (uint64_t)(uint32_t)ret);
			return ret;
		}
		len += plen;
	}
	nfc_hex_line("rx", buf, len);
	return len;
}

/* Send one command, wait for its answer, read it: -1 write, -2 IRQ, -3 read. */
static int
nfc_cmd(const uint8_t *cmd, int len, uint8_t *buf, int max)
{
	int ret;

	nfc_hex_line("tx", cmd, len);
	if (nfc_write_retry(cmd, len) < 0) return -1;
	if (!nfc_irq_wait(100)) return -2;
	ret = nfc_packet_read(buf, max);
	return ret < 0 ? -3 : ret;
}

static const char *nfc_err(int ret)
{
	return ret == -1 ? "write failed" : ret == -2 ? "IRQ never rose" : "read failed";
}

static int
nfc_probe(void)
{
	static uint8_t buf[260];
	static const uint8_t reset[4] = { 0x20, 0x00, 0x01, 0x00 };
	uint8_t init[5] = { 0x20, 0x01, 0x00, 0x00, 0x00 };
	int len, major = 1, ok = -1;

	nfc_sda_cfg = aud_rd(nfc_cfg(NFC_SDA));
	nfc_scl_cfg = aud_rd(nfc_cfg(NFC_SCL));
	/* 1. Packets left over from Android, if the controller still has some. */
	for (int i = 0; i < 4 && nfc_irq_wait(5); i++) {
		if (nfc_packet_read(buf, (int)sizeof(buf)) < 0) {
			print("vayu-nfc: leftover packet read failed, probe aborted\n");
			goto restore;
		}
	}
	/* 2. CORE_RESET_CMD, reset type 0: keep the configuration. */
	len = nfc_cmd(reset, 4, buf, (int)sizeof(buf));
	if (len < 0 || buf[0] != 0x40 || buf[1] != 0x00 || buf[2] < 1) {
		print("vayu-nfc: no answer to CORE_RESET (%s)\n", len < 0 ? nfc_err(len) : "bad response");
		goto restore;
	}
	if (buf[2] == 3) {
		print("vayu-nfc: NCI 1.0 style: status %x, NCI version %x (0x10 = 1.0, 0x20 = 2.0), config status %x\n",
		    (uint64_t)buf[3], (uint64_t)buf[4], (uint64_t)buf[5]);
		major = buf[4] >= 0x20 ? 2 : 1;
	} else if (buf[2] == 1) {
		print("vayu-nfc: NCI 2.0 style: status %x\n", (uint64_t)buf[3]);
		major = 2;
	} else {
		print("vayu-nfc: no answer to CORE_RESET (length %d)\n", (uint64_t)buf[2]);
		goto restore;
	}
	for (int i = 0; i < 3 && nfc_irq_wait(50); i++) {
		if (nfc_packet_read(buf, (int)sizeof(buf)) < 0) {
			print("vayu-nfc: CORE_RESET_NTF read failed, probe aborted\n");
			goto restore;
		}
		if (buf[0] == 0x60 && buf[1] == 0x00 && buf[2] >= 5) {
			int n = buf[7], avail = buf[2] - 5;

			if (n > avail) n = avail;
			print("vayu-nfc: CORE_RESET_NTF: trigger %x, config status %x, NCI version %x, manufacturer %x, "
			    "info %d:", (uint64_t)buf[3], (uint64_t)buf[4], (uint64_t)buf[5], (uint64_t)buf[6],
			    (uint64_t)n);
			nfc_hex_bytes(buf + 8, n);
			print("\n");
		}
	}
	/* 3. CORE_INIT_CMD, in the flavour the reset announced. */
	if (major == 2) init[2] = 2;
	len = nfc_cmd(init, major == 2 ? 5 : 3, buf, (int)sizeof(buf));
	if (len < 0) {
		print("vayu-nfc: no answer to CORE_INIT (%s)\n", nfc_err(len));
		goto restore;
	}
	if (major == 1 && buf[0] == 0x40 && buf[1] == 0x01 && buf[2] >= 6) {
		uint8_t *p = buf + 3;
		int plen = buf[2];

		print("vayu-nfc: CORE_INIT_RSP status %x\n", (uint64_t)p[0]);
		print("vayu-nfc: manufacturer %x, hw %x rom %x fw %x.%x (NXP layout)\n", (uint64_t)p[plen - 5],
		    (uint64_t)p[plen - 4], (uint64_t)p[plen - 3], (uint64_t)p[plen - 2], (uint64_t)p[plen - 1]);
	} else if (major == 1) {
		print("vayu-nfc: unexpected CORE_INIT response\n");
		goto restore;
	}
	print("vayu-nfc: probe done\n");
	ok = 0;
restore:
	/* Back to step 4's state for XNU's driver. */
	nfc_reg_wr("probe restore", NFC_SDA, NFC_CFG, nfc_sda_cfg);
	nfc_reg_wr("probe restore", NFC_SDA, NFC_IO, 0);
	nfc_reg_wr("probe restore", NFC_SCL, NFC_CFG, nfc_scl_cfg);
	nfc_reg_wr("probe restore", NFC_SCL, NFC_IO, 0);
	return ok;
}

/* VEN back to 0 (the lead's condition for exception 6): after the probe unless XNU's steps follow, on an
 * error path, and after the kernel returns. FIRM stays low. */
static int nfc_ven_on;

static void
nfc_ven_off(const char *why)
{
	if (!nfc_ven_on) {
		return;
	}
	nfc_reg_wr(why, NFC_VEN, NFC_IO, 0);
	nfc_ven_on = 0;
}

static void
vayu_nfc_pins(const char *cmdline)
{
	static const unsigned pins[] = { NFC_SDA, NFC_SCL, NFC_IRQ, NFC_FIRM, NFC_VEN };
	static const unsigned off[] = { NFC_FIRM, NFC_VEN };
	uint32_t cfg;

	for (unsigned i = 0; i < 5; i++) nfc_show("before", pins[i]);
	if (aud_word(cmdline, "vayu_nfc_pins=2")) {
		for (unsigned i = 0; i < 2; i++) {
			cfg = aud_rd(nfc_cfg(off[i]));
			nfc_reg_wr("power off", off[i], NFC_IO, 0);
			nfc_reg_wr("power off", off[i], NFC_CFG, (cfg & (3u | (7u << 6))) | NFC_CFG_OE);
			aud_spin_us(10000);
		}
		for (unsigned i = 0; i < 2; i++) nfc_show("after", off[i]);
		return;
	}
	/* IRQ 47: input, pull-up, 2 mA. */
	nfc_reg_wr("IRQ", NFC_IRQ, NFC_CFG, 3);
	/* FIRM 48 stays low (normal mode); only the output buffer is enabled. */
	cfg = aud_rd(nfc_cfg(NFC_FIRM));
	nfc_reg_wr("FIRM", NFC_FIRM, NFC_IO, 0);
	nfc_reg_wr("FIRM", NFC_FIRM, NFC_CFG, (cfg & 3) | NFC_CFG_OE);
	aud_spin_us(10000);
	/* VEN 102 (exception 6, loader-only): 0, 20 ms, 1, 20 ms; FIRM is already low. */
	cfg = aud_rd(nfc_cfg(NFC_VEN));
	nfc_reg_wr("VEN", NFC_VEN, NFC_IO, 0);
	nfc_reg_wr("VEN", NFC_VEN, NFC_CFG, (cfg & 3) | NFC_CFG_OE);
	aud_spin_us(20000);
	nfc_reg_wr("VEN", NFC_VEN, NFC_IO, 2);
	nfc_ven_on = 1;
	aud_spin_us(20000);
	/* 39/40: released plain GPIOs (function 0); the board pull-ups hold them high. */
	for (unsigned pin = NFC_SDA; pin <= NFC_SCL; pin++) {
		nfc_reg_wr("I2C", pin, NFC_CFG, aud_rd(nfc_cfg(pin)) & 3);
		nfc_reg_wr("I2C", pin, NFC_IO, 0);
	}
	for (unsigned i = 0; i < 5; i++) nfc_show("after", pins[i]);
	print("vayu-nfc: IRQ 47 reads %s\n", nfc_high(NFC_IRQ) ? "high" : "low");
	if (aud_word(cmdline, "vayu_nfc_probe=1") && nfc_probe() < 0) {
		nfc_ven_off("VEN off (probe failed)");
	} else if (!aud_word(cmdline, "vayu_nfc_xnu_tlmm=1")) {
		nfc_ven_off("VEN off (no XNU steps follow)");
	} else {
		print("vayu-nfc: VEN left on for XNU (vayu_nfc_xnu_tlmm=1); off again after the kernel returns\n");
	}
}

struct payload_info {
	const uint8_t *kernel;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	const char *platform;           /* optional: the device this image was built for */
	uint32_t platform_size;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "platform")) {
			pi->platform = text;
			pi->platform_size = e->size;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/*
 * fb_spare (loader 1.7): the frames for the display controller, below 4 GB, kept for the loader session. MDP3 and MDP4
 * (2026-10-25) hung the whole phone as soon as the kernel touched them; one candidate is a range UEFI calls free but the
 * vendor reserves (secure or protected). So a piece that overlaps any of the vendor's reserved-memory nodes below 4 GB
 * (vayu.dtb /reserved-memory, /tmp/vayu.dts: the 22 fixed regions) is given back and the next one is taken below it.
 */
static const struct { uint64_t base, size; const char *name; } vendor_reserved[] = {
	{ 0x85700000, 0x600000, "hyp" }, { 0x85d00000, 0x100000, "xbl_dump" }, { 0x85e00000, 0x140000, "xbl_aop" },
	{ 0x86000000, 0x200000, "smem" }, { 0x86200000, 0x5500000, "removed_regions" }, { 0x8b700000, 0x500000, "camera" },
	{ 0x8bc00000, 0x180000, "pil_wlan" }, { 0x8bd80000, 0x80000, "pil_npu" }, { 0x8be00000, 0x2200000, "pil_adsp" },
	{ 0x8e000000, 0x9600000, "modem" }, { 0x97600000, 0x500000, "pil_video" }, { 0x97b00000, 0x1400000, "pil_slpi" },
	{ 0x98f00000, 0x17000, "pil_ipa/gpu" }, { 0x99000000, 0x100000, "pil_spss" }, { 0x99100000, 0x1400000, "cdsp" },
	{ 0x9c000000, 0x2400000, "cont_splash" }, { 0x9e400000, 0x1400000, "qseecom" }, { 0xa4c00000, 0x3c00000, "cdsp_sec" },
	{ 0xaf000000, 0x1000000, "ramdump_fb" }, { 0xb0000000, 0x800000, "ramoops" },
};
static uint64_t fb_spare_pa, fb_spare_count;

/* The type of the UEFI map's descriptor that holds pa (or -1), for the log. */
static int
memory_type_at(uint64_t pa)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		return -1;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (pa >= d->phys_start && pa < d->phys_start + (d->pages << 12)) {
			return (int)d->type;
		}
	}
	return -1;
}

/* N frames of 12 MiB in one piece below 4 GB and clear of the vendor's reserved ranges; 0 on failure. Kept once got. */
static uint64_t
fb_spare_get(uint64_t frames)
{
	uint64_t pages = frames * ((12ULL << 20) >> 12), max = 0xFFFFFFFFULL;

	if (fb_spare_pa != 0 && fb_spare_count >= frames) {
		return fb_spare_pa;
	}
	for (int tries = 0; tries < 12; tries++) {
		uint64_t pa = max, end;
		int hit = -1;

		if (EFI_ERROR(BS->allocate_pages(AllocateMaxAddress, EfiLoaderData, pages, &pa))) {
			print("fb_spare: no %d frames below %x\n", frames, max + 1);
			return 0;
		}
		end = pa + (pages << 12);
		for (unsigned r = 0; r < sizeof(vendor_reserved) / sizeof(vendor_reserved[0]); r++) {
			if (pa < vendor_reserved[r].base + vendor_reserved[r].size && end > vendor_reserved[r].base) {
				hit = (int)r;
				break;
			}
		}
		if (hit < 0) {
			fb_spare_pa = pa;
			fb_spare_count = frames;
			print("fb_spare: %d frames of 12 MiB at %x - %x (UEFI type %d), clear of the vendor's reserved ranges\n", frames, pa,
			    end, (uint64_t)memory_type_at(pa));
			return pa;
		}
		print("fb_spare: %x - %x overlaps the vendor's %s (%x + %x): given back, the next below it\n", pa, end,
		    vendor_reserved[hit].name, vendor_reserved[hit].base, vendor_reserved[hit].size);
		BS->free_pages(pa, pages);
		if (vendor_reserved[hit].base < (pages << 12) + 0x80000000ULL) {
			return 0;
		}
		max = vendor_reserved[hit].base - 1;
	}
	return 0;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload sits in RAM that UEFI may consider free: stay clear of it */
		if (lo < pay_hi && hi > pay_lo) {
			if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
				hi = pay_lo;
			} else {
				lo = pay_hi;
			}
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVT/QUP hand-over: pins and touch supply, for XNU's QUP SPI driver.       */
/*                                                                          */
/* XNU cannot write TLMM on this phone: the vendor pinctrl window at        */
/* 0x03000000 ignores writes, and the first tile alias at 0x03100000 wedged */
/* the bus (watchdog reset). The vendor pinctrl driver's four tiles are at  */
/* 0x03000000 + 0x100000 west, +0x500000 east, +0x900000 north, +0xd00000   */
/* south; a pin's registers are +0x1000 * pin from its tile. Pins 55-58 are */
/* SOUTH and qup17 is function index 1 in their pinctrl group; reset GPIO12 */
/* is NORTH; VDDIO GPIO100 and INT GPIO122 are SOUTH. UEFI can write these; */
/* this runs only when the payload's command line asks for nvt_qup=1 or    */
/* touch_driver=nvt_qup. It also enables the SE's GCC clocks; XNU checks   */
/* them, sets up GENI and speaks NVT.                                       */
/* ------------------------------------------------------------------------ */

#define NVT_TLMM_NORTH  0x03900000ULL
#define NVT_TLMM_SOUTH  0x03D00000ULL
#define NVT_PIN_CFG(tile, pin) ((tile) + 0x1000ULL * (pin))
#define NVT_PIN_IO(tile, pin)  (NVT_PIN_CFG(tile, pin) + 4)

#define NVT_PIN_RESET   12
#define NVT_PIN_INT     122
#define NVT_PIN_VDDIO   100
#define NVT_SPI_PIN0    55
#define NVT_SPI_FUNC    1       /* qup17: the second entry (index 1) of pins 55-58's function list */

static inline uint32_t
nvt_rd(uint64_t pa)
{
	return *(volatile uint32_t *)(uintptr_t)pa;
}

static inline void
nvt_wr(uint64_t pa, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)pa = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* A word on the payload command line, so "nvt_qup=1" does not match "xnvt_qup=1". */
static int
nvt_cmdline_word(const char *cmdline, const char *word)
{
	size_t n = 0;

	while (word[n] != 0) {
		n++;
	}
	for (const char *p = cmdline; *p != 0; ) {
		while (*p == ' ') {
			p++;
		}
		const char *q = p;
		while (*q != 0 && *q != ' ') {
			q++;
		}
		if ((size_t)(q - p) == n) {
			size_t i = 0;
			while (i < n && p[i] == word[i]) {
				i++;
			}
			if (i == n) {
				return 1;
			}
		}
		p = q;
	}
	return 0;
}

static void
nvt_pin_show(const char *when, uint64_t tile, unsigned pin, const char *name)
{
	uint32_t cfg = nvt_rd(NVT_PIN_CFG(tile, pin));
	uint32_t io = nvt_rd(NVT_PIN_IO(tile, pin));

	print("nvt-qup: %s %s gpio%x cfg %x io %x (mux %x, pull %x, drive %x, oe %x)\n", when, name, (uint64_t)pin,
	    (uint64_t)cfg, (uint64_t)io, (uint64_t)((cfg >> 2) & 0xf), (uint64_t)(cfg & 3),
	    (uint64_t)((cfg >> 6) & 7), (uint64_t)((cfg >> 9) & 1));
}

static void
nvt_pin_set(uint64_t tile, unsigned pin, unsigned func, unsigned pull, unsigned ma, int output, int high)
{
	uint64_t cfg_pa = NVT_PIN_CFG(tile, pin), io_pa = NVT_PIN_IO(tile, pin);
	uint32_t cfg = nvt_rd(cfg_pa);
	uint32_t newcfg = cfg & ~((0xfu << 2) | 0x3u | (0x7u << 6) | (1u << 9));

	if (output) {
		nvt_wr(io_pa, high ? 2 : 0);
	}
	newcfg |= ((func & 0xf) << 2) | (pull & 3) | ((((ma / 2) - 1) & 7) << 6);
	if (output) {
		newcfg |= 1u << 9;
	}
	nvt_wr(cfg_pa, newcfg);
	print("nvt-qup: pin %x -> cfg %x (was %x), io %x\n", (uint64_t)pin, (uint64_t)nvt_rd(cfg_pa),
	    (uint64_t)cfg, (uint64_t)nvt_rd(io_pa));
}

/*
 * GCC for SE17 (spi@c80000), from the vendor clock driver (MiCode
 * Xiaomi_Kernel_OpenSource vayu-r-oss 0c9888aa, drivers/clk/qcom/):
 *   gcc-sm8150.c:951-952   gcc_qupv3_wrap2_s0_clk_src cmd_rcgr 0x1e148
 *   gcc-sm8150.c:60-61     parent map 0: P_BI_TCXO = 0; :627 F(19200000, XO, 1)
 *   gcc-sm8150.c:2857-2862 gcc_qupv3_wrap2_s0_clk halt 0x1e144, vote 0x52014 bit 4
 *   gcc-sm8150.c:3021-3026 gcc_qupv3_wrap_2_m_ahb_clk halt 0x1e004, vote bit 2
 *   gcc-sm8150.c:3034-3041 gcc_qupv3_wrap_2_s_ahb_clk halt 0x1e008, vote bit 1,
 *                          hwcg_reg 0x1e008 hwcg_bit 1
 *   clk-rcg2.c:34-58       CMD_UPDATE 0, ROOT_EN 1, ROOT_OFF 31; CFG +4 (div
 *                          [4:0], src [10:8], mode [13:12], HW_CLK_CTRL 20);
 *                          DFSR +0x14, DFS_EN bit 0
 *   clk-branch.c:29-39,86-91 a branch in hardware-gated mode (hwcg bit set)
 *                          is not halt-checked: CLK_OFF stays 1 while idle.
 * Mainline v6.6 gcc-sm8150.c:2402,2560,2573,666 has the same offsets.
 *
 * The s_ahb CBCR reads 0x80000002 (CLK_OFF + HW_CTL) on this phone both
 * before and after the vote (run 150913), while the wrapper's HW_VER read
 * through that clock returned 0x10030000; waiting for its CLK_OFF to clear
 * was the wrong test.
 */
#define NVT_GCC          0x00100000ULL
#define NVT_GCC_VOTE     0x52014
#define NVT_VOTE_S_AHB   (1u << 1)
#define NVT_VOTE_M_AHB   (1u << 2)
#define NVT_VOTE_SE0     (1u << 4)
#define NVT_HALT_M_AHB   0x1e004
#define NVT_HALT_S_AHB   0x1e008
#define NVT_HALT_SE0     0x1e144
#define NVT_RCG_CMD      0x1e148
#define NVT_RCG_CFG      0x1e14c
#define NVT_RCG_DFS      0x1e15c
#define NVT_RCG_UPDATE   (1u << 0)
#define NVT_RCG_ROOT_EN  (1u << 1)
#define NVT_RCG_ROOT_OFF (1u << 31)
#define NVT_CLK_OFF      (1u << 31)
#define NVT_CBCR_HW_CTL  (1u << 1)

static void
nvt_gcc_wr(uint32_t off, uint32_t v, const char *name)
{
	nvt_wr(NVT_GCC + off, v);
	print("nvt-qup: gcc %s %x <- %x, reads %x\n", name, (uint64_t)off, (uint64_t)v,
	    (uint64_t)nvt_rd(NVT_GCC + off));
}

/* 1 when the branch runs (or is hardware-gated and so has no halt to wait on). */
static int
nvt_gcc_branch_on(uint32_t off, const char *name, int hwcg)
{
	uint32_t v = 0;
	int i;

	for (i = 0; i < 1000; i++) {
		v = nvt_rd(NVT_GCC + off);
		if (hwcg && (v & NVT_CBCR_HW_CTL)) {
			print("nvt-qup: gcc %s %x = %x: hardware-gated, not halt-checked\n", name, (uint64_t)off,
			    (uint64_t)v);
			return 1;
		}
		if ((v & NVT_CLK_OFF) == 0) {
			print("nvt-qup: gcc %s %x = %x: on after %x us\n", name, (uint64_t)off, (uint64_t)v,
			    (uint64_t)(i * 10));
			return 1;
		}
		BS->stall(10);
	}
	print("nvt-qup: gcc %s %x = %x: still off after 10 ms\n", name, (uint64_t)off, (uint64_t)v);
	return 0;
}

/* SE17 clocks: the RCG at XO/1 with the SE branch off, then AHB, then the SE. */
static int
nvt_gcc_setup(void)
{
	uint32_t v;
	int i;

	nvt_gcc_wr(NVT_GCC_VOTE, nvt_rd(NVT_GCC + NVT_GCC_VOTE) & ~NVT_VOTE_SE0, "vote (se0 off)");
	BS->stall(10);

	v = nvt_rd(NVT_GCC + NVT_RCG_CMD);
	if (v & NVT_RCG_ROOT_OFF) {
		nvt_gcc_wr(NVT_RCG_CMD, v | NVT_RCG_ROOT_EN, "rcg cmd (root en)");
		for (i = 0; i < 100 && (nvt_rd(NVT_GCC + NVT_RCG_CMD) & NVT_RCG_ROOT_OFF); i++) {
			BS->stall(10);
		}
	}
	v = nvt_rd(NVT_GCC + NVT_RCG_DFS);
	if (v & 1) {
		nvt_gcc_wr(NVT_RCG_DFS, v & ~1u, "rcg dfs (off)");
	}
	v = nvt_rd(NVT_GCC + NVT_RCG_CFG);
	nvt_gcc_wr(NVT_RCG_CFG, (v & ~((0x7u << 8) | 0x1fu | (0x3u << 12) | (1u << 20))) | 1u, "rcg cfg (xo/1)");
	nvt_gcc_wr(NVT_RCG_CMD, nvt_rd(NVT_GCC + NVT_RCG_CMD) | NVT_RCG_UPDATE, "rcg cmd (update)");
	for (i = 0; i < 100 && (nvt_rd(NVT_GCC + NVT_RCG_CMD) & NVT_RCG_UPDATE); i++) {
		BS->stall(10);
	}
	v = nvt_rd(NVT_GCC + NVT_RCG_CMD);
	print("nvt-qup: gcc rcg cmd %x cfg %x dfs %x (want root on, update clear, cfg 1)\n", (uint64_t)v,
	    (uint64_t)nvt_rd(NVT_GCC + NVT_RCG_CFG), (uint64_t)nvt_rd(NVT_GCC + NVT_RCG_DFS));
	if (v & (NVT_RCG_ROOT_OFF | NVT_RCG_UPDATE)) {
		print("nvt-qup: gcc rcg did not take; SE clock stays off\n");
		return 0;
	}

	nvt_gcc_wr(NVT_GCC_VOTE, nvt_rd(NVT_GCC + NVT_GCC_VOTE) | NVT_VOTE_M_AHB | NVT_VOTE_S_AHB, "vote (ahb)");
	if (!nvt_gcc_branch_on(NVT_HALT_M_AHB, "m_ahb", 0) || !nvt_gcc_branch_on(NVT_HALT_S_AHB, "s_ahb", 1)) {
		return 0;
	}
	nvt_gcc_wr(NVT_GCC_VOTE, nvt_rd(NVT_GCC + NVT_GCC_VOTE) | NVT_VOTE_SE0, "vote (se0)");
	if (!nvt_gcc_branch_on(NVT_HALT_SE0, "se0", 0)) {
		return 0;
	}
	print("nvt-qup: gcc on: vote %x halts m %x s %x se %x\n", (uint64_t)nvt_rd(NVT_GCC + NVT_GCC_VOTE),
	    (uint64_t)nvt_rd(NVT_GCC + NVT_HALT_M_AHB), (uint64_t)nvt_rd(NVT_GCC + NVT_HALT_S_AHB),
	    (uint64_t)nvt_rd(NVT_GCC + NVT_HALT_SE0));
	return 1;
}

/* A GPIO (mux 0) output (OE, cfg bit 9) driving high (io bit 1): pinctrl-sm8150.c/pinctrl-msm.c layout. */
static int
nvt_pin_out_high(uint64_t tile, unsigned pin)
{
	uint32_t cfg = nvt_rd(NVT_PIN_CFG(tile, pin)), io = nvt_rd(NVT_PIN_IO(tile, pin));

	return ((cfg >> 2) & 0xf) == 0 && ((cfg >> 9) & 1) && ((io >> 1) & 1);
}

static void
nvt_qup_setup(const char *cmdline)
{
	int dry = nvt_cmdline_word(cmdline, "nvt_qup_dry=1");
	uint32_t vote, halt_m, halt_s, halt_se, rcg_cfg, rcg_dfs, fw;
	int i, keep, reset_up, vddio_up;

	print("nvt-qup: preparing the touch bus%s\n", dry ? " (dry run: reads only)" : "");

	/* What the bootloader left, before anything is written. */
	nvt_pin_show("before", NVT_TLMM_NORTH, NVT_PIN_RESET, "reset");
	nvt_pin_show("before", NVT_TLMM_SOUTH, NVT_PIN_VDDIO, "vddio");
	nvt_pin_show("before", NVT_TLMM_SOUTH, NVT_PIN_INT, "int");
	for (i = 0; i < 4; i++) {
		nvt_pin_show("before", NVT_TLMM_SOUTH, NVT_SPI_PIN0 + i, "spi");
	}

	vote = nvt_rd(NVT_GCC + NVT_GCC_VOTE);
	halt_m = nvt_rd(NVT_GCC + NVT_HALT_M_AHB);
	halt_s = nvt_rd(NVT_GCC + NVT_HALT_S_AHB);
	halt_se = nvt_rd(NVT_GCC + NVT_HALT_SE0);
	rcg_cfg = nvt_rd(NVT_GCC + NVT_RCG_CFG);
	rcg_dfs = nvt_rd(NVT_GCC + NVT_RCG_DFS);
	print("nvt-qup: before gcc vote %x halts m %x s %x se %x rcg %x dfs %x\n", (uint64_t)vote, (uint64_t)halt_m,
	    (uint64_t)halt_s, (uint64_t)halt_se, (uint64_t)rcg_cfg, (uint64_t)rcg_dfs);
	print("nvt-qup: wrapper cc0004 hw %x\n", (uint64_t)nvt_rd(0xcc0004));
	if ((halt_se & 0x80000000u) == 0) {
		fw = nvt_rd(0xc80068);
		print("nvt-qup: se fw %x protocol %x (1 = SPI); clk_sel %x m_clk_cfg %x\n", (uint64_t)fw,
		    (uint64_t)((fw >> 8) & 0xff), (uint64_t)nvt_rd(0xc8007c), (uint64_t)nvt_rd(0xc80048));
	} else {
		print("nvt-qup: the SE clock is off; enabled below before the SE is read\n");
	}

	if (dry) {
		print("nvt-qup: dry run: no TLMM, reset, VDDIO or GCC writes\n");
		return;
	}

	/*
	 * nvt_qup_keep_power (default 1; nvt_qup_keep_power=0 = the old cycle): the NT36675 is flashless and
	 * keeps the firmware Android downloaded only while VDDIO stays on and RESX stays high. If both
	 * already read as GPIO outputs driven high, they are not written and only 55-58 are muxed; the
	 * kernel then finds the running firmware. Otherwise the power was dropped before the loader ran
	 * (the firmware is gone already) and the old power-up runs so the chip at least answers.
	 */
	keep = !nvt_cmdline_word(cmdline, "nvt_qup_keep_power=0");
	reset_up = nvt_pin_out_high(NVT_TLMM_NORTH, NVT_PIN_RESET);
	vddio_up = nvt_pin_out_high(NVT_TLMM_SOUTH, NVT_PIN_VDDIO);
	if (keep && reset_up && vddio_up) {
		print("nvt-qup: keep power: reset and VDDIO already driven high; not touched (firmware kept)\n");
	} else {
		if (keep) {
			print("nvt-qup: keep power: reset %s, VDDIO %s -- power/reset already dropped before the loader; "
			    "the firmware is gone, powering up as before\n", reset_up ? "high" : "NOT high",
			    vddio_up ? "high" : "NOT high");
		}
		keep = 0;
		/* VDDIO on, reset held low, INT as an input; the DT gives VDDIO 4 ms. */
		nvt_pin_set(NVT_TLMM_SOUTH, NVT_PIN_VDDIO, 0, 0, 2, 1, 1);
		nvt_pin_set(NVT_TLMM_NORTH, NVT_PIN_RESET, 0, 3, 8, 1, 0);
		nvt_pin_set(NVT_TLMM_SOUTH, NVT_PIN_INT, 0, 1, 8, 0, 0);
		BS->stall(4000);
	}

	/* 55-58 to qup17, 6 mA, pull-down, peripheral owns the output. */
	for (i = 0; i < 4; i++) {
		nvt_pin_set(NVT_TLMM_SOUTH, NVT_SPI_PIN0 + i, NVT_SPI_FUNC, 1, 6, 0, 0);
	}
	for (i = 0; i < 4; i++) {
		uint32_t cfg = nvt_rd(NVT_PIN_CFG(NVT_TLMM_SOUTH, NVT_SPI_PIN0 + i));

		if (((cfg >> 2) & 0xf) != NVT_SPI_FUNC) {
			print("nvt-qup: pin %x did not take qup17 (cfg %x); reset stays low\n",
			    (uint64_t)(NVT_SPI_PIN0 + i), (uint64_t)cfg);
			return;
		}
	}

	if (keep) {
		print("nvt-qup: 55-58 on qup17; reset and VDDIO left as found\n");
	} else {
		/* Release reset, then the DT's 10 ms before the first command. */
		nvt_pin_set(NVT_TLMM_NORTH, NVT_PIN_RESET, 0, 3, 8, 1, 1);
		BS->stall(10000);
		print("nvt-qup: reset released, VDDIO on, 55-58 on qup17\n");
	}

	/*
	 * The SE clock: only with nvt_qup_gcc=1 as well. Without it XNU runs the
	 * same sequence itself (vayu_qup_spi.c), so a loader fault here can be
	 * told apart from one in the pin work above.
	 */
	if (!nvt_cmdline_word(cmdline, "nvt_qup_gcc=1")) {
		print("nvt-qup: gcc left to XNU (no nvt_qup_gcc=1)\n");
		return;
	}
	print("nvt-qup: gcc setup (nvt_qup_gcc=1): vote %x rcg cmd %x\n", (uint64_t)nvt_rd(NVT_GCC + NVT_GCC_VOTE),
	    (uint64_t)nvt_rd(NVT_GCC + NVT_RCG_CMD));
	if (!nvt_gcc_setup()) {
		print("nvt-qup: gcc setup failed; XNU will try its own sequence\n");
		return;
	}
	print("nvt-qup: reading the SE fw revision at c80068 (first SE access)\n");
	fw = nvt_rd(0xc80068);
	print("nvt-qup: se fw %x protocol %x (1 = SPI); wrapper hw %x; XNU can use the SE\n", (uint64_t)fw,
	    (uint64_t)((fw >> 8) & 0xff), (uint64_t)nvt_rd(0xcc0004));
}

/* ------------------------------------------------------------------------ */
/* vayu camera: the camera pins, for XNU's camera probe.                    */
/*                                                                          */
/* As for touch and the amplifiers, XNU does not write TLMM on this phone:  */
/* the loader owns the pin configuration. The camera probe only toggles     */
/* output levels and, for its bit-banged CCI, the output-enable bit; it     */
/* refuses to run unless the loader muxed the pins as below. This runs      */
/* only when the payload's command line says vayu_cam_pins=1.               */
/*                                                                          */
/* Tiles and register layout (vendor pinctrl-sm8150.c:28-31 with the tlmm   */
/* base 0x03000000, sm8150-pinctrl.dtsi:14-16): NORTH 0x03900000, EAST      */
/* 0x03500000, SOUTH 0x03D00000, WEST 0x03100000. A pin's cfg register is   */
/* at tile + 0x1000 * pin, io at cfg + 4: [1:0] pull (0 none, 1 down, 3     */
/* up), [5:2] function, [8:6] drive ((mA / 2) - 1), [9] output enable; io   */
/* bit 0 is the input level, bit 1 the output level.                        */
/*                                                                          */
/* The pins are the camera's (vayu-sm8150-camera-sensor-mtp.dtsi:156-503,   */
/* vayu-pinctrl.dtsi:357-610, sm8150-pinctrl.dtsi:4328-4440,                */
/* pinctrl-sm8150.c:1599-1780):                                             */
/*   GPIO 13-16 NORTH  MCLK0-3: cam_mclk is function 1, bias-disable, 4 mA, */
/*                     input (vayu-pinctrl.dtsi mclk*_active)               */
/*   GPIO 17-20, 31-34 NORTH  CCI0/CCI1 SDA/SCL: function 0 (GPIO),         */
/*                     pull-up, 2 mA, input; XNU bit-bangs them, low = OE 1 */
/*                     with io 0, high = OE 0                               */
/*   GPIO 23,28,30 EAST; 54,59,118 SOUTH                                    */
/*                     sensor resets and vdig enables:                      */
/*                     function 0, no pull, 2 mA, output, level low (io     */
/*                     written before OE, as nvt_pin_set() does)            */
/* Not touched: 27 (hi259 reset), 84 (hi259 standby; hi259 is not probed),  */
/* 94 (vana enable) and 125 (ov02b1b standby): 27, 94 and 125 have a second */
/* owner in the phone's live DT (panel ESD irq, msm_tspp tsif1, aqt1000),   */
/* so they are outside TLMM exception 3 (the bring-up notes).         */
/* Every write is read back and logged; a value that does not take is       */
/* reported. No clock or other register is touched.                         */
/* ------------------------------------------------------------------------ */

#define CAM_TLMM_EAST   0x03500000ULL

enum cam_pin_kind {
	CAM_MCLK,
	CAM_I2C,
	CAM_OUT,
};

struct cam_pin {
	unsigned pin;
	uint64_t tile;
	const char *role;
	unsigned kind;
};

static const struct cam_pin cam_pins[] = {
	{ 13,  NVT_TLMM_NORTH, "MCLK0 (s5k3t2 front)",                 CAM_MCLK },
	{ 14,  NVT_TLMM_NORTH, "MCLK1 (imx582 rear)",                  CAM_MCLK },
	{ 15,  NVT_TLMM_NORTH, "MCLK2 (ov02b1b depth)",                CAM_MCLK },
	{ 16,  NVT_TLMM_NORTH, "MCLK3 (ov8856 ultra, hi259 macro)",    CAM_MCLK },
	{ 17,  NVT_TLMM_NORTH, "CCI0 M0 SDA",                          CAM_I2C },
	{ 18,  NVT_TLMM_NORTH, "CCI0 M0 SCL",                          CAM_I2C },
	{ 19,  NVT_TLMM_NORTH, "CCI0 M1 SDA",                          CAM_I2C },
	{ 20,  NVT_TLMM_NORTH, "CCI0 M1 SCL",                          CAM_I2C },
	{ 31,  NVT_TLMM_NORTH, "CCI1 M0 SDA",                          CAM_I2C },
	{ 32,  NVT_TLMM_NORTH, "CCI1 M0 SCL",                          CAM_I2C },
	{ 33,  NVT_TLMM_NORTH, "CCI1 M1 SDA",                          CAM_I2C },
	{ 34,  NVT_TLMM_NORTH, "CCI1 M1 SCL",                          CAM_I2C },
	{ 23,  CAM_TLMM_EAST,  "ov02b1b reset",                        CAM_OUT },
	{ 24,  CAM_TLMM_EAST,  "actuator VAF enable (gpio-regulator, BOB 2.8 V)", CAM_OUT },   /* exception 3, amendment 3 */
	{ 28,  CAM_TLMM_EAST,  "ov8856 reset",                         CAM_OUT },
	{ 30,  CAM_TLMM_EAST,  "imx582 reset",                         CAM_OUT },
	{ 54,  NVT_TLMM_SOUTH, "s5k3t2 reset",                         CAM_OUT },
	{ 59,  NVT_TLMM_SOUTH, "imx582 vdig enable",                   CAM_OUT },
	{ 118, NVT_TLMM_SOUTH, "s5k3t2 vdig enable",                   CAM_OUT },
};

/* Program one pin, log old -> new with the readback, and say whether it took. */
static int
cam_pin_apply(const struct cam_pin *p)
{
	uint64_t cfg_pa = NVT_PIN_CFG(p->tile, p->pin), io_pa = NVT_PIN_IO(p->tile, p->pin);
	uint32_t cfg = nvt_rd(cfg_pa), io = nvt_rd(io_pa), want, got;
	unsigned func, pull, drive;
	int output;

	switch (p->kind) {
	case CAM_MCLK:
		func = 1;                       /* cam_mclk */
		pull = 0;                       /* bias-disable */
		drive = 4;
		output = 0;
		break;
	case CAM_I2C:
		func = 0;                       /* GPIO: XNU bit-bangs CCI */
		pull = 3;                       /* pull-up */
		drive = 2;
		output = 0;
		break;
	default:                                /* CAM_OUT */
		func = 0;
		pull = 0;                       /* bias-disable */
		drive = 2;
		output = 1;
		break;
	}
	want = (cfg & ~0x3ffu) | ((func & 0xf) << 2) | (pull & 3) | ((((drive / 2) - 1) & 7) << 6) |
	    (output ? (1u << 9) : 0);
	if (output) {
		nvt_wr(io_pa, 0);               /* level low before OE, as nvt_pin_set() */
	}
	nvt_wr(cfg_pa, want);
	got = nvt_rd(cfg_pa);
	print("vayu-cam: gpio%d %s: cfg %x io %x -> cfg %x io %x (mux %x, pull %x, drive %x, oe %x)\n",
	    (uint64_t)p->pin, p->role, (uint64_t)cfg, (uint64_t)io, (uint64_t)got, (uint64_t)nvt_rd(io_pa),
	    (uint64_t)((got >> 2) & 0xf), (uint64_t)(got & 3), (uint64_t)((got >> 6) & 7), (uint64_t)((got >> 9) & 1));
	if ((got & 0x3ffu) != (want & 0x3ffu)) {
		print("vayu-cam: gpio%d DID NOT TAKE (wanted cfg %x)\n", (uint64_t)p->pin, (uint64_t)want);
		return 0;
	}
	return 1;
}

static void
vayu_cam_pins(const char *cmdline)
{
	unsigned n = 0, failed = 0;

	/* The caller checks this too; any value but =1 must do nothing. */
	if (!nvt_cmdline_word(cmdline, "vayu_cam_pins=1")) {
		return;
	}
	print("vayu-cam: setting up the camera pins (vayu_cam_pins=1)\n");
	for (unsigned i = 0; i < sizeof(cam_pins) / sizeof(cam_pins[0]); i++) {
		if (!cam_pin_apply(&cam_pins[i])) {
			failed++;
		}
		n++;
	}
	print("vayu-cam: camera pins done (%d pins, %d did not take)\n", (uint64_t)n, (uint64_t)failed);
}

/* vayu IR blaster: GPIO 114-117 for spi@884000 (QUP wrapper 0 SE1, irled@0 */
/* "ir-spi"), TLMM exception 7 (maintainers, 2026-10-25), loader only, only    */
/* with vayu_ir_pins=1. The DT's qupv3_se1_spi_active (vayu.dtb):           */
/* function qup1 (index 1), 6 mA (drive code 2), bias-disable, all four in  */
/* the SOUTH tile (pinctrl-sm8150.c:1425-1428). Only mux/pull/drive bits    */
/* (cfg [8:0]) change; OE is left clear (the SE drives the pins). Each      */
/* write is read back and logged. Nothing else is touched.                  */
static void
vayu_ir_pins(const char *cmdline)
{
	unsigned failed = 0;

	if (!nvt_cmdline_word(cmdline, "vayu_ir_pins=1")) {
		return;
	}
	print("vayu-ir: setting up the IR blaster pins 114-117 (vayu_ir_pins=1)\n");
	for (unsigned pin = 114; pin <= 117; pin++) {
		uint64_t cfg_pa = NVT_PIN_CFG(NVT_TLMM_SOUTH, pin);
		uint32_t cfg = nvt_rd(cfg_pa), want, got;

		want = (cfg & ~0x3ffu) | (1u << 2) | (2u << 6);        /* function 1, no pull, drive code 2 (6 mA), OE 0 */
		nvt_wr(cfg_pa, want);
		got = nvt_rd(cfg_pa);
		print("vayu-ir: gpio%d: cfg %x -> %x%s\n", (uint64_t)pin, (uint64_t)cfg, (uint64_t)got,
		    (got & 0x3ffu) == (want & 0x3ffu) ? "" : " DID NOT TAKE");
		failed += (got & 0x3ffu) != (want & 0x3ffu);
	}
	print("vayu-ir: pins done, %d did not take\n", (uint64_t)failed);
}

/* vayu IR blaster: QUP wrapper 0 SE1's clock (GCC exception 7a, maintainers,  */
/* 2026-10-25), loader only, only with vayu_ir_clk=1. Vendor vayu-r-oss      */
/* gcc-sm8150.c: vote word 0x5200c bit 6 wrap0 m_ahb (halt 0x17004,         */
/* l.2965-2977), bit 7 wrap0 s_ahb (halt 0x17008, l.2978-2990), bit 11 SE1  */
/* (halt 0x17274, l.2623-2628); SE1's RCG cmd 0x17278, cfg 0x1727c, DFSR    */
/* cmd + 0x14 (l.665-670; clk-rcg2.c as for SE3). Only while DFS is off,    */
/* the RCG goes to XO (src 0, parent_map_0 l.60-65) divided by 1: 19.2 MHz  */
/* (ftbl F(19200000, P_BI_TCXO, 1, 0, 0)); XNU's vayu_ir.c divides by 10 in  */
/* the SE for the vendor ir-spi's 1.92 MHz. Each write read back; each      */
/* branch halt-checked (bounded); nothing else is touched.                  */
#define NVT_IR_GCC_VOTE   0x5200c
#define NVT_IR_VOTE_M_AHB (1u << 6)
#define NVT_IR_VOTE_S_AHB (1u << 7)
#define NVT_IR_VOTE_SE1   (1u << 11)
#define NVT_IR_HALT_M_AHB 0x17004
#define NVT_IR_HALT_S_AHB 0x17008
#define NVT_IR_HALT_SE1   0x17274
#define NVT_IR_RCG_CMD    0x17278
#define NVT_IR_RCG_CFG    0x1727c
#define NVT_IR_RCG_DFS    0x1728c       /* cmd + 0x14 */
#define NVT_IR_SE_BASE    0x00884000ULL

static void
vayu_ir_clk(const char *cmdline)
{
	uint32_t vote;
	int i, on;

	if (!nvt_cmdline_word(cmdline, "vayu_ir_clk=1")) {
		return;
	}
	vote = nvt_rd(NVT_GCC + NVT_IR_GCC_VOTE);
	print("vayu-ir: clock (vayu_ir_clk=1): before vote %x (m_ahb %d, s_ahb %d, se1 %d), halts %x %x %x, rcg cmd %x cfg %x dfs %x\n",
	    (uint64_t)vote, (uint64_t)((vote >> 6) & 1), (uint64_t)((vote >> 7) & 1), (uint64_t)((vote >> 11) & 1),
	    (uint64_t)nvt_rd(NVT_GCC + NVT_IR_HALT_M_AHB), (uint64_t)nvt_rd(NVT_GCC + NVT_IR_HALT_S_AHB),
	    (uint64_t)nvt_rd(NVT_GCC + NVT_IR_HALT_SE1), (uint64_t)nvt_rd(NVT_GCC + NVT_IR_RCG_CMD),
	    (uint64_t)nvt_rd(NVT_GCC + NVT_IR_RCG_CFG), (uint64_t)nvt_rd(NVT_GCC + NVT_IR_RCG_DFS));
	if ((vote & (NVT_IR_VOTE_M_AHB | NVT_IR_VOTE_S_AHB)) != (NVT_IR_VOTE_M_AHB | NVT_IR_VOTE_S_AHB)) {
		nvt_gcc_wr(NVT_IR_GCC_VOTE, nvt_rd(NVT_GCC + NVT_IR_GCC_VOTE) | NVT_IR_VOTE_M_AHB | NVT_IR_VOTE_S_AHB, "ir vote (wrap0 ahb)");
	}
	on = nvt_gcc_branch_on(NVT_IR_HALT_M_AHB, "wrap0 m_ahb", 0);
	if (!nvt_gcc_branch_on(NVT_IR_HALT_S_AHB, "wrap0 s_ahb", 1)) {
		on = 0;
	}
	if (!on) {
		print("vayu-ir: clock: wrapper 0 AHB did not come on: SE1 left alone\n");
		return;
	}
	nvt_gcc_wr(NVT_IR_GCC_VOTE, nvt_rd(NVT_GCC + NVT_IR_GCC_VOTE) | NVT_IR_VOTE_SE1, "ir vote (se1)");
	on = nvt_gcc_branch_on(NVT_IR_HALT_SE1, "wrap0 se1", 0);
	if (nvt_rd(NVT_GCC + NVT_IR_RCG_DFS) & 1) {
		print("vayu-ir: clock: se1 rcg is in DFS: rate left to its levels (XNU uses CLK_SEL)\n");
	} else {
		uint32_t c = nvt_rd(NVT_GCC + NVT_IR_RCG_CFG);

		nvt_gcc_wr(NVT_IR_RCG_CFG, (c & ~(0x1fu | (0x7u << 8) | (0x3u << 12))) | 1u, "ir se1 rcg cfg (xo/1)");
		nvt_gcc_wr(NVT_IR_RCG_CMD, nvt_rd(NVT_GCC + NVT_IR_RCG_CMD) | NVT_RCG_UPDATE, "ir se1 rcg update");
		for (i = 0; i < 100 && (nvt_rd(NVT_GCC + NVT_IR_RCG_CMD) & NVT_RCG_UPDATE); i++) {
			BS->stall(10);
		}
		print("vayu-ir: clock: se1 rcg cmd %x cfg %x%s\n", (uint64_t)nvt_rd(NVT_GCC + NVT_IR_RCG_CMD),
		    (uint64_t)nvt_rd(NVT_GCC + NVT_IR_RCG_CFG), (nvt_rd(NVT_GCC + NVT_IR_RCG_CMD) & (NVT_RCG_UPDATE | NVT_RCG_ROOT_OFF)) ?
		    ": UPDATE DID NOT COMPLETE or root off" : ": 19.2 MHz");
	}
	print("vayu-ir: clock: done: se1 %s, fw revision %x (protocol %x, 1 = SPI)\n", on ? "on" : "OFF",
	    (uint64_t)nvt_rd(NVT_IR_SE_BASE + 0x68), (uint64_t)((nvt_rd(NVT_IR_SE_BASE + 0x68) >> 8) & 0xff));
}

/* vayu Bluetooth: QUP wrapper 2 SE3's clock vote and GPIO 43-46, for XNU's */
/* bsd/dev/vayu_bt.c.                                                       */
/*                                                                          */
/* The WCN3990 hangs on SE3 (registers at 0xc8c000) behind QUP wrapper 2.   */
/* XNU writes neither TLMM nor GCC on this phone (this port's rule, as for    */
/* touch and audio): the loader prepares the clocks and the pins here.      */
/* GPIO 43-46 are the UART's CTS, RTS, TX and RX in the TLMM EAST tile      */
/* (0x03500000; cfg at +0x1000 * pin, io at cfg + 4). The live DT's active  */
/* state is qup13, function index 1, 2 mA (drive code 0), input (OE 0):     */
/* 44 pulls down, 45 pulls up, 43 and 46 have no pull.                      */
/*                                                                          */
/* GCC is at 0x00100000. Vote word 0x52014 (gcc-sm8150.c:2911-2916): bit 1  */
/* wrapper 2 s_ahb, bit 2 wrapper 2 m_ahb, bit 7 wrapper 2 SE3. The halt    */
/* registers are 0x1e008, 0x1e004 and 0x1e4d4; bit 31 reads 1 while a clock */
/* is off. s_ahb is hardware-gated (its CBCR keeps bit 31 set while idle),  */
/* so nvt_gcc_branch_on() gets hwcg = 1 for it. SE3's RCG is cmd 0x1e4d8,   */
/* cfg 0x1e4dc and dfs 0x1e4ec (cmd + 0x14); the vendor kernel leaves it in */
/* DFS mode as XBL set it (gcc-sm8150.c:4188), so this loader never writes  */
/* those three: it reads and prints them only.                              */
/*                                                                          */
/* The SE (fw revision 0xc8c068, IF_DISABLE 0xc8c064) is read only when     */
/* wrapper 2's AHB path runs; no SE address at or above 0xc8e000 is read.   */
/*                                                                          */
/*   vayu_bt_loader=1  reads only: the vote, the three halt registers, the  */
/*                     RCG and the pins, then the SE if its AHB is already  */
/*                     running; nothing is written at all.                  */
/*   vayu_bt_loader=2  the same, then votes and checks s_ahb and m_ahb,     */
/*                     votes and checks SE3, and muxes GPIO 43-46. Only the */
/*                     vote word (read-modify-write, bits 1/2/7) and the    */
/*                     four pin cfg registers are written.                  */
/* ------------------------------------------------------------------------ */

#define NVT_TLMM_EAST     0x03500000ULL
#define NVT_BT_PIN0       43
#define NVT_BT_PINS       4
#define NVT_VOTE_SE3      (1u << 7)     /* wrapper 2 SE3 (gcc-sm8150.c:2911-2916) */
#define NVT_BT_HALT_SE3   0x1e4d4
#define NVT_BT_RCG_CMD    0x1e4d8
#define NVT_BT_RCG_CFG    0x1e4dc
#define NVT_BT_RCG_DFS    0x1e4ec       /* cmd + 0x14 */
#define NVT_BT_SE_BASE    0x00c8c000ULL
#define NVT_BT_SE_FWREV   0x68          /* GENI_FW_REVISION_RO */
#define NVT_BT_SE_IFDIS   0x64          /* GENI_IF_DISABLE_RO */

static void
vayu_bt_pin_show(const char *when, unsigned pin, const char *role)
{
	uint32_t cfg = nvt_rd(NVT_PIN_CFG(NVT_TLMM_EAST, pin));
	uint32_t io = nvt_rd(NVT_PIN_IO(NVT_TLMM_EAST, pin));

	print("vayu-bt: loader: %s gpio%d %s: cfg %x io %x (mux %x, pull %x, drive %x, oe %x, in %x)\n", when,
	    (uint64_t)pin, role, (uint64_t)cfg, (uint64_t)io, (uint64_t)((cfg >> 2) & 0xf), (uint64_t)(cfg & 3),
	    (uint64_t)((cfg >> 6) & 7), (uint64_t)((cfg >> 9) & 1), (uint64_t)(io & 1));
}

static void
vayu_bt_loader(const char *cmdline)
{
	static const char *const role[NVT_BT_PINS] = { "CTS", "RTS", "TX", "RX" };
	static const uint32_t pull[NVT_BT_PINS] = { 0, 1, 3, 0 };
	uint32_t vote, halt_m, halt_s, halt_se3, rcg_cmd, rcg_cfg, rcg_dfs;
	uint32_t want, got, io, fw, ifdis;
	int mode, i, ahb_on = 0, se3_on, pins_ok;

	if (nvt_cmdline_word(cmdline, "vayu_bt_loader=2")) {
		mode = 2;
	} else if (nvt_cmdline_word(cmdline, "vayu_bt_loader=1")) {
		mode = 1;
	} else {
		return;
	}
	print("vayu-bt: loader: mode %d (1: reads only, 2: SE3 clock vote + pins)\n", (uint64_t)mode);

	/* What the bootloader left, before anything is written. */
	vote = nvt_rd(NVT_GCC + NVT_GCC_VOTE);
	halt_m = nvt_rd(NVT_GCC + NVT_HALT_M_AHB);
	halt_s = nvt_rd(NVT_GCC + NVT_HALT_S_AHB);
	halt_se3 = nvt_rd(NVT_GCC + NVT_BT_HALT_SE3);
	print("vayu-bt: loader: before vote %x (s_ahb %d, m_ahb %d, se3 %d)\n", (uint64_t)vote,
	    (uint64_t)((vote & NVT_VOTE_S_AHB) ? 1 : 0), (uint64_t)((vote & NVT_VOTE_M_AHB) ? 1 : 0),
	    (uint64_t)((vote & NVT_VOTE_SE3) ? 1 : 0));
	print("vayu-bt: loader: before halts m_ahb %x (%s), s_ahb %x (%s), se3 %x (%s)\n", (uint64_t)halt_m,
	    (halt_m & NVT_CLK_OFF) ? "OFF" : "on", (uint64_t)halt_s, (halt_s & NVT_CLK_OFF) ? "OFF" : "on",
	    (uint64_t)halt_se3, (halt_se3 & NVT_CLK_OFF) ? "OFF" : "on");
	rcg_cmd = nvt_rd(NVT_GCC + NVT_BT_RCG_CMD);
	rcg_cfg = nvt_rd(NVT_GCC + NVT_BT_RCG_CFG);
	rcg_dfs = nvt_rd(NVT_GCC + NVT_BT_RCG_DFS);
	print("vayu-bt: loader: before se3 rcg cmd %x cfg %x dfs %x (dfs %s)\n", (uint64_t)rcg_cmd,
	    (uint64_t)rcg_cfg, (uint64_t)rcg_dfs, (rcg_dfs & 1) ? "enabled" : "off");
	for (i = 0; i < NVT_BT_PINS; i++) {
		vayu_bt_pin_show("before", NVT_BT_PIN0 + i, role[i]);
	}

	se3_on = (halt_se3 & NVT_CLK_OFF) == 0;
	if (mode == 2) {
		if ((vote & (NVT_VOTE_M_AHB | NVT_VOTE_S_AHB)) != (NVT_VOTE_M_AHB | NVT_VOTE_S_AHB)) {
			nvt_gcc_wr(NVT_GCC_VOTE, nvt_rd(NVT_GCC + NVT_GCC_VOTE) | NVT_VOTE_M_AHB | NVT_VOTE_S_AHB,
			    "bt vote (ahb)");
		}
		ahb_on = nvt_gcc_branch_on(NVT_HALT_M_AHB, "m_ahb", 0);
		if (!nvt_gcc_branch_on(NVT_HALT_S_AHB, "s_ahb", 1)) {
			ahb_on = 0;
		}
		if (!ahb_on) {
			print("vayu-bt: loader: wrapper 2 AHB did not come on: SE3 clock and pins left alone\n");
		} else {
			nvt_gcc_wr(NVT_GCC_VOTE, nvt_rd(NVT_GCC + NVT_GCC_VOTE) | NVT_VOTE_SE3, "bt vote (se3)");
			se3_on = nvt_gcc_branch_on(NVT_BT_HALT_SE3, "se3", 0);
			/*
			 * The SE3 rate. Run 031412 (bt1): XBL leaves this RCG with DFS off (dfs word 0x8020,
			 * all DFSR 0) at XO/bypass (cfg 0x100000) -- 19.2 MHz has no integer divider for
			 * 115200 x 32. So, only while DFS is off, the loader programs the vendor's first
			 * table entry, 7 372 800 Hz = GPLL0_EVEN (src 6) / 1 * 384 / 15625 (vendor
			 * gcc-sm8150.c:624-641, clk_rcg2_configure() clk-rcg2.c:440-490: M = m, N = ~(n - m),
			 * D = ~n, CFG = hid 1 | src 6 | dual-edge mode, CFG bit 20 kept), then UPDATE -- after the SE3 branch is on, so the root runs (bt1: cmd 0x80000000 = root off before the vote).
			 * With DFS on nothing is written (the vendor kernel never writes a DFS RCG).
			 */
			if (nvt_rd(NVT_GCC + NVT_BT_RCG_DFS) & 1) {
				print("vayu-bt: loader: se3 rcg is in DFS: rate left to its levels\n");
			} else {
				uint32_t c = nvt_rd(NVT_GCC + NVT_BT_RCG_CFG);

				print("vayu-bt: loader: gpll0 mode %x user_ctl %x (bit 31 lock, user_ctl bit 1 even out)\n",
				    (uint64_t)nvt_rd(NVT_GCC + 0x0), (uint64_t)nvt_rd(NVT_GCC + 0xc));
				nvt_gcc_wr(NVT_BT_RCG_CMD + 0x8, 384, "bt se3 rcg m");
				nvt_gcc_wr(NVT_BT_RCG_CMD + 0xc, ~(15625u - 384u) & 0xffffu, "bt se3 rcg n");
				nvt_gcc_wr(NVT_BT_RCG_CMD + 0x10, ~15625u & 0xffffu, "bt se3 rcg d");
				nvt_gcc_wr(NVT_BT_RCG_CFG, (c & ~(0x1fu | (0x7u << 8) | (0x3u << 12))) | 1u | (6u << 8) |
				    (2u << 12), "bt se3 rcg cfg (gpll0_even, mnd)");
				nvt_gcc_wr(NVT_BT_RCG_CMD, nvt_rd(NVT_GCC + NVT_BT_RCG_CMD) | NVT_RCG_UPDATE, "bt se3 rcg update");
				for (i = 0; i < 100 && (nvt_rd(NVT_GCC + NVT_BT_RCG_CMD) & NVT_RCG_UPDATE); i++) {
					BS->stall(10);
				}
				print("vayu-bt: loader: se3 rcg cmd %x cfg %x m %x n %x d %x%s\n",
				    (uint64_t)nvt_rd(NVT_GCC + NVT_BT_RCG_CMD), (uint64_t)nvt_rd(NVT_GCC + NVT_BT_RCG_CFG),
				    (uint64_t)nvt_rd(NVT_GCC + NVT_BT_RCG_CMD + 0x8), (uint64_t)nvt_rd(NVT_GCC + NVT_BT_RCG_CMD + 0xc),
				    (uint64_t)nvt_rd(NVT_GCC + NVT_BT_RCG_CMD + 0x10),
				    (nvt_rd(NVT_GCC + NVT_BT_RCG_CMD) & (NVT_RCG_UPDATE | NVT_RCG_ROOT_OFF)) ?
				    ": UPDATE DID NOT COMPLETE or root off" : ": 7372800 Hz");
			}
			for (i = 0; i < NVT_BT_PINS; i++) {
				unsigned pin = NVT_BT_PIN0 + i;
				uint64_t cfg_pa = NVT_PIN_CFG(NVT_TLMM_EAST, pin);
				uint32_t cfg = nvt_rd(cfg_pa);

				want = (cfg & ~0x3ffu) | (1u << 2) | pull[i] | (0u << 6);
				nvt_wr(cfg_pa, want);
				got = nvt_rd(cfg_pa);
				io = nvt_rd(NVT_PIN_IO(NVT_TLMM_EAST, pin));
				print("vayu-bt: loader: gpio%d %s: cfg %x -> %x (mux %x, pull %x, drive %x, oe %x), io %x\n",
				    (uint64_t)pin, role[i], (uint64_t)cfg, (uint64_t)got, (uint64_t)((got >> 2) & 0xf),
				    (uint64_t)(got & 3), (uint64_t)((got >> 6) & 7), (uint64_t)((got >> 9) & 1),
				    (uint64_t)io);
				if ((got & 0x3ff) != (want & 0x3ff)) {
					print("vayu-bt: loader: gpio%d DID NOT TAKE (wanted %x)\n", (uint64_t)pin,
					    (uint64_t)want);
				}
			}
		}
	} else {
		ahb_on = (nvt_rd(NVT_GCC + NVT_HALT_M_AHB) & NVT_CLK_OFF) == 0 &&
		    (nvt_rd(NVT_GCC + NVT_GCC_VOTE) & (NVT_VOTE_M_AHB | NVT_VOTE_S_AHB)) ==
		    (NVT_VOTE_M_AHB | NVT_VOTE_S_AHB);
	}

	if (ahb_on) {
		fw = nvt_rd(NVT_BT_SE_BASE + NVT_BT_SE_FWREV);
		ifdis = nvt_rd(NVT_BT_SE_BASE + NVT_BT_SE_IFDIS);
		print("vayu-bt: loader: after se3 fw revision %x protocol %x (2 = UART), if_disable %x (FIFO %s)\n",
		    (uint64_t)fw, (uint64_t)((fw >> 8) & 0xff), (uint64_t)ifdis, (ifdis & 1) ? "disabled" : "allowed");
	} else {
		print("vayu-bt: loader: the wrapper 2 AHB clocks are off; the SE is not read\n");
	}

	pins_ok = 0;
	for (i = 0; i < NVT_BT_PINS; i++) {
		uint32_t cfg = nvt_rd(NVT_PIN_CFG(NVT_TLMM_EAST, NVT_BT_PIN0 + i));

		if (mode == 2) {
			if ((cfg & 0x3ffu) == ((1u << 2) | pull[i])) {
				pins_ok++;
			}
		} else if (((cfg >> 2) & 0xf) == 1 && (cfg & 3) == pull[i]) {
			pins_ok++;
		}
	}
	print("vayu-bt: loader: done: mode %d, se3 clock %s, %d of 4 pins as wanted\n", (uint64_t)mode,
	    se3_on ? "on" : "off", (uint64_t)pins_ok);
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

static void usb_restart_after_run(void);
static int usb_restart_enabled = 1;            /* payload cmdline usb_restart=0: no restart after the run */
/*
 * usb_handover=1 (the USB networking's USB networking): the kernel asks for the USB controller (kernel_live usb_take = 1)
 * and this loader then leaves it alone for the rest of the run (usb_given = 1): no polling, no transfers, and
 * no stop() first (Qualcomm's stop may gate the clocks the kernel needs; the kernel halts and resets the
 * core itself). After the run the stack is always stopped and started again (usb_restart_after_run), on
 * every way out of a run. Live logs end at the handover.
 */
static int usb_handover_enabled, usb_given_away;
/*
 * After a kernel's return, no command from the host for IDLE_RESET_SECONDS: the phone is restarted
 * (UEFI ResetSystem, cold), the log having been written to logdump first. A loader the host cannot reach
 * any more (runs 001804, 005616, 022501: after a replug) then comes back by itself instead of waiting for
 * a person; the host side brings it back to the loader as after any reboot. Payload cmdline idle_reset=0
 * turns it off. Every command that arrives starts the count again.
 */
#define IDLE_RESET_SECONDS_PLAIN 120
#define IDLE_RESET_SECONDS_HANDOVER 600     /* usb_handover=1: the host may take minutes to find the loader again (test run, loader 1.8) */
#define IDLE_RESET_SECONDS (usb_handover_enabled ? IDLE_RESET_SECONDS_HANDOVER : IDLE_RESET_SECONDS_PLAIN)
static int idle_reset_enabled = 1;
static uint64_t idle_reset_at;                  /* CNTVCT; 0: not counting */
static unsigned usb_run_changes;                /* (tentative: defined with the USB code below) */
static int usb_rx_queued;

static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
	kernel_started_this_session = 1;        /* (from here on, logdump is this session's: loader 1.8) */
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	if (pi.resident_rd != 0) {
		resident_rd_size = pi.resident_rd;
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0, ufs_requested = 0;
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (bsd/dev/hakios.torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is iOS's (bsd/dev/hakios.camera.c) */
	int camera_requested = 0;
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			kernel_mpidr = (uint64_t)(c[8] - '0') << PLAT->core_shift;
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '2' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '9';
	}
	if (battery_requested) {
		charger_start();
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start();
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	/*
	 * The command line is composed in full here (loader 1.9): XNU's boot_args has room for 607 characters
	 * (BOOT_LINE_LENGTH 608) and a test run's 920 were cut at "vayu_cam" (run 220922). boot_args gets the first 607;
	 * the whole line goes into the device tree as /chosen "hakios-bootargs", which the kernel's PE_boot_args()
	 * prefers when it is there (pexpert/arm/pe_bootargs.c).
	 */
	static char full_cmd[FULL_CMD_LEN];
	memset(full_cmd, 0, sizeof(full_cmd));
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < FULL_CMD_LEN - 1; c++) {
		full_cmd[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < FULL_CMD_LEN - 1; c++) {
			full_cmd[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < FULL_CMD_LEN - 1; c++) {
			full_cmd[n++] = *c;
		}
	}
	if (wdt_armed) {
		for (const char *c = "hv_wdt_pet=1 "; *c && n < FULL_CMD_LEN - 1; c++) {
			full_cmd[n++] = *c;
		}
	}
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < FULL_CMD_LEN - 1; c++) {
			full_cmd[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < FULL_CMD_LEN - 1; shift -= 4) {
			full_cmd[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < FULL_CMD_LEN - 1) full_cmd[n++] = ' ';
		/* the port's partition as this loader knows it: the kernel checks the table against these */
		if (PLAT->darwin != NULL) {
			const char *names[3] = { "ufs_darwin_first=0x", "ufs_darwin_blocks=0x", "ufs_write_first=0x" };
			uint64_t values[3] = { PLAT->darwin->first_lba, PLAT->darwin->last_lba - PLAT->darwin->first_lba + 1,
			    PLAT->darwin->write_first };

			for (int k = 0; k < 3; k++) {
				for (const char *c = names[k]; *c && n < FULL_CMD_LEN - 1; c++) {
					full_cmd[n++] = *c;
				}
				for (int shift = 60; shift >= 0 && n < FULL_CMD_LEN - 1; shift -= 4) {
					full_cmd[n++] = hex[(values[k] >> shift) & 0xf];
				}
				if (n < FULL_CMD_LEN - 1) full_cmd[n++] = ' ';
			}
		}
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < FULL_CMD_LEN - 1; c++) {
			full_cmd[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < FULL_CMD_LEN - 1; shift -= 4) {
			full_cmd[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < FULL_CMD_LEN - 1) full_cmd[n++] = ' ';
	}
	/*
	 * fb_spare=N (1-3): N frames of 12 MiB each (loader 1.6; 1.2-1.5: the panel's 1080 x 2400 x 4) in one piece below 4 GB,
	 * for the display controller to scan out (vayu_mdp.c; its address registers have 32 bits, and XNU could
	 * find no such piece of its own: run 034941). Taken from what UEFI has left once XNU's window is claimed,
	 * once per loader session (kept for the next runs), and handed over as fb_spare_pa= and fb_spare_frames=.
	 */
	for (const char *c = pi.cmdline; *c; c++) {

		if ((c == pi.cmdline || c[-1] == ' ') && starts_with(c, "fb_spare=") && c[9] >= '1' && c[9] <= '3') {
			uint64_t frames = (uint64_t)(c[9] - '0'), frame_pages = (12ULL << 20) >> 12;   /* 12 MiB: a 1125x2436 BGRA surface with its row padding (zero-copy, fb_zero) */
			static const char hex[] = "0123456789abcdef";
			const char *names[2] = { "fb_spare_pa=0x", "fb_spare_frames=0x" };
			uint64_t values[2];

			(void)frame_pages;
			if (fb_spare_get(frames) == 0) {
				break;
			}
			print("fb_spare: %d frames at %x (below 4 GB, outside XNU's window)\n", fb_spare_count, fb_spare_pa);
			values[0] = fb_spare_pa;
			values[1] = fb_spare_count;
			for (int k = 0; k < 2; k++) {
				for (const char *q = names[k]; *q && n < FULL_CMD_LEN - 1; q++) {
					full_cmd[n++] = *q;
				}
				for (int shift = 60; shift >= 0 && n < FULL_CMD_LEN - 1; shift -= 4) {
					full_cmd[n++] = hex[(values[k] >> shift) & 0xf];
				}
				if (n < FULL_CMD_LEN - 1) full_cmd[n++] = ' ';
			}
			break;
		}
	}
	for (const char *c = pi.cmdline; *c && n < FULL_CMD_LEN - 1; c++) {
		full_cmd[n++] = *c;
	}
	for (uint32_t i = 0; i < BOOT_LINE_LENGTH - 1; i++) {
		ba->commandLine[i] = i < n ? full_cmd[i] : 0;
	}
	ba->commandLine[BOOT_LINE_LENGTH - 1] = 0;
	if (n >= BOOT_LINE_LENGTH - 1) {
		print("cmdline: %d characters: boot_args has the first %d, /chosen hakios-bootargs all of it\n", (uint64_t)n,
		    (uint64_t)(BOOT_LINE_LENGTH - 1));
	}

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = READ_SYSREG(CNTFRQ_EL0);
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): iOS user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the whole command line (boot_args holds only its first 607 characters): pexpert/arm/pe_bootargs.c reads it */
	dt_prop_str(&w, "hakios-bootargs", full_cmd);
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes iOS play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it iOS looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (torch_requested) {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = (int)(kernel_mpidr >> plat->core_shift);

	if (more_cores) {
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr = (uint64_t)core << plat->core_shift;
				int64_t state;

				if (core < 0 || core >= plat->cores || mpidr == this_mpidr) {
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (plat->small_cores & (1u << ((cpu_mpidr[i] >> plat->core_shift) & 31))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, 0x100000 };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);

	/*
	 * The touch bus: only when the payload asks for it. XNU cannot write
	 * TLMM on this phone, so the loader owns the pins, reset, VDDIO and the
	 * SE's GCC clocks; the kernel's QUP driver (bsd/dev/vayu_qup_spi.c)
	 * checks the clocks and sets up GENI.
	 */
	charger_paused = nvt_cmdline_word(pi.cmdline, "loader_charger=0");
	if (charger_paused) {
		print("loader_charger=0: no charger polls during this run\n");
	}
	if (nvt_cmdline_word(pi.cmdline, "nvt_qup=1") || nvt_cmdline_word(pi.cmdline, "touch_driver=nvt_qup")) {
		nvt_qup_setup(pi.cmdline);
	}

	/* The speaker amplifiers' pins: only when the payload asks (XNU does not write TLMM: see vayu_audio_pins()). */
	if (aud_word(pi.cmdline, "vayu_audio_pins=1") || aud_word(pi.cmdline, "vayu_audio_pins=2") ||
	    aud_word(pi.cmdline, "vayu_audio_mi2s=1") || aud_word(pi.cmdline, "vayu_audio_mi2s=2")) {
		vayu_audio_pins(pi.cmdline);
	}
	/* The PN553's pins and the optional read-only probe: only when the payload asks (see vayu_nfc_pins()). */
	if (aud_word(pi.cmdline, "vayu_nfc_pins=1") || aud_word(pi.cmdline, "vayu_nfc_pins=2")) {
		vayu_nfc_pins(pi.cmdline);
	}
	usb_restart_enabled = !aud_word(pi.cmdline, "usb_restart=0");
	usb_handover_enabled = aud_word(pi.cmdline, "usb_handover=1");
	idle_reset_enabled = !aud_word(pi.cmdline, "idle_reset=0");
	idle_reset_at = 0;
	usb_run_changes = 0;
	/* The codec's reset pin: see vayu_jack_pins(). */
	if (aud_word(pi.cmdline, "vayu_jack_pins=1") || aud_word(pi.cmdline, "vayu_jack_pins=2")) {
		vayu_jack_pins(pi.cmdline);
	}
	/* The IR blaster's SE1 clock: only with vayu_ir_clk=1 (GCC exception 7a: see vayu_ir_clk()). */
	if (nvt_cmdline_word(pi.cmdline, "vayu_ir_clk=1")) {
		vayu_ir_clk(pi.cmdline);
	}
	/* The IR blaster's pins: only with vayu_ir_pins=1 (TLMM exception 7: see vayu_ir_pins()). */
	if (nvt_cmdline_word(pi.cmdline, "vayu_ir_pins=1")) {
		vayu_ir_pins(pi.cmdline);
	}
	/* The camera's pins: only with vayu_cam_pins=1 (XNU does not write TLMM: see vayu_cam_pins()). */
	if (nvt_cmdline_word(pi.cmdline, "vayu_cam_pins=1")) {
		vayu_cam_pins(pi.cmdline);
	}
	/* The Bluetooth UART's SE3 clock vote and pins: only with vayu_bt_loader=1|2 (see vayu_bt_loader()). */
	if (nvt_cmdline_word(pi.cmdline, "vayu_bt_loader=1") || nvt_cmdline_word(pi.cmdline, "vayu_bt_loader=2")) {
		vayu_bt_loader(pi.cmdline);
	}
	print("cmdline: %s\n", full_cmd);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				usb_restart_after_run();        /* (also after a handover: usb_handover=1) */
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				usb_restart_after_run();
				return 1;
			}
		} else {
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into darwin, %d bytes, read back the same; %d refused\n", block_writes,
		    block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		nfc_ven_off("VEN off (kernel returned)");
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	usb_restart_after_run();
	if (idle_reset_enabled) {
		idle_reset_at = READ_SYSREG(CNTVCT_EL0) + IDLE_RESET_SECONDS * READ_SYSREG(CNTFRQ_EL0);
		print("loader: no command from the host for %d s from now: the phone restarts by itself (idle_reset=0: not)\n",
		    (uint64_t)IDLE_RESET_SECONDS);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static usb_device_descriptor_set usb_set;      /* what start_ex() was given: again after a run that saw the cable change */

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	usb_device_descriptor_set *const set_p = &usb_set;
#define set (*set_p)
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
#undef set
}

static int usb_reconnected;     /* a cable change since the last command that arrived (see usb_rearm_if_stale) */
/*
 * Cable changes while a kernel runs. This core then watches with its interrupts masked (watching_only: XNU
 * owns the GIC), so whatever of Qualcomm's UEFI USB stack runs on interrupts or timers cannot set a new
 * session up after a replug: the device enumerates (handle_event is polled) but no command got through
 * again (test runs 001804, 005616, 022501, loader 0.5-0.8; 0.7's re-arm of the receive was not enough).
 * Each change is noted in the log; after the kernel's return -- the log written to logdump first, the
 * interrupts back -- the stack is stopped and started again (start_ex, the same descriptors), as a
 * soft reconnect, so that the host can fetch the result and the log. usb_restart=0 skips that.
 */
static unsigned usb_run_changes;

static void
usb_restart_after_run(void)
{
	efi_status s;

	if (USB == NULL || (usb_run_changes == 0 && !usb_given_away)) {
		return;
	}
	if (!usb_restart_enabled && !usb_given_away) {
		print("usb: the cable changed %d times during the run; usb_restart=0: the stack is left as it is\n",
		    (uint64_t)usb_run_changes);
		return;
	}
	print("usb: %s: the USB stack is stopped and started again\n", usb_given_away ? "the kernel had the controller" :
	    "the cable changed during the run");
	usb_given_away = 0;
	if (USB->abort_xfer != NULL) {
		USB->abort_xfer(USB_EP_OUT);
		USB->abort_xfer(USB_EP_IN);
	}
	s = USB->stop();
	print("usb: stop %x\n", (uint64_t)s);
	BS->stall(300000);
	s = USB->start_ex(&usb_set);
	print("usb: start_ex %x%s\n", (uint64_t)s, EFI_ERROR(s) ? ": THE HOST CANNOT REACH THIS LOADER NOW" : "");
	usb_connected = 0;
	usb_rx_queued = 0;
	usb_reconnected = 1;
	usb_run_changes = 0;
	write_logs(returned_klog, returned_klog_len);   /* (these lines into logdump too) */
}

static void
usb_note_change(uint32_t state)
{
	if (running_live != NULL) {
		if (usb_run_changes++ < 20) {
			print("usb: the cable %s while the kernel runs (change %d)\n", state == UsbDeviceStateConnected ?
			    "came back" : "went away", (uint64_t)usb_run_changes);
		}
	}
}
static uint64_t usb_rx_since;   /* CNTVCT when the receive now queued was queued */

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		if (idle_reset_at != 0 && running_live == NULL && READ_SYSREG(CNTVCT_EL0) > idle_reset_at) {
			print("loader: no command from the host for %d s after the kernel's return: restarting the phone\n",
			    (uint64_t)IDLE_RESET_SECONDS);
			write_logs(returned_klog, returned_klog_len);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		}
		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			usb_reconnected = 1;
			usb_note_change(data.device_state);
			return -1;
		}
		/* (after a cable change: a receive that has waited 2 s may be one the controller lost; queued again) */
		if (endpoint == USB_EP_OUT && usb_reconnected && READ_SYSREG(CNTVCT_EL0) - began > 2 * freq) {
			USB->abort_xfer(USB_EP_OUT);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */
/*
 * Set by every change of the cable's state, cleared by the next command that arrives. After a cable is
 * pulled and put back, a receive queued as soon as the controller says "connected" -- before the host has
 * configured the device -- can be lost without a notification: usb_rx_queued then stayed 1 for ever and
 * no command was read again, while the host still listed the device (test runs 001804 and 005616: the
 * tester unplugged the cable; the loader answered nothing more and had to be forced off). While this is
 * set, a receive that has waited 2 s is aborted and queued again.
 */

static void
usb_rearm_if_stale(void)
{
	uint64_t now = READ_SYSREG(CNTVCT_EL0);

	if (usb_reconnected && usb_rx_queued && now - usb_rx_since > 2 * READ_SYSREG(CNTFRQ_EL0)) {
		USB->abort_xfer(USB_EP_OUT);
		usb_rx_queued = 0;      /* (queued again by the next poll; a "cancelled" notice for it is ignored) */
	}
}

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
static void
fb_command(char *cmd)
{
	int busy = running_live != NULL;

	{
		if (!busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest"))) {
			returned_klog = NULL;   /* (memory a new payload or run may reuse: the last run's log is not staged again) */
			returned_klog_len = 0;
		}
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap") ||
		    starts_with(cmd, "oem fbspare") || starts_with(cmd, "oem fbtouch"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "2.1");      /* 0.6: idle "oem log" stages the last kernel log; 0.7: USB receive re-armed; 0.8: vayu-ufs; 0.9: USB stack restarted after cable changes; 1.0: restart after 120 s without a host command; 1.1: usb_handover=1; 1.2: fb_spare=N frames below 4 GB; 1.3: the vayu train (cam/bt/jack/nfc pin paths, all opt-in by cmdline); 1.4: vayu_ir_pins=1 (TLMM exception 7); 1.5: vayu_ir_clk=1 (GCC exception 7a); 1.6: fb_spare frames of 12 MiB (zero-copy surfaces); 1.7: fb_spare clear of the vendor's reserved ranges, oem fbspare / oem fbtouch; 1.8: logdump kept at boot if it holds the last run's log, oem prevlog; 1.8a: idle reset 600 s with usb_handover=1; 1.9: the whole command line in /chosen hakios-bootargs; 2.0: vayu darwin (sda33) known, writes served with ufs_write=1; 2.1: vayu_cam_pins also sets GPIO 24 (rear VAF enable) */
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : "0x03f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				uint32_t mark = log_len;
				print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
				log_buf[log_len] = 0;
				fb_reply("OKAY", log_buf + mark);
				log_len = mark;
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = PAYLOAD_MAX_SIZE - 0x100000;
			uint8_t *dest = (uint8_t *)PAYLOAD_BASE;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				got += (uint64_t)r;
			}
			if (got == total) {
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem fbspare")) {
			/* the frames fb_spare=2 would get, allocated and said (then "oem log" and fetch); nothing touched */
			fb_reply(fb_spare_get(2) != 0 ? "OKAY" : "FAIL", "");
		} else if (starts_with(cmd, "oem fbtouch")) {
			/*
			 * The loader itself writes and reads back the first and last word of each fb_spare frame: if that
			 * hangs the phone, the memory is the problem (H1), with no kernel involved. Each step said first.
			 */
			uint64_t pa = fb_spare_get(2);

			logdump_force = 1;      /* (each step must reach logdump before the touch; the last run's log stays in memory: oem prevlog) */
			for (uint64_t f = 0; pa != 0 && f < fb_spare_count; f++) {
				volatile uint32_t *first = (volatile uint32_t *)(uintptr_t)(pa + f * (12ULL << 20));
				volatile uint32_t *last = (volatile uint32_t *)(uintptr_t)(pa + (f + 1) * (12ULL << 20) - 4);

				print("fbtouch: frame %d first word at %x: writing\n", f, (uint64_t)(uintptr_t)first);
				flush_log();
				*first = 0x5a5a1234u;
				print("fbtouch: frame %d first word reads %x; last word at %x: writing\n", f, (uint64_t)*first,
				    (uint64_t)(uintptr_t)last);
				flush_log();
				*last = 0xa5a54321u;
				print("fbtouch: frame %d last word reads %x\n", f, (uint64_t)*last);
			}
			fb_reply(pa != 0 ? "OKAY" : "FAIL", "");
		} else if (starts_with(cmd, "oem prevlog")) {
			/* the run before this boot, as logdump had it (then get_staged) */
			if (prev_log != NULL) {
				staged_data = prev_log;
				staged_len = prev_log_len;
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "logdump held no kernel's log at this boot");
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(returned_klog, returned_klog_len);  /* (the last run's kernel log, if any) */
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (usb_given_away) {
		return;         /* (the kernel has the controller until the run ends) */
	}
	if (usb_handover_enabled && running_live != NULL && running_live->magic2 == KERNEL_LIVE_MAGIC2 &&
	    running_live->magic3 == KERNEL_LIVE_MAGIC3 && running_live->usb_take == 1) {
		if (USB->abort_xfer != NULL) {
			USB->abort_xfer(USB_EP_OUT);
			USB->abort_xfer(USB_EP_IN);
		}
		print("usb: handed to the kernel (usb_handover=1); live logs stop here\n");
		usb_given_away = 1;
		usb_rx_queued = 0;
		((struct kernel_live *)(uintptr_t)running_live)->usb_given = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
		return;
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
		usb_rx_since = READ_SYSREG(CNTVCT_EL0);
	}
	usb_rearm_if_stale();
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		usb_reconnected = 1;
		usb_note_change(data.device_state);
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	usb_reconnected = 0;            /* (a command came: the channel works) */
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		usb_reconnected = 0;    /* (a command came: the channel works) */
		if (idle_reset_at != 0) {
			idle_reset_at = READ_SYSREG(CNTVCT_EL0) + IDLE_RESET_SECONDS * READ_SYSREG(CNTFRQ_EL0);
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what iOS expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	static struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * PHONE_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(PHONE_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	if (dma.control != 0) {
		print("ramfb: control %x after the request\n", (uint64_t)be32(dma.control));
		return;
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = PHONE_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	print("== XNULOADER v3 ==\n");
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0), (uint64_t)(uintptr_t)&__ImageBase);

	install_exception_hook();

	int have_payload = parse_payload(&pi, 0);

	/*
	 * The image says which device it was built for; that is stronger than
	 * guessing from the framebuffer base (which can change with the panel).
	 */
	PLAT = &platform_qemu_virt;
	if (have_payload && pi.platform != NULL) {
		const struct platform *named = platform_by_name(pi.platform, pi.platform_size);

		if (named != NULL) {
			PLAT = named;
		}
	}
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		if (PLAT == &platform_qemu_virt) {
			for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
				if (platforms[i]->fb_base == SCREEN.base) {
					PLAT = platforms[i];
				}
			}
		}
		if (PLAT == &platform_qemu_virt) {
			ramfb_phone_screen();
		}
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	print("platform %s\n", PLAT->name);

	wdt_arm();

	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/* One-shot mode: run the kernel that came with the image, then reset. */
	if (have_payload) {
		run_kernel(0, 0);
	}
	reboot();
	return 0;
}
