/*
 * The flashlight of the POCO X5 Pro (redwood): the flash LED module of the PM8350C (SPMI slave 2,
 * registers 0xee00; "qcom,pm8350c-flash-led", four channels), the rear flash LED on its channel 0.
 * Every address and number below belongs to this phone, from Android's own device tree, Xiaomi's
 * driver for it (drivers/leds/leds-qti-flash.c of the phone's kernel, MiCode hakios-s-oss) and
 * what Android does when its own flashlight is switched on (its LED class devices read, nothing
 * written by us):
 *
 *   Android's torch    led:torch_0 at 125 mA through led:switch_0 (channel 0 alone), current
 *                      resolution 12.5 mA ("qcom,ires-ua" 12500): IRESOLUTION bit 0 clear, ITARGET
 *                      code round(125 / 12.5) - 1 = 9; the module enabled (0x46 = 0x80), then the
 *                      channel (0x4e bit 0). Off in the reverse order, ITARGET back to 0. The
 *                      torch has no safety timer (its bit is kept clear, as the driver keeps it)
 *   at Android's probe channel 0's strobe is the software's (0x4a: bits 6:4 and 2 clear) and its
 *                      safety timer off (0x3e bit 7 clear)
 *
 * Writing to a PMIC is not like writing anywhere else: the PMICs make the phone's supplies. So
 * every write goes through one function that takes only this module's registers above (the
 * peripheral the arbiter's channel is checked to be, the registers by a list), every write is
 * read back, and any surprise ends all writing for the run. Transfers through the arbiter are made
 * one at a time with the buttons' driver (hakios_spmi_lock()): two at once, and it drops one.
 *
 * What iOS's flashlight is on this machine: the loader says the camera has a flash (with torch=N,
 * /product/camera "flash": MobileGestalt's camera-flash, Control Center's flashlight button), and
 * libhakios.media.dylib in mediaserverd hands FigFlashlight, which finds no camera here, a device
 * of ours whose torch level is written to /dev/hakios-torch (userland/src/inject_media.c).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   torch=1    a look, reading only: the arbiter's channel for the module and who may write
 *              through it, the module's type, its registers, the PMIC's temperature alarm
 *   torch=2    ... and the light on once at Android's torch current for torch_secs (default 2),
 *              then off, every write read back
 *   torch=3    ... no light at the start; the light for iOS until the run ends: /dev/hakios-torch
 *              takes a level of 0 to 1000 (a 32-bit number; thousandths of Android's current, 0
 *              off) and the module is set to it -- never above Android's 125 mA; a read gives
 *              what is lit now
 *
 * Limits, whatever asks: Android's current at most; the light on for torch_max_s at a time at most
 * (default 30, never more than 120), after which it stays off until it is asked for anew (0, then
 * more); while it is on, ten times a second, the module's fault and over-temperature flags, the
 * PMIC's temperature alarm and the battery's temperature (at most 45 C; from the audio DSP's battery
 * manager with adsp=1, or the firmware's with battery=1; with neither the other two guard): any of
 * them, a read that fails or a write that does not read back, and the light goes off and stays off
 * for the rest of the run. At the run's end (and on a panic's way out) it goes off.
 *
 * The POCO X3 Pro (vayu) has instead the flash LED module of the PM8150L (SPMI slave 5, registers
 * 0xd300: arbiter channel 333, type 0x18, subtype 0x04; "qcom,qpnp-flash-led-v2", three channels),
 * whose registers are not the PM8350C's. Its map and sequence are Xiaomi's vayu kernel
 * (MiCode/Xiaomi_Kernel_OpenSource vayu-r-oss): drivers/leds/leds-qpnp-flash-v2.c (sha256
 * ee5d3031b6f9bcd8e28224a15270d8c69f255c1181ff80aaf8cd63fbec631b32; "v2:N" below is its line N)
 * and arch/arm64/boot/dts/qcom/pm8150l.dtsi (qcom,leds@d300). The rear flash is LED1 and LED2
 * (torch_0 and torch_1 through led:switch_2, mask 3: vayu-sm8150-camera-sensor-mtp.dtsi); a torch
 * node has 12.5 mA steps and no safety timer. The sequence, qpnp_flash_led_switch_set() (v2:1488)
 * and _switch_disable() (v2:1294), for the LEDs of torch_leds (3, both, by default as the vendor flashlight; 1 LED1 alone,
 *  torch_ma in all, split between them; module-wide probe writes first):
 *
 *   on    IRES (0x47) 2 bits per LED: 0 for 12.5 mA, 3 for 5 mA at 20 mA or less (PM8150L_IRES_WA,
 *         v2:1272); per LED: STROBE_CTRL (0x49 + n) bit 2 clear (software strobe), TGR_CURRENT
 *         (0x43 + n) round(mA / step) - 1, SAFETY_TMR (0x40 + n) 0x13 (off); then MOD_CTRL (0x46)
 *         bit 7, then EN_LED_CTRL (0x4c) the LEDs' bits
 *   off   EN_LED_CTRL bits clear, MITIGATION_SW (0x6f) bit 4 clear, MOD_CTRL bit 7 clear; per LED
 *         TGR_CURRENT 0, SAFETY_TMR 0x13
 *   probe per LED HDRM_PRGM (0x4d + n) 0x84 (325 mV headroom, 100 mV window) and SAFETY_TMR 0x13
 *         (v2:441-453); the module-wide probe writes (headroom auto mode, delays, derating) are
 *         not made
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (arbiter version 5: a system's write
 * channel for a peripheral is 0x10000 bytes of "chnls" per channel), Xiaomi's leds-qti-flash.c,
 * drivers/thermal/qcom/qcom-spmi-temp-alarm.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level;
static bool broken, leaving;

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well (and to the console when that may be written) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

/*
 * The events that matter (lit, off, cut, trouble) are also kept, and said again at the run's end:
 * the log of a long run keeps its beginning and its end, and these happen in between.
 */
#define NOTES                   24
static char notes[NOTES][200];
static unsigned noted;

static void note(const char *format, ...) __printflike(1, 2);

static void
note(const char *format, ...)
{
	char *slot = notes[noted++ % NOTES];
	uint64_t ns;
	va_list list;
	int n;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	n = snprintf(slot, sizeof(notes[0]), "%llu.%03llu s: ", ns / NSEC_PER_SEC, ns / NSEC_PER_MSEC % 1000);
	va_start(list, format);
	vsnprintf(slot + n, sizeof(notes[0]) - (size_t)n, format, list);
	va_end(list);
	say("torch: %s\n", slot);
}

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

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- the PMIC arbiter (version 5) ---------------------------------------------------------------- */

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
	vm_offset_t     write;          /* its write channel, mapped (only the flash module's) */
};
static struct peripheral flash = { .name = "the PM8150L's flash LED module", .id = 0x5d3 };   /* pm8150l.dtsi: interrupts <0x5 0xd3 ..> */
static struct peripheral alarm = { .name = "the PM8150L's temperature alarm", .id = 0x424 };

static vm_offset_t arb_core, arb_cnfg, arb_observer;
static uint8_t alarm_subtype;

/* (the arbiter one transfer at a time with the buttons' driver; at the run's end the others are stopped, perhaps holding it) */
static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
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

/*
 * `count' (1 to 8) registers from `reg' on; NULL, or what went wrong. A transfer the arbiter drops
 * now and then (the buttons' driver sees one in a thousand or so, from whatever else is on the bus)
 * is asked for again, twice at most.
 */
static const char *spmi_read_once(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count);

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

/* ---- the flash LED module -------------------------------------------------------------------------- */

#define FLASH_REVISION1         0x00
#define FLASH_TYPE              0x04            /* 0x18: a flash LED module */
#define FLASH_SUBTYPE           0x05            /* 0x07: four channels */
#define FLASH_STATUS1           0x06            /* a channel open or shorted */
#define FLASH_STATUS2           0x07
#define  STATUS2_OTST1          0x20            /* over-temperature, the first and the second threshold */
#define  STATUS2_OTST2          0x10
#define  STATUS2_VPH_LOW        0x01            /* the supply drooped */
#define FLASH_INT_RT_STS        0x10
#define  RT_STS_FAULT           0x01            /* open, short, or its supply not ready */
#define FLASH_SAFETY_TIMER0     0x3e            /* [7] on */
#define FLASH_ITARGET0          0x42            /* [6:0]: (code + 1) x the resolution */
#define FLASH_MODULE_EN         0x46            /* [7] */
#define FLASH_IRESOLUTION       0x49            /* a bit per channel: 0 12.5 mA, 1 5 mA */
#define FLASH_STROBE_CTRL0      0x4a            /* [6:4] hardware strobe's kind, [2] hardware strobe */
#define FLASH_EN_LED_CTRL       0x4e            /* a bit per channel */
#define FLASH_OTST2_CFG1        0x78
#define FLASH_OTST1_CFG1        0x7a

#define ALARM_SUBTYPE           0x05            /* 0x08: the first generation, 0x09 the second */
#define ALARM_STATUS            0x08            /* the over-temperature stage: [1:0] (first generation) or [6:4] */

#define ANDROID_TORCH_MA        125             /* led:torch_0 while Android's flashlight is on */
#define IRES_UA                 12500           /* its resolution ("qcom,ires-ua") */
#define ANDROID_ITARGET         9               /* round(125000 / 12500) - 1: the most ever written */
#define HOTTEST_BATTERY_C       45

static const uint8_t writable_regs[] = {
	FLASH_SAFETY_TIMER0, FLASH_ITARGET0, FLASH_MODULE_EN, FLASH_IRESOLUTION, FLASH_STROBE_CTRL0, FLASH_EN_LED_CTRL,
};

/* The PM8150L's module, subtype 0x04 (leds-qpnp-flash-v2.c of vayu-r-oss; "v2:N" is its line N) */
#define V2_SUBTYPE              0x04
#define V2_STATUS1              0x08            /* v2:36, a LED open or shorted */
#define V2_STATUS2              0x09            /* v2:38 */
#define  V2_STATUS2_OTST        0x07            /* the over-temperature level (v2:1158-1167) */
#define  V2_STATUS2_VPH_DROOP   0x10            /* v2:39 */
#define V2_SAFETY_TMR(n)        (0x40 + (n))    /* v2:43 */
#define  V2_SAFETY_TMR_OFF      0x13            /* v2:184 */
#define V2_TGR_CURRENT(n)       (0x43 + (n))    /* v2:46, [6:0] (v2:110) */
#define V2_MOD_CTRL             0x46            /* v2:48, [7] (v2:50) */
#define V2_IRES                 0x47            /* v2:52, 2 bits per LED (v2:1516-1524) */
#define  V2_IRES_12P5           0               /* v2:170-172, 1980-1985: 3 - (12500 - 5000) / 2500 */
#define  V2_IRES_5P0            3
#define V2_STROBE_CTRL(n)       (0x49 + (n))    /* v2:57, [2] hardware strobe (v2:58) */
#define V2_EN_LED_CTRL          0x4c            /* v2:61, a bit per LED */
#define V2_HDRM_PRGM(n)         (0x4d + (n))    /* v2:64 */
#define  V2_HDRM_VAYU           0x84            /* ((325 - 125) / 25) << 4 | 100 / 25 (v2:2035-2049, pm8150l.dtsi:208-209) */
#define V2_MITIGATION_SW        0x6f            /* v2:116, [4] the charger's mitigation (v2:118) */
#define V2_MIN_MA               12              /* the torch's least: its ires 12500 / 1000 (v2:1248-1253) */
/*
 * Module-wide probe writes of qpnp_flash_led_init_settings() (v2:441-641), vendor defaults since vayu's DT
 * sets none of them (VAYU_DIFF_PMIC.md T4-T10): HDRM_AUTO_MODE_CTRL 0x50 = a bit per fnode *id* (the LED
 * channel, 0-2: val |= 1 << fnode[i].id, v2:446, 503-505) = 0x07 on this three-channel module -- 0x0f, a bit per
 * fnode, read back 0x07 and stopped the light for the run (test run 194827); ISC_DELAY 0x52 [1:0] = 3 (v2:71-72, 141,
 * 510); THERMAL_RMP_DN_RATE 0x55 [7] OTST1 ramp backup on (v2:75-77, 530-544, otst_ramp_bkup_en default);
 * VPH_DROOP_DEBOUNCE 0x62 [1:0] = 2 (v2:99-100, 142, 594-597); ILED_GRT_THRSH 0x67 [5:0] = 0x0a (v2:102-103,
 * 639-641, default FLASH_LED_CHGR_MITIGATION_THRSH_DEFAULT 0xA at v2:166, 2643).
 */
#define V2_HDRM_AUTO_MODE       0x50
#define V2_ISC_DELAY            0x52
#define V2_THERMAL_RMP_DN       0x55
#define V2_VPH_DROOP_DEBOUNCE   0x62
#define V2_ILED_GRT_THRSH       0x67
/*
 * The flashlight lights both torch nodes with the same value (v2:1777-1781); each torch node's DT default
 * is qcom,current-ma = 300, its max 500 (pm8150l.dtsi:240-257, live DT 0x12c). Default here: 300 mA in all
 * (150 per LED); torch_ma=N (total, 25..600) up to the DT default per LED.
 */
#define V2_TORCH_MA             300
#define V2_LED_MAX_MA           300
#define V2_WA_MAX_MA            20              /* 20 mA or less in 5 mA steps on the PM8150L (v2:1272-1275, 2299) */

static const uint8_t v2_writable_regs[] = {
	V2_SAFETY_TMR(0), V2_SAFETY_TMR(1), V2_TGR_CURRENT(0), V2_TGR_CURRENT(1), V2_MOD_CTRL, V2_IRES, V2_STROBE_CTRL(0),
	V2_STROBE_CTRL(1), V2_EN_LED_CTRL, V2_HDRM_PRGM(0), V2_HDRM_PRGM(1), V2_MITIGATION_SW,
	V2_HDRM_AUTO_MODE, V2_ISC_DELAY, V2_THERMAL_RMP_DN, V2_VPH_DROOP_DEBOUNCE, V2_ILED_GRT_THRSH,
};

static bool v2;                 /* the module is the PM8150L's */
static uint32_t v2_leds = 3;    /* torch_leds: LED1 and LED2 (3, the vendor's flashlight), or LED1 (1) */
static uint32_t v2_torch_ma = V2_TORCH_MA;      /* torch_ma: the total at full brightness */

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		note("%s: no more writing to the flash module for the rest of the run", why);
	}
}

/*
 * One register of the flash module (and of the six above only) written and read back: false, and
 * no more writing, if it cannot be or does not read back.
 */
/*
 * `verify': the bits that must read back as written -- the ones the write means to set. Bits of the
 * register the write leaves as they were (a masked write) may be changed by the module itself
 * meanwhile (status, self-clearing) and are not held against it: after the first light (test run
 * 001804, lit ~0.5 s) one such mismatch is the likeliest way the light was given up on for the run.
 */
static bool
put_verified(uint8_t reg, uint8_t value, uint8_t verify)
{
	const char *problem;
	uint8_t back = 0xff;
	bool allowed = false;

	if (v2) {
		for (size_t i = 0; i < sizeof(v2_writable_regs); i++) {
			allowed |= v2_writable_regs[i] == reg;
		}
	} else {
		for (size_t i = 0; i < sizeof(writable_regs); i++) {
			allowed |= writable_regs[i] == reg;
		}
	}
	if (broken || !allowed || !flash.writable || flash.write == 0) {
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	/* (the channel is still the module's: its entry in the arbiter's table is looked at again) */
	if (((rd(arb_core + ARB_CHANNEL(flash.channel)) >> 8) & 0xfff) != flash.id) {
		give_up("the arbiter's channel is no longer the flash module's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(flash.write + CHANNEL_WDATA0, value);
		wr(flash.write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(flash.write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;          /* (a dropped write has not been made: asked for again, then read back) */
		}
	}
	if (problem != NULL || (problem = spmi_read(&flash, reg, &back, 1)) != NULL) {
		say("torch: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if ((back & verify) != (value & verify)) {
		say("torch: %#04x <- %#04x, but it reads %#04x (the bits %#04x matter)\n", reg, value, back, verify);
		give_up("a register that did not read back as written");
		return false;
	}
	if (back != value) {
		say("torch: %#04x <- %#04x reads %#04x: bits outside %#04x changed by the module (not held against it)\n", reg,
		    value, back, verify);
	}
	return true;
}

static bool
put(uint8_t reg, uint8_t value)
{
	return put_verified(reg, value, 0xff);
}

static bool
get(const struct peripheral *p, uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(p, reg, value, 1);

	if (problem != NULL) {
		say("torch: reading %#04x of %s: %s\n", reg, p->name, problem);
		return false;
	}
	return true;
}

/* only the bits of `mask' changed, as the driver's masked writes do */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(&flash, reg, &now)) {
		give_up("a read that failed");
		return false;
	}
	return (now & mask) == (value & mask) || put_verified(reg, (uint8_t)((now & ~mask) | (value & mask)), mask);
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
		say("torch: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == ARB_SYSTEM;
		}
	}
	if (!p->found) {
		say("torch: no channel of the arbiter is %s\n", p->name);
	}
}

static void
show_registers(void)
{
	uint8_t r[8];
	char line[200];

	for (unsigned base = 0; base < 0x100; base += 0x20) {
		int n = snprintf(line, sizeof(line), "torch: %#04x:", base);

		for (unsigned i = 0; i < 0x20; i += 8) {
			if (spmi_read(&flash, (uint8_t)(base + i), r, 8) != NULL) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " (unreadable)");
				continue;
			}
			for (unsigned j = 0; j < 8; j++) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", r[j]);
			}
		}
		say("%s\n", line);
	}
}

/* What would make the light go off now, or NULL. */
static const char *
trouble(void)
{
	static char why[180];
	uint8_t rt = 0, status1 = 0, status2 = 0, alarm_status = 0, stage;

	uint8_t bad2 = v2 ? V2_STATUS2_OTST | V2_STATUS2_VPH_DROOP : STATUS2_OTST1 | STATUS2_OTST2 | STATUS2_VPH_LOW;

	if (!get(&flash, FLASH_INT_RT_STS, &rt) || !get(&flash, v2 ? V2_STATUS1 : FLASH_STATUS1, &status1) ||
	    !get(&flash, v2 ? V2_STATUS2 : FLASH_STATUS2, &status2) || (alarm.found && !get(&alarm, ALARM_STATUS, &alarm_status))) {
		return "a read of the flags that failed";
	}
	stage = alarm_subtype == 0x08 ? alarm_status & 0x03 : (alarm_status >> 4) & 0x07;
	/*
	 * On the PM8150L's module (v2) a channel's open/short flag (LED_STATUS1) and the fault bit of INT_RT_STS
	 * that goes with it are what the vendor's driver only reports ("led short/open fault detected!",
	 * qpnp_flash_led_irq_handler, leds-qpnp-flash-v2.c:1862-1886): it goes on driving the LEDs, and so does
	 * Android's torch on this phone. Test run 005616 read STATUS1 0x08 / RT 0x81 100 ms after the light came
	 * on, and this driver put the light out for the run. They are said once per lighting here; what stops the
	 * light on v2 is over-temperature, VPH droop (STATUS2), the PMIC's temperature stage or a hot battery.
	 */
	if (v2 && ((rt & RT_STS_FAULT) || status1 != 0) && !(status2 & bad2) && stage == 0) {
		static uint8_t said_rt = 0xff, said_status1 = 0xff;

		if (rt != said_rt || status1 != said_status1) {
			note("v2: the module reports fault %#x, channels %#x (open/short; reported only, as the vendor's driver does)",
			    rt, status1);
			said_rt = rt;
			said_status1 = status1;
		}
		rt &= (uint8_t)~RT_STS_FAULT;
		status1 = 0;
	}
	if ((rt & RT_STS_FAULT) || status1 != 0 || (status2 & bad2) || stage != 0) {
		uint8_t more[8] = { 0 }, latched[2] = { 0 };

		(void)spmi_read(&flash, 0x06, more, 8);
		(void)spmi_read(&flash, 0x18, latched, 2);
		snprintf(why, sizeof(why), "the module says fault %#x, channels %#x, temperature and supply %#x (0x06-0x0d: %02x %02x %02x "
		    "%02x %02x %02x %02x %02x, latched %#x); the PMIC's temperature stage %u", rt, status1, status2, more[0], more[1],
		    more[2], more[3], more[4], more[5], more[6], more[7], latched[0], stage);
		return why;
	}
	if (hakios_battery_temperature() >= HOTTEST_BATTERY_C) {
		snprintf(why, sizeof(why), "the battery is at %d C", hakios_battery_temperature());
		return why;
	}
	return NULL;
}

static bool configured, lit;

/* ---- the PM8150L's module (subtype 0x04) ------------------------------------------------------------ */

/* Android's probe for the LEDs used: their headroom, their safety timers off (v2:441-453). */
static bool
v2_configure(void)
{
	if (!configured && (!put_bits(V2_HDRM_AUTO_MODE, 0xff, 0x07) || !put_bits(V2_ISC_DELAY, 0x03, 0x03) ||
	    !put_bits(V2_THERMAL_RMP_DN, 0x80, 0x80) || !put_bits(V2_VPH_DROOP_DEBOUNCE, 0x03, 0x02) ||
	    !put_bits(V2_ILED_GRT_THRSH, 0x3f, 0x0a))) {
		return false;
	}
	for (unsigned n = 0; n < 2 && !configured; n++) {
		if ((v2_leds & (1u << n)) && (!put_bits(V2_HDRM_PRGM(n), 0xff, V2_HDRM_VAYU) ||
		    !put_bits(V2_SAFETY_TMR(n), 0xff, V2_SAFETY_TMR_OFF))) {
			return false;
		}
	}
	configured = true;
	return true;
}

/* qpnp_flash_led_switch_disable() (v2:1294-1352) for the LEDs of torch_leds */
static bool
v2_dark(void)
{
	bool ok = put_bits(V2_EN_LED_CTRL, (uint8_t)v2_leds, 0x00) && put_bits(V2_MITIGATION_SW, 0x10, 0x00) &&
	    put_bits(V2_MOD_CTRL, 0x80, 0x00);

	for (unsigned n = 0; n < 2 && ok; n++) {
		if (v2_leds & (1u << n)) {
			ok = put_bits(V2_TGR_CURRENT(n), 0x7f, 0x00) && put_bits(V2_SAFETY_TMR(n), 0xff, V2_SAFETY_TMR_OFF);
		}
	}
	lit = !ok && lit;
	return ok;
}

/*
 * qpnp_flash_led_node_set() (v2:1244-1292) and _switch_set() (v2:1488-1634): `permille' of Android's
 * 125 mA split between the LEDs. A new level while lit goes through off first: the driver never
 * changes the current of a lit switch.
 */
static bool
v2_light(uint32_t permille)
{
	uint32_t count = (v2_leds & 1) + ((v2_leds >> 1) & 1), ma, ires_ua, code;
	uint8_t ires = 0, ires_mask = 0;
	const char *why;

	ma = v2_torch_ma * permille / 1000 / count;
	ma = ma < V2_MIN_MA ? V2_MIN_MA : ma;
	ires_ua = ma <= V2_WA_MAX_MA ? 5000 : IRES_UA;
	code = (ma * 1000 + ires_ua / 2) / ires_ua - 1;         /* get_current_reg_code() (v2:362-368) */
	if ((code + 1) * ires_ua > V2_LED_MAX_MA * 1000u) {
		give_up("a current above the DT torch current per LED");
		return false;
	}
	if (lit && !v2_dark()) {
		return false;
	}
	if ((why = trouble()) != NULL) {
		note("not lit: %s", why);
		give_up("trouble before the light");
		return false;
	}
	note("v2: no trouble; probe writes (HDRM, SAFETY_TMR)");
	if (!v2_configure()) {
		return false;
	}
	for (unsigned n = 0; n < 2; n++) {
		if (v2_leds & (1u << n)) {
			ires |= (uint8_t)((ires_ua == IRES_UA ? V2_IRES_12P5 : V2_IRES_5P0) << (2 * n));
			ires_mask |= (uint8_t)(3u << (2 * n));
		}
	}
	note("v2: IRES %#04x (mask %#04x)", ires, ires_mask);
	if (!put_bits(V2_IRES, ires_mask, ires)) {
		return false;
	}
	note("v2: per LED STROBE_CTRL, TGR_CURRENT %u, SAFETY_TMR", code);
	for (unsigned n = 0; n < 2; n++) {
		if ((v2_leds & (1u << n)) && (!put_bits(V2_STROBE_CTRL(n), 0x04, 0x00) ||
		    !put_bits(V2_TGR_CURRENT(n), 0x7f, (uint8_t)code) || !put_bits(V2_SAFETY_TMR(n), 0xff, V2_SAFETY_TMR_OFF))) {
			return false;
		}
	}
	lit = true;             /* (from here on whatever fails is switched off: here, at the run's end) */
	note("v2: MOD_CTRL <- 0x80 next");
	if (!put_bits(V2_MOD_CTRL, 0x80, 0x80) || (note("v2: MOD_CTRL written; EN_LED_CTRL <- %#04x next", v2_leds), false) ||
	    !put_bits(V2_EN_LED_CTRL, (uint8_t)v2_leds, (uint8_t)v2_leds)) {
		broken = false;         /* (one last time, to switch off) */
		(void)v2_dark();
		broken = true;
		return false;
	}
	note("v2: EN_LED_CTRL written: the LEDs are on");
	return true;
}

/* Android's probe, once: channel 0 strobed by software, its safety timer off. */
static bool
configure(void)
{
	if (!configured && put_bits(FLASH_STROBE_CTRL0, 0x74, 0x00) && put_bits(FLASH_SAFETY_TIMER0, 0x80, 0x00)) {
		configured = true;
	}
	return configured;
}

/* The light at `permille' of Android's current: its writes in its order. */
static bool
light(uint32_t permille)
{
	uint32_t ua = ANDROID_TORCH_MA * 1000 * permille / 1000, code;
	const char *why;

	if (v2) {
		return v2_light(permille);
	}
	if (ua < IRES_UA) {
		ua = IRES_UA;           /* (the driver's least: one step) */
	}
	code = (ua + IRES_UA / 2) / IRES_UA - 1;
	if (code > ANDROID_ITARGET) {
		code = ANDROID_ITARGET;
	}
	if (!lit && (why = trouble()) != NULL) {
		note("not lit: %s", why);
		give_up("trouble before the light");
		return false;
	}
	if (!configure() || !put_bits(FLASH_IRESOLUTION, 0x01, 0x00) || !put_bits(FLASH_ITARGET0, 0x7f, (uint8_t)code)) {
		return false;
	}
	if (!lit && (!put(FLASH_MODULE_EN, 0x80) || !put_bits(FLASH_EN_LED_CTRL, 0x01, 0x01))) {
		return false;
	}
	lit = true;
	return true;
}

/* Off, in Android's order: the channel, the module, the target current, the timer's bit. */
static bool
dark(void)
{
	bool ok;

	if (v2) {
		return v2_dark();
	}
	ok = put_bits(FLASH_EN_LED_CTRL, 0x01, 0x00) && put(FLASH_MODULE_EN, 0x00) && put_bits(FLASH_ITARGET0, 0x7f, 0x00) &&
	    put_bits(FLASH_SAFETY_TIMER0, 0x80, 0x00);

	lit = !ok && lit;
	return ok;
}

/* ---- torch=3: the light for iOS --------------------------------------------------------------------- */

static volatile uint32_t wanted;                /* the level asked for last, 0 to 1000 */
static volatile uint32_t asked;                 /* how many times one was */
static uint32_t max_secs = 30;

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value;
	int error;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(value)) {
		if ((error = uiomove((caddr_t)&value, sizeof(value), uio)) != 0) {
			return error;
		}
		wanted = value > 1000 ? 1000 : value;
		asked++;
	}
	return 0;
}

/* what is lit now: the level, 0 if nothing */
static uint32_t shown;

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t value = shown;

	return uiomove((caddr_t)&value, (int)MIN(sizeof(value), (size_t)uio_resid(uio)), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
serve(void)
{
	uint32_t applied = 0, times = 0, cut = 0;
	uint64_t since = 0, last_look = 0;
	bool waiting_for_off = false;           /* cut at the limit: dark until iOS asks for 0 */
	int major = cdevsw_add(-1, &torch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
		say("torch: /dev/hakios-torch cannot be made\n");
		return;
	}
	say("torch: THE LIGHT IS IOS'S (/dev/hakios-torch) from here on, until the run ends: at most Android's %u mA, at most %u s "
	    "at a time\n", ANDROID_TORCH_MA, max_secs);
	while (!broken) {
		uint32_t want = wanted;
		uint64_t now = now_ms();

		if (want == 0) {
			waiting_for_off = false;
		}
		if (waiting_for_off) {
			want = 0;
		}
		if (want != applied) {
			if (want == 0 ? dark() : light(want)) {
				if (applied == 0) {
					since = now;
					times++;
				}
				if (want == 0) {
					note("off (iOS asked for 0)");
				} else {
					note("ON (iOS asked for %u of 1000; the battery %d C)", wanted, hakios_battery_temperature());
				}
				applied = want;
				shown = want;
			} else {
				break;
			}
		}
		if (applied != 0 && now - since >= max_secs * 1000ull) {
			cut++;
			note("on for %u s, the limit: off until iOS asks again", max_secs);
			waiting_for_off = true;
			if (!dark()) {
				break;
			}
			applied = 0;
			shown = 0;
			continue;
		}
		if (applied != 0 && now - last_look >= 100) {
			const char *why;

			last_look = now;
			if ((why = trouble()) != NULL) {
				note("OFF: %s", why);
				(void)dark();
				give_up("trouble while lit");
				break;
			}
		}
		wait_ms(20);
	}
	if (lit) {
		(void)dark();
	}
	shown = 0;
	say("torch: the light is given up on (%u times lit, %u cut at %u s)\n", times, cut, max_secs);
}

/* ---- the start ------------------------------------------------------------------------------------- */

static void
torch_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t version, channels;
	uint8_t id[2] = { 0, 0 }, revision = 0, alarm_status = 0;
	uint32_t chosen = 0;

	if (PE_parse_boot_argn("torch_id", &chosen, sizeof(chosen)) && chosen != 0) {
		flash.id = (uint16_t)chosen;
		say("torch: using peripheral id %#x from torch_id\n", flash.id);
	}
	say("torch: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the light on once" :
	    "the light for iOS");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		say("torch: PMIC arbiter version %#x, not 5: nothing done\n", version);
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	find_channel(&flash, channels);
	find_channel(&alarm, channels);
	if (!flash.found || spmi_read(&flash, FLASH_TYPE, id, 2) != NULL || spmi_read(&flash, FLASH_REVISION1, &revision, 1) != NULL) {
		say("torch: the flash module cannot be read\n");
		goto out;
	}
	say("torch: the module says type %#x, subtype %#x, revision %#x%s\n", id[0], id[1], revision,
	    id[0] == 0x18 && id[1] == 0x07 ? ": a flash LED module of four channels (the PM8350C's map)" :
	    id[0] == 0x18 && id[1] == V2_SUBTYPE ? ": a flash LED module of three channels (the PM8150L's qpnp-flash-v2 map)" :
	    ": NOT WHAT WAS EXPECTED");
	show_registers();
	if (alarm.found && get(&alarm, ALARM_SUBTYPE, &alarm_subtype) && get(&alarm, ALARM_STATUS, &alarm_status)) {
		uint8_t stage = alarm_subtype == 0x08 ? alarm_status & 0x03 : (alarm_status >> 4) & 0x07;

		say("torch: the PMIC's temperature alarm (subtype %#x) says %#x: stage %u%s\n", alarm_subtype, alarm_status, stage,
		    stage == 0 ? " (all is well)" : "");
	} else {
		alarm.found = false;
	}
	if (id[0] != 0x18 || (id[1] != 0x07 && id[1] != V2_SUBTYPE) || level < 2) {
		goto out;
	}
	v2 = id[1] == V2_SUBTYPE;
	if (v2 && PE_parse_boot_argn("torch_ma", &v2_torch_ma, sizeof(v2_torch_ma))) {
		v2_torch_ma = v2_torch_ma < 25 ? 25 : v2_torch_ma > 2 * V2_LED_MAX_MA ? 2 * V2_LED_MAX_MA : v2_torch_ma;
	}
	if (v2 && PE_parse_boot_argn("torch_leds", &v2_leds, sizeof(v2_leds)) && v2_leds != 1 && v2_leds != 3) {
		say("torch: torch_leds=%u is neither 1 (LED1) nor 3 (LED1 and LED2): nothing written\n", v2_leds);
		goto out;
	}
	if (!flash.writable) {
		say("torch: the channel is not ours to write through: nothing written\n");
		goto out;
	}
	flash.write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * flash.channel, 0x1000);
	PE_parse_boot_argn("torch_max_s", &max_secs, sizeof(max_secs));
	max_secs = max_secs < 1 ? 1 : max_secs > 120 ? 120 : max_secs;
	if (level == 2) {
		uint32_t secs = 2;
		uint64_t until;
		const char *why = NULL;

		PE_parse_boot_argn("torch_secs", &secs, sizeof(secs));
		secs = secs < 1 ? 1 : secs > max_secs ? max_secs : secs;
		if (v2) {
			say("torch: the light ON at %u mA in all (LEDs %#x, 12.5 mA steps) for %u s\n", v2_torch_ma, v2_leds, secs);
		} else {
			say("torch: the light ON at Android's %u mA (ITARGET %u, 12.5 mA steps) for %u s\n", ANDROID_TORCH_MA, ANDROID_ITARGET,
			    secs);
		}
		note("torch=2: lighting now");
		if (light(1000)) {
			uint8_t rt = 0, s1 = 0, s2 = 0;

			note("light() returned; waiting 50 ms");
			wait_ms(50);
			note("the 50 ms are over");
			(void)get(&flash, FLASH_INT_RT_STS, &rt);
			(void)get(&flash, v2 ? V2_STATUS1 : FLASH_STATUS1, &s1);
			(void)get(&flash, v2 ? V2_STATUS2 : FLASH_STATUS2, &s2);
			note("lit at Android's %u mA; 50 ms on: interrupt status %#x (%s), channels %#x, temperature and supply %#x",
			    ANDROID_TORCH_MA, rt, (rt & 0x10) ? "ramped up" : "NOT ramped up", s1, s2);
			if (v2) {
				uint8_t en = 0, mod = 0, tgr = 0;

				(void)get(&flash, V2_MOD_CTRL, &mod);
				(void)get(&flash, V2_EN_LED_CTRL, &en);
				(void)get(&flash, V2_TGR_CURRENT(0), &tgr);
				note("PM8150L: MOD_CTRL %#04x, EN_LED_CTRL %#04x, TGR_CURRENT0 %#04x, INT_RT_STS %#04x (ramp-up bit 4, fault bit 0)",
				    mod, en, tgr, rt);
			}
			show_registers();
			for (until = now_ms() + secs * 1000ull; now_ms() < until && (why = trouble()) == NULL;) {
				wait_ms(100);
			}
			if (why != NULL) {
				note("OFF early: %s", why);
			}
			if (dark()) {
				note("off again after %u s", secs);
			}
			show_registers();
		}
	} else {
		serve();
	}
out:
	say("torch: done\n");
	thread_terminate(current_thread());
}

void
hakios_torch_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("torch", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 3 ? 3 : level;
	if (kernel_thread_start(torch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the light off, if it was ever lit. */
void
hakios_torch_quiesce(void)
{
	uint8_t module = 0xff, channels = 0xff, target = 0xff;

	if (level < 2 || flash.write == 0) {
		return;
	}
	leaving = true;
	if (lit) {
		broken = false;         /* (one last time, whatever went before) */
		say("torch: at the run's end: the light %s\n", dark() ? "switched off" : "COULD NOT BE SWITCHED OFF");
	}
	say("torch: what happened in this run (%u events):\n", noted);
	for (unsigned i = noted > NOTES ? noted - NOTES : 0; i < noted; i++) {
		say("torch:   %s\n", notes[i % NOTES]);
	}
	(void)spmi_read(&flash, v2 ? V2_MOD_CTRL : FLASH_MODULE_EN, &module, 1);
	(void)spmi_read(&flash, v2 ? V2_EN_LED_CTRL : FLASH_EN_LED_CTRL, &channels, 1);
	(void)spmi_read(&flash, v2 ? V2_TGR_CURRENT(0) : FLASH_ITARGET0, &target, 1);
	say("torch: at the run's end the module reads: enable %#04x, channels %#04x, target %#04x%s\n", module, channels, target,
	    (module & 0x80) == 0 && (channels & (v2 ? 0x07 : 0x01)) == 0 && (target & 0x7f) == 0 ? " (dark)" : " (NOT DARK)");
}
