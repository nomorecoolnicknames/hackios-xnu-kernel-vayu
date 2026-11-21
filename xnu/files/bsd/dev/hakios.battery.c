/*
 * Battery monitor for Xiaomi redwood (POCO X5 Pro).
 *
 * The phone selects PM7325B and qcom,battery-charger over PMIC GLINK/ADSP, not
 * the local PM7250B/QG layout.  The resident UEFI contains the matching
 * Qualcomm charger drivers.  xnuloader owns that firmware context on core 0,
 * polls it, and publishes a read-only cache; XNU only consumes that cache.
 * Charger limits and JEITA protection therefore remain firmware-owned.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

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

#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)
#define LOADER_PAGE_SIZE                0x4000UL

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

static volatile struct loader_battery *shared_battery;
static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates, battery_failures;
static uint32_t battery_ios = 1;

/*
 * For the flashlight's thermal guard: a temperature counts only while a live source keeps giving it
 * (the gauge, the DSP, or a loader snapshot whose sequence moves); none for 30 s and it is -273, unknown,
 * which the torch takes as too hot. A snapshot that stops changing must not renew the guard.
 */
#define BATTERY_TEMPERATURE_MAX_AGE_NS  (30ULL * NSEC_PER_SEC)
static LCK_GRP_DECLARE(battery_lock_group, "hakios-battery");
static LCK_SPIN_DECLARE(battery_temperature_lock, &battery_lock_group);
static uint64_t battery_temperature_at;
static int battery_guard_temperature = -273;

static void
battery_temperature_update(int temperature)
{
	lck_spin_lock(&battery_temperature_lock);
	battery_temperature_c = battery_guard_temperature = temperature;
	battery_temperature_at = mach_absolute_time();
	lck_spin_unlock(&battery_temperature_lock);
}

static void
battery_loader_temperature_update(uint32_t sequence, int temperature)
{
	static uint32_t last_sequence;
	static bool seen;

	/* (the first snapshot has no age: only a later publication by the firmware shows it is live) */
	if (seen && sequence != last_sequence) {
		battery_temperature_update(temperature);
	}
	last_sequence = sequence;
	seen = true;
}

int
hakios_battery_temperature(void)
{
	uint64_t age;
	int temperature;

	lck_spin_lock(&battery_temperature_lock);
	temperature = battery_guard_temperature;
	absolutetime_to_nanoseconds(mach_absolute_time() - battery_temperature_at, &age);
	lck_spin_unlock(&battery_temperature_lock);
	return temperature <= -273 || age > BATTERY_TEMPERATURE_MAX_AGE_NS ? -273 : temperature;
}

SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");

static bool
battery_snapshot(struct loader_battery *out)
{
	uint32_t before, after;

	for (unsigned retry = 0; retry < 4; retry++) {
		before = shared_battery->sequence;
		if (before & 1) continue;
		__asm__ volatile ("dmb ishld" ::: "memory");
		memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
		__asm__ volatile ("dmb ishld" ::: "memory");
		after = shared_battery->sequence;
		if (before == after && !(after & 1)) return true;
	}
	return false;
}

extern void HakiosBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/hakios.battery.cpp */

/*
 * The charger as it is now, from the PM8150B (SPMI slave 2) and not from the loader's sample, which
 * with loader_charger=0 is the boot's and never changes (tester, 2026-10-24: "charging" stayed after
 * the cable came out). Read only, as the vendor's smb5 driver reads them:
 *   USBIN_INT_RT_STS  USBIN_BASE 0x1300 + 0x10, bit 4 = a cable is in (smb5-reg.h:21,31,239;
 *                     smblib_get_prop_usb_present, smb5-lib.c:4219-4225)
 *   BATTERY_CHARGER_STATUS_1  CHGR_BASE 0x1000 + 0x06, [2:0]: 1 trickle, 2 pre, 3 full-on, 4 taper =
 *                     charging; 5 terminated = full; 0 inhibit, 6 pause, 7 disabled (smb5-reg.h:18,36-47)
 * Through hakios_spmi_peek() (the buttons' driver maps the arbiter: buttons=1).
 */
#define CHARGER_USBIN_ID        0x213           /* slave 2, peripheral 0x13 */
#define CHARGER_CHGR_ID         0x210           /* slave 2, peripheral 0x10 */
static int charger_live = -1;                   /* -1 not read yet; else plugged << 4 | status */

/*
 * The fuel gauge as it is now (the PM8150B's FG gen4, qpnp,fg in pm8150b.dtsi:383-420: BATT_SOC 0x4000, BATT_INFO 0x4100),
 * read only, as the vendor's fg-util.c reads it:
 *   monotonic SOC  BATT_SOC + 0x09, its shadow + 0x0a: equal, else read again; percent = raw * 100 / 255
 *                  (fg_get_msoc_raw, fg-util.c:880-900; FULL_SOC_RAW 255, fg-core.h:93-94)
 *   battery volts  BATT_INFO + 0xa0/0xa1, copy at 0xa6/0xa7: LSB first, x 122070 nV (fg-util.c:1034-1072)
 *   battery amps   BATT_INFO + 0xa2/0xa3, copy at 0xa8/0xa9: signed 16 bits x 488281 nA (fg-util.c:990-1030); the
 *                  gauge counts discharge as positive (HYPOTHESIS: inverted for iOS, whose current is > 0 while
 *                  charging; the log prints the raw value next to the charger state to check)
 *   battery amps sign: FACT-backed (runs of 2026-10-25, charger "charging"): raw -61 mA at 3790 mV, +751 mA at
 *                  3660 mV -- the terminal voltage falls as the raw value rises (about 0.15 ohm), so raw > 0 is
 *                  discharge, as the vendor says; iOS gets -raw.
 *   temperature    the gen4 gauge's ADC_RR (DT qcom,fg-rradc@4200; subtype 0x13 = FG_ADC_RR_PM8150B, vendor
 *                  qpnp-fg-gen4.c:38,5948) has BATT_TEMP at +0x88/0x89, MSB [1:0] (vendor vayu-r-oss fg-reg.h:46-48).
 *                  The vendor itself reads the SRAM BATT_TEMP word (fg_gen4_get_battery_temp, qpnp-fg-gen4.c:742-760:
 *                  10 bits signed, 0.25 C a step). REJECTED (run 064554): the register is not in that format -- raw
 *                  0x23..0x25 would be 9 C on a phone whose SoC sensors read 41-48 C while charging. INFERENCE: 1 C a
 *                  step, signed 10 bits (35..37 C there); no source states the unit, the log keeps the raw value. Used if it reads -20..70 C; else gen3's BATT_INFO + 0x50/0x51 (11 bits of
 *                  K/4, qpnp-fg-gen3.c:568-587) under the same check; else the loader's boot reading stays.
 */
#define FG_SOC_ID               0x240           /* slave 2, peripheral 0x40 */
#define FG_INFO_ID              0x241           /* slave 2, peripheral 0x41 */
#define FG_RRADC_ID             0x242           /* slave 2, peripheral 0x42 (qcom,fg-rradc@4200) */

struct fg_live {
	int percent, raw_soc, voltage_mv, current_ma, temperature_c;
	bool soc_ok, voltage_ok, current_ok, temperature_ok;
	int temperature_raw;                    /* the ADC_RR word as read (-1: not read) */
	const char *temperature_from;           /* "rradc", "gen3" or NULL */
};

static int rradc_subtype = -1;                  /* the ADC_RR peripheral's subtype, read once (0x13: PM8150B's) */

static bool
fg_pair(uint16_t id, uint8_t reg, uint8_t copy, uint16_t *value)
{
	uint8_t a[2], b[2];

	for (int tries = 0; tries < 3; tries++) {
		if (!hakios_spmi_peek(id, reg, &a[0]) || !hakios_spmi_peek(id, (uint8_t)(reg + 1), &a[1]) ||
		    !hakios_spmi_peek(id, copy, &b[0]) || !hakios_spmi_peek(id, (uint8_t)(copy + 1), &b[1])) {
			return false;
		}
		if (a[0] == b[0] && a[1] == b[1]) {
			*value = (uint16_t)(a[1] << 8 | a[0]);
			return true;
		}
	}
	return false;
}

static void
fg_read(struct fg_live *f)
{
	uint8_t soc[2], t[2];
	uint16_t v;

	bzero(f, sizeof(*f));
	for (int tries = 0; tries < 3 && !f->soc_ok; tries++) {
		if (hakios_spmi_peek(FG_SOC_ID, 0x09, &soc[0]) && hakios_spmi_peek(FG_SOC_ID, 0x0a, &soc[1]) && soc[0] == soc[1]) {
			f->raw_soc = soc[0];
			f->percent = (soc[0] * 100 + 127) / 255;
			f->soc_ok = true;
		}
	}
	if (fg_pair(FG_INFO_ID, 0xa0, 0xa6, &v)) {
		f->voltage_mv = (int)((uint64_t)v * 122070 / 1000000);
		f->voltage_ok = f->voltage_mv > 2500 && f->voltage_mv < 5000;
	}
	if (fg_pair(FG_INFO_ID, 0xa2, 0xa8, &v)) {
		f->current_ma = (int)((int64_t)(int16_t)v * 488281 / 1000000);
		f->current_ok = true;
	}
	f->temperature_raw = -1;
	if (rradc_subtype < 0 && hakios_spmi_peek(FG_RRADC_ID, 0x05, &t[0])) {
		rradc_subtype = t[0];
	}
	if (rradc_subtype == 0x13 && hakios_spmi_peek(FG_RRADC_ID, 0x88, &t[0]) && hakios_spmi_peek(FG_RRADC_ID, 0x89, &t[1])) {
		int raw = (t[1] & 0x03) << 8 | t[0];

		f->temperature_raw = raw;
		f->temperature_c = raw >= 0x200 ? raw - 0x400 : raw;     /* 1 C a step: see the comment above */
		f->temperature_ok = f->temperature_c >= -20 && f->temperature_c <= 70 && raw != 0;
		f->temperature_from = f->temperature_ok ? "rradc" : NULL;
	}
	if (!f->temperature_ok && hakios_spmi_peek(FG_INFO_ID, 0x50, &t[0]) && hakios_spmi_peek(FG_INFO_ID, 0x51, &t[1])) {
		int kelvin = ((((t[1] & 0x07) << 8) | t[0]) + 2) / 4;

		f->temperature_c = kelvin - 273;
		f->temperature_ok = f->temperature_c >= -20 && f->temperature_c <= 70 && kelvin != 0;
		f->temperature_from = f->temperature_ok ? "gen3" : NULL;
	}
}

/*
 * The charger's limits as UEFI left them, read only (run 064554: +750 mA discharge while "charging", the battery
 * falling overnight). PM8150B SMB5 (vendor vayu-r-oss smb5-reg.h; scales from qpnp-smb5.c smb5_pm8150b_params,
 * 50 mA a step): FCC CHGR 0x1061; USB ICL USBIN 0x1370; ICL max status DCDC 0x1106; AICL ICL status DCDC 0x1108;
 * APSD result USBIN 0x1308 (bit 0 SDP, 2 CDP, 3 DCP, 4 float, 5 QC2, 6 QC3); ICL options USBIN 0x1366 (bit 0
 * USBIN_MODE_CHG = high-current mode, bit 1 USB51 = 500 mA rather than 100, bit 2 USB 3.0); CMD_ICL_OVERRIDE
 * USBIN 0x1342 bit 0; AICL status DCDC 0x110a (bit 0 done, 6 soft limit); power path DCDC 0x110b (bit 4 USBIN in
 * use, bit 6 USBIN suspended); Type-C sink status 0x1506 (bit 3 Rp default, 2 Rp 1.5 A, 1 Rp 3 A).
 */
static void
charger_limits(void)
{
	static const struct { uint16_t id; uint8_t reg; const char *name; } regs[] = {
		{ 0x210, 0x61, "FCC" }, { 0x213, 0x70, "USB ICL" }, { 0x211, 0x06, "ICL max status" },
		{ 0x211, 0x08, "AICL ICL status" }, { 0x213, 0x08, "APSD" }, { 0x213, 0x66, "ICL options" },
		{ 0x213, 0x42, "ICL override" }, { 0x211, 0x0a, "AICL" }, { 0x211, 0x0b, "power path" },
		{ 0x215, 0x06, "Type-C sink" },
	};
	static char said[200];
	char line[200];
	uint8_t v[sizeof(regs) / sizeof(regs[0])];

	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		if (!hakios_spmi_peek(regs[i].id, regs[i].reg, &v[i])) {
			return;
		}
	}
	snprintf(line, sizeof(line), "FCC %u mA, USB ICL %u mA, ICL max %u mA, AICL ICL %u mA, APSD %#x, ICL options %#x, "
	    "override %#x, AICL %#x, power path %#x, Type-C sink %#x", v[0] * 50u, v[1] * 50u, v[2] * 50u, v[3] * 50u, v[4], v[5],
	    v[6], v[7], v[8], v[9]);
	if (strcmp(line, said) != 0) {
		printf("battery: charger limits (read only): %s\n", line);
		strlcpy(said, line, sizeof(said));
	}
}

/*
 * battery_charger_dump=S (o6; default 0: off): at the first poll and then every S seconds (30 or more),
 * the registers of the PM8150B's charger peripherals that charger_limits() reads -- CHGR 0x1000, DCDC
 * 0x1100, USBIN 0x1300, TYPEC 0x1500 -- and of the gauge's ADC_RR 0x4200, all 256 of each, as they are:
 * hex lines "battery: dump 0x213 70: ...", "--" where a read fails, a peripheral with nothing readable left
 * out. Read only, through hakios_spmi_peek(), and nothing decoded: the same port under Android, dumped
 * the same way, says what Android sets that UEFI did not (the USB input limit, its mode bits, AICL)
 * before anything is ever written to the charger here.
 */
static uint32_t charger_dump_secs;

static void
charger_dump(void)
{
	static const uint16_t ids[] = { 0x210, 0x211, 0x213, 0x215, 0x242 };
	static uint64_t next;
	uint64_t now = mach_absolute_time(), every;

	if (charger_dump_secs == 0 || (next != 0 && now < next)) {
		return;
	}
	nanoseconds_to_absolutetime((uint64_t)charger_dump_secs * NSEC_PER_SEC, &every);
	next = now + every;
	printf("battery: charger dump (read only, battery_charger_dump=%u): PM8150B 0x10 CHGR, 0x11 DCDC, 0x13 USBIN, "
	    "0x15 TYPEC, 0x42 the gauge's ADC_RR\n", charger_dump_secs);
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		for (unsigned row = 0; row < 256; row += 16) {
			char line[96];
			int at = snprintf(line, sizeof(line), "battery: dump %#x %02x:", ids[i], row);
			bool any = false;

			for (unsigned col = 0; col < 16 && at > 0 && at < (int)sizeof(line); col++) {
				uint8_t value;

				if (hakios_spmi_peek(ids[i], (uint8_t)(row + col), &value)) {
					at += snprintf(line + at, sizeof(line) - at, " %02x", value);
					any = true;
				} else {
					at += snprintf(line + at, sizeof(line) - at, " --");
				}
			}
			if (any) {
				printf("%s\n", line);
			}
		}
	}
}

static bool
charger_read(bool *plugged, int *status)
{
	uint8_t usbin, chgr;

	if (!hakios_spmi_peek(CHARGER_USBIN_ID, 0x10, &usbin) || !hakios_spmi_peek(CHARGER_CHGR_ID, 0x06, &chgr)) {
		return false;
	}
	*plugged = (usbin & 0x10) != 0;
	*status = chgr & 0x07;
	return true;
}

static void
battery_poll(void)
{
	struct loader_battery sample;

	if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
	    sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
	    !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
		battery_failures++;
		return;
	}
	battery_soc_percent = sample.soc_percent;
	battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
	battery_current_ma = sample.current_ma;
	battery_temperature_c = sample.temperature_c;
	battery_loader_temperature_update(sample.sequence, sample.temperature_c);
	battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
	battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
	battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
	battery_charger_action = (int)sample.charger_action;
	{
		bool plugged;
		int status, now;
		static const char *const names[8] = { "inhibited", "trickle", "pre-charge", "full-on", "taper", "terminated (full)",
			                              "paused", "disabled" };

		if (charger_read(&plugged, &status)) {
			now = (plugged ? 0x10 : 0) | status;
			if (now != charger_live) {
				printf("battery: charger now %s, %s (PM8150B USBIN %s, CHARGER_STATUS_1 %d)%s\n",
				    plugged ? "PLUGGED IN" : "UNPLUGGED", names[status], plugged ? "plug-in" : "no plug-in", status,
				    charger_live < 0 ? " (first read)" : "");
				charger_live = now;
			}
			battery_charger_present = plugged;
			battery_charging = plugged && status >= 1 && status <= 4;
		}
	}
	{
		static int said_percent = -1;
		static unsigned said_fg;
		struct fg_live f;

		fg_read(&f);
		if (f.soc_ok) {
			if (f.percent != said_percent) {
				printf("battery: fuel gauge %d%% (raw %d/255)%s\n", f.percent, f.raw_soc, said_percent < 0 ? " (first read)" : "");
				said_percent = f.percent;
			}
			sample.soc_percent = f.percent;
			battery_soc_percent = f.percent;
			battery_soc_estimated = 0;
		}
		if (f.voltage_ok) {
			sample.voltage_mv = f.voltage_mv;
			sample.flags |= LOADER_BATTERY_VOLTAGE_VALID;
			battery_voltage_mv = f.voltage_mv;
		}
		if (f.current_ok) {
			sample.current_ma = -f.current_ma;      /* (the gauge: discharge > 0; iOS: charge > 0) */
			battery_current_ma = -f.current_ma;
		}
		if (f.temperature_ok) {
			sample.temperature_c = f.temperature_c;
			battery_temperature_update(f.temperature_c);
		}
		if (said_fg % 120 == 0) {
			charger_limits();               /* (said when it changes; looked at once a minute) */
		}
		charger_dump();                         /* (battery_charger_dump=S: every S s) */
		if (said_fg++ % 120 == 0) {                      /* (once a minute) */
			printf("battery: live: %s%d%%, %s%d mV, gauge current %s%d mA (raw sign), %s%d C (%s, rradc %#x raw, subtype %d); charger %s\n",
			    f.soc_ok ? "" : "?", f.percent, f.voltage_ok ? "" : "?", f.voltage_mv, f.current_ok ? "" : "?", f.current_ma,
			    f.temperature_ok ? "" : "?", f.temperature_c, f.temperature_from ? f.temperature_from : "none",
			    f.temperature_raw, rradc_subtype, battery_charging ? "charging" : battery_charger_present ?
			    "plugged, not charging" : "unplugged");
		}
	}
	battery_updates++;
	{
		/* to iOS when what it shows changes (the charger, the percent), else every 5 s: polled every 0.5 s */
		static int last_key = -1;
		static unsigned since;
		int key = (battery_charger_present ? 0x1000 : 0) | (battery_charging ? 0x800 : 0) | sample.soc_percent;

		if (key == last_key && ++since < 10) {
			return;
		}
		last_key = key;
		since = 0;
	}
	if (battery_ios) {
		struct hakios_battery_state st = { 0 };

		st.size = sizeof(st);
		st.percent_hundredths = sample.soc_percent * 100;
		st.voltage_uv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv * 1000 : 0;
		st.current_ua = sample.current_ma * 1000;
		st.temperature_hundredths = sample.temperature_c * 100;
		st.present = battery_present;
		st.external = battery_charger_present;
		if (battery_charging) {
			st.status = 1;
		} else if (battery_charger_present && (sample.soc_percent == 100 || (charger_live & 0x0f) == 5)) {
			st.status = 4;
		} else if (battery_charger_present) {
			st.status = 3;
		} else {
			st.status = 2;
		}
		HakiosBatteryReport(&st, sizeof(st));
	}
}

/*
 * What the audio DSP's battery manager says (bsd/dev/hakios.adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that iOS reads (iokit/Kernel/hakios.battery.cpp). The loader's
 * own reading goes there too unless battery_ios=0.
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_hakios, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_hakios, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

void
hakios_battery_report(const struct hakios_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	/* (rounded in 64 bits: the DSP's value near INT32_MAX plus 50 would overflow, and a hot battery read as cold) */
	battery_temperature_update((int)(((int64_t)b->temperature_hundredths + 50) / 100));
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_source_dsp = 1;
	battery_updates++;
	HakiosBatteryReport(b, sizeof(*b));
}

/*
 * boot-arg vayu_charge=1: keep the PM8150B charging from the SMB5 sequence in
 * drivers/input/hakios.buttons.c of Akhdaa/hakios-xiaomi-munch@15014aa2ffee768d9a7e34c94998804f57a107fe.
 * Pet MISC 0x43 every ten seconds; only raise the USB ICL/FCC for CDP or DCP,
 * and only when below 1500/1800 mA. The write helper refuses channels not owned
 * by this system and verifies every register write.
 */
#define VAYU_CHARGE_USBIN_ID       0x213
#define VAYU_CHARGE_CHGR_ID        0x210
#define VAYU_CHARGE_MISC_ID        0x216
#define VAYU_CHARGE_ICL_MA         1500
#define VAYU_CHARGE_FCC_MA         1800
#define VAYU_CHARGE_APSD_CDP       (1u << 2)
#define VAYU_CHARGE_APSD_DCP       (1u << 3)

static bool charge_broken;
static bool charge_status_seen;
static uint8_t charge_last_input, charge_last_apsd, charge_last_state;
static bool charge_read_error_seen;
static bool charge_watchdog_owner_known, charge_watchdog_owner;
static bool charge_watchdog_owner_last;
static bool charge_current_owner_known, charge_current_owner, charge_current_owner_last;

static bool
vayu_charge_put(uint16_t id, uint8_t reg, uint8_t mask, uint8_t value, const char *what)
{
	uint8_t before, after;
	bool written;

	if (!hakios_spmi_update(id, reg, mask, value, &before, &after, &written)) {
		printf("vayu-charge: %s register %#x failed; no more writes this run\n", what, reg);
		charge_broken = true;
		return false;
	}
	if (written) {
		printf("vayu-charge: write %s register %#x: %#x -> %#x\n", what, reg, before, after);
	}
	return true;
}

static void
vayu_charge_poll(void)
{
	uint8_t input, apsd, state, icl, fcc;

	if (charge_broken) return;
	charge_watchdog_owner = hakios_spmi_owned(VAYU_CHARGE_MISC_ID);
	if (!charge_watchdog_owner_known || charge_watchdog_owner != charge_watchdog_owner_last) {
		printf("vayu-charge: watchdog channel %s\n",
		    charge_watchdog_owner ? "is owned by this system" : "is not owned by this system; no charger writes");
		charge_watchdog_owner_last = charge_watchdog_owner;
		charge_watchdog_owner_known = true;
	}
	if (!charge_watchdog_owner) return;
	if (!vayu_charge_put(VAYU_CHARGE_MISC_ID, 0x43, 0, 0x01, "charger watchdog")) return;
	if (!hakios_spmi_read_live(VAYU_CHARGE_USBIN_ID, 0x10, &input) ||
	    !hakios_spmi_read_live(VAYU_CHARGE_USBIN_ID, 0x08, &apsd) ||
	    !hakios_spmi_read_live(VAYU_CHARGE_CHGR_ID, 0x06, &state)) {
		if (!charge_read_error_seen) {
			printf("vayu-charge: USBIN/APSD/CHGR read failed; no charger writes this interval\n");
			charge_read_error_seen = true;
		}
		return;
	}
	charge_read_error_seen = false;
	if (!charge_status_seen || input != charge_last_input || apsd != charge_last_apsd || state != charge_last_state) {
		printf("vayu-charge: USBIN status 0x1310=%#x APSD 0x1308=%#x CHGR 0x1006=%#x\n",
		    input, apsd, state);
		charge_last_input = input;
		charge_last_apsd = apsd;
		charge_last_state = state;
		charge_status_seen = true;
	}
	if (!(input & (1u << 4)) || !(apsd & (VAYU_CHARGE_APSD_DCP | VAYU_CHARGE_APSD_CDP))) return;
	charge_current_owner = hakios_spmi_owned(VAYU_CHARGE_USBIN_ID) && hakios_spmi_owned(VAYU_CHARGE_CHGR_ID);
	if (!charge_current_owner_known || charge_current_owner != charge_current_owner_last) {
		printf("vayu-charge: current-limit channels %s\n",
		    charge_current_owner ? "are owned by this system" : "are not owned by this system; watchdog only");
		charge_current_owner_last = charge_current_owner;
		charge_current_owner_known = true;
	}
	if (!charge_current_owner) return;
	if (hakios_spmi_read_live(VAYU_CHARGE_USBIN_ID, 0x70, &icl)) {
		if (icl < VAYU_CHARGE_ICL_MA / 50 &&
		    (!vayu_charge_put(VAYU_CHARGE_USBIN_ID, 0x70, 0xff, VAYU_CHARGE_ICL_MA / 50,
		    "USB input current limit") ||
		    !vayu_charge_put(VAYU_CHARGE_USBIN_ID, 0x66, 0x01, 0x01, "USB high-current mode") ||
		    !vayu_charge_put(VAYU_CHARGE_USBIN_ID, 0x42, 0x01, 0x00, "USB ICL override") ||
		    !vayu_charge_put(VAYU_CHARGE_USBIN_ID, 0x65, 0x10, 0x10, "USB ICL override after APSD"))) return;
	} else {
		if (!charge_read_error_seen) {
			printf("vayu-charge: USB ICL read failed; current limits left unchanged\n");
			charge_read_error_seen = true;
		}
	}
	if (!hakios_spmi_read_live(VAYU_CHARGE_CHGR_ID, 0x61, &fcc)) {
		if (!charge_read_error_seen) {
			printf("vayu-charge: battery FCC read failed; current limit left unchanged\n");
			charge_read_error_seen = true;
		}
		return;
	}
	if (fcc < VAYU_CHARGE_FCC_MA / 50) {
		(void)vayu_charge_put(VAYU_CHARGE_CHGR_ID, 0x61, 0xff, VAYU_CHARGE_FCC_MA / 50,
		    "battery fast-charge current");
	}
}

static void
vayu_charge_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		vayu_charge_poll();
		delay_for_interval(10, NSEC_PER_SEC);
	}
}

static void
vayu_charge_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("vayu_charge", &enabled, sizeof(enabled)) || enabled == 0) return;
	if (enabled != 1) {
		printf("vayu-charge: vayu_charge=%u refused; only 1 enables PM8150B charging maintenance\n", enabled);
		return;
	}
	printf("vayu-charge: vayu_charge=1: PM8150B watchdog every 10 s, current limits only raised\n");
	if (kernel_thread_start(vayu_charge_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("vayu-charge: could not start the PM8150B charging thread\n");
	}
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned tick = 0;; tick++) {
		battery_poll();
		if (tick % 120 == 0) {
			printf("battery: SOC=%d%%%s, V=%d mV, I=%d mA, T=%d C, battery=%d charger=%d charging=%d "
			    "action=%d (%llu reads, %llu failed)\n", battery_soc_percent,
			    battery_soc_estimated ? " (voltage estimate)" : "", battery_voltage_mv,
			    battery_current_ma, battery_temperature_c, battery_present, battery_charger_present,
			    battery_charging, battery_charger_action, battery_updates, battery_failures);
		}
		delay_for_interval(500, NSEC_PER_MSEC);       /* (0.5 s: a cable out shows within half a second) */
	}
}

void
hakios_battery_init(void)
{
	uint32_t enabled = 0;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	vayu_charge_init();
	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
	PE_parse_boot_argn("battery_ios", &battery_ios, sizeof(battery_ios));
	if (PE_parse_boot_argn("battery_charger_dump", &charger_dump_secs, sizeof(charger_dump_secs)) &&
	    charger_dump_secs != 0 && charger_dump_secs < 30) {
		charger_dump_secs = 30;
	}
	if (!PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) || shared_pa == 0) {
		printf("battery: xnuloader did not provide battery_shared_pa\n");
		return;
	}
	page = ml_io_map((vm_offset_t)(shared_pa & ~(LOADER_PAGE_SIZE - 1)), LOADER_PAGE_SIZE);
	shared_battery = (volatile struct loader_battery *)(page + (shared_pa & (LOADER_PAGE_SIZE - 1)));
	if (shared_battery->magic != LOADER_BATTERY_MAGIC || shared_battery->version != LOADER_BATTERY_VERSION ||
	    shared_battery->size < sizeof(struct loader_battery)) {
		printf("battery: bad xnuloader cache at %#llx (magic %#llx version %u size %u)\n", shared_pa,
		    shared_battery->magic, shared_battery->version, shared_battery->size);
		return;
	}
	printf("battery: Qualcomm UEFI cache at pa %#llx; polling every 0.5 s, the charger and the fuel gauge read live from the PM8150B\n", shared_pa);
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
