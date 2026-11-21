/*
 * The vibration motor of the POCO X3 Pro (vayu, SM8150-AC): the haptics block of the PM8150B
 * (SPMI slave 3, registers 0xc000), bsd/dev/vayu_haptic.c.
 *
 * Three steps, one file. Step 1 is the look that ran on the phone (run 164539) and is unchanged:
 * read-only, no write channel, no write command. Step 2 sets the block up the way Qualcomm's
 * driver does for this LRA and plays two constant vibrations, so the tester can feel them. Step 3
 * keeps the block set up and serves iOS's actuator: /dev/hakios-haptic takes the int16 mono
 * samples the audio plug-in writes (userland/src/audio_plugin.c opens that path once, on its first
 * actuator write) and turns them into taps and vibrations.
 *
 * What the phone says (run 164539, step 1 of this file, on the hardware): PMIC arbiter version 5,
 * channel 307 is id 0x3c0 (slave 3, peripheral 0xc0), owner system 0, which is us. The block says
 * type 0x15, subtype 0x09, revision 00 02 00 03. Its registers at that moment: STATUS1 0x00,
 * EN_CTL1 0x00, EN_CTL2 0x03, EN_CTL3 0xff, AUTO_RES_CTRL 0xdc, CFG1 0x00, CFG2 0x01, SEL 0x00,
 * AUTO_RES_CFG 0x90, SLEW_CFG 0x01, VMAX_CFG 0x22, ILIM_CFG 0x01, SC_DEB_CFG 0x01, RATE_CFG1/2
 * 0x03e8 (5000 us), ZX_CFG 0x03, BRAKE 0, WF_S1..S8 0, PLAY 0, PERPH_RESET_CTL3 0x07. Those are
 * Android's leftovers; the writes below replace the ones the vendor's probe sets.
 *
 * The vendor, qti-haptics.c ("l." is a line there), for this phone's device tree: actuator LRA,
 * lra_shape "sine" = RES_SIG_SINE = 0 and auto-resonance mode "qwd" = AUTO_RES_MODE_QWD = 1
 * (pm8150b.dtsi:497-501), vmax 2800 mV and play rate 5102 us (vayu-sm8150.dtsi:1829-1831), and no
 * vdd-supply. The register map is l.78-175. Writes to addresses above 0xD0 need SEC_ACCESS
 * (l.268-271); WE NEVER WRITE ANY OF THOSE -- the allow-list below stops at 0x70.
 *
 * qti_haptics_hw_init() (l.1100-1200), for an LRA, in its order:
 *   1. CFG1 (0x4c) = 0x00: ACT_LRA (l.1107-1113)
 *   2. ILIM_CFG (0x52) = 0x02: HAP_ILIM_DEFAULT_SEL = HAP_ILIM_SEL_1000MA (l.1115-1122, 139-140)
 *   3. EN_CTL3 (0x4a) = 0xfd: HBRIDGE|PWM_SIGNAL|ILIM|ILIM_CC|AUTO_RES_RBIAS|DAC|PWM_CTL, with
 *      ZX_HYST off (l.1124-1133, 89-97)
 *   4. ZX_CFG (0x5a) bits 2:0 <- 3: ZX_DET_DEB_80US (l.1135-1143, 151-156)
 *   5. RATE_CFG1/2 (0x54/0x55) = 5102 / 5 = 1020 = 0x3fc: 0xfc, 0x03 (l.1149, 544-559)
 *   6. CFG2 (0x4d) = 0x00: RES_SIG_SINE (l.1170-1176, 39-42)
 *   7. AUTO_RES_CFG (0x4f) masked 0x8f <- 0x88: QWD (bit 7) | CAL_EOP_EN (bit 3) | period 0
 *      (l.1178-1186, 122-128)
 *   8. AUTO_RES_CTRL (0x4b) = 0xec: AUTO_RES_EN|SEL_AUTO_RES_PERIOD|CNT_ERR_DELTA(2)|
 *      ERR_RECOVERY|EN_DLY(4) (l.1188-1197, 99-107)
 *
 * A constant vibration of 20 ms or more (qti_haptics_load_constant_waveform(), l.649-709, VMAX
 * branch, and qti_haptics_playback(), l.973-1020): the play rate as above; VMAX_CFG (0x51) masked
 * 0x3e <- (mv / 116) << 1 (l.509-523, 131-136); AUTO_RES_CTRL bit 7 set (l.600-614); SEL (0x4e)
 * masked 0x31 <- 0x00, the VMAX source (l.525-542, 113-120); EN_CTL1 (0x46) = 0x80; PLAY (0x70) =
 * 0x80. After the length PLAY = 0x00 (the stop timer, l.1202-1214), and EN_CTL1 = 0x00 one play
 * period plus 1 ms later (the disable timer, l.1216-1228; HAP_DISABLE_DELAY_USEC 1000 and
 * play_rate_us, l.75 and 1043-1050). We never use the waveform buffer (WF_S1..S8) or the audio
 * source, so qti_haptics_clear_settings() (l.616-647), which deals with that buffer and the
 * brake, is not needed: PLAY = 0 in safe_off() ends a VMAX-source vibration.
 *
 * Step 2: vayu_haptic=2 does step 1 and hw_init, then two constant vibrations of vayu_haptic_ms
 * (default 300 ms, 20..1000) at vayu_haptic_mv (default 2800 mV, 1160..2800 and never more than
 * the vendor's 2800), two seconds apart, polling STATUS1 every 10 ms.
 *
 * Step 3: vayu_haptic=3 does step 1 and hw_init (no test pulses), then serves iOS. The node
 * /dev/hakios-haptic is made first thing in the thread, before any hardware init, so userland
 * finds it early. A tap is a constant vibration of vayu_haptic_tap_ms (default 35, 20..60) at the
 * peak's strength, stopped with PLAY = 0; the module goes off 6 ms after the last play (one
 * 5.1 ms period plus 1 ms). If iOS keeps writing past TAP_US, the sound is a vibration: PLAY
 * stays on until iOS stops or vayu_haptic_max_ms (default 1500, 100..3000), then the motor rests
 * for as long as it played. The strength follows iOS's peak: 2800 mV from a third of full scale
 * up, down to 5/8 of 2800 for the softest (hakios.haptic.c's gain_for() shape). While a vibration
 * plays, VMAX is rewritten only when its code changes by two steps or more, at most every 20 ms.
 * Every 50 ms while playing STATUS1 is read for SC_DET.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   vayu_haptic=1   step 1 only: a look, reading only
 *   vayu_haptic=2   step 1, the vendor's setup, and two test pulses
 *   vayu_haptic=3   step 1, the vendor's setup, and iOS's motor (/dev/hakios-haptic)
 *   vayu_haptic_ms=N        step 2's pulse, default 300, 20..1000
 *   vayu_haptic_mv=N        step 2's strength, default 2800, 1160..2800
 *   vayu_haptic_tap_ms=N    step 3's tap, default 35, 20..60 (25 until run 132828)
 *   vayu_haptic_min_mv=N    step 3's weakest play, default 2500, 1160..2800 (5/8 of 2800 until run 132828)
 *   vayu_haptic_max_ms=N    step 3's longest vibration, default 1500, 100..3000
 *
 * Limits, whatever asks: never above 2800 mV; only the thirteen registers in writable_regs[];
 * every one of those writes is read back (PLAY and EN_CTL1 are logged but may read differently,
 * being state-like), and a failed or different read stops all writing for the run except
 * safe_off(); SC_DET in STATUS1 (bit 3) stops the motor and gives up; no pulse longer than
 * 1000 ms in step 2, no play longer than 3000 ms in step 3. The write window is the haptics
 * channel's alone (307 on this phone), mapped only after find_channel() proved the channel is the
 * block's and its owner is system 0; before every write the arbiter's channel entry is looked at
 * again and must still be 0x3c0. Transfers are one at a time with the buttons' driver
 * (hakios_spmi_lock(): two at once, and it drops one); the arbiter drops one now and then, and a
 * dropped write is retried up to three times.
 *
 * The arbiter section (addresses, offsets, lock(), wait_done(), spmi_read()) is copied from
 * bsd/dev/hakios.torch.c, which explains it in full. All of it runs in a kernel thread, so a
 * slow arbiter never holds up boot. At the run's end, and on a panic's way out,
 * vayu_haptic_quiesce() (osfmk/arm64/loader_return.c) puts the motor off; leaving makes it take
 * no lock and wait no longer than the arbiter's 2000 us per transfer.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/conf.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vayu_haptic.h>

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

static uint32_t level;
static bool broken, leaving;
static const char *broken_why;                  /* give_up()'s reason, for the lines that survive (step 3) */
static bool wrote;                      /* (anything sent through the write window yet) */
static bool module_on;                  /* (EN_CTL1 bit 7 is set) */
static uint8_t last_read;               /* (what the last register read gave, for the step logs) */

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

/* (on the panic's way out nothing waits in a scheduler, only spins) */
static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* ---- the PMIC arbiter (version 5), from bsd/dev/hakios.torch.c -------------------------------- */

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
	vm_offset_t     write;          /* its write channel, mapped (only the haptics block's) */
};

static vm_offset_t arb_core, arb_cnfg, arb_observer;

static struct peripheral haptics = {
	.name = "the PM8150B's haptics block",
	.id = 0x3c0,                    /* slave 3, peripheral 0xc0: pm8150b.dtsi:491-496; channel 307, run 164539 */
};

/* (the arbiter one transfer at a time with the buttons' driver; at the run's end it may be held) */
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

/* ---- the PM8150B's haptics block (qti-haptics.c:78-175) ---------------------------------------- */

#define HAPTIC_REVISION1        0x00
#define HAPTIC_REVISION2        0x01
#define HAPTIC_REVISION3        0x02
#define HAPTIC_REVISION4        0x03
#define HAPTIC_TYPE             0x04
#define HAPTIC_SUBTYPE          0x05
#define HAPTIC_STATUS1          0x0a            /* [3] SC_DET, [1] BUSY */
#define HAPTIC_EN_CTL1          0x46            /* [7] HAP_EN */
#define HAPTIC_EN_CTL2          0x48
#define HAPTIC_EN_CTL3          0x4a
#define HAPTIC_AUTO_RES_CTRL    0x4b
#define HAPTIC_CFG1             0x4c
#define HAPTIC_CFG2             0x4d
#define HAPTIC_SEL              0x4e
#define HAPTIC_AUTO_RES_CFG     0x4f
#define HAPTIC_SLEW_CFG         0x50
#define HAPTIC_VMAX_CFG         0x51            /* [5:1] x 116 mV (l.131-136) */
#define HAPTIC_ILIM_CFG         0x52
#define HAPTIC_SC_DEB_CFG       0x53
#define HAPTIC_RATE_CFG1        0x54            /* the play rate's low 8 bits */
#define HAPTIC_RATE_CFG2        0x55            /* the play rate's high bits (5 us per step, l.68-70) */
#define HAPTIC_ZX_CFG           0x5a
#define HAPTIC_BRAKE            0x5c
#define HAPTIC_WF_REPEAT        0x5e
#define HAPTIC_WF_S1            0x60
#define HAPTIC_WF_S2            0x61
#define HAPTIC_WF_S3            0x62
#define HAPTIC_WF_S4            0x63
#define HAPTIC_WF_S5            0x64
#define HAPTIC_WF_S6            0x65
#define HAPTIC_WF_S7            0x66
#define HAPTIC_WF_S8            0x67
#define HAPTIC_PLAY             0x70            /* [7] PLAY */
#define HAPTIC_PERPH_RESET_CTL3 0xda

#define HAPTIC_SC_DET           (1u << 3)       /* l.79 */
#define HAPTIC_BUSY             (1u << 1)       /* l.80 */
#define HAPTIC_HAP_EN           (1u << 7)       /* l.83 */
#define HAPTIC_PLAY_BIT         (1u << 7)       /* l.174 */
#define HAPTIC_VMAX_MASK        0x3e            /* l.134: GENMASK(5, 1) */
#define HAPTIC_SEL_MASK         0x31            /* l.114-116: source [5:4] and trigger [0] */
#define HAPTIC_AUTO_RES_MASK    0x8f            /* l.123-127: mode [7], EOP [3], period [2:0] */
#define HAPTIC_AUTO_RES_EN      (1u << 7)       /* l.100: HAP_AUTO_RES_EN_BIT */
#define HAPTIC_ZX_MASK          0x07            /* l.152 */

#define HAPTIC_VMAX_MV_PER_STEP 116u            /* qti-haptics.c:131-136 */
#define HAPTIC_VMAX_MV_MAX      2800u           /* vayu-sm8150.dtsi:1829-1831; never above it */
#define HAPTIC_VMAX_MV_MIN      1160u           /* 10 steps */
#define HAPTIC_VMAX_2800        0x30u           /* (2800 / 116) << 1: pm8150b.dtsi:497-501, vayu-sm8150.dtsi:1829-1831 */
#define HAPTIC_PLAY_RATE_US_LSB 5u              /* qti-haptics.c:68-70 */
#define HAPTIC_PLAY_RATE_5102   0x3fcu          /* 5102 / 5 = 1020: vayu-sm8150.dtsi:1829-1831 */
#define HAPTIC_RATE_5102_LO     0xfcu           /* 1020 = 0x3fc: low byte, then the high 4 bits */
#define HAPTIC_RATE_5102_HI     0x03u
#define HAPTIC_MODULE_OFF_MS    6u              /* 5102 us + 1000 us (l.75, 1043-1050), in whole ms */
#define HAPTIC_PLAY_WAIT_MS     10u             /* step 2's STATUS1 poll */

struct reg_name {
	const char *    name;
	uint8_t         reg;
};

static const struct reg_name regs[] = {
	{ "REVISION1", HAPTIC_REVISION1 },
	{ "REVISION2", HAPTIC_REVISION2 },
	{ "REVISION3", HAPTIC_REVISION3 },
	{ "REVISION4", HAPTIC_REVISION4 },
	{ "TYPE", HAPTIC_TYPE },
	{ "SUBTYPE", HAPTIC_SUBTYPE },
	{ "STATUS1", HAPTIC_STATUS1 },
	{ "EN_CTL1", HAPTIC_EN_CTL1 },
	{ "EN_CTL2", HAPTIC_EN_CTL2 },
	{ "EN_CTL3", HAPTIC_EN_CTL3 },
	{ "AUTO_RES_CTRL", HAPTIC_AUTO_RES_CTRL },
	{ "CFG1", HAPTIC_CFG1 },
	{ "CFG2", HAPTIC_CFG2 },
	{ "SEL", HAPTIC_SEL },
	{ "AUTO_RES_CFG", HAPTIC_AUTO_RES_CFG },
	{ "SLEW_CFG", HAPTIC_SLEW_CFG },
	{ "VMAX_CFG", HAPTIC_VMAX_CFG },
	{ "ILIM_CFG", HAPTIC_ILIM_CFG },
	{ "SC_DEB_CFG", HAPTIC_SC_DEB_CFG },
	{ "RATE_CFG1", HAPTIC_RATE_CFG1 },      /* (read with RATE_CFG2 by show_registers()) */
	{ "ZX_CFG", HAPTIC_ZX_CFG },
	{ "BRAKE", HAPTIC_BRAKE },
	{ "WF_REPEAT", HAPTIC_WF_REPEAT },
	{ "WF_S1", HAPTIC_WF_S1 },
	{ "WF_S2", HAPTIC_WF_S2 },
	{ "WF_S3", HAPTIC_WF_S3 },
	{ "WF_S4", HAPTIC_WF_S4 },
	{ "WF_S5", HAPTIC_WF_S5 },
	{ "WF_S6", HAPTIC_WF_S6 },
	{ "WF_S7", HAPTIC_WF_S7 },
	{ "WF_S8", HAPTIC_WF_S8 },
	{ "PLAY", HAPTIC_PLAY },
	{ "PERPH_RESET_CTL3", HAPTIC_PERPH_RESET_CTL3 },
};

/* One register, printed the way the torch driver prints the flash module's. */
static void
show_reg(uint8_t reg, const char *name, uint8_t value)
{
	uint32_t code;

	printf("vayu-haptic: %-18s 0x%02x = %#04x", name, reg, value);
	switch (reg) {
	case HAPTIC_STATUS1:
		printf("  (SC_DET %u, BUSY %u)", (unsigned)((value >> 3) & 1), (unsigned)((value >> 1) & 1));
		break;
	case HAPTIC_EN_CTL1:
		printf("  (HAP_EN %u)", (unsigned)((value >> 7) & 1));
		break;
	case HAPTIC_VMAX_CFG:
		code = (value & 0x3e) >> 1;
		printf("  (VMAX %u mV: bits 5:1 = %u; the vendor's 2800 mV is code %#04x)",
		    (unsigned)(code * HAPTIC_VMAX_MV_PER_STEP), (unsigned)code, HAPTIC_VMAX_2800);
		break;
	case HAPTIC_PLAY:
		printf("  (PLAY %u)", (unsigned)((value >> 7) & 1));
		break;
	}
	printf("\n");
}

/* RATE_CFG1 and RATE_CFG2 together are the play rate in 5 us steps (qti-haptics.c:68-70). */
static void
show_rate(uint8_t cfg1, uint8_t cfg2)
{
	uint32_t code = ((uint32_t)cfg2 << 8) | cfg1;
	uint32_t us = code * HAPTIC_PLAY_RATE_US_LSB;

	printf("vayu-haptic: %-18s 0x%02x = %#04x\n", "RATE_CFG1", HAPTIC_RATE_CFG1, cfg1);
	printf("vayu-haptic: %-18s 0x%02x = %#04x  (rate %#x = %u us at 5 us steps; qcom,play-rate-us 5102 is code %#x)\n",
	    "RATE_CFG2", HAPTIC_RATE_CFG2, cfg2, code, (unsigned)us, HAPTIC_PLAY_RATE_5102);
}

static void
show_registers(void)
{
	const char *problem;
	uint8_t value, pair[2];

	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		if (regs[i].reg == HAPTIC_RATE_CFG1) {
			problem = spmi_read(&haptics, HAPTIC_RATE_CFG1, pair, 2);
			if (problem != NULL) {
				printf("vayu-haptic: reading RATE_CFG1 and RATE_CFG2: %s\n", problem);
			} else {
				show_rate(pair[0], pair[1]);
			}
			continue;
		}
		problem = spmi_read(&haptics, regs[i].reg, &value, 1);
		if (problem != NULL) {
			printf("vayu-haptic: reading %s at 0x%02x: %s\n", regs[i].name, regs[i].reg, problem);
			continue;
		}
		show_reg(regs[i].reg, regs[i].name, value);
	}
}

/* ---- writing: one register at a time, from a short list, read back (as hakios.torch.c) -------- */

/*
 * The only registers this file may ever write. Everything else -- the revision and status
 * registers, the waveform buffer (WF_S1..S8 0x60-0x67), SLEW_CFG, the brake and the reset -- is
 * not on it. No address above 0xD0 is here because those need SEC_ACCESS (qti-haptics.c:268-271),
 * and we never write any of them.
 */
static const uint8_t writable_regs[] = {
	HAPTIC_EN_CTL1, HAPTIC_EN_CTL3, HAPTIC_AUTO_RES_CTRL, HAPTIC_CFG1, HAPTIC_CFG2, HAPTIC_SEL,
	HAPTIC_AUTO_RES_CFG, HAPTIC_VMAX_CFG, HAPTIC_ILIM_CFG, HAPTIC_RATE_CFG1, HAPTIC_RATE_CFG2,
	HAPTIC_ZX_CFG, HAPTIC_PLAY,
};

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		broken_why = why;
		printf("vayu-haptic: %s: no more writing to the haptics block for the rest of the run\n", why);
	}
}

/*
 * One write command: the first try and, if the arbiter drops it, up to three more; NULL, or what
 * went wrong. The arbiter's channel entry is looked at first: it must still say this block (0x3c0).
 */
static const char *
write_retry(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;

	if (haptics.write == 0 || !haptics.writable) {
		return "no write channel of ours";
	}
	for (int tries = 0; tries < 4; tries++) {
		/* (the channel is still the block's: its entry in the arbiter's table, before each write) */
		if (((rd(arb_core + ARB_CHANNEL(haptics.channel)) >> 8) & 0xfff) != haptics.id) {
			return "the arbiter's channel is no longer the haptics block's";
		}
		wrote = true;
		lock();
		wr(haptics.write + CHANNEL_WDATA0, value);
		wr(haptics.write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(haptics.write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;          /* (a dropped write has not been made: asked for again, then read back) */
		}
	}
	return problem;
}

static bool
get(uint8_t reg, uint8_t *value)
{
	const char *problem;

	*value = 0xff;
	problem = spmi_read(&haptics, reg, value, 1);
	if (problem == NULL) {
		last_read = *value;
		return true;
	}
	printf("vayu-haptic: reading %#04x of %s: %s\n", reg, haptics.name, problem);
	return false;
}

/*
 * One register of the block (and of the thirteen above only) written and read back: false, and no
 * more writing, if it cannot be or does not read back. PLAY and EN_CTL1 are the exception: they
 * may look different while the block runs, so their reads are said but do not stop anything.
 */
#define STATE_SAY_MAX           24      /* PLAY/EN_CTL1 readbacks said: the first so many (step 3 writes them per tap) */
static unsigned state_said;

static bool
put(uint8_t reg, uint8_t value)
{
	const char *problem;
	uint8_t back = 0xff;
	bool allowed = false;

	for (size_t i = 0; i < sizeof(writable_regs); i++) {
		allowed |= writable_regs[i] == reg;
	}
	if (broken || !allowed || !haptics.writable || haptics.write == 0) {
		give_up(allowed ? "a write that may not be made" : "a register that is not to be written");
		return false;
	}
	problem = write_retry(reg, value);
	if (problem != NULL || (problem = spmi_read(&haptics, reg, &back, 1)) != NULL) {
		printf("vayu-haptic: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	last_read = back;
	if (back != value) {
		if (reg == HAPTIC_PLAY || reg == HAPTIC_EN_CTL1) {
			if (state_said++ < STATE_SAY_MAX) {
				printf("vayu-haptic: %#04x <- %#04x, reads %#04x (state-like: not a give-up)\n", reg, value, back);
			}
			return true;
		}
		/*
		 * Configuration registers whose bits may not all exist on this PMIC revision: logged, not a give-up.
		 * Run 065229 (HAPTIC2): ILIM_CFG 0x52 reads 0x01 after reset and 0x00 after 0x02 is written -- only bit 0
		 * is there on rev 0.2.0.3; the vendor writes 0x02 with no readback (qti-haptics.c:1115-1122), so its chip
		 * ends up the same. Strict stay: CFG1 (the actuator type), VMAX_CFG (the strength), SEL (the source).
		 */
		if (reg == HAPTIC_ILIM_CFG || reg == HAPTIC_EN_CTL3 || reg == HAPTIC_ZX_CFG || reg == HAPTIC_RATE_CFG1 ||
		    reg == HAPTIC_RATE_CFG2 || reg == HAPTIC_CFG2 || reg == HAPTIC_AUTO_RES_CFG || reg == HAPTIC_AUTO_RES_CTRL) {
			printf("vayu-haptic: %#04x <- %#04x, reads %#04x (a configuration register: logged, not a give-up)\n", reg, value,
			    back);
			return true;
		}
		printf("vayu-haptic: %#04x <- %#04x, but it reads %#04x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	if ((reg == HAPTIC_PLAY || reg == HAPTIC_EN_CTL1) && state_said++ < STATE_SAY_MAX) {
		printf("vayu-haptic: %#04x <- %#04x, reads %#04x\n", reg, value, back);
	}
	return true;
}

/* only the bits of `mask' changed, as the driver's masked writes do */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!get(reg, &now)) {
		give_up("a read that failed");
		return false;
	}
	return (now & mask) == (value & mask) || put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

/*
 * PLAY = 0, then EN_CTL1 = 0: the two writes that stop the motor. Unlike put() this works when
 * the run is broken (a fault, a failed readback), and it does not read anything back -- there may
 * be no time, and this is the way out. Both writes still go to the block's own channel only.
 */
static void
safe_off(void)
{
	const char *problem;

	if (haptics.write == 0) {
		return;
	}
	problem = write_retry(HAPTIC_PLAY, 0x00);
	printf("vayu-haptic: safe off: PLAY 0x%02x <- 0x00%s%s\n", HAPTIC_PLAY,
	    problem == NULL ? " (done)" : ": ", problem == NULL ? "" : problem);
	problem = write_retry(HAPTIC_EN_CTL1, 0x00);
	printf("vayu-haptic: safe off: EN_CTL1 0x%02x <- 0x00%s%s\n", HAPTIC_EN_CTL1,
	    problem == NULL ? " (done)" : ": ", problem == NULL ? "" : problem);
	module_on = false;
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
		printf("vayu-haptic: channel %u is %s (%#x); owner system %u%s\n", n, p->name, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : ", not us");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == ARB_SYSTEM;
		}
	}
	if (!p->found) {
		printf("vayu-haptic: no channel of the arbiter is %s\n", p->name);
	}
}

/* ---- the vendor's setup for this LRA (qti-haptics.c:1100-1200) --------------------------------- */

/*
 * The write of one setting, with the read before and after that step 2's log asks for. `value'
 * is the vendor's value for the masked bits; the readback is the whole register, so bits outside
 * `mask' may remain -- AUTO_RES_CFG's bit 4, which Android leaves set, is the one that does.
 */
static bool
init_put(const char *name, uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t was = 0xff;
	bool ok;

	if (!get(reg, &was)) {
		give_up("a read that failed");
		return false;
	}
	last_read = 0xff;
	ok = put_bits(reg, mask, value);
	printf("vayu-haptic: 2: %-13s 0x%02x <- 0x%02x (was 0x%02x), reads 0x%02x\n", name, reg, value, was, last_read);
	return ok;
}

/*
 * qti_haptics_hw_init() for an LRA: the eight settings of the header comment, in the vendor's
 * order. The values are the vendor's; the readbacks are ours. RATE_CFG1 and RATE_CFG2 are one
 * setting (the two halves of the 10-bit rate) and qti-haptics.c writes them in one transfer.
 */
static bool
hw_init(void)
{
	if (!init_put("CFG1", HAPTIC_CFG1, 0xff, 0x00)) {             /* ACT_LRA */
		return false;
	}
	if (!init_put("ILIM_CFG", HAPTIC_ILIM_CFG, 0xff, 0x02)) {     /* HAP_ILIM_SEL_1000MA */
		return false;
	}
	if (!init_put("EN_CTL3", HAPTIC_EN_CTL3, 0xff, 0xfd)) {       /* all but ZX_HYST */
		return false;
	}
	if (!init_put("ZX_CFG", HAPTIC_ZX_CFG, HAPTIC_ZX_MASK, 0x03)) {        /* ZX_DET_DEB_80US */
		return false;
	}
	if (!init_put("RATE_CFG1", HAPTIC_RATE_CFG1, 0xff, HAPTIC_RATE_5102_LO)) {     /* 5102 us / 5 */
		return false;
	}
	if (!init_put("RATE_CFG2", HAPTIC_RATE_CFG2, 0xff, HAPTIC_RATE_5102_HI)) {
		return false;
	}
	if (!init_put("CFG2", HAPTIC_CFG2, 0xff, 0x00)) {             /* RES_SIG_SINE */
		return false;
	}
	if (!init_put("AUTO_RES_CFG", HAPTIC_AUTO_RES_CFG, HAPTIC_AUTO_RES_MASK, 0x88)) {      /* QWD, EOP */
		return false;
	}
	if (!init_put("AUTO_RES_CTRL", HAPTIC_AUTO_RES_CTRL, 0xff, 0xec)) {    /* auto-res on, as l.1188-1197 */
		return false;
	}
	return true;
}

/* ---- a constant vibration (qti-haptics.c:649-709 and 973-1020) --------------------------------- */

static uint8_t
vmax_code(uint32_t mv)
{
	return (uint8_t)((mv / HAPTIC_VMAX_MV_PER_STEP) << 1);        /* bits 5:1, l.509-523 */
}

/*
 * Start the VMAX-source vibration at `code': the play rate, the strength, auto-resonance on (an
 * LRA, l.600-614), the VMAX source (SEL, l.525-542), the module on and PLAY. The buffer source
 * and the audio source are never selected here.
 */
static bool
start_play(uint8_t code)
{
	if (!put(HAPTIC_RATE_CFG1, HAPTIC_RATE_5102_LO) || !put(HAPTIC_RATE_CFG2, HAPTIC_RATE_5102_HI)) {
		return false;
	}
	if (!put_bits(HAPTIC_VMAX_CFG, HAPTIC_VMAX_MASK, code)) {
		return false;
	}
	if (!put_bits(HAPTIC_AUTO_RES_CTRL, HAPTIC_AUTO_RES_EN, HAPTIC_AUTO_RES_EN)) {
		return false;
	}
	if (!put_bits(HAPTIC_SEL, HAPTIC_SEL_MASK, 0x00)) {           /* WF source VMAX, no trigger */
		return false;
	}
	if (!put(HAPTIC_EN_CTL1, HAPTIC_HAP_EN)) {
		return false;
	}
	module_on = true;
	return put(HAPTIC_PLAY, HAPTIC_PLAY_BIT);
}

static bool
stop_play(void)
{
	return put(HAPTIC_PLAY, 0x00);
}

static bool
module_off(void)
{
	if (!module_on) {
		return true;
	}
	if (!put(HAPTIC_EN_CTL1, 0x00)) {
		return false;
	}
	module_on = false;
	return true;
}

/*
 * STATUS1 (0x0a): OR into `seen', remember whether BUSY was ever set, and stop everything on
 * SC_DET -- the module's short-circuit detection (qti-haptics.c:78-80). true means the caller
 * must stop: a short circuit, or a read that failed.
 */
static bool
short_circuit(uint8_t *seen, bool *busy)
{
	uint8_t value = 0;

	if (!get(HAPTIC_STATUS1, &value)) {
		safe_off();
		give_up("a STATUS1 read that failed");
		return true;
	}
	if (seen != NULL) {
		*seen |= value;
	}
	if (busy != NULL) {
		*busy |= (value & HAPTIC_BUSY) != 0;
	}
	if ((value & HAPTIC_SC_DET) != 0) {
		safe_off();
		printf("vayu-haptic: SHORT CIRCUIT DETECTED (STATUS1 0x%02x): the motor is off\n", value);
		give_up("a short circuit");
		return true;
	}
	return false;
}

/* ---- step 2: two pulses the tester can feel ----------------------------------------------------- */

static bool
pulse(uint32_t ms, uint32_t mv)
{
	uint8_t code = vmax_code(mv), seen = 0;
	uint8_t final_en = 0xff, final_play = 0xff, final_status = 0xff;
	bool busy = false;
	uint64_t began;

	if (!start_play(code)) {
		return false;
	}
	printf("vayu-haptic: 2: PLAYING %u ms at %u mV (VMAX 0x%02x)\n", (unsigned)ms, (unsigned)mv, code);
	began = now_ms();
	for (;;) {
		if (short_circuit(&seen, &busy)) {
			return false;
		}
		if (now_ms() - began >= ms) {
			break;
		}
		wait_ms(HAPTIC_PLAY_WAIT_MS);
	}
	if (!stop_play()) {
		return false;
	}
	wait_ms(HAPTIC_MODULE_OFF_MS);
	if (!module_off()) {
		return false;
	}
	(void)get(HAPTIC_EN_CTL1, &final_en);
	(void)get(HAPTIC_PLAY, &final_play);
	if (get(HAPTIC_STATUS1, &final_status)) {
		seen |= final_status;
	}
	printf("vayu-haptic: 2: STATUS1 seen 0x%02x (OR of every read; BUSY %s)\n", seen, busy ? "was 1" : "never 1");
	printf("vayu-haptic: 2: stopped, module off (EN_CTL1 0x%02x, PLAY 0x%02x, STATUS1 0x%02x)\n",
	    final_en, final_play, final_status);
	return true;
}

static void
step2(void)
{
	uint32_t ms = 300, mv = HAPTIC_VMAX_MV_MAX;

	PE_parse_boot_argn("vayu_haptic_ms", &ms, sizeof(ms));
	ms = ms < 20 ? 20 : ms > 1000 ? 1000 : ms;
	PE_parse_boot_argn("vayu_haptic_mv", &mv, sizeof(mv));
	mv = mv < HAPTIC_VMAX_MV_MIN ? HAPTIC_VMAX_MV_MIN : mv > HAPTIC_VMAX_MV_MAX ? HAPTIC_VMAX_MV_MAX : mv;
	printf("vayu-haptic: step 2: two pulses of %u ms at %u mV (VMAX 0x%02x), two seconds apart\n",
	    (unsigned)ms, (unsigned)mv, vmax_code(mv));
	for (unsigned n = 0; n < 2 && !broken; n++) {
		if (n != 0) {
			wait_ms(2000);
		}
		if (!pulse(ms, mv)) {
			safe_off();
			break;
		}
	}
	printf("vayu-haptic: step 2 done\n");
}

/* ---- step 3: the motor for iOS (/dev/hakios-haptic) ------------------------------------------- */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define TAP_US                  60000           /* longer than this, and it is a vibration, not a tap */
#define SETTLE_US               8000            /* how long iOS's waveform is listened to before a tap */
#define FOLLOW_US               20000           /* how often a vibration's strength follows iOS's */
#define STATUS_LOOK_US          50000           /* how often STATUS1's SC_DET is looked at while playing */
#define STOP_TO_OFF_US          (HAPTIC_MODULE_OFF_MS * 1000ull)
#define TOLD_EVENTS             24              /* the first so many taps and vibrations are each said */

static volatile uint64_t heard_us;              /* when the last write that was not silence came */
static volatile uint64_t quiet_us;              /* when the last write that was silence came */
static volatile uint64_t onset_us;              /* when iOS's last waveform began: the first write not silence after a pause */
static volatile uint32_t heard_peak;            /* the largest sample of that waveform, or since the service last looked */
static uint64_t written_frames;
static unsigned taps, loops, capped;            /* what has been played, for the run's end */
/*
 * Whether serve() is running: iOS's samples are taken by the node (made first thing) whether or not the
 * motor is served. Run 022501: 1.46 M frames written, peaks up to 5451, "0 taps and 0 vibrations" -- and
 * the lines saying why were lost off the front of the log. So every write line and the run's summary say it.
 */
static volatile bool serving, served;  /* serving now; ever served (both set in serve()) */
static uint64_t loud_writes, onsets;            /* writes louder than SILENT_BELOW; waveforms begun (after a pause) */
static uint32_t tap_ms = 35, max_ms = 1500;    /* (tap 25 ms until run 132828: too short to feel) */
static uint32_t min_mv = 2500;                  /* vayu_haptic_min_mv: the weakest a tap or vibration is played */

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		{
			/* that iOS's writes arrive, while the run lasts: the first, then a line at most every 10 s with any */
			static uint64_t said_us, said_frames;
			uint64_t t = now_us();

			if (said_frames == 0 || (t - said_us >= 10000000 && written_frames != said_frames)) {
				printf("vayu-haptic: iOS has written %llu frames to /dev/hakios-haptic (this write's peak %u; %llu loud writes, "
				    "%llu waveforms, %u taps, %u vibrations); the motor %s%s%s\n",
				    (unsigned long long)written_frames, peak, (unsigned long long)loud_writes, (unsigned long long)onsets, taps,
				    loops, serving ? "is served" : "IS NOT SERVED",
				    broken ? ": given up: " : "", broken ? (broken_why != NULL ? broken_why : "?") : "");
				said_us = t;
				said_frames = written_frames;
			}
		}
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			/*
			 * A new waveform only after a real pause (ACTIVE_US with nothing loud). Before, any silent chunk
			 * since the last loud one began a new one: an actuator stream whose loud and silent 256-frame
			 * chunks alternate moved the onset with every chunk, SETTLE_US was never reached, and run 022501
			 * played nothing for 1.46 M frames with peaks up to 5451 (HYPOTHESIS; the counters below show it).
			 */
			loud_writes++;
			if (heard_us == 0 || now - heard_us >= ACTIVE_US) {
				onset_us = now;
				heard_peak = 0;
				onsets++;
			}
			heard_us = now;
			if (peak > heard_peak) {
				heard_peak = peak;
			}
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* The node iOS's plug-in opens, made before any hardware init so userland finds it early. */
static bool
make_node(void)
{
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		printf("vayu-haptic: /dev/hakios-haptic cannot be made\n");
		return false;
	}
	printf("vayu-haptic: /dev/hakios-haptic (0666) is made; iOS's actuator writes go there\n");
	return true;
}

/* How strong to play what iOS plays (hakios.haptic.c's gain_for(), for an LRA's mV). */
static uint32_t
mv_for(uint32_t peak)
{
	/* the floor: vayu_haptic_min_mv, default 2500 (test run 132828: a tap at 1863 mV for 25 ms was not felt) */
	const uint32_t top = HAPTIC_VMAX_MV_MAX, low = min_mv, third = 32767 / 3;

	return peak >= third ? top : low + (top - low) * peak / third;
}

static void
summary(void)
{
	printf("vayu-haptic: step 3: %u taps and %u vibrations played (%u cut at %u ms), %llu frames written by iOS; the motor was "
	    "%s%s%s\n", taps, loops, capped, (unsigned)max_ms, (unsigned long long)written_frames, served ? "served" : "NEVER SERVED",
	    broken ? "; given up: " : "", broken ? (broken_why != NULL ? broken_why : "?") : "");
}

/*
 * iOS's samples in, the actuator out. A waveform that begins after a pause is a tap; one that is
 * still being written past TAP_US is a vibration, which plays until iOS stops or max_ms. Taps and
 * vibrations are constant vibrations of the VMAX source; the strength follows the peak.
 */
static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, last_look = 0, tap_onset = 0, followed = 0, off_at = 0;
	uint8_t playing_code = 0;
	bool playing = false;
	unsigned follows = 0;                   /* how often the vibration playing now has changed strength */

	PE_parse_boot_argn("vayu_haptic_tap_ms", &tap_ms, sizeof(tap_ms));
	PE_parse_boot_argn("vayu_haptic_min_mv", &min_mv, sizeof(min_mv));
	min_mv = min_mv < HAPTIC_VMAX_MV_MIN ? HAPTIC_VMAX_MV_MIN : min_mv > HAPTIC_VMAX_MV_MAX ? HAPTIC_VMAX_MV_MAX : min_mv;
	tap_ms = tap_ms < 20 ? 20 : tap_ms > 60 ? 60 : tap_ms;
	PE_parse_boot_argn("vayu_haptic_max_ms", &max_ms, sizeof(max_ms));
	max_ms = max_ms < 100 ? 100 : max_ms > 3000 ? 3000 : max_ms;
	serving = served = true;
	printf("vayu-haptic: step 3: the motor is iOS's from here on: taps of %u ms, vibrations of %u ms at most, "
	    "never above %u mV\n", (unsigned)tap_ms, (unsigned)max_ms, HAPTIC_VMAX_MV_MAX);
	while (!broken && !leaving) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;
		uint32_t want_mv = mv_for(peak);
		uint8_t want_code = vmax_code(want_mv);

		switch (state) {
		case IDLE:
		case TAP:
			/*
			 * A waveform of iOS's that has begun (after a pause: a tap of a series is a tap of
			 * its own) is a tap -- once its first milliseconds are heard: a waveform begins
			 * softly, and they say how loud it is; one that goes on without a pause, a vibration.
			 */
			if (active && onset_us != tap_onset && now - onset_us >= SETTLE_US) {
				if (playing && !stop_play()) {
					break;
				}
				playing = false;
				if (!start_play(want_code)) {
					break;
				}
				state = TAP;
				since = now;
				tap_onset = onset_us;
				playing = true;
				playing_code = want_code;
				taps++;
				if (taps + loops <= TOLD_EVENTS) {
					printf("vayu-haptic: tap %u: peak %u -> %u mV (VMAX 0x%02x)\n",
					    taps, peak, (unsigned)want_mv, want_code);
				}
				break;
			}
			if (state == IDLE) {
				break;
			}
			/* the tap's tap_ms of vibration are over: PLAY 0, the module goes off 6 ms later */
			if (playing && now - since >= (uint64_t)tap_ms * 1000) {
				if (!stop_play()) {
					break;
				}
				playing = false;
				off_at = now + STOP_TO_OFF_US;
			}
			/* still loud TAP_US after it began (silent chunks in between do not end it; a pause of ACTIVE_US does) */
			if (active && onset_us == tap_onset && now - since >= TAP_US) {
				/* it was not a tap: it goes on. PLAY on until iOS stops or max_ms */
				if (!start_play(want_code)) {
					break;
				}
				state = LOOP;
				since = followed = now;
				playing = true;
				playing_code = want_code;
				follows = 0;
				loops++;
				heard_peak = 0;
				if (taps + loops <= TOLD_EVENTS) {
					printf("vayu-haptic: vibration %u: peak %u -> %u mV (VMAX 0x%02x, %u ms at most)\n",
					    loops, peak, (unsigned)want_mv, want_code, (unsigned)max_ms);
				}
			} else if (!active && now - since > TAP_US) {
				state = IDLE;
			}
			break;
		case LOOP:
			/* as strong as iOS's waveform is now (2800 mV at full scale, never more) */
			if (active && now - followed >= FOLLOW_US) {
				followed = now;
				heard_peak = 0;
				if (want_code != 0 && (want_code >= playing_code + 2 || playing_code >= want_code + 2)) {
					if (!put_bits(HAPTIC_VMAX_CFG, HAPTIC_VMAX_MASK, want_code)) {
						break;
					}
					playing_code = want_code;
					follows++;
				}
			}
			if (!active || now - since >= (uint64_t)max_ms * 1000) {
				bool cut = now - since >= (uint64_t)max_ms * 1000;
				uint64_t played = now - since;

				if (cut) {
					capped++;
				}
				if (!stop_play()) {
					break;
				}
				playing = false;
				off_at = now + STOP_TO_OFF_US;
				rest_until = now + played;      /* rest as long as it played */
				state = REST;
				if (taps + loops <= TOLD_EVENTS + 1) {
					printf("vayu-haptic: vibration %u: %s after %llu ms (its strength changed %u times, "
					    "last VMAX 0x%02x); the motor is off\n", loops,
					    cut ? "CUT at the limit" : "iOS stopped, and so does the motor",
					    (unsigned long long)(played / 1000), follows, playing_code);
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if (module_on && !playing && now >= off_at && off_at != 0) {
			if (!module_off()) {
				break;
			}
		}
		if (((state == TAP && playing) || state == LOOP) && now - last_look >= STATUS_LOOK_US) {
			last_look = now;
			if (short_circuit(NULL, NULL)) {
				break;
			}
		}
		wait_ms(4);
	}
	if (module_on) {
		safe_off();
	}
	summary();
}

/* ---- step 1: the look (unchanged; this is what ran as run 164539) ------------------------------ */

static bool block_matches;              /* type 0x15, subtype 0x09, as the phone's block says */

static bool
step1(void)
{
	uint32_t version, channels;
	uint8_t identity[2] = { 0, 0 }, revision[4] = { 0, 0, 0, 0 };
	const char *problem;
	bool identity_ok, revision_ok, readable = false;

	printf("vayu-haptic: step 1: a read-only look at the PM8150B haptics block\n");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	printf("vayu-haptic: PMIC arbiter version %#x\n", version);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("vayu-haptic: the arbiter is not version 5: nothing done\n");
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	find_channel(&haptics, channels);
	if (!haptics.found) {
		printf("vayu-haptic: the haptics block has no channel: nothing read\n");
		goto out;
	}
	problem = spmi_read(&haptics, HAPTIC_TYPE, identity, 2);
	identity_ok = problem == NULL;
	if (!identity_ok) {
		printf("vayu-haptic: reading type and subtype: %s\n", problem);
	}
	problem = spmi_read(&haptics, HAPTIC_REVISION1, revision, 4);
	revision_ok = problem == NULL;
	if (!revision_ok) {
		printf("vayu-haptic: reading the revision: %s\n", problem);
	}
	if (identity_ok && revision_ok) {
		printf("vayu-haptic: PM8150B haptics at slave 3 0xc000: type %#x subtype %#x rev %02x %02x %02x %02x\n",
		    identity[0], identity[1], revision[0], revision[1], revision[2], revision[3]);
		block_matches = identity[0] == 0x15 && identity[1] == 0x09;
	} else {
		printf("vayu-haptic: PM8150B haptics at slave 3 0xc000: type, subtype or revision not all readable\n");
	}
	show_registers();
	readable = true;
out:
	printf("vayu-haptic: step 1 done, nothing written\n");
	return readable;
}

/* ---- the start --------------------------------------------------------------------------------- */

static void
vayu_haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (level == 3 && !make_node()) {
		goto out;
	}
	if (!step1() || level < 2) {
		goto out;
	}
	if (!block_matches) {
		printf("vayu-haptic: the block is not the PM8150B haptics block (type 0x15 subtype 0x09): nothing written\n");
		goto out;
	}
	if (!haptics.writable) {
		printf("vayu-haptic: the channel is not ours to write through: nothing written\n");
		goto out;
	}
	haptics.write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * haptics.channel, 0x10000);
	if (haptics.write == 0) {
		printf("vayu-haptic: the write window of channel %u cannot be mapped: nothing written\n", haptics.channel);
		goto out;
	}
	printf("vayu-haptic: writing through channel %u's own 0x10000 window (%#lx)\n", haptics.channel,
	    (unsigned long)(ARB_CHNLS_PHYS + 0x10000UL * haptics.channel));
	if (!hw_init()) {
		goto out;
	}
	if (level == 2) {
		step2();
	} else {
		serve();
	}
out:
	if (level >= 2) {
		printf("vayu-haptic: done\n");
	}
	thread_terminate(current_thread());
}

void
vayu_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("vayu_haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 3) {
		printf("vayu-haptic: only steps 1 to 3 exist\n");
		level = 1;
	}
	if (kernel_thread_start(vayu_haptic_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("vayu-haptic: the probe's thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}

/*
 * At the run's end and on a panic's way out: called from osfmk/arm64/loader_return.c. If anything
 * was ever written, the motor is put off (PLAY = 0, then EN_CTL1 = 0) even when the run is broken,
 * and step 3 says what it played. `leaving' makes the arbiter transfers take no lock and never
 * wait more than the arbiter's 2000 us.
 */
void
vayu_haptic_quiesce(void)
{
	if (!wrote || haptics.write == 0) {
		return;
	}
	leaving = true;
	safe_off();
	if (level == 3) {
		summary();
	}
}
