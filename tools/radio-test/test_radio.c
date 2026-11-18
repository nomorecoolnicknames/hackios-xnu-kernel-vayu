/*
 * The radios' fixed paths, as the driver files have them (run.sh takes the functions out of the kernel
 * tree), against host stand-ins for the IO: BT's kctl H4 ring takes a write whole or refuses it; the
 * vendor-command shim answers the 0xfce2 sub-commands with vayu_bt_vsc_olympic_all; the NFC bridge backs
 * off on a stuck IRQ and stops on a dead bus; the Wi-Fi CE TX withholds WMI payloads and reports a
 * published but unconfirmed descriptor; an open network's frames are not Protected.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define __unused __attribute__((unused))
#define BT_KCTL_TX_SIZE 16384U
#define BT_KCTL_LOG_EVERY 100U
#define BT_TTY_IO_SIZE 256U
#define BT_H4_EVT 4
#define BT_H4_CMD 1

/* ---- BT: the kctl send ring ---- */
typedef int errno_t;
typedef uint32_t u_int32_t;
typedef void *kern_ctl_ref;
struct mbuf { struct mbuf *next; const uint8_t *data; size_t len; };
typedef struct mbuf *mbuf_t;
static unsigned freed;
static mbuf_t mbuf_next(mbuf_t m) { return m->next; }
static const void *mbuf_data(mbuf_t m) { return m->data; }
static size_t mbuf_len(mbuf_t m) { return m->len; }
static void mbuf_freem(mbuf_t m) { freed++; }
static int kctl_lock;
static void lck_mtx_lock(int *lock) { }
static void lck_mtx_unlock(int *lock) { }
static bool g_kctl_connected = true;
static kern_ctl_ref g_kctl_ref = &kctl_lock;
static uint32_t g_kctl_unit = 1;
#include "bt_queue.inc"

static void queue_test(void)
{
	static uint8_t a[BT_KCTL_TX_SIZE], out[BT_KCTL_TX_SIZE];
	struct mbuf second = { NULL, a, 9 }, first = { &second, a, 5 };
	uint32_t tail, head;

	memset(a, 0x71, sizeof(a));
	g_kctl_tx.tail = g_kctl_tx.head = BT_KCTL_TX_SIZE - 3;  /* a copy that wraps */
	assert(bt_kctl_send(g_kctl_ref, 1, NULL, &first, 0) == 0);
	assert(g_kctl_tx.count == 14 && freed == 1);
	assert(bt_kctl_tx_drain(out, sizeof(out)) == 14 && memcmp(out, a, 14) == 0);
	first.next = NULL;
	first.len = BT_KCTL_TX_SIZE - 8;
	assert(bt_kctl_send(g_kctl_ref, 1, NULL, &first, 0) == 0);
	tail = g_kctl_tx.tail;
	head = g_kctl_tx.head;
	first.next = &second;
	first.len = 5;                                           /* the first mbuf fits, the chain does not */
	assert(bt_kctl_send(g_kctl_ref, 1, NULL, &first, 0) == ENOBUFS);
	assert(g_kctl_tx.tail == tail && g_kctl_tx.head == head && g_kctl_tx.count == BT_KCTL_TX_SIZE - 8);
	assert(g_kctl_tx.drops == 1 && g_kctl_tx.dropped == 14 && freed == 3);
	assert(bt_kctl_tx_drain(out, sizeof(out)) == BT_KCTL_TX_SIZE - 8);
	first.next = NULL;
	first.len = sizeof(a);
	assert(bt_kctl_send(g_kctl_ref, 1, NULL, &first, 0) == 0);      /* exactly the ring's room */
	assert(bt_kctl_send(g_kctl_ref, 1, NULL, &second, 0) == ENOBUFS);
	assert(bt_kctl_tx_drain(out, sizeof(out)) == sizeof(a) && memcmp(out, a, sizeof(a)) == 0);
	first.len = sizeof(a) + 1;                               /* larger than the ring: refused, nothing read */
	assert(bt_kctl_send(g_kctl_ref, 1, NULL, &first, 0) == ENOBUFS && g_kctl_tx.count == 0);
	assert(bt_kctl_send(g_kctl_ref, 2, NULL, &second, 0) == 0 && g_kctl_tx.count == 0);  /* another unit: dropped, as before */
	assert(freed == 7);
}

/* ---- BT: the vendor-command shim ---- */
static uint8_t event[512];
static uint32_t event_len;
static uint16_t bt_le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static bool g_leanswer_enabled;
static bool bt_vsc_verbose(void);
static void bt_vsc_trace_synthetic(const uint8_t *p, uint32_t n) { }
static void bt_inject_put(const uint8_t *p, uint32_t n) { memcpy(event, p, n); event_len = n; }
static int quiet_printf(const char *f, ...) { return 0; }
#define printf quiet_printf             /* (the shim's summaries every 50 commands: thousands here) */
#include "bt_vsc.inc"
#undef printf
static bool bt_vsc_verbose(void) { return false; }

static void vsc_test(void)
{
	uint8_t cmd[] = { 1, 0xe2, 0xfc, 1, 0x15 };

	assert(bt_vsc_tx_filter(cmd, sizeof(cmd)) == BT_VSC_PASS);      /* vayu_bt_vsc=0, nothing set */
	g_vsc_mode = 1;
	assert(bt_vsc_tx_filter(cmd, sizeof(cmd)) == BT_VSC_PASS);      /* 0x15 is opt-in */
	g_olympic_bytes = 8;
	assert(bt_vsc_tx_filter(cmd, sizeof(cmd)) == BT_VSC_ANSWERED && event_len == 15);
	for (unsigned i = 7; i < event_len; i++) assert(event[i] == 0); /* vayu_bt_vsc_olympic as it was */
	cmd[4] = 0x36;
	assert(bt_vsc_tx_filter(cmd, sizeof(cmd)) == BT_VSC_PASS);
	for (unsigned mode = 0; mode <= 2; mode++) {
		g_vsc_mode = mode;
		for (unsigned n = 1; n <= 8; n++) {
			g_olympic_all_bytes = (uint8_t)n;
			for (unsigned sub = 0; sub <= 255; sub++) {
				cmd[4] = (uint8_t)sub;
				assert(bt_vsc_tx_filter(cmd, sizeof(cmd)) == BT_VSC_ANSWERED);
				assert(event[0] == 4 && event[1] == 0x0e && event[3] == 1 && event[4] == 0xe2 && event[5] == 0xfc &&
				    event[6] == 0);
				if (sub == 0x0d) {
					/* OLYMPIC_READ_PROFILING_STATS keeps its own answer: four u16 of zeros */
					assert(event_len == 15 && event[2] == 12);
					for (unsigned i = 7; i < event_len; i++) assert(event[i] == 0);
					continue;
				}
				assert(event_len == n + 7 && event[2] == n + 4 && event[7] == sub);
				for (unsigned i = 8; i < event_len; i++) assert(event[i] == 0);
			}
		}
	}
	g_vsc_mode = 0;
	cmd[3] = 0;
	assert(bt_vsc_tx_filter(cmd, 4) == BT_VSC_PASS);                /* no sub-command to echo */
	cmd[1] = 0x03;
	cmd[2] = 0x0c;
	assert(bt_vsc_tx_filter(cmd, 4) == BT_VSC_PASS);                /* another command: untouched */
	g_olympic_all_bytes = g_olympic_bytes = 0;
}

/* ---- NFC: the bridge's RX loop ---- */
#define NFC_BUF 260
#define NSEC_PER_MSEC 1000000U
typedef int wait_result_t;
typedef void (*vayu_nfc_rx_fn)(const uint8_t *, unsigned);
static vayu_nfc_rx_fn bridge_rx;
static volatile bool bridge_running;
static bool bus_dead, i2c_quiet;
static unsigned bridge_generation, bridge_packets_in;
/* vayu_nfc_idle_ms (o6): off, its default, so the loop is the one the scenarios below describe */
static uint32_t bridge_idle_ms;
static uint64_t bridge_idle_after_abs;
static volatile uint64_t bridge_last_abs;
static uint64_t ticks, last_log;
static unsigned recv_count, delay_count, log_count, scenario, last_delay;
static bool irq_high(void) { return true; }
static void nfc_bus_lock(void) { }
static void nfc_bus_unlock(void) { }
static uint64_t mach_absolute_time(void) { return ticks; }
static uint64_t ms_since(uint64_t t) { return ticks - t; }
static void delay_for_interval(unsigned n, unsigned scale)
{
	assert(n >= 10 && n <= 160 && (last_delay == 0 || n == 10 || n == 2 * last_delay || n == 160));
	last_delay = n;
	ticks += n;
	delay_count++;
	if (scenario == 0 && delay_count == 40) bridge_running = false;  /* (the test's own stop: a live bus never ends it) */
}
static int say(const char *fmt, ...)
{
	assert(log_count == 0 || ticks - last_log >= 1000);
	last_log = ticks;
	log_count++;
	return 0;
}
static unsigned nfc_recv(uint8_t *p, unsigned cap, unsigned timeout)
{
	recv_count++;
	if (scenario == 1) bus_dead = true;
	if (scenario == 2 && recv_count == 3) return 3;
	if (scenario == 3 && recv_count % 5 == 0) return 3;
	return 0;
}
static int current_thread(void) { return 0; }
static void thread_terminate(int thread) { }
static void receive_packet(const uint8_t *p, unsigned len)
{ if (scenario != 3 || bridge_packets_in >= 20) bridge_running = false; }
#include "nfc_bridge.inc"

static void nfc_test(void)
{
	for (scenario = 0; scenario < 4; scenario++) {
		recv_count = delay_count = log_count = bridge_packets_in = last_delay = 0;
		ticks = 0;
		bus_dead = false;
		bridge_running = true;
		bridge_rx = receive_packet;
		bridge_generation = 1;
		bridge_thread((void *)(uintptr_t)1, 0);
		assert(!bridge_running);
		if (scenario == 0) {
			/* a stuck IRQ over a live bus: 10, 20, 40, 80, then 160 ms; a line at most once a second */
			assert(recv_count == 40 && delay_count == 40 && ticks == 150 + 36 * 160 && log_count <= ticks / 1000 + 1);
		}
		if (scenario == 1) assert(recv_count == 1 && delay_count == 0 && log_count == 1);   /* a dead bus: stop */
		if (scenario == 2) assert(recv_count == 3 && delay_count == 2 && bridge_packets_in == 1 && log_count == 1);
		if (scenario == 3) assert(recv_count == 100 && ticks == 3000 && log_count <= 4);    /* reset by each packet */
	}
}

/* ---- Wi-Fi: CE TX ---- */
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#include "vayu_wifi_htc_connect.h"
static const struct vayu_htcc_ops *htcc_ops;
static struct { bool tx_init[3]; uint32_t tx_wr[3]; } htcc;
static uint32_t vayu_wmi_mode, vayu_scan_mode = 1;
#include "htc_tx.inc"
static uint8_t ce_ring[8 * 16], ce_buf[2 * VAYU_HTCC_TX_MAX];
static uint32_t ce_wr, ce_rd, ce_delays;
static bool ce_complete;
static char ce_log[16384];
static uint32_t ce_log_len;
static int host_log(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(ce_log + ce_log_len, sizeof(ce_log) - ce_log_len, fmt, ap);
	va_end(ap);
	assert(n >= 0 && (uint32_t)n < sizeof(ce_log) - ce_log_len);
	ce_log_len += (uint32_t)n;
	return n;
}
static uint32_t ce_read(uint32_t id, uint32_t reg) { return reg == HTCC_SR_WR_IDX ? ce_wr : ce_rd; }
static void ce_write(uint32_t id, uint32_t reg, uint32_t value) { ce_wr = value; if (ce_complete) ce_rd = value; }
static void ce_shadow(uint32_t offset, uint32_t value) { ce_write(0, 0, value); }
static void ce_delay(uint32_t ms) { ce_delays += ms; }
static bool ce_source(uint32_t id, uint8_t **ring, uint32_t *n, uint32_t *wr) { *ring = ce_ring; *n = 8; *wr = 0; return true; }

static void ce_test(void)
{
	const struct vayu_htcc_ops ops = {
		.rd = ce_read, .wr = ce_write, .shadow_wr = ce_shadow, .src_ring = ce_source,
		.tx_buf = ce_buf, .tx_pa = 0x10000, .delay_ms = ce_delay, .log = host_log,
	};
	uint8_t payload[160];

	memset(payload, 0xab, sizeof(payload));
	htcc_ops = &ops;
	for (vayu_wmi_mode = 2; vayu_wmi_mode <= 8; vayu_wmi_mode++) {
		memset(&htcc, 0, sizeof(htcc));
		ce_wr = ce_rd = ce_log_len = 0;
		ce_log[0] = 0;
		ce_complete = true;
		assert(htcc_ce_tx(VAYU_HTCC_WMI_TX_CE, payload, sizeof(payload), 1) == 0);
		assert(strstr(ce_log, "WMI payload withheld (152 bytes)") != NULL);
		assert(strstr(ce_log, " ab") == NULL && strstr(ce_log, "+000:") == NULL);   /* no byte of it */
	}
	memset(&htcc, 0, sizeof(htcc));
	ce_wr = ce_rd = ce_log_len = ce_delays = 0;
	ce_complete = false;
	assert(htcc_ce_tx(VAYU_HTCC_HTT_TX_CE, payload, sizeof(payload), 2) == VAYU_HTCC_TX_UNKNOWN);
	assert(ce_wr == 1 && ce_rd == 0 && ce_delays == 2 * HTCC_TX_WAIT_MS);
	assert(htcc_ce_tx(VAYU_HTCC_HTT_TX_CE, payload, sizeof(payload), 2) == -1 && ce_wr == 1);   /* the slot is held */
	ce_rd = ce_wr;                                          /* the engine consumes it late */
	ce_complete = true;
	assert(htcc_ce_tx(VAYU_HTCC_HTT_TX_CE, payload, sizeof(payload), 2) == 0 && ce_wr == 2);
}

/* ---- Wi-Fi: Ethernet -> 802.11 ---- */
#define ETH_HLEN 14U
static struct {
	bool started, linked, protected;
	uint8_t mac[6], bssid[6];
	uint32_t tx_drop;
	uint64_t tx_frames, tx_bytes;
	int (*tx80211)(const uint8_t *, uint32_t, bool);
} net;
static bool sent_clear;
static int capture_frame(const uint8_t *p, uint32_t n, bool clear) { memcpy(event, p, n); event_len = n; sent_clear = clear; return 0; }
#include "net_tx.inc"

static void open_test(void)
{
	uint8_t frame[64] = { 0 };

	net.started = net.linked = true;
	net.tx80211 = capture_frame;
	for (unsigned secured = 0; secured <= 1; secured++) {
		net.protected = secured != 0;
		assert(net_send_eth(frame, sizeof(frame)) == 0);
		assert(event[1] == (secured ? 0x41 : 0x01) && sent_clear == !secured);
		assert(event_len == sizeof(frame) - ETH_HLEN + 32);
	}
}

int main(void)
{
	queue_test();
	vsc_test();
	nfc_test();
	ce_test();
	open_test();
	puts("radio-test: BT whole-write ring, every 0xfce2 sub-command (N=1..8, vsc 0..2, 0x0d its own), NFC backoff and dead-bus stop, "
	    "CE timeout and WMI payload withheld, open/WPA2 TX: PASS");
	return 0;
}
