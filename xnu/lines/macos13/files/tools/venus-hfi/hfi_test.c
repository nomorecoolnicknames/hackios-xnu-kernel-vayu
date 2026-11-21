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
#include <sys/vayu_venus_pwr.h>

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

/* ---- M2/M3: the session packets, from mainline's structs word for word ------------------------- */

static void
test_session_init(void)
{
	uint32_t packet[16] = { 0 };
	const uint32_t want[5] = { 20, 0x00010007, 0x5e551001, 1, 0x00002000 };

	/* hfi_session_init_pkt: size 20, HFI_CMD_SYS_SESSION_INIT, session hash, ENC = 1, HEVC. */
	CHECK(vayu_hfi_session_init(packet, 16, 0x5e551001, VAYU_HFI_SESSION_TYPE_ENC,
	    VAYU_HFI_CODEC_HEVC) == 20);
	check_words("SESSION_INIT", packet, want, 5);

	CHECK(vayu_hfi_session_init(packet, 4, 0x5e551001, 1, 0x2000) == 0);   /* 5 words needed */
	CHECK(vayu_hfi_session_init(packet, 16, 0, 1, 0x2000) == 0);          /* no session id */
	CHECK(vayu_hfi_session_init(packet, 16, 1, 1, 0) == 0);               /* no codec */
	CHECK(vayu_hfi_session_init(NULL, 16, 1, 1, 2) == 0);

	/* H.264 is the same packet with 2 instead of 0x2000. */
	CHECK(vayu_hfi_session_init(packet, 16, 1, VAYU_HFI_SESSION_TYPE_ENC,
	    VAYU_HFI_CODEC_H264) == 20);
	CHECK(packet[4] == 2);
}

static void
test_session_properties(void)
{
	uint32_t packet[16];
	uint32_t session = 0x5e551001;
	const uint32_t fsize[8] = { 32, 0x00011001, 0x5e551001, 1, 0x1001, 1, 1920, 1080 };
	const uint32_t color[7] = { 28, 0x00011001, 0x5e551001, 1, 0x1003, 1, 2 };
	const uint32_t rate[7] = { 28, 0x00011001, 0x5e551001, 1, 0x2001, 2, 0x001e0000 };
	const uint32_t profile[7] = { 28, 0x00011001, 0x5e551001, 1, 0x1005, 1, 0x00000100 };
	const uint32_t bitrate[7] = { 28, 0x00011001, 0x5e551001, 1, 0x02006001, 0x007a1200, 0 };
	const uint32_t scalar[6] = { 24, 0x00011001, 0x5e551001, 1, 0x02005004, 0x01000003 };
	const uint32_t stride[11] = { 44, 0x00011001, 0x5e551001, 1, 0x1002,
		1, 2, 1920, 1080, 1920, 540 };

	CHECK(vayu_hfi_session_frame_size(packet, 16, session, VAYU_HFI_BUFFER_INPUT, 1920, 1080) == 32);
	check_words("SET_PROPERTY FRAME_SIZE", packet, fsize, 8);

	CHECK(vayu_hfi_session_color_format(packet, 16, session, VAYU_HFI_BUFFER_INPUT,
	    VAYU_HFI_COLOR_FORMAT_NV12) == 28);
	check_words("SET_PROPERTY FORMAT NV12", packet, color, 7);

	CHECK(vayu_hfi_session_frame_rate(packet, 16, session, VAYU_HFI_BUFFER_OUTPUT, 30) == 28);
	check_words("SET_PROPERTY FRAME_RATE", packet, rate, 7);

	CHECK(vayu_hfi_session_profile_level(packet, 16, session, VAYU_HFI_HEVC_PROFILE_MAIN,
	    VAYU_HFI_HEVC_LEVEL_51) == 28);
	check_words("SET_PROPERTY PROFILE_LEVEL", packet, profile, 7);

	CHECK(vayu_hfi_session_bitrate(packet, 16, session,
	    VAYU_HFI_PROPERTY_CONFIG_VENC_TARGET_BITRATE, 8000000, 0) == 28);
	check_words("SET_PROPERTY BITRATE", packet, bitrate, 7);

	CHECK(vayu_hfi_session_scalar(packet, 16, session,
	    VAYU_HFI_PROPERTY_PARAM_VENC_RATE_CONTROL, VAYU_HFI_RATE_CONTROL_VBR_CFR) == 24);
	check_words("SET_PROPERTY RATE_CONTROL", packet, scalar, 6);

	CHECK(vayu_hfi_session_property(packet, 16, session,
	    VAYU_HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_INFO, &stride[5], 6) == 44);
	check_words("SET_PROPERTY plane actual info", packet, stride, 11);

	/* A buffer that cannot hold it, a null value array, too many values. */
	CHECK(vayu_hfi_session_property(packet, 5, session, 0x1001, fsize, 3) == 0);
	CHECK(vayu_hfi_session_property(packet, 16, session, 0x1001, NULL, 3) == 0);
	CHECK(vayu_hfi_session_property(packet, 16, session, 0x1001, fsize, 11) == 0);
}

static void
test_session_buffers_and_commands(void)
{
	uint32_t packet[24];
	uint32_t session = 0x5e551001;
	const uint32_t in_addrs[2] = { 0x27e00000, 0x27f00000 };
	const uint32_t out_addrs[2] = { 0x28000000, 0x28010000 };
	const uint32_t set_in[10] = { 40, 0x00011002, 0x5e551001, 1, 0x00300000, 0, 0x00300000, 2,
		0x27e00000, 0x27f00000 };
	const uint32_t set_out[12] = { 48, 0x00011002, 0x5e551001, 2, 0x00100000, 0, 0x00100000, 2,
		0x28000000, 0, 0x28010000, 0 };
	const uint32_t get_req[5] = { 20, 0x00211009, 0x5e551001, 1, 0x00202001 };
	const uint32_t etb[16] = { 64, 0x00211004, 0x5e551001, 0, 0, 0, 0, 0, 0,
		0x00300000, 0x00300000, 0, 7, 0x27e00000, 0, 0 };
	const uint32_t ftb[11] = { 44, 0x00211005, 0x5e551001, 0, 0, 0x00100000, 0, 9,
		0x28000000, 0, 0 };

	CHECK(vayu_hfi_session_set_buffers(packet, 24, session, VAYU_HFI_BUFFER_INPUT, 0x300000,
	    in_addrs, 2) == 40);
	check_words("SET_BUFFERS input", packet, set_in, 10);

	CHECK(vayu_hfi_session_set_buffers(packet, 24, session, VAYU_HFI_BUFFER_OUTPUT, 0x100000,
	    out_addrs, 2) == 48);
	check_words("SET_BUFFERS output", packet, set_out, 12);

	CHECK(vayu_hfi_session_set_buffers(packet, 24, session, 1, 0x1000, NULL, 1) == 0);
	CHECK(vayu_hfi_session_set_buffers(packet, 24, session, 1, 0x1000, in_addrs, 0) == 0);

	CHECK(vayu_hfi_session_get_property(packet, 24, session,
	    VAYU_HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS) == 20);
	check_words("SESSION_GET_PROPERTY", packet, get_req, 5);

	CHECK(vayu_hfi_session_empty_buffer(packet, 24, session, 0x27e00000, 0x300000, 0x300000,
	    7) == 64);
	check_words("EMPTY_BUFFER", packet, etb, 16);

	CHECK(vayu_hfi_session_fill_buffer(packet, 24, session, 0, 0x28000000, 0x100000, 9) == 44);
	check_words("FILL_BUFFER", packet, ftb, 11);

	CHECK(vayu_hfi_session_fill_buffer(packet, 24, session, 0, 0, 0x100000, 9) == 0);
	CHECK(vayu_hfi_session_empty_buffer(packet, 24, session, 0, 0x300000, 0x300000, 7) == 0);

	CHECK(vayu_hfi_session_command(packet, 24, session, VAYU_HFI_CMD_SESSION_LOAD_RESOURCES) == 12);
	check_words("LOAD_RESOURCES", packet, (const uint32_t[]){ 12, 0x00211001, 0x5e551001 }, 3);
	CHECK(vayu_hfi_session_command(packet, 24, session, VAYU_HFI_CMD_SESSION_START) == 12);
	CHECK(packet[1] == 0x00211002);
	CHECK(vayu_hfi_session_command(packet, 2, session, VAYU_HFI_CMD_SESSION_START) == 0);
}

static void
test_session_replies(void)
{
	uint32_t session = 0, error = ~0u, properties = 0;
	const uint32_t init_done[5] = { 20, 0x00020006, 0x5e551001, 0, 3 };
	const uint32_t init_bad[5] = { 20, 0x00020006, 0x5e551001, 7, 0 };
	const uint32_t start_done[4] = { 16, 0x00221002, 0x5e551001, 0 };
	const uint32_t start_bad[4] = { 16, 0x00221002, 0x5e551001, 4 };

	CHECK(vayu_hfi_session_init_done(init_done, 20, &session, &error, &properties));
	CHECK(session == 0x5e551001 && error == 0 && properties == 3);
	CHECK(vayu_hfi_session_init_done(init_bad, 20, &session, &error, &properties));
	CHECK(error == 7);
	CHECK(!vayu_hfi_session_init_done(init_done, 16, &session, &error, &properties));

	CHECK(vayu_hfi_session_done(start_done, 16, 0x00221002, &session, &error));
	CHECK(session == 0x5e551001 && error == 0);
	CHECK(vayu_hfi_session_done(start_bad, 16, 0x00221002, &session, &error));
	CHECK(error == 4);
	CHECK(!vayu_hfi_session_done(start_done, 16, 0x00221001, &session, &error));
	CHECK(!vayu_hfi_session_done(start_done, 12, 0x00221002, &session, &error));
}

static void
test_buffer_requirements(void)
{
	uint32_t session = 0, property = 0, words = 0;
	const uint32_t *data = NULL;
	struct vayu_hfi_buffer_requirement one;
	/* Two records after the five-word header: input and output. */
	const uint32_t info[21] = { 84, 0x00221009, 0x5e551001, 1, 0x00202001,
		1, 0x00300000, 0, 0, 1, 2, 1, 4096,
		2, 0x00100000, 0, 0, 1, 2, 0, 4096 };

	CHECK(vayu_hfi_session_property_info(info, 84, &session, &property, &data, &words));
	CHECK(session == 0x5e551001);
	CHECK(property == 0x00202001);
	CHECK(words == 16);
	CHECK(words / VAYU_HFI_BUFFER_REQUIREMENT_WORDS == 2);

	CHECK(vayu_hfi_buffer_requirement(data, words, 0, &one));
	CHECK(one.type == 1 && one.size == 0x300000 && one.count_actual == 2 && one.alignment == 4096);
	CHECK(vayu_hfi_buffer_requirement(data, words, 1, &one));
	CHECK(one.type == 2 && one.size == 0x100000 && one.contiguous == 0);
	CHECK(!vayu_hfi_buffer_requirement(data, words, 2, &one));
	CHECK(!vayu_hfi_session_property_info(info, 16, &session, &property, &data, &words));
}

static void
test_fill_buffer_done(void)
{
	struct vayu_hfi_fill_done fbd;
	uint32_t session = 0, error = ~0u, filled = 0, buffer = 0;
	/* hfi_msg_session_fbd_compressed_pkt, eighteen words; the fields at their offsets. */
	const uint32_t fbd_packet[18] = { 72, 0x00221008, 0x5e551001, 0, 0, 0, 0x0a, 0, 0, 0,
		0, 0x00100000, 0x00004321, 7, 9, 0x8, 0x28000000, 0 };
	const uint32_t empty[9] = { 36, 0x00221007, 0x5e551001, 0, 0, 0x00300000, 5, 0x27e00000, 0 };

	CHECK(vayu_hfi_session_fill_done(fbd_packet, 72, &fbd));
	CHECK(fbd.session == 0x5e551001);
	CHECK(fbd.error == 0 && fbd.flags == 0x0a);
	CHECK(fbd.offset == 0 && fbd.alloc_len == 0x100000 && fbd.filled_len == 0x4321);
	CHECK(fbd.input_tag == 7 && fbd.output_tag == 9 && fbd.picture_type == 0x8);
	CHECK(fbd.packet_buffer == 0x28000000 && fbd.extradata_buffer == 0);
	CHECK(!vayu_hfi_session_fill_done(fbd_packet, 68, &fbd));   /* its own size word says 72 */

	CHECK(vayu_hfi_session_empty_done(empty, 36, &session, &error, &filled, &buffer));
	CHECK(session == 0x5e551001 && error == 0);
	CHECK(filled == 0x300000 && buffer == 0x27e00000);
	CHECK(!vayu_hfi_session_empty_done(empty, 28, &session, &error, &filled, &buffer));
}

/*
 * The power-up plan of sys/vayu_venus_pwr.h (bsd/dev/vayu_venus.c executes it one step at a
 * time).  The expected orders are written out by hand: k25 is what the phone ran when the mvs0
 * MMCC handshake read 0, and "clocks first" is the order the hypothesis under test needs.
 */
static void
test_power_plan(void)
{
	/* k25: the GDSCs before every video_cc branch and before the GCC AXI clocks. */
	const uint8_t legacy[15] = { 1, 2, 3, 12, 13, 14, 4, 5, 6, 7, 8, 9, 10, 11, 15 };
	/* The hypothesis: every clock, then the three domains. */
	const uint8_t clocks_first[15] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	uint8_t plan[VAYU_VENUS_POWER_STEPS];
	uint8_t small[4] = { 0xee, 0xee, 0xee, 0xee };
	unsigned count;
	int seen;

	/* The enum is 1..15: a step value of 0 is never a plan entry. */
	CHECK(VAYU_VENUS_POWER_RAILS == 1);
	CHECK(VAYU_VENUS_POWER_RESETS == 15);
	CHECK(VAYU_VENUS_POWER_STEPS == 16);

	for (uint32_t mode = 0; mode < VAYU_VENUS_POWER_MODES; mode++) {
		memset(plan, 0, sizeof(plan));
		count = vayu_venus_power_plan(mode, plan, sizeof(plan));
		CHECK(count == 15);
		if (mode == VAYU_VENUS_POWER_LEGACY || mode == VAYU_VENUS_POWER_LEGACY_1) {
			CHECK(memcmp(plan, legacy, sizeof(legacy)) == 0);
		} else {
			CHECK(memcmp(plan, clocks_first, sizeof(clocks_first)) == 0);
		}
		/* Every step exactly once. */
		for (int step = 1; step < VAYU_VENUS_POWER_STEPS; step++) {
			seen = 0;
			for (unsigned i = 0; i < count; i++) {
				if (plan[i] == step) {
					seen++;
				}
			}
			CHECK(seen == 1);
		}
		/* The reset is the last thing, in every order. */
		CHECK(plan[count - 1] == VAYU_VENUS_POWER_RESETS);
	}

	/* The two halves of the hypothesis, spelled out as the relative positions they are about. */
	count = vayu_venus_power_plan(VAYU_VENUS_POWER_CLOCKS_FIRST_1, plan, sizeof(plan));
	CHECK(count == 15);
	CHECK(plan[0] == VAYU_VENUS_POWER_RAILS && plan[1] == VAYU_VENUS_POWER_BCM);
	/* GCC AXI/XO, video_cc XO, PLL0 and the core clocks all before the mvs0 domain. */
	for (unsigned i = 0; i < count; i++) {
		if (plan[i] == VAYU_VENUS_POWER_GDSC_MVS0) {
			CHECK(i == 12);
		}
	}
	count = vayu_venus_power_plan(VAYU_VENUS_POWER_LEGACY, plan, sizeof(plan));
	CHECK(count == 15 && plan[3] == VAYU_VENUS_POWER_GDSC_MVSC && plan[12] == VAYU_VENUS_POWER_MVS0_CORE);

	/* A bad buffer is refused, and nothing is written past it. */
	CHECK(vayu_venus_power_plan(VAYU_VENUS_POWER_CLOCKS_FIRST_1, small, 4) == 0);
	CHECK(small[0] == 0xee && small[1] == 0xee && small[2] == 0xee && small[3] == 0xee);
	CHECK(vayu_venus_power_plan(VAYU_VENUS_POWER_CLOCKS_FIRST_1, NULL, 16) == 0);
	/* An unknown mode is the k25 order, never a new one. */
	memset(plan, 0xff, sizeof(plan));
	CHECK(vayu_venus_power_plan(99, plan, sizeof(plan)) == 15);
	CHECK(memcmp(plan, legacy, sizeof(legacy)) == 0);

	/* Polarity and order, one mode at a time (the two variables of the run). */
	CHECK(vayu_venus_mmcc_control(VAYU_VENUS_POWER_LEGACY) == -1);
	CHECK(vayu_venus_mmcc_control(VAYU_VENUS_POWER_CLOCKS_FIRST_1) == 1);
	CHECK(vayu_venus_mmcc_control(VAYU_VENUS_POWER_CLOCKS_FIRST_0) == 0);
	CHECK(vayu_venus_mmcc_control(VAYU_VENUS_POWER_LEGACY_1) == 1);
	CHECK(vayu_venus_mmcc_control(77) == -1);
	CHECK(!vayu_venus_clocks_first(VAYU_VENUS_POWER_LEGACY));
	CHECK(vayu_venus_clocks_first(VAYU_VENUS_POWER_CLOCKS_FIRST_1));
	CHECK(vayu_venus_clocks_first(VAYU_VENUS_POWER_CLOCKS_FIRST_0));
	CHECK(!vayu_venus_clocks_first(VAYU_VENUS_POWER_LEGACY_1));
	CHECK(!vayu_venus_clocks_first(77));

	/* Every step has a name, and a step outside the enum says so. */
	for (int step = 1; step < VAYU_VENUS_POWER_STEPS; step++) {
		const char *name = vayu_venus_power_step_name(step);

		CHECK(name != NULL && name[0] != 0 && strcmp(name, "?") != 0);
	}
	CHECK(strcmp(vayu_venus_power_step_name(0), "?") == 0);
	CHECK(strcmp(vayu_venus_power_step_name(VAYU_VENUS_POWER_STEPS), "?") == 0);
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
	test_session_init();
	test_session_properties();
	test_session_buffers_and_commands();
	test_session_replies();
	test_buffer_requirements();
	test_fill_buffer_done();
	test_power_plan();

	if (failures != 0) {
		fprintf(stderr, "venus-hfi: %u of %u checks FAILED\n", failures, checks);
		return 1;
	}
	printf("venus-hfi: %u checks OK\n", checks);
	return 0;
}
