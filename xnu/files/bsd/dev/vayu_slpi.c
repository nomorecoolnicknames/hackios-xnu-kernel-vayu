/*
 * The sensors DSP (SLPI) of the POCO X3 Pro (vayu, SM8150-AC): a probe. Nothing here is touched
 * unless boot-arg vayu_slpi=N asks for it (N >= 1).
 *
 * The sensors' QMI service (SSC, service 0x190) runs on the SLPI, so the first sensors step is:
 * load the SLPI image exactly as vayu_audio.c loads the audio DSP, start it, open GLINK's IPCRTR
 * channel, and print the QRTR services the SLPI announces. This is a probe: no QMI request is
 * sent, no QRTR service of ours is announced, no rmtfs or tftp server is run, and nothing is
 * written to any storage. FastRPC is set up only when boot-arg vayu_frpc >= 1 (bsd/dev/
 * vayu_fastrpc.c); everything that was started is taken down at the end.
 *
 * Facts (the stock live device tree firmware/vayu/dt/recovery-live.dtb, decompiled; the numbers
 * are lines of that .dts):
 *
 *   - qcom,ssc@5c00000 (:7128-7175): qcom,pas-id = <12>, qcom,smem-id = <0x1a8> (424),
 *     qcom,firmware-name = "slpi"; vdd_cx = pm8150_l8_level and vdd_mx = pm8150_l4_level, both
 *     <0x181 0> (0x181 = RPMH_REGULATOR_LEVEL_TURBO = 384 + offset 1); qcom,signal-aop.
 *     vdd_cx is RPMh's "lcx.lvl" (vendor sm8150-regulator.dtsi:272-273), vdd_mx is "lmx.lvl"
 *     (:176-177).
 *   - Memory: pil_slpi_region@97b00000, size 0x1400000 (:27766-27771). The image slpi_vy.mdt is
 *     linked at 0x97300000 and is relocatable: vayu_pas.c places it from the start of its
 *     memory, Linux's mdt_loader rule, so it is relocated from 0x97300000 to 0x97b00000.
 *   - qcom,smp2p-dsps@1799000c (:22559-22565): qcom,smem = <481 430> (theirs, ours), SPI 172,
 *     qcom,ipc = <&apcs 0 26>, qcom,remote-pid = <3>.
 *   - qcom,glink / dsps (:7961-7986): remote-pid 3, mboxes = <&apcs 24>, SPI 170; the channel
 *     "IPCRTR" with qcom,intents = <0x800 5 0x2000 3 0x4400 2>. It also opens
 *     "fastrpcglink-apps-dsp" (qcom,intents = <0x64 0x40>), answered by bsd/dev/vayu_fastrpc.c
 *     when vayu_frpc=1, and "glink_ssr", which stays unanswered (vayu_pas.c logs it in `others`).
 *   - Linux's qcom_q6v5_pas.c:1392: load_state name "slpi" (the name the always-on processor is
 *     told when the image is loaded), pas_id 12.
 *   - Firmware: the vendor sensors loader asks for "slpi_vy" and falls back to "slpi"
 *     (see the bring-up notes); they hold slpi_vy.mdt and slpi_vy.b00-b20.
 *   - SSC = QMI service 0x190 (libqmi's qmi-service-ssc.json, docs/research/
 *     VAYU_SENSORS_PHASE0.md §3.1).
 *
 * Boot-args (nothing at all unless vayu_slpi is at least 1):
 *
 *   vayu_slpi=N        0: nothing; 1: read and survey; 2: load and start; 3: also GLINK and QRTR;
 *                      4: also SSC's SUID lookups and attributes; 5: also accel and gyro streamed;
 *                      6: also the accelerometer kept streaming for iOS (/dev/vayu-motion) until the
 *                      run ends, the SLPI left up meanwhile
 *                      (bsd/dev/vayu_ssc.c; the first QMI requests sent to the SLPI)
 *   vayu_ssc_secs=S    level 5: how long each sensor is streamed (default 10, at most 60)
 *   vayu_ssc_hz=N      level 5: the sample rate asked for (default 50; read by vayu_ssc.c)
 *   vayu_ssc_late=S    level 4+: if no SUID answers, keep asking for the registry and accel
 *                      every 10 s for up to S seconds (default 60; read by vayu_ssc.c)
 *   vayu_slpi_wait=S   how long vayu_pas_start watches for "ready" (default 15)
 *   vayu_slpi_listen=S QRTR listening time (default 20, at most 120)
 *   vayu_slpi_secs=S   kept up after step 2 when the level is 2 (default 2, at most 60)
 *   vayu_slpi_rails=0  no RPMh vote at all (default 1: "lcx.lvl" and "lmx.lvl", raised while it
 *                      boots and never given back)
 *   vayu_pause=MS      before each first-time write (default 2200, given to vayu_pas_set_pause)
 *   vayu_frpc=0|1|2    the FastRPC endpoint for the sensors PD (default 0: nothing): 1 opens
 *                      "fastrpcglink-apps-dsp", attaches the sensors PD, runs the reverse
 *                      listener and serves apps_std from fw/sensors.pak; 2 also hex-dumps the
 *                      first 16 messages each way (bsd/dev/vayu_fastrpc.c). Only read when
 *                      the level reaches GLINK (3).
 *
 * What levels 1-3 do NOT do (level 4+ hands SSC to vayu_ssc.c, which sends SSC's QMI
 * requests): no QMI request of any kind, no sensor is read, no rmtfs, no tftp server, no QRTR
 * service of ours announced, nothing written to any storage, and no GLINK message built here at
 * all. FastRPC stays off unless vayu_frpc=1; then step_link() starts bsd/dev/vayu_fastrpc.c,
 * which answers "fastrpcglink-apps-dsp" and adds the SMMU bypass for SIDs 0x5a1-0x5a3.
 *
 * What may be seen:
 *   (a) TrustZone refuses at INIT_IMAGE or AUTH_AND_RESET (vayu_pas.c prints its code and answer);
 *   (b) it comes up and the QRTR services it announces are listed (SSC 0x190 among them is what
 *       the sensors' next step needs);
 *   (c) it comes up, then says FATAL within seconds because nobody answers its SSR channel
 *       (and FastRPC too when vayu_frpc=0) -- an INFERENCE, the same way the modem gives up
 *       without rmtfs in VAYU_MODEM_PHASE0.md §3; the crash text then comes from SMEM item 424.
 *
 * The payload: slpi_vy.mdt and slpi_vy.b00-b20, read from fw/ by vayu_pas.c.
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
#include <sys/vayu_fastrpc.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_slpi.h>
#include <sys/vayu_ssc.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level, wait_s = 15, listen_s = 20, secs = 2, ssc_secs = 10, frpc = 0;
static bool glink_prepared;             /* its items were made: vayu_glink_down() on the way out */

/* Every line to the loader's log as well: what is done to the SLPI should be known even if the kernel dies. */
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
 * dt: qcom,ssc@5c00000 (recovery-live.dts:7128-7175): qcom,pas-id = <12>, qcom,smem-id = <0x1a8>
 * (424), qcom,firmware-name = "slpi". Linux's qcom_q6v5_pas.c:1392 says the same of it: load_state
 * name "slpi", pas_id 12.
 */
static struct vayu_rproc slpi = {
	.name = "slpi", .firmware = "slpi_vy",		/* dt: qcom,firmware-name; Linux: the AOP's load_state name */
	.pas_id = 12,					/* dt: qcom,pas-id = <12>; Linux: pas_id 12 */
	.memory = 0x97b00000, .memory_size = 0x1400000,	/* dt: pil_slpi_region@97b00000, 0x1400000 (:27766-27771) */
	.smem_host = 3,					/* dt: qcom,remote-pid = <3> (smp2p-dsps and glink/dsps) */
	.smp2p_theirs = 481, .smp2p_ours = 430,		/* dt: qcom,smp2p-dsps@1799000c, qcom,smem = <481 430> */
	.crash_item = 424,				/* dt: qcom,smem-id = <0x1a8>; Linux: crash_reason_smem 424 */
	.apcs_smp2p = 26,				/* dt: qcom,ipc = <&apcs 0 26> */
	.rails = { "lcx.lvl", "lmx.lvl" },		/* vdd_cx = pm8150_l8_level = "lcx.lvl" (sm8150-regulator.dtsi:272-273);
							 * vdd_mx = pm8150_l4_level = "lmx.lvl" (:176-177) */
	.rail_level = 384,				/* dt: <0x181 0>; 0x181 = RPMH_REGULATOR_LEVEL_TURBO = 384 + 1 */
	.rails_shared = true,				/* the island rails are never voted down from here (conservative; costs only power) */
};

/*
 * dt: qcom,glink / dsps (recovery-live.dts:7961-7986): remote-pid 3, mboxes = <&apcs 24>, SPI 170;
 * the channel "IPCRTR" with qcom,intents = <0x800 5 0x2000 3 0x4400 2>. The same edge also opens
 * "fastrpcglink-apps-dsp" (qcom,intents = <0x64 0x40>), which vayu_fastrpc.c answers when
 * vayu_frpc >= 1; "glink_ssr" stays unanswered (vayu_pas.c logs it in `others`).
 */
enum { CHANNEL_QRTR, CHANNEL_FASTRPC };
static struct vayu_qrtr qrtr;
static struct vayu_glink glink = {
	.rp = &slpi,
	.apcs_glink = 24,				/* dt: mboxes = <&apcs 24> */
	.fifo_size = 16384,				/* Linux's qcom_glink_smem.c: 16 KB */
	.channels = 1,					/* 2 when vayu_frpc >= 1 (vayu_slpi_init) */
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } },
		                   .input = vayu_qrtr_input, .context = &qrtr },	/* dt: qcom,intents = <0x800 5 0x2000 3 0x4400 2> */
		[CHANNEL_FASTRPC] = { .name = "fastrpcglink-apps-dsp",
		                   .intents = { { 0x64, 0x40 } },	/* dt: qcom,intents = <0x64 0x40> */
		                   .input = vayu_fastrpc_input, .context = NULL },
	},
};
static struct vayu_qrtr qrtr = { .glink = &glink, .channel = &glink.channel[CHANNEL_QRTR] };

/*
 * The rails: "lcx.lvl" and "lmx.lvl" by default, shared with the rest of the SoC and never voted
 * down from here (conservative; costs only power). vayu_slpi_rails=0: no RPMh vote at all.
 */
static void
rails_choose(uint32_t rails)
{
	if (rails == 0) {
		slpi.rails[0] = slpi.rails[1] = NULL;
		slpi.rails_shared = false;
	} else {
		slpi.rails[0] = "lcx.lvl";
		slpi.rails[1] = "lmx.lvl";
		slpi.rails_shared = true;
	}
}

/* Only SSC is named: it is what the sensors' next step needs. */
static const char *
service_note(uint32_t service)
{
	switch (service) {
	case 0x190: return " = SSC (the sensors' QMI service)";	/* QMI SSC (VAYU_SENSORS_PHASE0.md §3.1) */
	}
	return "";
}

static bool
step_look(void)
{
	say("vayu-slpi: 1: reads: the shared memory, the SLPI's last words, GLINK's items\n");
	if (!vayu_soc_check("vayu-slpi: 1")) {
		return false;
	}
	vayu_pas_survey(&slpi);
	say("vayu-slpi: 1: done\n");
	return true;
}

/* The SLPI loaded and started, its GLINK items made first: the order of vayu_audio.c's step_dsp(). */
static bool
step_up(void)
{
	uint32_t state;

	say("vayu-slpi: 2: the SLPI: TrustZone asked, image checked, given to TrustZone, written; GLINK's items made; "
	    "started; watched for %u s\n", wait_s);
	if (!vayu_pas_supported(&slpi)) {
		say("vayu-slpi: 2: FAILED: TrustZone does not answer IS_CALL_AVAILABLE either way\n");
		return false;
	}
	if (!vayu_pas_load(&slpi)) {
		say("vayu-slpi: 2: FAILED: not loaded\n");
		return false;
	}
	if (!vayu_glink_prepare(&glink)) {
		say("vayu-slpi: 2: FAILED: GLINK's items not made (the image is written but not started)\n");
		return false;
	}
	glink_prepared = true;
	if (!vayu_pas_start(&slpi, wait_s * 1000)) {
		say("vayu-slpi: 2: FAILED: it did not come up\n");
		vayu_pas_stop(&slpi, "it did not come up");
		return false;
	}
	state = vayu_pas_state(&slpi);
	say("vayu-slpi: 2: THE SLPI IS UP (\"slave-kernel\" %#x)\n", state);
	if (state & VAYU_RPROC_FATAL) {
		say("vayu-slpi: 2: FAILED: it says it has given up\n");
		return false;
	}
	return true;
}

/*
 * GLINK up, then QRTR listened to: every second the SLPI's SMP2P state and the services it has
 * announced. No request is sent and no service of ours is announced.
 */
static void
step_link(void)
{
	bool seen[64] = { false };
	uint32_t heard = listen_s, s, node = 0, port = 0;

	say("vayu-slpi: 3: GLINK with the SLPI, QRTR on IPCRTR (%u s)\n", listen_s);
	if (!vayu_glink_up(&glink, 5000)) {
		say("vayu-slpi: 3: FAILED: the channel IPCRTR is not open\n");
		return;
	}
	if (frpc >= 1) {
		/* Answer "fastrpcglink-apps-dsp" before SSC is talked to: the sensors PD's own
		 * registry reads go through this endpoint (bsd/dev/vayu_fastrpc.c). */
		vayu_fastrpc_start(&glink, &glink.channel[CHANNEL_FASTRPC], level);
	}
	for (s = 1; s <= listen_s; s++) {
		delay_for_interval(1, NSEC_PER_SEC);
		for (unsigned i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
			if (!qrtr.servers[i].there || seen[i]) {
				continue;
			}
			seen[i] = true;
			say("vayu-slpi: 3: +%u s: service %u (%#x) instance %#x at node %u port %u%s\n", s,
			    qrtr.servers[i].service, qrtr.servers[i].service, qrtr.servers[i].instance,
			    qrtr.servers[i].node, qrtr.servers[i].port, service_note(qrtr.servers[i].service));
		}
		if (vayu_pas_state(&slpi) & VAYU_RPROC_FATAL) {
			say("vayu-slpi: 3: THE SLPI SAYS FATAL after %u s\n", s);
			vayu_pas_survey(&slpi);
			heard = s;
			break;
		}
	}
	say("vayu-slpi: 3: after %u s: %s; the SLPI is node %u with %u services; it looked for %u of ours; QRTR in %u, out %u\n",
	    heard, qrtr.hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE SLPI", qrtr.peer, vayu_qrtr_services(&qrtr),
	    qrtr.lookups, qrtr.in, qrtr.out);
	if (vayu_qrtr_find(&qrtr, 0x190, &node, &port)) {
		say("vayu-slpi: 3: SSC (0x190) IS THERE: node %u port %u\n", node, port);
		if (level >= 4 && !(vayu_pas_state(&slpi) & VAYU_RPROC_FATAL)) {
			/* level 4: SUID lookups and attributes; 5: accel and gyro streamed (vayu_ssc.c) */
			vayu_qrtr_attach(&qrtr);	/* our ports on IPCRTR (vayu_pas.c; up to four routers at once) */
			say("vayu-slpi: %u: SSC: %s\n", level, vayu_ssc_run(&vayu_qrtr_ops, node, port, level, ssc_secs) ?
			    "it answered" : "NO ANSWER to any lookup");
			vayu_qrtr_detach(&qrtr);
		}
	} else {
		say("vayu-slpi: 3: SSC (0x190) is not there\n");
	}
}

static void
slpi_thread(void *arg __unused, wait_result_t wr __unused)
{
	if (level >= 2) {
		/* (the firmware may be in /fw of the root, which is not mounted yet when this begins: a minute at most) */
		for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
			delay_for_interval(200, NSEC_PER_MSEC);
		}
	}
	if (!step_look()) {
		say("vayu-slpi: 1: FAILED: not vayu's shared memory/command database; nothing more is done\n");
	} else if (level >= 2 && step_up()) {
		if (level >= 3) {
			step_link();
		} else {
			delay_for_interval(secs, NSEC_PER_SEC);
			say("vayu-slpi: after %u s more its \"slave-kernel\" says %#x\n", secs, vayu_pas_state(&slpi));
		}
	}
	if (frpc >= 1) {
		vayu_fastrpc_quiesce();		/* the next2 invoke returns; the thread ends */
	}
	if (glink_prepared) {
		vayu_glink_down(&glink);
	}
	if (slpi.started) {
		vayu_pas_stop(&slpi, "the probe is over");
	}
	if (frpc >= 1) {
		vayu_fastrpc_release();		/* the SLPI is stopped: the SMMU entry goes back */
	}
	say("vayu-slpi: everything taken down\n");
	thread_terminate(current_thread());
}

void
vayu_slpi_quiesce(void)
{
	vayu_fastrpc_quiesce();			/* before the SLPI is stopped */
	vayu_ssc_quiesce();
	if (slpi.started) {
		vayu_pas_quiesce(&slpi);
	}
	/* No vayu_fastrpc_release() on this path: a panic leaves the SMR in place; it is harmless
	 * because the DSP is stopped with it. */
}

void
vayu_slpi_init(void)
{
	uint32_t rails = 1, pause = 2200;
	thread_t thread;

	if (!PE_parse_boot_argn("vayu_slpi", &level, sizeof(level)) || level == 0) {
		return;
	}
	PE_parse_boot_argn("vayu_slpi_wait", &wait_s, sizeof(wait_s));
	PE_parse_boot_argn("vayu_slpi_listen", &listen_s, sizeof(listen_s));
	if (listen_s > 120) {
		listen_s = 120;                         /* twenty seconds is plenty; two minutes at the most */
	}
	PE_parse_boot_argn("vayu_slpi_secs", &secs, sizeof(secs));
	if (secs > 60) {
		secs = 60;
	}
	PE_parse_boot_argn("vayu_ssc_secs", &ssc_secs, sizeof(ssc_secs));
	if (ssc_secs > 60) {
		ssc_secs = 60;
	}
	PE_parse_boot_argn("vayu_slpi_rails", &rails, sizeof(rails));
	rails_choose(rails);
	PE_parse_boot_argn("vayu_pause", &pause, sizeof(pause));
	vayu_pas_set_pause(pause);
	PE_parse_boot_argn("vayu_frpc", &frpc, sizeof(frpc));
	if (frpc >= 1) {
		/* The second channel in the table is "fastrpcglink-apps-dsp" (bsd/dev/vayu_fastrpc.c):
		 * it must be in the table before the SLPI starts, or vayu_pas.c logs the open as
		 * unanswered. */
		glink.channels = 2;
	}
	say("vayu-slpi: vayu_slpi=%u vayu_slpi_wait=%u vayu_slpi_listen=%u vayu_slpi_secs=%u vayu_ssc_secs=%u vayu_slpi_rails=%u "
	    "vayu_pause=%u vayu_frpc=%u\n", level, wait_s, listen_s, secs, ssc_secs, rails, pause, frpc);
	if (kernel_thread_start(slpi_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
