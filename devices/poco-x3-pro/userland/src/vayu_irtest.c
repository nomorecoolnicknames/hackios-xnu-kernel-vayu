/*
 * vayu-irtest: one write to /dev/vayu-ir, the POCO X3 Pro's IR blaster.
 *
 *   vayu-irtest [carrier_hz] [ms]        defaults: 38000 and 100
 *
 * The SE shifts bits at 1 920 000 Hz (1.92 MHz), so one bit is one sample. A carrier period is
 * 1 920 000 / carrier_hz bits; 50 % duty is half of that many ones followed by the same number
 * of zeros (25 + 25 at 38 kHz, i.e. 38 400 Hz really). The buffer lasts `ms` milliseconds and is
 * rounded up to a whole number of 4-byte FIFO words.
 *
 * Byte order: /dev/vayu-ir builds each 32-bit FIFO word from four user bytes with byte 0 in bits
 * 7:0 (mainline's geni_spi_handle_tx(), spi-geni-qcom.c:734-743) and shifts the word out MSB
 * first (bits 31..0). So this program puts the first bit to be sent in bit 31 of each word by
 * building the word as a number, then stores it little-endian: the driver reads those bytes back
 * into exactly the same word. That is the vendor driver's bit order for bits_per_word 32 too.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IR_DEV          "/dev/vayu-ir"
#define IR_BIT_RATE     1920000u
#define IR_MAX_BYTES    150000u
#define IR_DEFAULT_HZ   38000u
#define IR_DEFAULT_MS   100u

static int
parse_ul(const char *text, unsigned long *value)
{
	char *end = NULL;

	errno = 0;
	*value = strtoul(text, &end, 0);
	return errno == 0 && end != text && *end == '\0';
}

int
main(int argc, char **argv)
{
	unsigned long carrier = IR_DEFAULT_HZ, ms = IR_DEFAULT_MS;
	unsigned long half, period, nbits, nwords, nbytes;
	uint8_t *buf;
	size_t at;
	ssize_t written;
	int fd;

	if (argc > 3) {
		fprintf(stderr, "usage: %s [carrier_hz] [ms]\n", argv[0]);
		return 2;
	}
	if (argc > 1 && !parse_ul(argv[1], &carrier)) {
		fprintf(stderr, "%s: carrier_hz is not a number: %s\n", argv[0], argv[1]);
		return 2;
	}
	if (argc > 2 && !parse_ul(argv[2], &ms)) {
		fprintf(stderr, "%s: ms is not a number: %s\n", argv[0], argv[2]);
		return 2;
	}
	if (carrier == 0 || carrier > IR_BIT_RATE / 2) {
		fprintf(stderr, "%s: carrier_hz must be 1..%u at a 1.92 MHz bit rate\n", argv[0], IR_BIT_RATE / 2);
		return 2;
	}
	if (ms == 0 || ms > (unsigned long)IR_MAX_BYTES / (IR_BIT_RATE / 8000u)) {
		fprintf(stderr, "%s: ms must be 1..%lu at 1.92 MHz and %u bytes\n", argv[0],
		    (unsigned long)IR_MAX_BYTES / (IR_BIT_RATE / 8000u), IR_MAX_BYTES);
		return 2;
	}

	/* 1.92 Mbit/s = 240 bytes per millisecond; every millisecond is a whole 32-bit count (60). */
	nbits = (unsigned long)IR_BIT_RATE * ms / 1000u;
	nwords = (nbits + 31u) / 32u;
	nbytes = nwords * 4u;
	half = ((unsigned long)IR_BIT_RATE + carrier) / (2u * carrier);
	if (half == 0) {
		half = 1;
	}
	period = 2u * half;

	buf = calloc((size_t)nbytes, 1);
	if (buf == NULL) {
		fprintf(stderr, "%s: no memory for %lu bytes\n", argv[0], nbytes);
		return 1;
	}

	/*
	 * Build each 32-bit FIFO word in wire order: the first bit of the word is bit 31. A one
	 * for the first half of every carrier period, a zero for the second half; padding after
	 * nbits stays zero (LED off).
	 */
	for (unsigned long w = 0; w < nwords; w++) {
		uint32_t word = 0;

		for (unsigned k = 0; k < 32u; k++) {
			unsigned long bit = w * 32u + k;
			int on = bit < nbits && (bit % period) < half;

			word = (word << 1) | (on ? 1u : 0u);
		}
		at = (size_t)w * 4u;
		buf[at + 0] = (uint8_t)(word & 0xffu);
		buf[at + 1] = (uint8_t)((word >> 8) & 0xffu);
		buf[at + 2] = (uint8_t)((word >> 16) & 0xffu);
		buf[at + 3] = (uint8_t)((word >> 24) & 0xffu);
	}

	printf("vayu-irtest: %lu Hz for %lu ms: %lu bits, %lu words, %lu bytes "
	    "(half-period %lu bits = %lu Hz actual)\n", carrier, ms, nbits, nwords, nbytes,
	    half, (unsigned long)IR_BIT_RATE / period);

	fd = open(IR_DEV, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", argv[0], IR_DEV, strerror(errno));
		free(buf);
		return 1;
	}

	do {
		written = write(fd, buf, (size_t)nbytes);
	} while (written < 0 && errno == EINTR);

	if (written < 0) {
		fprintf(stderr, "%s: write to %s failed after 0 bytes: %s\n", argv[0], IR_DEV, strerror(errno));
	} else if ((unsigned long)written != nbytes) {
		fprintf(stderr, "%s: short write to %s: %zd of %lu bytes\n", argv[0], IR_DEV, written, nbytes);
	} else {
		printf("vayu-irtest: wrote %zd of %lu bytes to %s: ok\n", written, nbytes, IR_DEV);
	}

	close(fd);
	free(buf);
	return written >= 0 && (unsigned long)written == nbytes ? 0 : 1;
}
