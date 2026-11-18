/*
 * Tests of the Bluetooth UART's rate change (bsd/dev/vayu_bt.c, vayu_bt_baud; sys/vayu_bt_baud.h) on
 * the machine that builds the kernel. tools/bt-test/run.sh compiles and runs it.
 *
 * What is here: the QCA_BAUDRATE_ codes, the EDL command's bytes, the RCG words for both rates the
 * driver uses -- checked against the words the loader writes today for 7 372 800 Hz (xnuloader/
 * loader.c vayu_bt_loader: M 384, N ~(15625 - 384), D ~15625, CFG hid 1 | source 6 | dual-edge) and
 * read back the way the kernel reads them -- and the serial dividers 2400/115200/3000000 come to.
 *
 * What is NOT tested here: the controller and the clock. Whether the WCN3990 of this phone takes
 * 0xfc48 before its firmware and whether SE3 runs at 96 MHz is for the phone (vayu_bt_baud=3000000).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/vayu_bt_baud.h>

static int checks, failures;

static void
check(bool ok, const char *format, ...)
{
	va_list list;

	checks++;
	if (ok) {
		return;
	}
	failures++;
	printf("FAIL: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

static void
test_codes(void)
{
	uint8_t command[5];
	static const uint8_t want[5] = { 0x01, 0x48, 0xfc, 0x01, 0x0e };

	check(vayu_bt_qca_baud_code(115200) == 0x00, "115200 is QCA_BAUDRATE_115200 (0)");
	check(vayu_bt_qca_baud_code(3000000) == 0x0e, "3000000 is QCA_BAUDRATE_3000000 (0x0e)");
	check(vayu_bt_qca_baud_code(3200000) == VAYU_BT_QCA_BAUD_NONE, "3200000 is not run here");
	check(vayu_bt_qca_baud_code(0) == VAYU_BT_QCA_BAUD_NONE, "0 is no rate");
	check(vayu_bt_set_baud_command(0x0e, command) == 5 && memcmp(command, want, 5) == 0,
	    "EDL_SET_BAUDRATE is 01 48 fc 01 0e");
}

static void
test_rcg(void)
{
	uint32_t m, n, mreg, nreg, dreg, cfg;
	const uint32_t cfg_before = 0x00100000u;        /* run 031412: XBL's CFG, bit 20 kept by the loader */

	/* the loader's 7.3728 MHz, word for word */
	check(vayu_bt_rcg_mn(7372800, &m, &n) && m == 384 && n == 15625, "7372800 Hz is 384/15625");
	check(vayu_bt_rcg_words(m, n, cfg_before, &mreg, &nreg, &dreg, &cfg), "the words for 384/15625");
	check(mreg == 384, "M is 384 (%#x)", mreg);
	check(nreg == (~(15625u - 384u) & 0xffffu), "N is ~(n - m) as the loader writes it (%#x)", nreg);
	check(dreg == (~15625u & 0xffffu), "D is ~n as the loader writes it (%#x)", dreg);
	check(cfg == ((cfg_before & ~(0x1fu | (0x7u << 8) | (0x3u << 12))) | 1u | (6u << 8) | (2u << 12)),
	    "CFG is the loader's: hid 1, source 6, dual-edge, bit 20 kept (%#x)", cfg);
	check(vayu_bt_rcg_rate(cfg, mreg, nreg) == 7372800ull, "read back: 7372800 Hz (%llu)",
	    (unsigned long long)vayu_bt_rcg_rate(cfg, mreg, nreg));

	/* 96 MHz for 3 Mbit/s */
	check(vayu_bt_rcg_mn(96000000, &m, &n) && m == 8 && n == 25, "96 MHz is 8/25");
	check(vayu_bt_rcg_words(m, n, cfg, &mreg, &nreg, &dreg, &cfg), "the words for 8/25");
	check(mreg == 8 && nreg == (~17u & 0xffffu) && dreg == (~25u & 0xffffu), "M 8, N ~17, D ~25 (%#x %#x %#x)", mreg,
	    nreg, dreg);
	check(vayu_bt_rcg_rate(cfg, mreg, nreg) == 96000000ull, "read back: 96000000 Hz (%llu)",
	    (unsigned long long)vayu_bt_rcg_rate(cfg, mreg, nreg));

	/* clk-rcg2's clamp of 2d to [m, 2(n - m)] */
	check(vayu_bt_rcg_words(10, 12, 0, &mreg, &nreg, &dreg, &cfg) && dreg == (~4u & 0xffffu),
	    "2d clamped to 2(n - m) when n is past it, as clamp_t does (%#x)", dreg);
	check(!vayu_bt_rcg_words(0, 25, 0, &mreg, &nreg, &dreg, &cfg), "m 0 is refused");
	check(!vayu_bt_rcg_words(25, 25, 0, &mreg, &nreg, &dreg, &cfg), "n == m is refused");
	check(!vayu_bt_rcg_mn(19200000, &m, &n), "a rate outside the two is refused");
	check(vayu_bt_rcg_rate(0x00002001u, 8, ~17u) == 0, "another source reads as 0");
}

static void
test_dividers(void)
{
	check(vayu_bt_uart_divider(7372800, 2400) == 96, "2400 from 7.3728 MHz: 96");
	check(vayu_bt_uart_divider(7372800, 115200) == 2, "115200 from 7.3728 MHz: 2");
	check(vayu_bt_uart_divider(96000000, 3000000) == 1, "3000000 from 96 MHz: 1");
	check(vayu_bt_uart_divider(7372800, 3000000) == 0, "3000000 cannot come from 7.3728 MHz");
	check(vayu_bt_uart_divider(96000000, 115200) == 0, "115200 is no exact divider of 96 MHz");
	check(vayu_bt_uart_divider(19200000, 2400) == 250, "2400 from XO: 250 (run 031412)");
}

int
main(void)
{
	test_codes();
	test_rcg();
	test_dividers();
	printf("bt-test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
