/*
 * The modem of the POCO X3 Pro (vayu, SM8150-AC): a probe. Nothing here is touched unless
 * boot-arg vayu_modem=N asks for it (N >= 1).
 *
 * Wi-Fi's firmware service (QMI WLFW, service 0x45) runs on the modem processor (MPSS), so the
 * first Wi-Fi step is: load the modem image exactly as vayu_audio.c loads the audio DSP, start
 * it, open GLINK's IPCRTR channel, and print the QRTR services the modem announces. This is a
 * probe: with no boot-arg but vayu_modem, no QMI request is sent, no QRTR service of ours is
 * announced, no rmtfs or tftp server is run, and nothing is written to any storage. Everything
 * that was started is taken down at the end.
 *
 * Facts (the stock live device tree firmware/vayu/dt/recovery-live.dtb, decompiled; the numbers
 * are lines of that .dts):
 *
 *   - qcom,mss@4080000 (:7071-7097): qcom,pas-id = <4>, qcom,smem-id = <0x1a5> (421),
 *     qcom,firmware-name = "modem"; vdd_cx and vdd_mss both <0x181 100000> (0x181 =
 *     RPMH_REGULATOR_LEVEL_TURBO = 384 + offset 1, vendor
 *     include/dt-bindings/regulator/qcom,rpmh-regulator.h:32); vdd_mss is pm8150_s1_level =
 *     RPMh's "mss.lvl" (vendor sm8150-regulator.dtsi:36-40), vdd_cx is "cx.lvl" (:639).
 *   - Memory: stock modem_region@8e000000, size 0x9600000 (the bring-up notes
 *     §1 and its addendum: modem.mdt's loadable range is exactly 0x8e000000..0x97600000).
 *   - qcom,smp2p-modem@1799000c (VAYU_MODEM_PHASE0.md addendum): qcom,smem = <435 428> (theirs,
 *     ours), remote-pid 1, APCS IPC bit 14, SPI 451.
 *   - qcom,glink / modem (:7872-7903): remote-pid 1, mboxes = <&apcs 12>, SPI 449; the channel
 *     "IPCRTR" with qcom,intents = <0x800 5 0x2000 3 0x4400 2>; it may also open "DS",
 *     "glink_ssr" and "fastrpcglink-apps-dsp" (nothing here answers them; vayu_pas.c logs them
 *     in `others`).
 *   - Linux's qcom_q6v5_pas.c:1336-1351 (mpss_resource_init): load_state name "modem",
 *     crash_reason_smem 421, pas_id 4.
 *   - WLFW = QMI service 0x45 (the bring-up notes); the tftp server the modem
 *     may look for is QRTR service 4096 (same doc, W5 tqftpserv.c:965); rmtfs is QMI service 14
 *     (VAYU_MODEM_PHASE0.md §3).
 *
 * Boot-args (nothing at all unless vayu_modem is at least 1):
 *
 *   vayu_modem=N      0: nothing; 1: read and survey; 2: load and start; 3: also GLINK and QRTR
 *   vayu_modem_wait=S how long vayu_pas_start watches for "ready" (default 15)
 *   vayu_modem_listen=S  QRTR listening time (default 20, at most 120)
 *   vayu_modem_secs=S  kept up after step 2 when the level is 2 (default 2, at most 60)
 *   vayu_modem_cx=1    RPMh's "cx.lvl" raised with "mss.lvl" while it boots, and left there
 *   vayu_modem_rails=0 no RPMh vote at all (default 1: "mss.lvl" only, given back at the stop)
 *   vayu_pause=MS      before each first-time write (default 2200, given to vayu_pas_set_pause)
 *   vayu_wifi=N  (bsd/dev/vayu_wifi.c) our pd-mapper + tftp (1), and the WLFW probe (2)
 *   vayu_rmtfs=N (bsd/dev/vayu_rmtfs.c) the EFS service the modem asks for: 1 publishes it and logs
 *                the requests, 2 serves modemst1/modemst2/fsc/fsg from RAM (writes never reach storage)
 *   vayu_cell=N  (bsd/dev/vayu_cell.c) the SIM and the network over QMI, read-only: 1 one round,
 *                2 a round every vayu_cell_poll s; /dev/vayu-cell holds the record (see sys/vayu_cell.h)
 *   vayu_modem_chan=1  also open back "glink_ssr" and "SSM_RTR_MODEM_APPS" (the modem opened both in run 012936)
 *                      and log what it sends on them; nothing is sent there
 * After a FATAL, vayu_smem_census() lists the shared memory's items (its crash text was not in item 421).
 *
 * What it does NOT do: it runs no rmtfs server and no EFS of its own, it sends no QMI request
 * itself (not even WLFW's), it announces no QRTR service of ours, it writes nothing to any
 * storage, and it builds no GLINK message here at all. The servers and clients are other files,
 * each behind its own boot-arg: with vayu_wifi>=1 bsd/dev/vayu_wifi.c announces services of ours
 * (the pd-mapper and the tftp server, both read-only), and with vayu_wifi=2 it asks WLFW; with
 * vayu_rmtfs>=1 bsd/dev/vayu_rmtfs.c serves those partitions from RAM; with vayu_cell>=1
 * bsd/dev/vayu_cell.c asks UIM and NAS and leaves a record in /dev/vayu-cell. The default
 * (vayu_modem=3 alone) still sends no request at all.
 *
 * What may be seen:
 *   (a) TrustZone refuses at INIT_IMAGE or AUTH_AND_RESET (vayu_pas.c prints its code and answer);
 *   (b) it comes up and the QRTR services it announces are listed (WLFW 0x45 among them is what
 *       Wi-Fi's next step needs);
 *   (c) it comes up, then says FATAL within seconds because nobody serves rmtfs -- an INFERENCE
 *       from VAYU_MODEM_PHASE0.md §3, where every Linux setup runs rmtfs; the crash text then
 *       comes from SMEM item 421.
 *
 * The payload: the modem image is 33 files, 77.5 MB (about 38 MB compressed in the small image),
 * read from fw/ by vayu_pas.c (modem.mdt and modem.bNN).
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vayu_cell.h>
#include <sys/vayu_modem.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_rmtfs.h>
#include <sys/vayu_wifi.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level, wait_s = 15, listen_s = 20, secs = 2;
static bool glink_prepared;             /* its items were made: vayu_glink_down() on the way out */

/* Every line to the loader's log as well: what is done to the modem should be known even if the kernel dies. */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
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

/*
 * dt: qcom,mss@4080000 (recovery-live.dts:7071-7097): qcom,pas-id = <4>, qcom,smem-id = <0x1a5>
 * (421), qcom,firmware-name = "modem". Linux's qcom_q6v5_pas.c:1336-1351 (mpss_resource_init)
 * says the same: load_state name "modem", crash_reason_smem 421, pas_id 4.
 */
static struct vayu_rproc mpss = {
	.name = "modem", .firmware = "modem",		/* dt: qcom,firmware-name; Linux: load_state name */
	.pas_id = 4,					/* dt: qcom,pas-id = <4>; Linux: pas_id 4 */
	.memory = 0x8e000000, .memory_size = 0x9600000,	/* stock modem_region@8e000000, 0x9600000 (VAYU_MODEM_PHASE0.md §1) */
	.smem_host = 1,					/* dt: qcom,remote-pid = <1> (smp2p-modem and glink/modem) */
	.smp2p_theirs = 435, .smp2p_ours = 428,		/* dt: qcom,smp2p-modem@1799000c, qcom,smem = <435 428> */
	.smp2p_extra = "ipa",				/* dt: qcom,smp2p-ipa-1-out (only with vayu_smp2p_extra=1) */
	.crash_item = 421,				/* dt: qcom,smem-id = <0x1a5>; Linux: crash_reason_smem 421 */
	.apcs_smp2p = 14,				/* dt: APCS IPC bit 14 (VAYU_MODEM_PHASE0.md addendum) */
	.rails = { "mss.lvl", NULL },			/* vdd_mss = pm8150_s1_level = RPMh "mss.lvl" (sm8150-regulator.dtsi:36-40) */
	.rail_level = 384,				/* dt: <0x181 100000>; 0x181 = RPMH_REGULATOR_LEVEL_TURBO = 384 + 1 */
	.rails_shared = false,				/* mss.lvl is the modem's own: given back at the stop */
};

/*
 * dt: qcom,glink / modem (recovery-live.dts:7872-7903): remote-pid 1, mboxes = <&apcs 12>, SPI
 * 449; the channel "IPCRTR" with qcom,intents = <0x800 5 0x2000 3 0x4400 2>. The same edge may
 * open "DS", "glink_ssr" and "fastrpcglink-apps-dsp"; nothing here answers them (vayu_pas.c logs
 * them in `others`).
 */
enum { CHANNEL_QRTR, CHANNEL_SSR, CHANNEL_SSM };
static struct vayu_qrtr qrtr;
static void other_input(void *context, const uint8_t *data, uint32_t length);
static struct vayu_glink glink = {
	.rp = &mpss,
	.apcs_glink = 12,				/* dt: mboxes = <&apcs 12> */
	.fifo_size = 16384,				/* Linux's qcom_glink_smem.c: 16 KB */
	.channels = 1,
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } },
		                   .input = vayu_qrtr_input, .context = &qrtr },	/* dt: qcom,intents = <0x800 5 0x2000 3 0x4400 2> */
		/* vayu_modem_chan=1 only (.channels raised to 3): the two the modem opened on its own in run 012936 */
		[CHANNEL_SSR] = { .name = "glink_ssr", .intents = { { 0x400, 2 } }, .input = other_input, .context = (void *)(uintptr_t)"glink_ssr" },
		[CHANNEL_SSM] = { .name = "SSM_RTR_MODEM_APPS", .intents = { { 0x800, 2 } }, .input = other_input,
		                  .context = (void *)(uintptr_t)"SSM_RTR_MODEM_APPS" },
	},
};
static struct vayu_qrtr qrtr = { .glink = &glink, .channel = &glink.channel[CHANNEL_QRTR] };

/* What the modem sends on glink_ssr or SSM_RTR_MODEM_APPS (opened, never answered): the first 16 messages, in hex. */
static void
other_input(void *context, const uint8_t *data, uint32_t length)
{
	static unsigned said;
	char hex[3 * 24 + 1];
	uint32_t n = length < 24 ? length : 24;

	if (said++ >= 16) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		snprintf(hex + 3 * i, 4, " %02x", data[i]);
	}
	hex[3 * n] = 0;
	say("vayu-modem: on %s, %u bytes:%s%s\n", (const char *)context, length, hex, length > n ? " ..." : "");
}

/*
 * The rails: by default only the modem's own "mss.lvl", given back at the stop. vayu_modem_cx=1
 * asks for "cx.lvl" too (shared with UEFI, the display and UFS: never voted down; vayu_pas.c then
 * leaves mss raised too, which only costs power). vayu_modem_rails=0: no RPMh vote at all.
 */
static void
rails_choose(uint32_t cx, uint32_t rails)
{
	if (rails == 0) {
		mpss.rails[0] = mpss.rails[1] = NULL;
		mpss.rails_shared = false;
	} else if (cx != 0) {
		mpss.rails[0] = "cx.lvl";
		mpss.rails[1] = "mss.lvl";
		mpss.rails_shared = true;
	} else {
		mpss.rails[0] = "mss.lvl";
		mpss.rails[1] = NULL;
		mpss.rails_shared = false;
	}
}

/* Only these three are named: WLFW is what Wi-Fi needs; rmtfs and tftp are what the modem may look for. */
static const char *
service_note(uint32_t service)
{
	switch (service) {
	case 0x40: return " = SERVREG locator (pd-mapper)";	/* QMI SERVREG locator (ref/pdm_servreg_loc.qmi) */
	case 0x42: return " = SERVREG notifier";		/* QMI SERVREG notifier */
	case 0x45: return " = WLFW (Wi-Fi firmware service)";	/* QMI WLFW (VAYU_WIFI_PHASE0.md §2) */
	case 14: return " = rmtfs (ours: vayu_rmtfs.c)";	/* QMI rmtfs (VAYU_MODEM_PHASE0.md §3) */
	case 4096: return " = tftp";				/* QRTR tftp (VAYU_WIFI_PHASE0.md W5, tqftpserv.c:965) */
	}
	return "";
}

static bool
step_look(void)
{
	say("vayu-modem: 1: reads: the shared memory, the modem's last words, GLINK's items\n");
	if (!vayu_soc_check("vayu-modem: 1")) {
		return false;
	}
	vayu_pas_survey(&mpss);
	say("vayu-modem: 1: done\n");
	return true;
}

/* The modem loaded and started, its GLINK items made first: the order of vayu_audio.c's step_dsp(). */
static bool
step_up(void)
{
	uint32_t state;

	say("vayu-modem: 2: the modem: TrustZone asked, image checked, given to TrustZone, written; GLINK's items made; "
	    "started; watched for %u s\n", wait_s);
	if (!vayu_pas_supported(&mpss)) {
		say("vayu-modem: 2: FAILED: TrustZone does not answer IS_CALL_AVAILABLE either way\n");
		return false;
	}
	if (!vayu_pas_load(&mpss)) {
		say("vayu-modem: 2: FAILED: not loaded\n");
		return false;
	}
	if (!vayu_glink_prepare(&glink)) {
		say("vayu-modem: 2: FAILED: GLINK's items not made (the image is written but not started)\n");
		return false;
	}
	glink_prepared = true;
	if (!vayu_pas_start(&mpss, wait_s * 1000)) {
		say("vayu-modem: 2: FAILED: it did not come up\n");
		vayu_pas_stop(&mpss, "it did not come up");
		return false;
	}
	state = vayu_pas_state(&mpss);
	say("vayu-modem: 2: THE MODEM IS UP (\"slave-kernel\" %#x)\n", state);
	if (state & VAYU_RPROC_FATAL) {
		say("vayu-modem: 2: FAILED: it says it has given up\n");
		return false;
	}
	return true;
}

/*
 * GLINK up, then QRTR listened to: every second the modem's SMP2P state and the services it has
 * announced. No request is sent and no service of ours is announced.
 */
static void
step_link(void)
{
	bool seen[64] = { false };
	uint32_t heard = listen_s, s, node = 0, port = 0;

	say("vayu-modem: 3: GLINK with the modem, QRTR on IPCRTR (%u s)\n", listen_s);
	if (!vayu_glink_up(&glink, 5000)) {
		say("vayu-modem: 3: FAILED: the channel IPCRTR is not open\n");
		return;
	}
	vayu_qrtr_attach(&qrtr);
	vayu_rmtfs_start(&qrtr);
	vayu_cell_start(&qrtr);		/* vayu_cell=N: the SIM and the network, read-only (bsd/dev/vayu_cell.c) */
	vayu_wifi_start(&qrtr, listen_s);
	for (s = 1; s <= listen_s; s++) {
		delay_for_interval(1, NSEC_PER_SEC);
		for (unsigned i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
			if (!qrtr.servers[i].there || seen[i]) {
				continue;
			}
			seen[i] = true;
			say("vayu-modem: 3: +%u s: service %u (%#x) instance %#x at node %u port %u%s\n", s,
			    qrtr.servers[i].service, qrtr.servers[i].service, qrtr.servers[i].instance,
			    qrtr.servers[i].node, qrtr.servers[i].port, service_note(qrtr.servers[i].service));
		}
		if (vayu_pas_state(&mpss) & VAYU_RPROC_FATAL) {
			say("vayu-modem: 3: THE MODEM SAYS FATAL after %u s\n", s);
			vayu_pas_survey(&mpss);
			vayu_smem_census_all("vayu-modem");     /* every partition (critique W2 #6) */
			heard = s;
			break;
		}
	}
	say("vayu-modem: 3: after %u s: %s; the modem is node %u with %u services; it looked for %u of ours; QRTR in %u, out %u\n",
	    heard, qrtr.hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE MODEM", qrtr.peer, vayu_qrtr_services(&qrtr),
	    qrtr.lookups, qrtr.in, qrtr.out);
	if (vayu_qrtr_find(&qrtr, 0x45, &node, &port)) {
		say("vayu-modem: 3: WLFW (0x45) IS THERE: node %u port %u\n", node, port);
	} else {
		say("vayu-modem: 3: WLFW (0x45) is not there\n");
	}
	vayu_cell_stop();
	vayu_wifi_stop();
	vayu_rmtfs_stop();
	vayu_qrtr_detach(&qrtr);
}

static void
modem_thread(void *arg __unused, wait_result_t wr __unused)
{
	if (level >= 2) {
		/* (the firmware may be in /fw of the root, which is not mounted yet when this begins: a minute at most) */
		for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
			delay_for_interval(200, NSEC_PER_MSEC);
		}
	}
	if (!step_look()) {
		say("vayu-modem: 1: FAILED: not vayu's shared memory/command database; nothing more is done\n");
	} else if (level >= 2 && step_up()) {
		if (level >= 3) {
			step_link();
		} else {
			delay_for_interval(secs, NSEC_PER_SEC);
			say("vayu-modem: after %u s more its \"slave-kernel\" says %#x\n", secs, vayu_pas_state(&mpss));
		}
	}
	if (glink_prepared) {
		vayu_glink_down(&glink);
	}
	if (mpss.started) {
		vayu_pas_stop(&mpss, "the probe is over");
	}
	say("vayu-modem: everything taken down\n");
	thread_terminate(current_thread());
}

void
vayu_modem_quiesce(void)
{
	if (mpss.started) {
		vayu_pas_quiesce(&mpss);
	}
}

void
vayu_modem_init(void)
{
	uint32_t cx = 0, rails = 1, pause = 2200, chan = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("vayu_modem", &level, sizeof(level)) || level == 0) {
		return;
	}
	PE_parse_boot_argn("vayu_modem_wait", &wait_s, sizeof(wait_s));
	PE_parse_boot_argn("vayu_modem_listen", &listen_s, sizeof(listen_s));
	if (listen_s > 120) {
		listen_s = 120;                         /* twenty seconds is plenty; two minutes at the most */
	}
	PE_parse_boot_argn("vayu_modem_secs", &secs, sizeof(secs));
	if (secs > 60) {
		secs = 60;
	}
	PE_parse_boot_argn("vayu_modem_cx", &cx, sizeof(cx));
	PE_parse_boot_argn("vayu_modem_rails", &rails, sizeof(rails));
	rails_choose(cx, rails);
	PE_parse_boot_argn("vayu_pause", &pause, sizeof(pause));
	PE_parse_boot_argn("vayu_modem_chan", &chan, sizeof(chan));
	if (chan != 0) {
		glink.channels = 3;                     /* glink_ssr and SSM_RTR_MODEM_APPS opened back, their data logged */
	}
	vayu_pas_set_pause(pause);
	vayu_rmtfs_prepare();           /* the 3 MB rmtfs buffer from low memory, before the userland */
	say("vayu-modem: vayu_modem=%u vayu_modem_wait=%u vayu_modem_listen=%u vayu_modem_secs=%u vayu_modem_cx=%u vayu_modem_rails=%u "
	    "vayu_pause=%u vayu_modem_chan=%u\n", level, wait_s, listen_s, secs, cx, rails, pause, chan);
	if (kernel_thread_start(modem_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
