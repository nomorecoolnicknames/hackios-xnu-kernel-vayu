/*
 * wifiprobe: what iOS's Wi-Fi side is answered, asked the same way it asks.
 *
 * Every request wifid makes ends as an Apple80211 ioctl on a socket -- SIOCGA80211 to read,
 * SIOCSA80211 to ask -- with a struct apple80211req that names the interface and a buffer in this
 * program's memory (bsd/dev/hakios.apple80211.c answers them in the kernel).  This program asks the
 * questions Settings and the status bar depend on, in the order wifid asks them, and prints the
 * answers: the radio's power, the state of the station, the network joined, and then a scan -- the
 * request wifid never makes while the kernel answers "no channels" -- with every access point the
 * kernel gives back.
 *
 * It changes nothing: no join, no power, no scan that the kernel has to carry out (a scan is answered
 * from the one the kernel made on boot).  A network's name is printed; no key, no passphrase, nothing
 * of the sort is read or printed.
 *
 * Built by userland/build.sh (obj/wifiprobe), run by rc.jobs.wifiprobe.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SIOCSA80211     0x802869c8u     /* _IOW('i', 200, struct apple80211req) */
#define SIOCGA80211     0xc02869c9u     /* _IOWR('i', 201, struct apple80211req) */

struct apple80211req {
	char name[16];
	int32_t type, value;
	uint32_t length;
	void *data __attribute__((aligned(8)));
};

/* the channel: CHANNEL (4), and inside a scan's result */
struct apple80211_channel {
	uint32_t version, channel, flags;
};

/* what SCAN_RESULT (11) and CURRENT_NETWORK (103) answer: 1164 bytes (IO80211.framework, iOS 16.5) */
struct apple80211_scan_result {
	uint32_t version;
	struct apple80211_channel channel;
	uint16_t noise_known;
	int16_t noise, snr, rssi;
	uint16_t beacon_interval, capability;
	uint8_t bssid[6];
	uint8_t rate_count, unused_0;
	uint32_t rates[15];
	uint8_t ssid_length, ssid[32], unused_1[3];
	uint32_t age_ms;
	uint8_t flags, unused_2;
	uint16_t elements_length;
	uint8_t elements[1024];
};

/* the scan's request: 5456 bytes (IO80211.framework, iOS 16.5) */
struct apple80211_scan_request {
	uint32_t version, bss_type;
	uint8_t bssid[6], unused_0[2];
	uint32_t ssid_length;
	uint8_t ssid[32];
	uint32_t scan_type;                     /* 1: asking, 2: listening only */
	uint32_t phy_mode;
	uint16_t dwell_ms, unused_1;
	uint32_t rest_ms;
	uint32_t channel_count;
	struct apple80211_channel channels[400];
	uint64_t flags;
	uint32_t unused_2;
	uint32_t name_count;
	uint8_t names[10][56];
	uint32_t short_ssid, unused_3;
};

_Static_assert(sizeof(struct apple80211req) == 40, "the request as the kernel has it (28 bytes, then the buffer's address at 32, 8-byte aligned)");
_Static_assert(sizeof(struct apple80211_scan_result) == 1164, "a scan result as iOS 16.5 has it");
_Static_assert(sizeof(struct apple80211_scan_request) == 5456, "a scan request as iOS 16.5 has it");

static int probe;                       /* the socket every request goes on */

static const char *
state_name(int32_t state)
{
	static const char *const names[] = { "init", "scan", "auth", "assoc", "run" };

	return state >= 0 && state < 5 ? names[state] : "?";
}

/* the name as it came: printable bytes as they are, anything else in hex */
static void
print_name(const uint8_t *name, unsigned length)
{
	for (unsigned i = 0; i < length; i++) {
		if (name[i] >= 0x20 && name[i] < 0x7f && name[i] != '\\' && name[i] != '\'') {
			putchar(name[i]);
		} else {
			printf("\\x%02x", name[i]);
		}
	}
}

static void
print_bssid(const uint8_t *bssid)
{
	printf("xx:xx:xx:xx:%02x:%02x", bssid[4], bssid[5]);    /* (the first four are not printed, as in the logs) */
}

static const char *
security(const struct apple80211_scan_result *result)
{
	unsigned at;

	for (at = 0; at + 2 <= result->elements_length && at + 2 + result->elements[at + 1] <= result->elements_length;
	    at += 2 + result->elements[at + 1]) {
		if (result->elements[at] == 48) {
			return "WPA2 (RSN)";
		}
	}
	for (at = 0; at + 2 <= result->elements_length && at + 2 + result->elements[at + 1] <= result->elements_length;
	    at += 2 + result->elements[at + 1]) {
		if (result->elements[at] == 221 && result->elements[at + 1] >= 4 && result->elements[at + 2] == 0x00 &&
		    result->elements[at + 3] == 0x50 && result->elements[at + 4] == 0xf2 && result->elements[at + 5] == 0x01) {
			return "WPA (vendor)";
		}
	}
	return "open (no RSN/WPA element)";
}

static int
ask(int32_t type, void *data, uint32_t length, int *value)
{
	struct apple80211req request;

	memset(&request, 0, sizeof(request));
	strlcpy(request.name, "en0", sizeof(request.name));
	request.type = type;
	request.length = length;
	request.data = data;
	if (ioctl(probe, SIOCGA80211, &request) != 0) {
		return errno != 0 ? errno : EIO;
	}
	if (value != NULL) {
		*value = request.value;
	}
	return 0;
}

int
main(void)
{
	static struct apple80211_scan_request scan;
	static struct apple80211_scan_result result;
	uint8_t ssid[32], bssid[6];
	struct {
		uint32_t version, radios, state[4];
	} power;
	struct {
		uint32_t version;
		struct apple80211_channel channel;
	} channel;
	struct {
		uint32_t version, radios, unit;
		int32_t each[4], all, each_ext[4], all_ext;
	} rssi;
	int value = 0, error, count = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	probe = socket(AF_INET, SOCK_DGRAM, 0);
	if (probe < 0) {
		printf("wifiprobe: no socket: %s\n", strerror(errno));
		return 1;
	}
	/* the radio, as wifid asks it first */
	memset(&power, 0, sizeof(power));
	error = ask(19, &power, sizeof(power), NULL);
	printf("wifiprobe: POWER(19): %s (radios %u, state %u)\n", error != 0 ? strerror(error) : "answered",
	    error == 0 ? power.radios : 0, error == 0 ? power.state[0] : 0);
	error = ask(13, NULL, 0, &value);
	printf("wifiprobe: STATE(13): %s\n", error != 0 ? strerror(error) : state_name(value));

	/* the network joined, if any */
	memset(ssid, 0, sizeof(ssid));
	error = ask(1, ssid, sizeof(ssid), &value);
	if (error == 0) {
		unsigned length = value > 0 && value <= (int)sizeof(ssid) ? (unsigned)value : 0;

		printf("wifiprobe: SSID(1): '");
		print_name(ssid, length);
		printf("' (%u bytes)\n", length);
	} else {
		printf("wifiprobe: SSID(1): %s (no network joined)\n", strerror(error));
	}
	memset(bssid, 0, sizeof(bssid));
	if ((error = ask(9, bssid, sizeof(bssid), NULL)) == 0) {
		printf("wifiprobe: BSSID(9): ");
		print_bssid(bssid);
		printf("\n");
	} else {
		printf("wifiprobe: BSSID(9): %s\n", strerror(error));
	}
	memset(&channel, 0, sizeof(channel));
	if ((error = ask(4, &channel, sizeof(channel), NULL)) == 0) {
		printf("wifiprobe: CHANNEL(4): %u (flags %#x)\n", channel.channel.channel, channel.channel.flags);
	} else {
		printf("wifiprobe: CHANNEL(4): %s\n", strerror(error));
	}
	memset(&rssi, 0, sizeof(rssi));
	if ((error = ask(16, &rssi, sizeof(rssi), NULL)) == 0) {
		printf("wifiprobe: RSSI(16): %d dBm\n", rssi.all);
	} else {
		printf("wifiprobe: RSSI(16): %s\n", strerror(error));
	}

	/* the channel list, which wifid asks before it will scan at all */
	{
		static struct {
			uint32_t version, count;
			struct apple80211_channel channels[401];
			uint32_t unused;
		} channels;
		uint32_t i, n = 0;

		memset(&channels, 0, sizeof(channels));
		error = ask(254, &channels, sizeof(channels), NULL);
		if (error == 0 && channels.count <= 401) {
			n = channels.count;
		}
		printf("wifiprobe: HW_SUPPORTED_CHANNELS(254): %s, %u channel(s)", error != 0 ? strerror(error) : "answered", n);
		for (i = 0; i < n && i < 8; i++) {
			printf("%s%u", i == 0 ? " " : ",", channels.channels[i].channel);
		}
		printf("%s\n", n > 8 ? " ..." : "");
	}

	/* the scan: exactly the request wifid would make (5456 bytes, no channel filter, no name) */
	memset(&scan, 0, sizeof(scan));
	scan.version = 1;
	scan.scan_type = 1;
	{
		struct apple80211req request;

		memset(&request, 0, sizeof(request));
		strlcpy(request.name, "en0", sizeof(request.name));
		request.type = 10;
		request.length = sizeof(scan);
		request.data = &scan;
		if (ioctl(probe, SIOCSA80211, &request) != 0) {
			printf("wifiprobe: SCAN_REQ(10): REFUSED: %s\n", strerror(errno));
			goto current;
		}
	}
	printf("wifiprobe: SCAN_REQ(10): asked; reading SCAN_RESULT(11) until the kernel says there are no more\n");
	usleep(300000);                 /* (the kernel answers from the scan it made on boot) */
	for (;;) {
		memset(&result, 0, sizeof(result));
		result.elements_length = sizeof(result.elements);       /* how much room there is for the elements */
		{
			struct apple80211req request;

			memset(&request, 0, sizeof(request));
			strlcpy(request.name, "en0", sizeof(request.name));
			request.type = 11;
			request.length = sizeof(result);
			request.data = &result;
			if (ioctl(probe, SIOCGA80211, &request) != 0) {
				error = errno != 0 ? errno : EIO;
				break;
			}
		}
		printf("wifiprobe: [%d] ch %u rssi %d age %u ms '", count, result.channel.channel, result.rssi, result.age_ms);
		print_name(result.ssid, result.ssid_length <= sizeof(result.ssid) ? result.ssid_length : sizeof(result.ssid));
		printf("' ");
		print_bssid(result.bssid);
		printf(" %s, elements %u\n", security(&result), result.elements_length);
		count++;
	}
	printf("wifiprobe: SCAN_RESULT(11): %d network(s), end: %s\n", count,
	    error == EIO ? "end of the list (EIO, as Apple80211 says)" : strerror(error));

current:
	/* what SpringBoard's SBWiFiManager asks for the icon */
	memset(&result, 0, sizeof(result));
	result.elements_length = sizeof(result.elements);
	{
		struct apple80211req request;

		memset(&request, 0, sizeof(request));
		strlcpy(request.name, "en0", sizeof(request.name));
		request.type = 103;
		request.length = sizeof(result);
		request.data = &result;
		if (ioctl(probe, SIOCGA80211, &request) != 0) {
			printf("wifiprobe: CURRENT_NETWORK(103): %s\n", strerror(errno));
		} else {
			printf("wifiprobe: CURRENT_NETWORK(103): '");
			print_name(result.ssid, result.ssid_length <= sizeof(result.ssid) ? result.ssid_length : sizeof(result.ssid));
			printf("' ch %u rssi %d ", result.channel.channel, result.rssi);
			print_bssid(result.bssid);
			printf(" %s\n", security(&result));
		}
	}
	close(probe);
	return 0;
}
