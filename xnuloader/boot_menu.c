/*
 * The boot menu of a standalone start (boot_menu=1): on the phone's screen, before anything of
 * the start is done, a choice of what to start -- HakiOS (iOS), Android, EFI applications, what the phone starts
 * at power-on, a page of diagnostics, power off. Compiled as part of loader.c (it includes this file after
 * everything used here); boot_menu.h says what the other entries' parts provide.
 *
 *   boot_menu=1                the menu at every standalone start (without it: none, the start goes on as before)
 *   boot_menu_timeout=N        seconds until the default entry starts by itself (default 8, at most 600). 0 is
 *                              not taken (standalone.sh build refuses it, this loader takes 8): a phone on the
 *                              bench would wait for a key for good
 *   boot_menu_default=ios|android   the entry the timer starts and the cursor starts on (default ios)
 *   boot_menu_idle=N           once a key has stopped the timer: seconds without a key until the default entry
 *                              starts after all (default 300), so that a phantom key does not hold the phone here
 *   boot_menu_efi=1            the EFI applications found (efi_chainload.h), one entry each
 *   boot_menu_bcb=1            "Default Boot": what misc's boot control block makes the phone start at power-on
 *   boot_menu_poweroff=1       "Power Off" (EfiResetShutdown), asked twice: without the cable a phone that is off
 *                              stays off, and with it on the bench it is in the charger's screen, out of reach
 *   boot_menu=refind           (m4) the menu drawn by rEFInd (refind_menu.c) -- the image must carry it (payload
 *                              entry refindfs, standalone.sh REFIND=); this menu if rEFInd does not start. Takes the
 *                              place of boot_menu=1 (either turns the menu on); boot_menu_timeout= and
 *                              boot_menu_idle= hold for rEFInd too, the other boot_menu_* for this menu only
 *
 * A loader started with LoadOptions menu=off / menu=android / menu=diag (rEFInd's entries, or anything else that
 * starts loader.efi) takes that instead of a menu: HakiOS at once, Android, or this menu on its diagnostics page.
 *
 * The keys come from UEFI's console input (ST->con_in). On vayu that is the firmware's ButtonsDxe (the stock
 * Qualcomm driver in the FD of the lab's boot-test4.img, which these images take); its translation, read off
 * its code: Vol+ scan 0x01 (up), Vol- scan 0x02 (down), Power the character 0x0d; Vol+ with Vol- scan 0x17,
 * Power with Vol+ scan 0x05, Power with Vol- scan 0x08 (those three only logged). Under QEMU: the serial
 * console's arrow keys and Enter. Every key is logged.
 *
 * Mu-Silicium's own "[Volume Up] Boot Manager": Vol+ pressed between Mu's logo and the end of its boot timeout
 * (PcdPlatformBootTimeOut, 3 s) makes its BDS start its Boot Manager Menu instead of this loader. So nothing
 * here wants a key held at power-on: with boot_menu=1 the menu comes at every start by itself, keys pressed
 * before it is up are thrown away (150 ms, then whatever is still queued), and the list wraps, so that Vol-
 * alone reaches every entry. Hold no key once the Mi logo is up; wait for the menu.
 *
 * No entry comes back to Mu: HakiOS goes on with the start, every other entry ends in a reset (Mu's BDS, if
 * this returned, would power the phone off 10 s later). Our own work, not taken from another port.
 */
#include "boot_menu_font.h"     /* Terminus Font Bold 16x32, ASCII (mkfont.py; SIL OFL 1.1, font-OFL.txt) */

#define MENU_GLYPH_W         16
#define MENU_GLYPH_H         32
#define MENU_TIMEOUT_DEFAULT 8
#define MENU_TIMEOUT_MAX     600
#define MENU_IDLE_DEFAULT    300
#define MENU_DRAIN_MS        150
#define MENU_POLL_US         20000
#define MENU_CONFIRM_MS      5000
#define MENU_KEYS_KEPT       8

#define MENU_X          64
#define MENU_TITLE_Y    160
#define MENU_ITEMS_Y    440
#define MENU_ROW        150
#define MENU_FOOTER     420             /* the bottom band: the keys and the timer */

#define MENU_BLACK      0x000000u
#define MENU_WHITE      0xffffffu
#define MENU_GREY       0x909090u
#define MENU_DIM        0xb0b0b0u
#define MENU_BAR        0x1a1a1au
#define MENU_BAR_SEL    0x1e5aa8u
#define MENU_WARN       0xd04030u

/* LoadOptions menu=...: MENU_OPT_* (refind_child() in refind_menu.c, at the top of efi_main), 0 if none */
static int loadopt_menu;
static void refind_menu(const struct payload_info *pi);

static int
boot_menu_wanted(const char *cmdline)
{
	return aud_word(cmdline, "boot_menu=1") || aud_word(cmdline, "boot_menu=refind");
}

enum menu_action { MENU_IOS, MENU_ANDROID, MENU_EFI, MENU_DEFAULT, MENU_DIAG, MENU_POWEROFF };

struct menu_item {
	enum menu_action action;
	int efi;                        /* MENU_EFI: its menu_efi[] (-1: none found) */
	char label[64];
};

static struct {
	struct menu_item items[5 + BOOT_MENU_EFI_MAX];
	int count, cursor, def;
	int timer_on, in_diag, poweroff_armed, rgb, efi_found;
	unsigned timeout, idle;
	uint64_t deadline_ms, last_key_ms, armed_ms, shown_s;
	struct menu_input_key keys[MENU_KEYS_KEPT];
	unsigned nkeys;
	char diag[14][72];
	int ndiag;
} menu;

/* The EFI applications found (efi_chainload.c, scanned once when the menu comes up) */
static struct efi_chain_entry menu_efi[EFI_CHAIN_MAX];

static uint64_t
menu_now_ms(void)
{
	return READ_SYSREG(CNTVCT_EL0) * 1000 / READ_SYSREG(CNTFRQ_EL0);
}

static void
menu_put(char *d, unsigned cap, const char *s)
{
	unsigned n = (unsigned)str_len(d);

	while (*s && n + 1 < cap) {
		d[n++] = *s++;
	}
	d[n] = 0;
}

static void
menu_put_num(char *d, unsigned cap, uint64_t v, unsigned base)
{
	char t[24], o[24];
	int i = 0, k = 0;

	do {
		t[i++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (i) {
		o[k++] = t[--i];
	}
	o[k] = 0;
	menu_put(d, cap, o);
}

/* ------------------------------------------------------------------------ */
/* The screen: SCREEN, 32 bits a pixel; nothing is drawn without one (QEMU without ramfb)                    */

static void
menu_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
	uint32_t px = menu.rgb ? ((rgb & 0xff) << 16 | (rgb & 0xff00) | (rgb >> 16 & 0xff)) : rgb;

	if (SCREEN.base == 0 || x >= SCREEN.width || y >= SCREEN.height) {
		return;
	}
	if (w > SCREEN.width - x) {
		w = SCREEN.width - x;
	}
	if (h > SCREEN.height - y) {
		h = SCREEN.height - y;
	}
	for (uint32_t r = 0; r < h; r++) {
		uint32_t *p = (uint32_t *)(uintptr_t)(SCREEN.base + (uint64_t)(y + r) * SCREEN.stride) + x;

		for (uint32_t c = 0; c < w; c++) {
			p[c] = px;
		}
	}
}

static void
menu_text(uint32_t x, uint32_t y, unsigned scale, uint32_t rgb, const char *s)
{
	for (; *s; s++, x += MENU_GLYPH_W * scale) {
		unsigned c = (uint8_t)*s;
		const uint16_t *g;

		if (c < 0x20 || c > 0x7e) {
			c = '?';
		}
		g = menu_font[c - 0x20];
		for (unsigned r = 0; r < MENU_GLYPH_H; r++) {
			for (unsigned b = 0; b < MENU_GLYPH_W; b++) {
				if (g[r] & (0x8000u >> b)) {
					menu_fill(x + b * scale, y + r * scale, scale, scale, rgb);
				}
			}
		}
	}
}

static void
menu_draw_footer(void)
{
	char t[72] = "";
	uint32_t y = SCREEN.height > MENU_FOOTER ? SCREEN.height - MENU_FOOTER : 0;

	if (SCREEN.base == 0) {
		return;
	}
	menu_fill(0, y, SCREEN.width, MENU_FOOTER, MENU_BLACK);
	menu_text(MENU_X, y, 1, MENU_GREY, menu.in_diag ? "Any key: back to the menu" : "Vol+ / Vol-: move    Power: select");
	if (menu.poweroff_armed) {
		menu_put(t, sizeof(t), "Power again: OFF");
	} else if (menu.timer_on) {
		uint64_t now = menu_now_ms(), left = menu.deadline_ms > now ? (menu.deadline_ms - now + 999) / 1000 : 0;

		menu_put(t, sizeof(t), menu.items[menu.def].label);
		menu_put(t, sizeof(t), " in ");
		menu_put_num(t, sizeof(t), left, 10);
		menu_put(t, sizeof(t), " s");
	} else {
		menu_put(t, sizeof(t), "Timer off");
	}
	menu_text(MENU_X, y + 80, 2, menu.poweroff_armed ? MENU_WARN : MENU_WHITE, t);
}

static void
menu_draw(void)
{
	if (SCREEN.base == 0) {
		return;
	}
	menu_fill(0, 0, SCREEN.width, SCREEN.height, MENU_BLACK);
	if (menu.in_diag) {
		menu_text(MENU_X, MENU_TITLE_Y, 2, MENU_WHITE, "Diagnostics");
		for (int i = 0; i < menu.ndiag; i++) {
			menu_text(MENU_X / 2, MENU_TITLE_Y + 160 + (uint32_t)i * 48, 1, MENU_DIM, menu.diag[i]);
		}
	} else {
		menu_text(MENU_X, MENU_TITLE_Y, 3, MENU_WHITE, "HakiOS boot");
		menu_text(MENU_X, MENU_TITLE_Y + 120, 1, MENU_GREY, "vayu: HakiOS, Android, EFI");
		for (int i = 0; i < menu.count; i++) {
			uint32_t y = MENU_ITEMS_Y + (uint32_t)i * MENU_ROW;
			int sel = i == menu.cursor;

			menu_fill(MENU_X / 2, y, SCREEN.width - MENU_X, MENU_ROW - 24, sel ? MENU_BAR_SEL : MENU_BAR);
			unsigned scale = str_len(menu.items[i].label) * 2 * MENU_GLYPH_W + MENU_X * 2 > SCREEN.width ? 1 : 2;

			menu_text(MENU_X, y + (MENU_ROW - 24 - scale * MENU_GLYPH_H) / 2, scale, sel ? MENU_WHITE : MENU_DIM,
			    menu.items[i].label);
		}
	}
	menu_draw_footer();
}

/* ------------------------------------------------------------------------ */

static void
menu_add(enum menu_action action, int efi, const char *a, const char *b)
{
	struct menu_item *it = &menu.items[menu.count++];

	it->action = action;
	it->efi = efi;
	it->label[0] = 0;
	menu_put(it->label, sizeof(it->label), a);
	if (b != NULL) {
		menu_put(it->label, sizeof(it->label), b);
	}
}

static void
menu_diag_line(const char *a, const char *b)
{
	char *l;

	if (menu.ndiag >= (int)(sizeof(menu.diag) / sizeof(menu.diag[0]))) {
		return;
	}
	l = menu.diag[menu.ndiag++];
	l[0] = 0;
	menu_put(l, sizeof(menu.diag[0]), a);
	if (b != NULL) {
		menu_put(l, sizeof(menu.diag[0]), b);
	}
	print("boot_menu: diag: %s\n", l);
}

/* What this start sees, read only: misc's boot control block, darwin's count of starts, the EFI applications, keys */
static void
menu_diagnostics(const struct payload_info *pi)
{
	const char *cmdline = pi->cmdline;
	struct bcb_menu_state st;
	char t[72];
	uint64_t v;

	menu.ndiag = 0;
	bcb_menu_read(pi, &st);
	if (st.command == BCB_MENU_UNREAD) {
		menu_diag_line("misc: ", st.why != NULL ? st.why : "not read");
	} else {
		menu_diag_line("misc command: ", st.command_text[0] ? st.command_text : "(empty)");
		menu_diag_line("misc status: ", st.status_text[0] ? st.status_text : "(empty)");
		menu_diag_line("misc recovery: ", st.recovery_text[0] ? st.recovery_text : "(empty)");
		t[0] = 0;
		menu_put_num(t, sizeof(t), st.dirty, 10);
		menu_put(t, sizeof(t), st.wipe_asked ? " (WIPE ASKED: --wipe_data)" : "");
		menu_diag_line("misc bytes past the command: ", t);
	}
	t[0] = 0;
	if (!st.tries_known) {
		menu_put(t, sizeof(t), "no count (no tries block, or none written)");
	} else {
		menu_put_num(t, sizeof(t), st.tries_count, 10);
		menu_put(t, sizeof(t), " since a good start, ");
		menu_put_num(t, sizeof(t), st.tries_starts, 10);
		menu_put(t, sizeof(t), " starts, ");
		menu_put_num(t, sizeof(t), st.tries_recoveries, 10);
		menu_put(t, sizeof(t), " recoveries");
	}
	menu_diag_line("tries: ", t);
	if (st.tries_known) {
		t[0] = 0;
		menu_put_num(t, sizeof(t), st.tries_cycles, 10);
		menu_diag_line("tries: evacuation cycles ", t);
	}
	t[0] = 0;
	if (cmdline_number(cmdline, "boot_tries=", &v)) {
		menu_put_num(t, sizeof(t), v, 10);
	} else {
		menu_put(t, sizeof(t), "off");
	}
	menu_diag_line("boot_tries=", t);
	if (!aud_word(cmdline, "boot_menu_efi=1")) {
		menu_diag_line("EFI: not looked for (boot_menu_efi=1)", NULL);
	} else {
		t[0] = 0;
		menu_put_num(t, sizeof(t), (uint64_t)menu.efi_found, 10);
		menu_put(t, sizeof(t), " found");
		menu_diag_line("EFI: ", t);
		for (int i = 0; i < menu.efi_found && i < BOOT_MENU_EFI_MAX; i++) {
			menu_diag_line("  ", menu_efi[i].label);
		}
	}
	t[0] = 0;
	for (unsigned i = 0; i < menu.nkeys && i < MENU_KEYS_KEPT; i++) {
		const struct menu_input_key *k = &menu.keys[(menu.nkeys - 1 - i) % MENU_KEYS_KEPT];

		menu_put(t, sizeof(t), i ? " " : "");
		menu_put_num(t, sizeof(t), k->scan, 16);
		menu_put(t, sizeof(t), "/");
		menu_put_num(t, sizeof(t), k->ch, 16);
	}
	menu_diag_line("keys, last first: ", menu.nkeys ? t : "none");
	t[0] = 0;
	menu_put_num(t, sizeof(t), SCREEN.width, 10);
	menu_put(t, sizeof(t), "x");
	menu_put_num(t, sizeof(t), SCREEN.height, 10);
	menu_put(t, sizeof(t), ", timeout ");
	menu_put_num(t, sizeof(t), menu.timeout, 10);
	menu_put(t, sizeof(t), " s, idle ");
	menu_put_num(t, sizeof(t), menu.idle, 10);
	menu_put(t, sizeof(t), " s");
	menu_diag_line("screen ", t);
}

static void
menu_leave(int quiet)
{
	menu_fill(0, 0, SCREEN.width, SCREEN.height, MENU_BLACK);
	screen_quiet = quiet;
}

/* What the chosen entry does: 1 -- HakiOS, the start goes on; 0 -- the menu stays. Every other entry resets. */
static int
menu_select(int i, const struct payload_info *pi, int quiet, int by_hand)
{
	struct menu_item *it = &menu.items[i];

	if (it->action != MENU_POWEROFF && menu.poweroff_armed) {
		menu.poweroff_armed = 0;
	}
	switch (it->action) {
	case MENU_IOS:
		print("boot_menu: chosen: %s\n", it->label);
		standalone_by_hand = by_hand;   /* (counted by boot_tries, but not sent elsewhere) */
		menu_leave(quiet);
		return 1;
	case MENU_ANDROID:
		print("boot_menu: chosen: %s\n", it->label);
		menu_leave(quiet);
		bcb_menu_android();
		reboot();
		return 0;
	case MENU_EFI:
		if (it->efi < 0) {
			print("boot_menu: no EFI applications found: nothing to start\n");
			return 0;
		}
		print("boot_menu: chosen: %s\n", it->label);
		menu_leave(quiet);
		if (USB != NULL) {      /* (what is started would inherit a running USB stack) */
			if (USB->abort_xfer != NULL) {
				USB->abort_xfer(USB_EP_OUT);
				USB->abort_xfer(USB_EP_IN);
			}
			print("boot_menu: the loader's USB stack stopped first: %x\n", (uint64_t)USB->stop());
		}
		efi_chain_start(&efi_host, &menu_efi[it->efi], NULL);
		return 0;
	case MENU_DEFAULT:
		it->label[0] = 0;
		menu_put(it->label, sizeof(it->label), "Default: ");
		menu_put(it->label, sizeof(it->label), bcb_menu_default_next());
		print("boot_menu: chosen: %s\n", it->label);
		return 0;
	case MENU_DIAG:
		print("boot_menu: chosen: diagnostics\n");
		menu_diagnostics(pi);
		menu.in_diag = 1;
		return 0;
	case MENU_POWEROFF:
		if (!menu.poweroff_armed) {
			menu.poweroff_armed = 1;
			menu.armed_ms = menu_now_ms();
			print("boot_menu: power off asked: Power again within 5 s powers the phone off\n");
			return 0;
		}
		print("boot_menu: power off confirmed: EfiResetShutdown\n");
		menu_leave(quiet);
		hc_take_back("before the power off");
		flush_log();
		ST->rt->reset_system(2 /* EfiResetShutdown */, 0, 0, NULL);
		for (;;) {
		}
	}
	return 0;
}

static void
menu_key_kept(const struct menu_input_key *k)
{
	menu.keys[menu.nkeys % MENU_KEYS_KEPT] = *k;
	menu.nkeys++;
}

/*
 * boot_menu=1: the menu, until an entry is chosen or the timer chooses the default. Comes back only for HakiOS
 * (the standalone start goes on); every other way out is a reset or the power off.
 */
static void
boot_menu(const struct payload_info *pi)
{
	const char *cmdline = pi->cmdline;
	menu_text_input *in = (menu_text_input *)ST->con_in;
	struct menu_input_key k;
	uint64_t v, start, now;
	int quiet = screen_quiet, android;

	if (loadopt_menu == MENU_OPT_OFF) {
		print("boot_menu: LoadOptions menu=off: no menu, HakiOS\n");
		return;
	}
	if (loadopt_menu == MENU_OPT_ANDROID) {
		print("boot_menu: LoadOptions menu=android\n");
		bcb_menu_android();
		reboot();
	}
	if (!boot_menu_wanted(cmdline) && loadopt_menu != MENU_OPT_DIAG) {
		return;
	}
	if (loadopt_menu != MENU_OPT_DIAG && aud_word(cmdline, "boot_menu=refind")) {
		refind_menu(pi);        /* (comes back only if rEFInd did not start, or came back itself) */
	}
	memset(&menu, 0, sizeof(menu));
	menu.timeout = MENU_TIMEOUT_DEFAULT;
	if (cmdline_number(cmdline, "boot_menu_timeout=", &v)) {
		if (v == 0) {
			print("boot_menu: boot_menu_timeout=0 is not taken (the phone would wait for a key for good): %d s\n",
			    (uint64_t)MENU_TIMEOUT_DEFAULT);
		} else if (v > MENU_TIMEOUT_MAX) {
			print("boot_menu: boot_menu_timeout=%d: %d s at most\n", v, (uint64_t)MENU_TIMEOUT_MAX);
			menu.timeout = MENU_TIMEOUT_MAX;
		} else {
			menu.timeout = (unsigned)v;
		}
	}
	menu.idle = MENU_IDLE_DEFAULT;
	if (cmdline_number(cmdline, "boot_menu_idle=", &v)) {
		if (v == 0 || v > MENU_TIMEOUT_MAX) {
			print("boot_menu: boot_menu_idle=%d is not taken: %d s\n", v, (uint64_t)MENU_IDLE_DEFAULT);
		} else {
			menu.idle = (unsigned)v;
		}
	}
	android = aud_word(cmdline, "boot_menu_default=android");
	if (!android && !aud_word(cmdline, "boot_menu_default=ios") && cmdline_number(cmdline, "boot_menu_default=", &v)) {
		print("boot_menu: boot_menu_default= is neither ios nor android: ios\n");
	}

	menu_add(MENU_IOS, 0, "HakiOS (iOS)", NULL);
	menu_add(MENU_ANDROID, 0, "Android", NULL);
	if (aud_word(cmdline, "boot_menu_efi=1")) {
		struct efi_chain_diag diag;

		efi_chain_host_init(cmdline);
		if (aud_word(cmdline, "boot_efi_connect=1")) {
			efi_chain_connect(&efi_host);
		}
		efi_chain_diag(&efi_host, &diag);
		menu.efi_found = efi_chain_scan(&efi_host, menu_efi, EFI_CHAIN_MAX);
		if (menu.efi_found < 0) {
			menu.efi_found = 0;
		}
		for (int i = 0; i < menu.efi_found && i < BOOT_MENU_EFI_MAX; i++) {
			menu_add(MENU_EFI, i, "EFI: ", menu_efi[i].label);
		}
		if (menu.efi_found == 0) {
			menu_add(MENU_EFI, -1, "EFI: none found", NULL);
		}
		if (menu.efi_found > BOOT_MENU_EFI_MAX) {
			print("boot_menu: %d EFI applications found, the first %d shown\n", (uint64_t)menu.efi_found,
			    (uint64_t)BOOT_MENU_EFI_MAX);
		}
	}
	if (aud_word(cmdline, "boot_menu_bcb=1")) {
		menu_add(MENU_DEFAULT, 0, "Default: ", bcb_menu_default_name());
	}
	menu_add(MENU_DIAG, 0, "Diagnostics", NULL);
	if (aud_word(cmdline, "boot_menu_poweroff=1")) {
		menu_add(MENU_POWEROFF, 0, "Power Off", NULL);
	}
	menu.def = menu.cursor = android ? 1 : 0;
	menu.rgb = GOP != NULL && SCREEN.base == GOP->mode->fb_base && GOP->mode->info->pixel_format == 0;

	print("boot_menu=1: %d entries; %s in %d s unless a key\n", (uint64_t)menu.count, menu.items[menu.def].label,
	    (uint64_t)menu.timeout);
	for (int i = 0; i < menu.count; i++) {
		print("boot_menu: entry %d: %s\n", (uint64_t)i, menu.items[i].label);
	}
	if (in == NULL || in->read_key_stroke == NULL) {
		print("boot_menu: no console input: only the timer chooses\n");
		in = NULL;
	}

	/* Keys from before the menu (held through the logo, a bounce of Power) are not choices: dropped, logged */
	for (start = menu_now_ms(); in != NULL && menu_now_ms() - start < MENU_DRAIN_MS;) {
		while (!EFI_ERROR(in->read_key_stroke(in, &k))) {
			print("boot_menu: dropped a key from before the menu: scan %x char %x\n", (uint64_t)k.scan,
			    (uint64_t)k.ch);
		}
		BS->stall(10000);
	}

	if (SCREEN.base != 0 && PLAT != &platform_qemu_virt) {
		screen_quiet = 1;       /* (the console's text would land on the menu; QEMU's goes to the serial line too) */
	}
	now = menu_now_ms();
	menu.timer_on = 1;
	menu.deadline_ms = now + (uint64_t)menu.timeout * 1000;
	menu.last_key_ms = now;
	menu.shown_s = now / 1000;
	if (loadopt_menu == MENU_OPT_DIAG) {
		print("boot_menu: LoadOptions menu=diag: the diagnostics page first\n");
		menu_diagnostics(pi);
		menu.in_diag = 1;
	}
	menu_draw();
	print("boot_menu: shown, cursor on %s\n", menu.items[menu.cursor].label);
	flush_log();

	for (;;) {
		wdt_pet();
		now = menu_now_ms();
		if (in != NULL && !EFI_ERROR(in->read_key_stroke(in, &k))) {
			menu_key_kept(&k);
			print("boot_menu: key scan %x char %x\n", (uint64_t)k.scan, (uint64_t)k.ch);
			menu.last_key_ms = now;
			if (menu.timer_on) {
				menu.timer_on = 0;
				print("boot_menu: a key: the timer is off (idle %d s)\n", (uint64_t)menu.idle);
			}
			if (menu.in_diag) {
				menu.in_diag = 0;
			} else if (k.scan == MENU_SCAN_UP || k.scan == MENU_SCAN_DOWN) {
				menu.cursor = (menu.cursor + (k.scan == MENU_SCAN_UP ? menu.count - 1 : 1)) % menu.count;
				if (menu.poweroff_armed) {
					menu.poweroff_armed = 0;
					print("boot_menu: power off not confirmed\n");
				}
				print("boot_menu: cursor on %s\n", menu.items[menu.cursor].label);
			} else if (k.scan == 0 && (k.ch == '\r' || k.ch == '\n' || k.ch == ' ')) {
				if (menu_select(menu.cursor, pi, quiet, 1)) {
					return;
				}
			} else {
				print("boot_menu: that key does nothing here\n");
			}
			menu_draw();
		} else if (menu.timer_on && now >= menu.deadline_ms) {
			print("boot_menu: the timer: %s\n", menu.items[menu.def].label);
			if (menu_select(menu.def, pi, quiet, 0)) {
				return;
			}
		} else if (!menu.timer_on && now - menu.last_key_ms >= (uint64_t)menu.idle * 1000) {
			print("boot_menu: no key for %d s: %s\n", (uint64_t)menu.idle, menu.items[menu.def].label);
			menu.in_diag = 0;
			if (menu_select(menu.def, pi, quiet, 0)) {
				return;
			}
		}
		if (menu.poweroff_armed && menu_now_ms() - menu.armed_ms >= MENU_CONFIRM_MS) {
			menu.poweroff_armed = 0;
			print("boot_menu: power off not confirmed in 5 s\n");
			menu_draw_footer();
		}
		if (menu.timer_on && now / 1000 != menu.shown_s) {
			menu.shown_s = now / 1000;
			menu_draw_footer();
		}
		BS->stall(MENU_POLL_US);
	}
}
