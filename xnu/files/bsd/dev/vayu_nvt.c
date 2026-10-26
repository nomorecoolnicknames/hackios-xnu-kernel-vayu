/*
 * The touch screen of the POCO X3 Pro (vayu): a Novatek NT36672C behind the SPI pins of
 * QUPv3 wrapper 2, serial engine 0 (the controller at 0x00c80000), as a source of touches
 * for /dev/hakios-touch (sys/hakios.input.h). Only with boot-args
 * touchscreen=1 touch_driver=nvt (bit-banged) or touchscreen=1 nvt_qup=1 /
 * touch_driver=nvt_qup (QUPv3): every pin and address below belongs to this phone.
 *
 * This is the second touch driver in the kernel: redwood's Goodix GT9916/FocalTech FT3680
 * driver (bsd/dev/hakios.goodix.c) is for a different SoC and a different controller, and
 * hakios_input_init() starts that one unless touch_driver=nvt / nvt_qup (or nvt_qup=1) is
 * given. QEMU and redwood therefore behave exactly as before.
 *
 * There is no SPI, clock, pin or regulator framework in this kernel, and the boot loader
 * has no SPI or GPIO service either (its only shared services are the log, the storage and
 * the charger state). The existing Goodix driver talks to the SoC's registers directly
 * with ml_io_map(); this one does the same.
 *
 * The vayu bus is QUPv3 SPI, not bit-banged GPIO. Start the kernel with
 * "touchscreen=1 nvt_qup=1" (or "touch_driver=nvt_qup"): the loader muxes GPIO 55-58 to
 * qup17 and applies reset/VDDIO, and bsd/dev/vayu_qup_spi.c drives spi@c80000. XNU does
 * not write TLMM on this phone. Without nvt_qup the old bit-banged path below is selected;
 * it is kept for the record, but on vayu it writes the wrong TLMM tile and was the path
 * that never saw data.
 *
 *   pins    the vendor pinctrl window is 0x03000000; its four tiles are at +0x100000
 *           (west), +0x500000 (east), +0x900000 (north), +0xd00000 (south), so pins
 *           55-58 are 0x03d00000 + 0x1000 * pin, reset GPIO12 is 0x0390c000, and
 *           VDDIO GPIO100/INT GPIO122 are in the south tile. The old code below uses
 *           the bare 0x03000000 + 0x1000 * pin and is not the QUP path.
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
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Lines go to the loader's live log as well as printf (as hakios.buttons.c does): the loader
 * reads that ring while the kernel runs, so a bus hang on the touch path still leaves its
 * last line; printf alone only reaches the loader if the kernel comes back.
 */
extern void loader_log_putc(char c);

static int nvt_say(const char *format, ...) __printflike(1, 2);

static int
nvt_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf nvt_say

/*
 * The QUPv3 bus half (bsd/dev/vayu_qup_spi.c). The loader muxes GPIO 55-58 to
 * qup17 and applies reset/VDDIO; this file then speaks the same Novatek protocol
 * through the GENI serial engine instead of the (dead on vayu) bit-banged pins.
 */
extern bool vayu_qup_spi_init(void);
extern bool vayu_qup_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length);
extern void vayu_qup_spi_fini(void);
extern bool vayu_qup_nvt_resx(bool high);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static bool nvt_qup;            /* boot arg nvt_qup=1 or touch_driver=nvt_qup */

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x03000000UL
#define TLMM_SIZE               0x100000UL
#define GPIO_CFG(n)             (tlmm_west + 0x1000UL * (n)) /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (GPIO_CFG(n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

static unsigned pin_miso = 55;
static unsigned pin_mosi = 56;
static unsigned pin_clk = 57;
static unsigned pin_cs = 58;
static unsigned miso_pull = PULL_NONE;
static unsigned reset_release = 1;
static unsigned spi_mode;
static unsigned cs_active_low = 1;
static unsigned skip_rails;
static unsigned tlmm_test;
#define PIN_RESET               12
#define PIN_INT                 122
#define PIN_VDDIO               100
#define PIN_FUNCTION_SPI        1       /* "qup17" is the first mux after gpio on 55-58 (not used: bit-banged) */

static vm_offset_t tlmm_west;

static bool
tlmm_map(void)
{
	tlmm_west = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm_west == 0) {
		printf("nvt: cannot map the pins at %#lx\n", (unsigned long)TLMM_PHYS);
		return false;
	}
	return true;
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

/* CS inactive, clock idle, data out low: the idle state before the chip is powered. */
static void
spi_pins_idle(void)
{
	gpio_output(pin_cs, cs_active_low, PULL_NONE, 6);
	gpio_output(pin_clk, (spi_mode & 1) != 0, PULL_NONE, 6);
	gpio_output(pin_mosi, false, PULL_NONE, 6);
	gpio_configure(pin_miso, 0, miso_pull, 2, false);
}

/* One transaction, chip selected throughout; rx may be NULL. */
static bool
spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	bool cpol = (spi_mode & 1) != 0;

	if (nvt_qup) {
		return vayu_qup_spi_xfer(tx, rx, length);
	}

	gpio_set(pin_cs, !cs_active_low);
	gpio_set(pin_clk, cpol);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(pin_mosi, out >> bit & 1);
			spi_delay();
			if (cpol) {              /* mode 3: sample on the rising edge */
				gpio_set(pin_clk, false);
				spi_delay();
				in = (uint8_t)(in << 1 | (gpio_get(pin_miso) ? 1 : 0));
				gpio_set(pin_clk, true);
			} else {                 /* mode 0: sample on the rising edge */
				gpio_set(pin_clk, true);
				spi_delay();
				in = (uint8_t)(in << 1 | (gpio_get(pin_miso) ? 1 : 0));
				gpio_set(pin_clk, false);
			}
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(pin_clk, cpol);
	gpio_set(pin_cs, cs_active_low);
	spi_delay();
	return true;
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
	return spi_xfer(tx, NULL, length + 1);
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
	if (!spi_xfer(tx, rx, total)) {
		return false;
	}
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
	if (nvt_qup) {
		/*
		 * The loader did this before it jumped here: GPIO100 (I/O supply) high,
		 * GPIO12 (reset) released, GPIO55-58 muxed to qup17. XNU must not write
		 * TLMM on vayu; only the read-only report in vayu_qup_spi.c looks at it.
		 */
		printf("nvt: power, reset and pins belong to the loader (QUP mode); 4 ms\n");
		sleep_ms(4);
		return;
	}
	spi_pins_idle();
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	if (!skip_rails) {
		gpio_configure(PIN_INT, 0, PULL_DOWN, 8, false);
		gpio_output(PIN_VDDIO, true, PULL_NONE, 2);
	}
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

/* ---- firmware download: vayu's NT36675 is flashless ------------------------------------------- */
/*
 * vendor nt36xxx_fw_update.c nvt_update_firmware() -> nvt_download_firmware_hw_crc() (:912-983), run every
 * boot. Pieces, all ported 1:1 except the SPI transfer size (vendor NVT_TRANSFER_LEN 63 KB per write,
 * nt36xxx.h:237; here NVT_DL_CHUNK, the page re-selected per chunk -- the chip auto-increments either way):
 * bin checks (end flag "NVT"/"MOD" per 4 KB sector, FW_VER + ~FW_VER = 0xff at need_size - 4 KB; :59-94,
 * :327-335), header parser for hw_crc chips (:148-290), SRAM writes of size+1 bytes per partition
 * (:522-616), BLD CRC banks for ILM/DLM (:673-738, hw_crc 2 = 3-byte length), BLD CRC enable, FW CRC enable,
 * boot ready, reset state >= 0xA0 (nt36xxx.c:350-410, :569-600). Addresses: NT36675_memory_map
 * (nt36xxx_mem_map.h:66-105), hw_crc = 2 (:199-202). File: fw/ of the small image; the vendor loads
 * j20s_novatek_ts_fw01.bin only for panel dsi_j20s_36_02_0a, else fw02 (nt36xxx.c:1096-1112, :1160-1172;
 * DT novatek,cfg_0/1); vayu's panel is dsi_j20s_42_02_0b (recovery dmesg :1294) -> fw02. nvt_fw=1|2.
 */
#define NVT_DL_CHUNK            2048
#define NVT_BOOT_RDY_ADDR       0x3f10d
#define NVT_ILM_LENGTH_ADDR     0x3f118
#define NVT_DLM_LENGTH_ADDR     0x3f130
#define NVT_ILM_DES_ADDR        0x3f128
#define NVT_DLM_DES_ADDR        0x3f12c
#define NVT_G_ILM_CHECKSUM_ADDR 0x3f100
#define NVT_G_DLM_CHECKSUM_ADDR 0x3f104
#define NVT_R_ILM_CHECKSUM_ADDR 0x3f120
#define NVT_R_DLM_CHECKSUM_ADDR 0x3f124
#define NVT_BLD_CRC_EN_ADDR     0x3f30e
#define NVT_BLD_ILM_DLM_CRC_ADDR 0x3f133
#define NVT_EVENT_MAP_RESET_COMPLETE 0x60
#define NVT_EVENT_MAP_HOST_CMD  0x50
#define NVT_MAX_PARTS           32

static struct {
	uint32_t bin, sram, size, crc;
} nvt_parts[NVT_MAX_PARTS];
static unsigned nvt_nparts;

static uint32_t
nvt_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool
nvt_fw_parse(const uint8_t *fw, uint64_t fwsize)
{
	uint64_t need = 0;
	uint32_t end, pos, info_sec = 0, ovly_sec;

	for (uint64_t i = fwsize / 4096; i > 0; i--) {
		const uint8_t *flag = fw + i * 4096 - 3;

		if ((flag[0] == 'N' && flag[1] == 'V' && flag[2] == 'T') || (flag[0] == 'M' && flag[1] == 'O' && flag[2] == 'D')) {
			need = i * 4096;
			break;
		}
	}
	if (need == 0 || (uint8_t)(fw[need - 4096] + fw[need - 4096 + 1]) != 0xff) {
		printf("nvt: firmware file: no NVT/MOD end flag or bad version bar (need %llu)\n", (unsigned long long)need);
		return false;
	}
	end = nvt_le32(fw);
	for (pos = 0x30; pos < end; pos += 0x10) {
		info_sec++;
	}
	ovly_sec = (fw[0x28] & 0x10) ? (fw[0x28] & 0x0f) : 0;
	nvt_nparts = 2 + ovly_sec + info_sec;
	if (nvt_nparts > NVT_MAX_PARTS) {
		return false;
	}
	for (unsigned i = 0; i < nvt_nparts; i++) {
		if (i < 2) {                    /* ILM, DLM: BIN, SRAM, size at 12*i; crc at 0x18 + 4*i */
			nvt_parts[i].bin = nvt_le32(fw + 12 * i);
			nvt_parts[i].sram = nvt_le32(fw + 4 + 12 * i);
			nvt_parts[i].size = nvt_le32(fw + 8 + 12 * i);
			nvt_parts[i].crc = nvt_le32(fw + 0x18 + 4 * i);
		} else {                        /* info sections at 0x30, overlays at DLM's BIN address: SRAM, size, BIN, crc */
			pos = i < 2 + info_sec ? 0x30 + 0x10 * (i - 2) : nvt_parts[1].bin + 0x10 * (i - 2 - info_sec);
			if (pos + 16 > fwsize) {
				return false;
			}
			nvt_parts[i].sram = nvt_le32(fw + pos);
			nvt_parts[i].size = nvt_le32(fw + pos + 4);
			nvt_parts[i].bin = nvt_le32(fw + pos + 8);
			nvt_parts[i].crc = nvt_le32(fw + pos + 12);
		}
		if ((uint64_t)nvt_parts[i].bin + nvt_parts[i].size > fwsize) {
			printf("nvt: firmware partition %u (bin %#x size %#x) runs past the file\n", i, nvt_parts[i].bin,
			    nvt_parts[i].size);
			return false;
		}
	}
	printf("nvt: firmware file: %u partitions (2 + %u info + %u overlay), version %#x, ILM %#x+%#x crc %#x, DLM %#x+%#x crc %#x\n",
	    nvt_nparts, info_sec, ovly_sec, fw[need - 4096], nvt_parts[0].sram, nvt_parts[0].size, nvt_parts[0].crc,
	    nvt_parts[1].sram, nvt_parts[1].size, nvt_parts[1].crc);
	return true;
}

/* A write of any length: { addr & 0x7f | 0x80, data... } per chunk, page selected for each. */
static bool
nvt_write_long(uint32_t address, const uint8_t *data, uint32_t length)
{
	static uint8_t tx[NVT_DL_CHUNK + 1];

	while (length > 0) {
		uint32_t n = length < NVT_DL_CHUNK ? length : NVT_DL_CHUNK;

		if (!nvt_set_page(address)) {
			return false;
		}
		tx[0] = (uint8_t)((address & 0x7f) | NVT_WRITE_MASK);
		memcpy(tx + 1, data, n);
		if (!spi_xfer(tx, NULL, n + 1)) {
			return false;
		}
		address += n;
		data += n;
		length -= n;
	}
	return true;
}

static void
nvt_crc_bank(uint32_t des, uint32_t sram, uint32_t length_addr, uint32_t size, uint32_t golden_addr, uint32_t crc)
{
	uint8_t b[4];

	nvt_set_page(des);                      /* DES, LENGTH and golden CRC share one 128-byte page */
	b[0] = sram & 0xff; b[1] = (sram >> 8) & 0xff; b[2] = (sram >> 16) & 0xff;
	nvt_write((uint8_t)(des & 0x7f), b, 3);
	b[0] = size & 0xff; b[1] = (size >> 8) & 0xff; b[2] = (size >> 16) & 0x01;
	nvt_write((uint8_t)(length_addr & 0x7f), b, 3);         /* hw_crc 2: 4-byte write = reg + 3 */
	b[0] = crc & 0xff; b[1] = (crc >> 8) & 0xff; b[2] = (crc >> 16) & 0xff; b[3] = (crc >> 24) & 0xff;
	nvt_write((uint8_t)(golden_addr & 0x7f), b, 4);
}

static void
nvt_crc_report(void)
{
	uint8_t flag = 0, g[4] = { 0 }, r[4] = { 0 }, gd[4] = { 0 }, rd[4] = { 0 };

	nvt_read_addr(NVT_BLD_ILM_DLM_CRC_ADDR, &flag, 1);
	nvt_read_addr(NVT_G_ILM_CHECKSUM_ADDR, g, 4);
	nvt_read_addr(NVT_R_ILM_CHECKSUM_ADDR, r, 4);
	nvt_read_addr(NVT_G_DLM_CHECKSUM_ADDR, gd, 4);
	nvt_read_addr(NVT_R_DLM_CHECKSUM_ADDR, rd, 4);
	printf("nvt: crc done %u ilm flag %u dlm flag %u; ilm bin %#x golden %#x result %#x; dlm bin %#x golden %#x result %#x\n",
	    (flag >> 2) & 1, flag & 1, (flag >> 1) & 1, nvt_parts[0].crc, nvt_le32(g), nvt_le32(r), nvt_parts[1].crc,
	    nvt_le32(gd), nvt_le32(rd));
}

static bool
nvt_fw_download(uint32_t event_buf)
{
	uint32_t which = 2;
	char name[32];
	uint64_t fwsize = 0;
	const uint8_t *fw;
	uint64_t t0 = mach_absolute_time(), ns;

	PE_parse_boot_argn("nvt_fw", &which, sizeof(which));
	snprintf(name, sizeof(name), "j20s_novatek_ts_fw%02u.bin", which == 1 ? 1 : 2);
	fw = hakios_image_file("fw", name, &fwsize, NULL);
	if (fw == NULL) {
		printf("nvt: no fw/%s in the small image; the chip has no firmware (blank reports)\n", name);
		return false;
	}
	printf("nvt: downloading fw/%s (%llu bytes)\n", name, (unsigned long long)fwsize);
	if (!nvt_fw_parse(fw, fwsize)) {
		return false;
	}
	for (int attempt = 1; attempt <= 3; attempt++) {
		bool ok = true;
		uint8_t b[5];

		nvt_write_addr(NVT_SWRST_N8_ADDR, 0x69);        /* boot-loader reset */
		sleep_ms(5);
		nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00);
		for (unsigned i = 0; i < nvt_nparts && ok; i++) {
			uint32_t size = nvt_parts[i].size;

			if (size == 0) {
				continue;
			}
			size += 1;                      /* the vendor writes size + 1 bytes (nvt_write_firmware :601) */
			if ((uint64_t)nvt_parts[i].bin + size > fwsize) {
				size = (uint32_t)(fwsize - nvt_parts[i].bin);
			}
			ok = nvt_write_long(nvt_parts[i].sram, fw + nvt_parts[i].bin, size);
		}
		if (!ok) {
			printf("nvt: download attempt %d: SRAM write failed\n", attempt);
			continue;
		}
		nvt_crc_bank(NVT_ILM_DES_ADDR, nvt_parts[0].sram, NVT_ILM_LENGTH_ADDR, nvt_parts[0].size,
		    NVT_G_ILM_CHECKSUM_ADDR, nvt_parts[0].crc);
		nvt_crc_bank(NVT_DLM_DES_ADDR, nvt_parts[1].sram, NVT_DLM_LENGTH_ADDR, nvt_parts[1].size,
		    NVT_G_DLM_CHECKSUM_ADDR, nvt_parts[1].crc);
		b[0] = 0xff;                            /* BLD CRC enable: bit 7 */
		nvt_read_addr(NVT_BLD_CRC_EN_ADDR, b, 1);
		b[0] |= 0x80;
		nvt_write((uint8_t)(NVT_BLD_CRC_EN_ADDR & 0x7f), b, 1);
		nvt_set_page(event_buf);                /* FW CRC enable: clear reset state, host command 0xAE */
		b[0] = 0x00;
		nvt_write(NVT_EVENT_MAP_RESET_COMPLETE, b, 1);
		b[0] = 0xae;
		nvt_write(NVT_EVENT_MAP_HOST_CMD, b, 1);
		nvt_write_addr(NVT_BOOT_RDY_ADDR, 1);   /* boot ready */
		sleep_ms(5);
		nvt_set_page(event_buf | NVT_EVENT_MAP_RESET_COMPLETE);
		for (int poll = 0; poll < 50; poll++) {   /* reset state >= 0xA0 (vendor: 10 x 10 ms) */
			nvt_read(NVT_EVENT_MAP_RESET_COMPLETE, b, 5);
			if (b[0] >= 0xa0 && b[0] <= 0xaf) {
				absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
				printf("nvt: firmware downloaded and running (attempt %d, reset state %#x after %d polls, %llu ms)\n",
				    attempt, b[0], poll, (unsigned long long)(ns / 1000000));
				return true;
			}
			sleep_ms(10);
		}
		printf("nvt: download attempt %d: reset state %02x %02x %02x %02x %02x, not >= 0xA0\n", attempt, b[0], b[1], b[2],
		    b[3], b[4]);
		nvt_crc_report();
	}
	return false;
}

/*
 * The vendor's trim_id_table (nt36xxx_mem_map.h:232-273): six id bytes, a mask of which bytes count,
 * and the chip family. The vendor rejects an unknown id (nt36xxx.c:1581-1641); this only warns, so a
 * chip the table does not list still gets its reports read.
 */
static const struct {
	uint8_t id[6], mask[6];
	const char *chip;
} nvt_trim_ids[] = {
	{ { 0x0c, 0xff, 0xff, 0x72, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36675" },
	{ { 0xff, 0xff, 0xff, 0x75, 0x66, 0x03 }, { 0, 0, 0, 1, 1, 1 }, "NT36675" },
	{ { 0x0b, 0xff, 0xff, 0x72, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0b, 0xff, 0xff, 0x82, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0b, 0xff, 0xff, 0x25, 0x65, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0a, 0xff, 0xff, 0x72, 0x65, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0a, 0xff, 0xff, 0x72, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0a, 0xff, 0xff, 0x82, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0a, 0xff, 0xff, 0x70, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0b, 0xff, 0xff, 0x70, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x0a, 0xff, 0xff, 0x72, 0x67, 0x03 }, { 1, 0, 0, 1, 1, 1 }, "NT36672A" },
	{ { 0x55, 0x00, 0xff, 0x00, 0x00, 0x00 }, { 1, 1, 0, 1, 1, 1 }, "NT36772" },
	{ { 0x55, 0x72, 0xff, 0x00, 0x00, 0x00 }, { 1, 1, 0, 1, 1, 1 }, "NT36772" },
	{ { 0xaa, 0x00, 0xff, 0x00, 0x00, 0x00 }, { 1, 1, 0, 1, 1, 1 }, "NT36772" },
	{ { 0xaa, 0x72, 0xff, 0x00, 0x00, 0x00 }, { 1, 1, 0, 1, 1, 1 }, "NT36772" },
	{ { 0xff, 0xff, 0xff, 0x72, 0x67, 0x03 }, { 0, 0, 0, 1, 1, 1 }, "NT36772" },
	{ { 0xff, 0xff, 0xff, 0x70, 0x66, 0x03 }, { 0, 0, 0, 1, 1, 1 }, "NT36772" },
	{ { 0xff, 0xff, 0xff, 0x70, 0x67, 0x03 }, { 0, 0, 0, 1, 1, 1 }, "NT36772" },
	{ { 0xff, 0xff, 0xff, 0x72, 0x66, 0x03 }, { 0, 0, 0, 1, 1, 1 }, "NT36772" },
	{ { 0xff, 0xff, 0xff, 0x25, 0x65, 0x03 }, { 0, 0, 0, 1, 1, 1 }, "NT36525" },
	{ { 0xff, 0xff, 0xff, 0x76, 0x66, 0x03 }, { 0, 0, 0, 1, 1, 1 }, "NT36676F" },
};

static const char *
nvt_chip_check(const uint8_t *id)
{
	for (unsigned row = 0; row < sizeof(nvt_trim_ids) / sizeof(nvt_trim_ids[0]); row++) {
		unsigned i;

		for (i = 0; i < 6; i++) {
			if (nvt_trim_ids[row].mask[i] && id[i] != nvt_trim_ids[row].id[i]) {
				break;
			}
		}
		if (i == 6) {
			printf("nvt: chip id matches the vendor table row %u (%s)\n", row, nvt_trim_ids[row].chip);
			return nvt_trim_ids[row].chip;
		}
	}
	printf("nvt: WARNING: chip id is in no row of the vendor table (the vendor would stop here); going on\n");
	return NULL;
}

/*
 * Hold the chip in reset, run its boot loader's reset command, then ask who it is. The vendor
 * driver does the same (nvt_eng_reset, nvt_bootloader_reset, nvt_ts_check_chip_ver_trim).
 */
static bool
nvt_probe(void)
{
	uint32_t maps[] = { NVT_EVENT_MAP_NT36672A, NVT_EVENT_MAP_NT36675 };
	uint8_t id[6] = { 0 }, info[17];
	bool answered = false;
	const char *chip;

	if (nvt_qup) {
		/*
		 * vayu's NT36675 is flashless: the vendor downloads its firmware on every boot
		 * (nt36xxx_fw_update.c:1063-1135, Boot_Update_Firmware) and its chip check starts with a
		 * boot-loader reset (nt36xxx.c:1593), after which no firmware runs until the download. So
		 * before any reset: is the firmware Android loaded still running? (NT36675 map first: v26
		 * read chip id 0c 00 00 72 66 03 = vendor table row 0, NT36675.)
		 */
		nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00);
		for (unsigned i = 0; i < 2; i++) {
			uint32_t map = i == 0 ? NVT_EVENT_MAP_NT36675 : NVT_EVENT_MAP_NT36672A;

			if (nvt_fw_info(map, info)) {
				event_map = map;
				max_x = (uint32_t)info[4] << 8 | info[5];
				max_y = (uint32_t)info[6] << 8 | info[7];
				printf("nvt: firmware already running (no reset): %u (type %#x), panel %u x %u, event buffer at %#x\n",
				    info[0], info[13], max_x, max_y, event_map);
				nvt_set_page(event_map);
				return true;
			}
		}
		printf("nvt: no running firmware before reset (fw info %02x %02x ... at both maps)\n", info[0], info[1]);
		/*
		 * The loader released reset. The vendor sends ENG_RST (0x5a to 0x7fff80) with RESX low, then
		 * raises RESX and waits 10 ms (nt36xxx.c:424-430, :2397-2405). On the QUP path that takes one
		 * TLMM write (GPIO12), granted narrowly (the bring-up notes); off by default (it loses a flashless
		 * chip's firmware), nvt_qup_xnu_reset=1 to use it; otherwise the
		 * boot-loader reset below (SWRST_N8 = 0x69) has to do.
		 */
		if (vayu_qup_nvt_resx(false)) {
			sleep_ms(10);
			nvt_write_addr(NVT_ENG_RST_ADDR, 0x5a);
			sleep_ms(1);
			if (!vayu_qup_nvt_resx(true)) {
				printf("nvt: WARNING: RESX did not read back high; the chip may still be in reset\n");
			}
			sleep_ms(10);
			printf("nvt: eng reset sent with RESX low, RESX released, 10 ms (vendor order)\n");
		} else {
			printf("nvt: reset is the loader's (released; GPIO12 write off or refused); "
			    "boot-loader reset only\n");
		}
	} else {
		/* reset is already low and the supply on; the vendor's order: eng reset, then release */
		nvt_write_addr(NVT_ENG_RST_ADDR, 0x5a);
		sleep_ms(1);
		gpio_output(PIN_RESET, reset_release ? true : false, PULL_UP, 8);
		sleep_ms(10);
		printf("nvt: after reset: MISO %u, RESET %u, INT %u\n", gpio_get(pin_miso) ? 1 : 0,
		    gpio_get(PIN_RESET) ? 1 : 0, skip_rails ? 0 : (gpio_get(PIN_INT) ? 1 : 0));
	}

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
	chip = nvt_chip_check(id);
	if (chip != NULL && strcmp(chip, "NT36675") == 0) {
		maps[0] = NVT_EVENT_MAP_NT36675;        /* vendor NT36675_memory_map EVENT_BUF_ADDR 0x22D00 (nt36xxx_mem_map.h:66-67) */
		maps[1] = NVT_EVENT_MAP_NT36672A;
	}

	/*
	 * The two maps differ only in where the event buffer lives; the firmware-info block tells.
	 * The boot loader reset above starts the firmware, which needs a moment before it answers.
	 */
	bool downloaded = nvt_qup && nvt_fw_download(maps[0]);

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
		if (attempt % 20 == 19 && !downloaded) {        /* it may have answered before its firmware was up */
			nvt_write_addr(NVT_SWRST_N8_ADDR, 0x69);
			sleep_ms(10);
			nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00);
		}
		sleep_ms(10);
	}
	/*
	 * vendor nvt_get_fw_info() (nt36xxx.c:680-696): after its retries, 1080 x 2400 and carry on. On vayu
	 * this means no firmware is running (flashless, nothing downloaded): reports will stay blank until
	 * the firmware download exists.
	 */
	event_map = maps[0];
	max_x = 1080;
	max_y = 2400;
	printf("nvt: no valid firmware information in either event-buffer map; vendor fallback: panel %u x %u, "
	    "event buffer at %#x (chip %s); flashless chip -- no firmware download yet, expect blank reports\n", max_x, max_y,
	    event_map, chip ? chip : "?");
	nvt_set_page(event_map);
	return true;
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
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id - 1, x, y);
		} else if (contact[id - 1].x != x || contact[id - 1].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id - 1, x, y);
		}
		contact[id - 1].x = x;
		contact[id - 1].y = y;
	}
	/* a finger whose slot is empty in this report has lifted */
	for (unsigned i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (contact[i].down && !present[i]) {
			contact[i].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, i, contact[i].x, contact[i].y);
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
			hakios_touch_post(HAKIOS_TOUCH_UP, i, contact[i].x, contact[i].y);
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
	uint32_t nvt_pin;
	if (PE_parse_boot_argn("nvt_miso", &nvt_pin, sizeof(nvt_pin)) && nvt_pin < 150) {
		pin_miso = nvt_pin;
	}
	if (PE_parse_boot_argn("nvt_mosi", &nvt_pin, sizeof(nvt_pin)) && nvt_pin < 150) {
		pin_mosi = nvt_pin;
	}
	if (PE_parse_boot_argn("nvt_clk", &nvt_pin, sizeof(nvt_pin)) && nvt_pin < 150) {
		pin_clk = nvt_pin;
	}
	if (PE_parse_boot_argn("nvt_cs", &nvt_pin, sizeof(nvt_pin)) && nvt_pin < 150) {
		pin_cs = nvt_pin;
	}
	if (PE_parse_boot_argn("nvt_miso_pull", &nvt_pin, sizeof(nvt_pin)) && nvt_pin <= PULL_UP) {
		miso_pull = nvt_pin;
	}
	if (PE_parse_boot_argn("nvt_mode", &nvt_pin, sizeof(nvt_pin)) && nvt_pin <= 3) {
		spi_mode = nvt_pin;
	}
	if (PE_parse_boot_argn("nvt_reset_release", &nvt_pin, sizeof(nvt_pin))) {
		reset_release = nvt_pin ? 1 : 0;
	}
	if (PE_parse_boot_argn("nvt_cs_active", &nvt_pin, sizeof(nvt_pin))) {
		cs_active_low = nvt_pin ? 0 : 1;
	}
	if (PE_parse_boot_argn("nvt_skip_rails", &nvt_pin, sizeof(nvt_pin))) {
		skip_rails = nvt_pin ? 1 : 0;
	}
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

	if (nvt_qup) {
		printf("nvt: QUP backend: the loader muxed 55-58 to qup17 and applied reset/VDDIO; "
		    "XNU only reads the pins\n");
	} else {
		if (!tlmm_map()) {
			thread_terminate(current_thread());
			return;
		}
		printf("nvt: pins at west %#lx; MISO %u MOSI %u CLK %u CS %u pull %u; mode %u; %s CS; reset %s; "
		    "the bus at about %u Hz (half period %u us)\n",
		    (unsigned long)tlmm_west, pin_miso, pin_mosi, pin_clk, pin_cs, miso_pull, spi_mode,
		    cs_active_low ? "active-low" : "active-high", reset_release ? "released" : "held",
		    500000 / spi_half_us, spi_half_us);
	}
	power_on();
	if (!nvt_qup) {
		printf("nvt: MISO idle level %u before the first transaction\n", gpio_get(pin_miso) ? 1 : 0);
	}

	if (nvt_qup && !vayu_qup_spi_init()) {
		printf("nvt: the QUP bus did not come up; see the nvt-qup lines above\n");
		thread_terminate(current_thread());
		return;
	}

	if (!nvt_probe()) {
		printf("nvt: no Novatek touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("nvt: touch screen ready; the event buffer is read every %u us; interrupt line %s\n", poll_us,
	    nvt_qup ? "polled, not used" : (skip_rails ? "not used" : (gpio_get(PIN_INT) ? "high" : "low")));

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
			    seen.bad, seen.touches, seen.last_x, seen.last_y,
			    nvt_qup ? "polled, not used" : (skip_rails ? "not used" : (gpio_get(PIN_INT) ? "high" : "low")),
			    (unsigned long long)(ns / 1000), poll_us);
		}
		sleep_us(poll_us);
	}
}

static bool
nvt_sync_probe(void)
{
	uint32_t v;

	PE_parse_boot_argn("nvt_spi_us", &spi_half_us, sizeof(spi_half_us));
	if (spi_half_us < 1) {
		spi_half_us = 1;
	}
	if (spi_half_us > 20) {
		spi_half_us = 20;
	}
	if (PE_parse_boot_argn("nvt_miso", &v, sizeof(v)) && v < 150) {
		pin_miso = v;
	}
	if (PE_parse_boot_argn("nvt_mosi", &v, sizeof(v)) && v < 150) {
		pin_mosi = v;
	}
	if (PE_parse_boot_argn("nvt_clk", &v, sizeof(v)) && v < 150) {
		pin_clk = v;
	}
	if (PE_parse_boot_argn("nvt_cs", &v, sizeof(v)) && v < 150) {
		pin_cs = v;
	}
	if (PE_parse_boot_argn("nvt_miso_pull", &v, sizeof(v)) && v <= PULL_UP) {
		miso_pull = v;
	}
	if (PE_parse_boot_argn("nvt_mode", &v, sizeof(v)) && v <= 3) {
		spi_mode = v;
	}
	if (PE_parse_boot_argn("nvt_reset_release", &v, sizeof(v))) {
		reset_release = v ? 1 : 0;
	}
	if (PE_parse_boot_argn("nvt_cs_active", &v, sizeof(v))) {
		cs_active_low = v ? 0 : 1;
	}
	if (PE_parse_boot_argn("nvt_skip_rails", &v, sizeof(v))) {
		skip_rails = v ? 1 : 0;
	}
	if (PE_parse_boot_argn("nvt_tlmm_test", &v, sizeof(v))) {
		tlmm_test = v ? 1 : 0;
	}

	if (nvt_qup) {
		printf("nvt: sync probe; QUP backend (the loader owns the pins, reset and VDDIO)\n");
		power_on();
		if (!vayu_qup_spi_init()) {
			printf("nvt: the QUP bus did not come up\n");
			return false;
		}
		return nvt_probe();
	}
	if (!tlmm_map()) {
		return false;
	}
	if (tlmm_test) {
		printf("nvt: tlmm read test begins\n");
		printf("nvt: tlmm read test west %#lx raw %#x cfg12 %#x io12 %#x\n", (unsigned long)tlmm_west,
		    rd(tlmm_west), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)));
		printf("nvt: tlmm read test done\n");
		return true;
	}
	printf("nvt: sync probe; pins at west %#lx; MISO %u MOSI %u CLK %u CS %u pull %u; mode %u; %s CS; reset %s; %u Hz\n",
	    (unsigned long)tlmm_west, pin_miso, pin_mosi, pin_clk, pin_cs, miso_pull, spi_mode,
	    cs_active_low ? "active-low" : "active-high", reset_release ? "released" : "held",
	    500000 / spi_half_us);
	power_on();
	printf("nvt: sync probe; MISO idle level %u before the first transaction\n", gpio_get(pin_miso) ? 1 : 0);
	printf("nvt: tlmm mapped %#lx raw %#x %#x %#x\n", (unsigned long)tlmm_west, rd(tlmm_west),
	    rd(tlmm_west + 4), rd(GPIO_CFG(PIN_RESET)));
	printf("nvt: cfg rst %#x io rst %#x; cfg cs %#x io cs %#x; cfg miso %#x io miso %#x\n",
	    rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(pin_cs)), rd(GPIO_IO(pin_cs)),
	    rd(GPIO_CFG(pin_miso)), rd(GPIO_IO(pin_miso)));
	return nvt_probe();
}

void
vayu_nvt_init(void)
{
	uint32_t enabled = 0, probe_only = 0, qup_arg = 0;
	char driver[16] = "";
	bool selected = false, driver_qup = false;
	thread_t thread;

	/* bsd_init calls this after devfs is mounted, i.e. only once there is a root (rd=md0 or a disk). */
	PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled));
	printf("nvt: init from bsd_init (root mounted); touchscreen=%u\n", enabled);
	if (!enabled) {
		return;
	}
	if (PE_parse_boot_argn("touch_driver", driver, sizeof(driver))) {
		if (strcmp(driver, "nvt") == 0) {
			selected = true;
		} else if (strcmp(driver, "nvt_qup") == 0) {
			selected = true;
			driver_qup = true;
		}
	}
	if (!selected) {
		/*
		 * "nvt_qup=1" alone also selects this driver: it names both the bus
		 * and the chip, and the loader uses the same word for its pin work.
		 */
		if (!PE_parse_boot_argn("nvt_qup", &qup_arg, sizeof(qup_arg)) || !qup_arg) {
			return;
		}
		selected = true;
	}
	if (PE_parse_boot_argn("nvt_qup", &qup_arg, sizeof(qup_arg))) {
		nvt_qup = qup_arg != 0;
	}
	if (driver_qup) {
		nvt_qup = true;
	}
	printf("nvt: selected; %s backend%s\n", nvt_qup ? "QUP" : "bit-banged",
	    (PE_parse_boot_argn("nvt_probe_only", &probe_only, sizeof(probe_only)) && probe_only) ?
	    ", synchronous probe only" : ", polling thread");
	if (probe_only) {
		bool ok = nvt_sync_probe();

		if (nvt_qup) {
			vayu_qup_spi_fini();    /* a GSI path is taken down again (nothing for the FIFO path) */
		}

		printf("nvt: sync probe %s\n", ok ? "answered" : "failed");
		return;
	}
	if (kernel_thread_start(nvt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
