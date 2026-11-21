/*
 * The panel's backlight of the POCO X3 Pro (vayu, SM8150-AC): the PM8150L's WLED (version 5),
 * bsd/dev/vayu_wled.c. iOS turns the screen off by asking the framebuffer to power the display down
 * (MiniIOMobileFramebuffer, RequestPowerChange); with no backlight driver the panel stayed lit.
 *
 * The facts:
 *
 *   the panel   stock live DT qcom,mdss_dsi_j20s_42_02_0b_dsc_video: qcom,mdss-dsi-bl-pmic-control-type
 *               = "bl_ctrl_wled", levels 1..4095 (fable-hang/recovery-live.dts:3014-3060); vendor
 *               dsi-panel-j20s-42-02-0b-lcd-dsc-vid.dtsi:40-42 (min 2, max 4095).
 *   the WLED    vendor pm8150l.dtsi:311-323, qcom,wled@d800 "qcom,pm8150l-spmi-wled": control
 *               registers 0xd800, sink registers 0xd900; vayu-sm8150.dtsi:1549-1555: strings 0-2
 *               (string-cfg 7), ovp 28000, fs-current 22500, cabc-sel 1. The PM8150L's peripherals
 *               are on SPMI slave 5 (the torch's flash module is 0x5d3), so: control 0x5d8, sink 0x5d9.
 *   registers   mainline drivers/video/backlight/qcom-wled.c (WLED5 is the pm8150l's): control 0x08
 *               fault status, 0x10 real-time interrupt status, 0x46 module enable (bit 7), 0x4c
 *               frequency, 0x4d OVP, 0x4e current limit (l.27-61); sink 0x46 string enables (bits
 *               7:4), 0x50/0x60 modulator A/B enable (bit 7), 0x52/0x62 brightness width (0: 12 bits,
 *               1: 15 bits), 0x53-0x54/0x63-0x64 brightness LSB/MSB, 0x65 sync (bit 0 A, bit 1 B),
 *               0x73 + 0x10 n string n's modulator (l.115-146). Brightness: two bytes little-endian
 *               into the modulator's LSB/MSB (wled5_set_brightness, l.272-290), then the sync bits
 *               cleared and set again (wled5_mod_sync_toggle, l.350-370). Module on/off: control
 *               0x46 bit 7 (wled_module_enable, l.300-306).
 *
 * Boot-args:
 *
 *   vayu_wled=1        a look, reading only: both register blocks and what they say
 *   vayu_wled=2        and then writes allowed: vayu_wled_power() (module on/off, which the framebuffer
 *                      calls when iOS turns the display off and on) and vayu_wled_brightness()
 *   vayu_wled_level=N  with vayu_wled=2: the brightness set once at the start, N permille (1..1000)
 *
 * Only control 0x46 and sink 0x53, 0x54, 0x63, 0x64, 0x65 are ever written, each read back; one
 * failure and nothing more is written for the rest of the run. Nothing is written through a channel
 * that is not ours. The arbiter section is the torch's (bsd/dev/hakios.torch.c), shared with the
 * buttons through hakios_spmi_lock().
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <os/atomic_private.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/vayu_wled.h>

/*
 * Every line of this driver also goes into the loader's log (as the torch's and the audio's say() do):
 * a long run's live log is the loader's log and a ring of the kernel's, and the ring fills with what
 * user space logs -- runs 005616 and 022501 kept no printf-only line of this driver at all, while the
 * torch's lines were there. (printf in parentheses below is the kernel's own.)
 */
extern void loader_log_putc(char c);
static int tee_printf(const char *format, ...) __printflike(1, 2);

static int
tee_printf(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		(printf)("%s", line);
	}
	return length;
}
#define printf(...) tee_printf(__VA_ARGS__)

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern bool (*hakios_display_power)(bool on);
extern bool (*hakios_display_brightness)(uint32_t permille);

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the PMIC arbiter (version 5), from bsd/dev/hakios.torch.c ------------------------------------ */

#define ARB_CORE_PHYS           0x0c440000UL    /* the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* which system may write through each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* reads: 0x80 per channel for our system (the first) */
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL    /* writes: 0x10000 per channel */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* [19:8] slave and peripheral */
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)      /* "extended register write long"; [11:4] register, [2:0] bytes - 1 */
#define COMMAND_READ            (1u << 27)      /* "extended register read long" */
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral */
	uint16_t        channel;
	bool            found, writable;
	vm_offset_t     write;          /* its write channel, mapped */
	const uint8_t * allowed;        /* the only registers written */
	size_t          allowed_count;
};

static const uint8_t ctrl_writable[] = { 0x46 };
static const uint8_t sink_writable[] = { 0x53, 0x54, 0x63, 0x64, 0x65 };
static struct peripheral ctrl = { .name = "the PM8150L's WLED control", .id = 0x5d8,  /* pm8150l.dtsi:313 */
	                          .allowed = ctrl_writable, .allowed_count = sizeof(ctrl_writable) };
static struct peripheral sink = { .name = "the PM8150L's WLED sink", .id = 0x5d9,     /* pm8150l.dtsi:313 */
	                          .allowed = sink_writable, .allowed_count = sizeof(sink_writable) };

static vm_offset_t arb_core, arb_cnfg, arb_observer;

static void
lock(void)
{
	hakios_spmi_lock();
}

static void
unlock(void)
{
	hakios_spmi_unlock();
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static const char *
spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	const char *problem;
	uint32_t data[2];

	if (!p->found) {
		return "no channel";
	}
	lock();
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = wait_done(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	unlock();
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

/* `count' (1 to 8) registers from `reg' on; a dropped transfer asked for again, twice at most. */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (int tries = 0; tries < 3; tries++) {
		if ((problem = spmi_read_once(p, reg, bytes, count)) == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		printf("vayu-wled: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == ARB_SYSTEM;
		}
	}
	if (!p->found) {
		printf("vayu-wled: no channel of the arbiter is %s\n", p->name);
	}
}

/* ---- writing (vayu_wled=2 only) ------------------------------------------------------------------------ */

static LCK_GRP_DECLARE(wled_lck_grp, "vayu_wled");
static LCK_MTX_DECLARE(wled_lock, &wled_lck_grp);       /* our writes, one caller at a time */
static bool writes_ok, broken;
static uint8_t saved_mod_en;                            /* control 0x46 as found */
static bool use_b;                                      /* the modulator in use: B, else A */
static bool brightness_ok;                              /* the strings agree on one enabled modulator */
static uint32_t max_level = 0xfff;                      /* 12 bits, or 0x7fff with a 15-bit width */

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		printf("vayu-wled: %s: no more writing to the WLED for the rest of the run\n", why);
	}
}

/* One allowed register written and read back; false, and no more writing, if it cannot be. */
/*
 * `restore': the one write let through after a failure -- control 0x46 back to bit 7 set, the module
 * on as it was found -- so that a failure while the backlight is off cannot keep the screen dark.
 * The sync register (0x65) is checked by the transfer only: its bits may clear themselves once the
 * modulator has taken the new value (mainline writes it without reading back, qcom-wled.c:350-365).
 */
static bool
put_any(struct peripheral *p, uint8_t reg, uint8_t value, bool restore)
{
	const char *problem = NULL;
	uint8_t back = 0xff;
	bool allowed = false;

	for (size_t i = 0; i < p->allowed_count; i++) {
		allowed |= p->allowed[i] == reg;
	}
	if ((broken && !restore) || !os_atomic_load(&writes_ok, acquire) || !allowed || !p->writable || p->write == 0) {
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(p->channel)) >> 8) & 0xfff) != p->id) {
		give_up("the arbiter's channel is no longer the WLED's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(p->write + CHANNEL_WDATA0, value);
		wr(p->write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(p->write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || (problem = spmi_read(p, reg, &back, 1)) != NULL) {
		printf("vayu-wled: %s %#04x <- %#04x: %s\n", p->name, reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if (back != value && !(p == &sink && reg == 0x65)) {
		printf("vayu-wled: %s %#04x <- %#04x, but it reads %#04x\n", p->name, reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

static bool
put(struct peripheral *p, uint8_t reg, uint8_t value)
{
	return put_any(p, reg, value, false);
}

/* only the bits of `mask' changed; *was: the register before */
static bool
put_bits(struct peripheral *p, uint8_t reg, uint8_t mask, uint8_t value, uint8_t *was)
{
	uint8_t now;

	if (spmi_read(p, reg, &now, 1) != NULL) {
		give_up("a read that failed");
		return false;
	}
	if (was != NULL) {
		*was = now;
	}
	return (now & mask) == (value & mask) || put(p, reg, (uint8_t)((now & ~mask) | (value & mask)));
}

bool
vayu_wled_power(bool on)
{
	uint8_t was = 0;
	bool done = false, known;

	if (!os_atomic_load(&writes_ok, acquire)) {
		return false;
	}
	lck_mtx_lock(&wled_lock);
	known = spmi_read(&ctrl, 0x46, &was, 1) == NULL;
	if (!on) {
		done = known && put_bits(&ctrl, 0x46, 0x80, 0x00, NULL);        /* wled_module_enable: 0x46 bit 7 */
	} else {
		/* on: the module bit as found at the start (set), allowed even after a failure; three tries */
		for (int tries = 0; tries < 3 && !done; tries++) {
			done = put_any(&ctrl, 0x46, (uint8_t)((known ? was : saved_mod_en) | 0x80), true);
		}
	}
	lck_mtx_unlock(&wled_lock);
	if (known) {
		printf("vayu-wled: backlight %s (0x46 %#x -> %#x)%s\n", on ? "on" : "off", was, (was & ~0x80) | (on ? 0x80 : 0),
		    done ? "" : ": NOT DONE");
	} else {
		printf("vayu-wled: backlight %s (0x46 unreadable)%s\n", on ? "on" : "off", done ? "" : ": NOT DONE");
	}
	return done;
}

bool
vayu_wled_brightness(uint32_t permille)
{
	uint32_t value;
	uint8_t lsb = use_b ? 0x63 : 0x53, sync_bit = use_b ? 0x02 : 0x01;
	bool done;

	if (!os_atomic_load(&writes_ok, acquire) || !brightness_ok) {
		return false;
	}
	permille = permille < 1 ? 1 : permille > 1000 ? 1000 : permille;
	value = max_level * permille / 1000;
	value = value < 1 ? 1 : value;
	lck_mtx_lock(&wled_lock);
	/* wled5_set_brightness (l.272-290), then wled5_mod_sync_toggle (l.350-370) */
	done = put(&sink, lsb, (uint8_t)(value & 0xff)) && put(&sink, (uint8_t)(lsb + 1), (uint8_t)(value >> 8)) &&
	    put_bits(&sink, 0x65, 0x03, 0x00, NULL) && put_bits(&sink, 0x65, 0x03, sync_bit, NULL);
	lck_mtx_unlock(&wled_lock);
	{
		static unsigned said;           /* (iOS fades in many steps: the first 20, then every 50th) */

		if (!done || said < 20 || said % 50 == 0) {
			printf("vayu-wled: brightness %u permille = %u of %u (modulator %c)%s\n", permille, value, max_level,
			    use_b ? 'B' : 'A', done ? "" : ": NOT DONE");
		}
		said++;
	}
	return done;
}

/* ---- the look ---------------------------------------------------------------------------------------- */

static void
dump(struct peripheral *p, const char *which)
{
	for (uint8_t reg = 0; reg < 0x80; reg += 8) {
		uint8_t b[8];
		const char *problem = spmi_read(p, reg, b, 8);

		if (problem != NULL) {
			printf("vayu-wled: %s 0x%02x: %s\n", which, reg, problem);
			continue;
		}
		printf("vayu-wled: %s 0x%02x: %02x %02x %02x %02x %02x %02x %02x %02x\n", which, reg, b[0], b[1], b[2], b[3], b[4],
		    b[5], b[6], b[7]);
	}
}

static void
vayu_wled_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t level = (uint32_t)(uintptr_t)parameter, version, channels, start_level = 0;
	uint8_t c[0x80], s[0x80];

	printf("vayu-wled: step 1: a look at the PM8150L's WLED (the panel's backlight)\n");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("vayu-wled: the arbiter is version %#x, not 5: nothing done\n", version);
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	find_channel(&ctrl, channels);
	find_channel(&sink, channels);
	if (!ctrl.found || !sink.found) {
		goto out;
	}
	dump(&ctrl, "ctrl");
	dump(&sink, "sink");
	for (uint8_t reg = 0; reg < 0x80; reg += 8) {
		if (spmi_read(&ctrl, reg, c + reg, 8) != NULL || spmi_read(&sink, reg, s + reg, 8) != NULL) {
			printf("vayu-wled: the registers cannot be read again: nothing more done\n");
			goto out;
		}
	}
	printf("vayu-wled: type %#x subtype %#x; module %s (0x46 = %#x); fault %#x int_rt %#x; ovp %#x ilim %#x freq %#x\n",
	    c[0x04], c[0x05], (c[0x46] & 0x80) ? "ON" : "off", c[0x46], c[0x08], c[0x10], c[0x4d], c[0x4e], c[0x4c]);
	printf("vayu-wled: sink: strings %#x; MOD_A en %u width %s brightness %u; MOD_B en %u width %s brightness %u; sync %#x\n",
	    (unsigned)(s[0x46] >> 4), (unsigned)(s[0x50] >> 7), s[0x52] & 1 ? "15-bit" : "12-bit", (unsigned)(s[0x53] | s[0x54] << 8),
	    (unsigned)(s[0x60] >> 7), s[0x62] & 1 ? "15-bit" : "12-bit", (unsigned)(s[0x63] | s[0x64] << 8), s[0x65]);
	printf("vayu-wled: sink: MOD_A source %#x, MOD_B source %#x (0: high, 3: external)\n", s[0x51], s[0x61]);
	unsigned on_a = 0, on_b = 0, unknown = 0;

	for (unsigned n = 0; n < 4; n++) {
		uint8_t src = 0xff;                     /* STR_SRC_SEL(n) = 0x73 + 0x10 n (l.143); 1 = B (l.144-146) */
		bool enabled = (s[0x46] >> (4 + n)) & 1; /* CURR_SINK bits 7:4 (l.89-91, wled5_setup l.1129) */
		bool read = spmi_read(&sink, (uint8_t)(0x73 + 0x10 * n), &src, 1) == NULL;

		printf("vayu-wled: string %u: %s, modulator %s (%#x)\n", n, enabled ? "ENABLED" : "off",
		    !read ? "?" : (src & 3) ? "B" : "A", src);
		if (enabled) {
			if (!read) {
				unknown++;
			} else if (src & 3) {
				on_b++;
			} else {
				on_a++;
			}
		}
	}
	saved_mod_en = c[0x46];
	/* the modulator is the one the enabled strings take (mainline wled5_setup, l.1124-1128), and it must be enabled */
	use_b = on_b > 0;
	brightness_ok = unknown == 0 && (on_a == 0) != (on_b == 0) && ((use_b ? s[0x60] : s[0x50]) & 0x80);
	max_level = (use_b ? s[0x62] : s[0x52]) & 1 ? 0x7fff : 0xfff;
	printf("vayu-wled: the strings take modulator %c (%u on A, %u on B, %u unread; A %s, B %s), at most %u: brightness %s\n",
	    use_b ? 'B' : 'A', on_a, on_b, unknown, (s[0x50] & 0x80) ? "on" : "off", (s[0x60] & 0x80) ? "on" : "off", max_level,
	    brightness_ok ? "may be set" : "WILL NOT BE SET (the strings and modulators do not agree)");
	if (level < 2) {
		printf("vayu-wled: step 1 done, nothing written\n");
		goto out;
	}
	if (!ctrl.writable || !sink.writable) {
		printf("vayu-wled: the channels are not ours to write through: nothing will be written\n");
		goto out;
	}
	ctrl.write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * ctrl.channel, 0x1000);
	sink.write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * sink.channel, 0x1000);
	os_atomic_store(&writes_ok, true, release);
	os_atomic_store(&hakios_display_brightness, vayu_wled_brightness, release);
	os_atomic_store(&hakios_display_power, vayu_wled_power, release);
	printf("vayu-wled: writes allowed (module on/off when iOS turns the display off/on, brightness)\n");
	PE_parse_boot_argn("vayu_wled_level", &start_level, sizeof(start_level));
	if (start_level != 0) {
		vayu_wled_brightness(start_level);
	}
out:
	thread_terminate(current_thread());
}

void
vayu_wled_init(void)
{
	thread_t thread;
	uint32_t level = 0;

	if (!PE_parse_boot_argn("vayu_wled", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(vayu_wled_thread, (void *)(uintptr_t)level, &thread) != KERN_SUCCESS) {
		printf("vayu-wled: the thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}
