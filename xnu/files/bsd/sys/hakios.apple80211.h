/*
 * Two answers of bsd/dev/hakios.apple80211.c that wifid's auto-join judges networks by, apart from
 * the rest so that a program on the build machine can test them (tools/a80211-test).
 *
 * RSSI_BOUNDS (106): a version, then three dBm values. Two ports whose wifid joins on its own read
 * the order as RSSI_MIN, RSSI_MAX, RSSI_THRESHOLD: telegdyd/negroni-dev b4c39cc65 ("the layout as
 * iOSPlus10 read it from IO80211.framework of 20H392"; {1, -90, -40, -80}) and kiwiiiii-eth/fajita
 * d82116fb9 (wifid logs "Device RSSI bounds (%ld, %ld) with treshold (%ld)"; {1, -90, -20, -80}).
 * This port has answered {1, -60, -80, -75} since it first answered at all, read as MAX, MIN,
 * THRESHOLD -- in the forks' order that is a minimum of -60 above a maximum of -80, and a threshold
 * of -75 that turns away every network weaker than that. vayu_wifi_bounds=1 answers in the forks'
 * order with kiwiiiii's values; the default stays as it was.
 *
 * The noise floor (vayu_wifi_snr=1): this radio gives none, and with noise and SNR 0 in every scan
 * result CoreWiFi's auto-join turned known networks away as "Below minimum RSSI threshold"
 * (kiwiiiii-eth/fajita 68594340f, r159). ath10k's own default when the firmware says nothing,
 * ATH10K_DEFAULT_NOISE_FLOOR, is -95 dBm, which NOISE (17) here already answers.
 *
 * From other HakiOS ports (APSL 2.0): telegdyd/hakios-xnu-kernel, branch negroni-dev, commit b4c39cc65b12;
 * kiwiiiii-eth/hakios-xnu-kernel, branch fajita, commits d82116fb9050 and 68594340fc87.
 */
#ifndef _SYS_HAKIOS_APPLE80211_H_
#define _SYS_HAKIOS_APPLE80211_H_

#include <stdbool.h>
#include <stdint.h>

#define HAKIOS_A80211_NOISE_FLOOR       (-95)

struct hakios_a80211_rssi_bounds {
	uint32_t version;
	int32_t first, second, threshold;       /* the forks' order: minimum, maximum, threshold */
};

static inline struct hakios_a80211_rssi_bounds
hakios_a80211_bounds(bool forks_order)
{
	struct hakios_a80211_rssi_bounds old = { .version = 1, .first = -60, .second = -80, .threshold = -75 };
	struct hakios_a80211_rssi_bounds forks = { .version = 1, .first = -90, .second = -20, .threshold = -80 };

	return forks_order ? forks : old;
}

/* The signal over the floor, in dB; 0 when the signal is not above it. */
static inline int16_t
hakios_a80211_snr(int rssi)
{
	return rssi > HAKIOS_A80211_NOISE_FLOOR && rssi <= 0 ? (int16_t)(rssi - HAKIOS_A80211_NOISE_FLOOR) : 0;
}

#endif /* _SYS_HAKIOS_APPLE80211_H_ */
