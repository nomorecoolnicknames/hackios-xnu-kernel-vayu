/*
 * sms: the userland side of /dev/vayu-sms (bsd/dev/vayu_sms.c).
 *
 *   sms list                 the kernel's inbox: seq, time, sender, text
 *   sms send NUMBER TEXT...  one short message; NUMBER may begin with '+'
 *   sms delete SEQ           remove one stored message from the SIM/NV
 *   sms refresh [uim|nv]     sweep that storage now and print how many were read
 *   sms status               the WMS client's state and counters
 *   sms selftest NUMBER [WAIT_READY_S] [WAIT_RX_S] [--any-reg]
 *                            wait for WMS (and the cell record's registration),
 *                            send "vayu selftest <time>" exactly once, then watch
 *                            the inbox and print every new message masked.
 *                            --any-reg sends after 120 s even without a registered
 *                            network (or as soon as one was seen), leaving the
 *                            refusal to the modem
 *
 * A message longer than one segment is refused by the kernel (EMSGSIZE); this
 * stage has no concatenation.  The kernel builds the PDU and speaks QMI WMS; this
 * program only moves the structures of sys/vayu_sms.h through ioctl.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "vayu_sms.h"

static const char *
state_name(uint32_t state)
{
	switch (state) {
	case VAYU_SMS_STATE_OFF: return "off";
	case VAYU_SMS_STATE_WAITING: return "waiting for WMS";
	case VAYU_SMS_STATE_CONFIGURING: return "configuring";
	case VAYU_SMS_STATE_READY: return "ready";
	case VAYU_SMS_STATE_FAILED: return "failed";
	default: return "?";
	}
}

static const char *
alphabet_name(uint32_t alphabet)
{
	switch (alphabet) {
	case VAYU_SMS_ALPHABET_GSM7: return "GSM 7-bit";
	case VAYU_SMS_ALPHABET_8BIT: return "8-bit";
	case VAYU_SMS_ALPHABET_UCS2: return "UCS-2";
	default: return "?";
	}
}

static int
open_device(void)
{
	int device = open("/dev/" VAYU_SMS_DEVICE, O_RDWR);

	if (device < 0) {
		fprintf(stderr, "sms: /dev/%s: %s\n", VAYU_SMS_DEVICE, strerror(errno));
	}
	return device;
}

/* ---- selftest ------------------------------------------------------------------------------- */

/*
 * The selftest's lines all start with this prefix, on stdout and stderr: rc.jobs
 * copies both to the loader log.  It never prints the destination or the text.
 */
static void
st_out(const char *format, ...)
{
	va_list list;

	fputs("sms-selftest: ", stdout);
	va_start(list, format);
	vfprintf(stdout, format, list);
	va_end(list);
	fputc('\n', stdout);
	fflush(stdout);
}

static void
st_err(const char *format, ...)
{
	va_list list;

	fputs("sms-selftest: ", stderr);
	va_start(list, format);
	vfprintf(stderr, format, list);
	va_end(list);
	fputc('\n', stderr);
	fflush(stderr);
}

/* The last four digits only: "...1234"; a short or alphanumeric sender is "...". */
static void
mask_number(const char *number, char out[8])
{
	size_t length = strlen(number);
	size_t kept = 0;

	out[0] = '.'; out[1] = '.'; out[2] = '.';
	if (length >= 5) {
		for (size_t i = length; i > 0 && kept < 4; i--) {
			if (number[i - 1] < '0' || number[i - 1] > '9') {
				break;
			}
			kept++;
		}
	}
	for (size_t i = 0; i < kept; i++) {
		out[3 + i] = number[length - kept + i];
	}
	out[3 + kept] = 0;
}

/*
 * The first 68 bytes of bsd/sys/vayu_cell.h's struct vayu_cell_record (the same
 * offsets: magic, version, size, ..., reg_state at 61).  The record is 152 bytes;
 * a different magic/version/size is "unknown", never a guess.
 */
struct vayu_cell_head {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t seq;
	uint64_t updated_ms;
	uint32_t iccid_length;
	uint32_t imsi_length;
	int32_t  signal_dbm;
	int32_t  rsrq_db;
	int32_t  snr_tenth_db;
	uint32_t lac;
	uint32_t cid;
	uint16_t mcc;
	uint16_t mnc;
	uint8_t  sim_state;
	uint8_t  card_state;
	uint8_t  app_state;
	uint8_t  pin_state;
	uint8_t  pin_retries;
	uint8_t  reg_state;
	uint8_t  radio;
	uint8_t  roam;
	uint32_t flags;
};

_Static_assert(offsetof(struct vayu_cell_head, reg_state) == 61, "vayu_cell record layout");
_Static_assert(offsetof(struct vayu_cell_head, flags) == 64, "vayu_cell record layout");
_Static_assert(sizeof(struct vayu_cell_head) == 72, "vayu_cell record layout (tail padding)");

#define VAYU_CELL_MAGIC   0x43454c4cu
#define VAYU_CELL_VERSION 1
#define VAYU_CELL_BYTES   152
#define VAYU_CELL_REGISTERED 1

/* The raw QMI registration state (0..4, 0xff), or -1 when there is no usable record. */
static int
cell_reg_state(void)
{
	uint8_t record[VAYU_CELL_BYTES];
	struct vayu_cell_head head;
	ssize_t got;
	int device;

	device = open("/dev/vayu-cell", O_RDONLY);
	if (device < 0) {
		return -1;
	}
	got = read(device, record, sizeof(record));
	close(device);
	if (got != (ssize_t)sizeof(record)) {
		return -1;
	}
	memcpy(&head, record, sizeof(head));
	if (head.magic != VAYU_CELL_MAGIC || head.version != VAYU_CELL_VERSION ||
	    head.size != VAYU_CELL_BYTES) {
		return -1;
	}
	if (head.reg_state > 4 && head.reg_state != 0xff) {
		return -1;
	}
	return head.reg_state;
}

static const char *
network_name(int state)
{
	switch (state) {
	case 0: return "not registered";
	case 1: return "registered";
	case 2: return "searching";
	case 3: return "denied";
	case 4: return "unknown(qmi)";
	case 0xff: return "unknown";
	default: return "no record";
	}
}

/*
 * Wait for the WMS client to be ready.  Without --any-reg the network must be
 * registered at that moment (or the cell record must be absent, as before).  With
 * --any-reg the send may proceed once WMS is ready and either the network has been
 * registered at least once during this wait or ANY_REG_DELAY seconds have passed;
 * the modem is then allowed to refuse the send.  At most wait_s, polling every 5 s.
 * Prints a line on every raw reg_state change and a heartbeat every 30 s.  Returns
 * an open /dev/vayu-sms or -1; *reason_out says why the send is made.
 */
#define ANY_REG_DELAY 120

static int
wait_ready(uint32_t wait_s, bool any_reg, struct vayu_sms_status *status_out, int *network_out,
    const char **reason_out, time_t *waited_out)
{
	time_t start = time(NULL), last_report = start - 100;
	int last_wms = -1, last_state = -1, last_network = -2;
	bool seen_registered = false;

	if (reason_out != NULL) {
		*reason_out = "not ready";
	}
	for (;;) {
		struct vayu_sms_status status;
		int device, wms = 0, state = -1, network;

		memset(&status, 0, sizeof(status));
		device = open("/dev/" VAYU_SMS_DEVICE, O_RDWR);
		if (device >= 0) {
			if (ioctl(device, VAYU_SMS_IOC_STATUS, &status) != 0 ||
			    status.magic != VAYU_SMS_MAGIC || status.version != VAYU_SMS_VERSION) {
				memset(&status, 0, sizeof(status));
				status.state = VAYU_SMS_STATE_OFF;
			} else {
				wms = status.wms_present ? 1 : 0;
				state = (int)status.state;
			}
		}
		network = cell_reg_state();
		if (network == 1) {
			seen_registered = true;
		}
		{
			time_t now = time(NULL);
			bool ready = device >= 0 && wms && state == VAYU_SMS_STATE_READY;
			const char *reason = NULL;

			if (ready) {
				if (!any_reg) {
					if (network == 1) {
						reason = "network-registered";
					} else if (network < 0) {
						reason = "network-unknown";
					}
				} else if (seen_registered) {
					reason = "seen-registered";
				} else if (now - start >= ANY_REG_DELAY) {
					reason = "any-reg-delay";
				}
			}
			if (reason != NULL) {
				st_out("ready after %ld s: reason=%s wms=present network=%s", (long)(now - start),
				    reason, network_name(network));
				if (status_out != NULL) {
					*status_out = status;
				}
				if (network_out != NULL) {
					*network_out = network;
				}
				if (reason_out != NULL) {
					*reason_out = reason;
				}
				if (waited_out != NULL) {
					*waited_out = now - start;
				}
				return device;
			}
			if (now - start >= (time_t)wait_s) {
				st_err("ready timeout after %ld s: wms=%s state=%s network=%s%s", (long)(now - start),
				    wms ? "present" : "absent", state_name((uint32_t)state), network_name(network),
				    any_reg ? " (--any-reg)" : "");
				if (device >= 0) {
					close(device);
				}
				return -1;
			}
			if (now - last_report >= 30 || wms != last_wms || state != last_state || network != last_network) {
				st_out("waiting: wms=%s state=%s network=%s (%ld s)%s", wms ? "present" : "absent",
				    state_name((uint32_t)state), network_name(network), (long)(now - start),
				    any_reg ? " any-reg" : "");
				last_report = now;
				last_wms = wms;
				last_state = state;
				last_network = network;
			}
		}
		if (device >= 0) {
			close(device);
		}
		sleep(5);
	}
}

static uint32_t
parse_seconds(const char *argument, uint32_t fallback)
{
	char *end = NULL;
	unsigned long value;

	if (argument == NULL) {
		return fallback;
	}
	errno = 0;
	value = strtoul(argument, &end, 10);
	if (errno != 0 || end == argument || *end != 0 || value > 86400) {
		return fallback;
	}
	return (uint32_t)value;
}

static int
command_selftest(int argc, char **argv)
{
	const char *number, *reason = "not ready";
	uint32_t wait_ready_s = 600, wait_rx_s = 300;
	struct vayu_sms_status status;
	struct vayu_sms_send send;
	char text[VAYU_SMS_TEXT_MAX];
	size_t text_length;
	uint32_t baseline = 0, last_seq = 0;
	unsigned received = 0, matched = 0;
	bool send_attempted = false, any_reg = false;
	int device, network = -1, numeric_args = 0;
	time_t deadline, waited = 0;

	if (argc < 3) {
		st_err("usage: sms selftest NUMBER [WAIT_READY_S] [WAIT_RX_S] [--any-reg]");
		return 2;
	}
	number = argv[2];
	if (strlen(number) == 0 || strlen(number) >= VAYU_SMS_NUMBER_MAX) {
		st_err("the destination is empty or longer than %u bytes", VAYU_SMS_NUMBER_MAX - 1);
		return 2;
	}
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--any-reg") == 0) {
			any_reg = true;
		} else if (numeric_args == 0) {
			wait_ready_s = parse_seconds(argv[i], wait_ready_s);
			numeric_args++;
		} else if (numeric_args == 1) {
			wait_rx_s = parse_seconds(argv[i], wait_rx_s);
			numeric_args++;
		} else {
			st_err("usage: sms selftest NUMBER [WAIT_READY_S] [WAIT_RX_S] [--any-reg]");
			return 2;
		}
	}

	memset(&status, 0, sizeof(status));
	device = wait_ready(wait_ready_s, any_reg, &status, &network, &reason, &waited);
	if (device < 0) {
		st_err("no usable /dev/%s after %u s; nothing is sent", VAYU_SMS_DEVICE, wait_ready_s);
		return 1;
	}

	/* exactly one send, whatever the answer */
	{
		struct vayu_sms_count count;

		if (ioctl(device, VAYU_SMS_IOC_COUNT, &count) == 0) {
			baseline = count.next_seq;
		}
		last_seq = baseline > 0 ? baseline - 1 : 0;
	}
	text_length = (size_t)snprintf(text, sizeof(text), "vayu selftest %lld", (long long)time(NULL));
	memset(&send, 0, sizeof(send));
	strcpy(send.number, number);
	memcpy(send.text, text, text_length + 1);
	send.text_length = (uint32_t)text_length;
	st_out("sending one message: reason=%s, waited=%ld s, %zu bytes of text", reason, (long)waited, text_length);
	/*
	 * The kernel returns EBUSY before it queues the job when an inbox sweep is
	 * running; that attempt sent nothing.  Wait for the sweep and try again only
	 * in that case.  Every other error is final: one message, no retry.
	 */
	for (unsigned attempt = 1; attempt <= 12; attempt++) {
		if (ioctl(device, VAYU_SMS_IOC_SEND, &send) == 0) {
			send_attempted = true;
			st_out("sent: message id=%u reference=%u pdu=%u bytes", send.message_id, send.message_reference,
			    send.pdu_length);
			break;
		}
		if (errno == EBUSY && attempt < 12) {
			st_out("send not attempted: the inbox sweep is running (busy); waiting 5 s");
			sleep(5);
			continue;
		}
		st_err("send failed: %s (errno %d); no retry is made", strerror(errno), errno);
		break;
	}

	/* watch the inbox; every new seq is printed once, with the sender masked */
	deadline = time(NULL) + (time_t)wait_rx_s;
	for (;;) {
		struct vayu_sms_count count;
		uint32_t storage = 0;
		int refresh_error = 0;

		if (ioctl(device, VAYU_SMS_IOC_REFRESH, &storage) != 0) {
			refresh_error = errno;
			if (refresh_error != EBUSY) {
				st_err("refresh: %s (errno %d)", strerror(refresh_error), refresh_error);
			}
		}
		if (ioctl(device, VAYU_SMS_IOC_COUNT, &count) == 0) {
			for (uint32_t seq = last_seq + 1; seq < count.next_seq; seq++) {
				struct vayu_sms_get get;
				char mask[8];
				bool same;

				memset(&get, 0, sizeof(get));
				get.seq = seq;
				if (ioctl(device, VAYU_SMS_IOC_GET, &get) != 0) {
					if (errno != ENOENT) {
						st_err("get %u: %s (errno %d)", seq, strerror(errno), errno);
					}
					last_seq = seq;
					continue;
				}
				last_seq = seq;
				if (seq < baseline) {
					continue;
				}
				mask_number(get.message.number, mask);
				same = get.message.text_length == text_length &&
				    memcmp(get.message.text, text, text_length) == 0;
				st_out("rx seq=%u from=%s len=%u enc=%s match=%s", get.message.seq, mask,
				    get.message.text_length, alphabet_name(get.message.alphabet), same ? "yes" : "no");
				received++;
				if (same) {
					matched++;
				}
			}
		} else {
			st_err("count: %s (errno %d)", strerror(errno), errno);
		}
		{
			time_t now = time(NULL);

			if (now >= deadline) {
				break;
			}
			sleep((deadline - now) < 10 ? (unsigned)(deadline - now) : 10);
		}
	}
	st_out("done: sent=%d rx=%u matched=%u waited=%us%s", send_attempted ? 1 : 0, received, matched, wait_rx_s,
	    received == 0 ? " (no incoming message)" : "");
	close(device);
	return matched > 0 ? 0 : 1;
}

static int
command_list(int device)
{
	struct vayu_sms_count count;
	struct vayu_sms_get get;
	uint32_t shown = 0;

	if (ioctl(device, VAYU_SMS_IOC_COUNT, &count) < 0) {
		fprintf(stderr, "sms: count: %s\n", strerror(errno));
		return 1;
	}
	for (uint32_t seq = 1; seq < count.next_seq; seq++) {
		memset(&get, 0, sizeof(get));
		get.seq = seq;
		if (ioctl(device, VAYU_SMS_IOC_GET, &get) < 0) {
			if (errno == ENOENT) {
				continue;
			}
			fprintf(stderr, "sms: get %u: %s\n", seq, strerror(errno));
			return 1;
		}
		printf("#%u  %s  %s  [%s, %u bytes]%s%s\n", get.message.seq,
		    get.message.timestamp[0] != 0 ? get.message.timestamp : "(no time)",
		    get.message.number[0] != 0 ? get.message.number : "(no sender)",
		    alphabet_name(get.message.alphabet), get.message.text_length,
		    get.message.tag == VAYU_SMS_TAG_MT_NOT_READ ? "  UNREAD" : "",
		    (get.message.flags & VAYU_SMS_F_MULTIPART) ? "  multipart segment" : "");
		fputs("    ", stdout);
		fputs(get.message.text, stdout);
		fputc('\n', stdout);
		shown++;
	}
	printf("%u message(s), %u unread, capacity %u\n", count.count, count.unread, count.capacity);
	if (shown != count.count) {
		fprintf(stderr, "sms: %u of %u could not be fetched\n", count.count - shown, count.count);
	}
	return 0;
}

static int
command_send(int device, int argc, char **argv)
{
	struct vayu_sms_send send;
	size_t used = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: sms send NUMBER TEXT...\n");
		return 2;
	}
	memset(&send, 0, sizeof(send));
	if (strlen(argv[2]) >= sizeof(send.number)) {
		fprintf(stderr, "sms: the number is too long\n");
		return 2;
	}
	strcpy(send.number, argv[2]);
	for (int i = 3; i < argc; i++) {
		size_t length = strlen(argv[i]);

		if (used != 0) {
			if (used + 1 >= sizeof(send.text)) {
				fprintf(stderr, "sms: the text is too long\n");
				return 2;
			}
			send.text[used++] = ' ';
		}
		if (used + length >= sizeof(send.text)) {
			fprintf(stderr, "sms: the text is too long\n");
			return 2;
		}
		memcpy(send.text + used, argv[i], length);
		used += length;
	}
	send.text[used] = 0;
	send.text_length = (uint32_t)used;
	if (ioctl(device, VAYU_SMS_IOC_SEND, &send) < 0) {
		fprintf(stderr, "sms: send: %s\n", strerror(errno));
		return 1;
	}
	printf("sent: message id %u, reference %u, PDU %u bytes\n", send.message_id, send.message_reference,
	    send.pdu_length);
	return 0;
}

static int
command_delete(int device, const char *argument)
{
	struct vayu_sms_delete delete;
	char *end = NULL;
	unsigned long seq;

	errno = 0;
	seq = strtoul(argument != NULL ? argument : "", &end, 10);
	if (errno != 0 || end == argument || *end != 0 || seq == 0 || seq > UINT32_MAX) {
		fprintf(stderr, "usage: sms delete SEQ\n");
		return 2;
	}
	delete.seq = (uint32_t)seq;
	if (ioctl(device, VAYU_SMS_IOC_DELETE, &delete) < 0) {
		fprintf(stderr, "sms: delete: %s\n", strerror(errno));
		return 1;
	}
	printf("deleted #%u\n", delete.seq);
	return 0;
}

static int
command_refresh(int device, const char *argument)
{
	uint32_t storage = 0;           /* QMI_WMS_STORAGE_TYPE_UIM */
	uint32_t read;

	if (argument != NULL) {
		if (strcmp(argument, "uim") == 0) {
			storage = 0;
		} else if (strcmp(argument, "nv") == 0) {
			storage = 1;
		} else {
			fprintf(stderr, "usage: sms refresh [uim|nv]\n");
			return 2;
		}
	}
	read = storage;
	if (ioctl(device, VAYU_SMS_IOC_REFRESH, &read) < 0) {
		fprintf(stderr, "sms: refresh: %s\n", strerror(errno));
		return 1;
	}
	printf("refresh: %u message(s) read from storage %u\n", read, storage);
	return 0;
}

static int
command_status(int device)
{
	struct vayu_sms_status status;

	if (ioctl(device, VAYU_SMS_IOC_STATUS, &status) < 0) {
		fprintf(stderr, "sms: status: %s\n", strerror(errno));
		return 1;
	}
	if (status.magic != VAYU_SMS_MAGIC || status.version != VAYU_SMS_VERSION) {
		fprintf(stderr, "sms: the device speaks another version (magic %#x, version %u)\n", status.magic,
		    status.version);
		return 1;
	}
	printf("state: %s, WMS %s, event report %s\n", state_name(status.state),
	    status.wms_present ? "present" : "absent", status.event_report ? "on" : "off");
	printf("routes: %u known=%u written=%u allowed=%u\n", status.route_count, status.routes_known,
	    status.routes_written, status.routes_allowed);
	printf("store: %u, inbox: %u\n", status.store, status.inbox_count);
	printf("received %u, sent %u, deleted %u, dropped %u\n", status.received, status.sent, status.deleted,
	    status.dropped);
	printf("requests %u, failures %u, last result %u error %u message %#x, gsm cause %u\n", status.requests,
	    status.failures, status.last_result, status.last_error, status.last_message, status.last_gsm_cause);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr, "usage: sms list | send NUMBER TEXT... | delete SEQ | refresh [uim|nv] | status\n"
	    "       sms selftest NUMBER [WAIT_READY_S] [WAIT_RX_S] [--any-reg]\n");
}

int
main(int argc, char **argv)
{
	int device, status;

	/* rc.jobs sends both streams to the loader log: do not let a buffer hide lines */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (argc < 2) {
		usage();
		return 2;
	}
	if (strcmp(argv[1], "selftest") == 0) {
		return command_selftest(argc, argv);    /* it opens the device itself */
	}
	device = open_device();
	if (device < 0) {
		return 1;
	}
	if (strcmp(argv[1], "list") == 0) {
		status = command_list(device);
	} else if (strcmp(argv[1], "send") == 0) {
		status = command_send(device, argc, argv);
	} else if (strcmp(argv[1], "delete") == 0) {
		status = command_delete(device, argc > 2 ? argv[2] : NULL);
	} else if (strcmp(argv[1], "refresh") == 0) {
		status = command_refresh(device, argc > 2 ? argv[2] : NULL);
	} else if (strcmp(argv[1], "status") == 0) {
		status = command_status(device);
	} else {
		usage();
		status = 2;
	}
	close(device);
	return status;
}
