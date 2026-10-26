/*
 * NFC on the POCO X3 Pro (vayu, SM8150): the NXP PN553, steps 1-4.
 *
 * Step 1 reads the five pins the controller uses; steps 2-4 talk NCI to the chip over I2C
 * done by hand on two of those pins. Nothing happens unless boot-arg vayu_nfc=N is present,
 * and nothing is written anywhere unless the operator has also given vayu_nfc_xnu_tlmm=1
 * (this port's TLMM exception) and the loader has already muxed the two I2C pins
 * (vayu_nfc_pins=1).
 *
 * The facts, with their sources:
 *
 *   the chip    NXP PN553, I2C address 0x28 (8-bit write 0x50, read 0x51). The vendor HAL's
 *               libnfc-nxp.conf says NXP_NFC_CHIP=0x07 (TheMuppets/proprietary_vendor_xiaomi_vayu,
 *               lineage-22.2, libnfc-nxp.conf:241); the clock it wants is NXP_SYS_CLK_SRC_SEL=0x02
 *               (PLL) at 19.2 MHz (:39, :50).
 *   the bus     i2c@a84000, QUP serial engine 9. Live DT firmware/vayu/dt/recovery-live.dtb,
 *               decompiled live.dts:23428-23440: nq@28 with qcom,nq-irq GPIO 47, qcom,nq-ven
 *               GPIO 102, qcom,nq-firm GPIO 48 and qcom,nq-clkreq GPIO 113. The bus's own pins:
 *               qupv3_se9_i2c_active, live.dts:12617-12631: GPIO 39 SDA, GPIO 40 SCL, function
 *               qup9.
 *   TLMM        mainline pinctrl-sm8150.c: NORTH tile 0x03900000 (pins 39, 40, 102), EAST tile
 *               0x03500000 (pins 47, 48), one 0x1000-byte block per pin (ctl_reg = 0x1000 * id):
 *               cfg at +0x0 and io at +0x4, as the other vayu drivers read it. Cfg bits, as
 *               vayu_audio.c's show_pins() decodes them: [1:0] pull, [5:2] function,
 *               [8:6] drive ((n + 1) * 2 mA), [9] output enable; io bit 0 is the input level
 *               and bit 1 the output value.
 *   the vendor  MiCode vayu-r-oss drivers/nfc/nq-nci.c (0c9888aa): the IRQ is rising edge and
 *               the host reads only while it is high (:219-245, :1479); after every write it
 *               sleeps 1 ms (:331); power on is FIRM 0, then VEN 1, 10 ms each (:637-646).
 *
 * This port's rule: XNU does not write TLMM. TLMM exception 6 (maintainers, 2026-10-25; the bring-up notes), behind
 * vayu_nfc_xnu_tlmm=1, is this driver: XNU may write ONLY pins 39 and 40, ONLY cfg bit 9 (output
 * enable) and io bit 1, for I2C by hand at <= 100 kHz to address 0x28; only if the pin's function
 * field already reads 0 (the loader muxed it, vayu_nfc_pins=1); the pull bits are preserved as
 * found; and the two pins are given back exactly as found after every transaction. Pins 47, 48
 * and 102 are only ever READ by XNU. GPIO 113 is never touched and its tile is not even mapped:
 * the west tile at 0x03100000 has wedged this phone before. FIRM is never raised: there is no
 * firmware download and nothing is ever written to the chip's flash.
 *
 * The registers mapped, per pin block (one small window per pin block):
 *   ml_io_map(NORTH + 0x1000 * 39, 0x2000)   pins 39 (SDA) and 40 (SCL)
 *   ml_io_map(NORTH + 0x1000 * 102, 0x1000)  pin 102 (VEN)
 *   ml_io_map(EAST + 0x1000 * 47, 0x2000)    pins 47 (IRQ) and 48 (FIRM)
 * No QUP, GENI or GCC register is touched at all.
 *
 * Boot-args:
 *   vayu_nfc=N             0 or absent: nothing runs. Otherwise steps 1..N; the thread stops at
 *                          the first step that fails and prints "done (reached step K of N)".
 *   vayu_nfc_xnu_tlmm=1    required for steps >= 2; without it the thread stops after step 1.
 *   vayu_nfc_secs=S        step 3 polls for tags for S seconds (default 120, max 3600).
 *   vayu_nfc_tech=MASK     bit 0 NFC-A, bit 1 NFC-B, bit 2 NFC-F, bit 3 NFC-V (ISO15693);
 *                          default 0x7.
 *   vayu_nfc_pause=MS      sleep before step 1, time for the loader's log (default 2000).
 *
 * What each step does:
 *   1  reads pins 39, 40, 47, 48 and 102 into the loader's log and prints a verdict: VEN on,
 *      FIRM low, IRQ level and whether the two I2C pins are plain GPIO. It never fails on the
 *      values; it only fails if the TLMM windows cannot be mapped.
 *   2  NCI core: drains the IRQ, CORE_RESET (keep configuration) and its NTF, CORE_INIT, and
 *      the NXP proprietary extension. Prints the NCI version, manufacturer, HW/ROM/FW versions
 *      and RF interfaces (NCI 1.x) or the raw payload (2.x).
 *   3  RF discovery: maps the protocols, starts polling with the technologies of
 *      vayu_nfc_tech, decodes every activation's UID, deactivates back to discovery, and ends
 *      with RF_DEACTIVATE to idle.
 *   4  when a Type 2 tag (NTAG/Ultralight) is activated in the Frame interface: one READ of
 *      page 4 (16 bytes), read-only -- no WRITE and no other tag command is ever sent.
 *
 * Sleeping is delay_for_interval(n, NSEC_PER_MSEC), as everywhere in this port's drivers; the
 * I2C half-bit is a spin (spin_us), as in vayu_audio.c. Every line is prefixed "vayu-nfc: "
 * and copied to the loader's log by say() -- if the kernel dies, the last line says where.
 */

#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vayu_nfc.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

/* ---- the TLMM tiles and the pins -------------------------------------------------------------- */

#define TLMM_NORTH_PHYS         0x03900000UL    /* pinctrl-sm8150.c: pins 39, 40, 102 */
#define TLMM_EAST_PHYS          0x03500000UL    /* pinctrl-sm8150.c: pins 47, 48 */
#define TLMM_PIN_STRIDE         0x1000UL        /* ctl_reg = 0x1000 * id (pinctrl-sm8150.c) */

#define PIN_SDA                 39              /* qupv3_se9_i2c_active: live.dts:12617-12631 */
#define PIN_SCL                 40
#define PIN_IRQ                 47              /* qcom,nq-irq: live.dts:23428-23440 */
#define PIN_FIRM                48              /* qcom,nq-firm */
#define PIN_VEN                 102             /* qcom,nq-ven */

/* Cfg and io bits, as vayu_audio.c decodes them. */
#define TLMM_CFG_PULL_MASK      0x3u            /* cfg [1:0] pull, preserved as found */
#define TLMM_CFG_FUNC_SHIFT     2               /* cfg [5:2] function */
#define TLMM_CFG_FUNC_MASK      0xfu
#define TLMM_CFG_DRIVE_SHIFT    6               /* cfg [8:6] drive: (n + 1) * 2 mA */
#define TLMM_CFG_DRIVE_MASK     0x7u
#define TLMM_CFG_OE             (1u << 9)       /* cfg [9] output enable */
#define TLMM_IO_IN              0x1u            /* io bit 0: the level read at the pad */
#define TLMM_IO_OUT             0x2u            /* io bit 1: the output value */

/* ---- the controller and the bus ---------------------------------------------------------------- */

#define NFC_ADDRESS             0x28            /* PN553, live.dts nq@28 */
#define NFC_ADDRESS_WRITE       (NFC_ADDRESS << 1)      /* 0x50 */
#define NFC_ADDRESS_READ        ((NFC_ADDRESS << 1) | 1)        /* 0x51 */

#define I2C_HALF_BIT_US         5               /* 100 kHz at most */
#define I2C_STRETCH_HALF_BITS   100             /* clock stretching: 100 half bits, as vayu_audio.c */

/*
 * A transaction's deadline: 10 ms of overhead plus 200 us per byte. A byte takes ~100 us at
 * 100 kHz, so a 258-byte read (~26 ms) has room for preemption of this thread mid-transaction.
 */
#define I2C_DEADLINE_US         10000
#define I2C_BYTE_US             200

#define NFC_BUF                 260             /* every packet buffer is this size (hard rule) */
#define NFC_SHOW_BYTES          48              /* at most this many bytes of a packet are shown */

/* NCI's message types and group ids (NCI 1.0/2.0). */
#define NCI_MT_RSP              0x40            /* response (MT 2) */
#define NCI_MT_NTF              0x60            /* notification (MT 3) */
#define NCI_MT_DATA             0x00            /* data packet (MT 0) */
#define NCI_GID_CORE            0x00            /* core group */
#define NCI_GID_RF              0x01            /* RF management group */
#define NCI_GID_PROP            0x0f            /* NXP's proprietary group: 2F/4F */

/* NCI's technology-and-mode values (step 3's discovery and activation payloads). */
#define NCI_TECH_A              0x00            /* NFC-A poll */
#define NCI_TECH_B              0x01            /* NFC-B poll */
#define NCI_TECH_F              0x02            /* NFC-F poll */
#define NCI_TECH_V              0x06            /* NFC-V poll (HYPOTHESIS: NCI 2.0's value) */

/* NCI's interface values. */
#define NCI_IF_FRAME            0x01            /* Frame interface: T1T, T2T, T3T */
#define NCI_IF_ISO_DEP          0x02            /* ISO-DEP interface */
#define NCI_IF_TAG_CMD          0x80            /* NXP's tag command interface for MIFARE Classic */

/* The tag command of step 4: READ page 4, two bytes, read-only. */
#define T2T_READ_PAGE4          0x30

/* ---- the boot-args ----------------------------------------------------------------------------- */

static uint32_t nfc_level, nfc_level_wanted = 1;        /* vayu_nfc=N, N as given */
static uint32_t nfc_xnu_tlmm;                           /* vayu_nfc_xnu_tlmm=1 */
static uint32_t nfc_secs = 120;                         /* vayu_nfc_secs=S */
static uint32_t nfc_tech = 0x7;                         /* vayu_nfc_tech=MASK */
static uint32_t nfc_pause = 2000;                       /* vayu_nfc_pause=MS */
static bool nfc_step4_read_ok;                          /* a Type 2 tag was read in step 4 */
static bool nfc_step4_failed;                           /* the Type 2 read was attempted and failed */

/* ---- output ------------------------------------------------------------------------------------ */

/*
 * One line: the prefix, the caller's text, the kernel's log and the loader's. The loader reads
 * that log from another core, so the last line survives a kernel that dies mid-step.
 */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char text[300], line[320];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(text, sizeof(text), format, list);
	va_end(list);
	snprintf(line, sizeof(line), "vayu-nfc: %s", text);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		printf("%s", line);
	}
	return length;
}

/* ---- reading and writing ----------------------------------------------------------------------- */

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

/* A busy wait for the I2C half-bit (vayu_audio.c: spin_us). */
static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* Milliseconds since a mach_absolute_time() stamp. */
static uint64_t
ms_since(uint64_t since)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
	return ns / NSEC_PER_MSEC;
}

/* ---- the pin windows --------------------------------------------------------------------------- */

static vm_offset_t north_i2c;           /* pins 39..40, ml_io_map(NORTH + 0x1000 * 39, 0x2000) */
static vm_offset_t north_ven;           /* pin 102,    ml_io_map(NORTH + 0x1000 * 102, 0x1000) */
static vm_offset_t east_nfc;            /* pins 47..48, ml_io_map(EAST + 0x1000 * 47, 0x2000) */

static vm_offset_t
pin_window(unsigned pin)
{
	switch (pin) {
	case PIN_SDA:
	case PIN_SCL:
		return north_i2c;
	case PIN_IRQ:
	case PIN_FIRM:
		return east_nfc;
	case PIN_VEN:
		return north_ven;
	}
	return 0;
}

static unsigned
pin_base(unsigned pin)
{
	if (pin == PIN_IRQ || pin == PIN_FIRM) {
		return PIN_IRQ;
	}
	if (pin == PIN_VEN) {
		return PIN_VEN;
	}
	return PIN_SDA;
}

static vm_offset_t
gpio_cfg(unsigned pin)                  /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
{
	return pin_window(pin) + TLMM_PIN_STRIDE * (pin - pin_base(pin));
}

static vm_offset_t
gpio_io(unsigned pin)                   /* bit 0 in, bit 1 out */
{
	return gpio_cfg(pin) + 4;
}

static bool
pin_high(unsigned pin)
{
	return (rd(gpio_io(pin)) & TLMM_IO_IN) != 0;
}

static unsigned
pin_function(unsigned pin)
{
	return (rd(gpio_cfg(pin)) >> TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK;
}

/* ---- I2C by hand (vayu_audio.c lines 208-336, with this bus's rules) ----------------------------- */

static uint32_t taken_cfg[2], taken_io[2];
static uint64_t i2c_deadline;           /* mach_absolute_time() past which a transaction gives up */
static bool i2c_late;
static bool i2c_quiet;                  /* step 3's long polling: no per-transaction pin line */
static unsigned bus_fail_run;           /* bus_take() failures in a row */
static bool bus_dead;                   /* three in a row: the step stops */

static void
half_bit(void)
{
	spin_us(I2C_HALF_BIT_US);       /* 100 kHz at most */
}

/*
 * A line is pulled low (the pin drives 0) or let go (the pin is an input and the bus's pull-ups
 * raise it). Function 0 (GPIO); the pull bits are the ones found on the pin when it was taken
 * (this port's exception: pull bits preserved); only cfg bit 9 and io bit 1 are written.
 */
static void
line(unsigned pin, bool high)
{
	unsigned slot = (pin == PIN_SDA) ? 0 : 1;

	wr(gpio_io(pin), 0);            /* io bit 1 = 0; the input bit reads back */
	wr(gpio_cfg(pin), high ? (taken_cfg[slot] & ~TLMM_CFG_OE) : (taken_cfg[slot] | TLMM_CFG_OE));
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
	for (unsigned stretched = 0; !pin_high(PIN_SCL) && stretched < I2C_STRETCH_HALF_BITS; stretched++) {
		half_bit();
	}
	half_bit();
	in = pin_high(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

/* Returns true if the byte was acknowledged. */
static bool
i2c_write_byte(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read_byte(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

/* A transaction's deadline: 10 ms + 200 us per byte (a byte takes ~100 us at 100 kHz; the slack covers preemption). */
static void
i2c_begin(unsigned bytes)
{
	uint64_t limit;

	nanoseconds_to_absolutetime(I2C_DEADLINE_US * NSEC_PER_USEC + (uint64_t)bytes * I2C_BYTE_US * NSEC_PER_USEC, &limit);
	i2c_deadline = mach_absolute_time() + limit;
	i2c_late = false;
}

static void
bus_failed(void)
{
	if (++bus_fail_run >= 3) {
		bus_dead = true;
		say("i2c: bus_take failed %u times in a row: stopping\n", bus_fail_run);
	}
}

/* The two pins taken from the serial engine for one transaction. False if they are not GPIO or the bus is not at rest. */
static bool
bus_take(void)
{
	unsigned function_sda, function_scl;

	taken_cfg[0] = rd(gpio_cfg(PIN_SDA));
	taken_cfg[1] = rd(gpio_cfg(PIN_SCL));
	taken_io[0] = rd(gpio_io(PIN_SDA));
	taken_io[1] = rd(gpio_io(PIN_SCL));
	function_sda = (taken_cfg[0] >> TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK;
	function_scl = (taken_cfg[1] >> TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK;
	if (function_sda != 0 || function_scl != 0) {
		say("i2c: gpio%u function %u, gpio%u function %u: not taken (need function 0, loader vayu_nfc_pins=1)\n",
		    PIN_SDA, function_sda, PIN_SCL, function_scl);
		bus_failed();
		return false;
	}
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!pin_high(PIN_SDA) || !pin_high(PIN_SCL)) {
		say("i2c: the bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n",
		    PIN_SDA, pin_high(PIN_SDA), PIN_SCL, pin_high(PIN_SCL));
		wr(gpio_io(PIN_SDA), taken_io[0] & TLMM_IO_OUT);
		wr(gpio_io(PIN_SCL), taken_io[1] & TLMM_IO_OUT);
		wr(gpio_cfg(PIN_SDA), taken_cfg[0]);
		wr(gpio_cfg(PIN_SCL), taken_cfg[1]);
		bus_failed();
		return false;
	}
	bus_fail_run = 0;
	return true;
}

static void
bus_give_back(void)
{
	wr(gpio_io(PIN_SDA), taken_io[0] & TLMM_IO_OUT);
	wr(gpio_io(PIN_SCL), taken_io[1] & TLMM_IO_OUT);
	wr(gpio_cfg(PIN_SDA), taken_cfg[0]);
	wr(gpio_cfg(PIN_SCL), taken_cfg[1]);
}

/* ---- logging the NCI packets ------------------------------------------------------------------- */

static void
hex_string(const uint8_t *bytes, unsigned count, char *out, size_t capacity, unsigned limit)
{
	size_t used = 0;
	unsigned shown = count < limit ? count : limit;

	out[0] = 0;
	for (unsigned i = 0; i < shown; i++) {
		int wrote;

		if (used + 4 >= capacity) {
			break;
		}
		wrote = snprintf(out + used, capacity - used, "%s%02x", i ? " " : "", bytes[i]);
		if (wrote < 0) {
			break;
		}
		used += (size_t)wrote;
	}
	if (shown < count && used + 5 < capacity) {
		snprintf(out + used, capacity - used, " ...");
	}
}

/* Every packet, sent or received, as hex: at most 48 bytes, then "...". */
static void
log_packet(const char *direction, const uint8_t *bytes, unsigned count)
{
	char shown[200];

	hex_string(bytes, count, shown, sizeof(shown), NFC_SHOW_BYTES);
	say("%s %u: %s\n", direction, count, shown);
}

/* ---- the I2C transactions ---------------------------------------------------------------------- */

/*
 * START, 0x50, the bytes, STOP. On an address NACK up to three more attempts, 1 ms apart: a
 * PN553 in standby needs a wake-up (vendor nq-nci.c:331 has the 1 ms after every write).
 */
static bool
nfc_write(const uint8_t *bytes, unsigned count)
{
	bool answered = false, address_ok = false;

	if (count == 0 || count > 255 || bus_dead) {
		return false;
	}
	for (unsigned attempt = 0; attempt < 4; attempt++) {
		if (attempt > 0) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
		i2c_begin(1 + count);
		if (!bus_take()) {
			return false;
		}
		i2c_start();
		address_ok = i2c_write_byte(NFC_ADDRESS_WRITE);
		answered = address_ok;
		for (unsigned i = 0; answered && i < count; i++) {
			answered = i2c_write_byte(bytes[i]);
		}
		i2c_stop();
		bus_give_back();
		if (!i2c_quiet) {
			say("i2c: write 0x28 <- %u bytes: %s; pins given back (cfg %#x/%#x)\n", count,
			    i2c_late ? "TIMED OUT" : answered ? "acknowledged" : address_ok ? "data not acknowledged" :
			    "address not acknowledged", rd(gpio_cfg(PIN_SDA)), rd(gpio_cfg(PIN_SCL)));
		}
		if (i2c_late) {
			say("i2c: write 0x28 <- %u bytes: deadline passed\n", count);
			return false;
		}
		if (answered) {
			log_packet("tx", bytes, count);
			delay_for_interval(1, NSEC_PER_MSEC);   /* vendor nq-nci.c:331 */
			return true;
		}
		if (address_ok) {
			say("i2c: write 0x28 <- %u bytes: data not acknowledged\n", count);
			return false;
		}
		if (attempt < 3) {
			say("i2c: write 0x28: address not acknowledged, retry %u of 3 in 1 ms (NXP standby wake-up)\n",
			    attempt + 1);
		}
	}
	say("i2c: write 0x28: address not acknowledged after 4 attempts\n");
	return false;
}

/* START, 0x51, count bytes (ACK all but the last), STOP. */
static bool
nfc_read(uint8_t *bytes, unsigned count)
{
	bool answered;

	if (count == 0 || bus_dead) {
		return false;
	}
	i2c_begin(1 + count);
	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write_byte(NFC_ADDRESS_READ);
	for (unsigned i = 0; answered && i < count; i++) {
		bytes[i] = i2c_read_byte(i + 1 < count);
	}
	i2c_stop();
	bus_give_back();
	if (!i2c_quiet) {
		say("i2c: read 0x28 -> %u bytes: %s; pins given back (cfg %#x/%#x)\n", count,
		    i2c_late ? "TIMED OUT" : answered ? "acknowledged" : "not acknowledged",
		    rd(gpio_cfg(PIN_SDA)), rd(gpio_cfg(PIN_SCL)));
	}
	if (i2c_late) {
		say("i2c: read 0x28 -> %u bytes: deadline passed\n", count);
		return false;
	}
	if (!answered) {
		say("i2c: read 0x28 -> %u bytes: not acknowledged\n", count);
	}
	return answered;
}

/* ---- the IRQ and the receive path --------------------------------------------------------------- */

static bool
irq_high(void)
{
	return (rd(gpio_io(PIN_IRQ)) & TLMM_IO_IN) != 0;
}

/*
 * Wait (1 ms at a time) until the IRQ is high, then read the 3-byte header and, if it says so,
 * the payload in a second transaction. Returns the total length, 0 on timeout or failure.
 * Every packet is logged as "rx %u: ...".
 */
static unsigned
nfc_recv(uint8_t *packet, unsigned capacity, unsigned timeout_ms)
{
	uint8_t header[3];
	uint64_t start;
	unsigned length;

	if (capacity < 3) {
		return 0;
	}
	start = mach_absolute_time();
	while (!irq_high()) {
		if (ms_since(start) >= timeout_ms) {
			return 0;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (!nfc_read(header, 3)) {
		say("rx: the 3-byte header is not read\n");
		return 0;
	}
	length = header[2];
	if (3 + (unsigned)length > capacity) {
		say("rx: %u bytes do not fit the %u-byte buffer: dropped\n", 3 + length, capacity);
		return 0;
	}
	memcpy(packet, header, 3);
	if (length > 0 && !nfc_read(packet + 3, length)) {
		say("rx: the %u-byte payload is not read\n", length);
		return 0;
	}
	log_packet("rx", packet, 3 + length);
	return 3 + length;
}

/* Wait for one control message with this first byte and OID; anything else is logged and skipped. */
static unsigned
wait_rsp(uint8_t first, uint8_t oid, uint8_t *packet, unsigned capacity, unsigned timeout_ms)
{
	uint64_t start = mach_absolute_time();

	while (ms_since(start) < timeout_ms) {
		unsigned left = timeout_ms - (unsigned)ms_since(start);
		unsigned length = nfc_recv(packet, capacity, left < 100 ? left : 100);

		if (bus_dead) {
			return 0;
		}
		if (length == 0) {
			continue;
		}
		if (length >= 3 && packet[0] == first && packet[1] == oid) {
			return length;
		}
		say("waiting for %02x %02x: got %02x %02x (%u bytes), kept going\n", first, oid, packet[0],
		    packet[1], length);
	}
	return 0;
}

/* ---- the pins, step 1 -------------------------------------------------------------------------- */

static bool
ven_is_on(void)
{
	uint32_t cfg = rd(gpio_cfg(PIN_VEN)), io = rd(gpio_io(PIN_VEN));

	return ((cfg >> TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK) == 0 && (cfg & TLMM_CFG_OE) != 0 &&
	    (io & TLMM_IO_OUT) != 0;
}

static const char *
firm_state(void)
{
	uint32_t cfg = rd(gpio_cfg(PIN_FIRM)), io = rd(gpio_io(PIN_FIRM));

	if (((cfg >> TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK) != 0) {
		return "not low";
	}
	if (cfg & TLMM_CFG_OE) {
		return (io & TLMM_IO_OUT) ? "not low" : "low";
	}
	return (io & TLMM_IO_IN) ? "not low" : "input low";
}

/* FIRM is low both when driven low and when it is an input that reads low. */
static bool
firm_is_low(void)
{
	uint32_t cfg = rd(gpio_cfg(PIN_FIRM)), io = rd(gpio_io(PIN_FIRM));

	if (((cfg >> TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK) != 0) {
		return false;
	}
	if (cfg & TLMM_CFG_OE) {
		return (io & TLMM_IO_OUT) == 0;
	}
	return (io & TLMM_IO_IN) == 0;
}

static bool
i2c_pins_are_gpio(void)
{
	return pin_function(PIN_SDA) == 0 && pin_function(PIN_SCL) == 0;
}

static int
step1(void)
{
	static const struct {
		unsigned pin;
		const char *name;
	} pins[] = {
		{ PIN_SDA, "SDA" }, { PIN_SCL, "SCL" }, { PIN_IRQ, "IRQ" }, { PIN_FIRM, "FIRM" },
		{ PIN_VEN, "VEN" },
	};
	char i2c[80];

	say("1: the PN553's pins, read-only\n");
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(gpio_cfg(pins[i].pin));
		uint32_t io = rd(gpio_io(pins[i].pin));
		unsigned function = (cfg >> TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK;
		unsigned drive = (cfg >> TLMM_CFG_DRIVE_SHIFT) & TLMM_CFG_DRIVE_MASK;

		say("1: gpio%u (%s): cfg %#x: function %u, pull %u, %u mA, oe %u, out %u; io %#x: in %u\n",
		    pins[i].pin, pins[i].name, cfg, function, cfg & TLMM_CFG_PULL_MASK, (drive + 1) * 2,
		    (cfg >> 9) & 1, (io & TLMM_IO_OUT) ? 1 : 0, io, (io & TLMM_IO_IN) ? 1 : 0);
	}
	if (i2c_pins_are_gpio()) {
		snprintf(i2c, sizeof(i2c), "%s", "gpio (function 0)");
	} else {
		snprintf(i2c, sizeof(i2c), "function %u/%u (loader vayu_nfc_pins=1 not run)",
		    pin_function(PIN_SDA), pin_function(PIN_SCL));
	}
	say("1: ven %s, firm %s, irq %s, i2c pins %s\n", ven_is_on() ? "on" : "off", firm_state(),
	    irq_high() ? "high" : "low", i2c);

	return 0;
}

/* ---- step 2: NCI core -------------------------------------------------------------------------- */

static const char *
nci_version_name(uint8_t version)
{
	switch (version) {
	case 0x10: return "1.0";
	case 0x11: return "1.1";
	case 0x20: return "2.0";
	}
	return "unknown";
}

static unsigned nci_max_control_payload = 255;  /* kept for later, from CORE_INIT_RSP */

/*
 * CORE_INIT_RSP, NCI 2.x layout: status (1), NFCC features (4), max logical connections (1),
 * max routing table size (2), max control packet payload (1).  The 1.x response has the RF
 * interface list in between and is decoded in step2() itself.
 */
static void
keep_max_control_payload(const uint8_t *packet)
{
	if (packet[2] >= 9) {
		nci_max_control_payload = packet[11];   /* payload[8] */
	}
}

static int
step2(void)
{
	uint8_t packet[NFC_BUF];
	uint8_t version = 0, status;
	unsigned length, interfaces = 0;
	bool have_version = false;
	uint64_t start;

	say("2: NCI core\n");

	/* 1. Drain what the loader or the chip left behind: at most four packets. */
	for (unsigned i = 0; i < 4 && irq_high(); i++) {
		length = nfc_recv(packet, sizeof(packet), 100);
		if (length == 0) {
			break;
		}
		say("2: left over: %u bytes\n", length);
	}
	if (bus_dead) {
		say("2: the I2C bus failed 3 times in a row: stopping\n");
		return -1;
	}

	/* 2. CORE_RESET_CMD, keep configuration. */
	{
		static const uint8_t command[] = { 0x20, 0x00, 0x01, 0x00 };

		if (!nfc_write(command, sizeof(command))) {
			say("2: CORE_RESET_CMD is not sent%s\n", bus_dead ? " (the bus failed 3 times in a row)" : "");
			return -1;
		}
	}
	length = wait_rsp(NCI_MT_RSP | NCI_GID_CORE, 0x00, packet, sizeof(packet), 100);
	if (length < 4 || packet[2] < 1) {
		say("2: CORE_RESET_CMD: no response within 100 ms%s\n", bus_dead ? " (the bus failed 3 times)" : "");
		return -1;
	}
	status = packet[3];
	if (status != 0x00) {
		say("2: CORE_RESET_RSP: status %#x: failed\n", status);
		return -1;
	}
	if (packet[2] == 3) {
		/* 40 00 03 ss vv cc: NCI 1.0 style. */
		version = packet[4];
		have_version = true;
		say("2: CORE_RESET_RSP: status %#x, NCI version %#x (%s), configuration %#x\n", status, version,
		    nci_version_name(version), packet[5]);
	} else if (packet[2] == 1) {
		/* 40 00 01 ss, then a 60 00 ... in NCI 2.0 style. */
		say("2: CORE_RESET_RSP: status %#x, NCI 2.0 style; collecting notifications for 50 ms\n", status);
	} else {
		char shown[200];

		hex_string(packet, length, shown, sizeof(shown), NFC_SHOW_BYTES);
		say("2: CORE_RESET_RSP: %u-byte payload, not decoded: %s\n", packet[2], shown);
	}

	/* Further packets for 50 ms: the 2.0-style NTF, or NXP vendor notifications. */
	start = mach_absolute_time();
	while (ms_since(start) < 50) {
		length = nfc_recv(packet, sizeof(packet), 10);
		if (bus_dead) {
			say("2: the I2C bus failed 3 times in a row: stopping\n");
			return -1;
		}
		if (length == 0) {
			continue;
		}
		if (length >= 8 && packet[0] == (NCI_MT_NTF | NCI_GID_CORE) && packet[1] == 0x00 &&
		    packet[2] >= 5) {
			unsigned info_length = packet[7];
			unsigned available = packet[2] - 5;
			char shown[200];

			if (info_length > available) {
				info_length = available;
			}
			version = packet[5];
			have_version = true;
			hex_string(packet + 8, info_length, shown, sizeof(shown), NFC_SHOW_BYTES);
			say("2: CORE_RESET_NTF: trigger %#x, configuration %#x, NCI version %#x (%s), "
			    "manufacturer %#x, info %u bytes: %s\n", packet[3], packet[4], version,
			    nci_version_name(version), packet[6], info_length, shown);
		} else {
			say("2: another packet during the reset's 50 ms: %u bytes\n", length);
		}
	}
	if (!have_version) {
		say("2: no NCI version was reported: CORE_INIT is not sent\n");
		return -1;
	}

	/* 3. CORE_INIT_CMD: 20 01 02 00 00 for 2.x, 20 01 00 for 1.x. */
	{
		static const uint8_t init_1x[] = { 0x20, 0x01, 0x00 };
		static const uint8_t init_2x[] = { 0x20, 0x01, 0x02, 0x00, 0x00 };
		bool two = (version >> 4) >= 2;
		const uint8_t *command = two ? init_2x : init_1x;
		unsigned command_length = two ? sizeof(init_2x) : sizeof(init_1x);

		if (!nfc_write(command, command_length)) {
			say("2: CORE_INIT_CMD is not sent%s\n", bus_dead ? " (the bus failed 3 times in a row)" : "");
			return -1;
		}
	}
	length = wait_rsp(NCI_MT_RSP | NCI_GID_CORE, 0x01, packet, sizeof(packet), 100);
	if (length < 4 || packet[2] < 1) {
		say("2: CORE_INIT_CMD: no response within 100 ms%s\n", bus_dead ? " (the bus failed 3 times)" : "");
		return -1;
	}
	status = packet[3];
	if (status != 0x00) {
		say("2: CORE_INIT_RSP: status %#x: failed\n", status);
		return -1;
	}
	if ((version >> 4) >= 2) {
		char shown[200];

		keep_max_control_payload(packet);
		hex_string(packet + 3, packet[2], shown, sizeof(shown), NFC_SHOW_BYTES);
		say("2: NCI 2.%u, CORE_INIT_RSP payload: %s\n", version & 0xf, shown);
	} else if (length >= 9) {
		char shown[200];

		interfaces = packet[8];
		if (length < 20 + interfaces) {
			hex_string(packet + 3, packet[2], shown, sizeof(shown), NFC_SHOW_BYTES);
			say("2: CORE_INIT_RSP: %u bytes is short for %u RF interfaces: %s\n", length, interfaces, shown);
		} else {
			static char list[100];
			size_t used = 0;
			unsigned manufacturer, hardware, rom, firmware_major, firmware_minor, maximum;

			for (unsigned i = 0; i < interfaces; i++) {
				int wrote = snprintf(list + used, sizeof(list) - used, "%s%#x", i ? ", " : "",
				    packet[9 + i]);

				if (wrote < 0) {
					break;
				}
				used += (size_t)wrote;
				if (used >= sizeof(list)) {
					break;
				}
			}
			maximum = packet[12 + interfaces];
			manufacturer = packet[15 + interfaces];
			hardware = packet[16 + interfaces];
			rom = packet[17 + interfaces];
			firmware_major = packet[18 + interfaces];
			firmware_minor = packet[19 + interfaces];
			nci_max_control_payload = maximum;
			say("2: NCI %u.%u, manufacturer %#x, hw %#x rom %#x fw %02x.%02x, rf interfaces: %s\n",
			    version >> 4, version & 0xf, manufacturer, hardware, rom, firmware_major,
			    firmware_minor, list[0] ? list : "none");
		}
	} else {
		char shown[200];

		hex_string(packet + 3, packet[2], shown, sizeof(shown), NFC_SHOW_BYTES);
		say("2: CORE_INIT_RSP: %u bytes, too short to decode: %s\n", packet[2], shown);
	}
	say("2: max control packet payload %u\n", nci_max_control_payload);

	/* 4. NXP proprietary extension on: 2F 02 00 (libnfc-nxp.conf:59 NXP_ACT_PROP_EXTN). */
	{
		static const uint8_t command[] = { 0x2F, 0x02, 0x00 };
		char shown[200];

		if (!nfc_write(command, sizeof(command))) {
			say("2: NXP_ACT_PROP_EXTN is not sent (not fatal)\n");
		} else {
			length = wait_rsp(NCI_MT_RSP | NCI_GID_PROP, 0x02, packet, sizeof(packet), 100);
			if (length == 0) {
				say("2: NXP_ACT_PROP_EXTN: no response within 100 ms (not fatal)\n");
			} else {
				hex_string(packet, length, shown, sizeof(shown), NFC_SHOW_BYTES);
				say("2: NXP_ACT_PROP_EXTN: %u bytes: %s\n", length, shown);
			}
		}
	}
	if (bus_dead) {
		say("2: the I2C bus failed 3 times in a row: stopping\n");
		return -1;
	}
	say("2: core up\n");
	return 0;
}

/* ---- tags -------------------------------------------------------------------------------------- */

struct tag_info {
	uint8_t tech;
	uint8_t protocol;
	uint8_t uid[10];
	unsigned uid_length;
	bool reverse_uid;               /* NFC-V: the UID is printed MSB first */
	bool have_sak;
	uint8_t sak;
	uint8_t atqa[2];
	uint8_t dsfid;
};

static const char *
tech_name(uint8_t tech)
{
	switch (tech) {
	case NCI_TECH_A: return "NFC-A poll";
	case NCI_TECH_B: return "NFC-B poll";
	case NCI_TECH_F: return "NFC-F poll";
	case NCI_TECH_V: return "NFC-V poll";
	}
	return "unknown";
}

static const char *
protocol_name(uint8_t protocol)
{
	static char unknown[16];

	switch (protocol) {
	case 0x01: return "T1T";
	case 0x02: return "T2T";
	case 0x03: return "T3T";
	case 0x04: return "ISO-DEP";
	case 0x05: return "NFC-DEP";
	case 0x06: return "T5T";
	case 0x80: return "MIFARE Classic";
	}
	snprintf(unknown, sizeof(unknown), "%#x", protocol);
	return unknown;
}

/*
 * Decode the technology-specific parameters of an activation or a discovery notification.
 * Every index is checked against the received length; false means "print the raw bytes".
 */
static bool
tag_decode(uint8_t tech, const uint8_t *parameters, unsigned length, struct tag_info *info)
{
	memset(info, 0, sizeof(*info));
	info->tech = tech;
	switch (tech) {
	case NCI_TECH_A: {
		unsigned id_length, offset, sel_length;

		/* SENS_RES (2), NFCID1 length (1), NFCID1, SEL_RES length (1), SEL_RES. */
		if (length < 3) {
			return false;
		}
		info->atqa[0] = parameters[0];
		info->atqa[1] = parameters[1];
		id_length = parameters[2];
		if (id_length > sizeof(info->uid) || 3 + id_length > length) {
			return false;
		}
		info->uid_length = id_length;
		memcpy(info->uid, parameters + 3, id_length);
		offset = 3 + id_length;
		if (offset >= length) {
			return false;
		}
		sel_length = parameters[offset];
		if (sel_length >= 1 && offset + 1 < length) {
			info->have_sak = true;
			info->sak = parameters[offset + 1];
		}
		return info->uid_length != 0;
	}
	case NCI_TECH_B: {
		unsigned res_length;

		/* SENSB_RES length (1), SENSB_RES: byte 0 is 0x50, bytes 1-4 the PUPI/NFCID0. */
		if (length < 1) {
			return false;
		}
		res_length = parameters[0];
		if (res_length < 5 || 1 + res_length > length || parameters[1] != 0x50) {
			return false;
		}
		info->uid_length = 4;
		memcpy(info->uid, parameters + 2, 4);
		return true;
	}
	case NCI_TECH_F: {
		unsigned res_length;

		/* Bit rate (1), SENSF_RES length (1), SENSF_RES (byte 0 = 0x01, NFCID2 = bytes 1..8). */
		if (length < 2) {
			return false;
		}
		res_length = parameters[1];
		if (res_length < 1 || 2 + res_length > length) {
			return false;
		}
		if (parameters[2] == 0x01 && res_length >= 9) {
			info->uid_length = 8;
			memcpy(info->uid, parameters + 3, 8);
			return true;
		}
		return false;
	}
	case NCI_TECH_V:
		/* RES_FLAG (1), DSFID (1), UID (8 bytes). */
		if (length < 10) {
			return false;
		}
		info->dsfid = parameters[1];
		info->uid_length = 8;
		memcpy(info->uid, parameters + 2, 8);
		info->reverse_uid = true;
		return true;
	}
	return false;
}

static void
format_uid(char *out, size_t capacity, const struct tag_info *info)
{
	size_t used = 0;

	if (info->uid_length == 0) {
		snprintf(out, capacity, "-");
		return;
	}
	out[0] = 0;
	for (unsigned i = 0; i < info->uid_length; i++) {
		unsigned index = info->reverse_uid ? info->uid_length - 1 - i : i;
		int wrote;

		if (used + 4 >= capacity) {
			break;
		}
		wrote = snprintf(out + used, capacity - used, "%s%02x", i ? " " : "", info->uid[index]);
		if (wrote < 0) {
			break;
		}
		used += (size_t)wrote;
	}
}

/* The task's line ends in "(sak %#x atqa %02x%02x)"; other technologies have neither. */
static void
format_extra(char *out, size_t capacity, const struct tag_info *info)
{
	snprintf(out, capacity, " (sak %#x atqa %02x%02x)",
	    info->have_sak ? info->sak : 0, info->atqa[0], info->atqa[1]);
}

/* The last UIDs printed, for the 5-second duplicate rule. */
#define RECENT_UID_COUNT        16
#define DISTINCT_UID_COUNT      32

static struct {
	uint8_t uid[10];
	unsigned length;
	uint64_t when;
} recent_uids[RECENT_UID_COUNT];
static unsigned recent_next;

static struct {
	uint8_t uid[10];
	unsigned length;
} distinct_uids[DISTINCT_UID_COUNT];
static unsigned distinct_seen;

static bool
uid_recently_printed(const struct tag_info *info)
{
	for (unsigned i = 0; i < RECENT_UID_COUNT; i++) {
		if (recent_uids[i].length == info->uid_length && info->uid_length != 0 &&
		    memcmp(recent_uids[i].uid, info->uid, info->uid_length) == 0 &&
		    ms_since(recent_uids[i].when) < 5000) {
			return true;
		}
	}
	return false;
}

static void
uid_remember(const struct tag_info *info)
{
	recent_uids[recent_next].length = info->uid_length;
	memcpy(recent_uids[recent_next].uid, info->uid, info->uid_length);
	recent_uids[recent_next].when = mach_absolute_time();
	recent_next = (recent_next + 1) % RECENT_UID_COUNT;
}

/* True the first time this UID is seen in the whole run. */
static bool
uid_first_seen(const struct tag_info *info)
{
	if (info->uid_length == 0) {
		return false;
	}
	for (unsigned i = 0; i < distinct_seen; i++) {
		if (distinct_uids[i].length == info->uid_length &&
		    memcmp(distinct_uids[i].uid, info->uid, info->uid_length) == 0) {
			return false;
		}
	}
	if (distinct_seen < DISTINCT_UID_COUNT) {
		distinct_uids[distinct_seen].length = info->uid_length;
		memcpy(distinct_uids[distinct_seen].uid, info->uid, info->uid_length);
		distinct_seen++;
	}
	return true;
}

/* ---- the activation remembered for step 4 ------------------------------------------------------ */

static struct {
	bool valid;
	uint8_t discovery_id;
	uint8_t interface;
	uint8_t protocol;
	uint8_t tech;
	uint8_t credits;
} activation;
static unsigned nci_credits;

static void
update_credits(const uint8_t *packet, unsigned length)
{
	if (length >= 6) {              /* 60 06 03 entries, conn id, credits */
		nci_credits = packet[5];
	} else if (length >= 5) {
		nci_credits = packet[4];
	}
}

/* ---- step 4: one READ of a Type 2 tag ---------------------------------------------------------- */

/*
 * 00 00 02 30 04: connection 0, a two-byte command, READ (0x30) page 4. The answer is a data
 * packet 00 00 len <16 bytes> status. Nothing but a READ is ever sent to a tag.
 */
static int
step4_read(void)
{
	static const uint8_t command[] = { 0x00, 0x00, 0x02, T2T_READ_PAGE4, 0x04 };
	uint8_t packet[NFC_BUF];
	uint64_t start;

	say("4: Type 2 tag: READ page 4 (16 bytes, read-only)\n");
	if (nci_credits == 0) {
		say("4: the chip reports no credits for the static RF connection\n");
		return -1;
	}
	if (!nfc_write(command, sizeof(command))) {
		say("4: the READ is not sent%s\n", bus_dead ? " (the bus failed 3 times in a row)" : "");
		return -1;
	}
	start = mach_absolute_time();
	while (ms_since(start) < 100) {
		unsigned length = nfc_recv(packet, sizeof(packet), 10);

		if (bus_dead) {
			say("4: the I2C bus failed 3 times in a row: stopping\n");
			return -1;
		}
		if (length == 0) {
			continue;
		}
		if (length >= 4 && packet[0] == (NCI_MT_NTF | NCI_GID_CORE) && packet[1] == 0x06) {
			update_credits(packet, length);
			continue;
		}
		if (length >= 4 && packet[0] == NCI_MT_DATA && packet[1] == 0x00) {
			unsigned payload = packet[2];
			char shown[100], ascii[17];

			if (payload < 17 || length < 3 + 17) {
				say("4: a data packet of %u bytes is too short for 16 bytes and a status\n", payload);
				return -1;
			}
			for (unsigned i = 0; i < 16; i++) {
				ascii[i] = (packet[3 + i] >= 0x20 && packet[3 + i] <= 0x7e) ? (char)packet[3 + i] : '.';
			}
			ascii[16] = 0;
			hex_string(packet + 3, 16, shown, sizeof(shown), 16);
			say("4: page 4: %s | %s | status %#x\n", shown, ascii, packet[19]);
			return packet[19] == 0x00 ? 1 : -1;
		}
		say("4: another packet while the READ is out: %u bytes\n", length);
	}
	say("4: no data within 100 ms\n");
	return -1;
}

/* ---- step 3's commands ------------------------------------------------------------------------- */

static int
rf_deactivate(uint8_t type, bool report)
{
	uint8_t command[4] = { 0x21, 0x06, 0x01, type };
	uint8_t packet[NFC_BUF];
	uint8_t status = 0xff;
	bool have_response = false, have_notification = false;
	uint64_t start;

	if (!nfc_write(command, sizeof(command))) {
		say("3: RF_DEACTIVATE_CMD(%#x) is not sent%s\n", type,
		    bus_dead ? " (the bus failed 3 times in a row)" : "");
		return -1;
	}
	start = mach_absolute_time();
	while (ms_since(start) < 100 && (!have_response || !have_notification)) {
		unsigned length = nfc_recv(packet, sizeof(packet), 20);

		if (bus_dead) {
			return -1;
		}
		if (length == 0) {
			continue;
		}
		if (length >= 4 && packet[0] == (NCI_MT_RSP | NCI_GID_RF) && packet[1] == 0x06) {
			have_response = true;
			status = packet[3];
			if (report || status != 0) {
				say("3: RF_DEACTIVATE_RSP(%#x): status %#x (raw above)\n", type, status);
			}
		} else if (length >= 5 && packet[0] == (NCI_MT_NTF | NCI_GID_RF) && packet[1] == 0x06) {
			have_notification = true;
			if (report || status != 0) {
				say("3: RF_DEACTIVATE_NTF: type %#x, reason %#x (raw above)\n", packet[3], packet[4]);
			}
		}
	}
	if (!have_response) {
		say("3: RF_DEACTIVATE_CMD(%#x): no response within 100 ms\n", type);
		return -1;
	}
	if (!have_notification && (report || status != 0)) {
		say("3: RF_DEACTIVATE_CMD(%#x): no notification within 100 ms\n", type);
	}
	if (status != 0x00) {
		say("3: RF_DEACTIVATE_CMD(%#x): status %#x\n", type, status);
		return -1;
	}
	return 0;
}

/* 21 03 len n [tech 0x01]*n */
static unsigned
build_discover(uint8_t *command, const unsigned *tech, unsigned count)
{
	command[0] = 0x21;
	command[1] = 0x03;
	command[2] = (uint8_t)(1 + 2 * count);     /* n + n (tech, frequency) pairs; nfc1 sent 1 + n: status 0x05 (run 034430) */
	command[3] = (uint8_t)count;
	for (unsigned i = 0; i < count; i++) {
		command[4 + 2 * i] = (uint8_t)tech[i];
		command[5 + 2 * i] = 0x01;
	}
	return 4 + 2 * count;
}

/* ---- step 3 ------------------------------------------------------------------------------------ */

static int
step3(void)
{
	uint8_t packet[NFC_BUF], command[32];
	unsigned tech[4], tech_count = 0;
	unsigned length, activations = 0, errors = 0, generic_streak = 0;
	uint64_t start, beat;
	uint8_t status = 0xff;
	bool with_mifare = true, with_v;
	bool have_selected = false, step4_done = false, step4_attempted = false, step4_skip_said = false;
	bool stop_after_step4 = false;
	int result = 0;

	if (nfc_tech == 0) {
		say("3: vayu_nfc_tech=%#x selects no technology\n", nfc_tech);
		return -1;
	}

	/* 1. RF_DISCOVER_MAP_CMD: protocol 0x01..0x04 and NXP's 0x80, all in poll mode. */
	{
		static const uint8_t entries[5][3] = {
			{ 0x01, 0x01, NCI_IF_FRAME },           /* T1T -> Frame */
			{ 0x02, 0x01, NCI_IF_FRAME },           /* T2T -> Frame */
			{ 0x03, 0x01, NCI_IF_FRAME },           /* T3T -> Frame */
			{ 0x04, 0x01, NCI_IF_ISO_DEP },         /* ISO-DEP -> ISO-DEP */
			{ 0x80, 0x01, NCI_IF_TAG_CMD },         /* NXP MIFARE Classic -> tag command (HYPOTHESIS) */
		};
		unsigned count;

	retry_map:
		count = with_mifare ? 5 : 4;
		command[0] = 0x21;
		command[1] = 0x00;
		command[2] = (uint8_t)(1 + 3 * count);
		command[3] = (uint8_t)count;
		for (unsigned i = 0; i < count; i++) {
			memcpy(command + 4 + 3 * i, entries[i], 3);
		}
		if (!nfc_write(command, 4 + 3 * count)) {
			say("3: RF_DISCOVER_MAP_CMD is not sent%s\n",
			    bus_dead ? " (the bus failed 3 times in a row)" : "");
			return -1;
		}
		length = wait_rsp(NCI_MT_RSP | NCI_GID_RF, 0x00, packet, sizeof(packet), 100);
		if (length < 4) {
			say("3: RF_DISCOVER_MAP_CMD: no response within 100 ms%s\n",
			    bus_dead ? " (the bus failed 3 times)" : "");
			return -1;
		}
		status = packet[3];
		if (status != 0x00 && with_mifare) {
			say("3: RF_DISCOVER_MAP_CMD: status %#x with the MIFARE Classic entry: retrying without it\n",
			    status);
			with_mifare = false;
			goto retry_map;
		}
		if (status != 0x00) {
			say("3: RF_DISCOVER_MAP_CMD: status %#x: failed\n", status);
			return -1;
		}
	}

	/* 2. RF_DISCOVER_CMD with the technologies vayu_nfc_tech asks for. */
	if (nfc_tech & 0x1) {
		tech[tech_count++] = NCI_TECH_A;
	}
	if (nfc_tech & 0x2) {
		tech[tech_count++] = NCI_TECH_B;
	}
	if (nfc_tech & 0x4) {
		tech[tech_count++] = NCI_TECH_F;
	}
	with_v = (nfc_tech & 0x8) != 0;
	if (with_v) {
		tech[tech_count++] = NCI_TECH_V;
	}
retry_discover:
	if (tech_count == 0) {
		say("3: RF_DISCOVER_CMD: no technology left after dropping NFC-V\n");
		return -1;
	}
	length = build_discover(command, tech, tech_count);
	if (!nfc_write(command, length)) {
		say("3: RF_DISCOVER_CMD is not sent%s\n", bus_dead ? " (the bus failed 3 times in a row)" : "");
		return -1;
	}
	length = wait_rsp(NCI_MT_RSP | NCI_GID_RF, 0x03, packet, sizeof(packet), 100);
	if (length < 4) {
		say("3: RF_DISCOVER_CMD: no response within 100 ms%s\n",
		    bus_dead ? " (the bus failed 3 times)" : "");
		return -1;
	}
	status = packet[3];
	if (status != 0x00 && with_v) {
		say("3: RF_DISCOVER_CMD: status %#x with NFC-V: retrying without it\n", status);
		with_v = false;
		tech_count--;
		goto retry_discover;
	}
	if (status != 0x00) {
		say("3: RF_DISCOVER_CMD: status %#x: failed\n", status);
		return -1;
	}

	say("3: polling for %u s, tech mask %#x\n", nfc_secs, nfc_tech);
	i2c_quiet = true;               /* the long polling: no per-transaction pin line, packets still logged */
	start = mach_absolute_time();
	beat = start;
	while (result == 0 && ms_since(start) < (uint64_t)nfc_secs * 1000) {
		length = nfc_recv(packet, sizeof(packet), 200);
		if (bus_dead) {
			say("3: the I2C bus failed 3 times in a row: stopping\n");
			result = -1;
			break;
		}
		if (length != 0) {
			if (packet[0] == (NCI_MT_NTF | NCI_GID_RF) && packet[1] == 0x05) {
				/* 61 05 RF_INTF_ACTIVATED_NTF. */
				bool activated_here = false;

				if (length < 10 || packet[2] < 7 || 10 + (unsigned)packet[9] > length) {
					say("3: RF_INTF_ACTIVATED_NTF of %u bytes: short, not decoded\n", length);
				} else {
					struct tag_info info;
					uint8_t id = packet[3], interface = packet[4], protocol = packet[5];
					uint8_t tech_and_mode = packet[6], credits = packet[8];
					unsigned parameters = packet[9];
					char uid[64], extra[64];

					activated_here = true;
					activations++;
					activation.valid = true;
					activation.discovery_id = id;
					activation.interface = interface;
					activation.protocol = protocol;
					activation.tech = tech_and_mode;
					activation.credits = credits;
					nci_credits = credits;
					have_selected = false;
					if (tag_decode(tech_and_mode, packet + 10, parameters, &info)) {
						format_uid(uid, sizeof(uid), &info);
						format_extra(extra, sizeof(extra), &info);
						(void)uid_first_seen(&info);
						if (!uid_recently_printed(&info)) {
							say("3: TAG #%u: tech %s, protocol %s, interface %#x, uid %s%s\n",
							    activations, tech_name(tech_and_mode), protocol_name(protocol),
							    interface, uid, extra);
							uid_remember(&info);
						}
					} else {
						char shown[200];

						hex_string(packet + 10, parameters, shown, sizeof(shown),
						    NFC_SHOW_BYTES);
						say("3: TAG #%u: tech %s, protocol %s, interface %#x, parameters: %s\n",
						    activations, tech_name(tech_and_mode), protocol_name(protocol),
						    interface, shown);
					}

					if (nfc_level >= 4 && !step4_done && !step4_attempted &&
					    activation.protocol == 0x02 &&
					    activation.interface == NCI_IF_FRAME) {
						if (activation.credits == 0) {
							if (!step4_skip_said) {
								step4_skip_said = true;
								say("4: no credits for the static RF connection: not read\n");
							}
						} else {
							int read;

							step4_attempted = true;
							read = step4_read();
							if (read > 0) {
								step4_done = true;
								nfc_step4_read_ok = true;
							} else {
								nfc_step4_failed = true;
								stop_after_step4 = true;
							}
						}
					} else if (nfc_level >= 4 && !step4_done && !step4_skip_said) {
						step4_skip_said = true;
						say("4: not a Type 2 tag in the Frame interface (protocol %s, "
						    "interface %#x): not read\n", protocol_name(protocol), interface);
					}
				}
				generic_streak = 0;
				if (activated_here) {
					/* Deactivate back to discovery; during the long polling only a refusal is shown. */
					if (rf_deactivate(0x03, !i2c_quiet) < 0 && result == 0) {
						result = -1;
					}
					have_selected = false;
					if (result < 0 || stop_after_step4) {
						break;
					}
				}
			} else if (packet[0] == (NCI_MT_NTF | NCI_GID_RF) && packet[1] == 0x03) {
				/* 61 03 RF_DISCOVER_NTF: several tags in the field. */
				if (length < 8 || 7 + (unsigned)packet[6] >= length) {
					say("3: RF_DISCOVER_NTF of %u bytes: short, not decoded\n", length);
				} else {
					struct tag_info info;
					uint8_t id = packet[3], protocol = packet[4], tech_and_mode = packet[5];
					unsigned parameters = packet[6];
					uint8_t notification_type = packet[7 + parameters];
					char uid[64], extra[64];

					if (tag_decode(tech_and_mode, packet + 7, parameters, &info)) {
						format_uid(uid, sizeof(uid), &info);
						format_extra(extra, sizeof(extra), &info);
						say("3: RF_DISCOVER_NTF: id %u, protocol %s, tech %s, uid %s%s\n", id,
						    protocol_name(protocol), tech_name(tech_and_mode), uid, extra);
					} else {
						char shown[200];

						hex_string(packet + 7, parameters, shown, sizeof(shown),
						    NFC_SHOW_BYTES);
						say("3: RF_DISCOVER_NTF: id %u, protocol %s, tech %s, "
						    "parameters: %s\n", id, protocol_name(protocol),
						    tech_name(tech_and_mode), shown);
					}
					if ((notification_type == 0x00 || notification_type == 0x01) && !have_selected) {
						uint8_t select[6] = { 0x21, 0x04, 0x03, id, protocol,
							protocol == 0x04 ? NCI_IF_ISO_DEP :
							protocol == 0x80 ? NCI_IF_TAG_CMD : NCI_IF_FRAME };

						if (!nfc_write(select, sizeof(select))) {
							say("3: RF_DISCOVER_SELECT_CMD is not sent%s\n",
							    bus_dead ? " (the bus failed 3 times in a row)" : "");
							result = -1;
						} else {
							length = wait_rsp(NCI_MT_RSP | NCI_GID_RF, 0x04,
							    packet, sizeof(packet), 100);
							if (length < 4) {
								say("3: RF_DISCOVER_SELECT_CMD: no response\n");
							} else if (packet[3] != 0x00) {
								say("3: RF_DISCOVER_SELECT_CMD: status %#x\n",
								    packet[3]);
							} else {
								have_selected = true;
							}
						}
					}
				}
				generic_streak = 0;
			} else if (packet[0] == (NCI_MT_NTF | NCI_GID_CORE) && packet[1] == 0x07) {
				/* 60 07 CORE_GENERIC_ERROR_NTF. */
				errors++;
				generic_streak++;
				say("3: CORE_GENERIC_ERROR_NTF: status %#x%s\n",
				    length >= 4 ? packet[3] : 0xff, generic_streak >= 20 ? " (20 in a row: stopping)" : "");
				if (generic_streak >= 20) {
					result = -1;
					break;
				}
			} else if (packet[0] == (NCI_MT_NTF | NCI_GID_CORE) && packet[1] == 0x08) {
				/* 60 08 CORE_INTERFACE_ERROR_NTF. */
				errors++;
				generic_streak = 0;
				say("3: CORE_INTERFACE_ERROR_NTF: status %#x\n", length >= 4 ? packet[3] : 0xff);
			} else if (packet[0] == (NCI_MT_NTF | NCI_GID_CORE) && packet[1] == 0x06) {
				/* 60 06 CORE_CONN_CREDITS_NTF: update silently. */
				update_credits(packet, length);
				generic_streak = 0;
			} else {
				generic_streak = 0;
				say("3: another packet: %u bytes (raw above)\n", length);
			}
		}
		if (ms_since(beat) >= 10000) {
			unsigned elapsed = (unsigned)ms_since(start);
			unsigned left = (unsigned)nfc_secs * 1000 > elapsed ?
			    ((unsigned)nfc_secs * 1000 - elapsed) / 1000 : 0;

			say("3: polling, %u s left, %u tags seen, %u errors\n", left, activations, errors);
			beat = mach_absolute_time();
		}
	}
	i2c_quiet = false;

	/* 4. Back to idle, then the summary. */
	if (!bus_dead) {
		(void)rf_deactivate(0x00, true);
	}
	if (nfc_level >= 4 && !step4_done && !step4_attempted && !step4_skip_said) {
		say("4: no Type 2 tag was activated in the Frame interface: nothing read\n");
	}
	say("3: %u activations, %u distinct uids\n", activations, distinct_seen);
	return result;
}

/* ---- the thread -------------------------------------------------------------------------------- */

static bool
map_pins(void)
{
	bool ok = true;

	north_i2c = ml_io_map(TLMM_NORTH_PHYS + TLMM_PIN_STRIDE * PIN_SDA, 2 * TLMM_PIN_STRIDE);
	if (north_i2c == 0) {
		say("cannot map the TLMM window for gpio%u/%u at %#lx\n", PIN_SDA, PIN_SCL,
		    (unsigned long)(TLMM_NORTH_PHYS + TLMM_PIN_STRIDE * PIN_SDA));
		ok = false;
	}
	north_ven = ml_io_map(TLMM_NORTH_PHYS + TLMM_PIN_STRIDE * PIN_VEN, TLMM_PIN_STRIDE);
	if (north_ven == 0) {
		say("cannot map the TLMM window for gpio%u at %#lx\n", PIN_VEN,
		    (unsigned long)(TLMM_NORTH_PHYS + TLMM_PIN_STRIDE * PIN_VEN));
		ok = false;
	}
	east_nfc = ml_io_map(TLMM_EAST_PHYS + TLMM_PIN_STRIDE * PIN_IRQ, 2 * TLMM_PIN_STRIDE);
	if (east_nfc == 0) {
		say("cannot map the TLMM window for gpio%u/%u at %#lx\n", PIN_IRQ, PIN_FIRM,
		    (unsigned long)(TLMM_EAST_PHYS + TLMM_PIN_STRIDE * PIN_IRQ));
		ok = false;
	}
	return ok;
}

static void
nfc_thread(__unused void *parameter, __unused wait_result_t result_thread)
{
	unsigned reached = 0;

	say("starting: %u step%s asked, xnu_tlmm %u, secs %u, tech %#x, pause %u ms\n", nfc_level_wanted,
	    nfc_level_wanted == 1 ? "" : "s", nfc_xnu_tlmm, nfc_secs, nfc_tech, nfc_pause);
	if (nfc_pause != 0) {
		delay_for_interval(nfc_pause, NSEC_PER_MSEC);
	}

	if (!map_pins()) {
		say("1: the pin windows are not mapped: nothing read\n");
		goto out;
	}
	if (step1() < 0) {
		goto out;
	}
	reached = 1;

	if (nfc_level >= 2) {
		if (!nfc_xnu_tlmm) {
			say("steps >= 2 need vayu_nfc_xnu_tlmm=1 (this port's TLMM exception): stopping after step 1\n");
			goto out;
		}
		if (!ven_is_on() || !firm_is_low() || !i2c_pins_are_gpio()) {
			say("2: refused: ven %s, firm %s, i2c pins %s (need ven on, firm low and both I2C pins "
			    "function 0; loader vayu_nfc_pins=1)\n", ven_is_on() ? "on" : "off", firm_state(),
			    i2c_pins_are_gpio() ? "gpio (function 0)" : "not gpio");
			goto out;
		}
		if (step2() < 0) {
			goto out;
		}
		reached = 2;
	}

	if (nfc_level >= 3) {
		if (step3() < 0) {
			goto out;
		}
		reached = 3;
		if (nfc_level >= 4 && nfc_step4_read_ok) {
			reached = 4;    /* step3() performed the read inside the first Type 2 activation */
		} else if (nfc_level >= 4 && nfc_step4_failed) {
			say("4: the Type 2 read failed; stopping at step 3\n");
		}
	}

out:
	say("done (reached step %u of %u)\n", reached, nfc_level_wanted);
	thread_terminate(current_thread());
}

void
vayu_nfc_init(void)
{
	thread_t thread;
	uint32_t value;

	if (!PE_parse_boot_argn("vayu_nfc", &nfc_level_wanted, sizeof(nfc_level_wanted)) ||
	    nfc_level_wanted == 0) {
		return;
	}
	nfc_level = nfc_level_wanted;
	if (nfc_level > 4) {
		say("only steps 1-4 exist: %u steps asked, treating it as 4\n", nfc_level);
		nfc_level = 4;
		nfc_level_wanted = 4;
	}
	if (PE_parse_boot_argn("vayu_nfc_xnu_tlmm", &value, sizeof(value))) {
		nfc_xnu_tlmm = value;
	}
	if (PE_parse_boot_argn("vayu_nfc_secs", &value, sizeof(value))) {
		nfc_secs = value > 3600 ? 3600 : value;
	}
	if (PE_parse_boot_argn("vayu_nfc_tech", &value, sizeof(value))) {
		nfc_tech = value & 0xf;
	}
	if (PE_parse_boot_argn("vayu_nfc_pause", &value, sizeof(value))) {
		nfc_pause = value > 60000 ? 60000 : value;
	}
	if (kernel_thread_start(nfc_thread, NULL, &thread) != KERN_SUCCESS) {
		say("the probe's thread cannot be started\n");
		return;
	}
	thread_deallocate(thread);
}
