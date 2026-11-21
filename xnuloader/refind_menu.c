/*
 * boot_menu=refind: the boot menu drawn by rEFInd (GPL v3; https://www.rodsbooks.com/refind/, git
 * https://git.code.sf.net/p/refind/code, tag v.0.14.2) instead of boot_menu.c's own text menu. Compiled as part
 * of loader.c (it includes this file after boot_menu.c). rEFInd itself is not in this file nor linked with it:
 * it is a separate EFI application, built from its sources with this port's patches by xnuloader/refind/build.sh,
 * and travels in the payload as the entry "refindfs" -- a small FAT image (xnuloader/refind/mkrefindfs.sh):
 *
 *   \EFI\refind\refind_aa64.efi, refind.conf, the theme (xnuloader/refind/mktheme.py), \EFI\hakios\loader.efi
 *   (this loader, the same file as the payload's), whose entries start it with LoadOptions "menu=off" (HakiOS),
 *   "menu=android", "menu=diag"
 *
 * How: the image is offered to UEFI as a read-only Block I/O device in RAM (a vendor device path of its own);
 * the firmware's own disk and FAT drivers give it a file system (ConnectController), and rEFInd is loaded and
 * started from it like from any disk. While rEFInd is up this loader is not running, so before it starts:
 *
 *   - the XNU window is claimed (choose_window), and the payload's RAM where UEFI does not reserve it (QEMU),
 *     so that rEFInd's allocations land elsewhere;
 *   - a protocol of ours (REFIND_HANDOFF_GUID) is installed: the loader.efi that rEFInd starts finds it and hands
 *     its choice back to this one (refind_child() at the top of efi_main), which goes on with the standalone
 *     start from there -- its state (log, watchdog, the layout read, boot_tries) is this instance's;
 *   - the console input is wrapped: only Vol+ (scan 1), Vol- (scan 2) and Power (char 0x0d) reach rEFInd -- the
 *     other chords of ButtonsDxe (Esc, Home, Delete: rEFInd's "hide this entry") are dropped -- and each key is
 *     timed;
 *   - rEFInd (patches/0002-hakios-marks.patch) finds the same protocol and reports its stages ("scanning
 *     volumes", "screen", "graphics", ..., "menu drawn"): "refind: mark +N ms: ..." in the log, flushed each
 *     time, and on the screen until rEFInd draws there itself; it scans only its own volume unless
 *     refind_scan=all; its menu loop reads the keys at least once a second; the 0003 patch adds a mark for each
 *     step of its graphics (each picture loaded and decoded, each Blt until the menu is up);
 *   - what may stop rEFInd's graphics on vayu's Mu, one argument each: refind_safe=1 (no ConsoleControl, no
 *     read-back of the screen, no VideoFill), refind_blt=ours (the GOP's Blt done by this loader: 32-bit
 *     stores, as boot_menu.c draws), refind_conf=nobanner|bmp|plain|text (another refind.conf);
 *   - a 1 s timer event feeds the hypervisor's watchdog (30 s bite) only while rEFInd shows life (a mark or a
 *     key read in the last 10 s); refind_hang= (default 35 s, QEMU 300) without one: a cold reset from the timer.
 *     If rEFInd lives but starts nothing -- boot_menu_timeout + 60 s with no key, or boot_menu_idle (300) s after
 *     the last one -- the next key read (rEFInd's loop) resets, with the log written;
 *   - BS->StartImage is wrapped: an EFI application that is not this loader (one rEFInd found on the phone's
 *     volumes) gets what efi_chain_start gives it (the hypervisor's watchdog off, the firmware's for
 *     boot_efi_watchdog= s, default 300), and its return is a reset, as with m2's entries.
 *
 * Nothing goes back to Mu's BDS: if rEFInd cannot be started, or comes back, the text menu of boot_menu.c is
 * shown instead (its timer starts HakiOS), with "refind:" lines in the log. rEFInd's own exit, shutdown and
 * firmware entries are left out by its configuration (showtools with none); its NVRAM writes are off
 * (use_nvram false, and the vertical_menu patch keeps its variables off NVRAM when its volume is read-only).
 *
 * Under QEMU (platform qemu-virt) UEFI's GOP is a landscape ramfb: it is pointed at 1080x2400 here
 * (refind_qemu_gop) -- the phone-sized ramfb of ramfb_phone_screen() if that took, else memory of its own -- so
 * that rEFInd lays itself out as on the phone, where the screenshots of qemu/run-standalone-test.sh look.
 *
 * Our own work, not taken from another port.
 */

#define REFIND_HANDOFF_GUID  { 0x6b1f6a4e, 0x2d39, 0x4c55, { 0x9a, 0x61, 0x3e, 0x0b, 0x7d, 0x52, 0xc4, 0x18 } }
#define REFIND_DISK_GUID     { 0x0f4e7a3c, 0x81d2, 0x4b6e, { 0xb5, 0x0c, 0x6d, 0x29, 0x14, 0xa7, 0x3e, 0x95 } }
#define REFIND_HANDOFF_MAGIC 0x444e4652u        /* "RFND" */
#define REFIND_PATH          "\\EFI\\refind\\refind_aa64.efi"
#define REFIND_BLOCK         512u
#define REFIND_KEYS_KEPT     16
#define REFIND_GRACE_S       60
#define REFIND_ALIVE_S       10         /* the watchdog is fed only this long after rEFInd's last sign of life */
#define REFIND_HANG_S        35         /* refind_hang= default: no sign of life this long -- a reset */
#define REFIND_HANG_QEMU_S   300        /* (TCG draws 1080x2400 slowly) */
#define EFI_WRITE_PROTECTED  (EFI_ERROR_BIT | 8)
#define EFI_NOT_READY        (EFI_ERROR_BIT | 6)
#define EFI_BAD_BUFFER_SIZE  (EFI_ERROR_BIT | 4)
#define EFI_MEDIA_CHANGED    (EFI_ERROR_BIT | 13)

#define REFIND_CHOICE_IOS     MENU_OPT_OFF
#define REFIND_CHOICE_ANDROID MENU_OPT_ANDROID
#define REFIND_CHOICE_DIAG    MENU_OPT_DIAG

struct refind_handoff {
	uint32_t magic, revision;
	void (EFIAPI *choose)(struct refind_handoff *self, uint32_t choice);     /* never returns */
	/* revision 2: rEFInd's side is patches/0002-hakios-marks.patch (refind/hakios.c) */
	void (EFIAPI *mark)(struct refind_handoff *self, const char16 *what, uint32_t flags);
	uint32_t scan_all;              /* rEFInd looks at every volume (refind_scan=all), else at its own alone */
	uint32_t test_hang;             /* refind_test_hang=1: rEFInd stops for good after its "graphics" mark (a test) */
	/* revision 3: patches/0003-hakios-graphics-marks.patch */
	uint32_t gfx;                   /* refind_safe=1: REFIND_GFX_* -- what rEFInd's graphics leave out */
	uint32_t reserved;
};

#define REFIND_GFX_NO_CONSOLE_CONTROL 1 /* no ConsoleControl SetMode */
#define REFIND_GFX_NO_SCREEN_READ     2 /* the background from the banner, never read back from the screen */
#define REFIND_GFX_FILL_BY_BUFFER     4 /* fills as rows of BufferToVideo, not VideoFill */

#define REFIND_MARK_STAGE  1    /* a stage of rEFInd's start: logged, the log flushed, shown */
#define REFIND_MARK_SCREEN 2    /* rEFInd draws from here on: no more lines of ours on the screen */

#pragma pack(push, 1)
struct refind_vendor_dp {
	efi_device_path h;
	efi_guid guid;
};
struct refind_disk_dp {
	struct refind_vendor_dp vendor;
	efi_device_path end;
};
struct refind_file_dp {
	struct refind_vendor_dp vendor;
	efi_device_path file;
	char16 path[40];
	efi_device_path end;
};
#pragma pack(pop)

static struct {
	int tried, running;
	const struct payload_info *pi;
	efi_block_io bio;
	efi_block_media media;
	const uint8_t *disk;
	uint64_t disk_size;
	efi_handle disk_handle, handoff_handle, image;
	struct refind_disk_dp disk_dp;
	struct refind_file_dp file_dp;
	struct refind_handoff handoff;
	menu_text_input *in;
	efi_status (EFIAPI *read_key)(menu_text_input *self, struct menu_input_key *key);
	efi_status (EFIAPI *start_image)(efi_handle image, uint64_t *exit_data_size, char16 **exit_data);
	void *pet_event;
	uint64_t started_ms, last_key_ms, timeout_s, idle_s;
	uint64_t alive_ms, hang_s, marks;       /* alive: rEFInd's last mark or key read */
	const char *give_up;                    /* set by the timer: the next key read (rEFInd's own loop) resets */
	char last_mark[72];
	char16 options[64];             /* rEFInd's LoadOptions: "refind_aa64.efi -c refind-NAME.conf" (refind_conf=) */
	uint32_t mark_y;
	int screen_given;
	unsigned keys, dropped;
	struct menu_input_key kept[REFIND_KEYS_KEPT];
	int quiet;
} rk;

static void standalone_go(const struct payload_info *pi);
static void refind_echo(void);

/* the Blt of our own (refind_blt): where it draws */
static struct {
	volatile uint32_t *fb;
	uint64_t width, height, pitch;
	int swap;
	void *firmware_blt;
	uint64_t calls;
} rb;

static void
memmove_u32(uint32_t *d, const uint32_t *s, uint64_t n)
{
	if (d < s) {
		for (uint64_t i = 0; i < n; i++) {
			d[i] = s[i];
		}
	} else {
		for (uint64_t i = n; i-- > 0;) {
			d[i] = s[i];
		}
	}
}

static void
refind_put16(char16 *d, const char *s)
{
	while (*s) {
		*d++ = (char16)*s++;
	}
	*d = 0;
}

static void
refind_dp_node(efi_device_path *n, uint8_t type, uint8_t subtype, uint16_t len)
{
	n->type = type;
	n->subtype = subtype;
	n->length[0] = (uint8_t)len;
	n->length[1] = (uint8_t)(len >> 8);
}

/* ------------------------------------------------------------------------ */
/* the FAT image as a read-only disk in RAM                                 */

static efi_status EFIAPI
refind_disk_reset(efi_block_io *self, uint8_t extended)
{
	return EFI_SUCCESS;
}

static efi_status EFIAPI
refind_disk_read(efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buf)
{
	if (media_id != rk.media.media_id) {
		return EFI_MEDIA_CHANGED;
	}
	if (size % REFIND_BLOCK != 0) {
		return EFI_BAD_BUFFER_SIZE;
	}
	if (buf == NULL || lba > rk.media.last_block || size / REFIND_BLOCK > rk.media.last_block + 1 - lba) {
		return EFI_INVALID_PARAMETER;
	}
	memcpy(buf, rk.disk + lba * REFIND_BLOCK, size);
	return EFI_SUCCESS;
}

static efi_status EFIAPI
refind_disk_write(efi_block_io *self, uint32_t media_id, uint64_t lba, uint64_t size, void *buf)
{
	return EFI_WRITE_PROTECTED;
}

static efi_status EFIAPI
refind_disk_flush(efi_block_io *self)
{
	return EFI_SUCCESS;
}

static int
refind_disk_up(void)
{
	static const efi_guid bio_guid = EFI_BLOCK_IO_GUID, dp_guid = EFI_DEVICE_PATH_GUID, fs_guid = EFI_SIMPLE_FS_GUID;
	static const efi_guid disk_guid = REFIND_DISK_GUID;
	efi_status (EFIAPI *install)(efi_handle *, const efi_guid *, int, void *) =
	    (efi_status (EFIAPI *)(efi_handle *, const efi_guid *, int, void *))BS->install_protocol_interface;
	void *fs = NULL;
	efi_status s;

	rk.media.media_id = 1;
	rk.media.present = 1;
	rk.media.read_only = 1;
	rk.media.block_size = REFIND_BLOCK;
	rk.media.io_align = 0;
	rk.media.last_block = rk.disk_size / REFIND_BLOCK - 1;
	rk.bio.revision = 0x00010000;
	rk.bio.media = &rk.media;
	rk.bio.reset = (void *)refind_disk_reset;
	rk.bio.read_blocks = refind_disk_read;
	rk.bio.write_blocks = refind_disk_write;
	rk.bio.flush_blocks = refind_disk_flush;

	refind_dp_node(&rk.disk_dp.vendor.h, 1 /* hardware */, 4 /* vendor */, sizeof(struct refind_vendor_dp));
	rk.disk_dp.vendor.guid = disk_guid;
	refind_dp_node(&rk.disk_dp.end, EFI_DP_TYPE_END, EFI_DP_END_ENTIRE, 4);

	rk.disk_handle = NULL;
	s = install(&rk.disk_handle, &dp_guid, 0, &rk.disk_dp);
	if (!EFI_ERROR(s)) {
		s = install(&rk.disk_handle, &bio_guid, 0, &rk.bio);
	}
	if (EFI_ERROR(s)) {
		print("refind: the disk in RAM not offered to UEFI: %x\n", s);
		return 0;
	}
	s = BS->connect_controller(rk.disk_handle, NULL, NULL, 1);
	if (EFI_ERROR(BS->handle_protocol(rk.disk_handle, &fs_guid, &fs))) {
		print("refind: no file system on the disk in RAM (ConnectController %x)\n", s);
		return 0;
	}
	print("refind: the disk in RAM (%d KB) is a FAT volume\n", rk.disk_size >> 10);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* what runs while rEFInd is on the screen                                   */

static efi_status EFIAPI
refind_read_key(menu_text_input *self, struct menu_input_key *key)
{
	struct menu_input_key k;
	efi_status s;

	rk.alive_ms = menu_now_ms();
	if (rk.give_up != NULL && rk.running) {         /* (rEFInd's loop, not the timer: the log can be written) */
		rk.running = 0;
		screen_quiet = rk.quiet;
		print("refind: %s: a reset\n", rk.give_up);
		reboot();
	}
	for (;;) {
		s = rk.read_key(self, &k);
		if (EFI_ERROR(s)) {
			return s;
		}
		rk.kept[rk.keys % REFIND_KEYS_KEPT] = k;
		rk.keys++;
		rk.last_key_ms = menu_now_ms();
		if ((k.scan == MENU_SCAN_UP || k.scan == MENU_SCAN_DOWN) && k.ch == 0) {
			break;
		}
		if (k.scan == 0 && k.ch == '\r') {
			break;
		}
		rk.dropped++;           /* Vol+ with Vol- (Esc), Power with a volume key (Home, Delete), anything else */
	}
	*key = k;
	return EFI_SUCCESS;
}

/*
 * Every second while rEFInd is up. The watchdog is fed only while rEFInd shows signs of life (a mark, or a read of
 * the keys -- its menu loop reads them at least once a second: the 0002 patch never lets it wait for good). Its
 * own timer starting nothing in time, or no key for boot_menu_idle s after one: the next key read resets (from
 * rEFInd's loop, where the log can still be written). No sign of life for refind_hang= s: a reset from here,
 * without the log (writing the flash from a timer event is not safe); the marks before it are in the log already.
 */
static void EFIAPI
refind_pet(void *event, void *ctx)
{
	uint64_t now = menu_now_ms(), until;

	if (!rk.running) {
		return;
	}
	/* rEFInd's timer starts its default by itself; a key stops that timer, and then the idle time holds */
	until = rk.keys != 0 ? rk.last_key_ms + rk.idle_s * 1000 : rk.started_ms + (rk.timeout_s + REFIND_GRACE_S) * 1000;
	if (now >= until && rk.give_up == NULL) {
		rk.give_up = rk.keys != 0 ? "no key for boot_menu_idle= s, nothing chosen" : "rEFInd's timer started nothing";
	}
	if (now - rk.alive_ms < REFIND_ALIVE_S * 1000) {
		wdt_pet();
		return;
	}
	if (now - rk.alive_ms >= rk.hang_s * 1000) {
		rk.running = 0;
		print("refind: no sign of life from rEFInd for %d s (last mark: %s): a reset\n", (now - rk.alive_ms) / 1000,
		    rk.last_mark[0] ? rk.last_mark : "none");
		refind_echo();
		ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	}
}

static void
refind_log_keys(void)
{
	print("refind: %d keys while rEFInd was up, %d of them dropped", (uint64_t)rk.keys, (uint64_t)rk.dropped);
	for (unsigned i = rk.keys > REFIND_KEYS_KEPT ? rk.keys - REFIND_KEYS_KEPT : 0; i < rk.keys; i++) {
		print(i == (rk.keys > REFIND_KEYS_KEPT ? rk.keys - REFIND_KEYS_KEPT : 0) ? ": %x/%x" : " %x/%x",
		    (uint64_t)rk.kept[i % REFIND_KEYS_KEPT].scan, (uint64_t)rk.kept[i % REFIND_KEYS_KEPT].ch);
	}
	print("\n");
}

static void
refind_bs_crc(void)
{
	efi_status (EFIAPI *crc32)(const void *, uint64_t, uint32_t *) =
	    (efi_status (EFIAPI *)(const void *, uint64_t, uint32_t *))BS->calculate_crc32;
	uint32_t c = 0;

	BS->hdr.crc32 = 0;
	crc32(BS, BS->hdr.header_size, &c);
	BS->hdr.crc32 = c;
}

/* Everything rEFInd's stay changed, put back: before anything else of this loader runs again */
static void
refind_leave(void)
{
	rk.running = 0;
	if (rk.pet_event != NULL) {
		BS->close_event(rk.pet_event);
		rk.pet_event = NULL;
	}
	if (rk.in != NULL && rk.read_key != NULL) {
		rk.in->read_key_stroke = rk.read_key;
		rk.read_key = NULL;
	}
	if (rk.start_image != NULL) {
		BS->start_image = rk.start_image;
		rk.start_image = NULL;
		refind_bs_crc();
	}
	if (rb.firmware_blt != NULL && GOP != NULL) {
		GOP->blt = rb.firmware_blt;
		rb.firmware_blt = NULL;
	}
	screen_quiet = rk.quiet;
	wdt_pet();
}

static int
refind_is_this_loader(efi_handle image)
{
	static const efi_guid li_guid = EFI_LOADED_IMAGE_GUID;
	efi_loaded_image *li = NULL;
	const uint8_t *a, *b = (const uint8_t *)&__ImageBase;
	uint32_t pe = *(const uint32_t *)(b + 0x3c), base_at = pe + 24 + 24;     /* PE32+ ImageBase */

	if (EFI_ERROR(BS->handle_protocol(image, &li_guid, (void **)&li)) || li == NULL || li->image_base == NULL) {
		return 0;
	}
	/* the same PE headers (/Brepro: the time stamp is the file's hash) but for ImageBase, which the firmware's
	   PE loader rewrites when it puts the image elsewhere (this one is at /base already) */
	a = li->image_base;
	return base_at + 8 <= 1024 && memcmp(a, b, base_at) == 0 && memcmp(a + base_at + 8, b + base_at + 8, 1024 - base_at - 8) == 0;
}

static efi_status EFIAPI
refind_start_image(efi_handle image, uint64_t *exit_data_size, char16 **exit_data)
{
	efi_status (EFIAPI *start)(efi_handle, uint64_t *, char16 **) = rk.start_image;
	efi_status s;

	if (refind_is_this_loader(image)) {
		s = start(image, exit_data_size, exit_data);    /* (its choice comes back through the hand-off) */
		refind_leave();
		print("refind: the loader started by rEFInd came back: EFI_STATUS %x: a reset\n", s);
		reboot();
	}
	refind_leave();
	refind_log_keys();
	print("refind: rEFInd starts an EFI application of its own finding\n");
	efi_before_start();     /* (the hypervisor's watchdog off: nothing started from here feeds it) */
	if (efi_host.watchdog_seconds) {
		BS->set_watchdog_timer(efi_host.watchdog_seconds, 0, 0, NULL);
	}
	s = start(image, exit_data_size, exit_data);
	BS->set_watchdog_timer(0, 0, 0, NULL);
	print("refind: the EFI application came back: EFI_STATUS %x: a reset\n", s);
	reboot();
	return s;
}

/*
 * QEMU: the console is quiet while rEFInd is up (its text would land on rEFInd's screen); the last line of the log
 * goes to the serial port (QEMU virt's PL011) by hand instead, for qemu/run-standalone-test.sh to see.
 */
static void
refind_uart(char c)
{
	for (unsigned n = 0; n < 100000 && (*(volatile uint32_t *)(uintptr_t)(PLAT->uart + 0x18) & 0x20); n++) {
	}                               /* (UARTFR.TXFF) */
	*(volatile uint32_t *)(uintptr_t)PLAT->uart = (uint8_t)c;
}

static void
refind_echo(void)
{
	uint32_t i;

	if (PLAT != &platform_qemu_virt || PLAT->uart == 0 || log_len == 0) {
		return;
	}
	for (i = log_len - 1; i > 0 && log_buf[i - 1] != '\n'; i--) {
	}
	for (; i < log_len; i++) {
		if (log_buf[i] == '\n') {
			refind_uart('\r');
		}
		refind_uart(log_buf[i]);
	}
}

/* A line of ours on the screen, under the logo, while nobody else draws there (before rEFInd's graphics) */
static void
refind_say(const char *what)
{
	if (SCREEN.base == 0 || rk.screen_given) {
		return;
	}
	if (rk.mark_y == 0 || rk.mark_y + MENU_GLYPH_H > SCREEN.height - 40) {
		rk.mark_y = SCREEN.height * 2 / 3;
		menu_fill(0, rk.mark_y, SCREEN.width, SCREEN.height - rk.mark_y, MENU_BLACK);
	}
	menu_text(MENU_X / 2, rk.mark_y, 1, MENU_GREY, what);
	rk.mark_y += MENU_GLYPH_H + 8;
}

/* rEFInd's progress (patches/0002-hakios-marks.patch) */
static void EFIAPI
refind_mark(struct refind_handoff *self, const char16 *what, uint32_t flags)
{
	char t[72], line[88] = "rEFInd: ";
	unsigned n = 0;

	rk.alive_ms = menu_now_ms();
	rk.marks++;
	while (what != NULL && what[n] != 0 && n + 1 < sizeof(t)) {
		t[n] = what[n] < 0x80 ? (char)what[n] : '?';
		n++;
	}
	t[n] = 0;
	if (!(flags & REFIND_MARK_STAGE)) {
		return;                 /* (one a volume: counted, not logged) */
	}
	if (str_eq(t, rk.last_mark)) {
		return;                 /* (the menu drawn again) */
	}
	memcpy(rk.last_mark, t, n + 1);
	print("refind: mark +%d ms: %s (%d marks)\n", rk.alive_ms - rk.started_ms, t, rk.marks);
	refind_echo();
	menu_put(line, sizeof(line), t);
	refind_say(line);
	if (flags & REFIND_MARK_SCREEN) {
		rk.screen_given = 1;
	}
	flush_log();
}

/* ------------------------------------------------------------------------ */
/* the hand-off: called by the loader.efi that rEFInd started               */

static const char *
refind_choice_name(uint32_t choice)
{
	return choice == REFIND_CHOICE_IOS ? "HakiOS" : choice == REFIND_CHOICE_ANDROID ? "Android" :
	    choice == REFIND_CHOICE_DIAG ? "Diagnostics" : "?";
}

static void EFIAPI
refind_choose(struct refind_handoff *self, uint32_t choice)
{
	const struct payload_info *pi = rk.pi;

	refind_leave();
	refind_log_keys();
	print("refind: chosen %s (%s)\n", refind_choice_name(choice), rk.keys ? "by hand" : "rEFInd's timer");
	if (SCREEN.base != 0) {
		menu_fill(0, 0, SCREEN.width, SCREEN.height, MENU_BLACK);
	}
	if (choice == REFIND_CHOICE_ANDROID) {
		bcb_menu_android();
		reboot();
	}
	if (choice == REFIND_CHOICE_DIAG) {
		loadopt_menu = REFIND_CHOICE_DIAG;
		boot_menu(pi);          /* the text menu on its diagnostics page: comes back only for HakiOS */
	} else {
		if (choice != REFIND_CHOICE_IOS) {
			print("refind: choice %d is none of ours: HakiOS\n", (uint64_t)choice);
		}
		standalone_by_hand = rk.keys != 0;
	}
	standalone_go(pi);
	resident_from_payload(pi);
	run_kernel(0, 0);
	reboot();
}

/* menu=off|android|diag anywhere in a UTF-16 LoadOptions: the choice, 0 if none */
static uint32_t
refind_options_choice(const efi_loaded_image *li)
{
	static const char *const words[] = { "menu=off", "menu=android", "menu=diag" };
	const char16 *o = li->load_options;
	uint32_t n = li->load_options_size / 2;

	if (o == NULL) {
		return 0;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (i != 0 && o[i - 1] != ' ') {
			continue;
		}
		for (uint32_t w = 0; w < 3; w++) {
			const char *p = words[w];
			uint32_t j = i;

			while (*p && j < n && o[j] == (char16)*p) {
				p++, j++;
			}
			if (*p == 0 && (j == n || o[j] == ' ' || o[j] == 0)) {
				return w + 1;   /* REFIND_CHOICE_IOS, _ANDROID, _DIAG */
			}
		}
	}
	return 0;
}

/*
 * At the very top of efi_main: a loader started with LoadOptions menu=... is either rEFInd's entry -- the choice
 * goes to the loader that started rEFInd, and this never returns -- or one started some other way (no hand-off
 * to find): then it is an ordinary start, and boot_menu() takes the choice instead of showing a menu.
 */
static void
refind_child(efi_handle image)
{
	static const efi_guid li_guid = EFI_LOADED_IMAGE_GUID, ho_guid = REFIND_HANDOFF_GUID;
	efi_loaded_image *li = NULL;
	struct refind_handoff *ho = NULL;
	uint32_t choice;

	if (EFI_ERROR(BS->handle_protocol(image, &li_guid, (void **)&li)) || li == NULL) {
		return;
	}
	choice = refind_options_choice(li);
	if (choice == 0) {
		return;
	}
	if (!EFI_ERROR(BS->locate_protocol(&ho_guid, NULL, (void **)&ho)) && ho != NULL &&
	    ho->magic == REFIND_HANDOFF_MAGIC && ho->choose != NULL) {
		ho->choose(ho, choice);
	}
	loadopt_menu = (int)choice;
}

/* ------------------------------------------------------------------------ */
/* QEMU: the GOP pointed at the phone-sized ramfb, 1080x2400 of it           */

typedef efi_status (EFIAPI *refind_blt_fn)(efi_gop *self, uint32_t *buf, uint32_t op, uint64_t sx, uint64_t sy,
    uint64_t dx, uint64_t dy, uint64_t w, uint64_t h, uint64_t delta);

static efi_gop_mode_info refind_qinfo;
static efi_gop_mode refind_qmode;

static efi_status EFIAPI
refind_qemu_query(efi_gop *self, uint32_t mode, uint64_t *size, efi_gop_mode_info **info)
{
	if (mode != 0 || size == NULL || info == NULL) {
		return EFI_INVALID_PARAMETER;
	}
	*size = sizeof(refind_qinfo);
	*info = &refind_qinfo;          /* (callers free it: UEFI's pool gets a pointer it never gave, so a copy) */
	if (!EFI_ERROR(BS->allocate_pool(EfiLoaderData, sizeof(refind_qinfo), (void **)info))) {
		memcpy(*info, &refind_qinfo, sizeof(refind_qinfo));
	}
	return EFI_SUCCESS;
}

static efi_status EFIAPI
refind_qemu_set_mode(efi_gop *self, uint32_t mode)
{
	return mode == 0 ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

/*
 * The Blt of our own (QEMU's GOP; refind_blt=ours on the phone): 32-bit loads and stores only, pixel by pixel --
 * as boot_menu.c draws the phone's screen -- instead of the firmware's library copies. swap: the screen is
 * red-green-blue (PixelRedGreenBlueReserved8BitPerColor), Blt's pixels blue-green-red.
 */
/* (rb: the frame buffer it draws on, above) */

static inline uint32_t
refind_px(uint32_t v)
{
	return rb.swap ? ((v & 0xff) << 16 | (v & 0xff00) | (v >> 16 & 0xff)) : (v & 0xffffff);
}

static efi_status EFIAPI
refind_blt(efi_gop *self, uint32_t *buf, uint32_t op, uint64_t sx, uint64_t sy, uint64_t dx, uint64_t dy,
    uint64_t w, uint64_t h, uint64_t delta)
{
	volatile uint32_t *fb = rb.fb;
	uint64_t W = rb.width, H = rb.height, pitch = rb.pitch;

	rb.calls++;
	if (buf == NULL || w == 0 || h == 0) {
		return EFI_INVALID_PARAMETER;
	}
	if (delta == 0) {
		delta = w * 4;
	}
	switch (op) {
	case 0:         /* EfiBltVideoFill */
		if (dx + w > W || dy + h > H) {
			return EFI_INVALID_PARAMETER;
		}
		for (uint64_t y = 0; y < h; y++) {
			for (uint64_t x = 0; x < w; x++) {
				fb[(dy + y) * pitch + dx + x] = refind_px(buf[0]);
			}
		}
		return EFI_SUCCESS;
	case 1:         /* EfiBltVideoToBltBuffer */
		if (sx + w > W || sy + h > H) {
			return EFI_INVALID_PARAMETER;
		}
		for (uint64_t y = 0; y < h; y++) {
			uint32_t *d = (uint32_t *)((uint8_t *)buf + (dy + y) * delta) + dx;

			for (uint64_t x = 0; x < w; x++) {
				d[x] = refind_px(fb[(sy + y) * pitch + sx + x]);
			}
		}
		return EFI_SUCCESS;
	case 2:         /* EfiBltBufferToVideo */
		if (dx + w > W || dy + h > H) {
			return EFI_INVALID_PARAMETER;
		}
		for (uint64_t y = 0; y < h; y++) {
			const uint32_t *s = (const uint32_t *)((const uint8_t *)buf + (sy + y) * delta) + sx;

			for (uint64_t x = 0; x < w; x++) {
				fb[(dy + y) * pitch + dx + x] = refind_px(s[x]);
			}
		}
		return EFI_SUCCESS;
	case 3:         /* EfiBltVideoToVideo */
		if (sx + w > W || sy + h > H || dx + w > W || dy + h > H) {
			return EFI_INVALID_PARAMETER;
		}
		for (uint64_t i = 0; i < h; i++) {
			uint64_t y = dy > sy ? h - 1 - i : i;

			memmove_u32((uint32_t *)&fb[(dy + y) * pitch + dx], (const uint32_t *)&fb[(sy + y) * pitch + sx], w);
		}
		return EFI_SUCCESS;
	}
	return EFI_INVALID_PARAMETER;
}

static void
refind_qemu_gop(void)
{
	uint64_t fb = 0, pitch = 1080;

	if (PLAT != &platform_qemu_virt || GOP == NULL || GOP->mode == NULL) {
		return;
	}
	if (SCREEN.base != 0 && SCREEN.base != GOP->mode->fb_base && SCREEN.width >= 1080 && SCREEN.height >= 2400) {
		fb = SCREEN.base;               /* the phone-sized ramfb: its top left 1080x2400 */
		pitch = SCREEN.stride / 4;
	} else if (EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (1080 * 2400 * 4 + 0xfff) >> 12, &fb))) {
		return;
	} else {
		memset((void *)(uintptr_t)fb, 0, 1080 * 2400 * 4);     /* ramfb not resized: 1080x2400 in memory only */
	}
	refind_qinfo.version = 0;
	refind_qinfo.width = 1080;
	refind_qinfo.height = 2400;
	refind_qinfo.pixel_format = 1;          /* PixelBlueGreenRedReserved8BitPerColor: ramfb's XR24 */
	refind_qinfo.pixels_per_scanline = (uint32_t)pitch;
	refind_qmode.max_mode = 1;
	refind_qmode.mode = 0;
	refind_qmode.info = &refind_qinfo;
	refind_qmode.info_size = sizeof(refind_qinfo);
	refind_qmode.fb_base = fb;
	refind_qmode.fb_size = pitch * 4 * 2400;
	rb.fb = (volatile uint32_t *)(uintptr_t)fb;
	rb.width = 1080;
	rb.height = 2400;
	rb.pitch = pitch;
	rb.swap = 0;
	GOP->query_mode = refind_qemu_query;
	GOP->set_mode = refind_qemu_set_mode;
	GOP->blt = (void *)(refind_blt_fn)refind_blt;
	GOP->mode = &refind_qmode;
	print("refind: QEMU: the GOP is 1080x2400, pitch %d, at %x%s\n", pitch, fb,
	    fb == SCREEN.base ? " (the phone-sized ramfb)" : " (memory alone: the screenshots read it)");
}

/*
 * refind_conf=NAME (nobanner, bmp, plain, text): rEFInd reads refind-NAME.conf (mkrefindfs.sh makes them: without
 * the full-screen banner; the theme in BMP, the banner at half size; rEFInd's own pictures; its text menu) -- its
 * LoadOptions "-c FILE". Anything else, or none: refind.conf.
 */
static void
refind_conf_option(const struct payload_info *pi)
{
	static const char *const names[] = { "nobanner", "bmp", "plain", "text" };
	static const efi_guid li_guid = EFI_LOADED_IMAGE_GUID;
	efi_loaded_image *li = NULL;
	char word[32], file[40] = "refind_aa64.efi -c refind-";

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		word[0] = 0;
		menu_put(word, sizeof(word), "refind_conf=");
		menu_put(word, sizeof(word), names[i]);
		if (!aud_word(pi->cmdline, word)) {
			continue;
		}
		menu_put(file, sizeof(file), names[i]);
		menu_put(file, sizeof(file), ".conf");
		if (EFI_ERROR(BS->handle_protocol(rk.image, &li_guid, (void **)&li)) || li == NULL) {
			print("refind: %s: no LoadedImage to give it to\n", word);
			return;
		}
		refind_put16(rk.options, file);
		li->load_options = rk.options;
		li->load_options_size = (uint32_t)(str_len(file) + 1) * 2;
		print("refind: %s: rEFInd's LoadOptions \"%s\"\n", word, file);
		return;
	}
}

/* refind_blt=ours: the phone's own GOP (mode as it is) with the Blt above, over its frame buffer */
static void
refind_own_blt(void)
{
	efi_gop_mode_info *mi;

	if (GOP == NULL || GOP->mode == NULL || (mi = GOP->mode->info) == NULL || GOP->mode->fb_base == 0 ||
	    mi->pixel_format > 1 || mi->width == 0 || mi->pixels_per_scanline < mi->width) {
		print("refind: refind_blt=ours: no frame buffer we can draw on (format %d): the firmware's Blt\n",
		    GOP != NULL && GOP->mode != NULL && GOP->mode->info != NULL ? (uint64_t)GOP->mode->info->pixel_format : 99);
		return;
	}
	rb.fb = (volatile uint32_t *)(uintptr_t)GOP->mode->fb_base;
	rb.width = mi->width;
	rb.height = mi->height;
	rb.pitch = mi->pixels_per_scanline;
	rb.swap = mi->pixel_format == 0;
	rb.firmware_blt = GOP->blt;
	GOP->blt = (void *)(refind_blt_fn)refind_blt;
	print("refind: refind_blt=ours: the GOP's Blt is the loader's (%dx%d, pitch %d, %s)\n", rb.width, rb.height,
	    rb.pitch, rb.swap ? "RGB" : "BGR");
}

/* ------------------------------------------------------------------------ */

/*
 * boot_menu=refind: rEFInd from the payload's "refindfs". Returns only if rEFInd could not be started or came
 * back; whatever it chooses goes on in refind_choose() and never comes back here.
 */
static void
refind_menu(const struct payload_info *pi)
{
	static const efi_guid ho_guid = REFIND_HANDOFF_GUID;
	efi_status (EFIAPI *install)(efi_handle *, const efi_guid *, int, void *) =
	    (efi_status (EFIAPI *)(efi_handle *, const efi_guid *, int, void *))BS->install_protocol_interface;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void (EFIAPI *)(void *, void *), void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void (EFIAPI *)(void *, void *), void *, void **))BS->create_event;
	efi_status (EFIAPI *set_timer)(void *, int, uint64_t) = (efi_status (EFIAPI *)(void *, int, uint64_t))BS->set_timer;
	uint64_t v, payload = PAYLOAD_BASE;
	efi_status s;

	if (rk.tried) {
		return;
	}
	rk.tried = 1;
	rk.pi = pi;
	if (pi->refindfs == NULL || pi->refindfs_size < 64 * 1024 || pi->refindfs_size % REFIND_BLOCK != 0) {
		print("refind: boot_menu=refind but no rEFInd in the image (payload entry refindfs: %d bytes): the text menu\n",
		    (uint64_t)pi->refindfs_size);
		return;
	}
	rk.disk = pi->refindfs;
	rk.disk_size = pi->refindfs_size;
	rk.timeout_s = MENU_TIMEOUT_DEFAULT;
	if (cmdline_number(pi->cmdline, "boot_menu_timeout=", &v) && v != 0 && v <= MENU_TIMEOUT_MAX) {
		rk.timeout_s = v;
	}
	rk.idle_s = MENU_IDLE_DEFAULT;
	if (cmdline_number(pi->cmdline, "boot_menu_idle=", &v) && v != 0 && v <= MENU_TIMEOUT_MAX) {
		rk.idle_s = v;
	}
	rk.hang_s = PLAT == &platform_qemu_virt ? REFIND_HANG_QEMU_S : REFIND_HANG_S;
	if (cmdline_number(pi->cmdline, "refind_hang=", &v) && v >= 15 && v <= MENU_TIMEOUT_MAX) {
		rk.hang_s = v;
	}
	if (SCREEN.base != 0) {
		menu_fill(0, 0, SCREEN.width, SCREEN.height, MENU_BLACK);
	}
	refind_say("HakiOS: starting rEFInd ...");

	/* rEFInd's memory away from XNU's window and from the payload (which holds rEFInd's disk) */
	if (!choose_window(pi)) {
		print("refind: no XNU window: the text menu\n");
		return;
	}
	s = BS->allocate_pages(AllocateAddress, EfiLoaderData, PAYLOAD_MAX_SIZE >> 12, &payload);
	print("refind: the payload's RAM claimed: %x (%s)\n", s, EFI_ERROR(s) ? "UEFI keeps it already" : "now ours");
	efi_chain_host_init(pi->cmdline);
	refind_qemu_gop();

	if (!refind_disk_up()) {
		return;
	}
	rk.handoff.magic = REFIND_HANDOFF_MAGIC;
	rk.handoff.revision = 3;
	rk.handoff.choose = refind_choose;
	rk.handoff.mark = refind_mark;
	rk.handoff.scan_all = aud_word(pi->cmdline, "refind_scan=all");
	rk.handoff.test_hang = aud_word(pi->cmdline, "refind_test_hang=1");
	if (aud_word(pi->cmdline, "refind_safe=1")) {
		rk.handoff.gfx = REFIND_GFX_NO_CONSOLE_CONTROL | REFIND_GFX_NO_SCREEN_READ | REFIND_GFX_FILL_BY_BUFFER;
	}
	rk.handoff_handle = NULL;
	s = install(&rk.handoff_handle, &ho_guid, 0, &rk.handoff);
	if (EFI_ERROR(s)) {
		print("refind: the hand-off not installed: %x: the text menu\n", s);
		return;
	}
	refind_dp_node(&rk.file_dp.vendor.h, 1, 4, sizeof(struct refind_vendor_dp));
	rk.file_dp.vendor.guid = rk.disk_dp.vendor.guid;
	refind_put16(rk.file_dp.path, REFIND_PATH);
	refind_dp_node(&rk.file_dp.file, EFI_DP_TYPE_MEDIA, EFI_DP_MEDIA_FILE_PATH,
	    (uint16_t)(sizeof(efi_device_path) + (str_len(REFIND_PATH) + 1) * 2));
	refind_dp_node((efi_device_path *)((uint8_t *)&rk.file_dp.file + sizeof(efi_device_path) +
	    (str_len(REFIND_PATH) + 1) * 2), EFI_DP_TYPE_END, EFI_DP_END_ENTIRE, 4);
	s = BS->load_image(0, IMAGE, (const efi_device_path *)&rk.file_dp, NULL, 0, &rk.image);
	if (EFI_ERROR(s)) {
		print("refind: LoadImage of %s: %x: the text menu\n", REFIND_PATH, s);
		return;
	}
	refind_conf_option(pi);
	if (GOP != NULL && GOP->mode != NULL && GOP->mode->info != NULL) {
		print("refind: GOP %dx%d, format %d, pitch %d, mode %d of %d, frame buffer %x (%x bytes)\n",
		    (uint64_t)GOP->mode->info->width, (uint64_t)GOP->mode->info->height,
		    (uint64_t)GOP->mode->info->pixel_format, (uint64_t)GOP->mode->info->pixels_per_scanline,
		    (uint64_t)GOP->mode->mode, (uint64_t)GOP->mode->max_mode, GOP->mode->fb_base, GOP->mode->fb_size);
	}
	if (PLAT != &platform_qemu_virt && aud_word(pi->cmdline, "refind_blt=ours")) {
		refind_own_blt();
	}

	/* from here until refind_leave(): rEFInd's */
	rk.in = (menu_text_input *)ST->con_in;
	rk.quiet = screen_quiet;
	if (rk.in != NULL && rk.in->read_key_stroke != NULL) {
		rk.read_key = rk.in->read_key_stroke;
		rk.in->read_key_stroke = refind_read_key;
	}
	rk.start_image = BS->start_image;
	BS->start_image = refind_start_image;
	refind_bs_crc();
	rk.started_ms = rk.last_key_ms = rk.alive_ms = menu_now_ms();
	rk.running = 1;
	if (EFI_ERROR(create_event(0x80000000u | 0x200u /* EVT_TIMER | EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */,
	    refind_pet, NULL, &rk.pet_event)) || EFI_ERROR(set_timer(rk.pet_event, 1 /* TimerPeriodic */, 10000000))) {
		print("refind: no timer event: the watchdog is fed by nothing while rEFInd is up (it bites in 30 s)\n");
		rk.pet_event = NULL;
	}
	print("refind: rEFInd started (%s), timeout %d s, idle %d s, hang %d s, %s, graphics %x; Vol+/Vol-/Power only\n",
	    REFIND_PATH, rk.timeout_s, rk.idle_s, rk.hang_s, rk.handoff.scan_all ? "every volume" : "its own volume alone",
	    (uint64_t)rk.handoff.gfx);
	refind_say("HakiOS: rEFInd loaded, starting it");
	flush_log();
	screen_quiet = 1;       /* (the console's text would land on rEFInd's screen) */
	wdt_pet();
	s = rk.start_image(rk.image, NULL, NULL);
	refind_leave();
	refind_log_keys();
	print("refind: rEFInd came back: EFI_STATUS %x (last mark: %s): the text menu\n", s,
	    rk.last_mark[0] ? rk.last_mark : "none");
}
