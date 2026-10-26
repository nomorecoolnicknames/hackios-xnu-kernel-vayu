/*
 * The 3.5 mm jack of the POCO X3 Pro (vayu, SM8150-AC), step 2b: the WCD934x codec's SLIMbus
 * bring-up, its chip ID, and with vayu_jack=2 its MCLK and init tables; no sound yet. Every
 * address and register below belongs to this phone, and nothing here is touched unless boot-arg
 * vayu_jack=N (needs vayu_audio>=3: the audio DSP and QRTR must already be up, bsd/dev/vayu_audio.c
 * calls this after its step 3).
 *
 * The codec hangs off the audio DSP's SLIMbus, not off a bus of ours: the DSP is the bus master
 * and its NGD (the satellite controller) answers QMI service 0x301 on QRTR. The steps, in order:
 *
 *   J0  the codec's reset and interrupt pins, read only: GPIO 143 active-low reset
 *       (dt msm_cdc_pinctrl@143 qcom,cdc-rst-n-gpio) and GPIO 123 its interrupt, SOUTH tile
 *       (pinctrl-sm8150.c PINGROUP(123/143, SOUTH), 0x1000 per pin). The loader owns TLMM;
 *       vayu_jack_pins=1 there leaves reset out of reset, and without it the codec is held in it.
 *   J1  SELECT_INSTANCE and POWER ACTIVE to the SLIMbus service (slim-msm.c:1740-1747,
 *       1325-1355), on a QRTR port of ours (the Wi-Fi driver's adapter, vayu_qrtr_ops).
 *   J2  the NGD at 0x171c0000 (dt slim@171c0000 "slimbus_physical"), its version in the top
 *       half of the first word (slim-msm-ngd.c:1416-1419), the register block at +0x1000 (v2)
 *       or +0x800 (v1) for cell-index 1 (slim-msm-ngd.c:42-44).
 *   J3  INT_EN, RX_MSGQ_CFG and CFG = 1: register mode, no message queues (slim-msm-ngd.c:
 *       1250-1304, 1479-1498; mainline's DEF_NGD_INT_MASK).
 *   J4  the DSP's MASTER_CAPABILITY and our REPORT_SATELLITE (slim-msm-ngd.c:1586-1598), then
 *       the NGD's own logical address (STATUS bit 1).
 *   J5  the codec's PGD and IFD logical addresses by ADDR_QUERY (slim-msm-ngd.c:1221-1248),
 *       elemental addresses from the device tree.
 *   J6  a WCD register is a page byte (register 0x00) and then a value element at
 *       0x800 + (reg & 0xff) (wcd9xxx-core.c:35,228,268; wcd9xxx-utils.c:37,497-523).
 *   J7  the chip ID bytes 0x0021-0x0024 (mfd/wcd934x.c:129-139), the eight vendor bring-up
 *       writes (wcd9xxx-rst.c:316-325 = mfd/wcd934x.c:142-151), and the ID again; with
 *       vayu_jack=1 also the efuse sense (wcd934x.c:11042-11099).
 *   J8  quiet (vayu_jack=1): 0x0011 <- 0x04 (wcd9xxx-rst.c:284-293), QMI POWER INACTIVE, the
 *       port unbound. The NGD is not written after that: the ADSP may take its clock away.
 *   J9  (vayu_jack=2) LN_BB_CLK2 on through RPMh, the MCLK rate select, the vendor's default
 *       and init tables, the IFD interrupt enables, MCLK2, MCLK on, the efuse sense, and the
 *       SVS release; from here the codec is clocked and ready (wcd934x.c:11151-11290,
 *       10337-10516, 9244-9285).
 *   J10 (vayu_jack=2) ready for the headphone path (vayu_audio's step 4); everything stays up:
 *       no J8 at that point, MCLK on, NGD/QMI powered, our QRTR port open.
 *   J11 (vayu_jack=3) the MBHC armed (wcd_mbhc_initialise): the HS-L pull-up, the plug types, the
 *       bias and clock, the button thresholds; SW_DET is masked and the jack is polled.
 *   J12 (vayu_jack=3) the poll thread "vayu-jack mbhc": RESULT_3 bit 4 every 20 ms, 100 ms of
 *       stable readings per event, the armed edge toggled like the vendor, kern.vayu_jack
 *       updated; the codec stays up for the whole run.
 *
 * Every wait is a poll (delay_for_interval or a short spin), no interrupt is used, and every
 * line goes to the loader's log as well as the kernel's (say()), so the last one says where a
 * dead kernel stopped.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_jack.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {        /* (not on the way out: loader_return runs with interrupts off) */
		printf("%s", line);
	}
	return length;
}

static uint64_t
now_ms(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000000;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

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

/* Hex for the log, at most 32 bytes of it. */
static void
hex(char *to, size_t size, const uint8_t *data, uint32_t length)
{
	uint32_t at = 0;

	to[0] = 0;
	for (uint32_t i = 0; i < length && i < 32 && at + 4 < size; i++) {
		int n = snprintf(to + at, size - at, i == 0 ? "%02x" : " %02x", data[i]);

		if (n <= 0) {
			break;
		}
		at += (uint32_t)n;
	}
}

static bool fail(unsigned step, const char *format, ...) __printflike(2, 3);

/* The one failure line: vayu-jack: FAILED: J<n>: ... */
static bool
fail(unsigned step, const char *format, ...)
{
	char line[240];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say("vayu-jack: FAILED: J%u: %s\n", step, line);
	return false;
}

/* One TID for all of it (QMI transactions and SLIMbus messages): 1..255, never 0. */
static uint8_t tid_counter;

static uint8_t
next_tid(void)
{
	tid_counter++;
	if (tid_counter == 0) {
		tid_counter = 1;
	}
	return tid_counter;
}

/* ---- J0: the codec's pins, read only ------------------------------------------------------------- */

#define TLMM_SOUTH_PHYS         0x03d00000UL
#define PIN_JACK_RESET          143     /* qcom,cdc-rst-n-gpio <&tlmm 143>, active low */
#define PIN_JACK_IRQ            123     /* the codec's interrupt line */

#define PIN_CFG_FUNCTION(cfg)   ((cfg >> 2) & 15)
#define PIN_CFG_PULL(cfg)       (cfg & 3)
#define PIN_CFG_DRIVE(cfg)      ((cfg >> 6) & 7)
#define PIN_CFG_OE              (1u << 9)

static bool
step_pins(void)
{
	vm_offset_t reset, irq;
	uint32_t cfg, io;

	reset = ml_io_map(TLMM_SOUTH_PHYS + 0x1000UL * PIN_JACK_RESET, 0x1000);
	irq = ml_io_map(TLMM_SOUTH_PHYS + 0x1000UL * PIN_JACK_IRQ, 0x1000);
	if (reset == 0 || irq == 0) {
		return fail(0, "the reset/interrupt pins cannot be mapped (reset %#lx irq %#lx)", (unsigned long)reset,
		    (unsigned long)irq);
	}
	cfg = rd(reset);
	io = rd(reset + 4);
	say("vayu-jack: J0: gpio%u (codec reset, active low): cfg %#x: function %u, pull %u, %u mA, %s, in %u out %u\n",
	    PIN_JACK_RESET, cfg, PIN_CFG_FUNCTION(cfg), PIN_CFG_PULL(cfg), (PIN_CFG_DRIVE(cfg) + 1) * 2,
	    (cfg & PIN_CFG_OE) ? "output" : "input", io & 1, (io >> 1) & 1);
	cfg = rd(irq);
	io = rd(irq + 4);
	say("vayu-jack: J0: gpio%u (codec interrupt, only shown): cfg %#x: function %u, pull %u, %u mA, %s, in %u out %u\n",
	    PIN_JACK_IRQ, cfg, PIN_CFG_FUNCTION(cfg), PIN_CFG_PULL(cfg), (PIN_CFG_DRIVE(cfg) + 1) * 2,
	    (cfg & PIN_CFG_OE) ? "output" : "input", io & 1, (io >> 1) & 1);

	cfg = rd(reset);
	io = rd(reset + 4);
	if (PIN_CFG_FUNCTION(cfg) != 0 || !(cfg & PIN_CFG_OE) || !(io & 2)) {
		say("vayu-jack: FAILED: the codec is held in reset (needs the loader's vayu_jack_pins=1)\n");
		return false;
	}
	say("vayu-jack: J0: the codec's reset is out of reset (gpio%u: function 0, output high)\n", PIN_JACK_RESET);
	return true;
}

/* ---- J1: QMI to the ADSP's SLIMbus service ------------------------------------------------------- */

#define QMI_SVC_SLIMBUS         0x0301
#define QMI_SELECT_INSTANCE     0x0020
#define QMI_POWER               0x0021
#define QMI_RESULT_SUCCESS      0

static LCK_GRP_DECLARE(jack_lock_group, "vayu-jack");
static LCK_MTX_DECLARE(jack_qmi_lock, &jack_lock_group);
static LCK_MTX_DECLARE(jack_ngd_lock, &jack_lock_group);       /* one NGD transaction at a time */
static uint8_t jack_qmi_data[256];
static uint32_t jack_qmi_length, jack_qmi_from_node, jack_qmi_from_port;
static bool jack_qmi_ready;
static struct hakios_qrtr_port *jack_port;     /* ours, from the Wi-Fi driver's adapter (vayu_qrtr_ops) on the audio DSP's router */
static struct vayu_qrtr *jack_q;                /* the router and the SLIMbus service, kept for vayu_jack_finish() */
static uint32_t jack_node, jack_service_port;
static bool jack_powered;                       /* POWER ACTIVE succeeded (finish/paths send INACTIVE) */
static bool jack_mclk_on;                       /* LN_BB_CLK2 was voted on (J9) */
static bool jack_ready;                         /* vayu_jack=2 and J9 done: the codec is up, MCLK on */
static uint32_t jack_run;                       /* boot-arg vayu_jack=N, kept for vayu_jack_keep() */
static volatile bool path_live;                 /* J2-B: the headphone path is up (read by the proof thread) */
static volatile bool proof_done;                /* J2-B: the proof thread has left */
static bool proof_running;                      /* J2-B: the proof thread was started and not yet waited for */
static uint64_t proof_began;                    /* J2-B: millisecond clock at P11, for "proof +N ms" */
static bool mic_live;                           /* M1-M9: the handset microphones' path is up */
static volatile bool mbhc_live;                 /* J11/J12: the MBHC poll thread runs while true */
static volatile bool mbhc_done;                 /* J11/J12: the MBHC poll thread has left */
static bool mbhc_running;                       /* J11/J12: the poll thread was started and not yet waited for */
static bool mbhc_armed;                         /* J11: the MBHC bias and clock are on */
static uint32_t mbhc_sw_det;                    /* J12: SW_DET status bits cleared (counted in the logs) */

/* kern.vayu_jack: -1 inactive, 0 none, 1 headphones, 2 headset; the poll thread is the only writer. */
static int jack_state_sysctl = -1;
/* kern.vayu_jack_gen: insert/remove events, so a quick out-in between userland's polls is not lost. */
static int jack_gen_sysctl;
/* kern.vayu_audio_route: the route vayu_audio actually took, 0 speaker, 1 headphones. */
static int audio_route_sysctl;

SYSCTL_INT(_kern, OID_AUTO, vayu_jack, CTLFLAG_RD | CTLFLAG_LOCKED, &jack_state_sysctl, 0,
    "3.5 mm jack: -1 inactive, 0 none, 1 headphones, 2 headset");
SYSCTL_INT(_kern, OID_AUTO, vayu_jack_gen, CTLFLAG_RD | CTLFLAG_LOCKED, &jack_gen_sysctl, 0,
    "3.5 mm jack: insert/remove event counter");
SYSCTL_INT(_kern, OID_AUTO, vayu_audio_route, CTLFLAG_RD | CTLFLAG_LOCKED, &audio_route_sysctl, 0,
    "vayu audio output: 0 speaker, 1 headphones");

/* On the GLINK thread: copy the datagram and wake the waiter; nothing else. */
static void
jack_qmi_input(void *context __unused, uint32_t from_node, uint32_t from_port, const void *data, size_t length)
{
	lck_mtx_lock(&jack_qmi_lock);
	jack_qmi_from_node = from_node;
	jack_qmi_from_port = from_port;
	jack_qmi_length = length < sizeof(jack_qmi_data) ? (uint32_t)length : (uint32_t)sizeof(jack_qmi_data);
	memcpy(jack_qmi_data, data, jack_qmi_length);
	jack_qmi_ready = true;
	lck_mtx_unlock(&jack_qmi_lock);
	wakeup(&jack_qmi_ready);
}

static bool
jack_qmi_take(uint8_t *to, uint32_t *length, uint32_t *from_node, uint32_t *from_port)
{
	bool there;

	lck_mtx_lock(&jack_qmi_lock);
	there = jack_qmi_ready;
	if (there) {
		memcpy(to, jack_qmi_data, jack_qmi_length);
		*length = jack_qmi_length;
		*from_node = jack_qmi_from_node;
		*from_port = jack_qmi_from_port;
		jack_qmi_ready = false;
	}
	lck_mtx_unlock(&jack_qmi_lock);
	return there;
}

/*
 * One QMI request, one response with the same transaction and message id. The wire is the QMI
 * one: [type][txn u16][message u16][payload u16], then TLVs [type][length u16][value]. A
 * response's TLV 0x02 is {result u16, error u16}; result 0 is success.
 */
static bool
qmi_call(unsigned step, struct vayu_qrtr *q, uint32_t node, uint32_t port, uint16_t message, const uint8_t *payload,
    uint32_t length, uint32_t *result, uint32_t *error, bool *have_result)
{
	uint8_t request[64], answer[256];
	uint16_t txn = next_tid();
	uint64_t deadline;
	char text[128];

	*have_result = false;
	*result = 0;
	*error = 0;
	if (length + 7 > sizeof(request)) {
		say("vayu-jack: J%u: a QMI request of %u bytes does not fit\n", step, length);
		return false;
	}
	request[0] = 0;                         /* request */
	request[1] = txn & 0xff;
	request[2] = txn >> 8;
	request[3] = message & 0xff;
	request[4] = message >> 8;
	request[5] = length & 0xff;
	request[6] = length >> 8;
	memcpy(request + 7, payload, length);

	lck_mtx_lock(&jack_qmi_lock);
	jack_qmi_ready = false;
	lck_mtx_unlock(&jack_qmi_lock);
	(void)q;
	if (jack_port == NULL || vayu_qrtr_ops.send(jack_port, node, port, request, length + 7) != 0) {
		say("vayu-jack: J%u: QMI 0x%04x: not sent (GLINK)\n", step, message);
		return false;
	}

	deadline = now_ms() + 1000;
	while (now_ms() < deadline) {
		uint32_t got, from_node, from_port;

		if (jack_qmi_take(answer, &got, &from_node, &from_port)) {
			if (got >= 7 && answer[0] == 2 && (uint32_t)(answer[1] | answer[2] << 8) == txn &&
			    (uint32_t)(answer[3] | answer[4] << 8) == message) {
				uint32_t payload_length = (uint32_t)(answer[5] | answer[6] << 8);
				uint32_t at = 7, end;

				if (payload_length > got - 7) {
					payload_length = got - 7;
				}
				end = 7 + payload_length;
				while (at + 3 <= end) {
					uint32_t type = answer[at], size = (uint32_t)(answer[at + 1] | answer[at + 2] << 8);

					if (at + 3 + size > end) {
						break;
					}
					if (type == 0x02 && size >= 4) {
						*result = (uint32_t)(answer[at + 3] | answer[at + 4] << 8);
						*error = (uint32_t)(answer[at + 5] | answer[at + 6] << 8);
						*have_result = true;
					}
					at += 3 + size;
				}
				hex(text, sizeof(text), answer, got < 32 ? got : 32);
				if (*have_result) {
					say("vayu-jack: J%u: QMI 0x%04x from node %u port %u: result %u error %u, %u bytes: %s\n",
					    step, message, from_node, from_port, *result, *error, got, text);
				} else {
					say("vayu-jack: J%u: QMI 0x%04x from node %u port %u: no result TLV, %u bytes: %s\n",
					    step, message, from_node, from_port, got, text);
				}
				return true;
			}
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	say("vayu-jack: J%u: QMI 0x%04x txn %u: no response in 1000 ms\n", step, message, txn);
	return false;
}

/* Our port closed and the router given back (the adapter holds one router at a time). */
static void
jack_release(struct vayu_qrtr *q)
{
	if (jack_port != NULL) {
		vayu_qrtr_ops.close(jack_port);
		jack_port = NULL;
	}
	vayu_qrtr_detach(q);
}

static bool
step_qmi(struct vayu_qrtr *q, uint32_t *node, uint32_t *port)
{
	uint8_t select[14] = { 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,       /* instance 0 */
	                       0x10, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00 };     /* mode MASTER = 2 */
	uint8_t power[7] = { 0x01, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00 };       /* ACTIVE = 2 */
	uint32_t result, error;
	bool have_result = false, answered;
	unsigned tries;

	if (!vayu_qrtr_find(q, QMI_SVC_SLIMBUS, node, port)) {
		return fail(1, "the audio DSP has no SLIMbus service (%#x)", QMI_SVC_SLIMBUS);
	}
	say("vayu-jack: J1: SLIMbus service %#x at node %u port %u\n", QMI_SVC_SLIMBUS, *node, *port);
	/* the Wi-Fi driver's QRTR adapter: this router behind the QMI helpers' transport, and a port of ours on it */
	vayu_qrtr_attach(q);
	jack_port = vayu_qrtr_ops.open(jack_qmi_input, NULL, NULL);
	if (jack_port == NULL) {
		return fail(1, "no QRTR port of our own");
	}
	say("vayu-jack: J1: our QRTR port %u\n", vayu_qrtr_port_number(jack_port));

	answered = qmi_call(1, q, *node, *port, QMI_SELECT_INSTANCE, select, sizeof(select), &result, &error, &have_result);
	if (!answered) {
		return fail(1, "SELECT_INSTANCE: no answer");
	}
	if (!have_result || result != QMI_RESULT_SUCCESS) {
		return fail(1, "SELECT_INSTANCE: result %u error %u", have_result ? result : 0xffff, have_result ? error : 0xffff);
	}
	say("vayu-jack: J1: SELECT_INSTANCE: instance 0, mode MASTER\n");

	for (tries = 1; tries <= 3; tries++) {
		answered = qmi_call(1, q, *node, *port, QMI_POWER, power, sizeof(power), &result, &error, &have_result);
		if (answered && have_result && result == QMI_RESULT_SUCCESS) {
			say("vayu-jack: J1: POWER ACTIVE on try %u\n", tries);
			return true;
		}
		if (tries < 3) {
			say("vayu-jack: J1: POWER ACTIVE try %u failed; again\n", tries);
		}
	}
	return fail(1, "POWER ACTIVE: no result 0 after 3 tries (result %u error %u)",
	    have_result ? result : 0xffff, have_result ? error : 0xffff);
}

/* ---- J2/J3/J4: the NGD, register mode, the capability exchange ----------------------------------- */

#define NGD_PHYS                0x171c0000UL
#define NGD_MAP_SIZE            0x2000
#define NGD_CFG                 0x00
#define NGD_STATUS              0x04
#define NGD_RX_MSGQ_CFG         0x08
#define NGD_INT_EN              0x10
#define NGD_INT_STAT            0x14
#define NGD_INT_CLR             0x18
#define NGD_TX_MSG              0x30
#define NGD_RX_MSG              0x70

#define NGD_STATUS_LADDR        (1u << 1)
#define NGD_INT_RECFG_DONE      (1u << 24)
#define NGD_INT_TX_NACKED_2     (1u << 25)
#define NGD_INT_BUF_CONTE       (1u << 26)
#define NGD_INT_TX_INVAL        (1u << 27)
#define NGD_INT_DEV_ERR         (1u << 29)
#define NGD_INT_RX_RCVD         (1u << 30)
#define NGD_INT_TX_SENT         (1u << 31)
#define NGD_INT_TX_ERRORS       (NGD_INT_TX_NACKED_2 | NGD_INT_BUF_CONTE | NGD_INT_TX_INVAL | NGD_INT_DEV_ERR)
#define NGD_INT_DEF_MASK        0xfe000000u     /* mainline DEF_NGD_INT_MASK, RX_RCVD included */

static vm_offset_t ngd_base;            /* the device's 0x2000: the version word is at +0 */
static vm_offset_t ngd;                 /* the register block: +0x1000 (v2) or +0x800 (v1) */
static bool ngd_already_up;
static bool ngd_quiet;                  /* J7 on: RX/TX/INT_CLR routine lines are not printed; errors still are */

static void
ngd_clear(unsigned step, uint32_t bits)
{
	wr(ngd + NGD_INT_CLR, bits);
	if (!ngd_quiet || (bits & ~(NGD_INT_RX_RCVD | NGD_INT_TX_SENT)) != 0) {
		say("vayu-jack: J%u: INT_CLR <- %#x (reads %#x)\n", step, bits, rd(ngd + NGD_INT_CLR));
	}
}

static void
ngd_write(unsigned step, const char *what, vm_offset_t address, uint32_t value)
{
	wr(address, value);
	say("vayu-jack: J%u: %s <- %#x (reads %#x)\n", step, what, value, rd(address));
}

/* J3's writes, also used by J4 when a capability wait times out (vendor: reconf HZ x 3). */
static void
ngd_enable(unsigned step)
{
	uint32_t rx_cfg;

	ngd_write(step, "INT_EN", ngd + NGD_INT_EN, NGD_INT_DEF_MASK);
	rx_cfg = rd(ngd + NGD_RX_MSGQ_CFG) | 0x10000;
	ngd_write(step, "RX_MSGQ_CFG", ngd + NGD_RX_MSGQ_CFG, rx_cfg);
	ngd_write(step, "CFG", ngd + NGD_CFG, 0x1);
}

static bool
step_ngd_map(void)
{
	uint32_t word0, ver;

	ngd_base = ml_io_map(NGD_PHYS, NGD_MAP_SIZE);
	if (ngd_base == 0) {
		return fail(2, "the NGD at %#lx cannot be mapped", (unsigned long)NGD_PHYS);
	}
	word0 = rd(ngd_base);
	ver = word0 >> 16;                      /* "Version info in 16 MSbits" (slim-msm-ngd.c:1416-1419) */
	ngd = ngd_base + (ver ? 0x1000 : 0x800);
	say("vayu-jack: J2: NGD at %#lx: word0 %#08x, ver %#x, registers at +%#x\n", (unsigned long)NGD_PHYS, word0, ver,
	    ver ? 0x1000 : 0x800);
	say("vayu-jack: J2: CFG %#x STATUS %#x RX_MSGQ_CFG %#x INT_EN %#x INT_STAT %#x\n", rd(ngd + NGD_CFG),
	    rd(ngd + NGD_STATUS), rd(ngd + NGD_RX_MSGQ_CFG), rd(ngd + NGD_INT_EN), rd(ngd + NGD_INT_STAT));
	return true;
}

static void
step_ngd_up(void)
{
	uint32_t status = rd(ngd + NGD_STATUS), int_en = rd(ngd + NGD_INT_EN);

	if ((status & NGD_STATUS_LADDR) && int_en != 0) {
		ngd_already_up = true;
		say("vayu-jack: J3: already up (STATUS %#x, INT_EN %#x); no writes\n", status, int_en);
		return;
	}
	ngd_enable(3);
	say("vayu-jack: J3: register mode (ENABLE only, no message queues)\n");
}

/* ---- the receive side, shared by J4 to J8 -------------------------------------------------------- */

#define JACK_RX_MESSAGES        8
#define JACK_RX_WORDS           32

static struct {
	uint32_t words[JACK_RX_WORDS];
	uint32_t length;
} jack_rx[JACK_RX_MESSAGES];
static unsigned jack_rx_head, jack_rx_tail;

static void
rx_push(const uint32_t *words, uint32_t length, uint32_t count)
{
	unsigned next = (jack_rx_head + 1) % JACK_RX_MESSAGES;

	if (next == jack_rx_tail) {
		jack_rx_tail = (jack_rx_tail + 1) % JACK_RX_MESSAGES;   /* full: the oldest goes */
	}
	bzero(jack_rx[jack_rx_head].words, sizeof(jack_rx[jack_rx_head].words));
	for (uint32_t i = 0; i < count && i < JACK_RX_WORDS; i++) {
		jack_rx[jack_rx_head].words[i] = words[i];
	}
	jack_rx[jack_rx_head].length = length;
	jack_rx_head = next;
}

/* The oldest message, or false. Its header is words[0], bytes on a little-endian ARM64. */
static bool
rx_take(uint32_t *words, uint32_t *length)
{
	if (jack_rx_tail == jack_rx_head) {
		return false;
	}
	for (unsigned i = 0; i < JACK_RX_WORDS; i++) {
		words[i] = jack_rx[jack_rx_tail].words[i];
	}
	*length = jack_rx[jack_rx_tail].length;
	jack_rx_tail = (jack_rx_tail + 1) % JACK_RX_MESSAGES;
	return true;
}

/*
 * If the NGD says a message is there, read it (RX_MSG's first word carries the length in its low
 * five bits; words 1.. follow it), clear the bit, log it whole, and keep it for the step that
 * waits for it. Returns true if a message was read; capability is set for MT 6 / MC 0x00.
 */
static bool
service_rx(unsigned step, bool *capability)
{
	uint32_t words[JACK_RX_WORDS], word0, length, count;
	char text[128];

	if (!(rd(ngd + NGD_INT_STAT) & NGD_INT_RX_RCVD)) {
		return false;
	}
	bzero(words, sizeof(words));
	word0 = rd(ngd + NGD_RX_MSG);
	length = word0 & 0x1f;
	count = (length + 3) / 4;
	if (count > JACK_RX_WORDS) {
		count = JACK_RX_WORDS;
	}
	words[0] = word0;
	for (uint32_t i = 1; i < count; i++) {
		words[i] = rd(ngd + NGD_RX_MSG + 4 * i);
	}
	ngd_clear(step, NGD_INT_RX_RCVD);
	if (!ngd_quiet || length < 2 || ((const uint8_t *)words)[1] != 0x64) {
		hex(text, sizeof(text), (const uint8_t *)words, length);
		say("vayu-jack: J%u: RX %u bytes: %s (mt %u, mc %#x)\n", step, length, text, (words[0] >> 5) & 7,
		    length >= 2 ? ((const uint8_t *)words)[1] : 0);
	}
	if (capability != NULL && ((words[0] >> 5) & 7) == 6 && length >= 2 && ((const uint8_t *)words)[1] == 0) {
		*capability = true;
	}
	rx_push(words, length, count);
	return true;
}

/*
 * One SLIMbus message: zero the 16-word window, copy the bytes, write (len+3)/4 words, then wait
 * up to 100 ms for TX_SENT; an error bit is written back and returned (bit 27 = invalid, the
 * others NACK-ish). Received messages are serviced meanwhile so none is lost. 0: sent. The
 * caller holds jack_ngd_lock: this is the inside half of one transaction.
 */
static int
tx_send(unsigned step, const uint8_t *bytes, uint32_t length)
{
	uint32_t words[16];
	uint32_t count;
	uint64_t deadline;
	char text[128];

	if (length > sizeof(words)) {
		return -1;
	}
	bzero(words, sizeof(words));
	memcpy(words, bytes, length);
	count = (length + 3) / 4;
	for (uint32_t i = 0; i < count; i++) {
		wr(ngd + NGD_TX_MSG + 4 * i, words[i]);
	}
	if (!ngd_quiet) {
		hex(text, sizeof(text), bytes, length);
		say("vayu-jack: J%u: TX %u bytes: %s\n", step, length, text);
	}

	deadline = now_ms() + 100;
	for (;;) {
		uint32_t stat = rd(ngd + NGD_INT_STAT);

		if (stat & NGD_INT_RX_RCVD) {
			service_rx(step, NULL);
			stat = rd(ngd + NGD_INT_STAT);
		}
		if (stat & NGD_INT_TX_SENT) {
			ngd_clear(step, NGD_INT_TX_SENT);
			return 0;
		}
		if (stat & NGD_INT_TX_ERRORS) {
			uint32_t bits = stat & NGD_INT_TX_ERRORS;

			ngd_clear(step, bits);
			say("vayu-jack: J%u: TX failed, INT_STAT bits %#x\n", step, bits);
			return (int)bits;
		}
		if (now_ms() >= deadline) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	say("vayu-jack: J%u: TX timed out after 100 ms (INT_STAT %#x)\n", step, rd(ngd + NGD_INT_STAT));
	return -1;
}

/* One message, one lock: from the first write to TX_SENT. */
static int
tx_message(unsigned step, const uint8_t *bytes, uint32_t length)
{
	int result;

	lck_mtx_lock(&jack_ngd_lock);
	result = tx_send(step, bytes, length);
	lck_mtx_unlock(&jack_ngd_lock);
	return result;
}

static bool
step_capability(void)
{
	static const uint8_t report[7] = { 0xc7, 0x01, 0xff, 0xd9, 0xc5, 0x01, 0x01 };  /* L 7, MT 6, MC 1 */
	bool capability = false, sent = false;
	uint64_t began;
	unsigned round;

	for (round = 1; round <= 3; round++) {
		capability = false;
		say("vayu-jack: J4: round %u: waiting up to 1000 ms for MASTER_CAPABILITY\n", round);
		began = now_ms();
		while (now_ms() - began < 1000) {
			service_rx(4, &capability);
			if (capability) {
				break;
			}
			delay_for_interval(1, NSEC_PER_MSEC);
		}
		if (!capability) {
			if (round < 3) {
				say("vayu-jack: J4: no MASTER_CAPABILITY in 1000 ms; redoing the bring-up writes\n");
				ngd_enable(4);
			}
			continue;
		}
		say("vayu-jack: J4: MASTER_CAPABILITY received; REPORT_SATELLITE\n");
		for (unsigned attempt = 1; attempt <= 4; attempt++) {
			int error = tx_message(4, report, sizeof(report));

			if (error == 0) {
				sent = true;
				break;
			}
			if (error > 0 && (error & NGD_INT_TX_INVAL)) {
				break;
			}
			if (attempt < 4) {
				say("vayu-jack: J4: REPORT_SATELLITE try %u failed; again in 10 ms\n", attempt);
				delay_for_interval(10, NSEC_PER_MSEC);
			}
		}
		break;
	}
	if (!capability) {
		return fail(4, "no MASTER_CAPABILITY after 3 rounds");
	}
	if (!sent) {
		return fail(4, "REPORT_SATELLITE not sent");
	}
	say("vayu-jack: J4: REPORT_SATELLITE sent\n");

	began = now_ms();
	while (!(rd(ngd + NGD_STATUS) & NGD_STATUS_LADDR) && now_ms() - began < 200) {
		service_rx(4, NULL);
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	say("vayu-jack: J4: STATUS %#x: LADDR %s\n", rd(ngd + NGD_STATUS),
	    (rd(ngd + NGD_STATUS) & NGD_STATUS_LADDR) ? "set" : "NOT SET");
	return true;
}

/* ---- J5: the codec's logical addresses ----------------------------------------------------------- */

static const uint8_t codec_pgd_ea[6] = { 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 };  /* dt tavil_codec elemental-addr */
static const uint8_t codec_ifd_ea[6] = { 0x00, 0x00, 0x50, 0x02, 0x17, 0x02 };  /* dt qcom,cdc-slim-ifd-elemental-addr */

/* The oldest queued message that is an ADDR_REPLY (MT 6, MC 0x0e) for this TID. */
static bool
rx_take_addr_reply(uint8_t tid, uint8_t *la)
{
	uint32_t words[JACK_RX_WORDS], length;

	while (rx_take(words, &length)) {
		const uint8_t *bytes = (const uint8_t *)words;

		if (length >= 11 && ((words[0] >> 5) & 7) == 6 && bytes[1] == 0x0e && bytes[3] == tid) {
			*la = bytes[10];
			return true;
		}
	}
	return false;
}

static bool
addr_query(unsigned step, const char *what, const uint8_t *ea, uint8_t *la)
{
	for (unsigned tries = 1; tries <= 10; tries++) {
		uint8_t frame[10] = { 0x4a, 0x0d, 0xff };      /* L 10, MT 2, MC 0x0d, LA 0xff; TID; EA */
		uint8_t tid = next_tid();
		uint64_t began;
		int error;
		bool found = false;

		frame[3] = tid;
		memcpy(frame + 4, ea, 6);
		lck_mtx_lock(&jack_ngd_lock);
		error = tx_send(step, frame, sizeof(frame));
		if (error == 0) {
			began = now_ms();
			for (;;) {
				service_rx(step, NULL);
				if (rx_take_addr_reply(tid, la)) {
					found = true;
					break;
				}
				if (now_ms() - began >= 100) {
					break;
				}
				delay_for_interval(1, NSEC_PER_MSEC);
			}
		}
		lck_mtx_unlock(&jack_ngd_lock);
		if (found) {
			return true;
		}
		if (error != 0) {
			say("vayu-jack: J%u: %s ADDR_QUERY try %u: TX error %d\n", step, what, tries, error);
		} else {
			say("vayu-jack: J%u: %s ADDR_QUERY try %u: no reply in 100 ms\n", step, what, tries);
		}
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	return false;
}

static bool
step_addresses(uint8_t *pgd_la, uint8_t *ifd_la)
{
	say("vayu-jack: J5: the codec's elemental addresses: PGD %02x %02x %02x %02x %02x %02x, IFD %02x %02x %02x %02x %02x %02x\n",
	    codec_pgd_ea[0], codec_pgd_ea[1], codec_pgd_ea[2], codec_pgd_ea[3], codec_pgd_ea[4], codec_pgd_ea[5],
	    codec_ifd_ea[0], codec_ifd_ea[1], codec_ifd_ea[2], codec_ifd_ea[3], codec_ifd_ea[4], codec_ifd_ea[5]);
	if (!addr_query(5, "PGD", codec_pgd_ea, pgd_la)) {
		return fail(5, "the codec is not on the bus");
	}
	say("vayu-jack: J5: PGD logical address %#x\n", *pgd_la);
	if (*pgd_la == 0xff) {
		return fail(5, "the codec is not on the bus");
	}
	if (!addr_query(5, "IFD", codec_ifd_ea, ifd_la)) {
		*ifd_la = 0xff;
		say("vayu-jack: J5: IFD logical address: no reply\n");
	} else {
		say("vayu-jack: J5: IFD logical address %#x%s\n", *ifd_la, *ifd_la == 0xff ? " (not present)" : "");
	}
	return true;
}

/* ---- J6: a WCD register, over the codec's PGD ----------------------------------------------- */

static uint8_t codec_pgd_la;
static unsigned wcd_step = 6;           /* the J number the register helpers put in their lines (6, then 9/10) */

/* slim_slicesize() plus the element code: (slice | 1<<3) | ((offset & 0xfff) << 4). */
static uint16_t
wcd_ec(uint32_t offset, unsigned bytes)
{
	unsigned slice;

	switch (bytes) {
	case 1: slice = 0; break;
	case 2: slice = 1; break;
	case 3: slice = 2; break;
	case 4: slice = 3; break;
	case 6: slice = 4; break;
	case 8: slice = 5; break;
	case 12: slice = 6; break;
	default: slice = 7; break;
	}
	return (uint16_t)(slice | (1u << 3) | ((offset & 0xfff) << 4));
}

static int wcd_page_written = -1;       /* the page register's last value; the first access always writes it */
static bool wcd_page_quiet;             /* wcd_update prints one line, not the page changes too */

/* The caller holds jack_ngd_lock. */
static bool
wcd_page_locked(uint8_t page)
{
	uint16_t ec;
	uint8_t frame[6];

	if (wcd_page_written == (int)page) {
		return true;
	}
	ec = wcd_ec(0x800, 1);
	frame[0] = 0x06;                        /* L 6 */
	frame[1] = 0x68;                        /* CHANGE_VALUE */
	frame[2] = codec_pgd_la;
	frame[3] = ec & 0xff;
	frame[4] = ec >> 8;
	frame[5] = page;
	if (tx_send(wcd_step, frame, sizeof(frame)) != 0) {
		return false;
	}
	wcd_page_written = page;
	if (!wcd_page_quiet) {
		say("vayu-jack: J%u: page %#x written to codec register 0x0000\n", wcd_step, page);
	}
	return true;
}

/* The oldest queued REPLY_VALUE (MC 0x64) for this TID. */
static bool
rx_take_value(uint8_t tid, uint8_t *value)
{
	uint32_t words[JACK_RX_WORDS], length;

	while (rx_take(words, &length)) {
		const uint8_t *bytes = (const uint8_t *)words;

		if (length >= 5 && bytes[1] == 0x64 && bytes[3] == tid) {
			*value = bytes[4];
			return true;
		}
	}
	return false;
}

/* The caller holds jack_ngd_lock. */
static bool
wcd_read_locked(uint16_t reg, uint8_t *value)
{
	uint16_t ec;
	uint8_t tid;

	if (!wcd_page_locked(reg >> 8)) {
		return false;
	}
	ec = wcd_ec(0x800 + (reg & 0xff), 1);
	for (unsigned tries = 1; tries <= 3; tries++) {
		uint8_t frame[6] = { 0x06, 0x60, 0 };   /* L 6, REQUEST_VALUE */
		uint64_t began;

		tid = next_tid();
		frame[2] = codec_pgd_la;
		frame[3] = tid;
		frame[4] = ec & 0xff;
		frame[5] = ec >> 8;
		if (tx_send(wcd_step, frame, sizeof(frame)) == 0) {
			began = now_ms();
			for (;;) {
				service_rx(wcd_step, NULL);
				if (rx_take_value(tid, value)) {
					return true;
				}
				if (now_ms() - began >= 100) {
					break;
				}
				delay_for_interval(1, NSEC_PER_MSEC);
			}
		}
		if (tries < 3) {
			delay_for_interval(5, NSEC_PER_MSEC);
		}
	}
	return false;
}

static bool
wcd_read_raw(uint16_t reg, uint8_t *value)
{
	bool ok;

	lck_mtx_lock(&jack_ngd_lock);
	ok = wcd_read_locked(reg, value);
	lck_mtx_unlock(&jack_ngd_lock);
	return ok;
}

static bool
wcd_read(uint16_t reg, uint8_t *value)
{
	if (!wcd_read_raw(reg, value)) {
		say("vayu-jack: wcd r 0x%04x: no answer\n", reg);
		return false;
	}
	say("vayu-jack: wcd r 0x%04x = 0x%02x\n", reg, *value);
	return true;
}

/* The caller holds jack_ngd_lock; one change and its readback, no log of its own. */
static bool
wcd_write_locked(uint16_t reg, uint8_t value, uint8_t *back, bool *read)
{
	uint16_t ec;
	uint8_t frame[6];
	bool wrote = false;

	*read = false;
	if (wcd_page_locked(reg >> 8)) {
		ec = wcd_ec(0x800 + (reg & 0xff), 1);
		frame[0] = 0x06;                /* L 6 */
		frame[1] = 0x68;                /* CHANGE_VALUE */
		frame[2] = codec_pgd_la;
		frame[3] = ec & 0xff;
		frame[4] = ec >> 8;
		frame[5] = value;
		wrote = tx_send(wcd_step, frame, sizeof(frame)) == 0;
	}
	if (wrote) {
		*read = wcd_read_locked(reg, back);
	}
	return wrote;
}

static bool
wcd_write(uint16_t reg, uint8_t value)
{
	uint8_t back = 0;
	bool wrote, read = false;

	lck_mtx_lock(&jack_ngd_lock);
	wrote = wcd_write_locked(reg, value, &back, &read);
	lck_mtx_unlock(&jack_ngd_lock);
	if (read) {
		say("vayu-jack: wcd w 0x%04x <- 0x%02x (reads 0x%02x)\n", reg, value, back);
	} else {
		say("vayu-jack: wcd w 0x%04x <- 0x%02x (no readback)\n", reg, value);
	}
	return wrote;
}

/*
 * regmap_update_bits() by hand: read, merge under the mask, write only when the byte changes.
 * One line either way, and read+write is one NGD transaction.
 */
static bool
wcd_update(uint16_t reg, uint8_t mask, uint8_t value)
{
	uint8_t old = 0, new, back = 0;
	bool wrote, read = false;

	lck_mtx_lock(&jack_ngd_lock);
	wcd_page_quiet = true;
	if (!wcd_read_locked(reg, &old)) {
		wcd_page_quiet = false;
		lck_mtx_unlock(&jack_ngd_lock);
		say("vayu-jack: wcd u 0x%04x m%02x v%02x: no answer\n", reg, mask, value);
		return false;
	}
	new = (uint8_t)((old & ~mask) | (value & mask));
	if (new == old) {
		wcd_page_quiet = false;
		lck_mtx_unlock(&jack_ngd_lock);
		say("vayu-jack: wcd u 0x%04x m%02x v%02x: %02x = %02x, kept\n", reg, mask, value, old, new);
		return true;
	}
	wrote = wcd_write_locked(reg, new, &back, &read);
	wcd_page_quiet = false;
	lck_mtx_unlock(&jack_ngd_lock);
	if (read) {
		say("vayu-jack: wcd u 0x%04x m%02x v%02x: %02x -> %02x (reads %02x)\n", reg, mask, value, old, new, back);
	} else if (wrote) {
		say("vayu-jack: wcd u 0x%04x m%02x v%02x: %02x -> %02x (no readback)\n", reg, mask, value, old, new);
	} else {
		say("vayu-jack: wcd u 0x%04x m%02x v%02x: %02x -> %02x: not written\n", reg, mask, value, old, new);
	}
	return wrote;
}

/* ---- the codec's interface device (IFD): no page register, the element is 0x800 + reg ------------ */

static uint8_t codec_ifd_la = 0xff;

/* The caller holds jack_ngd_lock (wcd9xxx-core.c:173-219: read_dev/write_dev with interface=true). */
static bool
ifd_read_locked(uint16_t reg, uint8_t *value)
{
	uint16_t ec = wcd_ec(0x800 + reg, 1);
	uint8_t tid;

	if (codec_ifd_la == 0xff) {
		return false;
	}
	for (unsigned tries = 1; tries <= 3; tries++) {
		uint8_t frame[6] = { 0x06, 0x60, 0 };   /* L 6, REQUEST_VALUE */
		uint64_t began;

		tid = next_tid();
		frame[2] = codec_ifd_la;
		frame[3] = tid;
		frame[4] = ec & 0xff;
		frame[5] = ec >> 8;
		if (tx_send(wcd_step, frame, sizeof(frame)) == 0) {
			began = now_ms();
			for (;;) {
				service_rx(wcd_step, NULL);
				if (rx_take_value(tid, value)) {
					return true;
				}
				if (now_ms() - began >= 100) {
					break;
				}
				delay_for_interval(1, NSEC_PER_MSEC);
			}
		}
		if (tries < 3) {
			delay_for_interval(5, NSEC_PER_MSEC);
		}
	}
	return false;
}

static bool
ifd_read(uint16_t reg, uint8_t *value) __unused
{
	bool ok;

	lck_mtx_lock(&jack_ngd_lock);
	ok = ifd_read_locked(reg, value);
	lck_mtx_unlock(&jack_ngd_lock);
	if (!ok) {
		say("vayu-jack: ifd r 0x%04x: no answer\n", reg);
		return false;
	}
	say("vayu-jack: ifd r 0x%04x = 0x%02x\n", reg, *value);
	return true;
}

/* The caller holds jack_ngd_lock. */
static bool
ifd_write_locked(uint16_t reg, uint8_t value, uint8_t *back, bool *read)
{
	uint16_t ec = wcd_ec(0x800 + reg, 1);
	uint8_t frame[6];

	*read = false;
	if (codec_ifd_la == 0xff) {
		return false;
	}
	frame[0] = 0x06;                        /* L 6 */
	frame[1] = 0x68;                        /* CHANGE_VALUE */
	frame[2] = codec_ifd_la;
	frame[3] = ec & 0xff;
	frame[4] = ec >> 8;
	frame[5] = value;
	if (tx_send(wcd_step, frame, sizeof(frame)) != 0) {
		return false;
	}
	*read = ifd_read_locked(reg, back);
	return true;
}

static bool
ifd_write(uint16_t reg, uint8_t value)
{
	uint8_t back = 0;
	bool wrote, read = false;

	lck_mtx_lock(&jack_ngd_lock);
	wrote = ifd_write_locked(reg, value, &back, &read);
	lck_mtx_unlock(&jack_ngd_lock);
	if (read) {
		say("vayu-jack: ifd w 0x%04x <- 0x%02x (reads 0x%02x)\n", reg, value, back);
	} else {
		say("vayu-jack: ifd w 0x%04x <- 0x%02x (no readback)\n", reg, value);
	}
	return wrote;
}

/* The oldest queued GENERIC_ACK (MT 6, MC 0x25) for this TID; byte 4 is its status. */
static bool
rx_take_ack(uint8_t tid, uint8_t *status)
{
	uint32_t words[JACK_RX_WORDS], length;

	while (rx_take(words, &length)) {
		const uint8_t *bytes = (const uint8_t *)words;

		if (length >= 5 && ((words[0] >> 5) & 7) == 6 && bytes[1] == 0x25 && bytes[3] == tid) {
			*status = bytes[4];
			return true;
		}
	}
	return false;
}

/*
 * One NGD user message: the TID is put at byte tid_index, then its GENERIC_ACK is waited for up to
 * 1000 ms (MT 6, MC 0x25, byte 3 the TID; success is 0x20 in byte 4; slim-msm-ngd.c:1350-1371).
 */
static bool
user_msg(uint8_t *bytes, uint32_t length, unsigned tid_index) __unused
{
	uint8_t tid, status = 0;
	uint64_t began;
	bool acked = false;
	int error;

	if (tid_index >= length || length > 64) {
		say("vayu-jack: a user message of %u bytes with the TID at %u does not fit\n", length, tid_index);
		return false;
	}
	lck_mtx_lock(&jack_ngd_lock);
	tid = next_tid();
	bytes[tid_index] = tid;
	error = tx_send(wcd_step, bytes, length);
	if (error == 0) {
		began = now_ms();
		for (;;) {
			service_rx(wcd_step, NULL);
			if (rx_take_ack(tid, &status)) {
				acked = true;
				break;
			}
			if (now_ms() - began >= 1000) {
				break;
			}
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
	lck_mtx_unlock(&jack_ngd_lock);
	if (!acked) {
		say("vayu-jack: msg %u bytes, TID %u: %s\n", length, tid,
		    error != 0 ? "not sent" : "no GENERIC_ACK in 1000 ms");
		return false;
	}
	say("vayu-jack: msg %u bytes, TID %u: GENERIC_ACK 0x%02x (%s)\n", length, tid, status,
	    (status & 0x20) ? "success" : "not success");
	return (status & 0x20) != 0;
}

/* ---- J7: the chip ID and the vendor bring-up (the efuse comes at J7 or J9, when the clock is up) -- */

static bool
step_codec(void)
{
	uint8_t id[4], after[4];
	bool known;

	for (unsigned i = 0; i < 4; i++) {
		if (!wcd_read(0x0021 + i, &id[i])) {
			return fail(7, "CHIP ID byte %u cannot be read", i);
		}
	}
	known = (id[0] == 0x00 || id[0] == 0x01) && id[1] == 0x00 && id[2] == 0x08 && id[3] == 0x01;
	say("vayu-jack: J7: CHIP ID %02x %02x %02x %02x: %s\n", id[0], id[1], id[2], id[3],
	    known ? (id[0] == 0 ? "WCD934x v1.0" : "WCD934x v1.1") : "UNKNOWN");

	/* wcd9xxx-rst.c:316-325 = mfd/wcd934x.c:142-151, exactly these eight writes. */
	if (!wcd_write(0x0009, 0x01) || !wcd_write(0x071b, 0x19) || !wcd_write(0x071c, 0x15)) {
		return fail(7, "the bring-up writes (0x0009/0x071b/0x071c) did not go");
	}
	spin_us(1100);                          /* 1000-1100 us after SIDO_NEW_VOUT_D_STARTUP */
	if (!wcd_write(0x0011, 0x05) || !wcd_write(0x0011, 0x07) || !wcd_write(0x0009, 0x03) ||
	    !wcd_write(0x0009, 0x07) || !wcd_write(0x0011, 0x03)) {
		return fail(7, "the bring-up writes after the delay did not go");
	}
	say("vayu-jack: J7: the eight bring-up writes are done\n");

	for (unsigned i = 0; i < 4; i++) {
		if (!wcd_read(0x0021 + i, &after[i])) {
			return fail(7, "CHIP ID byte %u cannot be read after bring-up", i);
		}
	}
	say("vayu-jack: J7: CHIP ID after bring-up %02x %02x %02x %02x\n", after[0], after[1], after[2], after[3]);
	return true;
}

/*
 * The efuse sense: 0x0025 bits 0x1e <- 0x10, then bit 0 <- 1, 5500 us, 0x0039 bit 0 sensed, and
 * 0x0037 bit 7 / 0x0038 bit 0 tell a WCD9340 from a WCD9341 (wcd934x.c:11042-11099). Called at
 * J7 with vayu_jack=1 (the clock is not needed for the bus) and at J9 with vayu_jack=2.
 */
static bool
step_efuse(unsigned step)
{
	uint8_t value, status, val37, val38;

	if (!wcd_read(0x0025, &value)) {
		return fail(step, "EFUSE_CTL 0x0025 cannot be read");
	}
	if (!wcd_write(0x0025, (uint8_t)((value & ~0x1e) | 0x10))) {
		return fail(step, "EFUSE_CTL 0x0025 cannot be written");
	}
	if (!wcd_read(0x0025, &value)) {
		return fail(step, "EFUSE_CTL 0x0025 cannot be read back");
	}
	if (!wcd_write(0x0025, (uint8_t)(value | 0x01))) {
		return fail(step, "EFUSE_CTL 0x0025 cannot be started");
	}
	spin_us(5500);                          /* 5000-5500 us */
	if (!wcd_read(0x0039, &status) || !wcd_read(0x0037, &val37) || !wcd_read(0x0038, &val38)) {
		return fail(step, "the efuse result registers cannot be read");
	}
	say("vayu-jack: J%u: efuse 0x0039 %02x (sensed %u), 0x0037 %02x (DSD disabled %u), 0x0038 %02x (SLNQ disabled %u)\n",
	    step, status, status & 1, val37, (val37 >> 7) & 1, val38, val38 & 1);
	if ((val37 & 0x80) && (val38 & 1)) {
		say("vayu-jack: J%u: the codec is a WCD9340\n", step);
	} else if (!(val37 & 0x80) && (val38 & 1)) {
		say("vayu-jack: J%u: the codec is a WCD9341\n", step);
	} else {
		say("vayu-jack: J%u: the codec is neither WCD9340 nor WCD9341 (efuse says UNKNOWN)\n", step);
	}
	return true;
}

/* ---- J9: MCLK and the codec's init (vayu_jack=2) -------------------------------------------------- */

struct wcd_row {
	uint16_t reg;
	uint8_t mask, value;
};

/* wcd934x.c:9535-9571 (tavil_update_reg_defaults): 36 rows, regmap_update_bits, vendor order. */
static const struct wcd_row tavil_codec_reg_defaults[] = {
	{ 0x0629, 0xff, 0x75 },         /* wcd934x.c:9536 BIAS_VBG_FINE_ADJ */
	{ 0x5022, 0xff, 0x7c },         /* wcd934x.c:9537 CODEC_CPR_SVS_CX_VDD */
	{ 0x5023, 0xff, 0x58 },         /* wcd934x.c:9538 CODEC_CPR_SVS2_CX_VDD */
	{ 0x0b53, 0x01, 0x01 },         /* wcd934x.c:9539 CDC_RX0_RX_PATH_DSMDEM_CTL */
	{ 0x0b67, 0x01, 0x01 },         /* wcd934x.c:9540 CDC_RX1_RX_PATH_DSMDEM_CTL */
	{ 0x0b7b, 0x01, 0x01 },         /* wcd934x.c:9541 CDC_RX2_RX_PATH_DSMDEM_CTL */
	{ 0x0b8f, 0x01, 0x01 },         /* wcd934x.c:9542 CDC_RX3_RX_PATH_DSMDEM_CTL */
	{ 0x0ba3, 0x01, 0x01 },         /* wcd934x.c:9543 CDC_RX4_RX_PATH_DSMDEM_CTL */
	{ 0x0bdf, 0x01, 0x01 },         /* wcd934x.c:9544 CDC_RX7_RX_PATH_DSMDEM_CTL */
	{ 0x0bf3, 0x01, 0x01 },         /* wcd934x.c:9545 CDC_RX8_RX_PATH_DSMDEM_CTL */
	{ 0x0b40, 0x1e, 0x18 },         /* wcd934x.c:9546 CDC_COMPANDER8_CTL7 */
	{ 0x0b38, 0x1e, 0x18 },         /* wcd934x.c:9547 CDC_COMPANDER7_CTL7 */
	{ 0x0b49, 0x08, 0x00 },         /* wcd934x.c:9548 CDC_RX0_RX_PATH_SEC0 */
	{ 0x0c03, 0x03, 0x00 },         /* wcd934x.c:9549 CDC_CLSH_DECAY_CTRL */
	{ 0x066c, 0x07, 0x01 },         /* wcd934x.c:9550 MICB1_TEST_CTL_2 */
	{ 0x0c1b, 0x3f, 0x12 },         /* wcd934x.c:9551 CDC_BOOST0_BOOST_CFG1 */
	{ 0x0c1c, 0x1c, 0x08 },         /* wcd934x.c:9552 CDC_BOOST0_BOOST_CFG2 */
	{ 0x0c23, 0x3f, 0x12 },         /* wcd934x.c:9553 CDC_BOOST1_BOOST_CFG1 */
	{ 0x0c24, 0x1c, 0x08 },         /* wcd934x.c:9554 CDC_BOOST1_BOOST_CFG2 */
	{ 0x0213, 0x1f, 0x09 },         /* wcd934x.c:9555 CPE_SS_CPARMAD_BUFRDY_INT_PERIOD */
	{ 0x0a33, 0x01, 0x00 },         /* wcd934x.c:9556 CDC_TX0_TX_PATH_CFG1 */
	{ 0x0a43, 0x01, 0x00 },         /* wcd934x.c:9557 CDC_TX1_TX_PATH_CFG1 */
	{ 0x0a53, 0x01, 0x00 },         /* wcd934x.c:9558 CDC_TX2_TX_PATH_CFG1 */
	{ 0x0a63, 0x01, 0x00 },         /* wcd934x.c:9559 CDC_TX3_TX_PATH_CFG1 */
	{ 0x0a73, 0x01, 0x00 },         /* wcd934x.c:9560 CDC_TX4_TX_PATH_CFG1 */
	{ 0x0a83, 0x01, 0x00 },         /* wcd934x.c:9561 CDC_TX5_TX_PATH_CFG1 */
	{ 0x0a93, 0x01, 0x00 },         /* wcd934x.c:9562 CDC_TX6_TX_PATH_CFG1 */
	{ 0x0aa3, 0x01, 0x00 },         /* wcd934x.c:9563 CDC_TX7_TX_PATH_CFG1 */
	{ 0x0ab3, 0x01, 0x00 },         /* wcd934x.c:9564 CDC_TX8_TX_PATH_CFG1 */
	{ 0x06b6, 0x0f, 0x02 },         /* wcd934x.c:9565 RX_OCP_CTL */
	{ 0x06ce, 0xff, 0x3a },         /* wcd934x.c:9566 HPH_OCP_CTL */
	{ 0x06d4, 0x01, 0x01 },         /* wcd934x.c:9567 HPH_L_TEST */
	{ 0x06d7, 0x01, 0x01 },         /* wcd934x.c:9568 HPH_R_TEST */
	{ 0x0111, 0xff, 0x20 },         /* wcd934x.c:9569 CPE_FLL_CONFIG_CTL_2 */
	{ 0x0721, 0x0c, 0x00 },         /* wcd934x.c:9570 MBHC_NEW_CTL_2 */
	{ 0x0003, 0x04, 0x04 },         /* wcd934x.c:9571 CODEC_RPM_CLK_MCLK_CFG (source select) */
};

/* wcd934x.c:9602-9631 (tavil_codec_init_reg, common): 28 rows. */
static const struct wcd_row tavil_codec_reg_init_common_val[] = {
	{ 0x0c0a, 0x0f, 0x00 },         /* wcd934x.c:9603 CDC_CLSH_K2_MSB */
	{ 0x0c0b, 0xff, 0x60 },         /* wcd934x.c:9604 CDC_CLSH_K2_LSB */
	{ 0x021b, 0x80, 0x00 },         /* wcd934x.c:9605 CPE_SS_DMIC_CFG */
	{ 0x0c1a, 0x7c, 0x58 },         /* wcd934x.c:9606 CDC_BOOST0_BOOST_CTL */
	{ 0x0c22, 0x7c, 0x58 },         /* wcd934x.c:9607 CDC_BOOST1_BOOST_CTL */
	{ 0x0bcf, 0x08, 0x08 },         /* wcd934x.c:9608 CDC_RX7_RX_PATH_CFG1 */
	{ 0x0be3, 0x08, 0x08 },         /* wcd934x.c:9609 CDC_RX8_RX_PATH_CFG1 */
	{ 0x0d82, 0x02, 0x02 },         /* wcd934x.c:9610 CDC_TOP_TOP_CFG1 */
	{ 0x0d82, 0x01, 0x01 },         /* wcd934x.c:9611 CDC_TOP_TOP_CFG1 */
	{ 0x0ac3, 0x01, 0x01 },         /* wcd934x.c:9612 CDC_TX9_SPKR_PROT_PATH_CFG0 */
	{ 0x0ac7, 0x01, 0x01 },         /* wcd934x.c:9613 CDC_TX10_SPKR_PROT_PATH_CFG0 */
	{ 0x0acb, 0x01, 0x01 },         /* wcd934x.c:9614 CDC_TX11_SPKR_PROT_PATH_CFG0 */
	{ 0x0acf, 0x01, 0x01 },         /* wcd934x.c:9615 CDC_TX12_SPKR_PROT_PATH_CFG0 */
	{ 0x006c, 0x01, 0x01 },         /* wcd934x.c:9616 DATA_HUB_SB_TX11_INP_CFG */
	{ 0x0d42, 0x01, 0x01 },         /* wcd934x.c:9617 CDC_CLK_RST_CTRL_FS_CNT_CONTROL */
	{ 0x0b34, 0x80, 0x80 },         /* wcd934x.c:9618 CDC_COMPANDER7_CTL3 */
	{ 0x0b3c, 0x80, 0x80 },         /* wcd934x.c:9619 CDC_COMPANDER8_CTL3 */
	{ 0x0b38, 0x01, 0x01 },         /* wcd934x.c:9620 CDC_COMPANDER7_CTL7 */
	{ 0x0b40, 0x01, 0x01 },         /* wcd934x.c:9621 CDC_COMPANDER8_CTL7 */
	{ 0x0002, 0x08, 0x00 },         /* wcd934x.c:9622 CODEC_RPM_CLK_GATE */
	{ 0x8019, 0xff, 0x0a },         /* wcd934x.c:9623 TLMM_DMIC3_CLK_PINCFG */
	{ 0x801a, 0xff, 0x0a },         /* wcd934x.c:9624 TLMM_DMIC3_DATA_PINCFG */
	{ 0x0214, 0x60, 0x00 },         /* wcd934x.c:9625 CPE_SS_SVA_CFG */
	{ 0x021c, 0x10, 0x10 },         /* wcd934x.c:9626 CPE_SS_CPAR_CFG */
	{ 0x066b, 0xff, 0xfa },         /* wcd934x.c:9627 MICB1_TEST_CTL_1 */
	{ 0x066e, 0xff, 0xfa },         /* wcd934x.c:9628 MICB2_TEST_CTL_1 */
	{ 0x0671, 0xff, 0xfa },         /* wcd934x.c:9629 MICB3_TEST_CTL_1 */
	{ 0x0674, 0xff, 0xfa },         /* wcd934x.c:9630 MICB4_TEST_CTL_1 */
};

/* wcd934x.c:9574-9581: the six rows only a 1.1 chip gets, after the common table. */
static const struct wcd_row tavil_codec_reg_init_1_1[] = {
	{ 0x0b08, 0x1e, 0x06 },         /* wcd934x.c:9575 CDC_COMPANDER1_CTL7 */
	{ 0x0b10, 0x1e, 0x06 },         /* wcd934x.c:9576 CDC_COMPANDER2_CTL7 */
	{ 0x0733, 0xff, 0x84 },         /* wcd934x.c:9577 HPH_NEW_INT_RDAC_HD2_CTL_L */
	{ 0x0736, 0xff, 0x84 },         /* wcd934x.c:9578 HPH_NEW_INT_RDAC_HD2_CTL_R */
	{ 0x0b85, 0xfc, 0xf4 },         /* wcd934x.c:9579 CDC_RX3_RX_PATH_SEC0 */
	{ 0x0b99, 0xfc, 0xf4 },         /* wcd934x.c:9580 CDC_RX4_RX_PATH_SEC0 */
};

static bool
wcd_table(const char *what, const struct wcd_row *rows, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		if (!wcd_update(rows[i].reg, rows[i].mask, rows[i].value)) {
			say("vayu-jack: J9: %s row %u (0x%04x) did not go\n", what, i + 1, rows[i].reg);
			return false;
		}
	}
	return true;
}

/* A raw RPMh write at a command-database address, + offset. LN_BB_CLK2's enable is at +4
 * (mainline clk-rpmh.c:19,350; vayu DT recovery-live.dts:10430, 27555-27562). */
static bool
vayu_rpmh_write(const char *name, uint32_t offset, uint32_t value)
{
	uint32_t address = vayu_rpmh_address(name);
	uint32_t at, v = value;
	bool done;

	if (address == 0) {
		say("vayu-jack: rpmh: no \"%s\" in the command database\n", name);
		return false;
	}
	at = address + offset;
	done = vayu_rpmh_send(&at, &v, 1, "vayu-jack");
	say("vayu-jack: rpmh: %s+%u <- %u: %s\n", name, offset, value, done ? "DONE" : "NOT DONE");
	return done;
}

/* TAVIL_INIT section 8 "Playback off / MCLK off" (wcd9xxx-resmgr-v2.c:316-342, 533-543). */
static void
mclk_off(void)
{
	wcd_update(0x0202, 0x01, 0x00);         /* the SVS vote, released with the clock (tavil_vote_svs(false)) */
	wcd_update(0x0711, 0x81, 0x00);
	wcd_update(0x0606, 0x04, 0x00);
	spin_us(110);                           /* 100-110 us */
	wcd_update(0x0606, 0x03, 0x00);
	spin_us(110);
	wcd_update(0x0603, 0x80, 0x00);
	spin_us(110);
	wcd_update(0x0601, 0x80, 0x00);
	wcd_update(0x0601, 0x20, 0x00);
}

/*
 * The codec still answering after the SIDO source switch? a test run's retry (run 223422) did the same J9 writes as
 * run 220922, read back 0x0606 = 07, and from the next message on every REQUEST_VALUE was NACKed (INT_STAT bit 25,
 * TX_NACKED_2) -- the codec had left the bus or come back at another logical address. HYPOTHESIS: the digital
 * supply's switch to RCO/BG can drop the codec's SLIMbus interface for a moment. So: wait a little, read 0x0011;
 * if that fails, ask the manager again for the PGD and IFD logical addresses and try once more. Every attempt is
 * logged; this is evidence either way.
 */
static bool
codec_still_there(unsigned step, const char *after)
{
	uint8_t v = 0, la = 0xff;

	for (unsigned attempt = 1; attempt <= 5; attempt++) {
		delay_for_interval(attempt == 1 ? 1 : 10, NSEC_PER_MSEC);
		if (wcd_read_raw(0x0011, &v)) {
			if (attempt > 1) {
				say("vayu-jack: J%u: after %s the codec answers again on try %u (0x0011=%02x)\n", step, after, attempt, v);
			}
			return true;
		}
		say("vayu-jack: J%u: after %s the codec does not answer (try %u): asking the manager for its address again\n",
		    step, after, attempt);
		if (addr_query(step, "PGD", codec_pgd_ea, &la) && la != 0xff) {
			if (la != codec_pgd_la) {
				say("vayu-jack: J%u: the codec's PGD came back at LA %#x (was %#x)\n", step, la, codec_pgd_la);
			}
			codec_pgd_la = la;
			wcd_page_written = -1;
		}
		if (addr_query(step, "IFD", codec_ifd_ea, &la) && la != 0xff) {
			codec_ifd_la = la;
		}
	}
	return fail(step, "the codec is gone after %s (5 tries, addresses asked again)", after);
}

static bool
step_mclk_init(void)
{
	uint8_t v11, v711, v0d41, v606, v601;

	wcd_step = 9;
	if (codec_ifd_la == 0xff) {
		return fail(9, "the codec's IFD logical address is not known (J5)");
	}

	/* 1. the external 9.6 MHz clock, from the PMIC through RPMh */
	if (!vayu_rpmh_write("lnbclka2", 4, 1)) {
		return fail(9, "LN_BB_CLK2 (the codec's 9.6 MHz MCLK) did not come on");
	}
	jack_mclk_on = true;
	say("vayu-jack: J9: MCLK: LN_BB_CLK2 on (lnbclka2+4 <- 1)\n");

	/* 2. the rate: 9.6 MHz (wcd934x.c:11249-11256) */
	if (!wcd_update(0x0003, 0x03, 0x01)) {
		return fail(9, "the MCLK rate select 0x0003 did not go");
	}
	say("vayu-jack: J9: MCLK rate 9.6 MHz (0x0003 m03 v01)\n");

	/* 3-4. the vendor's tables (wcd934x.c:9535-9571, 9602-9631, 9574-9581) */
	if (!wcd_table("tavil_codec_reg_defaults", tavil_codec_reg_defaults,
	    sizeof(tavil_codec_reg_defaults) / sizeof(tavil_codec_reg_defaults[0]))) {
		return fail(9, "the codec's default table did not go");
	}
	say("vayu-jack: J9: tavil_codec_reg_defaults: 36 rows done\n");
	if (!wcd_table("tavil_codec_reg_init_common_val", tavil_codec_reg_init_common_val,
	    sizeof(tavil_codec_reg_init_common_val) / sizeof(tavil_codec_reg_init_common_val[0]))) {
		return fail(9, "the common init table did not go");
	}
	say("vayu-jack: J9: tavil_codec_reg_init_common_val: 28 rows done\n");
	if (!wcd_table("tavil_codec_reg_init_1_1", tavil_codec_reg_init_1_1,
	    sizeof(tavil_codec_reg_init_1_1) / sizeof(tavil_codec_reg_init_1_1[0]))) {
		return fail(9, "the 1.1 init table did not go");
	}
	say("vayu-jack: J9: tavil_codec_reg_init_1_1: 6 rows done\n");

	/* 5. the IFD port interrupts (wcd934x.c:9718-9726) */
	if (!ifd_write(0x30, 0xff) || !ifd_write(0x31, 0xff) || !ifd_write(0x32, 0xff)) {
		return fail(9, "the IFD port interrupt enables (0x0030-0x0032) did not go");
	}
	say("vayu-jack: J9: IFD 0x0030-0x0032 <- 0xff\n");

	/* 6. MCLK2 (wcd934x.c:9507, 10146-10157) */
	if (!wcd_update(0x0712, 0x60, 0x20)) {
		return fail(9, "MCLK2 (0x0712 m60 v20) did not go");
	}
	say("vayu-jack: J9: MCLK2 (0x0712 m60 v20)\n");

	/* 7. MCLK on (wcd934x.c:9244-9285; M1 skipped: the digital core is up after bring-up) */
	if (!wcd_read_raw(0x0011, &v11)) {
		return fail(9, "0x0011 cannot be read before MCLK on");
	}
	say("vayu-jack: J9: MCLK on: M1 skipped (the digital core is up after bring-up), 0x0011=%02x\n", v11);
	if (!wcd_update(0x0202, 0x01, 0x01)) {
		return fail(9, "the SVS vote (0x0202 m01 v01) did not go");
	}
	if (!wcd_update(0x0601, 0x80, 0x80) || !wcd_update(0x0601, 0x40, 0x40)) {
		return fail(9, "the master bias (0x0601 m80/m40) did not go");
	}
	spin_us(1100);                          /* 1000-1100 us (wcd9xxx-resmgr-v2.c:189) */
	if (!wcd_update(0x0601, 0x40, 0x00) || !wcd_update(0x0601, 0x20, 0x00)) {
		return fail(9, "the master bias (0x0601 m40/m20) did not settle");
	}
	if (!wcd_update(0x0711, 0x80, 0x80) || !wcd_update(0x0711, 0x30, 0x10) ||
	    !wcd_update(0x0711, 0x02, 0x00) || !wcd_update(0x0711, 0x01, 0x01) ||
	    !wcd_update(0x0711, 0x02, 0x00) || !wcd_update(0x0d42, 0x01, 0x01) ||
	    !wcd_update(0x0d41, 0x01, 0x01) || !wcd_update(0x0002, 0x03, 0x00)) {
		return fail(9, "the MCLK block (0x0711/0x0D42/0x0D41/0x0002) did not go");
	}
	spin_us(15);                            /* 10-15 us (wcd9xxx-resmgr-v2.c:289) */
	if (!wcd_update(0x0603, 0x80, 0x80)) {
		return fail(9, "the RCO (0x0603 m80) did not go");
	}
	spin_us(110);                           /* 100-110 us */
	if (!wcd_update(0x0606, 0x02, 0x02)) {
		return fail(9, "the SIDO (0x0606 m02) did not go");
	}
	spin_us(110);
	if (!wcd_update(0x0606, 0x01, 0x01)) {
		return fail(9, "the SIDO (0x0606 m01) did not go");
	}
	spin_us(110);
	if (!wcd_update(0x0606, 0x04, 0x04)) {
		return fail(9, "the SIDO (0x0606 m04) did not go");
	}
	spin_us(110);
	if (!codec_still_there(9, "the SIDO switch to RCO/BG")) {
		return false;
	}
	say("vayu-jack: J9: MCLK on: SVS, master bias, MCLK block and RCO/BG SIDO done\n");

	/* 8. the efuse (it needs the clock) */
	if (!step_efuse(9)) {
		return false;
	}

	/*
	 * 9. The SVS vote stays while MCLK is on: the vendor releases it at the end of its probe because its MCLK
	 * goes off there (wcd934x.c:10503), and votes it again on every MCLK enable for playback/record
	 * (__tavil_cdc_mclk_enable_locked -> tavil_vote_svs(true), wcd934x.c:9258, 797-822). jack2 (a test run) released
	 * it with MCLK on: RX ports overflowed and the decimators gave zeros. Released in mclk_off().
	 */
	say("vayu-jack: J9: SVS vote kept while MCLK is on (0x0202 bit 0 stays 1)\n");

	/* 10. the ready line */
	if (!wcd_read_raw(0x0011, &v11) || !wcd_read_raw(0x0711, &v711) || !wcd_read_raw(0x0d41, &v0d41) ||
	    !wcd_read_raw(0x0606, &v606) || !wcd_read_raw(0x0601, &v601)) {
		return fail(9, "the codec's ready registers cannot be read");
	}
	say("vayu-jack: J9: codec ready: 0x0011=%02x 0x0711=%02x 0x0D41=%02x 0x0606=%02x 0x0601=%02x\n", v11, v711,
	    v0d41, v606, v601);
	return true;
}

/* ---- J10: everything is up; the headphone path is vayu_audio's step 4 ---------------------------- */

static bool
step_ready(void)
{
	wcd_step = 10;
	say("vayu-jack: J10: ready for the headphone path (vayu_audio step 4 with vayu_sound=2)\n");
	return true;
}

/* ---- J11/J12: the MBHC armed and polled (vayu_jack=3) ------------------------------------------- */

/*
 * J11: wcd_mbhc_initialise (wcd-mbhc-v2.c:1394-1503; MBHC_AFE_SEQ.md A2.C C1-C16) for this board:
 * hphl_swh=1 and gnd_swh=1, no USB-C analog audio. SW_DET is masked because this driver polls the
 * jack instead of taking the codec's interrupt line (the line is never requested).
 */
static const struct wcd_row mbhc_arm_rows[] = {
	{ 0x0722, 0xC0, 0x80 },         /* C1  HS L pull-up 2 uA */
	{ 0x0721, 0x0C, 0x04 },         /* C2  moisture Rref 24 k */
	{ 0x0614, 0x10, 0x10 },         /* C4  HPHL plug type NO (hphl_swh = 1) */
	{ 0x0614, 0x08, 0x08 },         /* C5  GND plug type NO (gnd_swh = 1) */
	{ 0x0614, 0x01, 0x01 },         /* C6  HPH LP 100 k to GND */
	{ 0x0614, 0x04, 0x04 },         /* C8  HS L det pull-up comp */
	{ 0x0614, 0x80, 0x80 },         /* C9  L_DET_EN */
	{ 0x0722, 0x0F, 0x09 },         /* C10 ins/rem debounce ~96 ms */
	{ 0x0720, 0x03, 0x02 },         /* C11 button debounce */
	{ 0x0615, 0x01, 0x01 },         /* C13 MBHC bias on */
	{ 0x0720, 0x80, 0x80 },         /* C14 MBHC clock on */
	{ 0x0721, 0x03, 0x02 },         /* C15 HS_VREF */
	{ 0x061A, 0xFC, 0x18 },         /* C16 BTN0 */
	{ 0x061B, 0xFC, 0x50 },         /* C16 BTN1 */
	{ 0x061C, 0xFC, 0xF0 },         /* C16 BTN2 */
	{ 0x061D, 0xFC, 0xF0 },         /* C16 BTN3 */
	{ 0x061E, 0xFC, 0xF0 },         /* C16 BTN4 */
	{ 0x061F, 0xFC, 0xF0 },         /* C16 BTN5 */
	{ 0x0620, 0xFC, 0xF0 },         /* C16 BTN6 */
	{ 0x0621, 0xFC, 0xF0 },         /* C16 BTN7 */
	{ 0x040A, 0x01, 0x01 },         /* SW_DET masked: the poll thread owns the jack */
};

static bool
step_mbhc_arm(void)
{
	uint8_t mech = 0, result3 = 0, status1 = 0;

	wcd_step = 11;
	mbhc_armed = true;              /* even a partial arm is brought down by mbhc_stop() */
	for (unsigned i = 0; i < sizeof(mbhc_arm_rows) / sizeof(mbhc_arm_rows[0]); i++) {
		if (!wcd_update(mbhc_arm_rows[i].reg, mbhc_arm_rows[i].mask, mbhc_arm_rows[i].value)) {
			return fail(11, "0x%04x m%02x v%02x did not go", mbhc_arm_rows[i].reg, mbhc_arm_rows[i].mask,
			    mbhc_arm_rows[i].value);
		}
	}
	if (!wcd_read_raw(0x0614, &mech) || !wcd_read_raw(0x0619, &result3) || !wcd_read_raw(0x0412, &status1)) {
		return fail(11, "the MBHC state registers (0x0614/0x0619/0x0412) cannot be read");
	}
	jack_state_sysctl = (result3 & 0x10) ? 0 : 1;
	say("vayu-jack: J11: MBHC armed: MECH 0x0614=%02x RESULT_3 0x0619=%02x STATUS1 0x0412=%02x -> %s\n",
	    mech, result3, status1, (result3 & 0x10) ? "no plug" : "plug");
	return true;
}

#define MBHC_POLL_MS    20      /* one RESULT_3 reading every 20 ms */
#define MBHC_STABLE     5       /* 5 in a row = 100 ms before an edge becomes an event */
#define MBHC_ALIVE_MS   10000   /* one status line every 10 s */

/* One poll transaction: the two reads and the SW_DET clear under one jack_ngd_lock. */
static bool
mbhc_poll(uint8_t *result3, uint8_t *status1, bool *sw_det, bool *cleared)
{
	uint8_t back = 0;
	bool ok, quiet, read = false;

	*sw_det = false;
	*cleared = false;
	lck_mtx_lock(&jack_ngd_lock);
	quiet = wcd_page_quiet;
	wcd_page_quiet = true;
	ok = wcd_read_locked(0x0619, result3) && wcd_read_locked(0x0412, status1);
	if (ok) {
		*sw_det = (*status1 & 0x01) != 0;
		if (*sw_det) {
			/* wcd9xxx-irq.c:262-264: write CLEAR1 bit 0; no CLR_COMMIT on SLIMbus. */
			*cleared = wcd_write_locked(0x041A, 0x01, &back, &read);
		}
	}
	wcd_page_quiet = quiet;
	lck_mtx_unlock(&jack_ngd_lock);
	return ok;
}

/*
 * J12: poll the mechanical switch, debounce it in software and publish the state. Every register
 * read and write is one jack_ngd_lock transaction; the 20 ms wait is outside the lock.
 */
static void
jack_mbhc_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t began = now_ms(), next_alive = began + MBHC_ALIVE_MS;
	uint8_t result3 = 0, status1 = 0;
	int stable = -1;
	unsigned held = 0;
	bool failing = false;

	while (mbhc_live) {
		bool sw_det, cleared, ok;
		int level;

		delay_for_interval(MBHC_POLL_MS, NSEC_PER_MSEC);       /* one sleep per poll (not 20 wake-ups) */
		if (!mbhc_live) {
			break;
		}

		ok = mbhc_poll(&result3, &status1, &sw_det, &cleared);
		if (!ok) {
			if (!failing) {
				say("vayu-jack: MBHC: RESULT_3/STATUS1 do not answer; polling on\n");
				failing = true;
			}
		} else {
			if (failing) {
				say("vayu-jack: MBHC: the MBHC registers answer again\n");
				failing = false;
			}
			if (sw_det) {
				if (cleared) {
					mbhc_sw_det++;
					say("vayu-jack: MBHC: SW_DET latched, cleared (0x041A <- 0x01, %u)\n", mbhc_sw_det);
				} else {
					say("vayu-jack: MBHC: SW_DET clear 0x041A <- 0x01 did not go\n");
				}
			}
			if (!mbhc_live) {
				break;
			}

			level = (result3 & 0x10) ? 0 : 1;       /* H-A3-1: bit 4 = 1 -> no plug, 0 -> plug */
			if (level != stable) {
				stable = level;
				held = 1;
			} else if (held < MBHC_STABLE) {
				held++;
			}
			if (held >= MBHC_STABLE && level != jack_state_sysctl) {
				uint32_t ms = (uint32_t)(now_ms() - began);
				uint8_t mech = 0;

				if (wcd_read_raw(0x0614, &mech)) {
					/* wcd-mbhc-v2.c:944-948: flip the armed edge, insertion <-> removal. */
					(void)wcd_update(0x0614, 0x20, (uint8_t)((mech & 0x20) ? 0x00 : 0x20));
				} else {
					say("vayu-jack: MBHC: MECH 0x0614 cannot be read for the edge toggle\n");
				}
				jack_state_sysctl = level;
				jack_gen_sysctl++;
				if (level) {
					say("vayu-jack: MBHC: INSERTED at +%u ms (RESULT_3 %02x MECH %02x sw_det %u)\n", ms, result3,
					    mech, mbhc_sw_det);
				} else {
					say("vayu-jack: MBHC: REMOVED at +%u ms (RESULT_3 %02x MECH %02x sw_det %u)\n", ms, result3,
					    mech, mbhc_sw_det);
				}
			}
		}

		if (!mbhc_live) {
			break;
		}
		if (now_ms() >= next_alive) {
			say("vayu-jack: MBHC: alive, state %d, gen %u, RESULT_3 %02x, sw_det %u\n", jack_state_sysctl,
			    (unsigned)jack_gen_sysctl, result3, mbhc_sw_det);
			next_alive = now_ms() + MBHC_ALIVE_MS;
		}
	}
	jack_state_sysctl = -1;
	mbhc_done = true;
	thread_terminate(current_thread());
}

static bool
step_mbhc_start(void)
{
	thread_t thread;

	mbhc_done = false;
	mbhc_running = true;
	mbhc_live = true;
	wcd_step = 12;
	if (kernel_thread_start(jack_mbhc_thread, NULL, &thread) != KERN_SUCCESS) {
		mbhc_live = false;
		mbhc_running = false;
		mbhc_done = true;
		jack_state_sysctl = -1;
		return fail(12, "no kernel thread for the MBHC poll");
	}
	thread_set_thread_name(thread, "vayu-jack mbhc");
	thread_deallocate(thread);
	say("vayu-jack: J12: poll thread \"vayu-jack mbhc\" started (every %u ms)\n", (unsigned)MBHC_POLL_MS);
	return true;
}

/* J12's stop (and finish's first half): the thread, then the MBHC bias and clock. Idempotent. */
static void
mbhc_stop(void)
{
	mbhc_live = false;
	if (mbhc_running) {
		for (unsigned ms = 0; ms < 200 && !mbhc_done; ms++) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
		say("vayu-jack: J12: poll thread %s\n", mbhc_done ? "stopped" : "did not stop in 200 ms");
		mbhc_running = false;
	}
	jack_state_sysctl = -1;
	if (mbhc_armed) {
		(void)wcd_update(0x0615, 0x01, 0x00);
		(void)wcd_update(0x0720, 0x80, 0x00);
		mbhc_armed = false;
	}
}

/* ---- J8: leave it quiet -------------------------------------------------------------------------- */

static bool
step_quiet(struct vayu_qrtr *q, uint32_t node, uint32_t port)
{
	uint8_t power[7] = { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 };       /* INACTIVE = 1 */
	uint32_t result = 0, error = 0;
	bool have_result = false, quiet;

	quiet = wcd_write(0x0011, 0x04);
	if (!quiet) {
		say("vayu-jack: J8: 0x0011 <- 0x04 did not go\n");
	} else {
		say("vayu-jack: J8: 0x0011 <- 0x04 (vendor bring-down)\n");
	}
	if (qmi_call(8, q, node, port, QMI_POWER, power, sizeof(power), &result, &error, &have_result)) {
		if (have_result) {
			say("vayu-jack: J8: POWER INACTIVE: result %u error %u\n", result, error);
		}
	} else {
		say("vayu-jack: J8: POWER INACTIVE: no answer\n");
	}
	jack_release(q);
	say("vayu-jack: J8: our QRTR port is closed, the router detached\n");
	return quiet;
}

bool
vayu_jack_run(struct vayu_qrtr *q)
{
	uint32_t run = 0, node = 0, port = 0;
	uint8_t pgd_la = 0xff, ifd_la = 0xff;
	bool bound = false, powered = false;

	if (!PE_parse_boot_argn("vayu_jack", &run, sizeof(run)) || run == 0) {
		return false;
	}
	if (q == NULL) {
		return false;
	}
	jack_ready = false;
	jack_run = run;
	jack_mclk_on = false;
	jack_q = NULL;
	jack_powered = false;
	jack_state_sysctl = -1;
	jack_gen_sysctl = 0;
	audio_route_sysctl = 0;
	mbhc_live = false;
	mbhc_done = true;
	mbhc_running = false;
	mbhc_armed = false;
	mbhc_sw_det = 0;
	ngd_quiet = false;                      /* J0-J6 keep the whole bus trace */
	wcd_step = 6;
	jack_rx_head = jack_rx_tail = 0;
	wcd_page_written = -1;
	ngd_already_up = false;
	codec_ifd_la = 0xff;

	if (!step_pins()) {
		goto failed;
	}
	bound = true;                           /* step_qmi binds it, even if it then fails */
	if (!step_qmi(q, &node, &port)) {
		goto failed;
	}
	powered = true;
	jack_q = q;
	jack_node = node;
	jack_service_port = port;
	jack_powered = true;
	if (!step_ngd_map()) {
		goto failed;
	}
	step_ngd_up();
	if (!ngd_already_up) {
		if (!step_capability()) {
			goto failed;
		}
	} else {
		say("vayu-jack: J4: skipped: the framer is already up\n");
	}
	if (!step_addresses(&pgd_la, &ifd_la)) {
		goto failed;
	}
	codec_pgd_la = pgd_la;
	codec_ifd_la = ifd_la;
	ngd_quiet = true;                       /* J7 on: the routine TX/RX/INT_CLR lines are off */
	if (!step_codec()) {
		goto failed;
	}
	if (run >= 2) {
		if (!step_mclk_init()) {
			goto failed;
		}
		if (!step_ready()) {
			goto failed;
		}
		jack_ready = true;
		if (run >= 3) {
			if (!step_mbhc_arm()) {
				goto failed;
			}
			if (!step_mbhc_start()) {
				goto failed;
			}
			say("vayu-jack: done (vayu_jack=3: the codec and MCLK stay up, the MBHC polls the jack)\n");
		} else {
			say("vayu-jack: done (vayu_jack=2: the codec and MCLK stay up)\n");
		}
		return true;
	}
	if (!step_efuse(7)) {
		goto failed;
	}
	if (!step_quiet(q, node, port)) {
		bound = false;                  /* step_quiet unbound it */
		powered = false;
		jack_powered = false;
		jack_q = NULL;
		return fail(8, "the codec did not take the bring-down (0x0011 <- 0x04)");
	}
	bound = false;
	powered = false;
	jack_powered = false;
	jack_q = NULL;
	say("vayu-jack: done\n");
	return true;

failed:
	if (run >= 3) {
		/* J11/J12 may have armed the MBHC and started the poll: stop both before the MCLK goes. */
		mbhc_stop();
		jack_ready = false;
	}
	if (run >= 2 && jack_mclk_on) {
		/* do not leave the codec clocked and powered after a failed J9 */
		say("vayu-jack: J9: the failure leaves the codec up: MCLK off and 0x0011 <- 0x04\n");
		mclk_off();
		vayu_rpmh_write("lnbclka2", 4, 0);
		wcd_write(0x0011, 0x04);
		jack_mclk_on = false;
	}
	if (powered) {
		/* the DSP was asked for the bus: give it back (the NGD itself is left alone) */
		uint8_t power[7] = { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 };       /* INACTIVE = 1 */
		uint32_t result = 0, error = 0;
		bool have_result = false;

		if (qmi_call(8, q, node, port, QMI_POWER, power, sizeof(power), &result, &error, &have_result)) {
			if (have_result) {
				say("vayu-jack: J8: POWER INACTIVE after the failure: result %u error %u\n", result, error);
			} else {
				say("vayu-jack: J8: POWER INACTIVE after the failure: no result TLV\n");
			}
		} else {
			say("vayu-jack: J8: POWER INACTIVE after the failure: no answer\n");
		}
	}
	jack_powered = false;
	if (bound) {
		jack_release(q);
	}
	jack_q = NULL;
	return false;
}

bool
vayu_jack_ready(void)
{
	return jack_ready;
}

bool
vayu_jack_keep(void)
{
	return jack_run >= 3 && jack_ready;
}

int
vayu_jack_state(void)
{
	return jack_state_sysctl;
}

void
vayu_jack_set_route(int route)
{
	audio_route_sysctl = route == 1 ? 1 : 0;
}

/* ---- J2-B: the headphone path (P1-P11) ----------------------------------------------------------- */

static bool
path_fail(unsigned step, const char *format, ...) __printflike(2, 3);

static bool
path_fail(unsigned step, const char *format, ...)
{
	char line[240];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say("vayu-jack: P%u: FAILED: %s\n", step, line);
	return false;
}

static bool
path_write(unsigned step, uint16_t reg, uint8_t mask, uint8_t value)
{
	if (!wcd_update(reg, mask, value)) {
		return path_fail(step, "0x%04x m%02x v%02x did not go", reg, mask, value);
	}
	return true;
}

static bool
path_ifd_write(unsigned step, uint16_t reg, uint8_t value)
{
	if (!ifd_write(reg, value)) {
		return path_fail(step, "ifd 0x%04x <- 0x%02x did not go", reg, value);
	}
	return true;
}

/* P1: the HAL's "headphones" mixer (mixer_paths_tavil.xml:3001-3007, HPH_PATH.md section 1.2). */
static uint32_t rx_main_path;           /* boot-arg vayu_jack_rxpath=1: RX2/RX3 into the main path (INT1_1/INT2_1), a test */

static bool
path_p1(void)
{
	PE_parse_boot_argn("vayu_jack_rxpath", &rx_main_path, sizeof(rx_main_path));
	if (rx_main_path) {
		/*
		 * The test path: RX2/RX3 into the interpolators' MAIN inputs (RX INT1_1 MIX1 INP0 = RX2, RX INT2_1 MIX1 INP0
		 * = RX3: CDC_RX_INP_MUX_RX_INT1/2_CFG0 [3:0] = 3/4, wcd934x.c enum rx_int1_1_mix_inp0) at main volume -20 dB,
		 * the mix inputs left at ZERO. a test run/a test run: the HAL's mix path left the codec's RX ports 18/19 in overflow.
		 */
		if (!path_write(1, 0x0D03, 0x0f, 0x03) || !path_write(1, 0x0D05, 0x0f, 0x04) ||
		    !path_write(1, 0x0D04, 0x0f, 0x00) || !path_write(1, 0x0D06, 0x0f, 0x00) ||
		    !path_write(1, 0x0B59, 0xff, 0xEC) || !path_write(1, 0x0B6D, 0xff, 0xEC) ||
		    !path_write(1, 0x0B55, 0x0f, 0x04) || !path_write(1, 0x0B69, 0x0f, 0x04)) {
			return false;
		}
		say("vayu-jack: P1: TEST: RX2/RX3 into the MAIN path (INT1_1/INT2_1 INP0), main volume -20 dB\n");
	}
	/* HAL default "HPHL/HPHR Volume" 20 of 24, inverted (mixer_paths_tavil.xml:373-374; wcd934x.c:6407-6408) */
	if (!path_write(1, 0x06D3, 0x1f, 0x04) || !path_write(1, 0x06D6, 0x1f, 0x04)) {
		return false;
	}
	if ((!rx_main_path && (!path_write(1, 0x0D04, 0x0f, 0x03) || !path_write(1, 0x0D06, 0x0f, 0x04))) ||
	    !path_write(1, 0x0B5C, 0xff, 0xEC) || !path_write(1, 0x0B70, 0xff, 0xEC) ||
	    !path_write(1, 0x0B5D, 0x03, 0x01) || !path_write(1, 0x0B71, 0x03, 0x01) ||
	    !path_write(1, 0x0B56, 0x08, 0x08) || !path_write(1, 0x0B6A, 0x08, 0x08) ||
	    !path_write(1, 0x0B5A, 0x0f, 0x04) || !path_write(1, 0x0B6E, 0x0f, 0x04)) {
		return false;
	}
	say("vayu-jack: P1: mixer: RX INT1_2 = RX2, RX INT2_2 = RX3, mix -20 dB, DEM = CLSH_DSM_OUT, 48 kHz\n");
	return true;
}

/* P2: the RX bias (TAVIL_INIT.md section 6.3, wcd934x.c:2073-2095). */
static bool
path_p2(void)
{
	if (!path_write(2, 0x0608, 0x01, 0x01)) {
		return false;
	}
	say("vayu-jack: P2: RX bias on (0x0608 m01 v01)\n");
	return true;
}

/*
 * P3: the codec's two SLIM ports (wcd9xxx-slimslave.c:215-307, CHAN_AFE.md section 3.3): payload 0x0C,
 * watermark 5, then CONNECT_SINK of port 18 to channel 146 and port 19 to channel 147. The device
 * LA in the message is the codec's PGD LA (the vendor rewrites the port byte only for its own
 * PGD, slim-msm-ngd.c:717-733, which this is not).
 */
static bool
path_p3(void)
{
	uint8_t connect[7] = { 0x47, 0x2d, 0xff, 0, 0x12, 0x92, 0 };       /* L 7, MT 2, MC 0x2D */

	if (!path_ifd_write(3, 0x0188, 0x0c) || !path_ifd_write(3, 0x0042, 0x05)) {
		return false;
	}
	connect[3] = codec_pgd_la;
	if (!user_msg(connect, sizeof(connect), 6)) {
		return path_fail(3, "CONNECT_SINK port 18 channel 146 was not acked");
	}
	if (!path_ifd_write(3, 0x018C, 0x0c) || !path_ifd_write(3, 0x0043, 0x05)) {
		return false;
	}
	connect[3] = codec_pgd_la;
	connect[4] = 0x13;
	connect[5] = 0x93;
	if (!user_msg(connect, sizeof(connect), 6)) {
		return path_fail(3, "CONNECT_SINK port 19 channel 147 was not acked");
	}
	say("vayu-jack: P3: SLIM ports 18/19: payload 0x0C, watermark 5, channels 146/147 connected (LA %#x)\n",
	    codec_pgd_la);
	return true;
}

/*
 * P4: DEF_ACT_CHAN for 146/147 and RECONFIG_NOW (slim-msm-ngd.c:1118-1152, 1156-1162). Byte 3 is
 * the codec's PGD LA; 0x24 = 16 bits >> 2 | COEFF_3 << 5; 0x20 = rootexp 2 << 4 | HARD_ISO 0;
 * 0x83 = 48 kHz exact.
 */
static bool
path_p4(void)
{
	uint8_t activate[10] = { 0x4a, 0x21, 0xff, 0, 0x24, 0x20, 0x83, 0, 0x92, 0x93 };
	uint8_t reconfig[5] = { 0x45, 0x24, 0xff, 0, 0 };

	activate[3] = codec_pgd_la & 0x1f;
	if (!user_msg(activate, sizeof(activate), 7)) {
		return path_fail(4, "DEF_ACT_CHAN channels 146/147 was not acked");
	}
	reconfig[4] = codec_pgd_la;
	if (!user_msg(reconfig, sizeof(reconfig), 3)) {
		return path_fail(4, "RECONFIG_NOW was not acked");
	}
	say("vayu-jack: P4: channels 146/147 active, reconfig now (LA %#x)\n", codec_pgd_la);
	return true;
}

/* P5: the interpolators, left then right (HPH_PATH.md section 6 M20-M26; a 1.1 chip has no HD2 rows). */
static bool
path_p5(void)
{
	if (!path_write(5, 0x0B55, 0x10, 0x10) || !path_write(5, 0x0B55, 0x20, 0x20) ||
	    !path_write(5, 0x0C0F, 0x01, 0x00) || !path_write(5, 0x0D8B, 0x80, 0x80) ||
	    (!rx_main_path && !path_write(5, 0x0B5A, 0x20, 0x20)) ||
	    !path_write(5, 0x0B69, 0x10, 0x10) || !path_write(5, 0x0B69, 0x20, 0x20) ||
	    !path_write(5, 0x0C0F, 0x02, 0x00) || !path_write(5, 0x0D90, 0x80, 0x80) ||
	    (!rx_main_path && !path_write(5, 0x0B6E, 0x20, 0x20))) {
		return false;
	}
	say("vayu-jack: P5: interpolators: left and right clocks on (LUT bypass; no compander: vayu's COMP1/COMP2 Switch 0)\n");
	return true;
}

/*
 * P6: class-H HPHL from IDLE in LOHIFI (HPH_PATH.md section 8.2 rows 1-6, 8-12, 20, 23-25; the
 * TASHA and TAVIL_IS_1_0 rows are not for a WCD934x v1.1).
 */
static bool
path_p6(void)
{
	if (!path_write(6, 0x0C01, 0x01, 0x01) || !path_write(6, 0x0C08, 0x0f, 0x00) ||
	    !path_write(6, 0x0C09, 0xff, 0xc0) || !path_write(6, 0x0B56, 0x40, 0x40) ||
	    !path_write(6, 0x0608, 0x02, 0x00) || !path_write(6, 0x0608, 0x04, 0x04) ||
	    !path_write(6, 0x06A8, 0xf0, 0x80) || !path_write(6, 0x0738, 0x20, 0x20) ||
	    !path_write(6, 0x06BF, 0xf0, 0xc0) || !path_write(6, 0x06D1, 0x0e, 0x02) ||
	    !path_write(6, 0x0608, 0x40, 0x40)) {
		return false;
	}
	spin_us(110);                           /* 100-110 us after the flyback users' first turn */
	spin_us(550);                           /* 500-550 us after the flyback tail */
	if (!path_write(6, 0x0608, 0x08, 0x08) || !path_write(6, 0x0608, 0x80, 0x80)) {
		return false;
	}
	spin_us(550);                           /* 500-550 us after the buck users' first turn */
	if (!path_write(6, 0x0609, 0x0c, 0x04)) {
		return false;
	}
	say("vayu-jack: P6: class-H HPHL from IDLE, LOHIFI (0x0609 m0c v04)\n");
	return true;
}

/* P7: HPHL DAC (HPH_PATH.md section 6 M29-M33; the DEM must say CLSH_DSM_OUT first). */
static bool
path_p7(void)
{
	uint8_t dem;

	if (!wcd_read_raw(0x0B5D, &dem)) {
		return path_fail(7, "0x0B5D cannot be read");
	}
	if ((dem & 0x03) != 0x01) {
		return path_fail(7, "0x0B5D DEM is %02x, not CLSH_DSM_OUT (1)", dem);
	}
	if (!path_write(7, 0x071E, 0x01, 0x01) || !path_write(7, 0x073A, 0x02, 0x00) ||
	    !path_write(7, 0x0609, 0x20, 0x20)) {
		return false;
	}
	say("vayu-jack: P7: HPHL DAC: DEM %02x (CLSH_DSM_OUT), ripple on, AutoChop off, DAC on\n", dem);
	return true;
}

/* P8: class-H stereo and the HPHR DAC (HPH_PATH.md section 6 M26, M34-M35 and section 8.2 S1). */
static bool
path_p8(void)
{
	uint8_t dem;

	if (!path_write(8, 0x0B6A, 0x40, 0x40)) {
		return false;
	}
	if (!wcd_read_raw(0x0B71, &dem)) {
		return path_fail(8, "0x0B71 cannot be read");
	}
	if ((dem & 0x03) != 0x01) {
		return path_fail(8, "0x0B71 DEM is %02x, not CLSH_DSM_OUT (1)", dem);
	}
	if (!path_write(8, 0x0609, 0x10, 0x10)) {
		return false;
	}
	say("vayu-jack: P8: class-H stereo, HPHR DAC (DEM %02x, 0x0609 m10 v10)\n", dem);
	return true;
}

/* P9: both PAs: OCP off, the 7 ms settles, test/GM3/unmute, then OCP on (HPH_PATH.md section 6 M36-M45). */
static bool
path_p9(void)
{
	if (!path_write(9, 0x06B6, 0x0f, 0x0f) || !path_write(9, 0x06CE, 0x10, 0x00) ||
	    !path_write(9, 0x0609, 0x80, 0x80)) {
		return false;
	}
	delay_for_interval(7, NSEC_PER_MSEC);
	if (!path_write(9, 0x0609, 0x40, 0x40)) {
		return false;
	}
	delay_for_interval(7, NSEC_PER_MSEC);
	if (!path_write(9, 0x06D4, 0x01, 0x01) || !path_write(9, 0x06D7, 0x01, 0x01) ||
	    !path_write(9, 0x06CC, 0x80, 0x80) || !path_write(9, 0x073A, 0x02, 0x02) ||
	    !path_write(9, 0x0B55, 0x10, 0x00) || !path_write(9, 0x0B5A, 0x10, 0x00) ||
	    !path_write(9, 0x0B69, 0x10, 0x00) || !path_write(9, 0x0B6E, 0x10, 0x00) ||
	    !path_write(9, 0x06CE, 0x10, 0x10) || !path_write(9, 0x06B6, 0x0f, 0x02)) {
		return false;
	}
	say("vayu-jack: P9: PAs on: OCP off, 7 ms settles, unmuted, OCP on (0x0609 should read 0xf4)\n");
	return true;
}

/* The two proof lines (P10 and every proof tick); the caller holds jack_ngd_lock. */
static bool
proof_read_locked(const char *where)
{
	uint8_t hph = 0xff, lstat = 0xff, rstat = 0xff, supplies = 0xff, svs = 0xff;
	uint8_t lctl = 0xff, lmix = 0xff, rctl = 0xff, rmix = 0xff, lvol = 0xff, rvol = 0xff;
	uint8_t intr[4] = { 0xff, 0xff, 0xff, 0xff };
	uint8_t ports[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint16_t port_regs[8] = { 0x0034, 0x0035, 0x0062, 0x0063, 0x0188, 0x018c, 0x0042, 0x0043 };
	bool ok = true;

	if (!wcd_read_locked(0x0609, &hph) || !wcd_read_locked(0x06c9, &lstat) ||
	    !wcd_read_locked(0x06ca, &rstat) || !wcd_read_locked(0x0608, &supplies) || !wcd_read_locked(0x0202, &svs)) {
		ok = false;
	}
	if (!wcd_read_locked(0x0B55, &lctl) || !wcd_read_locked(0x0B5A, &lmix) ||
	    !wcd_read_locked(0x0B69, &rctl) || !wcd_read_locked(0x0B6E, &rmix) ||
	    !wcd_read_locked(0x0B5C, &lvol) || !wcd_read_locked(0x0B70, &rvol)) {
		ok = false;
	}
	for (unsigned i = 0; i < sizeof(intr) / sizeof(intr[0]); i++) {
		if (!wcd_read_locked((uint16_t)(0x0411 + i), &intr[i])) {
			ok = false;
		}
	}
	say("vayu-jack: %s: HPH 0x0609=%02x L_STATUS 0x06c9=%02x R_STATUS 0x06ca=%02x 0x0608=%02x "
	    "0x0B55=%02x 0x0B5A=%02x 0x0B69=%02x 0x0B6E=%02x 0x0B5C=%02x 0x0B70=%02x "
	    "INTR 0x0411-0x0414=%02x %02x %02x %02x SVS 0x0202=%02x\n", where, hph, lstat, rstat, supplies, lctl, lmix, rctl, rmix,
	    lvol, rvol, intr[0], intr[1], intr[2], intr[3], svs);

	for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
		if (!ifd_read_locked(port_regs[i], &ports[i])) {
			ok = false;
		}
	}
	say("vayu-jack: %s: ifd 0x34=%02x 0x35=%02x 0x62=%02x 0x63=%02x 0x188=%02x 0x18C=%02x 0x42=%02x 0x43=%02x\n",
	    where, ports[0], ports[1], ports[2], ports[3], ports[4], ports[5], ports[6], ports[7]);
	return ok;
}

static bool
path_proof(const char *where)
{
	bool ok, quiet;

	lck_mtx_lock(&jack_ngd_lock);
	quiet = wcd_page_quiet;
	wcd_page_quiet = true;
	ok = proof_read_locked(where);
	wcd_page_quiet = quiet;
	lck_mtx_unlock(&jack_ngd_lock);
	return ok;
}

/* P10: the readback after the path is up; 0x0609 should be 0xf4 (HPH on). */
static bool
path_p10(void)
{
	if (!path_proof("P10")) {
		return path_fail(10, "the proof readback did not answer");
	}
	return true;
}

/*
 * P11: every 500 ms the same two lines, then clear the RX_0 port-interrupt bits so the next line
 * shows only new over/underflows (wcd934x.c:102-103, 9765-9795, 9871).
 */
static void
jack_proof_thread(void *arg __unused, wait_result_t wr __unused)
{
	while (path_live) {
		char where[32];
		uint32_t ms;

		for (unsigned waited = 0; waited < 500 && path_live; waited += 50) {
			delay_for_interval(50, NSEC_PER_MSEC);
		}
		if (!path_live) {
			break;
		}
		ms = (uint32_t)(now_ms() - proof_began);
		snprintf(where, sizeof(where), "proof +%u ms", ms);
		(void)path_proof(where);
		(void)ifd_write(0x0038, 0x0c);
	}
	proof_done = true;
	thread_terminate(current_thread());
}

static bool
path_p11(void)
{
	thread_t thread;

	proof_done = false;
	proof_began = now_ms();
	proof_running = true;
	path_live = true;
	if (kernel_thread_start(jack_proof_thread, NULL, &thread) != KERN_SUCCESS) {
		path_live = false;
		proof_running = false;
		proof_done = true;
		return path_fail(11, "no kernel thread for the proof");
	}
	thread_set_thread_name(thread, "vayu-jack proof");
	thread_deallocate(thread);
	say("vayu-jack: P11: proof thread \"vayu-jack proof\" started (every 500 ms)\n");
	return true;
}

/* The path, in order; a failure is logged by the step and takes everything down. */
bool
vayu_jack_path_on(void)
{
	if (!jack_ready) {
		return false;
	}
	if (path_live) {
		return true;
	}
	wcd_step = 10;

	if (!path_p1() || !path_p2() || !path_p3() || !path_p4() || !path_p5() || !path_p6() || !path_p7() ||
	    !path_p8() || !path_p9() || !path_p10() || !path_p11()) {
		vayu_jack_path_off();
		return false;
	}
	return true;
}

/*
 * J2-B: the path's idempotent teardown, in reverse (HPH_PATH.md section 6 P1-P9 and section 8.2 D1-D12). Every
 * write is logged with its readback by the helpers; an error does not stop the rest.
 */
void
vayu_jack_path_off(void)
{
	uint8_t disconnect[6] = { 0x46, 0x2e, 0xff, 0, 0x12, 0 };         /* L 6, MT 2, MC 0x2E */
	uint8_t remove[7] = { 0x47, 0x23, 0xff, 0, 0, 0x92, 0x93 };        /* L 7, MT 2, MC 0x23 */
	uint8_t reconfig[5] = { 0x45, 0x24, 0xff, 0, 0 };                  /* L 5, MT 2, MC 0x24 */

	if (!jack_ready) {
		return;
	}
	path_live = false;
	if (proof_running) {
		for (unsigned ms = 0; ms < 1000 && !proof_done; ms++) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
		say("vayu-jack: OFF: proof thread %s\n", proof_done ? "stopped" : "did not stop in 1000 ms");
		proof_running = false;
	}

	(void)wcd_update(0x0B55, 0x10, 0x10);
	(void)wcd_update(0x0B5A, 0x10, 0x10);
	(void)wcd_update(0x0B69, 0x10, 0x10);
	(void)wcd_update(0x0B6E, 0x10, 0x10);
	say("vayu-jack: OFF: outputs muted\n");

	(void)wcd_update(0x06D4, 0x01, 0x00);
	(void)wcd_update(0x06D7, 0x01, 0x00);
	say("vayu-jack: OFF: PA test mode off\n");

	(void)wcd_update(0x0609, 0x40, 0x00);
	(void)wcd_update(0x0609, 0x80, 0x00);
	delay_for_interval(5, NSEC_PER_MSEC);   /* 5 ms after the PAs */
	say("vayu-jack: OFF: PAs off, 5 ms\n");

	(void)wcd_update(0x0609, 0x30, 0x00);
	spin_us(1100);                          /* 1000-1100 us after the DACs */
	say("vayu-jack: OFF: DACs off\n");
	if (rx_main_path) {
		(void)wcd_update(0x0D03, 0x0f, 0x00);   /* the test's main-path inputs back to ZERO */
		(void)wcd_update(0x0D05, 0x0f, 0x00);
	}

	/* D1-D12: class-H back to IDLE (the TASHA rows are not for this chip). */
	(void)wcd_update(0x0B6A, 0x40, 0x00);
	(void)wcd_update(0x0609, 0x0c, 0x00);
	(void)wcd_update(0x0B56, 0x40, 0x00);
	(void)wcd_update(0x0C01, 0x01, 0x00);
	(void)wcd_update(0x0608, 0x80, 0x00);
	spin_us(550);                           /* 500-550 us */
	(void)wcd_update(0x0608, 0x40, 0x00);
	spin_us(110);                           /* 100-110 us */
	spin_us(550);                           /* then 500-550 us */
	(void)wcd_update(0x0738, 0x20, 0x00);
	(void)wcd_update(0x06BF, 0xf0, 0x80);
	(void)wcd_update(0x06D1, 0x0e, 0x06);
	(void)wcd_update(0x06CC, 0x80, 0x80);
	(void)wcd_update(0x06A8, 0xf0, 0x70);
	(void)wcd_update(0x0608, 0x04, 0x00);
	(void)wcd_update(0x0608, 0x08, 0x00);
	(void)wcd_update(0x0608, 0x02, 0x00);
	say("vayu-jack: OFF: class-H back to IDLE (D1-D12)\n");

	(void)wcd_update(0x071E, 0x01, 0x00);
	(void)wcd_update(0x0B5A, 0x20, 0x00);
	(void)wcd_update(0x0B6E, 0x20, 0x00);
	(void)wcd_update(0x0B55, 0x20, 0x00);
	(void)wcd_update(0x0B69, 0x20, 0x00);
	(void)wcd_update(0x0608, 0x01, 0x00);
	say("vayu-jack: OFF: ripple off, mix/interpolator clocks off, RX bias off\n");

	disconnect[3] = codec_pgd_la;
	(void)user_msg(disconnect, sizeof(disconnect), 5);
	disconnect[4] = 0x13;
	(void)user_msg(disconnect, sizeof(disconnect), 5);
	remove[3] = (uint8_t)(0x80 | (codec_pgd_la & 0x1f));       /* SLIM_CH_REMOVE = 2, slimbus.h:359-361 */
	(void)user_msg(remove, sizeof(remove), 4);
	reconfig[4] = codec_pgd_la;
	(void)user_msg(reconfig, sizeof(reconfig), 3);
	say("vayu-jack: OFF: SLIM ports 18/19 disconnected, channels 146/147 removed\n");
	path_live = false;
}

/* ---- M1-M9: the handset microphones (AMIC3 -> DEC7 -> SLIM TX7, AMIC1 -> DEC8 -> SLIM TX8) -------- */

#define MIC_ADC_GAIN_DEFAULT    5       /* the HAL's "ADC3/ADC1 Volume" 5 = 1.25 dB (wcd934x.c:6417-6419) */
#define MIC_BIAS_2700_MV        0x22    /* (2700 - 1000) / 50 (wcd934x.c:9942-9952) */

/* vayu_mic_gain=N (0..20, default 5): the ADC volume written to ADC3 and to ADC1. */
static uint8_t
mic_gain(void)
{
	uint32_t gain;

	if (!PE_parse_boot_argn("vayu_mic_gain", &gain, sizeof(gain))) {
		return MIC_ADC_GAIN_DEFAULT;
	}
	return (uint8_t)(gain > 20 ? 20 : gain);
}

/* vayu_mic_bias=0: M3 is skipped (the test that expects near silence without a microphone bias). */
static bool
mic_bias_wanted(void)
{
	uint32_t bias;

	return !PE_parse_boot_argn("vayu_mic_bias", &bias, sizeof(bias)) || bias != 0;
}

/* AMIC_3_4/AMIC_1_2 PWR MODE, bits [6:5] (wcd934x.c:119-124), as the vendor names it. */
static const char *
mic_pwr_name(uint8_t adc)
{
	switch ((adc >> 5) & 0x03) {
	case 0: return "LP";
	case 2: return "HP";
	case 3: return "HYBRID";
	default: return "DEFAULT";
	}
}

/* The same bits as the decimator's power level (wcd934x.c:4469-4493). */
static uint8_t
mic_pwr_level(uint8_t adc)
{
	switch ((adc >> 5) & 0x03) {
	case 0: return 0x02;            /* LP -> DEC_PWR_LVL_LP */
	case 2: return 0x04;            /* HP -> DEC_PWR_LVL_HP */
	default: return 0x00;           /* DEFAULT/HYBRID -> DEC_PWR_LVL_DF */
	}
}

/* M1: the HAL's muxes and gains (mixer_paths_overlay_static.xml:801-813, MIC_TX 1.1). */
static bool
mic_mux_gain(uint8_t gain)
{
	if (!wcd_update(0x0d3b, 0xc0, 0x80) ||        /* CDC_IF TX7 MUX = DEC7 */
	    !wcd_update(0x0d3c, 0x03, 0x02) ||        /* CDC_IF TX8 MUX = DEC8 */
	    !wcd_update(0x0d25, 0x0c, 0x04) ||        /* ADC MUX7 = AMIC */
	    !wcd_update(0x0aa2, 0x80, 0x00) ||        /* TX7_PATH_CFG0 bit 7: the ADC input */
	    !wcd_update(0x0d1e, 0x30, 0x10) ||        /* ADC MUX8 = AMIC */
	    !wcd_update(0x0ab2, 0x80, 0x00) ||        /* TX8_PATH_CFG0 bit 7: the ADC input */
	    !wcd_update(0x0d29, 0x07, 0x03) ||        /* AMIC MUX7 = ADC3 */
	    !wcd_update(0x0d2a, 0x07, 0x01) ||        /* AMIC MUX8 = ADC1 */
	    !wcd_update(0x0610, 0x1f, gain) ||        /* ADC3 Volume */
	    !wcd_update(0x060e, 0x1f, gain)) {        /* ADC1 Volume */
		return false;
	}
	say("vayu-jack: M1: muxes and gains: CDC_IF TX7=DEC7 TX8=DEC8, ADC MUX7/8=AMIC, AMIC MUX7=ADC3 "
	    "MUX8=ADC1, ADC3/ADC1 volume %u\n", gain);
	return true;
}

/* M2: the mic bias voltage, 2700 mV on MB3 and MB1 (wcd934x.c:9942-9982). */
static bool
mic_bias_voltage(void)
{
	if (!wcd_update(0x0625, 0x3f, MIC_BIAS_2700_MV) || !wcd_update(0x0622, 0x3f, MIC_BIAS_2700_MV)) {
		return false;
	}
	say("vayu-jack: M2: mic bias voltage 2700 mV on MB3 and MB1 (0x0625/0x0622 m3F v%02x)\n",
	    MIC_BIAS_2700_MV);
	return true;
}

/* M3: both biases on (tavil_micbias_control, wcd934x.c:4928), then the vendor's cnp wait. */
static bool
mic_bias_on(void)
{
	if (!wcd_update(0x0625, 0xc0, 0x40) || !wcd_update(0x0622, 0xc0, 0x40)) {
		return false;
	}
	spin_us(1100);                          /* POST_PMU "wait for cnp" (wcd934x.c:5012-5015) */
	say("vayu-jack: M3: mic biases 3 and 1 on (0x0625/0x0622 mC0 v40), waited 1100 us for cnp\n");
	return true;
}

/* M4: ADC3/ADC1 on, each with its TX hold (wcd934x.c:4240-4265, 4666-4679, 7974-7979). */
static bool
mic_adc_on(void)
{
	if (!wcd_update(0x0610, 0x80, 0x80) || !wcd_update(0x0611, 0x40, 0x40) ||
	    !wcd_update(0x060e, 0x80, 0x80) || !wcd_update(0x060f, 0x40, 0x40)) {
		return false;
	}
	say("vayu-jack: M4: ADC3 and ADC1 on, TX holds set (0x0610/0x060E m80 v80, 0x0611/0x060F m40 v40)\n");
	return true;
}

/* M5: the decimators' PRE_PMU (wcd934x.c:4409-4496): power level, mute, 48 kHz. */
static bool
mic_dec_pre(void)
{
	uint8_t adc3, adc1, pwr7, pwr8;

	if (!wcd_read_raw(0x0610, &adc3) || !wcd_read_raw(0x060e, &adc1)) {
		say("vayu-jack: M5: the AMIC power modes cannot be read\n");
		return false;
	}
	pwr7 = mic_pwr_level(adc3);
	pwr8 = mic_pwr_level(adc1);
	if (!wcd_update(0x0aa2, 0x06, pwr7) || !wcd_update(0x0ab2, 0x06, pwr8) ||
	    !wcd_update(0x0aa1, 0x10, 0x10) || !wcd_update(0x0ab1, 0x10, 0x10) ||
	    !wcd_update(0x0aa1, 0x0f, 0x04) || !wcd_update(0x0ab1, 0x0f, 0x04)) {
		return false;
	}
	/*
	 * The DAPM power bit of the "ADC MUX7"/"ADC MUX8" widgets is TX7/TX8_TX_PATH_CTL bit 5, the decimator's
	 * clock (SND_SOC_DAPM_MUX_E("ADC MUX7", WCD934X_CDC_TX7_TX_PATH_CTL, 5, ...), wcd934x.c:7924-7931): DAPM
	 * sets it between PRE_PMU and POST_PMU. Missing in mic1 (a test run): the DSP read exact zeros.
	 */
	if (!wcd_update(0x0aa1, 0x20, 0x20) || !wcd_update(0x0ab1, 0x20, 0x20)) {
		return false;
	}
	say("vayu-jack: M5: DEC7 %s (0x0610=%02x) -> 0x0AA2 m06 v%02x, DEC8 %s (0x060E=%02x) -> "
	    "0x0AB2 m06 v%02x; both muted, 48 kHz\n", mic_pwr_name(adc3), adc3, pwr7, mic_pwr_name(adc1),
	    adc1, pwr8);
	return true;
}

/*
 * M6: the decimators' POST_PMU (wcd934x.c:4500-4527): keep the HPF each one had, select the 150 Hz
 * corner, pulse GO, and write the digital gain back.
 */
static bool
mic_dec_post(uint8_t *hpf7, uint8_t *hpf8)
{
	uint8_t cfg, gain;

	if (!wcd_read_raw(0x0aa2, &cfg)) {
		say("vayu-jack: M6: TX7_PATH_CFG0 cannot be read\n");
		return false;
	}
	*hpf7 = (uint8_t)((cfg & 0x60) >> 5);
	if (!wcd_read_raw(0x0ab2, &cfg)) {
		say("vayu-jack: M6: TX8_PATH_CFG0 cannot be read\n");
		return false;
	}
	*hpf8 = (uint8_t)((cfg & 0x60) >> 5);
	if (!wcd_update(0x0aa2, 0x60, 0x40) || !wcd_update(0x0ab2, 0x60, 0x40) ||
	    !wcd_update(0x0aa9, 0x02, 0x02) || !wcd_update(0x0ab9, 0x02, 0x02)) {
		return false;
	}
	spin_us(1010);                          /* "minimum 1 clk cycle", the vendor's 1000-1010 us */
	if (!wcd_update(0x0aa9, 0x02, 0x00) || !wcd_update(0x0ab9, 0x02, 0x00)) {
		return false;
	}
	if (!wcd_read_raw(0x0aa4, &gain) || !wcd_write(0x0aa4, gain)) {
		say("vayu-jack: M6: the TX7 digital gain cannot be re-applied\n");
		return false;
	}
	if (!wcd_read_raw(0x0ab4, &gain) || !wcd_write(0x0ab4, gain)) {
		say("vayu-jack: M6: the TX8 digital gain cannot be re-applied\n");
		return false;
	}
	say("vayu-jack: M6: 150 Hz corner (0x0AA2/0x0AB2 m60 v40) and GO pulses, digital gain re-applied; "
	    "saved hpf7=%u hpf8=%u\n", *hpf7, *hpf8);
	return true;
}

/* M7: the SLIM TX ports and channels (wcd9xxx-slimslave.c:349-456, wcd934x.c:1486-1528). */
static bool
mic_slim_tx(void)
{
	uint8_t v32, v33;
	uint8_t connect7[7] = { 0x47, 0x2c, 0xff, 0, 0x07, 0x87, 0 };       /* CONNECT_SRC, 7 bytes, tid 6 */
	uint8_t connect8[7] = { 0x47, 0x2c, 0xff, 0, 0x08, 0x88, 0 };
	uint8_t def_act[10] = { 0x4a, 0x21, 0xff, 0, 0x24, 0x20, 0x83, 0, 0x87, 0x88 };  /* tid 7 */
	uint8_t reconfig[5] = { 0x45, 0x24, 0xff, 0, 0 };                   /* RECONFIG_NOW, tid 3 */

	if (!ifd_read(0x32, &v32) || !ifd_write(0x32, (uint8_t)(v32 | 0x80)) ||
	    !ifd_read(0x33, &v33) || !ifd_write(0x33, (uint8_t)(v33 | 0x01))) {
		return false;
	}
	say("vayu-jack: M7: port interrupts: IFD 0x32 %02x -> %02x (port 7), 0x33 %02x -> %02x (port 8)\n",
	    v32, v32 | 0x80, v33, v33 | 0x01);

	if (!ifd_write(0x1f7, 0x02) || !ifd_write(0x11c, 0x80) || !ifd_write(0x11d, 0x01) ||
	    !ifd_write(0x57, 0x05)) {
		return false;
	}
	connect7[3] = codec_pgd_la;
	if (!user_msg(connect7, sizeof(connect7), 6)) {
		return false;
	}
	say("vayu-jack: M7: TX7 IFD 0x1F7=02 0x11C=80 0x11D=01 0x57=05, CONNECT_SRC port 7 channel 135 "
	    "acked\n");

	if (!ifd_write(0x1f8, 0x02) || !ifd_write(0x120, 0x80) || !ifd_write(0x121, 0x01) ||
	    !ifd_write(0x58, 0x05)) {
		return false;
	}
	connect8[3] = codec_pgd_la;
	if (!user_msg(connect8, sizeof(connect8), 6)) {
		return false;
	}
	say("vayu-jack: M7: TX8 IFD 0x1F8=02 0x120=80 0x121=01 0x58=05, CONNECT_SRC port 8 channel 136 "
	    "acked\n");

	def_act[3] = (uint8_t)(codec_pgd_la & 0x1f);
	if (!user_msg(def_act, sizeof(def_act), 7)) {
		return false;
	}
	reconfig[4] = codec_pgd_la;
	if (!user_msg(reconfig, sizeof(reconfig), 3)) {
		return false;
	}
	say("vayu-jack: M7: DEF_ACT_CHAN 135/136 and RECONFIG_NOW acked\n");
	return true;
}

/*
 * M8: the vendor's two delayed works done inline (tx_unmute_delay 40 ms, wcd934x.c:494-497 and
 * 4361-4378; the HPF work at 300 ms, wcd934x.c:4326-4358). The DSP stream is already running.
 */
static bool
mic_settle(uint8_t hpf7, uint8_t hpf8, uint64_t m6_ms)
{
	uint64_t clear_at = m6_ms + 300;

	delay_for_interval(40, NSEC_PER_MSEC);
	if (!wcd_update(0x0aa1, 0x10, 0x00) || !wcd_update(0x0ab1, 0x10, 0x00)) {
		return false;
	}
	say("vayu-jack: M8: unmuted 40 ms after the decimators (0x0AA1/0x0AB1 m10 v00)\n");
	while (now_ms() < clear_at) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (!wcd_update(0x0611, 0x40, 0x00) || !wcd_update(0x060f, 0x40, 0x00)) {
		return false;
	}
	say("vayu-jack: M8: 300 ms after M6: TX holds cleared (0x0611/0x060F m40 v00)\n");
	if (hpf7 != 2) {
		if (!wcd_update(0x0aa2, 0x60, (uint8_t)(hpf7 << 5)) || !wcd_update(0x0aa9, 0x02, 0x02)) {
			return false;
		}
		spin_us(1010);
		if (!wcd_update(0x0aa9, 0x02, 0x00)) {
			return false;
		}
		say("vayu-jack: M8: DEC7 hpf7 %u restored (0x0AA2 m60 v%02x), GO pulse\n", hpf7, hpf7 << 5);
	} else {
		say("vayu-jack: M8: DEC7 hpf7 2: 150 Hz kept\n");
	}
	if (hpf8 != 2) {
		if (!wcd_update(0x0ab2, 0x60, (uint8_t)(hpf8 << 5)) || !wcd_update(0x0ab9, 0x02, 0x02)) {
			return false;
		}
		spin_us(1010);
		if (!wcd_update(0x0ab9, 0x02, 0x00)) {
			return false;
		}
		say("vayu-jack: M8: DEC8 hpf8 %u restored (0x0AB2 m60 v%02x), GO pulse\n", hpf8, hpf8 << 5);
	} else {
		say("vayu-jack: M8: DEC8 hpf8 2: 150 Hz kept\n");
	}
	return true;
}

/* M9: the proof, the two lines the capture log is read by (wcd934x.c:9765-9795 for the IFD status). */
static bool
mic_proof(void)
{
	uint8_t micb3, micb1, adc3, adc1, tx7_ctl, tx7_cfg, tx8_ctl, tx8_cfg;
	uint8_t mux_tx7, mux_tx8, mux_adc7, mux_adc8, mux_amic7, mux_amic8;
	uint8_t status0, status1, source7, source8, mc0_7, mc1_7, wm7, wm8;

	if (!wcd_read_raw(0x0625, &micb3) || !wcd_read_raw(0x0622, &micb1) ||
	    !wcd_read_raw(0x0610, &adc3) || !wcd_read_raw(0x060e, &adc1) ||
	    !wcd_read_raw(0x0aa1, &tx7_ctl) || !wcd_read_raw(0x0aa2, &tx7_cfg) ||
	    !wcd_read_raw(0x0ab1, &tx8_ctl) || !wcd_read_raw(0x0ab2, &tx8_cfg) ||
	    !wcd_read_raw(0x0d3b, &mux_tx7) || !wcd_read_raw(0x0d3c, &mux_tx8) ||
	    !wcd_read_raw(0x0d25, &mux_adc7) || !wcd_read_raw(0x0d1e, &mux_adc8) ||
	    !wcd_read_raw(0x0d29, &mux_amic7) || !wcd_read_raw(0x0d2a, &mux_amic8)) {
		say("vayu-jack: M9: the codec registers cannot be read back\n");
		return false;
	}
	say("vayu-jack: M9: MICB3 0x0625=%02x MICB1 0x0622=%02x ADC3 0x0610=%02x ADC1 0x060E=%02x "
	    "TX7 ctl 0x0AA1=%02x cfg0 0x0AA2=%02x TX8 ctl 0x0AB1=%02x cfg0 0x0AB2=%02x mux 0x0D3B=%02x "
	    "0x0D3C=%02x 0x0D25=%02x 0x0D1E=%02x 0x0D29=%02x 0x0D2A=%02x\n", micb3, micb1, adc3, adc1,
	    tx7_ctl, tx7_cfg, tx8_ctl, tx8_cfg, mux_tx7, mux_tx8, mux_adc7, mux_adc8, mux_amic7, mux_amic8);
	if (!ifd_read(0x36, &status0) || !ifd_read(0x37, &status1) || !ifd_read(0x77, &source7) ||
	    !ifd_read(0x78, &source8) || !ifd_read(0x11c, &mc0_7) || !ifd_read(0x11d, &mc1_7) ||
	    !ifd_read(0x57, &wm7) || !ifd_read(0x58, &wm8)) {
		say("vayu-jack: M9: the IFD registers cannot be read back\n");
		return false;
	}
	say("vayu-jack: M9: ifd 0x36=%02x 0x37=%02x 0x77=%02x 0x78=%02x 0x11C=%02x 0x11D=%02x 0x57=%02x "
	    "0x58=%02x\n", status0, status1, source7, source8, mc0_7, mc1_7, wm7, wm8);
	return true;
}

/* Any mic_on step's failure: one line, then the whole path down. */
static bool
mic_failed(unsigned step, const char *what)
{
	say("vayu-jack: M%u: %s did not go; the microphone path is taken down\n", step, what);
	vayu_jack_mic_off();
	return false;
}

bool
vayu_jack_mic_on(void)
{
	uint8_t gain, hpf7 = 0, hpf8 = 0;
	uint64_t m6_ms;

	if (!jack_ready) {
		say("vayu-jack: M1: no: the codec is not ready (vayu_jack=2 and J9)\n");
		return false;
	}
	if (mic_live) {
		say("vayu-jack: M1: the microphone path is already up\n");
		return true;
	}
	gain = mic_gain();
	if (!mic_mux_gain(gain)) {
		return mic_failed(1, "the muxes and gains");
	}
	if (!mic_bias_voltage()) {
		return mic_failed(2, "the mic bias voltage");
	}
	if (!mic_bias_wanted()) {
		say("vayu-jack: M3: skipped (vayu_mic_bias=0): no mic bias, the capture is expected near silent\n");
	} else if (!mic_bias_on()) {
		return mic_failed(3, "the mic biases");
	}
	if (!mic_adc_on()) {
		return mic_failed(4, "the ADCs");
	}
	if (!mic_dec_pre()) {
		return mic_failed(5, "the decimator power-up");
	}
	if (!mic_dec_post(&hpf7, &hpf8)) {
		return mic_failed(6, "the decimator POST_PMU");
	}
	m6_ms = now_ms();
	if (!mic_slim_tx()) {
		return mic_failed(7, "the SLIM TX ports and channels");
	}
	if (!mic_settle(hpf7, hpf8, m6_ms)) {
		return mic_failed(8, "the unmute and settle");
	}
	if (!mic_proof()) {
		return mic_failed(9, "the proof readback");
	}
	mic_live = true;
	return true;
}

/* Idempotent; every step is logged and a failure does not stop the rest of the teardown. */
void
vayu_jack_mic_off(void)
{
	uint8_t chan_remove[7] = { 0x47, 0x23, 0xff, 0, 0, 0x87, 0x88 };       /* CHAN_CTRL, tid 4 */
	uint8_t reconfig[5] = { 0x45, 0x24, 0xff, 0, 0 };                       /* RECONFIG_NOW, tid 3 */

	if (!jack_ready) {
		say("vayu-jack: M-off: no: the codec is not ready\n");
		return;
	}
	if (wcd_update(0x0aa1, 0x10, 0x10) && wcd_update(0x0ab1, 0x10, 0x10)) {
		say("vayu-jack: M-off: TX7/TX8 muted (0x0AA1/0x0AB1 m10 v10)\n");
	} else {
		say("vayu-jack: M-off: the TX mutes did not go\n");
	}
	(void)wcd_update(0x0aa1, 0x20, 0x00);   /* the widgets' power bit (decimator clock) off, as DAPM after PRE_PMD */
	(void)wcd_update(0x0ab1, 0x20, 0x00);
	if (wcd_update(0x0aa1, 0x10, 0x00) && wcd_update(0x0aa2, 0x06, 0x00) &&
	    wcd_update(0x0ab1, 0x10, 0x00) && wcd_update(0x0ab2, 0x06, 0x00)) {
		say("vayu-jack: M-off: decimators off (0x0AA1/0x0AB1 m10 v00, 0x0AA2/0x0AB2 m06 v00)\n");
	} else {
		say("vayu-jack: M-off: the decimators did not go off\n");
	}
	if (wcd_update(0x0611, 0x40, 0x00) && wcd_update(0x060f, 0x40, 0x00)) {
		say("vayu-jack: M-off: TX holds cleared (0x0611/0x060F m40 v00)\n");
	} else {
		say("vayu-jack: M-off: the TX holds were not cleared\n");
	}
	if (wcd_update(0x0610, 0x80, 0x00) && wcd_update(0x060e, 0x80, 0x00)) {
		say("vayu-jack: M-off: ADC3/ADC1 off (0x0610/0x060E m80 v00)\n");
	} else {
		say("vayu-jack: M-off: the ADCs did not go off\n");
	}
	if (wcd_update(0x0625, 0xc0, 0x00) && wcd_update(0x0622, 0xc0, 0x00)) {
		say("vayu-jack: M-off: mic biases 3 and 1 off (0x0625/0x0622 mC0 v00)\n");
	} else {
		say("vayu-jack: M-off: the mic biases did not go off\n");
	}
	chan_remove[3] = (uint8_t)(0x80 | (codec_pgd_la & 0x1f));
	if (user_msg(chan_remove, sizeof(chan_remove), 4)) {
		say("vayu-jack: M-off: CHAN_CTRL remove for channels 135/136 acked\n");
	} else {
		say("vayu-jack: M-off: CHAN_CTRL remove for channels 135/136 was not acked\n");
	}
	reconfig[4] = codec_pgd_la;
	if (user_msg(reconfig, sizeof(reconfig), 3)) {
		say("vayu-jack: M-off: RECONFIG_NOW acked\n");
	} else {
		say("vayu-jack: M-off: RECONFIG_NOW was not acked\n");
	}
	mic_live = false;
	say("vayu-jack: M-off: the microphone path is down\n");
}

/*
 * Part 3: at the end of vayu_audio's run, before the DSP is stopped. Idempotent; only acts when
 * vayu_jack=2/3 got as far as J10 (vayu_jack=3: the poll thread and the MBHC bias/clock first).
 */
void
vayu_jack_finish(void)
{
	uint8_t power[7] = { 0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 };       /* INACTIVE = 1 */
	uint32_t result = 0, error = 0;
	bool have_result = false;

	if (!jack_ready) {
		return;
	}
	mbhc_stop();                            /* J12: the poll thread, then the MBHC bias and clock */
	if (path_live) {
		vayu_jack_path_off();           /* J2-B: idempotent */
	}
	if (mic_live) {
		vayu_jack_mic_off();            /* the microphones go down before the MCLK */
	}
	mclk_off();
	jack_mclk_on = false;
	if (!vayu_rpmh_write("lnbclka2", 4, 0)) {
		say("vayu-jack: finish: LN_BB_CLK2 did not go off\n");
	}
	if (!wcd_write(0x0011, 0x04)) {
		say("vayu-jack: finish: 0x0011 <- 0x04 did not go\n");
	} else {
		say("vayu-jack: finish: MCLK off, 0x0011 <- 0x04 (vendor bring-down)\n");
	}
	if (jack_powered) {
		if (qmi_call(8, jack_q, jack_node, jack_service_port, QMI_POWER, power, sizeof(power), &result, &error,
		    &have_result)) {
			if (have_result) {
				say("vayu-jack: J8: POWER INACTIVE: result %u error %u\n", result, error);
			} else {
				say("vayu-jack: J8: POWER INACTIVE: no result TLV\n");
			}
		} else {
			say("vayu-jack: J8: POWER INACTIVE: no answer\n");
		}
		jack_powered = false;
	}
	if (jack_q != NULL) {
		jack_release(jack_q);
		say("vayu-jack: finish: our QRTR port is closed, the router detached\n");
	}
	jack_q = NULL;
	jack_ready = false;
	say("vayu-jack: finished\n");
}
