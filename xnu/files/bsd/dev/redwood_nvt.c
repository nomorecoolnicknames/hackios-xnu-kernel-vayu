/*
 * The touch screen of the POCO X3 Pro (vayu): a Novatek NT36672C behind the SPI pins of
 * QUPv3 wrapper 2, serial engine 0 (the controller at 0x00c80000), as a source of touches
 * for /dev/redwood-touch (sys/redwood_input.h). Only with boot-args
 * touchscreen=1 touch_driver=nvt: every pin and address below belongs to this phone.
 *
 * This is the second touch driver in the kernel: redwood's Goodix GT9916/FocalTech FT3680
 * driver (bsd/dev/redwood_goodix.c) is for a different SoC and a different controller, and
 * redwood_input_init() starts that one unless touch_driver=nvt is given. QEMU and redwood
 * therefore behave exactly as before.
 *
 * There is no SPI, clock, pin or regulator framework in this kernel, and the boot loader
 * has no SPI or GPIO service either (its only shared services are the log, the storage and
 * the charger state). The existing Goodix driver talks to the SoC's registers directly
 * with ml_io_map(); this one does the same, but only through the pins:
 *
 *   pins    TLMM at 0x03000000 (SM8150; redwood's TLMM is at 0x0f100000)
 *           MISO gpio55, MOSI gpio56, SCLK gpio57, CS gpio58 -- the device tree's
 *           "qup17" group; reset gpio12, interrupt gpio122, I/O supply enable gpio100
 *   bus     the four pins driven by hand in SPI mode 0. The serial engine is left alone:
 *           its clocks are votes in GCC (QUPV3_WRAP2_S0 at 0x1e148, votes at 0x52014
 *           bits 1, 2 and 4) and that path is not needed to read a 65-byte report every
 *           8 ms. The pins are muxed away from "qup17" to plain GPIO first.
 *   chip    Novatek NT36672C, the controller the phone's own kernel drives as
 *           drivers/input/touchscreen/nt36672c (MiCode, branch vayu-r-oss). Its SPI
 *           protocol, from that driver (nt36xxx.c, CTP_SPI_READ/WRITE and nvt_set_page):
 *             write: TX[0] = register | 0x80, then the data
 *             read:  TX[0] = register & 0x7f, then zeros; the chip returns two dummy
 *                    bytes followed by the data
 *             page:  write { 0xff, address >> 15, address >> 7 } first; the low seven
 *                    bits of the address then select the register in the page
 *           The report is 10 records of 6 bytes at the event-buffer page:
 *             byte 0: [7:3] finger (1..10), [2:0] 1 = down, 2 = move
 *             bytes 1,2: x and y, high 8 bits; byte 3: low 4 bits of each
 *           Panel size and firmware version come from the firmware-info block at
 *           event-buffer + 0x78 (the vendor's nvt_get_fw_info()).
 *   power   the panel's I/O supply is a fixed regulator on gpio100, enable active high,
 *           marked regulator-boot-on with a 4 ms startup delay; reset is active low.
 *
 * No interrupt is used: the driver reads the event buffer every touch_poll_us
 * microseconds (8000 by default), whether or not the interrupt line moved; a report is
 * kept by the chip, so a missed edge is not a lost touch. touch_flip=N mirrors the
 * coordinates as in the Goodix driver. nvt_spi_us=N sets the bit-bang half period
 * (1 us by default, about 500 kHz).
 *
 * This file was written from the sources named above and has never run on the phone.
 * Code inspection says the addresses and the protocol are the vendor's; whether the
 * panel is already powered and out of reset when XNU starts, and whether the bit-banged
 * bus is fast enough for the report rate, are hypotheses for the hardware test.
 *
 * Sources: arch/arm64/boot/dts/vendor/qcom (vayu.dtb here), MiCode
 * Xiaomi_Kernel_OpenSource vayu-r-oss drivers/input/touchscreen/nt36672c/nt36xxx.c,
 * nt36xxx.h, nt36xxx_mem_map.h, and drivers/input/touchscreen/spi-xiaomi-tp.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x03000000UL    /* SM8150's pinctrl@03000000 */
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

#define PIN_MISO                55
#define PIN_MOSI                56
#define PIN_CLK                 57
#define PIN_CS                  58
#define PIN_RESET               12
#define PIN_INT                 122
#define PIN_VDDIO               100
#define PIN_FUNCTION_SPI        1       /* "qup17" is the first mux after gpio on 55-58 (not used: bit-banged) */

static vm_offset_t tlmm;

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
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- SPI, on the four pins, mode 0 ------------------------------------------------------------- */

static unsigned spi_half_us = 1;

static void
spi_delay(void)
{
	delay_for_interval(spi_half_us, NSEC_PER_USEC);
}

/* CS high, clock low, data out low: the idle state before the chip is powered. */
static void
spi_pins_idle(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(PIN_MOSI, false, PULL_NONE, 6);
	gpio_configure(PIN_MISO, 0, PULL_NONE, 2, false);
}

/* One transaction, chip selected throughout; rx may be NULL. */
static void
spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	gpio_set(PIN_CS, false);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_MOSI, out >> bit & 1);
			spi_delay();
			gpio_set(PIN_CLK, true);        /* the chip samples MOSI here and drives MISO */
			spi_delay();
			in = (uint8_t)(in << 1 | (gpio_get(PIN_MISO) ? 1 : 0));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(PIN_CS, true);
	spi_delay();
}

/* ---- the Novatek protocol ---------------------------------------------------------------------- */

#define NVT_MAX_TRANSFER        80
#define NVT_MAX_TOUCHES         10
#define NVT_RECORD_SIZE         6
#define NVT_REPORT_SIZE         (NVT_RECORD_SIZE * NVT_MAX_TOUCHES + 5)  /* 65 bytes, as the vendor reads */

#define NVT_PAGE_REG            0xff    /* "set page": { 0xff, address >> 15, address >> 7 } */
#define NVT_READ_MASK           0x7f
#define NVT_WRITE_MASK          0x80

#define NVT_CHIP_ID_PAGE        0x1f600
#define NVT_CHIP_ID_REG         0x4e
#define NVT_SWRST_N8_ADDR       0x3f0fe /* novatek,swrst-n8-addr in vayu's device tree */
#define NVT_SPI_RD_FAST_ADDR    0x3f310 /* novatek,spi-rd-fast-addr; 0 disables fast read */
#define NVT_ENG_RST_ADDR        0x7fff80

#define NVT_EVENT_MAP_FWINFO    0x78    /* the firmware-info block inside the event-buffer page */
#define NVT_EVENT_MAP_NT36672A  0x21c00
#define NVT_EVENT_MAP_NT36675   0x22d00

static bool
nvt_write(uint8_t reg, const uint8_t *data, uint32_t length)
{
	uint8_t tx[NVT_MAX_TRANSFER];

	if (length + 1 > sizeof(tx)) {
		return false;
	}
	tx[0] = (uint8_t)(reg | NVT_WRITE_MASK);
	if (length) {
		memcpy(tx + 1, data, length);
	}
	spi_xfer(tx, NULL, length + 1);
	return true;
}

/* The chip returns two dummy bytes before the data (the vendor's CTP_SPI_READ). */
static bool
nvt_read(uint8_t reg, uint8_t *data, uint32_t length)
{
	uint8_t tx[NVT_MAX_TRANSFER], rx[NVT_MAX_TRANSFER];
	uint32_t total = length + 2;

	if (total > sizeof(tx)) {
		return false;
	}
	memset(tx, 0, total);
	tx[0] = (uint8_t)(reg & NVT_READ_MASK);
	spi_xfer(tx, rx, total);
	memcpy(data, rx + 2, length);
	return true;
}

static bool
nvt_set_page(uint32_t address)
{
	uint8_t page[2] = { (uint8_t)(address >> 15), (uint8_t)(address >> 7) };

	return nvt_write(NVT_PAGE_REG, page, sizeof(page));
}

static bool
nvt_write_addr(uint32_t address, uint8_t value)
{
	if (!nvt_set_page(address)) {
		return false;
	}
	return nvt_write((uint8_t)address, &value, 1);
}

static bool
nvt_read_addr(uint32_t address, uint8_t *data, uint32_t length)
{
	if (!nvt_set_page(address)) {
		return false;
	}
	return nvt_read((uint8_t)address, data, length);
}

/* ---- the chip ----------------------------------------------------------------------------------- */

/* Pins idle, chip held in reset, then its I/O supply; the device tree gives the supply 4 ms. */
static void
power_on(void)
{
	spi_pins_idle();
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	gpio_configure(PIN_INT, 0, PULL_DOWN, 8, false);
	gpio_output(PIN_VDDIO, true, PULL_NONE, 2);
	sleep_ms(4);
}

static uint32_t event_map;
static uint32_t max_x = 1080, max_y = 2400;
static uint32_t flip;

static bool
nvt_fw_info(uint32_t map, uint8_t *info)
{
	uint32_t x, y;

	if (!nvt_read_addr(map | NVT_EVENT_MAP_FWINFO, info, 17)) {
		return false;
	}
	/* the firmware says its version and its complement; either all zeros or all ones is no answer */
	if ((uint8_t)(info[0] + info[1]) != 0xff || info[0] == 0 || info[0] == 0xff) {
		return false;
	}
	x = (uint32_t)info[4] << 8 | info[5];
	y = (uint32_t)info[6] << 8 | info[7];
	return x >= 100 && x <= 10000 && y >= 100 && y <= 10000;
}

/*
 * Hold the chip in reset, run its boot loader's reset command, then ask who it is. The vendor
 * driver does the same (nvt_eng_reset, nvt_bootloader_reset, nvt_ts_check_chip_ver_trim).
 */
static bool
nvt_probe(void)
{
	static const uint32_t maps[] = { NVT_EVENT_MAP_NT36672A, NVT_EVENT_MAP_NT36675 };
	uint8_t id[6] = { 0 }, info[17];
	bool answered = false;

	/* reset is already low and the supply on; the vendor's order: eng reset, then release */
	nvt_write_addr(NVT_ENG_RST_ADDR, 0x5a);
	sleep_ms(1);
	gpio_output(PIN_RESET, true, PULL_UP, 8);
	sleep_ms(10);

	for (int attempt = 0; attempt < 5 && !answered; attempt++) {
		bool blank = true;

		nvt_write_addr(NVT_SWRST_N8_ADDR, 0x69);        /* boot loader reset */
		sleep_ms(10);
		nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00);     /* the fast-read mode would break the reads */
		if (nvt_read_addr(NVT_CHIP_ID_PAGE | NVT_CHIP_ID_REG, id, sizeof(id))) {
			for (unsigned i = 0; i < sizeof(id); i++) {
				blank &= id[i] == 0 || id[i] == 0xff;
			}
			answered = !blank;
		}
		if (!answered) {
			sleep_ms(10);
		}
	}
	printf("nvt: chip id %02x %02x %02x %02x %02x %02x%s\n", id[0], id[1], id[2], id[3], id[4], id[5],
	    answered ? "" : " (no answer)");
	if (!answered) {
		return false;
	}

	/*
	 * The two maps differ only in where the event buffer lives; the firmware-info block tells.
	 * The boot loader reset above starts the firmware, which needs a moment before it answers.
	 */
	for (int attempt = 0; attempt < 100; attempt++) {
		for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
			if (nvt_fw_info(maps[i], info)) {
				event_map = maps[i];
				max_x = (uint32_t)info[4] << 8 | info[5];
				max_y = (uint32_t)info[6] << 8 | info[7];
				printf("nvt: firmware %u (type %#x), panel %u x %u, event buffer at %#x\n", info[0],
				    info[13], max_x, max_y, event_map);
				nvt_set_page(event_map);        /* report reads start at offset 0 of this page */
				return true;
			}
		}
		if (attempt % 20 == 19) {               /* it may have answered before its firmware was up */
			nvt_write_addr(NVT_SWRST_N8_ADDR, 0x69);
			sleep_ms(10);
			nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00);
		}
		sleep_ms(10);
	}
	printf("nvt: no valid firmware information in either event-buffer map\n");
	return false;
}

/* ---- touch reports ------------------------------------------------------------------------------ */

static struct {
	bool down;
	uint32_t x, y;
} contact[NVT_MAX_TOUCHES];

static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

static uint32_t
scale(uint32_t value, uint32_t maximum)
{
	if (maximum == 0) {
		return 0;
	}
	if (value >= maximum) {
		return 65535;
	}
	return (uint32_t)((uint64_t)value * 65535 / maximum);
}

static void
nvt_report(const uint8_t *report)
{
	bool present[NVT_MAX_TOUCHES] = { false };
	static unsigned shown;

	for (unsigned i = 0; i < NVT_MAX_TOUCHES; i++) {
		const uint8_t *record = report + NVT_RECORD_SIZE * i;
		uint32_t id = record[0] >> 3, kind = record[0] & 0x07, x, y;

		/* 1 = finger down, 2 = moving; anything else is an empty slot */
		if (id == 0 || id > NVT_MAX_TOUCHES || (kind != 1 && kind != 2)) {
			continue;
		}
		x = (uint32_t)record[1] << 4 | record[3] >> 4;
		y = (uint32_t)record[2] << 4 | (record[3] & 0x0f);
		if (x > max_x || y > max_y) {           /* the vendor rejects these too */
			continue;
		}
		x = scale(x, max_x);
		y = scale(y, max_y);
		if (flip & 1) {
			x = 65535 - x;
		}
		if (flip & 2) {
			y = 65535 - y;
		}
		present[id - 1] = true;
		seen.touches++;
		seen.last_x = x;
		seen.last_y = y;
		if (shown < 16) {
			shown++;
			printf("nvt: finger %u %s at %u, %u (of %u x %u)\n", id - 1, kind == 1 ? "down" : "moved", x, y,
			    max_x, max_y);
		}
		if (!contact[id - 1].down) {
			contact[id - 1].down = true;
			redwood_touch_post(REDWOOD_TOUCH_DOWN, id - 1, x, y);
		} else if (contact[id - 1].x != x || contact[id - 1].y != y) {
			redwood_touch_post(REDWOOD_TOUCH_MOVE, id - 1, x, y);
		}
		contact[id - 1].x = x;
		contact[id - 1].y = y;
	}
	/* a finger whose slot is empty in this report has lifted */
	for (unsigned i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (contact[i].down && !present[i]) {
			contact[i].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, i, contact[i].x, contact[i].y);
		}
	}
}

/* A blank report means no finger, and also that a reset must not leave one held down. */
static void
nvt_release_all(void)
{
	for (unsigned i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (contact[i].down) {
			contact[i].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, i, contact[i].x, contact[i].y);
		}
	}
}

static void
nvt_poll(void)
{
	uint8_t report[NVT_REPORT_SIZE];
	bool blank = true;

	seen.polls++;
	/* the page could have been lost to a reset or an ESD event; setting it is three bytes */
	if (!nvt_set_page(event_map) || !nvt_read(0x00, report, sizeof(report))) {
		seen.bad++;
		return;
	}
	for (unsigned i = 0; i < sizeof(report); i++) {
		blank &= report[i] == 0 || report[i] == 0xff;
	}
	if (blank) {                            /* no finger, or the chip reset: nothing may stay down */
		nvt_release_all();
		return;
	}
	seen.reports++;
	nvt_report(report);
}

/* ---- all of it together ------------------------------------------------------------------------- */

static void
nvt_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 8000, status_every;
	uint64_t reading = 0, reads = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 500) {
		poll_us = 500;
	}
	if (poll_us > 100000) {
		poll_us = 100000;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("nvt_spi_us", &spi_half_us, sizeof(spi_half_us));
	if (spi_half_us < 1) {
		spi_half_us = 1;
	}
	if (spi_half_us > 20) {
		spi_half_us = 20;
	}
	status_every = 60000000 / poll_us;              /* a line a minute */
	if (status_every == 0) {
		status_every = 1;
	}

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		printf("nvt: cannot map the pins at %#lx\n", (unsigned long)TLMM_PHYS);
		thread_terminate(current_thread());
		return;
	}

	printf("nvt: pins at %#lx; the bus at about %u Hz (half period %u us)\n", (unsigned long)TLMM_PHYS,
	    500000 / spi_half_us, spi_half_us);
	power_on();

	if (!nvt_probe()) {
		printf("nvt: no Novatek touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("nvt: touch screen ready; the event buffer is read every %u us; interrupt line %s\n", poll_us,
	    gpio_get(PIN_INT) ? "high" : "low");

	for (unsigned tick = 1;; tick++) {
		uint64_t before = mach_absolute_time(), after;

		nvt_poll();
		after = mach_absolute_time();
		reading += after - before;
		reads++;
		if (tick % status_every == 0) {
			uint64_t ns;

			absolutetime_to_nanoseconds(reading / reads, &ns);
			printf("nvt: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us; %u us between reads\n", seen.reports, seen.polls,
			    seen.bad, seen.touches, seen.last_x, seen.last_y, gpio_get(PIN_INT) ? "high" : "low",
			    (unsigned long long)(ns / 1000), poll_us);
		}
		sleep_us(poll_us);
	}
}

void
redwood_nvt_init(void)
{
	uint32_t enabled = 0;
	char driver[16] = "";
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (!PE_parse_boot_argn("touch_driver", driver, sizeof(driver)) || strcmp(driver, "nvt") != 0) {
		return;
	}
	if (kernel_thread_start(nvt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
