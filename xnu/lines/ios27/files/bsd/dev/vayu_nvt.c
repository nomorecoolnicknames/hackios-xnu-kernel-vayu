/*
 * Copyright (c) 2026 HakiOS (vayu port). All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * This file is distributed under the terms of the Apple Public Source License Version 2.0 (APSL-2.0), as the rest of
 * this tree; see APPLE_LICENSE at the top of the tree.
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * The touch screen of the POCO X3 Pro (vayu): a Novatek NT36675 (the vendor's nt36672c driver) on the SPI of QUPv3
 * wrapper 2, serial engine 0 (0x00c80000, bsd/dev/vayu_qup_spi.c), its touches into iOS's HID through
 * bsd/dev/vayu_input.c (SweetHID's digitizer, iokit/Kernel/arm/hid/SweetHID.cpp).
 *
 * Origin: the iOS 16.5 line (tree 6a36d992) bsd/dev/vayu_nvt.c (the 8796 daily line, 3067ea2; its protocol, firmware
 * download, report decoding and pacing). Ported to xnu-12377 (K7):
 *  - QUP only: the bit-banged path (touch_driver=nvt, the wrong TLMM tile, never saw data) is gone, and so are
 *    its pin arguments; XNU writes no TLMM here (the loader muxes 55-58 to qup17 and applies VDDIO/reset when it
 *    is given nvt_qup=1, xnuloader/loader.c);
 *  - only with vayu27=1 touchscreen=1 nvt_qup=1, from the K7 hook after devfs (bsd/dev/vayu_input.c);
 *  - the firmware (flashless chip) is a file of the running root, fw/j20s_novatek_ts_fw02.bin (nvt_fw=1: fw01),
 *    read by vayu_input_fw(): a root without it is waited for nvt_fw_wait= seconds (default 60), then the driver
 *    stops -- no "touch screen ready" without firmware info (the 8796 line's vendor fallback is gone);
 *  - touches go to vayu_touch_post() (0..65535, as hakios_touch_post_at() took them); the display's per-second
 *    finger line and touch_off_hz= are not ported (no such hooks of K5's);
 *  - nvt_selftest=1: once ready, with no finger needed: the chip id, the firmware info read twice and compared,
 *    and nvt_selftest_looks= (default 240) reads of the event buffer -- "nvt: selftest: PASS" when every read
 *    answered and the info held still.
 *
 * The SPI protocol (the vendor's nt36xxx.c, CTP_SPI_READ/WRITE and nvt_set_page):
 *   write: TX[0] = register | 0x80, then the data
 *   read:  TX[0] = register & 0x7f, then zeros; the chip returns two dummy bytes followed by the data
 *   page:  write { 0xff, address >> 15, address >> 7 } first; the low seven bits select the register
 * The report is 10 records of 6 bytes at the event-buffer page:
 *   byte 0: [7:3] finger (1..10), [2:0] 1 = down, 2 = move; bytes 1,2: x and y, high 8 bits; byte 3: low 4 bits
 * Panel size and firmware version come from the firmware-info block at event-buffer + 0x78.
 *
 * No interrupt: the event buffer is read touch_hz times a second (240 by default, the panel's rate; touch_poll_us=N
 * for a plain sleep), touch_idle_hz=N with touch_idle_ms= slows down with no finger. touch_flip=N mirrors.
 * nvt_cmd=0xNN, nvt_hop=0, touch_tail=1, nvt_dump=1, nvt_rate=N: as on 8796 (below).
 *
 * Sources: arch/arm64/boot/dts/vendor/qcom (vayu.dtb), MiCode Xiaomi_Kernel_OpenSource vayu-r-oss
 * drivers/input/touchscreen/nt36672c/nt36xxx.c, nt36xxx.h, nt36xxx_mem_map.h, nt36xxx_fw_update.c.
 */
/* (the kernel builds with warnings on and not as errors: these two are this file's errors -- run ak7-t2) */
#pragma clang diagnostic error "-Wuninitialized"
#pragma clang diagnostic error "-Wsometimes-uninitialized"
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vayu_12377_abi.h>
#include <sys/vayu_input.h>

extern kern_return_t thread_terminate(thread_t thread);

/*
 * The QUPv3 bus half (bsd/dev/vayu_qup_spi.c). The loader muxes GPIO 55-58 to
 * qup17 and applies reset/VDDIO; this file then speaks the same Novatek protocol
 * through the GENI serial engine instead of the (dead on vayu) bit-banged pins.
 */
extern bool vayu_qup_spi_init(void);
extern bool vayu_qup_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length);
extern void vayu_qup_spi_fini(void);
extern bool vayu_qup_nvt_resx(bool high);

static const bool nvt_qup = true;      /* the QUP bus: the only one on 12377 */
/*
 * S8: 240 Hz by default, as Android reads this panel. The boot arg touch_hz=N still overrides it
 * (touch_hz=0 or touch_poll_us=N goes back to the plain sleep, 8000 us in the old default). A
 * finger-driven scroll at 120 frames a second needs the samples to come at the panel's own rate;
 * at the old 125 Hz every frame of a 120 Hz scroll had less than one new position to show.
 */
static uint32_t touch_hz = 240; /* boot arg touch_hz=N: sample at N Hz (0: touch_poll_us) */
/*
 * touch_idle_hz=N (o6; default 0: always touch_hz): with touch_hz, the panel is looked at only N times a
 * second (10..touch_hz) once no finger has been down for touch_idle_ms (default 2000), and at touch_hz again
 * from the first report of a finger. Every look is an SPI transfer this thread waits out busy (vayu_qup_spi.c
 * polls the FIFO: 149-158 us a report, 240 looks a second = 3.6-3.8 % of a core with no finger at all,
 * HEAT_REPORT.md, d1081c2), and 240 wakes a second of whatever core it runs on. The cost is the first touch
 * after a pause: it is seen up to 1/N s later (17 ms at 60).
 */
static uint32_t touch_idle_hz, touch_idle_ms = 2000;
static uint32_t nvt_host_cmd;   /* boot arg nvt_cmd=0xNN: one host command, after the download */
static bool touch_tail;         /* boot arg touch_tail=1: show the five bytes after the records */
static bool page_held;          /* the event-buffer page is selected and the last look saw data */
static bool nvt_selftest_on;    /* boot arg nvt_selftest=1 */
static const char *nvt_chip_name;       /* the vendor table's name for the chip id (nvt_chip_check) */
static void nvt_selftest(void);
/*
 * scroll120b: the panel's own report rate, told apart from what this driver posted. The display
 * driver's once-a-second finger line (iokit/Kernel/MiniIOMobileFramebuffer.cpp) reads these; the
 * previous line counted only the samples hakios_touch_post_at() produced, so a finger held still
 * (or a report the driver dropped) could not be told from a slow panel.
 *
 *   fresh    looks in which the 65-byte event buffer changed -- one new position from the chip
 *   repeats  looks that saw the same non-blank report again -- the panel had nothing new
 *   max_gap  the longest wait between two fresh reports while a finger was down
 */
static volatile uint64_t nvt_fresh_total;
static volatile uint64_t nvt_dup_total;
static volatile uint64_t nvt_max_gap_abs;
static uint64_t nvt_last_fresh_abs;
static bool nvt_hop_off;        /* boot arg nvt_hop=0: send the vendor's frequency-hop disable */
static uint32_t nvt_dump;       /* boot arg nvt_dump=1: the raw report once a second under a finger */

void
hakios_nvt_stats(uint64_t *fresh, uint64_t *repeats, uint64_t *max_gap_abs)
{
	*fresh = os_atomic_load(&nvt_fresh_total, relaxed);
	*repeats = os_atomic_load(&nvt_dup_total, relaxed);
	*max_gap_abs = os_atomic_xchg(&nvt_max_gap_abs, 0, relaxed);   /* the longest since the last line */
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

/* One transaction, chip selected throughout; rx may be NULL. */
static bool
spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	return vayu_qup_spi_xfer(tx, rx, length);
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
#define NVT_EVENT_MAP_RESET_COMPLETE 0x60 /* ... and the reset-state byte the download clears */
#define NVT_EVENT_MAP_HOST_CMD  0x50    /* the one-byte host command register (see nvt_host_command) */
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

static uint32_t event_map;
static uint32_t max_x = 1080, max_y = 2400;
static uint32_t flip;

/*
 * The vendor's host command, as the stock vayu kernel implements it (nt36xxx.c):
 * the event-buffer page's register 0x50 takes one command byte, and the firmware clears the
 * register to 0 when it has carried the command out. The one command that can be named from
 * the stock binary is 0x66: nvt_switch_FreqHopEnDis() (file offset 0x9c2d34 in the stock
 * Image) writes {0x50, 0x66} with the page set to event_buffer | 0x50, sleeps 35 ms, then
 * reads register 0x50 until it answers 0 -- at most twenty times, and its caller logs
 * "switch frequency hopping disable failed!" (string at 0x214da40) when it does not.
 *
 * There is NO report-rate command in that driver: the only writes to 0x50 outside the
 * firmware download are 0x13 (sleep, sent from the suspend path) and 0x66 (this one); the
 * report-rate strings and DT properties a Xiaomi "game mode" would need are absent from the
 * Image, and nvt_xiaomi_config_info is a read-only seq_file. So nvt_rate= does not pretend to
 * change the panel; it changes how often this driver looks (see nvt_thread), and nvt_hop=0
 * is the one chip-side knob the vendor actually has.
 */
#define NVT_HOST_CMD_FREQ_HOP_DISABLE 0x66

static bool
nvt_host_command(uint8_t command)
{
	uint8_t status = 0xff;
	int polls = 0;

	if (!nvt_set_page(event_map | NVT_EVENT_MAP_HOST_CMD)) {
		printf("nvt: host command %02x: the page could not be set\n", command);
		return false;
	}
	if (!nvt_write(NVT_EVENT_MAP_HOST_CMD, &command, 1)) {
		printf("nvt: host command %02x: the write failed\n", command);
		return false;
	}
	for (polls = 1; polls <= 20; polls++) {
		sleep_ms(35);
		if (nvt_read(NVT_EVENT_MAP_HOST_CMD, &status, 1) && status == 0) {
			break;
		}
	}
	nvt_set_page(event_map);                /* the report reads start at offset 0 of this page */
	page_held = true;
	printf("nvt: host command %02x %s after %d poll(s) (register 50 last read %02x)\n", command,
	    polls <= 20 ? "done" : "NOT acknowledged", polls, status);
	return polls <= 20;
}

/* ---- the chip ----------------------------------------------------------------------------------- */

/* The loader did this before it jumped here: GPIO100 (I/O supply) high, GPIO12 (reset) released, 55-58 on qup17. */
static void
power_on(void)
{
	printf("nvt: power, reset and pins belong to the loader (QUP mode); 4 ms\n");
	sleep_ms(4);
}

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
 * The whole 17-byte block, once per boot: this driver reads only the version pair and the panel
 * size out of it. If the firmware states its report period anywhere, it is likely here (a byte
 * 0x78 = 120 or 0xf0 = 240, a word 0x1046 = 4166 us = 240 Hz, 0x208d = 8333 us = 120 Hz).
 */
static void
nvt_fw_info_dump(const uint8_t *info, unsigned length)
{
	char text[3 * 17 + 1];
	char *p = text;

	for (unsigned i = 0; i < length && i < 17; i++) {
		*p++ = "0123456789abcdef"[info[i] >> 4];
		*p++ = "0123456789abcdef"[info[i] & 0xf];
		if (i + 1 < length && i + 1 < 17) {
			*p++ = ' ';
		}
	}
	*p = 0;
	printf("nvt: fw info %s\n", text);
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

static uint32_t nvt_fw_wait = 60;       /* nvt_fw_wait=S: how long the root may take to have the firmware */

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
	fw = vayu_input_fw("fw", name, &fwsize, nvt_fw_wait);
	if (fw == NULL) {
		printf("nvt: no fw/%s in the root after %u s: the chip has no firmware, the driver stops\n", name,
		    nvt_fw_wait);
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
				nvt_fw_info_dump(info, sizeof(info));
				nvt_set_page(event_map);
				page_held = true;
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
	nvt_chip_name = chip;
	if (chip != NULL && strcmp(chip, "NT36675") == 0) {
		maps[0] = NVT_EVENT_MAP_NT36675;        /* vendor NT36675_memory_map EVENT_BUF_ADDR 0x22D00 (nt36xxx_mem_map.h:66-67) */
		maps[1] = NVT_EVENT_MAP_NT36672A;
	}

	/*
	 * The two maps differ only in where the event buffer lives; the firmware-info block tells.
	 * The boot loader reset above starts the firmware, which needs a moment before it answers.
	 */
	bool downloaded = nvt_fw_download(maps[0]);

	for (int attempt = 0; attempt < 100; attempt++) {
		for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
			if (nvt_fw_info(maps[i], info)) {
				event_map = maps[i];
				max_x = (uint32_t)info[4] << 8 | info[5];
				max_y = (uint32_t)info[6] << 8 | info[7];
				printf("nvt: firmware %u (type %#x), panel %u x %u, event buffer at %#x\n", info[0],
				    info[13], max_x, max_y, event_map);
				nvt_fw_info_dump(info, sizeof(info));
				nvt_set_page(event_map);        /* report reads start at offset 0 of this page */
				page_held = true;
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
	 * The vendor's nvt_get_fw_info() (nt36xxx.c:680-696) goes on with 1080 x 2400 after its retries; on vayu that
	 * means no firmware runs (flashless), and the 8796 line then said "touch screen ready" over blank reports.
	 * Here that is the end: no firmware information, no touch screen.
	 */
	printf("nvt: no valid firmware information in either event-buffer map (chip %s, firmware %s): no touch screen\n",
	    chip ? chip : "?", downloaded ? "downloaded" : "not downloaded");
	return false;
}

/* ---- touch reports ------------------------------------------------------------------------------ */

static struct {
	bool down;
	uint32_t x, y;
} contact[NVT_MAX_TOUCHES];

static struct {
	uint32_t polls, reports, fresh, touches, bad, last_x, last_y;
} seen;

/* The previous non-blank report: what differs from it is a *new* report from the chip (seen.fresh). */
static uint8_t last_report[NVT_REPORT_SIZE];

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
nvt_report(const uint8_t *report, uint64_t time)
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
			vayu_touch_post(VAYU_TOUCH_DOWN, id - 1, x, y, time);
		} else if (contact[id - 1].x != x || contact[id - 1].y != y) {
			vayu_touch_post(VAYU_TOUCH_MOVE, id - 1, x, y, time);
		}
		contact[id - 1].x = x;
		contact[id - 1].y = y;
	}
	/* a finger whose slot is empty in this report has lifted */
	for (unsigned i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (contact[i].down && !present[i]) {
			contact[i].down = false;
			vayu_touch_post(VAYU_TOUCH_UP, i, contact[i].x, contact[i].y, time);
		}
	}
}

/* A blank report means no finger, and also that a reset must not leave one held down. */
static void
nvt_release_all(uint64_t time)
{
	for (unsigned i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (contact[i].down) {
			contact[i].down = false;
			vayu_touch_post(VAYU_TOUCH_UP, i, contact[i].x, contact[i].y, time);
		}
	}
}

/*
 * One look at the event buffer, stamped with the time it began (at): the chip is a snapshot, not a
 * queue, so the position it holds now was taken somewhere in the last report period, and the read
 * itself takes 0.85 ms at 960 kHz -- stamping the end would add that to every sample.
 *
 * The page set (three bytes, a whole extra transfer) is skipped while the previous look found data:
 * the page is a register the chip keeps, and a finger that keeps producing reports proves the chip
 * did not reset. A blank look sets it again, so a reset or an ESD event costs one look, not the
 * session. That is what makes a 4 ms period fit: 3 + 67 bytes became 67.
 */
static void
nvt_poll(uint64_t at)
{
	uint8_t report[NVT_REPORT_SIZE];
	bool blank = true;

	seen.polls++;
	if (!page_held && !nvt_set_page(event_map)) {
		seen.bad++;
		return;
	}
	if (!nvt_read(0x00, report, sizeof(report))) {
		seen.bad++;
		page_held = false;
		return;
	}
	for (unsigned i = 0; i < sizeof(report); i++) {
		blank &= report[i] == 0 || report[i] == 0xff;
	}
	if (blank) {                            /* no finger, or the chip reset: nothing may stay down */
		page_held = false;
		nvt_last_fresh_abs = 0;         /* the gap across a lift is not a report gap */
		nvt_release_all(at);
		return;
	}
	page_held = true;
	seen.reports++;
	if (memcmp(report, last_report, sizeof(report)) != 0) {
		seen.fresh++;
		os_atomic_inc(&nvt_fresh_total, relaxed);
		if (nvt_last_fresh_abs != 0 && at > nvt_last_fresh_abs) {
			uint64_t gap = at - nvt_last_fresh_abs;

			if (gap > os_atomic_load(&nvt_max_gap_abs, relaxed)) {
				os_atomic_store(&nvt_max_gap_abs, gap, relaxed);
			}
		}
		nvt_last_fresh_abs = at;
		memcpy(last_report, report, sizeof(report));
	} else {
		os_atomic_inc(&nvt_dup_total, relaxed);
	}
	if (nvt_dump != 0 && (seen.polls % 240) == 0) {
		char line[3 * NVT_REPORT_SIZE + 1];
		char *p = line;

		for (unsigned i = 0; i < sizeof(report); i++) {
			*p++ = "0123456789abcdef"[report[i] >> 4];
			*p++ = "0123456789abcdef"[report[i] & 0xf];
			*p++ = ' ';
		}
		*--p = 0;
		printf("nvt: raw %s\n", line);
	}
	if (touch_tail && (seen.polls % 750) == 0) {
		printf("nvt: tail %02x %02x %02x %02x %02x (after the ten records)\n", report[60], report[61],
		    report[62], report[63], report[64]);
	}
	nvt_report(report, at);
}

/* ---- all of it together ------------------------------------------------------------------------- */

static void
nvt_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 8000, status_every;
	uint64_t reading = 0, reads = 0;

	bool poll_given = PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));

	if (poll_us < 500) {
		poll_us = 500;
	}
	if (poll_us > 100000) {
		poll_us = 100000;
	}
	/*
	 * touch_hz=N: the sample rate, not the sleep. 240 Hz is 4166 us and is what Android sees from
	 * this panel; the number the chip really produces is printed once a minute (see "new reports").
	 * touch_hz wins over touch_poll_us when both are given. S8: with neither given, touch_hz stays
	 * at its default 240 -- the normal boot used to sample at touch_poll_us' 8000 us (125 Hz).
	 */
	if (PE_parse_boot_argn("touch_hz", &touch_hz, sizeof(touch_hz))) {
		if (touch_hz < 30 || touch_hz > 2000) {
			printf("nvt: touch_hz=%u is outside 30..2000; using touch_poll_us=%u\n", touch_hz, poll_us);
			touch_hz = 0;
		}
	} else if (poll_given) {
		touch_hz = 0;           /* an explicit touch_poll_us=N is the old sleep, as before */
	}
	if (touch_hz >= 30 && touch_hz <= 2000) {
		poll_us = 1000000 / touch_hz;
		if (poll_us < 500) {
			poll_us = 500;
		}
	}
	/*
	 * nvt_rate=N (scroll120b): how often this driver looks, not how often the panel reports.
	 * The stock vayu driver has no report-rate command (see nvt_host_command() above), so the
	 * boot arg cannot make the chip produce more positions; it does shorten the average age of
	 * the position the app is given, which is the part of "the scroll stutters" a host can fix.
	 * Named nvt_rate because that is the switch the owner asked for; touch_hz= is the same knob.
	 */
	{
		uint32_t rate = 0;

		if (PE_parse_boot_argn("nvt_rate", &rate, sizeof(rate))) {
			if (rate >= 30 && rate <= 2000) {
				touch_hz = rate;
				poll_us = 1000000 / rate;
				printf("nvt: nvt_rate=%u: polling every %u us; the firmware has no report-rate "
				    "command, so this shortens the sample's age, not the panel's period\n", rate, poll_us);
			} else {
				printf("nvt: nvt_rate=%u is outside 30..2000; ignored\n", rate);
			}
		}
	}
	{
		uint32_t tail = 0, command = 0, hop = 1, dump = 0;

		if (PE_parse_boot_argn("touch_tail", &tail, sizeof(tail))) {
			touch_tail = tail != 0;
		}
		if (PE_parse_boot_argn("nvt_cmd", &command, sizeof(command))) {
			nvt_host_cmd = command & 0xff;
		}
		if (PE_parse_boot_argn("nvt_hop", &hop, sizeof(hop))) {
			nvt_hop_off = hop == 0;         /* nvt_hop=0: the vendor's frequency-hop disable */
		}
		if (PE_parse_boot_argn("nvt_dump", &dump, sizeof(dump))) {
			nvt_dump = dump;
		}
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("nvt_fw_wait", &nvt_fw_wait, sizeof(nvt_fw_wait));
	if (nvt_fw_wait > 600) {
		nvt_fw_wait = 600;
	}
	printf("nvt: QUP backend: the loader is to mux 55-58 to qup17 and apply reset/VDDIO (nvt_qup=1); XNU reads the pins (nvt-qup: pins after the loader)\n");
	power_on();

	if (!vayu_qup_spi_init()) {
		printf("nvt: the QUP bus did not come up; see the nvt-qup lines above\n");
		thread_terminate(current_thread());
		return;
	}

	if (!nvt_probe()) {
		printf("nvt: no Novatek touch screen; the QUP bus is taken down again\n");
		vayu_qup_spi_fini();
		thread_terminate(current_thread());
		return;
	}
	printf("nvt: touch screen ready; the event buffer is read every %u us (%u Hz)%s; interrupt line %s\n", poll_us,
	    poll_us != 0 ? 1000000 / poll_us : 0, touch_hz != 0 ? ", paced by the read's start" : "",
	    "polled, not used");
	vayu_input_touch_source("nvt", max_x, max_y);
	if (nvt_selftest_on) {
		nvt_selftest();
	}

	/*
	 * nvt_cmd=0xNN: one host command to the event buffer's command register (+0x50), the register the
	 * firmware download writes 0xAE to; the vendor's report-rate / game-mode command, if this firmware
	 * has one, goes here. Off unless asked for, and the answer is printed.
	 */
	if (nvt_host_cmd != 0) {
		uint8_t command = (uint8_t)nvt_host_cmd, answer[5] = { 0 };

		nvt_set_page(event_map | NVT_EVENT_MAP_HOST_CMD);
		if (nvt_write(NVT_EVENT_MAP_HOST_CMD, &command, 1)) {
			nvt_read(NVT_EVENT_MAP_HOST_CMD, answer, sizeof(answer));
			printf("nvt: host command %02x sent; answer %02x %02x %02x %02x %02x\n", command, answer[0], answer[1],
			    answer[2], answer[3], answer[4]);
		} else {
			printf("nvt: host command %02x not written\n", command);
		}
		nvt_set_page(event_map);
		page_held = true;
	}

	/*
	 * nvt_hop=0: the vendor's verification path (nt36xxx.c nvt_switch_FreqHopEnDis()) sends
	 * command 0x66 to the event-buffer page's register 0x50 and waits for the firmware to
	 * clear it. The panel's frequency hopping moves the scan between bands; disabling it is
	 * the only chip-side change the stock driver can make that may affect how often a slow
	 * finger produces a new report. Sent after the download and after nvt_cmd=, and only
	 * when asked for: the chip keeps the setting until its next reset, and this driver does
	 * not reset the chip again after boot.
	 */
	if (nvt_hop_off) {
		nvt_host_command(NVT_HOST_CMD_FREQ_HOP_DISABLE);
	}

	/* a line a minute, at the final rate (the 12377 port lost this once: run ak7-t2, tick % 0, a compiler trap) */
	status_every = 60000000 / poll_us;
	if (status_every == 0) {
		status_every = 1;
	}
	uint64_t period = 0, next = mach_absolute_time(), status_at = next;
	uint64_t idle_period = 0, idle_after = 0;       /* touch_idle_hz */
	uint32_t status_polls = 0, status_fresh = 0;

	if (touch_hz != 0) {
		nanoseconds_to_absolutetime((uint64_t)poll_us * NSEC_PER_USEC, &period);
		PE_parse_boot_argn("touch_idle_hz", &touch_idle_hz, sizeof(touch_idle_hz));
		PE_parse_boot_argn("touch_idle_ms", &touch_idle_ms, sizeof(touch_idle_ms));
		if (touch_idle_hz != 0 && touch_idle_hz >= 10 && touch_idle_hz < touch_hz) {
			nanoseconds_to_absolutetime(NSEC_PER_SEC / touch_idle_hz, &idle_period);
			nanoseconds_to_absolutetime((uint64_t)(touch_idle_ms > 600000 ? 600000 : touch_idle_ms) * NSEC_PER_MSEC,
			    &idle_after);
			printf("nvt: touch_idle_hz=%u: %u looks a second after %u ms with no finger, %u again from the first report\n",
			    touch_idle_hz, touch_idle_hz, touch_idle_ms, touch_hz);
		} else if (touch_idle_hz != 0) {
			printf("nvt: touch_idle_hz=%u is outside 10..%u; ignored\n", touch_idle_hz, touch_hz - 1);
		}
	}
	for (unsigned tick = 1;; tick++) {
		uint64_t before = mach_absolute_time(), after;

		nvt_poll(before);
		after = mach_absolute_time();
		reading += after - before;
		reads++;
		if (tick % status_every == 0) {
			uint64_t ns, span;
			uint32_t looks = seen.polls - status_polls, fresh = seen.fresh - status_fresh;

			absolutetime_to_nanoseconds(reading / reads, &ns);
			absolutetime_to_nanoseconds(after - status_at, &span);
			status_at = after;
			status_polls = seen.polls;
			status_fresh = seen.fresh;
			printf("nvt: so far %u reports in %u looks (%u new, %u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us; every %u us: %u looks/s, %u new reports/s%s\n",
			    seen.reports, seen.polls, seen.fresh, seen.bad, seen.touches, seen.last_x, seen.last_y,
			    "polled, not used",
			    (unsigned long long)(ns / 1000), poll_us,
			    span != 0 ? (uint32_t)((uint64_t)looks * 1000000000ULL / span) : 0,
			    span != 0 ? (uint32_t)((uint64_t)fresh * 1000000000ULL / span) : 0,
			    touch_hz != 0 ? ", paced" : "");
		}
		if (touch_hz != 0) {
			/*
			 * The next look is due one period after the previous one was due, not one period after
			 * the work ended: the sleep is short, but the wake is late, and at 4 ms both matter.
			 */
			if (idle_period != 0) {
				uint64_t idle = vayu_touch_idle_abs();         /* (0: a finger is down; ~0: never touched) */

				next += idle != 0 && idle > idle_after ? idle_period : period;
			} else {
				next += period;
			}
			if (after < next) {
				uint64_t wait;

				absolutetime_to_nanoseconds(next - after, &wait);
				if (wait >= 1000) {
					sleep_us((uint32_t)(wait / 1000));
				}
			} else if (after - next > 4 * period) {
				next = after;           /* a long stall: start again from here, do not burst */
			}
		} else {
			sleep_us(poll_us);
		}
	}
}

/*
 * nvt_selftest=1: a check that needs no finger. The chip has answered its id and the firmware its info (nvt_probe);
 * here the info is read again and must be the same, and the event buffer is read nvt_selftest_looks times: every
 * read must answer (a dead bus or a chip in reset fails them), and with nobody touching the panel the reports are
 * blank or a finger the chip sees (counted apart). Nothing is written to the chip.
 */
/* one report in hex (the selftest's last): what an idle panel's report is made of */
static void
nvt_dump_one(const uint8_t *report)
{
	char line[3 * NVT_REPORT_SIZE + 1];
	char *p = line;

	for (unsigned i = 0; i < NVT_REPORT_SIZE; i++) {
		*p++ = "0123456789abcdef"[report[i] >> 4];
		*p++ = "0123456789abcdef"[report[i] & 0xf];
		*p++ = ' ';
	}
	*--p = 0;
	printf("nvt: selftest: the last report %s\n", line);
}

static void
nvt_selftest(void)
{
	uint8_t info[17], report[NVT_REPORT_SIZE];
	uint32_t looks = 240, answered = 0, blank = 0, fingers = 0, empty = 0;
	bool same;

	PE_parse_boot_argn("nvt_selftest_looks", &looks, sizeof(looks));
	if (looks == 0 || looks > 10000) {
		looks = 240;
	}
	same = nvt_fw_info(event_map, info) && (uint32_t)(info[4] << 8 | info[5]) == max_x &&
	    (uint32_t)(info[6] << 8 | info[7]) == max_y;
	nvt_set_page(event_map);
	page_held = true;
	for (uint32_t i = 0; i < looks; i++) {
		bool b = true;

		if (nvt_read(0x00, report, sizeof(report))) {
			bool finger = false;

			answered++;
			for (unsigned k = 0; k < sizeof(report); k++) {
				b &= report[k] == 0 || report[k] == 0xff;
			}
			/* a finger is a record nvt_report() would take: id 1..10, down or moving, inside the panel */
			for (unsigned r = 0; r < NVT_MAX_TOUCHES && !b; r++) {
				const uint8_t *rec = report + NVT_RECORD_SIZE * r;
				uint32_t id = rec[0] >> 3, kind = rec[0] & 7;
				uint32_t x = (uint32_t)rec[1] << 4 | rec[3] >> 4, y = (uint32_t)rec[2] << 4 | (rec[3] & 0x0f);

				finger |= id != 0 && id <= NVT_MAX_TOUCHES && (kind == 1 || kind == 2) && x <= max_x && y <= max_y;
			}
			if (b) {
				blank++;
			} else if (finger) {
				fingers++;
			} else {
				empty++;        /* data, but no finger in it (the slots' "up" records, the tail) */
			}
		}
		sleep_ms(4);
	}
	printf("nvt: selftest: chip %s, firmware info %s (panel %u x %u), %u of %u reads answered: %u blank, %u with no "
	    "finger in them, %u with a finger: %s\n", nvt_chip_name ? nvt_chip_name : "?",
	    same ? "the same on a second read" : "CHANGED or unread", max_x, max_y, answered, looks, blank, empty, fingers,
	    same && answered == looks ? "PASS" : "FAIL");
	nvt_dump_one(report);
}

/* From the K7 hook after devfs (bsd/dev/vayu_input.c): vayu27=1 is the runner's; touchscreen=1 nvt_qup=1 here. */
void
vayu_nvt_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!vayu_arg("touchscreen")) {
		return;
	}
	if (!vayu_arg("nvt_qup")) {
		printf("nvt: touchscreen=1 without nvt_qup=1: the bus is QUPv3 (the bit-banged path is not in this kernel); "
		    "nothing touched\n");
		return;
	}
	nvt_selftest_on = PE_parse_boot_argn("nvt_selftest", &on, sizeof(on)) && on != 0;
	printf("nvt: selected; QUP backend, polling thread%s\n", nvt_selftest_on ? ", selftest" : "");
	if (kernel_thread_start(nvt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
