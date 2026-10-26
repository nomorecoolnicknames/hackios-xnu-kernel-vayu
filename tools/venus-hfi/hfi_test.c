/*
 * Tests of the HFI 4xx wire format in sys/vayu_venus_hfi.h -- the packet builders and parsers
 * and the queue table that bsd/dev/vayu_venus.c writes for the Venus firmware of vayu.
 *
 * Every expected byte below is written out by hand from mainline Linux's HFI 4xx definitions
 * (drivers/media/platform/qcom/venus/hfi_cmds.c, hfi_msgs.h, hfi_venus.c:32-94, :747-783),
 * not produced by the code under test.
 *
 * What is NOT tested here: the firmware.  Whether this phone's Venus accepts these bytes is a
 * HYPOTHESIS until M1 runs on the phone; the bytes themselves are the ones the sources say.
 *
 * tools/venus-hfi/run.sh compiles and runs this on the build machine, with the header as it is
 * in the kernel tree (or the copy in xnu/files).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/vayu_venus_hfi.h>

static unsigned failures;
static unsigned checks;

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%u: FAILED: %s\n", __FILE__, __LINE__, #condition); \
		failures++; \
	} \
	checks++; \
} while (0)

static void
check_words(const char *what, const uint32_t *got, const uint32_t *want, size_t count)
{
	if (memcmp(got, want, count * sizeof(uint32_t)) != 0) {
		fprintf(stderr, "%s: words differ\n  got:", what);
		for (size_t i = 0; i < count; i++) {
			fprintf(stderr, " %#x", got[i]);
		}
		fprintf(stderr, "\n  want:");
		for (size_t i = 0; i < count; i++) {
			fprintf(stderr, " %#x", want[i]);
		}
		fprintf(stderr, "\n");
		failures++;
	}
	checks++;
}

static void
test_sys_init(void)
{
	uint32_t packet[8] = { 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef,
	                       0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef };
	const uint32_t want[3] = { 12, 0x00010001, 0x00000001 };

	/* Mainline pkt_sys_init(): size 12, HFI_CMD_SYS_INIT, HFI_VIDEO_ARCH_OX. */
	CHECK(vayu_hfi_sys_init(packet, 8) == 12);
	check_words("SYS_INIT", packet, want, 3);
	CHECK(vayu_hfi_sys_init(packet, 2) == 0);        /* a 2-word buffer cannot hold 3 */
	CHECK(vayu_hfi_sys_init(NULL, 8) == 0);
}

static void
test_properties(void)
{
	uint32_t packet[8];
	const uint32_t debug_want[6] = { 24, 0x00010005, 1, 1, 0x18, 1 };
	const uint32_t power_want[5] = { 20, 0x00010005, 1, 5, 1 };
	const uint32_t version_want[4] = { 16, 0x00010006, 1, 6 };
	const uint32_t ping_want[3] = { 12, 0x00210002, 0xbeef };

	CHECK(vayu_hfi_sys_debug_config(packet, 8, 0x18) == 24);
	check_words("debug config", packet, debug_want, 6);

	CHECK(vayu_hfi_sys_codec_power_plane(packet, 8, 1) == 20);
	check_words("codec power plane", packet, power_want, 5);

	CHECK(vayu_hfi_sys_get_image_version(packet, 8) == 16);
	check_words("get image version", packet, version_want, 4);
	CHECK(vayu_hfi_sys_get_image_version(packet, 3) == 0);

	CHECK(vayu_hfi_sys_ping(packet, 8, 0xbeef) == 12);
	check_words("ping", packet, ping_want, 3);

	/* The generic setter refuses no values, more than two, or a short buffer. */
	CHECK(vayu_hfi_sys_set_property(packet, 8, 7, NULL, 1) == 0);
	CHECK(vayu_hfi_sys_set_property(packet, 8, 7, (const uint32_t[]){ 1 }, 0) == 0);
	CHECK(vayu_hfi_sys_set_property(packet, 5, 7, (const uint32_t[]){ 1, 2 }, 2) == 0);
}

static void
test_queue_table(void)
{
	uint32_t table[6];
	const uint32_t table_want[6] = { 0, 0xc0, 0x18, 0x38, 3, 3 };
	volatile uint32_t qhdr[14];
	const uint32_t qhdr_want[14] = { 1, 0x25801000, 0x01010001, 0xc8000 / 4, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0 };

	vayu_hfi_table_init(table);
	check_words("table header", table, table_want, 6);

	memset((void *)qhdr, 0xaa, sizeof(qhdr));
	vayu_hfi_queue_header_init(qhdr, VAYU_HFI_QUEUE_MSG, 0x25801000);
	check_words("queue header", (const uint32_t *)qhdr, qhdr_want, 14);

	/* The debug queue asks for no receive interrupt. */
	vayu_hfi_queue_header_init(qhdr, VAYU_HFI_QUEUE_DBG, 0x27000000);
	CHECK(qhdr[8] == 0);
	CHECK(qhdr[2] == 0x01010002);
	/* And the three queues sit one queue's size apart, as the driver lays them out. */
	CHECK(VAYU_HFI_QUEUE_BYTES == 0xc8000);
	CHECK(VAYU_HFI_TABLE_BYTES + 3 * VAYU_HFI_QUEUE_BYTES == 0x2580c0);
	/* With the SFR's 4 KB in front, that is what the driver tells the firmware it has. */
	CHECK(0x1000 + VAYU_HFI_TABLE_BYTES + 3 * VAYU_HFI_QUEUE_BYTES == 0x2590c0);
	CHECK(0x1000 + VAYU_HFI_TABLE_BYTES + 3 * VAYU_HFI_QUEUE_BYTES <= 0x2600000);
}

static void
test_message_header(void)
{
	uint32_t type = 0;
	const uint32_t good[4] = { 16, 0x00020001, 0, 1 };
	const uint32_t bad_size[4] = { 12, 0x00020001, 0, 1 };

	CHECK(vayu_hfi_message(good, 16, &type));
	CHECK(type == 0x00020001);
	CHECK(!vayu_hfi_message(good, 12, &type));       /* the size word disagrees */
	CHECK(!vayu_hfi_message(bad_size, 16, &type));   /* 12-byte packet, told it is 16 */
	CHECK(!vayu_hfi_message(good, 6, &type));        /* not a whole number of words */
	CHECK(!vayu_hfi_message(NULL, 16, &type));
}

static void
test_sys_init_reply(void)
{
	uint32_t error = 0, properties = 0;
	const uint32_t ok[6] = { 24, 0x00020001, 0, 3, 0x11111111, 0x22222222 };
	const uint32_t refused[6] = { 24, 0x00020001, 2, 3, 1, 2 };
	const uint32_t empty[6] = { 24, 0x00020001, 0, 0, 0, 0 };
	const uint32_t short_pkt[3] = { 12, 0x00020001, 0 };

	CHECK(vayu_hfi_sys_init_done(ok, 24, &error, &properties));
	CHECK(error == 0);
	CHECK(properties == 3);
	CHECK(vayu_hfi_sys_init_ok(ok, 24));
	CHECK(!vayu_hfi_sys_init_ok(refused, 24));       /* error_type != HFI_ERR_NONE */
	CHECK(!vayu_hfi_sys_init_ok(empty, 24));         /* num_properties == 0 */
	CHECK(!vayu_hfi_sys_init_done(short_pkt, 12, &error, &properties));
	CHECK(!vayu_hfi_sys_init_ok(ok, 20));            /* truncated: its own size word says 24 */
}

static void
test_image_version(void)
{
	const uint8_t *text = NULL;
	uint32_t length = 0;
	/* {size, 0x2000a, 1, 6, "VIDEO.IR.1.2"} with no length word. */
	uint32_t plain[7] = { 28, 0x0002000a, 1, 6, 0, 0, 0 };
	/* The same with the string preceded by its length, 12 bytes. */
	uint32_t sized[8] = { 32, 0x0002000a, 1, 6, 12, 0, 0, 0 };
	uint32_t wrong[7] = { 28, 0x0002000a, 1, 5, 0, 0, 0 };

	memcpy(&plain[4], "VIDEO.IR.1.2", 12);
	memcpy(&sized[5], "VIDEO.IR.1.2", 12);

	CHECK(vayu_hfi_image_version(plain, 28, &text, &length));
	CHECK(length == 12);
	CHECK(text != NULL && memcmp(text, "VIDEO.IR.1.2", 12) == 0);

	CHECK(vayu_hfi_image_version(sized, 32, &text, &length));
	CHECK(length == 12);
	CHECK(text != NULL && memcmp(text, "VIDEO.IR.1.2", 12) == 0);

	CHECK(!vayu_hfi_image_version(wrong, 28, &text, &length));
	CHECK(!vayu_hfi_image_version(plain, 16, &text, &length));      /* no data at all */
	CHECK(!vayu_hfi_image_version(plain, 24, &text, &length));      /* size word says 28 */
}

static void
test_ping_and_event(void)
{
	const uint32_t ack[3] = { 12, 0x00220002, 0xbeef };
	const uint32_t other[3] = { 12, 0x00220002, 0xbeee };
	const uint32_t event[6] = { 24, 0x00021001, 0x100, 1, 0xdead, 0xbeef };
	const uint32_t short_event[4] = { 16, 0x00021001, 0x100, 1 };
	uint32_t session = 0, type = 0, data0 = 0, data1 = 0;

	CHECK(vayu_hfi_ping_ack(ack, 12, 0xbeef));
	CHECK(!vayu_hfi_ping_ack(other, 12, 0xbeef));
	CHECK(!vayu_hfi_ping_ack(ack, 8, 0xbeef));

	CHECK(vayu_hfi_event(event, 24, &session, &type, &data0, &data1));
	CHECK(session == 0x100);
	CHECK(type == 1);
	CHECK(data0 == 0xdead);
	CHECK(data1 == 0xbeef);

	CHECK(vayu_hfi_event(short_event, 16, &session, &type, &data0, &data1));
	CHECK(session == 0x100 && type == 1);
	CHECK(data0 == 0 && data1 == 0);
}

static void
test_ring(void)
{
	const uint32_t qsize = VAYU_HFI_QUEUE_BYTES / 4;        /* 0x32000 dwords */

	CHECK(qsize == 0x32000);
	CHECK(vayu_hfi_ring_used(qsize, 0, 0) == 0);
	CHECK(vayu_hfi_ring_free(qsize, 0, 0) == qsize);
	CHECK(vayu_hfi_ring_used(qsize, 10, 20) == 10);
	CHECK(vayu_hfi_ring_free(qsize, 10, 20) == qsize - 10);
	/* A write that wrapped: 5 dwords at the end and 5 at the start, read 0, write 5. */
	CHECK(vayu_hfi_ring_used(qsize, qsize - 5, 5) == 10);
	CHECK(vayu_hfi_ring_free(qsize, qsize - 5, 5) == qsize - 10);
	/* The last free slot is never used: full means used == qsize - 1. */
	CHECK(vayu_hfi_ring_free(qsize, 0, qsize - 1) == 1);

	CHECK(vayu_hfi_ring_advance(qsize, 0, 3) == 3);
	CHECK(vayu_hfi_ring_advance(qsize, qsize - 2, 4) == 2);
	CHECK(vayu_hfi_ring_advance(qsize, qsize - 1, 1) == 0);

	CHECK(vayu_hfi_ring_index_ok(qsize, 0));
	CHECK(vayu_hfi_ring_index_ok(qsize, qsize - 1));
	CHECK(!vayu_hfi_ring_index_ok(qsize, qsize));
	CHECK(!vayu_hfi_ring_index_ok(0, 0));
}

/* The five commands M1 sends, one after another into one buffer, as hfi_exchange() does. */
static void
test_command_sequence(void)
{
	uint32_t buffer[64];
	const uint32_t total = 12 + 24 + 20 + 16 + 12;
	uint32_t at = 0, size;

	size = vayu_hfi_sys_init(&buffer[at / 4], 64 - at / 4);
	CHECK(size == 12);
	at += size;
	size = vayu_hfi_sys_debug_config(&buffer[at / 4], 64 - at / 4, 0x18);
	CHECK(size == 24);
	at += size;
	size = vayu_hfi_sys_codec_power_plane(&buffer[at / 4], 64 - at / 4, 1);
	CHECK(size == 20);
	at += size;
	size = vayu_hfi_sys_get_image_version(&buffer[at / 4], 64 - at / 4);
	CHECK(size == 16);
	at += size;
	size = vayu_hfi_sys_ping(&buffer[at / 4], 64 - at / 4, 0xbeef);
	CHECK(size == 12);
	at += size;
	CHECK(at == total);

	/* Every packet's own size word agrees with the offset the next one starts at. */
	CHECK(buffer[0] == 12);
	CHECK(buffer[3] == 24);
	CHECK(buffer[9] == 20);
	CHECK(buffer[14] == 16);
	CHECK(buffer[18] == 12);
}

int
main(void)
{
	test_sys_init();
	test_properties();
	test_queue_table();
	test_message_header();
	test_sys_init_reply();
	test_image_version();
	test_ping_and_event();
	test_ring();
	test_command_sequence();

	if (failures != 0) {
		fprintf(stderr, "venus-hfi: %u of %u checks FAILED\n", failures, checks);
		return 1;
	}
	printf("venus-hfi: %u checks OK\n", checks);
	return 0;
}
