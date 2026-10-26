/*
 * The speakers of the POCO X3 Pro (vayu, SM8150-AC). Every address below belongs to this phone,
 * and nothing here is touched unless boot-arg vayu_audio=N asks for it.
 *
 * Two TI smart amplifiers, set up over I2C and fed by the SoC's quaternary MI2S, which the audio
 * DSP drives (the bring-up notes has the evidence for every
 * number here):
 *
 *   0x4c  TAS2562, the bottom speaker (Android's right channel): SDZ GPIO 92, IRQ GPIO 91
 *   0x4d  TAS2564, the earpiece / top speaker (left channel):     SDZ GPIO 89, IRQ GPIO 90
 *
 * on the bus of QUP0 serial engine 7 (i2c@89c000, GPIO 98 data and 99 clock, function qup7),
 * on which the phone's device tree has nothing else enabled. As on redwood (hakios.speaker.c),
 * I2C is done by hand: the two pins are taken from the serial engine as GPIOs for one transaction
 * and given back as they were found, so no QUP clock is involved. MI2S: GPIO 137 bit clock, 138
 * word select, 139 data to the amplifiers, 140 their I/V feedback (function qua_mi2s, only while
 * the DSP runs the port). SDZ is the chips' active-low hardware shutdown.
 *
 *   vayu_audio=1   reads, nothing else: the pins above (function, pull, drive, level); the shared
 *                  memory -- has the audio DSP run since the phone was started (its SMP2P item
 *                  443 to us), and its last words (item 423); with vayu_audio_i2c=1 also the
 *                  amplifiers' page, book and chip ID registers (0x00, 0x7f, 0x7d), but only
 *                  if both SDZ pins already read high: the chips are not woken for it; with
 *                  vayu_audio_i2c=2 they are: SDZ driven high (the driver's tas256x_hw_reset():
 *                  out of hardware shutdown into software shutdown, no output stage), 20 ms, the
 *                  same reads, and the two pins put back as found but driven low (hardware
 *                  shutdown, also when a run before left them high). No amplifier register is
 *                  written in either case; every pin write and transaction leaves a timestamped
 *                  breadcrumb, and a transaction gives up after 5 ms. Both need
 *                  vayu_audio_xnu_tlmm=1: by this port's rule XNU does not write TLMM, and the pins
 *                  are the loader's (xnuloader/loader.c, vayu_audio_pins=1 raises SDZ, and with
 *                  vayu_audio_ids=1 reads the IDs itself); without it the SDZ pins are only read
 *
 *   vayu_audio=2   ... and the audio DSP (bsd/dev/vayu_pas.c): TrustZone asked whether it offers image 1
 *                  (and which calling convention), its image (adsp.mdt, adsp.b02-b13 from fw/ of the
 *                  small image or /fw of the root; the phone's modem partition image/,
 *                  the phone's modem firmware) checked, a DSP left running stopped,
 *                  TrustZone given the header (image 1) and the place (0x8be00000), the image
 *                  written and read back; GLINK's items 478/479 and our SMP2P item 429 made in the
 *                  shared memory (nobody makes them before us on vayu); "adsp: on" to the
 *                  always-on processor; authenticated and out of reset; "up" waited for
 *                  (vayu_audio_wait=S, default 10)
 *   vayu_audio=3   ... and GLINK with it (IPCRTR, apr_audio_svc), QRTR's hello and the services it
 *                  tells of (vayu_audio_listen=S, default 3), APR: the core service asked until
 *                  the audio services say they are up (vayu_audio_apr_wait=S, default 10), and
 *                  their versions
 *
 *   vayu_jack=1    (with vayu_audio>=3) ... and the 3.5 mm jack's codec (bsd/dev/vayu_jack.c):
 *                  the audio DSP's SLIMbus NGD brought up in register mode, the WCD934x's chip ID
 *                  read, its efuse and eight bring-up writes; no sound is made
 *   vayu_jack=2    (with vayu_audio=4 vayu_sound=2) ... and the headphones: the codec's MCLK and
 *                  init, then the tone through the AFE's SLIMbus port 0x400C (the codec's ports
 *                  18/19, channels 146/147); the amplifiers stay off
 *   vayu_jack=3    ... and the 3.5 mm jack's MBHC armed and polled (kern.vayu_jack / kern.vayu_jack_gen,
 *                  the codec stays up); the tone goes to the jack only when headphones are plugged
 *                  at stream start, otherwise to the speaker, and with the feeder (vayu_sound=5) the
 *                  route follows the jack while the stream runs. vayu_jack_gain=N (1/8 dB, 0 by
 *                  default, at most 6 dB) is the guard's gain while on the headphones.
 *
 *                  At the end of the steps the DSP is kept vayu_audio_secs=S (default 2), then
 *                  shut down, unless vayu_audio_keep=1. vayu_audio_cx=1: RPMh's cx.lvl raised to
 *                  384 while it boots, and left there (a shared rail is never voted down from
 *                  here); by default it is not touched. vayu_pause=MS before each first-time
 *                  write (default 2200: time for the loader's log).
 *
 *   vayu_audio=4   ... and sound (vayu_sound=1|2|3|6; see "step 4" below): the port, then a quiet tone
 *                  through the DSP, then the bottom speaker's amplifier at a low level
 *
 * TLMM tiles: NORTH 0x03900000 (GPIO 89-92), SOUTH 0x03d00000 (98, 99, 137-140), 0x1000 per pin
 * (drivers/pinctrl/qcom/pinctrl-sm8150.c; vayu_qup_spi.c of branch vayu-qup found the first
 * tile alias at 0x03100000 unsafe to write). SMEM: reserved-memory smem@86000000, 2 MB.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/hakios.audio.h>
#include <sys/vayu_audio.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_jack.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_log_putc(char c);

static uint32_t level, want_i2c, xnu_tlmm;
static uint32_t command_step = 4;       /* the step command()'s lines carry (6 while the microphones are up) */
static vm_offset_t north, south;
static bool sdz_driven;                 /* set once a step drives an SDZ pin; the quiesce only undoes what was done */

/* Every line to the loader's log as well: what is done to the amplifiers should be known even if the kernel dies. */
static int say(const char *format, ...) __printflike(1, 2);

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
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		printf("%s", line);
	}
	return length;
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

/*
 * A breadcrumb: the time since the thread began and what was just done, to the loader's log as
 * well (say()): if the next one never comes, the last one says where it stopped.
 */
static uint64_t crumb_since;

static int crumb(const char *format, ...) __printflike(1, 2);

static int
crumb(const char *format, ...)
{
	char line[240];
	uint64_t ns;
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	absolutetime_to_nanoseconds(mach_absolute_time() - crumb_since, &ns);
	return say("vayu-audio: +%llu.%03llu ms: %s\n", ns / 1000000, (ns / 1000) % 1000, line);
}

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_NORTH_PHYS         0x03900000UL
#define TLMM_SOUTH_PHYS         0x03d00000UL
#define TLMM_TILE_SIZE          0x100000UL
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 98      /* the serial engine's first pin is the data line, the second the clock */
#define PIN_SCL                 99
#define PIN_SDZ_LEFT            89      /* 0x4d */
#define PIN_SDZ_RIGHT           92      /* 0x4c */

static vm_offset_t
tile(unsigned pin)
{
	return (pin >= 89 && pin <= 92) ? north : south;
}

static vm_offset_t
gpio_cfg(unsigned pin)                  /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
{
	return tile(pin) + 0x1000UL * pin;
}

static vm_offset_t
gpio_io(unsigned pin)                   /* bit 0 in, bit 1 out */
{
	return tile(pin) + 0x1000UL * pin + 4;
}

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 98, "I2C data (qup7 = 1)" }, { 99, "I2C clock (qup7 = 1)" },
	{ 89, "SDZ of 0x4d, TAS2564" }, { 90, "IRQ of 0x4d" }, { 92, "SDZ of 0x4c, TAS2562" }, { 91, "IRQ of 0x4c" },
	{ 137, "MI2S bit clock (qua_mi2s = 1)" }, { 138, "MI2S word select" }, { 139, "MI2S data 0, to the amplifiers" },
	{ 140, "MI2S data 1, their feedback" },
};

static void
show_pins(void)
{
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(gpio_cfg(pins[i].pin)), io = rd(gpio_io(pins[i].pin));

		say("vayu-audio: 1: gpio%u (%s): cfg %#x: function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what,
		    cfg, (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

static bool
pin_high(unsigned pin)
{
	return rd(gpio_io(pin)) & 1;
}

/* ---- I2C by hand (hakios.speaker.c) ---------------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static uint32_t taken_cfg[2], taken_io[2];

/*
 * A line is pulled low (the pin drives 0) or let go (the pin is an input and the bus's pull-ups
 * raise it). Function 0 (GPIO); the pull bits are the ones found on the pin when it was taken
 * (the maintainers' exception, 2026-10-23: pull bits preserved); drive strength 2 mA.
 */
static void
line(unsigned pin, bool high)
{
	uint32_t pull = taken_cfg[pin == PIN_SDA ? 0 : 1] & 3;

	wr(gpio_io(pin), 0);
	wr(gpio_cfg(pin), pull | (high ? 0 : CFG_OUTPUT));
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static uint64_t i2c_deadline;           /* mach_absolute_time() past which a transaction gives up */
static bool i2c_late;
static bool i2c_quiet;                  /* while sound plays: a crumb only for a transaction that fails */

static bool
i2c_overdue(void)
{
	if (!i2c_late && mach_absolute_time() > i2c_deadline) {
		i2c_late = true;
	}
	return i2c_late;
}

static bool
i2c_bit(bool out)
{
	bool in;

	if (i2c_overdue()) {
		return true;            /* (reads as "not acknowledged": the transaction ends) */
	}

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !pin_high(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = pin_high(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

/* Returns true if the byte was acknowledged. */
static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

static void
bus_give_back(void)
{
	wr(gpio_io(PIN_SDA), taken_io[0] & 2);
	wr(gpio_io(PIN_SCL), taken_io[1] & 2);
	wr(gpio_cfg(PIN_SDA), taken_cfg[0]);
	wr(gpio_cfg(PIN_SCL), taken_cfg[1]);
}

/* The two pins taken from the serial engine for one transaction. False if the bus is not at rest: then nothing is driven. */
static bool
bus_take(void)
{
	taken_cfg[0] = rd(gpio_cfg(PIN_SDA));
	taken_cfg[1] = rd(gpio_cfg(PIN_SCL));
	taken_io[0] = rd(gpio_io(PIN_SDA));
	taken_io[1] = rd(gpio_io(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!pin_high(PIN_SDA) || !pin_high(PIN_SCL)) {
		bus_give_back();
		return false;
	}
	return true;
}

/* An 8-bit register of a TAS256x (book and page as they stand). False if nobody answers. */
static bool
register_read(uint8_t address, uint8_t reg, uint8_t *value)
{
	bool answered;
	uint64_t limit;

	nanoseconds_to_absolutetime(5 * NSEC_PER_MSEC, &limit);   /* a 3-byte read takes ~0.4 ms at 100 kHz */
	i2c_deadline = mach_absolute_time() + limit;
	i2c_late = false;
	if (!i2c_quiet) {
		crumb("i2c %#x reg %#x: taking gpio%u/%u (cfg %#x/%#x)", address, reg, PIN_SDA, PIN_SCL, rd(gpio_cfg(PIN_SDA)),
		    rd(gpio_cfg(PIN_SCL)));
	}
	if (!bus_take()) {
		say("vayu-audio: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    pin_high(PIN_SDA), PIN_SCL, pin_high(PIN_SCL));
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write((uint8_t)(address << 1 | 1));
	}
	if (answered) {
		*value = i2c_read(false);
	}
	i2c_stop();
	bus_give_back();
	if (!i2c_quiet || i2c_late || !answered) {
		crumb("i2c %#x reg %#x: %s; pins given back (cfg %#x/%#x)", address, reg, i2c_late ? "TIMED OUT (5 ms)" :
		    answered ? "answered" : "not acknowledged", rd(gpio_cfg(PIN_SDA)), rd(gpio_cfg(PIN_SCL)));
	}
	return answered && !i2c_late;
}

/*
 * Consecutive registers of a TAS256x written in one transaction (auto-increment): the way the
 * driver's bulk_write() sets a coefficient (the digital volume, 4 bytes), which the chip takes only
 * whole -- written a byte at a time, it keeps its old value (run 180852: page 2 0x0c read 0x40 again).
 */
static bool
register_write_burst(uint8_t address, uint8_t reg, const uint8_t *values, unsigned count)
{
	bool answered;
	uint64_t limit;

	nanoseconds_to_absolutetime(5 * NSEC_PER_MSEC, &limit);
	i2c_deadline = mach_absolute_time() + limit;
	i2c_late = false;
	if (!bus_take()) {
		say("vayu-audio: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not written\n", PIN_SDA,
		    pin_high(PIN_SDA), PIN_SCL, pin_high(PIN_SCL));
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg);
	for (unsigned i = 0; answered && i < count; i++) {
		answered = i2c_write(values[i]);
	}
	i2c_stop();
	bus_give_back();
	if (i2c_late || !answered) {
		crumb("i2c %#x reg %#x <- %u bytes: %s", address, reg, count, i2c_late ? "TIMED OUT (5 ms)" : "NOT ACKNOWLEDGED");
	}
	return answered && !i2c_late;
}

/* An 8-bit register of a TAS256x written (book and page as they stand). False if a byte is not acknowledged. */
static bool
register_write(uint8_t address, uint8_t reg, uint8_t value)
{
	bool answered;
	uint64_t limit;

	nanoseconds_to_absolutetime(5 * NSEC_PER_MSEC, &limit);
	i2c_deadline = mach_absolute_time() + limit;
	i2c_late = false;
	if (!bus_take()) {
		say("vayu-audio: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not written\n", PIN_SDA,
		    pin_high(PIN_SDA), PIN_SCL, pin_high(PIN_SCL));
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1)) && i2c_write(reg) && i2c_write(value);
	i2c_stop();
	bus_give_back();
	if (i2c_late || !answered) {
		crumb("i2c %#x reg %#x <- %#x: %s; pins given back (cfg %#x/%#x)", address, reg, value, i2c_late ? "TIMED OUT (5 ms)" :
		    "NOT ACKNOWLEDGED", rd(gpio_cfg(PIN_SDA)), rd(gpio_cfg(PIN_SCL)));
	}
	return answered && !i2c_late;
}

/* ---- the amplifiers --------------------------------------------------------------------------------- */

#define TAS_PAGE                0x00            /* book 0, page 0: the page select */
#define TAS_BOOK                0x7f            /* the book select */
#define TAS_CHIPID              0x7d            /* 0x10 or 0x20: TAS2562; 0x00: TAS2564 (tas256x-regmap.c) */

static const struct {
	uint8_t address;
	unsigned sdz;
	const char *what;
} amplifiers[] = {
	{ 0x4c, PIN_SDZ_RIGHT, "TAS2562, bottom speaker" },
	{ 0x4d, PIN_SDZ_LEFT, "TAS2564, earpiece" },
};

static const char *
chip_name(uint8_t id)
{
	switch (id) {
	case 0x10:
	case 0x20: return "TAS2562";
	case 0x00: return "TAS2564";
	}
	return "neither TAS2562 nor TAS2564";
}

static uint32_t sdz_found_cfg[2], sdz_found_io[2];

/* Both chips out of hardware shutdown: SDZ a GPIO output, high (function 0, 2 mA, no pull). */
static void
sdz_raise(void)
{
	for (unsigned i = 0; i < 2; i++) {
		unsigned pin = amplifiers[i].sdz;

		sdz_found_cfg[i] = rd(gpio_cfg(pin));
		sdz_found_io[i] = rd(gpio_io(pin));
		crumb("about to drive gpio%u (SDZ of %#x) high: cfg %#x io %#x -> cfg %#x (pull kept, as gpio_direction_output)",
		    pin, amplifiers[i].address, sdz_found_cfg[i], sdz_found_io[i], sdz_found_cfg[i] | CFG_OUTPUT);
		sdz_driven = true;
		wr(gpio_io(pin), 2);
		crumb("gpio%u io written: reads %#x", pin, rd(gpio_io(pin)));
		wr(gpio_cfg(pin), sdz_found_cfg[i] | CFG_OUTPUT);
		crumb("gpio%u cfg written: cfg %#x io %#x", pin, rd(gpio_cfg(pin)), rd(gpio_io(pin)));
	}
	spin_us(20000);                         /* (tas256x_hw_reset(): 20 ms before the first transfer) */
	crumb("20 ms later: gpio%u io %#x, gpio%u io %#x", PIN_SDZ_RIGHT, rd(gpio_io(PIN_SDZ_RIGHT)), PIN_SDZ_LEFT,
	    rd(gpio_io(PIN_SDZ_LEFT)));
}

/*
 * ... and put back: the configuration as found, the output value low -- whatever was found (run
 * 172738 left both driven high when a panic cut the step short), the chips end in hardware shutdown.
 */
static void
sdz_restore(void)
{
	for (unsigned i = 0; i < 2; i++) {
		unsigned pin = amplifiers[i].sdz;

		wr(gpio_io(pin), 0);
		wr(gpio_cfg(pin), sdz_found_cfg[i]);
		crumb("gpio%u put back: cfg %#x, reads %u", pin, rd(gpio_cfg(pin)), pin_high(pin));
	}
	sdz_driven = false;
}

static void
ask_amplifiers(void)
{
	bool woke = false;

	crumb("amplifiers: SDZ gpio%u cfg %#x io %#x, gpio%u cfg %#x io %#x", PIN_SDZ_RIGHT, rd(gpio_cfg(PIN_SDZ_RIGHT)),
	    rd(gpio_io(PIN_SDZ_RIGHT)), PIN_SDZ_LEFT, rd(gpio_cfg(PIN_SDZ_LEFT)), rd(gpio_io(PIN_SDZ_LEFT)));
	if (want_i2c >= 2 && (!pin_high(PIN_SDZ_LEFT) || !pin_high(PIN_SDZ_RIGHT))) {
		sdz_raise();
		woke = true;
	}
	for (unsigned i = 0; i < sizeof(amplifiers) / sizeof(amplifiers[0]); i++) {
		uint8_t page = 0xff, book = 0xff, id = 0xff;

		if (!pin_high(amplifiers[i].sdz)) {
			say("vayu-audio: 1: %#x (%s): SDZ gpio%u reads low, the chip is in shutdown: not asked\n", amplifiers[i].address,
			    amplifiers[i].what, amplifiers[i].sdz);
			continue;
		}
		if (!register_read(amplifiers[i].address, TAS_PAGE, &page) || !register_read(amplifiers[i].address, TAS_BOOK, &book)) {
			say("vayu-audio: 1: %#x (%s): no answer\n", amplifiers[i].address, amplifiers[i].what);
			continue;
		}
		if (page != 0 || book != 0) {
			say("vayu-audio: 1: %#x (%s): book %#x page %#x, not 0/0: the chip ID is not read (that would need a write)\n",
			    amplifiers[i].address, amplifiers[i].what, book, page);
			continue;
		}
		if (!register_read(amplifiers[i].address, TAS_CHIPID, &id)) {
			say("vayu-audio: 1: %#x (%s): no answer for the chip ID\n", amplifiers[i].address, amplifiers[i].what);
			continue;
		}
		say("vayu-audio: 1: %#x (%s): chip ID %#x: %s\n", amplifiers[i].address, amplifiers[i].what, id, chip_name(id));
	}
	if (woke) {
		sdz_restore();
	}
	crumb("amplifiers: done");
}

/* ---- the audio DSP (sys/vayu_pas.h) --------------------------------------------------------------- */

/*
 * dt: qcom,lpass@17300000 (qcom,pil-tz-generic): pas-id 1, firmware "adsp", smem-id 423,
 * memory-region pil_adsp_region@8be00000 (0x2200000: the image's last segment ends at 0x8e000000
 * exactly), vdd_cx = cx.lvl; qcom,smp2p-adsp: smem <443 429>, remote-pid 2, qcom,ipc <apcs 0 10>.
 * cx.lvl is the only rail it names, and the application processors' one vote for it is what
 * UEFI, the display and UFS stand on: it is not touched unless vayu_audio_cx=1 asks, and then only
 * raised to 384 (Linux's proxy vote: the cx domain at its top) and never voted down again.
 */
static struct vayu_rproc adsp = {
	.name = "adsp", .firmware = "adsp", .pas_id = 1, .memory = 0x8be00000, .memory_size = 0x2200000,
	.smem_host = 2, .smp2p_theirs = 443, .smp2p_ours = 429, .crash_item = 423, .apcs_smp2p = 10,
	.rails = { NULL, NULL }, .rail_level = 384, .rails_shared = true,
};

/*
 * dt: qcom,glink/adsp: remote-pid 2, mboxes <apcs-hmss-global 8>, interrupt SPI 156 (not listened
 * to: the edge's thread looks every millisecond); qcom,adsp_qrtr "IPCRTR" intents <0x800 5 0x2000
 * 3 0x4400 2>; qcom,apr_tal_rpmsg "apr_audio_svc" intents <0x200 20>.
 */
enum { CHANNEL_QRTR, CHANNEL_APR };
static struct vayu_qrtr qrtr;
static void apr_input(void *context, const uint8_t *data, uint32_t length);
static struct vayu_glink glink = {
	.rp = &adsp, .apcs_glink = 8, .fifo_size = 16384, .channels = 2,
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } },
	                           .input = vayu_qrtr_input, .context = &qrtr },
		[CHANNEL_APR] = { .name = "apr_audio_svc", .intents = { { 0x200, 20 }, { 0, 0 }, { 0, 0 } }, .input = apr_input },
	},
};
static struct vayu_qrtr qrtr = { .glink = &glink, .channel = &glink.channel[CHANNEL_QRTR] };

/* ---- APR: the audio services' protocol on apr_audio_svc (hakios.adsp.c, step 11) ---------------------------- */

/*
 * A packet is a header of eight words (kind, header length and version, the packet's length; from
 * and to: a service, a domain -- we are 5, the DSP 4 -- and a port each; a token the answer carries
 * back; an opcode) and what the opcode takes (Linux: drivers/soc/qcom/apr.c; Android's q6core.c).
 * The DSP's core service (3) answers whether the audio services are up and which it has.
 */
#define APR_DOMAIN_ADSP                 4
#define APR_DOMAIN_APPS                 5
#define APR_SVC_ADSP_CORE               3
#define APR_MSG_TYPE_SEQ_CMD            2
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)((type) << 8 | ((bytes) / 4) << 4))
#define AVCS_GET_VERSIONS               0x00012905u
#define AVCS_CMD_ADSP_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290du

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};

static struct {
	uint32_t opcode, token, length;
	uint32_t words[64];
	volatile bool there;
} apr_answer;
static uint32_t apr_in, apr_out, apr_next_token = 1, apr_waited_for;
static void sound_events(uint8_t service, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length);

static void
apr_input(void *context __unused, const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload;

	if (length < sizeof(header)) {
		say("vayu-audio: apr: %u bytes, no APR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if (header_length < sizeof(header) || header_length > length) {
		say("vayu-audio: apr: a header of %u bytes in a packet of %u: dropped\n", header_length, length);
		return;
	}
	payload = (header.size < length ? header.size : length) - header_length;
	apr_in++;
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		say("vayu-audio: apr: < service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	if (apr_waited_for == 0 || header.token != apr_waited_for) {
		uint32_t words[16];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		sound_events(header.from_service, header.opcode, header.token, words, payload);
		return;
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	apr_answer.there = true;
}

/* A command to one of the DSP's services and its answer waited for (ms): true if answered (in apr_answer). One at a time. */
static bool
apr_ask(uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	uint8_t packet[sizeof(struct apr_header) + 256];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	uint32_t token;
	int error;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	token = apr_next_token++;
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->token = token;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	apr_answer.there = false;
	apr_waited_for = token;
	error = vayu_glink_send(&glink, &glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		say("vayu-audio: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			answered = apr_answer.there && apr_answer.token == token;
			if (!answered) {
				delay_for_interval(1, NSEC_PER_MSEC);
			}
		}
	}
	apr_waited_for = 0;
	return answered;
}

/* A command not waited for (a write): what comes back goes to sound_events(). */
static bool
apr_post(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length)
{
	uint8_t packet[sizeof(struct apr_header) + 64];
	struct apr_header *header = (struct apr_header *)packet;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->token = token;
	header->opcode = opcode;
	memcpy(packet + sizeof(*header), payload, length);
	if (vayu_glink_send(&glink, &glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length) != 0) {
		return false;
	}
	apr_out++;
	return true;
}

/* ---- step 4: sound on the bottom speaker (hakios.sound.c's way, vayu's numbers) ---------------------- */

/*
 * What the audio services are told so that the bottom speaker plays, as Android's do it
 * (techpack/audio asoc/sm8150.c, msm-dai-q6-v2.c, dsp/q6afe.c, q6adm.c, q6asm.c; Linux's
 * sound/soc/qcom/qdsp6/): the quaternary MI2S port (AFE 0x1006) with its bit clock
 * (QUAD_MI2S_IBIT 0x106, 48000 x 16 x 2 = 1.536 MHz, couple-no), SD0, stereo, 16 bits, word select
 * ours, 48 kHz; a device path to it (ADM, no processing); a stream (ASM, 16-bit PCM, no
 * processing) routed to the path, its samples in memory of ours that the DSP reads through the
 * application processors' SMMU as stream 0x1b21 (dt: qcom,msm-audio-ion iommus <&apps_smmu 0x1b21
 * 0>, qcom,smmu-sid-mask 0xf: the stream's low bits, 1, in bits 32-35 of every address it is given).
 * The pins (137 bit clock, 138 word select, 139 data out, 140 data in) are switched to qua_mi2s by
 * the loader (vayu_audio_mi2s=1). With vayu_audio=4:
 *
 *   vayu_sound=1  the port only: clock on, I2S set, started; whether 137/138 toggle is looked at
 *                 (reads); the apps SMMU's matching for 0x1b21 read (no write); held, then stopped
 *   vayu_sound=2  ... and the stream: the SMMU given a context of ours for 0x1b21 (put back after),
 *                 ADM and ASM opened, 1 kHz at -30 dBFS written for vayu_audio_secs (at most 3);
 *                 the DSP's "write done" events say whether it takes 48 kHz. The amplifiers stay off.
 *   vayu_sound=3  ... and the bottom speaker's amplifier (TAS2562 at 0x4c; the earpiece's stays in
 *                 hardware shutdown, unless vayu_audio_ear=1): needs vayu_audio_xnu_tlmm=1 (this port's narrow exception for
 *                 98/99 and 89/92). Set up as Android's driver sets it (tas256x.c: software reset;
 *                 I/V sense off; misc 0xcf; I2S at 48 kHz, 16-bit slots, falling edge, offset 1;
 *                 the right channel; clock config 0x0c; halt timer 838 ms; interrupts masked; and the
 *                 vendor's power-up writes: DC blocker off, limiter off, brown-out protection on, TX
 *                 configuration 0x11, ICN; comparator hysteresis and I/V sense on at power-up), its
 *                 digital volume (book 0 page 2 0x0c-0x0f, Q2.30 big-endian, written in one burst as the
 *                 driver's bulk_write: the chip ignores single bytes; 2 dB steps) at vayu_audio_dvc (default
 *                 52 = 0x2013739e = -6 dB, which is also the cap; vayu_audio_dvc_max=N raises the cap up to
 *                 55 = 0x40000000 = 0 dB, the vendor's level), powered up only once the DSP takes the tone, its faults (over-temperature,
 *                 over-current, clock error, brownout, UVLO, OVLO: 0x24 bits 0-2, 0x25 bits 1-3) read
 *                 every 100 ms, any of them -> shut down at once. vayu_amp_vendor=0 makes none of the
 *                 vendor's extra writes: the sequence is as it was before them.
 *                 Then shutdown, SDZ low. Android's speaker protection (TI's algorithm in the DSP)
 *                 is not there: the levels stay far under anything that needs it.
 *                 vayu_audio_ear=1 drives the earpiece's TAS2564 at 0x4d (SDZ 89) with it and at the
 *                 same moments: left channel, receiver mode, 16 dBV and vayu_audio_ear_dvc (default 46 = -18 dB, at most 52).
 *   vayu_sound=4  the port and the stream kept running for vayu_sound_secs (default 40, at most 86400) with
 *                 what iOS writes to /dev/hakios-audio (feed(), below); the amplifiers are not touched
 *   vayu_sound=5  ... and the guard and the watcher (watch(), below): the bottom speaker's amplifier on while
 *                 there is sound; driven only with vayu_audio_amp=1 vayu_audio_xnu_tlmm=1, else only said
 *
 *   vayu_sound_pre=N vayu_sound_pre2=M  short steps (1-3, 6) before the main one; then the jack is given back,
 *                 so e.g. vayu_sound=5 vayu_sound_pre=2 vayu_sound_pre2=6 = headphone tone, recording, then the speaker feeder
 *   vayu_sound_then=N  a second step in the same boot after vayu_sound's (1-3 or 6), e.g. vayu_sound=2
 *                 vayu_sound_then=6: the headphone tone, then the microphones
 *   vayu_sound=6  the handset microphones, nothing played: the AFE's SLIMbus port 0x4001 (the codec's TX7 and TX8,
 *                 channels 135/136), a 3 s capture (vayu_mic_secs=N, 1..30) through ASM session 2 and an ADM live
 *                 recording path, the level (peak and integer RMS) of each channel every second. Needs vayu_jack=2
 *                 (the codec); the speaker amplifiers are never touched.
 */
extern lck_mtx_t hakios_smmu_lock;     /* (the SMMU's match registers and contexts: the Wi-Fi's and the GPU's take it too) */

#define APR_SVC_AFE             4
#define APR_SVC_ASM             7
#define APR_SVC_ADM             8
#define APR_BASIC_RSP_RESULT    0x000110e8u

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "an error this does not know";
}

/* One command and its answer: true if the basic answer has status 0, or the answer is `wanted' (then in apr_answer). */
static bool
command(const char *what, uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted)
{
	if (!apr_ask(service, port, opcode, payload, length, 1000)) {
		say("vayu-audio: %u: %s (opcode %#x to service %u, port %#x): NO ANSWER\n", command_step, what, opcode, service, port);
		return false;
	}
	if (apr_answer.opcode == APR_BASIC_RSP_RESULT && apr_answer.words[0] == opcode) {
		say("vayu-audio: %u: %s: %s%s\n", command_step, what, apr_answer.words[1] == 0 ? "done" : "REFUSED: ",
		    apr_answer.words[1] == 0 ? "" : dsp_error(apr_answer.words[1]));
		return apr_answer.words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && apr_answer.opcode == wanted) {
		return true;
	}
	say("vayu-audio: %u: %s: an answer not expected: opcode %#x, %u bytes: %#x %#x\n", command_step, what,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0], apr_answer.words[1]);
	return false;
}

/* ---- AFE: the quaternary MI2S port ---- */

#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_I2S_CONFIG                 0x0001020du
#define AFE_PARAM_ID_SLIMBUS_CONFIG             0x00010212u     /* q6afe.c:42 */
#define AFE_MODULE_CDC_DEV_CFG                  0x00010234u     /* HYPOTHESIS: wcd934x.c:9905-9925, q6afe.c:2809-2830 */
#define AFE_PARAM_ID_CDC_SLIMBUS_SLAVE_CFG      0x00010235u     /* HYPOTHESIS: wcd934x.c:9905-9925, q6afe.c:2809-2830 */
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u
#define PORT                    0x1006          /* AFE_PORT_ID_QUATERNARY_MI2S_RX */
#define CLOCK_QUAD_MI2S_IBIT    0x106
#define CLOCK_COUPLE_NO         1
#define RATE                    48000
#define BIT_CLOCK               (RATE * 16 * 2)

static uint16_t port_id = PORT;

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_i2s {
	uint32_t version;
	uint16_t bits, lines, stereo, ws_source;
	uint32_t rate;
	uint16_t data_format, reserved;
} __attribute__((packed));

struct afe_slimbus {
	uint32_t minor;
	uint16_t dev_id, bit_width, data_format, num_channels;
	uint8_t shared_ch[8];
	uint32_t sample_rate;
} __attribute__((packed));                       /* q6afe.c:419-446; msm-dai-q6-v2.c:2236-2291, 2557-2590 */

struct afe_slave_cfg {
	uint32_t minor_version, enum_addr_lsw, enum_addr_msw;
	uint16_t tx_port_offset, rx_port_offset;
} __attribute__((packed));                       /* HYPOTHESIS: wcd934x.c:9905-9925, q6afe.c:2809-2830 */

/* A parameter of the AFE as a whole (its clocks): in the packet. */
static bool
afe_service_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	uint32_t *head = (uint32_t *)packet;
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	head[0] = (uint32_t)sizeof(*p) + size;          /* payload size; address 0 and handle 0: in band */
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, 0, AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size, 0);
}

static bool
afe_port_param(const char *what, uint16_t port, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = port;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size, 0);
}

#define JACK_AFE_PORT           0x400C          /* AFE_PORT_ID_SLIMBUS_MULTI_CHAN_6_RX (q6afe.c:113-114); HAL's SLIMBUS_6_RX */
#define JACK_GAIN_MAX           (6 * 8)         /* vayu_jack_gain: at most 6 dB before the guard while on the jack */

static bool clock_on;                           /* the quaternary MI2S bit clock is on */
static bool jack_port_on;                       /* the port 0x400c is started (the jack's path is up) */
static bool mi2s_port_on;                       /* the port 0x1006 is started */
static bool audio_route_jack;                   /* the stream is routed to 0x400c: the watcher leaves the speaker amplifier alone */
static uint32_t route_switches;                 /* route_switch() calls that took */
static uint32_t jack_gain;                      /* vayu_jack_gain, 1/8 dB, 0..JACK_GAIN_MAX: the guard's gain on the jack */
static uint32_t speaker_gain = HAKIOS_GUARD_MAX_GAIN;  /* vayu_sound_gain, 1/8 dB: the guard's gain for the speaker */

static bool
afe_clock(bool on)
{
	struct afe_clock_set clock = { 1, CLOCK_QUAD_MI2S_IBIT, BIT_CLOCK, CLOCK_COUPLE_NO, 0, on };

	return afe_service_param(on ? "the quaternary MI2S bit clock (0x106) at 1.536 MHz, on" : "the MI2S bit clock, off",
	           AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

/* The jack's port: the SLIMbus configuration, started, then the codec's path up. */
static bool
afe_port_start_slimbus_rx(void)
{
	struct afe_slimbus slim = { .minor = 1, .dev_id = 0 /* AFE_SLIMBUS_DEVICE_1 */, .bit_width = 16,
		               .data_format = 0, .num_channels = 2, .shared_ch = { 146, 147 }, .sample_rate = RATE };
	uint16_t start[2] = { JACK_AFE_PORT, 0 };
	uint32_t slave = 0;

	if (jack_port_on) {                     /* (a stop that failed left it running; only the path is down) */
		return vayu_jack_path_on();
	}
	crumb("4: the port 0x400c: SLIMbus configuration, start (AFE)");
	PE_parse_boot_argn("vayu_jack_afe_slave", &slave, sizeof(slave));
	if (slave != 0) {
		struct afe_slave_cfg cfg = { 1, 0x02500100, 0x00000217, 0, 16 };

		/* (the ids are HYPOTHESIS: wcd934x.c:9905-9925, q6afe.c:2809-2830) */
		if (!afe_service_param("the codec's SLIMbus slave configuration (HYPOTHESIS)", AFE_MODULE_CDC_DEV_CFG,
		    AFE_PARAM_ID_CDC_SLIMBUS_SLAVE_CFG, &cfg, sizeof(cfg))) {
			return false;
		}
	}
	if (!afe_port_param("the port 0x400c: SLIMbus, 2 channels 146/147, 16 bits, 48 kHz", JACK_AFE_PORT,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_SLIMBUS_CONFIG, &slim, sizeof(slim))) {
		return false;
	}
	if (!command("the port 0x400c, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0)) {
		return false;
	}
	jack_port_on = true;
	if (!vayu_jack_path_on()) {
		command("the port 0x400c, stopped (the jack's path did not come up)", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP,
		    start, sizeof(start), 0);
		jack_port_on = false;
		return false;
	}
	return true;
}

/* The speaker's port: the bit clock, the I2S configuration, started. */
static bool
afe_port_start_mi2s(void)
{
	struct afe_i2s i2s = { .version = 1, .bits = 16, .lines = 1 /* SD0 */, .stereo = 1, .ws_source = 1 /* ours */,
		               .rate = RATE, .data_format = 0 };
	uint16_t start[2] = { PORT, 0 };

	if (mi2s_port_on) {                     /* (a stop that failed left it running) */
		if (!clock_on) {
			clock_on = afe_clock(true);
		}
		return clock_on;
	}
	crumb("4: the port: bit clock, I2S configuration, start (AFE)");
	clock_on = afe_clock(true);
	if (!clock_on || !afe_port_param("the port 0x1006: I2S, SD0, stereo, 16 bits, 48 kHz, word select ours", PORT,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, &i2s, sizeof(i2s))) {
		return false;
	}
	mi2s_port_on = command("the port 0x1006, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0);
	return mi2s_port_on;
}

/* One port down: the jack's path first, then its port; the speaker's port, then its bit clock. */
static bool
afe_port_stop(uint16_t port)
{
	uint16_t stop[2] = { port, 0 };
	bool ok = true;

	if (port == JACK_AFE_PORT) {
		if (!jack_port_on) {
			return true;
		}
		vayu_jack_path_off();
		ok = command("the port 0x400c, stopped", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0);
		if (ok) {
			jack_port_on = false;
		}
		return ok;
	}
	if (mi2s_port_on) {
		ok = command("the port 0x1006, stopped", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0);
		if (ok) {
			mi2s_port_on = false;
		}
	}
	if (clock_on) {
		ok = afe_clock(false) && ok;
		clock_on = false;
	}
	return ok;
}

static bool
afe_on(void)
{
	/* vayu_jack=3: the jack only when the MBHC sees headphones; vayu_jack=2 always uses it. */
	if (vayu_jack_ready() && (vayu_jack_state() == 1 || !vayu_jack_keep())) {
		vayu_jack_set_route(1);         /* vayu_jack=3: only when headphones are plugged at stream start */
		port_id = JACK_AFE_PORT;
		audio_route_jack = true;
		return afe_port_start_slimbus_rx();
	}

	vayu_jack_set_route(0);                 /* the MI2S path: the speaker amplifier */
	port_id = PORT;
	audio_route_jack = false;
	return afe_port_start_mi2s();
}

static void
afe_off(void)
{
	afe_port_stop(JACK_AFE_PORT);
	afe_port_stop(PORT);
	port_id = PORT;
	audio_route_jack = false;
}

/* Whether the bit clock and the word select move at the pins (reads only; the reads are slower than the clock). */
static void
pin_activity(void)
{
	static const unsigned watched[2] = { 137, 138 };

	for (unsigned i = 0; i < 2; i++) {
		uint32_t changes = 0, ones = 0, last = rd(gpio_io(watched[i])) & 1;

		for (unsigned n = 0; n < 20000; n++) {
			uint32_t now = rd(gpio_io(watched[i])) & 1;

			changes += now != last;
			ones += now;
			last = now;
		}
		say("vayu-audio: 4: gpio%u (%s; cfg %#x, function %u): %u changes and %u ones in 20000 reads%s\n", watched[i],
		    i == 0 ? "bit clock" : "word select", rd(gpio_cfg(watched[i])), (rd(gpio_cfg(watched[i])) >> 2) & 15, changes,
		    ones, changes ? ": IT MOVES" : ": still");
	}
}

/* ---- the DSP's view of our memory: stream 0x1b21 of the apps SMMU (hakios.sound.c) ---- */

static vm_offset_t smmu_base, smmu_second, smmu_context;
#define SMMU_PHYS               0x15000000UL    /* dt: apps-smmu@15000000 */
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define PTE_ADDRESS             0x0000fffffffff000ull
#define AUDIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define AUDIO_STREAM            0x1b21
#define AUDIO_ADDRESSES         0x10000000ull   /* our choice: nothing else uses this context */
#define AUDIO_STREAM_BITS       (1ull << 32)    /* 0x1b21 & 0xf */
#define PERIODS                 8
#define PERIOD_FRAMES           960             /* 20 ms */
#define PERIOD_BYTES            (PERIOD_FRAMES * 4)
#define BUFFER_BYTES            (PERIODS * 4096)

static struct {
	bool found, on, passed;
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	vm_offset_t tables;
} space;
static vm_offset_t buffer;
static uint64_t buffer_address;

static void
smmu_put(vm_offset_t base, const char *name, uint32_t offset, uint32_t value)
{
	say("vayu-audio: %u: smmu: about to write %s (+%#x): %#x\n", command_step, name, offset, value);
	wr(base + offset, value);
}

/* Reads only: is 0x1b21 matched already (and how), which match register and context are free. */
static bool
space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts;

	if (smmu_base == 0) {
		smmu_base = ml_io_map(SMMU_PHYS, 0x2000);
	}
	scr0 = rd(smmu_base + SMMU_SCR0);
	idr0 = rd(smmu_base + SMMU_IDR0);
	idr1 = rd(smmu_base + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	say("vayu-audio: %u: smmu: scr0 %#x idr0 %#x idr1 %#x: %u match registers, %u contexts\n", command_step, scr0, idr0,
	    idr1, matches, contexts);
	if (scr0 & 1u << 3) {
		say("vayu-audio: %u: smmu: 16-bit stream numbers (EXIDS), which nothing here is written for\n", command_step);
		return false;
	}
	space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	space.pages = 2u << (idr1 >> 28 & 7);
	space.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu_base + SMMU_SMR(n)), s2cr = rd(smmu_base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ AUDIO_STREAM) & ~mask) == 0) {
				say("vayu-audio: %u: smmu: match register %u covers stream %#x already: stream %#x mask %#x, s2cr %#x\n",
				    command_step, n, AUDIO_STREAM, stream, mask, s2cr);
				if ((s2cr >> 16 & 3) == 1) {
					space.passed = true;
					return true;
				}
				say("vayu-audio: %u: smmu: ... and does not let it through untranslated: somebody's, left alone\n", command_step);
				return false;
			}
		} else if (space.match == ~0u) {
			space.match = n;
			space.old_smr = smr;
			space.old_s2cr = s2cr;
		}
	}
	for (space.context = 0; space.context < contexts && used[space.context]; space.context++) {
	}
	if (space.match == ~0u || space.context >= contexts) {
		say("vayu-audio: %u: smmu: no match register or no context left\n", command_step);
		return false;
	}
	smmu_second = ml_io_map(SMMU_PHYS + space.page, 0x1000);
	smmu_context = ml_io_map(SMMU_PHYS + (space.pages + space.context) * space.page, 0x1000);
	space.old_sctlr = rd(smmu_context + SMMU_CB_SCTLR);
	say("vayu-audio: %u: smmu: nothing set up for stream %#x; free: match register %u (smr %#x, s2cr %#x), context %u at %#lx "
	    "(sctlr %#x)\n", command_step, AUDIO_STREAM, space.match, space.old_smr, space.old_s2cr, space.context,
	    SMMU_PHYS + (space.pages + space.context) * space.page, space.old_sctlr);
	if (space.old_sctlr & 1) {
		say("vayu-audio: %u: smmu: CONTEXT %u IS ON although no stream goes through it: somebody's, left alone\n", command_step,
		    space.context);
		return false;
	}
	space.found = true;
	return true;
}

static void
space_faults(void)
{
	uint32_t status, global;

	if (!space.on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu_base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("vayu-audio: %u: smmu: the DSP was refused nothing (fsr %#x, sgfsr 0)\n", command_step, status);
		return;
	}
	say("vayu-audio: %u: smmu: THE SMMU REFUSED A REQUEST: context %u status %#x address %#llx syndrome %#x; global %#x stream "
	    "%#x\n", command_step, space.context, status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 |
	    rd(smmu_context + SMMU_CB_FAR), rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu_base + SMMU_SGFSYNR1));
}

/* Our buffer behind the DSP's addresses from AUDIO_ADDRESSES on, through a context of ours. */
static bool
space_on(void)
{
	uint64_t *root, *second, *third, table_root;
	uint32_t sees = 1u << 31 | AUDIO_STREAM;

	if (kmem_alloc(kernel_map, &space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	root = (uint64_t *)space.tables;
	second = (uint64_t *)(space.tables + 4096);
	third = (uint64_t *)(space.tables + 8192);
	root[AUDIO_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[AUDIO_ADDRESSES >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned page = 0; page < BUFFER_BYTES / 4096; page++) {
		third[(AUDIO_ADDRESSES >> 12 & 0x1ff) + page] = kvtophys(buffer + 4096 * page) | AUDIO_PTE;
	}
	for (vm_offset_t line = space.tables; line < space.tables + 3 * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(space.tables) | (uint64_t)space.context << 48;
	crumb("%u: smmu: ABOUT TO BE WRITTEN: context %u, page tables at %#llx for %u KB at %#llx; match register %u sends %#x there",
	    command_step, space.context, table_root & PTE_ADDRESS, BUFFER_BYTES / 1024, AUDIO_ADDRESSES, space.match,
	    AUDIO_STREAM);
	smmu_put(smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	smmu_put(smmu_second, "cba2r", SMMU_CBA2R(space.context), SMMU_CBA2R_64_BIT);
	smmu_put(smmu_second, "cbar", SMMU_CBAR(space.context), SMMU_CBAR_STAGE_1);
	smmu_put(smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	smmu_put(smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	smmu_put(smmu_context, "ttbr0 low", SMMU_CB_TTBR0, (uint32_t)table_root);
	smmu_put(smmu_context, "ttbr0 high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	smmu_put(smmu_context, "ttbr1 low", SMMU_CB_TTBR1, 0);
	smmu_put(smmu_context, "ttbr1 high", SMMU_CB_TTBR1 + 4, 0);
	smmu_put(smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	smmu_put(smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	smmu_put(smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	smmu_put(smmu_context, "tlbiasid", SMMU_CB_TLBIASID, space.context);
	smmu_put(smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(smmu_context + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("vayu-audio: %u: smmu: it does not finish forgetting old translations\n", command_step);
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	smmu_put(smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	space.on = true;
	smmu_put(smmu_base, "s2cr", SMMU_S2CR(space.match), space.context);
	smmu_put(smmu_base, "smr", SMMU_SMR(space.match), sees);
	if (rd(smmu_base + SMMU_SMR(space.match)) != sees || (rd(smmu_base + SMMU_S2CR(space.match)) & 0x300ff) != space.context ||
	    !(rd(smmu_context + SMMU_CB_SCTLR) & 1) || rd(smmu_context + SMMU_CB_TTBR0) != (uint32_t)table_root) {
		say("vayu-audio: %u: smmu: IT DID NOT TAKE WHAT WAS WRITTEN (smr %#x s2cr %#x sctlr %#x ttbr0 %#x): the hypervisor may "
		    "own it\n", command_step, rd(smmu_base + SMMU_SMR(space.match)), rd(smmu_base + SMMU_S2CR(space.match)),
		    rd(smmu_context + SMMU_CB_SCTLR), rd(smmu_context + SMMU_CB_TTBR0));
		return false;
	}
	crumb("%u: smmu: STREAM %#x GOES THROUGH CONTEXT %u with our page tables", command_step, AUDIO_STREAM, space.context);
	return true;
}

static void
space_off(void)
{
	if (!space.on) {
		return;
	}
	space_faults();
	smmu_put(smmu_base, "smr (as found)", SMMU_SMR(space.match), space.old_smr);
	smmu_put(smmu_base, "s2cr (as found)", SMMU_S2CR(space.match), space.old_s2cr);
	smmu_put(smmu_context, "sctlr (as found)", SMMU_CB_SCTLR, space.old_sctlr);
	space.on = false;
}

/* ---- ADM and ASM: the path and the stream ---- */

#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_PATH_PLAYBACK                       1
#define ADM_MATRIX_AUDIO_RX                     0
#define ADM_TOPOLOGY_NONE                       0x00010312u
#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_WRITE_V3            0x00010db3u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2        0x00010d98u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_WRITE_V2                   0x00010dabu
#define ASM_DATA_EVENT_WRITE_DONE_V2            0x00010d99u
#define ASM_SESSION_EVENT_RX_UNDERFLOW          0x00010c17u
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define ASM_SHMEM_4K_POOL                       3
#define SESSION                                 1
#define STREAM_PORT                             (SESSION << 8 | 1)
#define WRITE_TOKEN                             0x80000000u
#define PCM_CHANNEL_FL                          1
#define PCM_CHANNEL_FR                          2

/* vayu_sound=6: the handset microphones (MIC_TX.md sections 4 and 5; mainline q6afe.c:91, q6asm.c:50-51) */
#define MIC_AFE_PORT                            0x4001u         /* AFE_PORT_ID_SLIMBUS_MULTI_CHAN_0_TX */
#define ADM_PATH_LIVE_RECORDING                 2
#define ADM_MATRIX_AUDIO_TX                     1               /* mainline q6adm.c:35 */
#define ASM_STREAM_CMD_OPEN_READ_V3             0x00010db4u
#define ASM_STREAM_CMD_SET_ENCDEC_PARAM         0x00010c10u
#define ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2      0x00010da3u
#define ASM_DATA_CMD_READ_V2                    0x00010dacu
#define ASM_DATA_EVENT_READ_DONE_V2             0x00010d9au
#define MIC_SESSION                             2               /* (the speakers' is 1) */
#define MIC_STREAM_PORT                         (MIC_SESSION << 8 | 1)
#define MIC_READ_TOKEN                          0x40000000u     /* | the period */
#define MIC_PERIODS                             8
#define MIC_PERIOD_FRAMES                       960             /* 20 ms at 48 kHz */
#define MIC_PERIOD_BYTES                        (MIC_PERIOD_FRAMES * 4) /* 2 channels of 16 bits: 3840; one 4096-byte page each */

static bool mapped, copp_open, stream_open;
static uint32_t map_handle, copp;
static volatile uint32_t returned, written_done, underflows, write_errors;

static bool mic_mapped, mic_stream_open, mic_copp_open, mic_port_on;
static uint32_t mic_map_handle, mic_copp, mic_seconds, mic_peaks[2];
static volatile uint32_t mic_read_ready, mic_reads_done, mic_read_errors;
static volatile uint32_t mic_read_sizes[MIC_PERIODS];
static volatile uint32_t mic_words_logged;

/* The DSP's events: every write's "done", underflows, the microphones' reads (on the GLINK edge's thread). */
static void
sound_events(uint8_t service, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if (service != APR_SVC_ASM) {
		return;
	}
	if ((opcode == ASM_DATA_EVENT_WRITE_DONE_V2 || opcode == APR_BASIC_RSP_RESULT) && (token & WRITE_TOKEN)) {
		if (opcode == APR_BASIC_RSP_RESULT || (length >= 16 && payload[3] != 0)) {
			OSIncrementAtomic((volatile SInt32 *)&write_errors);
		}
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
	} else if (opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		OSIncrementAtomic((volatile SInt32 *)&underflows);
	} else if ((token & MIC_READ_TOKEN) && (token & 0x3fffffff) < MIC_PERIODS) {
		uint32_t period = token & 0x3fffffff;

		if (opcode == ASM_DATA_EVENT_READ_DONE_V2) {
			uint32_t size = 0;

			/* status, buffer address lsw, buffer address msw, size, offset (q6asm.c:2270-2273) */
			if (length < 16 || payload[0] != 0) {
				OSIncrementAtomic((volatile SInt32 *)&mic_read_errors);
			} else {
				size = payload[3];
			}
			mic_read_sizes[period] = size;
			OSIncrementAtomic((volatile SInt32 *)&mic_reads_done);
			OSBitOrAtomic(1u << period, &mic_read_ready);
			if (!mic_words_logged) {
				char line[200];
				int at = 0;

				mic_words_logged = 1;
				for (uint32_t i = 0; i < length / 4 && i < 10 && at < (int)sizeof(line) - 12; i++) {
					at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", payload[i]);
				}
				say("vayu-audio: 6: the first READ_DONE: opcode %#x, token %#x, %u bytes:%s\n", opcode, token, length,
				    line);
			}
		} else if (opcode == APR_BASIC_RSP_RESULT && length >= 8 && payload[1] != 0) {
			mic_read_sizes[period] = 0;
			OSIncrementAtomic((volatile SInt32 *)&mic_read_errors);
			OSBitOrAtomic(1u << period, &mic_read_ready);
		}
	}
}

/* A playback device path to `port'; its COPP in *copp. The checks are the ones stream_on has always made. */
static bool
adm_open(uint16_t port, uint32_t *opened)
{
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_PLAYBACK, port, 0xffff, ADM_TOPOLOGY_NONE, 2, 16, RATE,
		                           { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };

	if (!command("a device path to the port: playback, no processing, 2 channels of 16 bits at 48 kHz", APR_SVC_ADM, port,
	    ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), ADM_CMDRSP_DEVICE_OPEN_V5)) {
		return false;
	}
	if (apr_answer.words[0] != 0) {
		say("vayu-audio: 4: the device path: REFUSED: %s\n", dsp_error(apr_answer.words[0]));
		return false;
	}
	*opened = apr_answer.words[1] & 0xffff;
	say("vayu-audio: 4: the device path is open: COPP %u\n", *opened);
	return true;
}

static bool
adm_close(uint32_t closed)
{
	return command("the device path, closed", APR_SVC_ADM, (uint16_t)closed, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0);
}

/* Session 1 to one COPP on the playback (AUDIO_RX) matrix. */
static bool
matrix_route(uint32_t to)
{
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_RX, 1, SESSION, 1, { 0, 0 } };

	route.copp[0] = (uint16_t)to;
	return command("session 1 routed to the device path", APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route),
	           0);
}

static bool
stream_on(void)
{
	struct {
		uint16_t pool, regions;
		uint32_t property;
		uint32_t low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)buffer_address, (uint32_t)(buffer_address >> 32),
		                          BUFFER_BYTES };
	struct {
		uint32_t flags;
		uint16_t sink, bits;
		uint32_t topology, format;
	} __attribute__((packed)) write = { 0, 0, 16, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
	struct {
		uint32_t size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) format = { 20, 2, 16, RATE, 1, 0, { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	uint32_t run[3] = { 0, 0, 0 };

	crumb("4: the DSP is given our periods (ASM), a device path to the port (ADM) and a stream routed to it (ASM)");
	if (!command("our periods, mapped for the DSP", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map),
	    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS)) {
		return false;
	}
	mapped = true;
	map_handle = apr_answer.words[0];
	say("vayu-audio: 4: our %u KB at %#llx are the DSP's handle %#x\n", BUFFER_BYTES / 1024, buffer_address, map_handle);
	if (!adm_open(port_id, &copp)) {
		return false;
	}
	copp_open = true;
	if (!command("the stream, opened for writing: 16-bit PCM, no processing", APR_SVC_ASM, STREAM_PORT,
	    ASM_STREAM_CMD_OPEN_WRITE_V3, &write, sizeof(write), 0)) {
		return false;
	}
	stream_open = true;
	return matrix_route(copp) &&
	       command("the stream's format: 2 channels of 16 bits at 48 kHz", APR_SVC_ASM, STREAM_PORT,
	           ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &format, sizeof(format), 0) &&
	       command("the session, run", APR_SVC_ASM, STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0);
}

static void
stream_off(void)
{
	uint32_t handle = map_handle;

	if (stream_open) {
		command("the stream, closed", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0);
		stream_open = false;
	}
	if (copp_open) {
		adm_close(copp);
		copp_open = false;
	}
	if (mapped) {
		command("our periods, unmapped", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &handle, sizeof(handle), 0);
		mapped = false;
	}
}

/* ---- the amplifiers: TAS2562 at 0x4c (bottom) and TAS2564 at 0x4d (earpiece) ---- */

#define TAS_RESET               0x01
#define TAS_POWER               0x02            /* [3] I sense off, [2] V sense off, [1:0] 0 active, 1 mute, 2 shutdown */
#define TAS_POWER_OFF           0x0e            /* I/V sense off, shutdown */
#define TAS_POWER_ON            0x0c            /* I/V sense off, active */
#define TAS_POWER_ON_IV         0x00            /* I/V sense on, active (tas256x.h:37-43; tas256x-codec.c:366) */
#define TAS_DCBLOCKER           0x03            /* [6] DC blocker disabled (tas256x.h:50-53; tas256x.c:994-1004) */
#define TAS_MISC                0x04
#define TAS_TDM0                0x06            /* [5] ramp 44.1, [3:1] rate (4: 44.1/48 kHz) */
#define TAS_TDM1                0x07            /* [5:1] RX offset, [0] RX edge (0 falling) */
#define TAS_TDM2                0x08            /* [5:4] RX slot config (2: mono right, 1: mono left), [3:2] word, [1:0] slot (0: 16) */
#define TAS_TDM4                0x0a            /* TX configuration 4: I/V data (0x11 right, 0xf1 left; tas256x.h:130) */
#define TAS_LIMITER             0x12            /* [0] limiter enable (tas256x.h:352-354; tas256x.c:1327-1340) */
#define TAS_BOP                 0x14            /* [4] brown-out shutdown, [1] mute, [0] enable (tas256x.h:366-372) */
#define TAS_MASK0               0x1a
#define TAS_MASK1               0x1b
#define TAS_LATCHED0            0x24            /* [2] TDM clock error, [1] over-current, [0] over-temperature */
#define TAS_LATCHED1            0x25            /* [3] VBAT OVLO, [2] VBAT UVLO, [1] brownout */
#define TAS_INT_CONFIG          0x30            /* [5:3] clock halt timer, [2] clear latched */
#define TAS_CLOCK_CONFIG        0x38
#define TAS_DVC                 0x0c            /* book 0 page 2: 0x0c-0x0f, big-endian Q2.30 */
#define TAS_FAULTS0             0x07
#define TAS_FAULTS1             0x0e
#define TAS_ICN_THRESHOLD       0x64            /* book 0 page 2: ICN threshold, 4 bytes (tas256x.h:315) */
#define TAS_ICN_HYSTERESIS      0x6c            /* book 0 page 2: ICN hysteresis, 4 bytes (tas256x.h:316) */
#define TAS_ICN_PAGE            0xfd            /* book 0 page 0xfd: the ICN configuration (tas256x.c:873-884) */
#define TAS_ICN_PAGE_CONFIG     0x0d            /* page 0xfd: test page configuration */
#define TAS_CLASSD_CONFIG1      0x19            /* page 0xfd: class-D configuration 1 */
#define TAS_MISC_CLASSD         0x21            /* book 0 page 1: [3] comparator hysteresis (tas256x.h:389-391) */
#define TAS_CMP_HYST            0x08            /* (tas256x.h:390: 1 << 3) */
#define TAS_PLAYBACK0           0x03            /* book 0 page 0: DC blocker, and on the TAS2564 RX mode and level */
#define TAS_TEST_PAGE_LOCK      0x0d            /* book 0 page 0xfd: the test-page unlock (tas256x.h:383) */
#define TAS_DAC_MODULATOR       0x12            /* book 0 page 0xfd: DAC modulator dither (tas256x.h:384) */
#define TAS_ICN_IMPROVE         0x46            /* book 0 page 0xfd: ICN improvement (tas256x.h:387) */
#define TAS_CLASSD_CONFIG2      0x32            /* book 0 page 0xfd: class-D configuration 2 (tas256x.h:324) */
#define TAS_ICN_SW              0x3e            /* book 0 page 0: ICN switch (tas256x.h:318) */

static const uint8_t dvc_table[56][4] = {       /* tas256x.c dvc_pcm[0..55]: -110 dB to 0 dB, 2 dB a step */
	{ 0x00, 0x00, 0x0D, 0x43 }, { 0x00, 0x00, 0x10, 0xB3 }, { 0x00, 0x00, 0x15, 0x05 }, { 0x00, 0x00, 0x1A, 0x77 },
	{ 0x00, 0x00, 0x21, 0x51 }, { 0x00, 0x00, 0x29, 0xF1 }, { 0x00, 0x00, 0x34, 0xCE }, { 0x00, 0x00, 0x42, 0x7A },
	{ 0x00, 0x00, 0x53, 0xB0 }, { 0x00, 0x00, 0x69, 0x5B }, { 0x00, 0x00, 0x84, 0xA3 }, { 0x00, 0x00, 0xA6, 0xFA },
	{ 0x00, 0x00, 0xD2, 0x37 }, { 0x00, 0x01, 0x08, 0xA5 }, { 0x00, 0x01, 0x4D, 0x2A }, { 0x00, 0x01, 0xA3, 0x6E },
	{ 0x00, 0x02, 0x10, 0x08 }, { 0x00, 0x02, 0x98, 0xC1 }, { 0x00, 0x03, 0x44, 0xE0 }, { 0x00, 0x04, 0x1D, 0x90 },
	{ 0x00, 0x05, 0x2E, 0x5B }, { 0x00, 0x06, 0x85, 0xC8 }, { 0x00, 0x08, 0x36, 0x22 }, { 0x00, 0x0A, 0x56, 0x6D },
	{ 0x00, 0x0D, 0x03, 0xA7 }, { 0x00, 0x10, 0x62, 0x4E }, { 0x00, 0x14, 0xA0, 0x51 }, { 0x00, 0x19, 0xF7, 0x86 },
	{ 0x00, 0x20, 0xB0, 0xBD }, { 0x00, 0x29, 0x27, 0x9E }, { 0x00, 0x33, 0xCF, 0x8E }, { 0x00, 0x41, 0x39, 0xD3 },
	{ 0x00, 0x52, 0x1D, 0x51 }, { 0x00, 0x67, 0x60, 0x45 }, { 0x00, 0x82, 0x24, 0x8A }, { 0x00, 0xA3, 0xD7, 0x0A },
	{ 0x00, 0xCE, 0x43, 0x29 }, { 0x01, 0x03, 0xAB, 0x3D }, { 0x01, 0x46, 0xE7, 0x5E }, { 0x01, 0x9B, 0x8C, 0x27 },
	{ 0x02, 0x06, 0x1B, 0x8A }, { 0x02, 0x8C, 0x42, 0x40 }, { 0x03, 0x35, 0x25, 0x29 }, { 0x04, 0x09, 0xC2, 0xB1 },
	{ 0x05, 0x15, 0x6D, 0x69 }, { 0x06, 0x66, 0x66, 0x66 }, { 0x08, 0x0E, 0x9F, 0x97 }, { 0x0A, 0x24, 0xB0, 0x63 },
	{ 0x0C, 0xC5, 0x09, 0xAC }, { 0x10, 0x13, 0x79, 0x88 }, { 0x14, 0x3D, 0x13, 0x62 }, { 0x19, 0x7A, 0x96, 0x7F },
	{ 0x20, 0x13, 0x73, 0x9E }, { 0x28, 0x61, 0x9A, 0xEA }, { 0x32, 0xD6, 0x46, 0x18 }, { 0x40, 0x00, 0x00, 0x00 },
};
/*
 * The level (lead, 2026-10-24): -6 dB by default and at most, 6 dB under the vendor's 0 dB (tas256x.c:262), as there is
 * no speaker protection loop here (TI's runs in Android's DSP); vayu_audio_dvc_max=N (up to 55 = 0 dB) allows more.
 * The guard's limiter (vayu_sound=5) stays in either case. (tester, run 202605: keyboard clicks audible at -20 dB.)
 */
#define DVC_DEFAULT             52              /* -6 dB */
#define DVC_TOP                 55              /* 0 dB, the vendor's level: the table's last row (tas256x.c:69, row 55) */
#define EAR_DVC_DEFAULT         46              /* -18 dB: 0x080e9f97, 2 dB a step below 52 (-6 dB), 55 being 0 dB */
#define EAR_DVC_TOP             52              /* -6 dB, whatever vayu_audio_dvc_max says */
static uint32_t dvc_most = DVC_DEFAULT;         /* vayu_audio_dvc_max */

/* tas256x.c:12-13 (written at 857-871): the ICN threshold and hysteresis the vendor sets for -80 dB (tas256x.h:315-316) */
static const uint8_t icn_threshold[4] = { 0x00, 0x01, 0x2f, 0x2c };
static const uint8_t icn_hysteresis[4] = { 0x00, 0x01, 0x5d, 0xc0 };

struct amp {
	uint8_t addr;
	unsigned sdz_pin;
	const char *name;
	bool tas2564;
	bool live;                      /* prepared and perhaps powered: stop/quiesce drive its SDZ low */
	bool prepared;                  /* the whole set-up went through: it may be started */
	bool sdz_raised;                /* this driver drove SDZ high and has not driven it low since */
};

static struct amp amp_bottom = {
	.addr = 0x4c, .sdz_pin = PIN_SDZ_RIGHT, .name = "TAS2562, bottom speaker",
};
static struct amp amp_ear = {
	.addr = 0x4d, .sdz_pin = PIN_SDZ_LEFT, .name = "TAS2564, earpiece", .tas2564 = true,
};

static bool amp_ear_enabled;             /* vayu_audio_ear=1 */
static uint32_t dvc_index = DVC_DEFAULT;
static uint32_t ear_dvc_index = EAR_DVC_DEFAULT;
static uint32_t amp_vendor = 1;         /* vayu_amp_vendor=0: the vendor's power-up writes are not made */
static uint32_t amp_boost = 1;          /* vayu_amp_boost=0: the TAS2562's boost voltage and current limit left at reset */
static uint32_t amp_limiter = 1;        /* vayu_amp_limiter=0: the TAS2562's own limiter stays off, as Android leaves it */

/*
 * The TAS2562's boost and limiter, as the vendor's init writes them (tas256x-codec.c:1786-1810 calls the setters with
 * the defaults of tas256x.c:261-282) -- tester, 2026-10-25: music distorted when loud, with the battery at ~20 %:
 *   boost max voltage   book 0 page 0 0x34 [3:0] = bst_vltg 0xa + 1 (tas256x.c:269, setter l.1288-1294; tas2562.h:647-649)
 *   boost current limit page 0 0x40 [5:0] = bst_ilm 0x36 (tas256x.c:270, setter l.1305-1318; tas256x.h:348-350)
 *   VBAT filter         page 0 0x3b [6:5] = vbat_lpf 2 (tas256x.c:282; tas256x.h:305-307)
 *   limiter             page 2 0x14 max attenuation -9 dB (index 7), 0x18 threshold max 9 V (index 13), 0x1c threshold
 *                       min 4 V (index 3), 0x20 inflection 3.3 V (index 13), 0x24 tracking slope 1 V/V (index 0), each
 *                       4 bytes in one burst (the tables at tas256x.c:101-170; tas256x.h:338-342); page 0 0x12 [3:1]
 *                       attack rate 1, [5:4] attack step 1, [0] enable; 0x13 [5:3] release rate 6, [7:6] release step 1
 *                       (tas256x.c:272-276; tas256x.h:352-364)
 * The vendor leaves the limiter itself off (lim_switch 0): under Android the DSP's speaker protection does that job,
 * which does not run here -- so here it is on (vayu_amp_limiter=0: off). A write that does not take is said and the
 * limiter left off; the boost likewise left as it was. Neither stops the amplifier from playing.
 */
static const uint8_t lim_params[5][4] = {
	{ 0x2D, 0x6A, 0x86, 0x6F },     /* 0x14: lim_max_attn[7], -9 dB */
	{ 0x48, 0x00, 0x00, 0x00 },     /* 0x18: vbat_lim_max_thd[13], 9 V */
	{ 0x20, 0x00, 0x00, 0x00 },     /* 0x1c: vbat_lim_min_thd[3], 4 V */
	{ 0x33, 0x33, 0x33, 0x33 },     /* 0x20: vbat_lim_infl_pt[13], 3.3 V */
	{ 0x10, 0x00, 0x00, 0x00 },     /* 0x24: vbat_lim_track_slope[0], 1 V/V */
};

/* The log prefix: the bottom speaker's lines stay as they were; the earpiece's say which chip they are about. */
static const char *
amp_tag(const struct amp *a)
{
	return a->tas2564 ? "amp 0x4d" : "amp";
}

/* A register written and read back (page as it stands); false, and said, if either fails or it reads otherwise. */
static bool
amp_set(struct amp *a, const char *what, uint8_t reg, uint8_t value)
{
	uint8_t back = 0xff;

	if (!register_write(a->addr, reg, value) || !register_read(a->addr, reg, &back) || back != value) {
		say("vayu-audio: 4: %s: %s (%#x <- %#x): %s (reads %#x)\n", amp_tag(a), what, reg, value, back == 0xff ? "NO ANSWER" :
		    "DOES NOT READ BACK", back);
		return false;
	}
	say("vayu-audio: 4: %s: %s: %#x <- %#x, reads back\n", amp_tag(a), what, reg, value);
	return true;
}

static uint8_t
amp_get(struct amp *a, uint8_t reg)
{
	uint8_t value = 0xff;

	register_read(a->addr, reg, &value);
	return value;
}

/* Book 0's page select (register 0x00), written and read back; false, and said, if it does not take. */
static bool
amp_page(struct amp *a, uint8_t page)
{
	if (register_write(a->addr, TAS_PAGE, page) && amp_get(a, TAS_PAGE) == page) {
		return true;
	}
	say("vayu-audio: 4: %s: page %#x not selected\n", amp_tag(a), page);
	return false;
}

/* A byte read, changed -- the bits of `clear_bits' cleared, the bits of `set_bits' set -- and written back through amp_set(). */
static bool
amp_patch(struct amp *a, const char *what, uint8_t reg, uint8_t clear_bits, uint8_t set_bits)
{
	uint8_t value = amp_get(a, reg);

	return amp_set(a, what, reg, (uint8_t)((value & ~clear_bits) | set_bits));
}

/* Four bytes written in one burst (as the DVC is) and read back; false, and said, if either fails or they read otherwise. */
static bool
amp_burst4(struct amp *a, const char *what, uint8_t reg, const uint8_t *values)
{
	uint8_t back[4];
	bool same = true;

	if (!register_write_burst(a->addr, reg, values, 4)) {
		say("vayu-audio: 4: %s: %s (%#x <- %02x%02x%02x%02x): NOT WRITTEN\n", amp_tag(a), what, reg, values[0], values[1],
		    values[2], values[3]);
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		back[i] = amp_get(a, (uint8_t)(reg + i));
		same = same && back[i] == values[i];
	}
	say("vayu-audio: 4: %s: %s (%#x, one burst): %02x%02x%02x%02x, reads %02x%02x%02x%02x%s\n", amp_tag(a), what, reg, values[0],
	    values[1], values[2], values[3], back[0], back[1], back[2], back[3], same ? "" : ": DOES NOT READ BACK");
	return same;
}

/* A test-page register (book 0 page 0xfd) written; its read-back is not documented, so a value that reads otherwise is only said. */
static bool
amp_test_write(struct amp *a, const char *what, uint8_t reg, uint8_t value)
{
	if (!register_write(a->addr, reg, value)) {
		say("vayu-audio: 4: %s: %s (page 0xfd, %#x <- %#x): NOT WRITTEN\n", amp_tag(a), what, reg, value);
		return false;
	}
	say("vayu-audio: 4: %s: %s (page 0xfd, %#x <- %#x): reads %#x\n", amp_tag(a), what, reg, value, amp_get(a, reg));
	return true;
}

/* SDZ of an amplifier (GPIO 92 or 89), driven, pull kept (this port's narrow exception). */
static void
amp_sdz(struct amp *a, bool high)
{
	uint32_t cfg = rd(gpio_cfg(a->sdz_pin));

	crumb("4: about to drive gpio%u (SDZ of %#x) %s: cfg %#x -> %#x", a->sdz_pin, a->addr, high ? "high" : "low", cfg,
	    cfg | CFG_OUTPUT);
	wr(gpio_io(a->sdz_pin), high ? 2 : 0);
	wr(gpio_cfg(a->sdz_pin), cfg | CFG_OUTPUT);
	a->sdz_raised = high;
	crumb("4: gpio%u: cfg %#x io %#x", a->sdz_pin, rd(gpio_cfg(a->sdz_pin)), rd(gpio_io(a->sdz_pin)));
}

/* Out of hardware shutdown, reset, set up as Android's driver sets it up; left shut down (software). */
static bool
amp_prepare(struct amp *a)
{
	uint8_t id = 0xff, value;
	uint32_t index = a->tas2564 ? ear_dvc_index : dvc_index;
	bool raised = false, lim_ready = false;

	a->prepared = false;
	say("vayu-audio: 4: %s: vendor power-up writes %s (vayu_amp_vendor)\n", amp_tag(a), amp_vendor ? "on" : "off");
	if (!pin_high(a->sdz_pin)) {
		a->live = true;
		amp_sdz(a, true);
		raised = true;
		spin_us(20000);
	}
	a->live = true;
	if (!register_read(a->addr, TAS_CHIPID, &id) || (a->tas2564 ? id != 0x00 : (id != 0x10 && id != 0x20))) {
		say("vayu-audio: 4: %s: chip ID %#x at %#x is not %s's: nothing written\n", amp_tag(a), id, a->addr,
		    a->tas2564 ? "TAS2564" : "TAS2562");
		if (a->tas2564) {
			if (raised) {
				amp_sdz(a, false);
			}
			a->live = false;
			a->prepared = false;
		}
		return false;
	}
	say("vayu-audio: 4: %s: %s (ID %#x); about to set it up (DVC index %u = %d dB)\n", amp_tag(a), chip_name(id), id, index,
	    2 * (int)index - 110);
	if (!register_write(a->addr, TAS_RESET, 1)) {
		say("vayu-audio: 4: %s: the software reset is not acknowledged\n", amp_tag(a));
		return false;
	}
	spin_us(20000);
	if (amp_get(a, TAS_PAGE) != 0 || amp_get(a, TAS_BOOK) != 0) {
		say("vayu-audio: 4: %s: after the reset not on book 0 page 0\n", amp_tag(a));
		return false;
	}
	/*
	 * TAS_TDM2 [5:4]: 2 = mono right, what 0x4c has always had; 1 = mono left for 0x4d
	 * (TAS256X_TDMCONFIGURATIONREG2_RXSCFG54_MONO_LEFT, tas256x.h:116; the value
	 * tas256x_update_rx_cfg() gives rx_cfg = 1, tas256x.c:1528-1556).
	 */
	if (!amp_set(a, "power: I/V sense off, shut down", TAS_POWER, TAS_POWER_OFF) ||
	    !amp_set(a, "misc (Android's 0xcf)", TAS_MISC, 0xcf) ||
	    (value = amp_get(a, TAS_TDM0), !amp_set(a, "I2S: 48 kHz", TAS_TDM0, (uint8_t)((value & ~0x2e) | 0x08))) ||
	    (value = amp_get(a, TAS_TDM1), !amp_set(a, "I2S: falling edge, offset 1", TAS_TDM1, (uint8_t)((value & ~0x3f) | 0x02))) ||
	    (value = amp_get(a, TAS_TDM2), !amp_set(a, a->tas2564 ? "I2S: 16-bit words and slots, the left channel" :
	    "I2S: 16-bit words and slots, the right channel", TAS_TDM2,
	    (uint8_t)((value & ~0x3f) | (a->tas2564 ? 0x10 : 0x20)))) ||
	    !amp_set(a, "clock configuration (Android's 0x0c)", TAS_CLOCK_CONFIG, 0x0c) ||
	    (value = amp_get(a, TAS_INT_CONFIG), !amp_set(a, "clock halt timer 838 ms", TAS_INT_CONFIG, (uint8_t)(value | 0x38))) ||
	    !amp_set(a, "interrupts masked", TAS_MASK0, 0xff) || !amp_set(a, "interrupts masked", TAS_MASK1, 0xff)) {
		return false;
	}
	if (amp_vendor) {
		/*
		 * The vendor's power-up writes that this driver was missing, page 0 and book 0:
		 *   0x03 bit 6 = 1: the DC blocker off (tas256x.h:50-53 DCBLOCKER_DISABLED;
		 *       tas256x.c:994-1004 tas256x_HPF_FF_Bypass)
		 *   0x12 bit 0 = 0: the limiter off (tas256x.h:352-354; tas256x.c:1327-1340;
		 *       lim_switch defaults to 0 at tas256x.c:272)
		 *   0x14: (value & ~0x12) | 0x01: brown-out protection on, its mute and shutdown
		 *       off (tas256x.h:366-372; defaults bop_enable 1, bop_mute 0, bosd_enable 0
		 *       at tas256x.c:277-279; the setters at tas256x.c:1412-1440)
		 *   0x0a: the TX configuration for I/V data: the vendor writes 0x11 for the right
		 *       channel and 0xf1 for the left (tas256x.c:949 and :953; the register at
		 *       tas256x.h:130; 0x4c is the right channel, vayu-sm8150.dtsi:341-342).
		 */
		if (!amp_patch(a, "DC blocker off", TAS_DCBLOCKER, 0, 0x40) ||
		    !amp_patch(a, "limiter off", TAS_LIMITER, 0x01, 0) ||
		    !amp_patch(a, "brown-out protection on, mute and shutdown off", TAS_BOP, 0x12, 0x01) ||
		    !amp_set(a, a->tas2564 ? "TX: I/V data, the left channel" : "TX: I/V data, the right channel", TAS_TDM4,
		    a->tas2564 ? 0xf1 : 0x11)) {
			return false;
		}
		if (!amp_page(a, TAS_ICN_PAGE)) {
			amp_page(a, 0);
			return false;
		}
		/* The ICN configuration (tas256x.c:873-884, called from probe at tas256x-codec.c:1006); its read-back on this
		 * page is not documented, so a value that reads otherwise is only said. */
		register_write(a->addr, TAS_ICN_PAGE_CONFIG, 0x0d);
		say("vayu-audio: 4: %s: ICN test page configuration (page 0xfd, 0x0d <- 0x0d): reads %#x\n", amp_tag(a),
		    amp_get(a, TAS_ICN_PAGE_CONFIG));
		register_write(a->addr, TAS_CLASSD_CONFIG1, 0x80);
		say("vayu-audio: 4: %s: class-D configuration 1 (page 0xfd, 0x19 <- 0x80): reads %#x\n", amp_tag(a),
		    amp_get(a, TAS_CLASSD_CONFIG1));
		if (!amp_page(a, 0)) {
			return false;
		}
	}
	if (!register_write(a->addr, TAS_PAGE, 2) || amp_get(a, TAS_PAGE) != 2) {
		say("vayu-audio: 4: %s: page 2 not selected\n", amp_tag(a));
		register_write(a->addr, TAS_PAGE, 0);
		return false;
	}
	{
		uint8_t before[4], after[4];
		bool same = true;

		for (unsigned i = 0; i < 4; i++) {
			before[i] = amp_get(a, (uint8_t)(TAS_DVC + i));
		}
		if (!register_write_burst(a->addr, TAS_DVC, dvc_table[index], 4)) {
			register_write(a->addr, TAS_PAGE, 0);
			return false;
		}
		for (unsigned i = 0; i < 4; i++) {
			after[i] = amp_get(a, (uint8_t)(TAS_DVC + i));
			same = same && after[i] == dvc_table[index][i];
		}
		say("vayu-audio: 4: %s: digital volume (page 2, 0x0c-0x0f, one burst): %02x%02x%02x%02x -> %02x%02x%02x%02x, reads "
		    "%02x%02x%02x%02x%s\n", amp_tag(a), before[0], before[1], before[2], before[3], dvc_table[index][0],
		    dvc_table[index][1], dvc_table[index][2], dvc_table[index][3], after[0], after[1], after[2], after[3], same ? "" :
		    ": DOES NOT READ BACK");
		if (!same) {
			register_write(a->addr, TAS_PAGE, 0);
			return false;
		}
		if (amp_vendor) {
			/* The ICN threshold and hysteresis for -80 dB (tas256x.c:857-871, values at 12-13; tas256x.h:315-316). */
			if (!amp_burst4(a, "ICN threshold, -80 dB", TAS_ICN_THRESHOLD, icn_threshold) ||
			    !amp_burst4(a, "ICN hysteresis, -80 dB", TAS_ICN_HYSTERESIS, icn_hysteresis)) {
				amp_page(a, 0);
				return false;
			}
		}
		if (!a->tas2564 && amp_limiter) {
			static const char *const names[5] = { "limiter max attenuation -9 dB", "limiter threshold max 9 V",
				                              "limiter threshold min 4 V", "limiter inflection 3.3 V",
				                              "limiter tracking slope 1 V/V" };

			lim_ready = true;
			for (unsigned i = 0; i < 5 && lim_ready; i++) {
				lim_ready = amp_burst4(a, names[i], (uint8_t)(0x14 + 4 * i), lim_params[i]);
			}
			if (!lim_ready) {
				say("vayu-audio: 4: %s: the limiter's parameters did not take: it stays off\n", amp_tag(a));
			}
		}
	}
	if (!register_write(a->addr, TAS_PAGE, 0) || amp_get(a, TAS_PAGE) != 0) {
		say("vayu-audio: 4: %s: not back on page 0\n", amp_tag(a));
		return false;
	}
	if (!a->tas2564) {
		say("vayu-audio: 4: %s: as found: playback 0x03 %#x, boost 0x34 %#x, boost current 0x40 %#x, VBAT filter 0x3b %#x, "
		    "limiter 0x12 %#x 0x13 %#x\n", amp_tag(a), amp_get(a, 0x03), amp_get(a, 0x34), amp_get(a, 0x40), amp_get(a, 0x3b),
		    amp_get(a, 0x12), amp_get(a, 0x13));
		if (amp_boost && (!amp_patch(a, "boost max voltage (the vendor's 0xa + 1)", 0x34, 0x0f, 0x0b) ||
		    !amp_patch(a, "boost current limit (the vendor's 0x36)", 0x40, 0x3f, 0x36))) {
			say("vayu-audio: 4: %s: the boost settings did not take (played as the chip has them)\n", amp_tag(a));
		}
		if (amp_vendor) {
			amp_patch(a, "VBAT filter (the vendor's 2)", 0x3b, 0x60, 0x40);
		}
		if (amp_limiter && lim_ready) {
			if (!amp_patch(a, "limiter release rate 6, step 1", 0x13, 0xf8, 0x30 | 0x40) ||
			    !amp_patch(a, "limiter attack rate 1, step 1, ON", TAS_LIMITER, 0x3f, 0x02 | 0x10 | 0x01)) {
				amp_patch(a, "limiter off again", TAS_LIMITER, 0x01, 0);
			}
		}
	}
	if (a->tas2564) {
		/*
		 * The TAS2564's receiver mode, after the common set-up, as tas2564_rx_mode_update(p, 1, ch)
		 * does it (tas256x.c:1098-1157): PLAYBACKCONFIGURATIONREG0 bit 7 = 1 (RX mode,
		 * tas2564.h:46-48), then the test page unlocked and the five writes of that function's
		 * rx_mode branch (registers at tas256x.h:314-324, 383-387): DAC modulator 0xc0, ICN
		 * improvement 0x1f, class-D configuration 2 0x08, misc configuration 0xca, ICN switch 0.
		 * Then the amplifier level, 16 dBV = PLAYBACKCONFIGURATIONREG0 bits [5:1] = 0x0d
		 * (tas2564.h:42-43; tas256x.c:906-911).
		 *
		 * The boost voltage is NOT written: the vendor's TAS2564 write (tas256x.c:1295-1303) is
		 * left out and the chip's default kept. No other write in the common set-up is marked
		 * TAS2562-only in the vendor source; the one that is, the boost voltage
		 * (tas256x.c:1289-1294, TAS2562 branch), is not made for either chip.
		 */
		if (!amp_patch(a, "receiver (RX) mode", TAS_PLAYBACK0, 0, 0x80)) {
			return false;
		}
		if (!amp_page(a, TAS_ICN_PAGE)) {
			return false;
		}
		if (!amp_test_write(a, "test page unlocked", TAS_TEST_PAGE_LOCK, 0x0d) ||
		    !amp_test_write(a, "DAC modulator dither minimum", TAS_DAC_MODULATOR, 0xc0) ||
		    !amp_test_write(a, "ICN improvement", TAS_ICN_IMPROVE, 0x1f) ||
		    !amp_test_write(a, "class-D configuration 2", TAS_CLASSD_CONFIG2, 0x08)) {
			amp_page(a, 0);
			return false;
		}
		if (!amp_page(a, 0) ||
		    !amp_set(a, "misc configuration (0xca, RX mode)", TAS_MISC, 0xca) ||
		    !amp_set(a, "ICN switch off", TAS_ICN_SW, 0x00)) {
			return false;
		}
		if (!amp_patch(a, "amplifier level 16 dBV", TAS_PLAYBACK0, 0x3e, 0x1a)) {
			return false;
		}
	}
	register_write(a->addr, TAS_INT_CONFIG, (uint8_t)(amp_get(a, TAS_INT_CONFIG) | 0x04));     /* the latched faults cleared */
	say("vayu-audio: 4: %s: set up, shut down; latched %#x %#x\n", amp_tag(a), amp_get(a, TAS_LATCHED0), amp_get(a, TAS_LATCHED1));
	a->prepared = true;
	return true;
}

/* A fault latched, or no answer, in one chip: true, and said with the chip's name. */
static bool
amp_fault(struct amp *a)
{
	uint8_t latched0 = amp_get(a, TAS_LATCHED0), latched1 = amp_get(a, TAS_LATCHED1);

	if (latched0 == 0xff || latched1 == 0xff || (latched0 & TAS_FAULTS0) || (latched1 & TAS_FAULTS1)) {
		if (a == &amp_bottom && !(amp_ear_enabled && amp_ear.prepared && amp_ear.live)) {
			say("vayu-audio: 4: amp: A FAULT (latched %#x %#x: over-temperature %u, over-current %u, clock error %u, brownout %u, "
			    "UVLO %u, OVLO %u) or no answer: shut down at once\n", latched0, latched1, latched0 & 1, latched0 >> 1 & 1,
			    latched0 >> 2 & 1, latched1 >> 1 & 1, latched1 >> 2 & 1, latched1 >> 3 & 1);
		} else {
			say("vayu-audio: 4: %s: A FAULT in %s at %#x (latched %#x %#x: over-temperature %u, over-current %u, clock error %u, "
			    "brownout %u, UVLO %u, OVLO %u) or no answer: shut down at once\n", amp_tag(a), a->name, a->addr, latched0,
			    latched1, latched0 & 1, latched0 >> 1 & 1, latched0 >> 2 & 1, latched1 >> 1 & 1, latched1 >> 2 & 1,
			    latched1 >> 3 & 1);
		}
		return true;
	}
	return false;
}

/* No fault latched in either chip that is on; on one, shut down both at once (software, then SDZ). */
static bool
amp_healthy(void)
{
	bool bottom = amp_bottom.live, ear = amp_ear_enabled && amp_ear.prepared && amp_ear.live;
	bool fault = false;

	if (bottom && amp_fault(&amp_bottom)) {
		fault = true;
	}
	if (ear && amp_fault(&amp_ear)) {
		fault = true;
	}
	if (!fault) {
		return true;
	}
	if (bottom) {
		register_write(amp_bottom.addr, TAS_POWER, TAS_POWER_OFF);
		amp_sdz(&amp_bottom, false);
		amp_bottom.live = false;
		amp_bottom.prepared = false;
	}
	if (ear) {
		register_write(amp_ear.addr, TAS_POWER, TAS_POWER_OFF);
		amp_sdz(&amp_ear, false);
		amp_ear.live = false;
		amp_ear.prepared = false;
	}
	if (bottom && ear) {
		say("vayu-audio: 4: amp: both shut down at once (TAS2562 0x4c, TAS2564 0x4d)\n");
	}
	return false;
}

static bool
amp_start(struct amp *a)
{
	if (!amp_set(a, amp_vendor ? "power: active (I/V sense on)" : "power: active (I/V sense off)", TAS_POWER,
	    amp_vendor ? TAS_POWER_ON_IV : TAS_POWER_ON)) {
		return false;
	}
	spin_us(20000);
	if (amp_vendor) {
		uint8_t value;

		/* The 20 ms are spent; comparator hysteresis on before the output plays (tas256x.c:314-325: tas256x_set_power_up();
		 * tas256x.h:389-391 MISC_CLASSD, CMP_HYST bit 3). */
		if (!amp_page(a, 1) ||
		    (value = amp_get(a, TAS_MISC_CLASSD), !amp_set(a, "comparator hysteresis on", TAS_MISC_CLASSD,
		    (uint8_t)(value | TAS_CMP_HYST))) || !amp_page(a, 0)) {
			say("vayu-audio: 4: %s: comparator hysteresis not set: shut down at once\n", amp_tag(a));
			amp_page(a, 0);
			register_write(a->addr, TAS_POWER, TAS_POWER_OFF);
			amp_sdz(a, false);
			a->live = false;
			a->prepared = false;
			return false;
		}
	}
	return true;
}

static void
amp_stop(struct amp *a)
{
	if (!a->live) {
		return;
	}
	if (amp_vendor) {
		uint8_t value;

		/* Comparator hysteresis off before the shutdown write, as the vendor does in
		 * tas256x_set_power_shutdown() (tas256x.c:350-362); best effort -- whatever happens here,
		 * the shutdown below is made. */
		if (amp_page(a, 1)) {
			value = amp_get(a, TAS_MISC_CLASSD);
			amp_set(a, "comparator hysteresis off", TAS_MISC_CLASSD, (uint8_t)(value & ~TAS_CMP_HYST));
		}
		amp_page(a, 0);
	}
	register_write(a->addr, TAS_POWER, TAS_POWER_OFF);
	spin_us(20000);
	say("vayu-audio: 4: %s: shut down (power %#x, latched %#x %#x)\n", amp_tag(a), amp_get(a, TAS_POWER),
	    amp_get(a, TAS_LATCHED0), amp_get(a, TAS_LATCHED1));
	amp_sdz(a, false);
	a->live = false;
	a->prepared = false;
}

/* Either amplifier still prepared or powered: for the play loop and its final stop. */
static bool
amp_any_live(void)
{
	return amp_bottom.live || (amp_ear_enabled && amp_ear.live);
}

/* ---- the test tone ---- */

/* 1 kHz at 48 kHz, -30 dBFS (a peak of 1036): one cycle */
static const int16_t tone[48] = {
	0, 135, 268, 396, 518, 631, 733, 822, 897, 957, 1001, 1027,
	1036, 1027, 1001, 957, 897, 822, 733, 631, 518, 396, 268, 135,
	0, -135, -268, -396, -518, -631, -733, -822, -897, -957, -1001, -1027,
	-1036, -1027, -1001, -957, -897, -822, -733, -631, -518, -396, -268, -135,
};
static uint32_t tone_at;

static void
period_fill(unsigned period)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);

	for (unsigned frame = 0; frame < PERIOD_FRAMES; frame++) {
		int16_t sample = tone[tone_at++ % 48];

		samples[2 * frame] = sample;
		samples[2 * frame + 1] = sample;
	}
	for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

static bool
period_write(unsigned period)
{
	uint32_t write[8] = { (uint32_t)(buffer_address + 4096 * period), (uint32_t)((buffer_address + 4096 * period) >> 32),
		              map_handle, PERIOD_BYTES, period, 0, 0, 0 };

	return apr_post(APR_SVC_ASM, STREAM_PORT, WRITE_TOKEN | period, ASM_DATA_CMD_WRITE_V2, write, sizeof(write));
}

/* The tone for `ms', period after period as the DSP gives them back; the amplifier started after the first ones come back. */
static bool
play(uint32_t ms, bool amplifier)
{
	uint64_t began, last_look, now;
	uint32_t sent = 0, done_before = written_done, looks = 0;
	bool ok = true, tone_over = false, amp_on = false;

	returned = 0;
	tone_at = 0;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill(p);
	}
	began = last_look = mach_absolute_time();
	for (unsigned p = 0; p < PERIODS; p++) {
		if (!period_write(p)) {
			say("vayu-audio: 4: the write of period %u could not be sent\n", p);
			return false;
		}
		sent++;
	}
	i2c_quiet = true;
	for (;;) {
		uint32_t back = returned;
		uint64_t elapsed_ns;

		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - began, &elapsed_ns);
		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			for (unsigned p = 0; p < PERIODS && !tone_over; p++) {
				if (!(back & 1u << p)) {
					continue;
				}
				period_fill(p);
				if (!period_write(p)) {
					say("vayu-audio: 4: the write of period %u could not be sent\n", p);
					ok = false;
					tone_over = true;
					break;
				}
				sent++;
			}
		}
		if (amplifier && !amp_on && !tone_over && written_done - done_before >= 3) {
			crumb("4: the DSP takes the tone (%u periods back): the amplifier, powered up", written_done - done_before);
			amp_on = true;
			if (!amp_start(&amp_bottom) || (amp_ear_enabled && amp_ear.prepared && !amp_start(&amp_ear)) || !amp_healthy()) {
				ok = false;
				tone_over = true;
			}
		}
		if (!tone_over && (elapsed_ns >= ms * NSEC_PER_MSEC)) {
			tone_over = true;
			if (amp_on) {
				amp_stop(&amp_bottom);
				amp_stop(&amp_ear);
			}
		}
		if (tone_over && written_done - done_before >= sent) {
			break;
		}
		if (elapsed_ns > ms * NSEC_PER_MSEC + 2 * NSEC_PER_SEC) {
			say("vayu-audio: 4: THE DSP HAS NOT GIVEN EVERY PERIOD BACK 2 s after the end (%u of %u)\n",
			    written_done - done_before, sent);
			ok = false;
			break;
		}
		if (written_done == done_before && elapsed_ns > NSEC_PER_SEC) {
			say("vayu-audio: 4: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND\n");
			ok = false;
			break;
		}
		if (amp_on && amp_any_live()) {
			uint64_t since_ns;

			absolutetime_to_nanoseconds(now - last_look, &since_ns);
			if (since_ns >= 100 * NSEC_PER_MSEC) {
				last_look = now;
				looks++;
				if (!amp_healthy()) {
					ok = false;
					tone_over = true;
				}
			}
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	i2c_quiet = false;
	if (amp_any_live()) {
		amp_stop(&amp_bottom);
		amp_stop(&amp_ear);
	}
	{
		uint32_t done = written_done - done_before;
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		say("vayu-audio: 4: %u periods written, %u given back (%u with an error), %u underflows, in %llu ms: the DSP took %llu "
		    "frames a second%s\n", sent, done, write_errors, underflows, ns / NSEC_PER_MSEC,
		    ns ? (uint64_t)done * PERIOD_FRAMES * NSEC_PER_SEC / ns : 0,
		    amplifier ? "" : " (the amplifier off: nothing to hear)");
		if (amplifier) {
			say("vayu-audio: 4: the amplifier's faults looked at %u times while it played\n", looks);
		}
	}
	return ok;
}

/* ---- vayu_sound=4: what iOS plays (hakios.sound.c's sound=4 feeder, the amplifiers off) ---- */

/*
 * /dev/hakios-audio's writer (the audio server's real-time thread, or sound_test raw; hakios.audio.c)
 * puts frames into a ring through ios_sink() and never waits; the feeder (this step's thread) fills
 * each period the DSP gives back from the ring, and with silence when it is empty, so the stream never
 * stops while it runs. With rec_sound=N rec_sound_fed=1 the recording ("xl sound") has what was fed.
 * The amplifiers are not touched at this level: it shows that iOS's samples reach the DSP whole.
 */
#define RING_FRAMES             32768           /* 0.68 s; a power of two, the indices run on and wrap */

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;
static uint64_t ios_frames, fed_frames, silent_frames;
static volatile uint32_t ios_peak, fed_peak;
/* The levels of the last second, for the feeder's per-second line (tester, 2026-10-25: music distorted when loud): what iOS
 * wrote, and what goes to the DSP after the guard; "full" counts samples at full scale (+-32767 or -32768). */
static volatile uint32_t sec_ios_peak, sec_ios_full, sec_out_peak, sec_out_full;
static volatile uint64_t last_sound;            /* mach_absolute_time() of the latest write that was not silence */
static bool guarded;                            /* vayu_sound=5: every period goes through hakios.guard.c */
#define SILENCE                 8               /* samples no larger than this are silence (as hakios.audio.c's) */

/* From the writer's thread, under hakios.audio.c's lock: never waits. */
static uint32_t
ios_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t left = frames[2 * i], right = frames[2 * i + 1];
		uint32_t l = left < 0 ? (uint32_t)-(int32_t)left : (uint32_t)left, r = right < 0 ? (uint32_t)-(int32_t)right : (uint32_t)right;

		ring[2 * ((head + i) % RING_FRAMES)] = left;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = right;
		sound |= l > SILENCE || r > SILENCE;
		if (l > ios_peak || r > ios_peak) {
			ios_peak = l > r ? l : r;
		}
		if (l > sec_ios_peak || r > sec_ios_peak) {
			sec_ios_peak = l > r ? l : r;
		}
		sec_ios_full += (l >= 32767) + (r >= 32767);
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	ios_frames += taken;
	if (sound) {
		last_sound = mach_absolute_time();
	}
	return taken;
}

/* dB under full scale of a peak, rounded down to 1 dB (96 for silence): integer only, as the kernel has no floating point */
static unsigned
under_full_scale(uint32_t peak)
{
	unsigned db = 0;
	uint64_t v = peak;

	if (peak == 0) {
		return 96;
	}
	while (v < 32767 && db < 96) {
		v = v * 1122 / 1000;                    /* (1 dB) */
		db++;
	}
	return db;
}

static void
period_fill_ios(unsigned period)
{
	int16_t *samples = (int16_t *)(buffer + 4096 * period);
	uint32_t tail = ring_tail, waiting = ring_head - tail, n = waiting < PERIOD_FRAMES ? waiting : PERIOD_FRAMES;

	for (uint32_t i = 0; i < n; i++) {
		int16_t left = ring[2 * ((tail + i) % RING_FRAMES)], right = ring[2 * ((tail + i) % RING_FRAMES) + 1];
		uint32_t l = left < 0 ? (uint32_t)-(int32_t)left : (uint32_t)left, r = right < 0 ? (uint32_t)-(int32_t)right : (uint32_t)right;

		samples[2 * i] = left;
		samples[2 * i + 1] = right;
		if (l > fed_peak || r > fed_peak) {
			fed_peak = l > r ? l : r;
		}
	}
	for (uint32_t i = n; i < PERIOD_FRAMES; i++) {
		samples[2 * i] = samples[2 * i + 1] = 0;
	}
	OSMemoryBarrier();
	ring_tail = tail + n;
	fed_frames += n;
	silent_frames += PERIOD_FRAMES - n;
	if (guarded) {
		hakios_guard_process(samples, PERIOD_FRAMES);  /* (silence too: its filter and averages go on) */
	}
	for (uint32_t i = 0; i < 2 * n; i++) {          /* (after the guard: what the DSP and the amplifier get) */
		uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

		sec_out_peak = v > sec_out_peak ? v : sec_out_peak;
		sec_out_full += v >= 32767;
	}
	if (n != 0 && hakios_audio_recording_fed()) {
		hakios_audio_record_fed(samples, n);
	}
	for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

/*
 * vayu_sound=5: the watcher (hakios.sound.c's watch(), in the feeder's loop). The amplifier on while
 * there is sound -- started 100 ms after a write that is not silence comes, faded in through its digital
 * volume from -110 dB to the level, stopped (faded out, software shutdown) after three seconds of
 * silence -- at the level vayu_audio_dvc less iOS's volume (hakios_audio_volume(), in 2 dB steps), off
 * while iOS's volume is 0; its faults read every 100 ms (any: off for the rest of the run, SDZ low). The
 * guard (hakios.guard.c) reckons with the amplifier's level under Android's (DVC 0 dB): (55 - index) x 16
 * eighths of a dB. Unless vayu_audio_amp=1 (with vayu_audio_xnu_tlmm=1) the amplifier itself is never
 * spoken to: the watcher only says what it would do -- a run that needs nobody next to the phone.
 */
#define QUIET_BEFORE_OFF_NS     (3ull * NSEC_PER_SEC)

static struct {
	bool amp;                       /* vayu_audio_amp=1: the amplifier is really driven */
	bool on, prepared, given_up, muted;
	uint32_t index, target, starts, changes, generation, below;
	uint64_t last_look;
} watcher;

/* The digital volume, one burst (page 2 and back), no readback: for the fade. */
static bool
amp_dvc(struct amp *a, uint32_t index)
{
	bool ok = register_write(a->addr, TAS_PAGE, 2) && register_write_burst(a->addr, TAS_DVC, dvc_table[index], 4);

	register_write(a->addr, TAS_PAGE, 0);
	return ok;
}

static uint32_t
watch_target(void)
{
	uint32_t steps = (watcher.below + 15) / 16;     /* iOS's attenuation, eighths of a dB, as 2 dB steps (rounded down in level) */

	return steps >= dvc_index ? 0 : dvc_index - steps;
}

static void
watch_level(uint32_t index)
{
	hakios_guard_attenuation((55 - index) * 16);
}

static void
watch_fade(uint32_t to)
{
	if (audio_route_jack && vayu_jack_keep()) {
		return;                         /* the dynamic route is on the headphones: the speaker amplifier is not ours to touch */
	}
	while (watcher.index != to) {
		watcher.index += watcher.index < to ? 1 : -1;
		if (watcher.amp && amp_ear_enabled && amp_ear.live && amp_ear.prepared) {
			/* the earpiece follows the fade, never above its own index (a failed write: it is shut down) */
			if (!amp_dvc(&amp_ear, watcher.index < ear_dvc_index ? watcher.index : ear_dvc_index)) {
				say("vayu-audio: 5: the earpiece's fade write failed at index %u: the earpiece is shut down\n", watcher.index);
				amp_stop(&amp_ear);
			}
		}
		if (watcher.amp && !amp_dvc(&amp_bottom, watcher.index)) {
			say("vayu-audio: 5: the fade's volume write failed at index %u: the amplifier is given up on\n", watcher.index);
			watcher.given_up = true;
			return;
		}
		spin_us(1000);
	}
}

static void
watch_stop(const char *why, bool give_up)
{
	crumb("5: %s: the amplifier %s", why, watcher.amp ? "is faded out and shut down" : "WOULD be faded out and shut down");
	if (!(audio_route_jack && vayu_jack_keep()) && watcher.amp && watcher.prepared && !watcher.given_up) {
		watch_fade(0);
		register_write(amp_bottom.addr, TAS_POWER, TAS_POWER_OFF);
		if (amp_ear_enabled && amp_ear.live) {
			register_write(amp_ear.addr, TAS_POWER, TAS_POWER_OFF);
		}
	}
	if (give_up && watcher.amp) {
		if (!(audio_route_jack && vayu_jack_keep())) {
			amp_stop(&amp_bottom);
			amp_stop(&amp_ear);
		}
		watcher.given_up = true;
	}
	watcher.on = false;
}

/* The amplifier up, as a stream start (and the way back from the jack) brings it: prepared if needed, started, faded in. */
static bool
watch_amp_up(const char *why)
{
	if (audio_route_jack && vayu_jack_keep()) {
		return false;                   /* (the dynamic route is on the headphones: never touch the speaker amplifier) */
	}
	watcher.starts++;
	crumb("5: %s: the amplifier %s, faded in to index %u (%d dB)", why, watcher.amp ? "is started" : "WOULD be started",
	    watcher.target, 2 * (int)watcher.target - 110);
	if (watcher.amp) {
		if (!watcher.prepared) {
			uint32_t keep = dvc_index;

			dvc_index = 0;                  /* (set up at -110 dB, then faded in) */
			watcher.prepared = amp_prepare(&amp_bottom);
			dvc_index = keep;
			watcher.index = 0;
			if (watcher.prepared && amp_ear_enabled) {
				uint32_t ear_keep = ear_dvc_index;

				ear_dvc_index = 0;      /* (set up at -110 dB too, then faded in with the bottom speaker) */
				amp_prepare(&amp_ear);    /* a failure leaves the bottom speaker alone */
				ear_dvc_index = ear_keep;
			}
		}
		if (!watcher.prepared || !amp_start(&amp_bottom) || (amp_ear_enabled && amp_ear.prepared && !amp_start(&amp_ear)) ||
		    !amp_healthy()) {
			say("vayu-audio: 5: THE AMPLIFIER DID NOT START; iOS's sound goes on to the DSP, unheard\n");
			amp_stop(&amp_bottom);
			amp_stop(&amp_ear);
			watcher.given_up = true;
			return false;
		}
	}
	watcher.on = true;
	watch_fade(watcher.target);
	watch_level(watcher.target);
	watcher.last_look = mach_absolute_time();
	return true;
}

/* The watcher's "there is sound now" test (its stream-start condition), for the way back from the jack. */
static bool
watch_amp_may_start(void)
{
	uint64_t since_sound = 0;

	if (watcher.given_up || watcher.muted || last_sound == 0) {
		return false;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - last_sound, &since_sound);
	return since_sound < 100 * NSEC_PER_MSEC;
}

static void
watch(void)
{
	uint64_t now = mach_absolute_time(), since_sound = 0, since_look;
	uint32_t below, generation;
	bool muted;

	if (last_sound != 0) {
		absolutetime_to_nanoseconds(now - last_sound, &since_sound);
	}
	if (hakios_audio_volume(&below, &muted, &generation) && generation != watcher.generation) {
		watcher.generation = generation;
		watcher.below = below;
		watcher.muted = muted;
		watcher.changes++;
		watcher.target = watch_target();
		say("vayu-audio: 5: iOS sets the volume: %s%u.%03u dB under the loudest: DVC index %u (%d dB)\n", muted ? "muted, " : "",
		    below / 8, below % 8 * 125, watcher.target, 2 * (int)watcher.target - 110);
		if (muted && watcher.on) {
			watch_stop("iOS's volume is 0", false);
		} else if (watcher.on && !watcher.given_up) {
			watch_fade(watcher.target);
			watch_level(watcher.target);
		}
	}
	if (!(audio_route_jack && vayu_jack_keep()) && !watcher.on && !watcher.given_up && !watcher.muted &&
	    last_sound != 0 && since_sound < 100 * NSEC_PER_MSEC) {
		ios_peak = 0;
		if (!watch_amp_up("iOS plays")) {
			return;
		}
	} else if (watcher.on && since_sound > QUIET_BEFORE_OFF_NS) {
		say("vayu-audio: 5: what iOS wrote while it was on peaked at %u%% of full scale (%u)\n", ios_peak * 100 / 32768, ios_peak);
		watch_stop("three seconds of silence", false);
	} else if (watcher.on && watcher.amp) {
		absolutetime_to_nanoseconds(now - watcher.last_look, &since_look);
		if (since_look >= 100 * NSEC_PER_MSEC) {
			watcher.last_look = now;
			if (!amp_healthy()) {
				watcher.on = false;
				watcher.given_up = true;
			}
		}
	}
}

/*
 * The route, make before break: the new port and path opened while the old one still plays, the session moved to the
 * new COPP, then only the old COPP and port taken down. A failure before the move goes back to the port still running;
 * after it, the new port is the one that runs and the old one is only a leak. `amp_was_on' is what the watcher had up
 * when the route left the speaker; the way back starts it exactly as a stream start would.
 */
static bool amp_was_on;

static bool
route_back_to_speaker(const char *why, uint32_t opened)
{
	if (opened != 0) {
		if (!adm_close(opened)) {
			say("vayu-audio: 5: route: (fallback) the new COPP %u did not close\n", opened);
		}
	}
	afe_port_stop(JACK_AFE_PORT);
	audio_route_jack = false;
	if (guarded) {
		hakios_guard_configure(true, speaker_gain);
	}
	say("vayu-audio: 5: route: guard gain %u/8 dB (speaker)\n", speaker_gain);
	if (amp_was_on) {
		bool up = false;

		if (watch_amp_may_start()) {
			up = watch_amp_up("the route falls back to the speaker");
		}
		amp_was_on = false;
		say("vayu-audio: 5: route: (fallback) the speaker amplifier %s\n", up ? "started again" :
		    watcher.given_up ? "did not come back; the watcher has given up on it" : "stays off (muted or no sound now)");
	}
	vayu_jack_set_route(0);
	port_id = PORT;
	if (mi2s_port_on) {
		say("vayu-audio: 5: route: (fallback) back to the speaker (%s); the headphones' port is down\n", why);
		return true;
	}
	say("vayu-audio: 5: route: (fallback) no port left (%s)\n", why);
	return false;
}

static bool
route_back_to_jack(const char *why, uint32_t opened)
{
	if (opened != 0) {
		if (!adm_close(opened)) {
			say("vayu-audio: 5: route: (fallback) the new COPP %u did not close\n", opened);
		}
	}
	afe_port_stop(PORT);
	audio_route_jack = true;
	vayu_jack_set_route(1);
	port_id = JACK_AFE_PORT;
	if (jack_port_on) {
		say("vayu-audio: 5: route: (fallback) back to the headphones (%s); the speaker's port is down\n", why);
		return true;
	}
	say("vayu-audio: 5: route: (fallback) no port left (%s)\n", why);
	return false;
}

static bool
route_switch(bool to_jack)
{
	uint32_t old_copp = copp, copp2 = 0;

	if (to_jack == audio_route_jack) {
		return to_jack ? jack_port_on : mi2s_port_on;
	}
	say("vayu-audio: 5: route: %s -> %s\n", audio_route_jack ? "headphones" : "speaker", to_jack ? "headphones" : "speaker");
	if (to_jack) {
		amp_was_on = watcher.on;
		if (amp_was_on) {
			watch_stop("the route goes to the headphones", false);
		}
		say("vayu-audio: 5: route: (a) the speaker amplifier %s\n", amp_was_on ? (watcher.amp ? "faded out and shut down" :
		    "would be faded out and shut down (not driven)") : "was not on");
		if (!afe_port_start_slimbus_rx()) {
			say("vayu-audio: 5: route: (b) the port 0x400c did not start\n");
			return route_back_to_speaker("the port 0x400c did not start", copp2);
		}
		say("vayu-audio: 5: route: (b) the port 0x400c: started (SLIMbus, the codec)\n");
		if (!adm_open(JACK_AFE_PORT, &copp2)) {
			say("vayu-audio: 5: route: (c) no device path to 0x400c\n");
			return route_back_to_speaker("no device path to 0x400c", copp2);
		}
		say("vayu-audio: 5: route: (c) a device path to 0x400c: COPP %u\n", copp2);
		if (!matrix_route(copp2)) {
			say("vayu-audio: 5: route: (d) session 1 did not take COPP %u\n", copp2);
			return route_back_to_speaker("the session did not take the new COPP", copp2);
		}
		say("vayu-audio: 5: route: (d) session 1 routed to COPP %u (headphones)\n", copp2);
		if (!adm_close(old_copp)) {
			say("vayu-audio: 5: route: (e) the old COPP %u did not close; it stays open, unused\n", old_copp);
		} else {
			say("vayu-audio: 5: route: (e) the old COPP %u closed\n", old_copp);
		}
		copp = copp2;
		copp_open = true;
		if (!afe_port_stop(PORT)) {
			say("vayu-audio: 5: route: (f) the port 0x1006 did not stop; it stays on, unrouted\n");
		} else {
			say("vayu-audio: 5: route: (f) the port 0x1006: stopped (MI2S)\n");
		}
		audio_route_jack = true;
		if (guarded) {
			hakios_guard_configure(true, jack_gain);
		}
		say("vayu-audio: 5: route: guard gain %u/8 dB (headphones)\n", jack_gain);
		vayu_jack_set_route(1);
		port_id = JACK_AFE_PORT;
		route_switches++;
		say("vayu-audio: 5: route: the headphones (AFE 0x400c), %u switches\n", route_switches);
		return true;
	}

	if (!afe_port_start_mi2s()) {
		say("vayu-audio: 5: route: (a) the port 0x1006 did not start\n");
		return route_back_to_jack("the port 0x1006 did not start", copp2);
	}
	say("vayu-audio: 5: route: (a) the port 0x1006: started (MI2S, the speaker)\n");
	if (!adm_open(PORT, &copp2)) {
		say("vayu-audio: 5: route: (b) no device path to 0x1006\n");
		return route_back_to_jack("no device path to 0x1006", copp2);
	}
	say("vayu-audio: 5: route: (b) a device path to 0x1006: COPP %u\n", copp2);
	if (!matrix_route(copp2)) {
		say("vayu-audio: 5: route: (c) session 1 did not take COPP %u\n", copp2);
		return route_back_to_jack("the session did not take the new COPP", copp2);
	}
	say("vayu-audio: 5: route: (c) session 1 routed to COPP %u (speaker)\n", copp2);
	if (!adm_close(old_copp)) {
		say("vayu-audio: 5: route: (d) the old COPP %u did not close; it stays open, unused\n", old_copp);
	} else {
		say("vayu-audio: 5: route: (d) the old COPP %u closed\n", old_copp);
	}
	copp = copp2;
	copp_open = true;
	if (!afe_port_stop(JACK_AFE_PORT)) {
		say("vayu-audio: 5: route: (e) the port 0x400c did not stop; it stays on, unrouted\n");
	} else {
		say("vayu-audio: 5: route: (e) the port 0x400c: stopped (the codec)\n");
	}
	audio_route_jack = false;
	if (guarded) {
		hakios_guard_configure(true, speaker_gain);
	}
	say("vayu-audio: 5: route: guard gain %u/8 dB (speaker)\n", speaker_gain);
	if (amp_was_on) {
		bool up = false;

		if (watch_amp_may_start()) {
			up = watch_amp_up("the route comes back to the speaker");
		}
		amp_was_on = false;
		say("vayu-audio: 5: route: (g) the speaker amplifier %s\n", up ? (watcher.amp ? "started again" :
		    "would be started again (not driven)") : watcher.given_up ? "did not come back; the watcher has given up on it" :
		    "stays off (muted or no sound now)");
	} else {
		say("vayu-audio: 5: route: (g) the speaker amplifier stays off (it was not on)\n");
	}
	vayu_jack_set_route(0);
	port_id = PORT;
	route_switches++;
	say("vayu-audio: 5: route: the speaker (AFE 0x1006), %u switches\n", route_switches);
	return true;
}

static bool
feed(uint32_t secs)
{
	uint64_t began = mach_absolute_time(), last_report = began, last_level = began, last_route = began, last_switch = began, ns;
	uint32_t sent = 0, done_before = written_done, said = 0, levels_said = 0, limited_before = hakios_guard_limited();
	bool ok = true;

	ring_head = ring_tail = 0;
	ios_frames = fed_frames = silent_frames = 0;
	ios_peak = fed_peak = 0;
	sec_ios_peak = sec_ios_full = sec_out_peak = sec_out_full = 0;
	returned = 0;
	hakios_audio_set_sink("the bottom speaker's stream (the audio DSP, vayu_audio.c; amplifiers off)", ios_sink);
	crumb("4: the feeder runs for %u s: /dev/hakios-audio -> ring -> ASM periods (amplifiers off)", secs);
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill_ios(p);
		if (!period_write(p)) {
			ok = false;
			break;
		}
		sent++;
	}
	while (ok) {
		uint32_t back = returned;

		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			for (unsigned p = 0; p < PERIODS; p++) {
				if (!(back & 1u << p)) {
					continue;
				}
				period_fill_ios(p);
				if (!period_write(p)) {
					say("vayu-audio: 4: the write of period %u could not be sent\n", p);
					ok = false;
					break;
				}
				sent++;
			}
		}
		if (guarded) {
			watch();
		}
		if (guarded && vayu_jack_keep()) {
			uint64_t since_route;

			absolutetime_to_nanoseconds(mach_absolute_time() - last_route, &since_route);
			if (since_route >= 100 * NSEC_PER_MSEC) {
				bool wanted = vayu_jack_state() == 1;

				last_route = mach_absolute_time();
				if (wanted != audio_route_jack) {
					uint64_t since_switch;

					absolutetime_to_nanoseconds(last_route - last_switch, &since_switch);
					if (since_switch >= 500 * NSEC_PER_MSEC) {        /* (on top of the MBHC's 100 ms) */
						last_switch = last_route;
						if (!route_switch(wanted)) {
							say("vayu-audio: 5: route: no port is left to feed; the feeder stops\n");
							ok = false;
							break;
						}
					}
				}
			}
		}
		if (ns >= (uint64_t)secs * NSEC_PER_SEC) {
			break;
		}
		if (written_done == done_before && ns > NSEC_PER_SEC) {
			say("vayu-audio: 4: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND\n");
			ok = false;
			break;
		}
		{
			uint64_t since_level;

			absolutetime_to_nanoseconds(mach_absolute_time() - last_level, &since_level);
			if (since_level >= NSEC_PER_SEC) {
				uint32_t ip = sec_ios_peak, iff = sec_ios_full, op = sec_out_peak, of = sec_out_full;
				uint32_t limited = guarded ? hakios_guard_limited() : 0, lim_sec = limited - limited_before;

				limited_before = limited;

				last_level = mach_absolute_time();
				sec_ios_peak = sec_ios_full = sec_out_peak = sec_out_full = 0;
				/* every second with sound for the first 10 minutes of sound, then every 10th; silence is not said */
				if (ip > SILENCE && (levels_said < 600 || levels_said % 10 == 0) && ++levels_said) {
					say("vayu-audio: 4: level +%llu s: iOS peak %u (-%u dBFS), %u at full scale; to the DSP peak %u (-%u dBFS), "
					    "%u at full scale; the guard's peak limiter in %u frames (%u%%); amplifier DVC index %u\n", ns / NSEC_PER_SEC, ip,
					    under_full_scale(ip), iff, op, under_full_scale(op), of, lim_sec, lim_sec / 480, watcher.index);
				} else if (ip > SILENCE) {
					levels_said++;
				}
			}
		}
		{
			uint64_t since;

			absolutetime_to_nanoseconds(mach_absolute_time() - last_report, &since);
			if (since >= (said < 120 ? 5 : 60) * NSEC_PER_SEC && ++said) {        /* (every 5 s for 10 min, then every minute: the whole run) */
				last_report = mach_absolute_time();
				say("vayu-audio: 4: feeder +%llu s: iOS wrote %llu frames (peak %u), fed %llu, silence %llu frames; DSP gave back %u of "
				    "%u periods, %u underflows, %u errors, route %s, %u switches\n", ns / NSEC_PER_SEC, ios_frames, ios_peak,
				    fed_frames, silent_frames, written_done - done_before, sent, underflows, write_errors,
				    audio_route_jack ? "headphones" : "speaker", route_switches);
			}
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	if (guarded) {
		struct hakios_guard_report report;

		if (watcher.on) {
			watch_stop("the feeder ends", false);
		}
		hakios_guard_take_report(&report);
		hakios_guard_attenuation(HAKIOS_GUARD_SILENT);
		say("vayu-audio: 5: the watcher: the amplifier %s %u times, iOS set the volume %u times%s; the guard: %u blocks, held down "
		    "%u/%u/%u (0.2 s/3 s/60 s averages), peak limiter in %u frames, peak in %u out %u\n", watcher.amp ? "started" :
		    "would have started", watcher.starts, watcher.changes, watcher.given_up ? ", then given up on" : "", report.blocks,
		    report.rms_blocks, report.fast_blocks, report.slow_blocks, report.limited_frames, report.peak_in, report.peak_out);
	}
	hakios_audio_set_sink("nothing", NULL);
	for (unsigned ms = 0; ms < 2000 && written_done - done_before < sent; ms += 2) {
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	say("vayu-audio: 4: feeder over after %llu ms: iOS wrote %llu frames (peak %u), fed %llu (peak %u), silence %llu; %u periods written, "
	    "%u given back (%u with an error), %u underflows: the DSP took %llu frames a second\n", ns / NSEC_PER_MSEC, ios_frames,
	    ios_peak, fed_frames, fed_peak, silent_frames, sent, written_done - done_before, write_errors, underflows,
	    ns ? (uint64_t)(written_done - done_before) * PERIOD_FRAMES * NSEC_PER_SEC / ns : 0);
	return ok;
}

/* ---- vayu_sound=6: the handset microphones, 3 s, levels per channel (MIC_TX.md 4 and 5) --------------- */

static uint32_t
isqrt64(uint64_t value)                 /* no floating point: integer square root, one bit at a time */
{
	uint64_t root = 0, bit = 1ull << 62;

	while (bit > value) {
		bit >>= 2;
	}
	while (bit != 0) {
		if (value >= root + bit) {
			value -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)root;
}

/* The buffer and the DSP's view of it, as the playback path sets them up (space_find, buffer, space_on). */
static bool
mic_buffer(void)
{
	bool ok;

	lck_mtx_lock(&hakios_smmu_lock);
	ok = space_find();
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!ok) {
		return false;
	}
	if (buffer == 0 && kmem_alloc(kernel_map, &buffer, BUFFER_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		buffer = 0;
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	ok = space.passed || space_on();
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!ok) {
		return false;
	}
	buffer_address = space.passed ? (kvtophys(buffer) | AUDIO_STREAM_BITS) : (AUDIO_ADDRESSES | AUDIO_STREAM_BITS);
	return true;
}

/* AFE SLIMBUS_0_TX: 0x4001, the codec's TX7 and TX8 as channels 135 and 136 (msm-dai-q6-v2.c:2606-2641). */
static bool
mic_afe_on(void)
{
	struct afe_slimbus slim = { .minor = 1, .dev_id = 0 /* AFE_SLIMBUS_DEVICE_1 */, .bit_width = 16,
		               .data_format = 0, .num_channels = 2, .shared_ch = { 135, 136 }, .sample_rate = RATE };
	uint16_t start[2] = { MIC_AFE_PORT, 0 };

	if (!afe_port_param("the port 0x4001: SLIMbus, 2 channels 135/136, 16 bits, 48 kHz", MIC_AFE_PORT,
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_SLIMBUS_CONFIG, &slim, sizeof(slim))) {
		return false;
	}
	mic_port_on = command("the port 0x4001, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0);
	return mic_port_on;
}

/* ASM: our periods mapped, session 2 opened for reading as 2 channels of 16-bit PCM at 48 kHz. */
static bool
mic_asm_on(void)
{
	struct {
		uint16_t pool, regions;
		uint32_t property, low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)buffer_address, (uint32_t)(buffer_address >> 32),
		                          BUFFER_BYTES };
	struct {
		uint32_t mode, endpoint, topology, format;
		uint16_t bits, reserved;
	} __attribute__((packed)) reading = { 0, 0 /* ASM_END_POINT_DEVICE_MATRIX */, ASM_TOPOLOGY_NONE,
		                                ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2, 16, 0 };
	struct {
		uint32_t param, size, frames_per_buffer, block_size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) encoder = { ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2, 28, 0, 20, 2, 16, RATE, 1, 0,
		                               { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };

	if (!command("our periods, mapped for the DSP (the microphones)", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map,
	    sizeof(map), ASM_CMDRSP_SHARED_MEM_MAP_REGIONS)) {
		return false;
	}
	mic_mapped = true;
	mic_map_handle = apr_answer.words[0];
	say("vayu-audio: 6: our %u KB at %#llx are the DSP's handle %#x (the microphones)\n", BUFFER_BYTES / 1024,
	    buffer_address, mic_map_handle);
	if (!command("the stream (session 2), opened for reading: 2 channels of 16-bit PCM, no processing", APR_SVC_ASM,
	    MIC_STREAM_PORT, ASM_STREAM_CMD_OPEN_READ_V3, &reading, sizeof(reading), 0)) {
		return false;
	}
	mic_stream_open = true;
	return command("the stream's format: 2 channels of 16 bits at 48 kHz", APR_SVC_ASM, MIC_STREAM_PORT,
	           ASM_STREAM_CMD_SET_ENCDEC_PARAM, &encoder, sizeof(encoder), 0);
}

/* ADM: a live-recording device path from 0x4001, routed to session 2 on the AUDIO_TX matrix. */
static bool
mic_adm_on(void)
{
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_LIVE_RECORDING, MIC_AFE_PORT, 0xffff, ADM_TOPOLOGY_NONE, 2, 16, RATE,
		                           { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_TX, 1, MIC_SESSION, 1, { 0, 0 } };

	if (!command("a device path from the port: live recording, no processing, 2 channels of 16 bits at 48 kHz",
	    APR_SVC_ADM, MIC_AFE_PORT, ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), ADM_CMDRSP_DEVICE_OPEN_V5)) {
		return false;
	}
	if (apr_answer.words[0] != 0) {
		say("vayu-audio: 6: the device path: REFUSED: %s\n", dsp_error(apr_answer.words[0]));
		return false;
	}
	mic_copp_open = true;
	mic_copp = apr_answer.words[1] & 0xffff;
	route.copp[0] = (uint16_t)mic_copp;
	say("vayu-audio: 6: the device path is open: COPP %u\n", mic_copp);
	return command("session 2 routed to the device path (the microphones)", APR_SVC_ADM, 0,
	           ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route), 0);
}

static bool
mic_run(void)
{
	uint32_t run[3] = { 0, 0, 0 };

	return command("the session (session 2), run", APR_SVC_ASM, MIC_STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0);
}

/* One 20 ms period: its own 4096-byte page, the DSP address mapped above, not waited for. */
static bool
mic_read(uint32_t period)
{
	uint64_t address = buffer_address + (uint64_t)4096 * period;
	uint32_t read[5] = { (uint32_t)address, (uint32_t)(address >> 32), mic_map_handle, MIC_PERIOD_BYTES, period };

	return apr_post(APR_SVC_ASM, MIC_STREAM_PORT, MIC_READ_TOKEN | period, ASM_DATA_CMD_READ_V2, read, sizeof(read));
}

/* The reads, the levels of each second, and the overall peaks. */
static bool
mic_listen(uint32_t secs)
{
	uint64_t began = mach_absolute_time(), last_event = began, last_data = began, squares[2] = { 0, 0 };
	uint32_t seconds = 0, periods = 0, frames = 0, peaks[2] = { 0, 0 }, all_peaks[2] = { 0, 0 }, seen = 0;
	bool samples_said = false, ok = true;

	mic_read_ready = 0;
	mic_reads_done = 0;
	mic_read_errors = 0;
	mic_seconds = 0;
	mic_peaks[0] = mic_peaks[1] = 0;
	for (unsigned p = 0; p < MIC_PERIODS; p++) {
		mic_read_sizes[p] = 0;
	}
	for (unsigned p = 0; p < MIC_PERIODS; p++) {
		if (!mic_read(p)) {
			say("vayu-audio: 6: a read of 20 ms could not be given to the DSP (period %u)\n", p);
			return false;
		}
	}
	say("vayu-audio: 6: reading: %u reads of 20 ms (3840 bytes, one page each), for %u s\n", MIC_PERIODS, secs);
	for (;;) {
		uint64_t now = mach_absolute_time(), since_event, since_data, elapsed;
		uint32_t done = mic_reads_done;

		if (done != seen) {
			seen = done;
			last_event = now;
		}
		absolutetime_to_nanoseconds(now - last_event, &since_event);
		absolutetime_to_nanoseconds(now - last_data, &since_data);
		if (since_event >= 500 * NSEC_PER_MSEC || since_data >= 500 * NSEC_PER_MSEC) {
			say("vayu-audio: 6: NO DATA FROM THE DSP (%llu ms without a read done, %llu ms without samples)\n",
			    since_event / NSEC_PER_MSEC, since_data / NSEC_PER_MSEC);
			ok = false;
			break;
		}
		absolutetime_to_nanoseconds(now - began, &elapsed);
		if (elapsed >= ((uint64_t)secs + 2) * NSEC_PER_SEC) {       /* (one period lost must not hang the thread) */
			say("vayu-audio: 6: the reading stops: %u s of data in %llu ms (wanted %u s)\n", seconds,
			    elapsed / NSEC_PER_MSEC, secs);
			ok = false;
			break;
		}
		for (unsigned p = 0; p < MIC_PERIODS && ok; p++) {
			uint32_t size, got;

			if (!(mic_read_ready & (1u << p))) {
				continue;
			}
			OSBitAndAtomic(~(1u << p), &mic_read_ready);
			size = mic_read_sizes[p];
			{
				int16_t *samples = (int16_t *)(buffer + 4096 * p);

				for (vm_offset_t line = (vm_offset_t)samples; line < (vm_offset_t)samples + MIC_PERIOD_BYTES; line += 64) {
					__asm__ volatile ("dc ivac, %0" : : "r" (line));
				}
				__asm__ volatile ("dsb sy");
				got = size / 4;                  /* 2 channels of 16 bits a frame */
				got = got > MIC_PERIOD_FRAMES ? MIC_PERIOD_FRAMES : got;
				if (got != 0) {
					if (!samples_said) {
						char line[80];
						int at = 0;
						uint32_t n = got * 2 < 8 ? got * 2 : 8;

						for (uint32_t i = 0; i < n && at < (int)sizeof(line) - 6; i++) {
							at += snprintf(line + at, sizeof(line) - (size_t)at, "%s%04x", i ? " " : "",
							    (unsigned)(uint16_t)samples[i]);
						}
						samples_said = true;
						say("vayu-audio: 6: the first period's first %u samples: %s\n", n, line);
					}
					for (uint32_t i = 0; i < got; i++) {
						int32_t l = samples[2 * i], r = samples[2 * i + 1];
						uint32_t al = l < 0 ? (uint32_t)-l : (uint32_t)l;
						uint32_t ar = r < 0 ? (uint32_t)-r : (uint32_t)r;

						squares[0] += (uint64_t)((int64_t)l * l);
						squares[1] += (uint64_t)((int64_t)r * r);
						peaks[0] = al > peaks[0] ? al : peaks[0];
						peaks[1] = ar > peaks[1] ? ar : peaks[1];
						all_peaks[0] = al > all_peaks[0] ? al : all_peaks[0];
						all_peaks[1] = ar > all_peaks[1] ? ar : all_peaks[1];
					}
					frames += got;
					last_data = now;
				}
			}
			periods++;
			if (!mic_read(p)) {
				say("vayu-audio: 6: the read of period %u could not be given back to the DSP\n", p);
				ok = false;
			}
		}
		if (!ok) {
			break;
		}
		if (frames >= RATE) {
			uint32_t rms[2];

			rms[0] = isqrt64(squares[0] / frames);
			rms[1] = isqrt64(squares[1] / frames);
			seconds++;
			say("vayu-audio: 6: +%us mic L(ch135, AMIC3/DEC7) peak %d rms %d, R(ch136, AMIC1/DEC8) peak %d rms %d, "
			    "%u periods, %u errors\n", seconds, (int)peaks[0], (int)rms[0], (int)peaks[1], (int)rms[1], periods,
			    mic_read_errors);
			frames = 0;
			periods = 0;
			peaks[0] = peaks[1] = 0;
			squares[0] = squares[1] = 0;
			if (seconds >= secs) {
				break;
			}
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	mic_seconds = seconds;
	mic_peaks[0] = all_peaks[0];
	mic_peaks[1] = all_peaks[1];
	return ok && seconds >= secs;
}

/* Everything the microphones' path took down, in reverse; idempotent. */
static void
mic_off(void)
{
	uint32_t handle = mic_map_handle;
	uint16_t stop[2] = { MIC_AFE_PORT, 0 };

	if (mic_stream_open) {
		command("the stream (session 2), closed", APR_SVC_ASM, MIC_STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0);
		mic_stream_open = false;
	}
	if (mic_copp_open) {
		command("the device path, closed", APR_SVC_ADM, (uint16_t)mic_copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0);
		mic_copp_open = false;
	}
	vayu_jack_mic_off();
	if (mic_port_on) {
		command("the port 0x4001, stopped", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0);
		mic_port_on = false;
	}
	if (mic_mapped) {
		command("our periods, unmapped (the microphones)", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &handle,
		    sizeof(handle), 0);
		mic_mapped = false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	space_off();
	lck_mtx_unlock(&hakios_smmu_lock);
}

static bool
step_mic(void)
{
	uint32_t secs = 3;
	const char *failed = NULL;
	bool ok;

	PE_parse_boot_argn("vayu_mic_secs", &secs, sizeof(secs));
	secs = secs < 1 ? 1 : secs;
	secs = secs > 30 ? 30 : secs;
	command_step = 6;
	crumb("6: port 0x4001, the microphones and a %u s recording", secs);
	if (!vayu_jack_ready()) {
		say("vayu-audio: 6: needs vayu_jack=2 (the codec): not done\n");
		command_step = 4;
		return false;
	}
	if (!mic_buffer()) {
		say("vayu-audio: 6: 1/8 the buffer and the SMMU: FAILED\n");
		failed = "the buffer and the SMMU";
	} else {
		say("vayu-audio: 6: 1/8 the buffer and the SMMU: %u KB at %#llx%s\n", BUFFER_BYTES / 1024, buffer_address,
		    space.passed ? " (the DSP's stream 0x1b21 already matches)" : " (a context of ours)");
	}
	if (failed == NULL) {
		if (!mic_afe_on()) {
			say("vayu-audio: 6: 2/8 the AFE port 0x4001: FAILED\n");
			failed = "the AFE port 0x4001";
		} else {
			say("vayu-audio: 6: 2/8 the AFE port 0x4001: SLIMbus, 2 channels 135/136, 16 bits, 48 kHz, started\n");
		}
	}
	if (failed == NULL) {
		if (!vayu_jack_mic_on()) {
			say("vayu-audio: 6: 3/8 the codec's microphone path: FAILED\n");
			failed = "the codec's microphone path";
		} else {
			say("vayu-audio: 6: 3/8 the codec's microphone path: TX7/TX8 on channels 135/136, on\n");
		}
	}
	if (failed == NULL) {
		if (!mic_asm_on()) {
			say("vayu-audio: 6: 4/8 the ASM stream (session 2): FAILED\n");
			failed = "the ASM stream";
		} else {
			say("vayu-audio: 6: 4/8 the ASM stream (session 2): mapped, opened for reading, 2 channels of 16 bits at 48 kHz\n");
		}
	}
	if (failed == NULL) {
		if (!mic_adm_on()) {
			say("vayu-audio: 6: 5/8 the ADM path: FAILED\n");
			failed = "the ADM path";
		} else {
			say("vayu-audio: 6: 5/8 the ADM path: live recording from 0x4001, COPP %u, routed to session 2\n", mic_copp);
		}
	}
	if (failed == NULL) {
		if (!mic_run()) {
			say("vayu-audio: 6: 6/8 the session, run: FAILED\n");
			failed = "the run";
		} else {
			say("vayu-audio: 6: 6/8 the session (session 2), run\n");
		}
	}
	if (failed == NULL) {
		if (!mic_listen(secs)) {
			say("vayu-audio: 6: 7/8 the reading: FAILED after %u s\n", mic_seconds);
			failed = "the reading";
		} else {
			say("vayu-audio: 6: 7/8 the reading: %u s of data, %u errors\n", mic_seconds, mic_read_errors);
		}
	}
	say("vayu-audio: 6: 8/8 taking it down, in reverse\n");
	mic_off();
	command_step = 4;
	ok = failed == NULL && mic_seconds >= secs;
	if (ok) {
		say("vayu-audio: 6: DONE: %u s, peaks L %d R %d\n", mic_seconds, (int)mic_peaks[0], (int)mic_peaks[1]);
	} else {
		say("vayu-audio: 6: FAILED at %s\n", failed != NULL ? failed : "the reading");
	}
	return ok;
}

/* vayu_sound=1|2|3|6 (see the top of this section); everything taken down again, in reverse. */
static bool
step_sound(uint32_t sound, uint32_t secs)
{
	bool ok;

	if (sound == 6) {
		return step_mic();
	}
	crumb("4: vayu_sound=%u: %s", sound, sound == 5 ? "port, stream, what iOS plays, the guard and the watcher" :
	    sound == 4 ? "port, stream and what iOS plays; the amplifiers off" :
	    sound == 3 ? (amp_ear_enabled ? "port, stream, tone and both amplifiers" : "port, stream, tone and the bottom speaker's amplifier") :
	    sound == 2 ? "port, stream and tone; the amplifiers off" : "the port only");
	if (sound == 3 && !xnu_tlmm) {
		say("vayu-audio: 4: vayu_sound=3 needs vayu_audio_xnu_tlmm=1 (the amplifier is spoken to over I2C by hand): not done\n");
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	ok = space_find();
	lck_mtx_unlock(&hakios_smmu_lock);
	if (sound >= 2 && !ok) {
		return false;
	}
	if (sound == 3) {
		if (!amp_prepare(&amp_bottom)) {
			amp_stop(&amp_bottom);
			return false;
		}
		if (amp_ear_enabled) {
			amp_prepare(&amp_ear);           /* a failure leaves the bottom speaker alone */
		}
	}
	ok = afe_on();
	if (ok && sound == 1) {
		delay_for_interval(100, NSEC_PER_MSEC);
		pin_activity();
		delay_for_interval(secs, NSEC_PER_SEC);
	} else if (ok) {
		if (buffer == 0 && kmem_alloc(kernel_map, &buffer, BUFFER_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			buffer = 0;
			ok = false;
		}
		lck_mtx_lock(&hakios_smmu_lock);
		/*
		 * The free match register and context found above are found again under this same hold of the lock: in a
		 * boot where vayu_camera's smmu_up runs too (a combined kernel), it may have taken them in between.
		 */
		ok = ok && (space.passed || (space_find() && space_on()));
		lck_mtx_unlock(&hakios_smmu_lock);
		if (ok) {
			buffer_address = space.passed ? (kvtophys(buffer) | AUDIO_STREAM_BITS) : (AUDIO_ADDRESSES | AUDIO_STREAM_BITS);
			ok = stream_on();
		}
		if (ok) {
			pin_activity();
			if (sound == 5) {
				uint32_t gain = speaker_gain, amp = 0, release = 11, guard_gain;

				PE_parse_boot_argn("vayu_sound_gain", &gain, sizeof(gain));
				speaker_gain = gain > HAKIOS_GUARD_MAX_GAIN ? HAKIOS_GUARD_MAX_GAIN : gain;
				PE_parse_boot_argn("vayu_guard_release", &release, sizeof(release));    /* 11 (43 ms) .. 14 (341 ms) */
				hakios_guard_set_release(release);
				PE_parse_boot_argn("vayu_audio_amp", &amp, sizeof(amp));
				bzero(&watcher, sizeof(watcher));
				amp_was_on = false;
				route_switches = 0;
				watcher.amp = amp != 0 && xnu_tlmm != 0;
				watcher.target = dvc_index;
				guard_gain = (audio_route_jack && vayu_jack_keep()) ? jack_gain : speaker_gain;
				hakios_guard_configure(true, guard_gain);
				watch_level(dvc_index);
				guarded = true;
				if (audio_route_jack && vayu_jack_keep()) {
					say("vayu-audio: 5: route: guard gain %u/8 dB (headphones)\n", jack_gain);
				}
				crumb("5: the watcher: the amplifier %s (vayu_audio_amp=%u, vayu_audio_xnu_tlmm=%u); level DVC index %u (%d dB), "
				    "guard gain %u/8 dB, peak release shift %u", watcher.amp ? "IS DRIVEN" : "is NOT driven, only what would be done is said", amp,
				    xnu_tlmm, dvc_index, 2 * (int)dvc_index - 110, guard_gain, release);
			}
			ok = sound >= 4 ? feed(secs) : play(secs * 1000, sound == 3);
			guarded = false;
		}
		stream_off();
		lck_mtx_lock(&hakios_smmu_lock);
		space_off();
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	if (sound != 5 || !audio_route_jack || !vayu_jack_keep()) {      /* (on the dynamic jack route the watcher has already shut it down) */
		amp_stop(&amp_bottom);
		amp_stop(&amp_ear);
	}
	afe_off();
	crumb("4: vayu_sound=%u %s; everything taken down", sound, ok ? "DONE" : "FAILED");
	return ok;
}

/* ---- the steps -------------------------------------------------------------------------------------- */

static bool
step_look(void)
{
	say("vayu-audio: 1: reads: pins, shared memory%s\n", want_i2c ? ", the amplifiers' ID registers" : "");
	show_pins();
	if (!vayu_soc_check("vayu-audio: 1")) {
		return false;
	}
	vayu_pas_survey(&adsp);
	if (want_i2c && !xnu_tlmm) {
		/* (this port's rule: XNU does not write TLMM; I2C by hand is nothing but TLMM writes) */
		say("vayu-audio: 1: the amplifiers are not asked from here: I2C by hand writes TLMM, which XNU does not (the "
		    "loader reads their IDs with vayu_audio_pins=1 vayu_audio_ids=1; vayu_audio_xnu_tlmm=1 overrides)\n");
		crumb("amplifiers: SDZ gpio%u cfg %#x io %#x, gpio%u cfg %#x io %#x (read only)", PIN_SDZ_RIGHT,
		    rd(gpio_cfg(PIN_SDZ_RIGHT)), rd(gpio_io(PIN_SDZ_RIGHT)), PIN_SDZ_LEFT, rd(gpio_cfg(PIN_SDZ_LEFT)),
		    rd(gpio_io(PIN_SDZ_LEFT)));
	} else if (want_i2c) {
		ask_amplifiers();
	}
	return true;
}

/* The DSP loaded and started, its GLINK items made first; up for vayu_audio_secs at step 2. */
static bool
step_dsp(void)
{
	uint32_t wait_s = 10, state;

	PE_parse_boot_argn("vayu_audio_wait", &wait_s, sizeof(wait_s));
	say("vayu-audio: 2: the audio DSP: TrustZone asked, image checked, given to TrustZone, written; GLINK's items made; "
	    "started; watched for %u s\n", wait_s);
	if (!vayu_pas_supported(&adsp)) {
		say("vayu-audio: 2: FAILED: TrustZone does not answer IS_CALL_AVAILABLE either way\n");
		return false;
	}
	if (!vayu_pas_load(&adsp)) {
		say("vayu-audio: 2: FAILED: not loaded\n");
		return false;
	}
	if (!vayu_glink_prepare(&glink)) {
		say("vayu-audio: 2: FAILED: GLINK's items not made (the image is written but not started)\n");
		return false;
	}
	if (!vayu_pas_start(&adsp, wait_s * 1000)) {
		say("vayu-audio: 2: FAILED: it did not come up\n");
		vayu_pas_stop(&adsp, "it did not come up");
		return false;
	}
	state = vayu_pas_state(&adsp);
	say("vayu-audio: 2: THE AUDIO DSP IS UP (\"slave-kernel\" %#x)\n", state);
	return !(state & VAYU_RPROC_FATAL);
}

/* GLINK taken up, QRTR's hello and services, APR's core service: the audio services up, and their versions. */
static bool
step_link(void)
{
	uint32_t listen_s = 3, apr_wait_s = 10, asked = 0, state = 0;
	uint64_t began;
	bool up;

	PE_parse_boot_argn("vayu_audio_listen", &listen_s, sizeof(listen_s));
	PE_parse_boot_argn("vayu_audio_apr_wait", &apr_wait_s, sizeof(apr_wait_s));
	say("vayu-audio: 3: GLINK with the audio DSP, QRTR on IPCRTR (%u s), APR on apr_audio_svc\n", listen_s);
	if (!vayu_glink_up(&glink, 5000)) {
		say("vayu-audio: 3: FAILED: the channel IPCRTR is not open\n");
		return false;
	}
	delay_for_interval(listen_s, NSEC_PER_SEC);
	say("vayu-audio: 3: after %u s: %s; the DSP is node %u with %u services; it looked for %u of ours; QRTR in %u, out %u\n",
	    listen_s, qrtr.hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr.peer, vayu_qrtr_services(&qrtr), qrtr.lookups,
	    qrtr.in, qrtr.out);
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		say("vayu-audio: 3: FAILED: the DSP has not opened apr_audio_svc\n");
		return false;
	}
	began = mach_absolute_time();
	for (;;) {
		uint64_t ns;

		asked++;
		if (apr_ask(APR_SVC_ADSP_CORE, 0, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 500) &&
		    apr_answer.opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE && (state = apr_answer.words[0]) == 1) {
			break;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		if (ns >= apr_wait_s * NSEC_PER_SEC) {
			break;
		}
		delay_for_interval(200, NSEC_PER_MSEC);
	}
	up = state == 1;
	say("vayu-audio: 3: %s after %u questions (the last answer: opcode %#x, %u bytes, %#x)\n", up ? "THE AUDIO SERVICES ARE UP" :
	    "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", asked, apr_answer.opcode, apr_answer.length, apr_answer.words[0]);
	if (apr_ask(APR_SVC_ADSP_CORE, 0, AVCS_GET_VERSIONS, NULL, 0, 1000)) {
		char line[400];
		int at = 0;

		for (uint32_t i = 0; i < apr_answer.length / 4 && i < 64 && at < (int)sizeof(line) - 12; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", apr_answer.words[i]);
		}
		say("vayu-audio: 3: the services and their versions (opcode %#x, %u bytes):%s\n", apr_answer.opcode, apr_answer.length,
		    line);
	} else {
		say("vayu-audio: 3: no answer to the question of the services' versions\n");
	}
	say("vayu-audio: 3: APR sent %u, received %u packets\n", apr_out, apr_in);
	if (up) {
		vayu_jack_run(&qrtr);                   /* boot-arg vayu_jack=N: the 3.5 mm jack's codec (vayu_jack.c) */
	}
	return up;
}

static void
audio_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t secs = 2, keep = 0;

	PE_parse_boot_argn("vayu_audio_secs", &secs, sizeof(secs));
	PE_parse_boot_argn("vayu_audio_keep", &keep, sizeof(keep));
	crumb_since = mach_absolute_time();
	north = ml_io_map(TLMM_NORTH_PHYS, TLMM_TILE_SIZE);
	south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_TILE_SIZE);
	if (north == 0 || south == 0) {
		say("vayu-audio: cannot map the TLMM tiles (north %#lx south %#lx)\n", north, south);
		thread_terminate(current_thread());
		return;
	}
	if (level >= 2) {
		/* (the firmware may be in /fw of the root, which is not mounted yet when this begins: a minute at most) */
		for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
			delay_for_interval(200, NSEC_PER_MSEC);
		}
	}
	if (!step_look()) {
		say("vayu-audio: 1: FAILED: not vayu's shared memory/command database; nothing more is done\n");
	} else if (level >= 2 && step_dsp()) {
		if (level >= 3 && !step_link()) {
			say("vayu-audio: 3: the steps after it are not attempted\n");
		} else if (level >= 4) {
			uint32_t sound = 1, tone_secs = secs, then = 0, pre[2] = { 0, 0 };

			PE_parse_boot_argn("vayu_sound", &sound, sizeof(sound));
			/*
			 * vayu_sound_pre=N, vayu_sound_pre2=M: short steps (1-3, 6) BEFORE the main one, e.g. the headphone tone and
			 * the microphones' recording ahead of the speaker feeder (vayu_sound=5) in a combined session. The jack's
			 * codec is then given back (vayu_jack_finish) so that the main step plays to the speaker, not the jack;
			 * with vayu_jack=3 the MBHC keeps polling, so the codec stays up (vayu_jack_keep).
			 */
			PE_parse_boot_argn("vayu_sound_pre", &pre[0], sizeof(pre[0]));
			PE_parse_boot_argn("vayu_sound_pre2", &pre[1], sizeof(pre[1]));
			for (unsigned i = 0; i < 2; i++) {
				if (pre[i] != 0 && (pre[i] <= 3 || pre[i] == 6)) {
					step_sound(pre[i], 3);
					delay_for_interval(1, NSEC_PER_SEC);
				}
			}
			if ((pre[0] != 0 || pre[1] != 0) && !vayu_jack_keep()) {
				vayu_jack_finish();
			}
			if (sound >= 4) {
				tone_secs = 40;                         /* the feeder: amplifiers off, any length */
				PE_parse_boot_argn("vayu_sound_secs", &tone_secs, sizeof(tone_secs));
				/* (a whole run: at most a day; it was 600 s, and Music and ringtones after ten minutes were silent, run 005616) */
				tone_secs = tone_secs > 86400 ? 86400 : tone_secs;
			} else if (tone_secs > 3) {
				tone_secs = 3;                          /* (no speaker protection: short) */
			}
			if (tone_secs == 0) {
				tone_secs = 1;
			}
			step_sound(sound, tone_secs);
			/* vayu_sound_then=N: a second step after the first, in the same boot (e.g. 2 = the headphone tone, then
			 * 6 = the microphones' recording); only the short steps 1-3 and 6 */
			if (PE_parse_boot_argn("vayu_sound_then", &then, sizeof(then)) && then != 0 && then != sound &&
			    (then <= 3 || then == 6)) {
				delay_for_interval(1, NSEC_PER_SEC);
				step_sound(then, tone_secs > 3 ? 3 : tone_secs);
			}
		}
		vayu_jack_finish();                     /* path off, MCLK off, the codec quiet (vayu_jack.c) */
		delay_for_interval(secs, NSEC_PER_SEC);
		say("vayu-audio: after %u s more its \"slave-kernel\" says %#x\n", secs, vayu_pas_state(&adsp));
		if (!keep) {
			vayu_pas_stop(&adsp, "the steps are over");
		} else {
			say("vayu-audio: the audio DSP stays on (vayu_audio_keep=1)\n");
		}
	}
	say("vayu-audio: done\n");
	thread_terminate(current_thread());
}

void
vayu_audio_quiesce(void)
{
	if (north != 0) {
		if (amp_bottom.live) {
			wr(gpio_io(PIN_SDZ_RIGHT), 0);  /* the speaker's amplifier into hardware shutdown, first of all */
		}
		if (amp_ear.live || amp_ear.sdz_raised) {
			wr(gpio_io(PIN_SDZ_LEFT), 0);   /* the earpiece's too, if it was set up or its SDZ raised here */
		}
	}
	vayu_pas_quiesce(&adsp);
	if (space.on) {
		/* (the DSP is down: the stream's SMMU entries put back as found, for the next kernel's step 4) */
		wr(smmu_base + SMMU_SMR(space.match), space.old_smr);
		wr(smmu_base + SMMU_S2CR(space.match), space.old_s2cr);
		wr(smmu_context + SMMU_CB_SCTLR, space.old_sctlr);
		space.on = false;
	}
	if (!sdz_driven || north == 0) {
		return;
	}
	wr(gpio_io(PIN_SDZ_LEFT), 0);
	wr(gpio_io(PIN_SDZ_RIGHT), 0);
}

void
vayu_audio_init(void)
{
	uint32_t cx = 0, pause = 2200, ear = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("vayu_audio", &level, sizeof(level)) || level == 0) {
		return;
	}
	PE_parse_boot_argn("vayu_audio_i2c", &want_i2c, sizeof(want_i2c));
	PE_parse_boot_argn("vayu_audio_xnu_tlmm", &xnu_tlmm, sizeof(xnu_tlmm));
	PE_parse_boot_argn("vayu_audio_dvc", &dvc_index, sizeof(dvc_index));
	PE_parse_boot_argn("vayu_jack_gain", &jack_gain, sizeof(jack_gain));
	jack_gain = jack_gain > JACK_GAIN_MAX ? JACK_GAIN_MAX : jack_gain;
	PE_parse_boot_argn("vayu_sound_gain", &speaker_gain, sizeof(speaker_gain));
	speaker_gain = speaker_gain > HAKIOS_GUARD_MAX_GAIN ? HAKIOS_GUARD_MAX_GAIN : speaker_gain;
	PE_parse_boot_argn("vayu_amp_vendor", &amp_vendor, sizeof(amp_vendor));
	PE_parse_boot_argn("vayu_amp_boost", &amp_boost, sizeof(amp_boost));
	PE_parse_boot_argn("vayu_amp_limiter", &amp_limiter, sizeof(amp_limiter));
	PE_parse_boot_argn("vayu_audio_dvc_max", &dvc_most, sizeof(dvc_most));
	PE_parse_boot_argn("vayu_audio_ear", &ear, sizeof(ear));
	PE_parse_boot_argn("vayu_audio_ear_dvc", &ear_dvc_index, sizeof(ear_dvc_index));
	amp_ear_enabled = ear != 0;
	ear_dvc_index = ear_dvc_index > EAR_DVC_TOP ? EAR_DVC_TOP : ear_dvc_index;
	dvc_most = dvc_most > DVC_TOP ? DVC_TOP : dvc_most;
	if (dvc_index > dvc_most) {
		dvc_index = dvc_most;                   /* -6 dB at the most, unless vayu_audio_dvc_max says more */
	}
	if (PE_parse_boot_argn("vayu_audio_cx", &cx, sizeof(cx)) && cx != 0) {
		adsp.rails[0] = "cx.lvl";
	}
	PE_parse_boot_argn("vayu_pause", &pause, sizeof(pause));
	vayu_pas_set_pause(pause);
	say("vayu-audio: vayu_audio=%u vayu_audio_i2c=%u vayu_audio_cx=%u vayu_pause=%u\n", level, want_i2c, cx, pause);
	if (amp_ear_enabled) {
		say("vayu-audio: vayu_audio_ear=1: the earpiece's TAS2564 at 0x4d, DVC index %u (%d dB)\n", ear_dvc_index,
		    2 * (int)ear_dvc_index - 110);
	}
	if (kernel_thread_start(audio_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
