/*
 * Tests of the auto-join answers of bsd/dev/hakios.apple80211.c (sys/hakios.apple80211.h) on the machine
 * that builds the kernel: the bytes of RSSI_BOUNDS (106) in both orders, and the SNR a scan result
 * carries with vayu_wifi_snr=1. tools/a80211-test/run.sh compiles and runs it.
 *
 * Not tested here: wifid. Which order wifid reads is the two forks' reading (see the header); the
 * phone's own wifid log ("Device RSSI bounds (%ld, %ld) with treshold (%ld)") settles it.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/hakios.apple80211.h>

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

int
main(void)
{
	/* the wire: four little-endian 32-bit words */
	static const uint8_t old_bytes[16] = { 1, 0, 0, 0, 0xc4, 0xff, 0xff, 0xff, 0xb0, 0xff, 0xff, 0xff, 0xb5, 0xff, 0xff, 0xff };
	static const uint8_t forks_bytes[16] = { 1, 0, 0, 0, 0xa6, 0xff, 0xff, 0xff, 0xec, 0xff, 0xff, 0xff, 0xb0, 0xff, 0xff, 0xff };
	struct hakios_a80211_rssi_bounds old = hakios_a80211_bounds(false), forks = hakios_a80211_bounds(true);

	check(sizeof(old) == 16, "RSSI_BOUNDS is 16 bytes");
	check(memcmp(&old, old_bytes, 16) == 0, "the default answer is the one given before: 1, -60, -80, -75");
	check(memcmp(&forks, forks_bytes, 16) == 0, "vayu_wifi_bounds=1: 1, -90, -20, -80");
	check(forks.first < forks.threshold && forks.threshold < forks.second, "the forks' order: minimum < threshold < maximum");

	check(hakios_a80211_snr(-68) == 27, "-68 dBm over -95: 27 dB (%d)", hakios_a80211_snr(-68));
	check(hakios_a80211_snr(-95) == 0 && hakios_a80211_snr(-100) == 0, "at or under the floor: 0");
	check(hakios_a80211_snr(5) == 0, "a positive RSSI is no signal: 0");
	printf("a80211-test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
