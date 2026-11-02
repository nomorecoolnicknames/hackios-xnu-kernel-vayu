/*
 * Wi-Fi's second step on the POCO X3 Pro (vayu, SM8150-AC): the two services of ours the modem's
 * firmware looks for before it starts its WLAN firmware, and a read-only look at WLFW.
 *
 * On the phone WLFW (QMI service 0x45) runs on the modem processor (MPSS). Before its WLAN firmware
 * starts, Linux gives the modem a pd-mapper (the SERVREG locator, QMI service 0x40) and a tftp server
 * (QRTR service 4096) from which it reads wlanmdsp.mbn and its MCFG configuration. This file does the
 * same two things -- /readonly/ out of the boot image, /readwrite/ in RAM, nothing on any storage --
 * and then asks WLFW four questions and no more. Everything is behind boot-arg vayu_wifi=N.
 *
 * Facts and where they come from:
 *   - WLFW is QMI service 0x45 on the modem (docs/research/VAYU_WIFI_PHASE0.md §2); its message ids,
 *     structures and element tables are the ones of ref/qmi_wlfw_v01.h and ref/qmi_wlfw_v01.c, which
 *     are the WCN3990 definitions used by ath10k (ref/qmi.c).
 *   - The pd-mapper is the SERVREG locator: service 0x40 (ref/pdm_servreg_loc.qmi:3). libqrtr publishes
 *     version 0x101 instance 0 (ref/pdm_pd-mapper.c:422-423); on the wire 0x101 is version 1 instance 1,
 *     and Linux's pdr_interface.c looks it up as version 1 instance 1. We publish that.
 *   - The domain table below is the stock vayu modem partition's firmware/vayu/modem/*.jsn: sr_domain's
 *     soc/domain/subdomain and sr_service's provider/service, with qmi_instance_id (ref/pdm_pd-mapper.c:
 *     122-194). The .jsn files named in the table are the ones on that partition.
 *   - The tftp service is QRTR service 4096 version 1 instance 0 (VAYU_WIFI_PHASE0.md W5;
 *     ref/tqftpserv.c:965). Its options and their handling are those of ref/tqftpserv.c
 *     (parse_options, handle_rrq, tftp_send_oack, tftp_send_data, handle_reader), and its paths
 *     those of ref/translate.c's read-only part, except that the part after the prefix may be
 *     nested: the firmware's MCFG loader asks for
 *     /readonly/firmware/image/modem_pr/mcfg/configs/mcfg_{hw,sw}/mbn_{hw,sw}.dig, which a phone's
 *     tqftpserv serves out of the whole /vendor/firmware_mnt tree.  The tree here is the boot
 *     image's fw/, so "/" in the path is a directory below fw/ (mkapfs.py builds it from root/fw,
 *     and build.sh's fw-modem.list says which MCFG files travel).
 *   - MSA: the stock device tree's icnss node has qcom,wlan-msa-memory = <0x100000>, and the fixed
 *     region pil_wlan_fw_region@8bc00000 is where it lives. WLFW's host_cap daemon_support, the
 *     ind_register client id 0x4b4e454c and the four requests are ath10k's (ref/qmi.c:25, 121-197,
 *     545-806, 964-1133).
 *   - hakios_image_file("fw", name, &size, NULL) is the boot image's fw/ directory, whole in memory,
 *     never freed.
 *
 * Boot-args (read in vayu_wifi_start()):
 *
 *   vayu_wifi=N          0: nothing (the default); 1: the pd-mapper and the tftp server; 2: also the
 *                        WLFW probe.
 *   vayu_wifi_hostcap=N  1 (default): host_cap with the normal table; 2: the 8-bit table; 0: no
 *                        host_cap at all.
 *   vayu_wifi_cap=0      do not send the cap request (default 1: send it).
 *   vayu_wifi_bdf_file=NAME  take exactly fw/NAME as the board data file, and no other: a name that
 *                        is not in the boot image is an error, not a fallback, so a run cannot
 *                        silently test the wrong file.  The alias vayu_wifi_bdf_name=NAME is accepted
 *                        for the older test7 patch.  Without the flag: bdwlan.b<hex board id>, then
 *                        bdwlan.<hex board id> (the stock high ids drop the b: bdwlan.102), then
 *                        bdwlan.bin.  Stock names are hexadecimal: bdwlan.b0a, bdwlan.b71,
 *                        bdwlan.102, ...
 *   vayu_wifi_cal_report=0  after the BDF, do not send CAL_REPORT; keep waiting for FW_READY.
 *                        Default 1: send it (ath10k always does, qmi.c:918-934).
 *   vayu_wifi_probe=N   0 (default): off, every byte of the pre-FW_READY flow stays as it is.
 *                        1: CAL_REPORT with no host board file at all -- the one combination never
 *                        run; every crash run so far had BDF + cal, so this separates "the cal
 *                        path alone kills it" from "the host BDF fed to the cal is the trigger".
 *                        2: BDF, then WLAN_CFG + WLAN_MODE MISSION, then CAL_REPORT (does the
 *                        firmware want the CE/MAC configuration before it calibrates?).
 *                        3: BDF, then WLAN_MODE with driver mode CALIBRATION (7), then CAL_REPORT.
 *                        4: BDF, then CAL_REPORT with cal ids {0,1,2,3} in meta_data (the shape
 *                        ath10k sends when the host already holds cal data, ref/qmi.c:341-352).
 *                        5: BDF, then CAL_REPORT with xo_cal_data_valid=1, xo_cal_data=0 (the
 *                        stock DT has no qcom,xo-cal-data, so ath10k would not send it; this is
 *                        the falsifier for "the firmware needed the XO calibration byte").
 *                        Why: wlanmdsp.mbn dies at PC 0xb0193960 inside WHAL's per-MAC-block
 *                        register write that runs after the cold-boot calibration
 *                        (wal_cold_boot_cal.c, wal_dev_attach); the store targets MAC block
 *                        0x16000 + 0xcc.  See W7-report.md, section 3.
 *   vayu_wifi_reattach=N 0 (default): off.  N = 1..3 (clamped): after the firmware's PD restart
 *                        re-announces the WLFW service on a new port, re-run IND_REGISTER -> CAP ->
 *                        BDF -> CAL_REPORT on that new instance, as ath10k does on server_arrive
 *                        (ref/qmi.c:1030-1075).  The instance we calibrated against is dead, so
 *                        without this the FW_READY wait can only run out.  Every re-run keeps the
 *                        same bounded timeouts; the wait for the new port is
 *                        vayu_wifi_reattach_wait ms (default 20000, clamped 1000..60000).
 *   vayu_wifi_bdf=3      the stock flow: no HOST_CAP, no board file, no cal_report, then wait for
 *                        FW_READY for vayu_wifi_bdf_wait ms.  This is the whole pre-FW_READY
 *                        sequence of the vayu stock driver: icnss.c:955-1050 powers the rails and
 *                        the cxo_ref_clk_pin clock, sends IND_REGISTER, MSA_INFO, the MSA
 *                        permissions, MSA_READY and CAP, and nothing else -- it has no BDF and no
 *                        CAL_REPORT call at all (the only mention of the BDF message length is
 *                        the QMI handle's buffer size, icnss_qmi.c:1270).  Use it to see whether
 *                        the firmware reaches FW_READY on its own, with the rails and the clock
 *                        voted, which is what the stock host gives it.
 *   vayu_wifi_stock2=1   0 (default): off, every existing flow is byte-identical.  1: repeat the
 *                        stock Android WLFW sequence of receipts/2026-10-24-vayu-wifi.md (W12):
 *                        no HOST_CAP; K's IND_REGISTER has rejuvenate_enable=1 (26 B); MSA_INFO
 *                        0x180000; MSA_READY; then a SECOND QMI client (its own qmi_handle and
 *                        QRTR port, like cnss-daemon 0x43f6) sends its own IND_REGISTER (31 B:
 *                        TLVs 0x10,0x11,0x12,0x13,0x15,0x1a,0x1d, client id 0x444d4f4e), CAP,
 *                        the BDF (bdwlan.bin, bdf_type TLV 0x15=0, segments 4*0x1826 + 0x6fe)
 *                        and CAL_REPORT (8 B: TLV 0x01 count 0 + TLV 0x11=0), while K sends
 *                        DYNAMIC_FEATURE_MASK 0x1 after CAP; FW_READY is waited for 30 s.
 *                        The other knobs still apply, but stock2 forces hostcap=0, dynmask=1,
 *                        msa_kb=1536 and skips WLAN_CFG/WLAN_MODE after FW_READY (stock icnss
 *                        sends neither; the wlan driver, absent in the stock capture, does).
 *   vayu_wifi_regdb=N    0 (default): no regdb download.  1: send fw/regdb.bin over BDF_DOWNLOAD
 *                        with bdf_type=1 (REGDB) after the BDF, before CAL_REPORT.  2: same, but
 *                        after CAL_REPORT.  If fw/regdb.bin is absent the run logs it and goes on;
 *                        the default firmware strings show regdb going through WMI, not QMI, so this
 *                        is an opt-in test of the stock host's behaviour, not a claimed fix.
 *
 * After cap and with vayu_wifi_bdf=1 (default) it downloads the board data file from the boot
 * image's fw/ (bdwlan.b<board>, fallback bdwlan.bin) over BDF_DOWNLOAD, sends CAL_REPORT, waits
 * for FW_READY, then with vayu_wifi_wlan=1 (default) sends WLAN_CFG (the ath10k_snoc WCN3990 CE
 * target/service/shadow tables) and WLAN_MODE MISSION.  The host WCN3990/SNOC/CE/ath10k data path
 * is still absent, so this does not create a network interface and there is no internet.
 *
 *   vayu_wifi_bdf=0      stop after cap: no MSA_READY wait, no BDF, no cal_report.
 *   vayu_wifi_bdf=2      the no-host-BDF test: no board file and no cal_report, but keep watching
 *                        for FW_READY and for the firmware's PFR for vayu_wifi_bdf_wait ms (15000).
 *                        The point is to see whether the firmware survives when the host does not
 *                        feed it its own BDF; with bdf=1 it dies ~10 ms after cal_report.
 *   vayu_wifi_dynmask=1  after CAP send the one pre-FW_READY message the stock icnss sends and
 *                        this file never did: WLFW DYNAMIC_FEATURE_MASK (QMI 0x003B, mask 0x01;
 *                        icnss.c:68 and :1042-1043, icnss_qmi.c:942-1010).  Default 0, so the
 *                        W5/W9 "stock flow" (vayu_wifi_bdf=3) stays exactly what it was.  The
 *                        stock driver ignores the answer; a rejection is logged and the
 *                        sequence goes on.  vayu_wifi_dynmask_bits=N changes the 64-bit mask
 *                        (default 1, ICNSS_DEFAULT_FEATURE_MASK).
 *   vayu_wifi_smmu=1     before the first WLFW message, put the WLAN stream IDs 0x640/0x641
 *                        (DT iommus = <&apps_smmu 0x640 0x1>) into a stage-1 SMMU context and
 *                        identity-map the 1.5 MiB MSA reservation in it.  Stock icnss does this
 *                        in icnss_probe() -> icnss_smmu_init() (icnss.c:2647-2735, :3837), i.e.
 *                        long before the firmware starts, and Linux enables the ANOC2 TBU
 *                        (anoc_2_tbu@0x15189000, stream-id range 0x400..0x7ff, vdd =
 *                        hlos1_vote_aggre_noc_mmu_tbu2_gdsc at GCC 0x17d048) through that TBU's
 *                        regulator.  Without the TBU a translation context is inert; this code
 *                        therefore powers the GDSC itself and logs the register read-back.
 *                        See vayu_wifi_smmu.c.  Default 0: no SMMU writes at all.
 *   vayu_wifi_ce=1       W15-A: after FW_READY and a WLAN_CFG/WLAN_MODE MISSION that both
 *                        answered result 0 error 0, wait at least 100 ms, allocate one
 *                        contiguous DMA pool (size computed from host_ce_config_wlan, no fixed
 *                        physical address), map it in the stage-1 SMMU context that
 *                        vayu_wifi_smmu=1 installed, program the 12 host copy engines, post 32
 *                        receive buffers on every host RX CE and poll (1 ms, hard cap 5 s, no
 *                        interrupts) for the firmware's HTC READY on CE2.  The pool's CPU side
 *                        is a Normal Non-cacheable alias (VM_WIMG_WCOMB -> MAIR attr 1 = 0x44)
 *                        and its zeroing is flushed once; the SMMU's Normal Non-cacheable leaf
 *                        alone is not a CPU-coherency argument.  A valid READY prints
 *                        "vayu-wifi: htc: READY on ce2 ..." and nothing is generated locally.
 *                        Requires vayu_wifi_smmu=1 and vayu_wifi_stock2=0; if either gate fails
 *                        no CE register is written.  This stage stops at READY: no HTC CONNECT,
 *                        no WMI, no HTT, no scan, no ifnet.  Default 0: no CE access at all.
 *   vayu_wifi_smp2p=1    read the modem's SMP2P "wlan" inbound entry (DT
 *                        qcom,smp2p-wlan-1-in, entry-name "wlan", SMEM item 435 of host 1) before
 *                        the first WLFW message and every 100 ms during the MSA_READY and
 *                        FW_READY waits.  Bit 0 is force-fatal-error, bit 1 is early-crash-ind;
 *                        this is the same channel the stock icnss registers after cap
 *                        (icnss.c:731-812, :1046-1049).  Read-only by default, as Linux's
 *                        inbound SMP2P path is (smp2p.c:qcom_smp2p_notify_in).  With
 *                        vayu_wifi_smp2p_abort=1 (default) a rising bit 1 ends the wait, which is
 *                        what the stock early-crash handler does (icnss.c:706-728).
 *   vayu_wifi_smp2p_item=N / vayu_wifi_smp2p_host=N   override the defaults 435 / 1 (the DT's
 *                        qcom,smem = <435 428> on the modem edge).
 *   vayu_wifi_smp2p_clear=1  also clear bits 0 and 1 found when the entry is armed.  Linux does
 *                        not do this; it is only for a bit left by an earlier boot.
 *   vayu_wifi_qdss=1     W13: do flame's pre-modem power step.  vayu_modem.c step_up() calls
 *                        vayu_wifi_pre_modem() before vayu_pas_load(), and that votes the four
 *                        WLAN LDOs (ldoa1/ldoa7/ldoc2/ldoc11) and rfclka2 exactly as the normal
 *                        path does, then sends the AOP QMP message
 *                        "{class: clock, res: qdss, val: 1}".  This is what ios-flame does in
 *                        phone/wifi/wland.c:128-148 before boot_modem() (wland.c:692), and what
 *                        mainline's clk-aop-qmp.c:223/:302 does at probe (stock vayu:
 *                        CONFIG_MSM_CLK_AOP_QMP=y, vayu_defconfig:4719).  Default 0: no QMP and
 *                        the rails are voted where they always were, after the modem is up.
 *                        The message goes out at most once; a second call is a no-op.
 *   vayu_wifi_mac=1      W13: send WLFW MAC_ADDR (QMI 0x0033, one 6-byte TLV 0x10) immediately
 *                        before CAL_REPORT, as ios-flame's wlfw.c:325-329 does (locally
 *                        administered 02:50:49:58:45:4c).  In vayu_wifi_stock2=1 the second
 *                        client (the cnss-daemon role) sends it, since that client sends the
 *                        CAL_REPORT there; otherwise the kernel client does.  Default 0: the
 *                        stock vayu capture has no 0x33 at all (W12 report, 30 messages).
 *   vayu_wifi_fw_ready_wait=MS  override every post-cap FW_READY wait (clamped to 180000).
 *                        0 (default) keeps the per-flow value: vayu_wifi_bdf_wait for bdf=2/3,
 *                        vayu_wifi_cal_wait after CAL_REPORT, and 10000 for BDF only.
 *   vayu_wifi_cal_wait=MS  how long to wait for FW_READY after CAL_REPORT (default 15000;
 *                        clamped 1000..180000).  The firmware's own cold boot cal timeouts are
 *                        4 s and 8 s (strings at wlanmdsp.mbn 0x3d42d4 and 0x3d4330), so 10 s
 *                        left no margin; 15 s does.
 *   vayu_wifi_msa_wait=MS  how long to wait for the MSA_READY indication (default 5000; clamped
 *                        100..30000).
 *   vayu_wifi_power_settle=MS  wait this long after the last rail/clock vote and before the first
 *                        QMI message (default 0; clamped to 5000).  Stock's rails are up since
 *                        icnss_hw_power_on() at server-arrive, i.e. also immediately before
 *                        ind_register, so this is only a timing experiment.
 *   vayu_wifi_msa_kb=N   size of the MSA handed to WLFW MSA_INFO, in KiB.  Default 1024 (0x100000,
 *                        the stock DT's qcom,wlan-msa-memory).  1536 (0x180000) is what the vayu
 *                        stock icnss actually passes: with qcom,wlan-msa-fixed-region it takes the
 *                        size of the reserved region (pil_wlan_fw_region@8bc00000 .. 8bd80000,
 *                        0x180000) and never reads qcom,wlan-msa-memory (icnss.c:3767-3791 of the
 *                        vayu-r-oss vendor tree).  The firmware splits whatever it gets into a
 *                        secure 0x4000 and the rest, and its cold boot calibration allocates from
 *                        that rest; 0x100000 leaves it 0xfc000, stock leaves it 0x17c000.
 *
 * What it never does: no INI, no ATHDIAG_*, no RESPOND_MEM, no SCM call and no SMEM write.  A
 * MAC_ADDR (0x33) is sent only with vayu_wifi_mac=1 (W13).  It never writes to any storage: the
 * tftp server serves /readonly/ out of the boot image
 * and keeps /readwrite/ in RAM (the modem's liveness probe and its temporary MCFG file), so a WRQ
 * is either stored in a kernel buffer or refused with the reason in the log.
 *
 * What may be seen:
 *   H1: the modem reads wlanmdsp.mbn over tftp and then WLFW (0x45) appears -- then the firmware
 *       waited for these two services, and the WLFW probe's answers are read from a live service.
 *   H2: 0x45 appears without any tftp read -- the pd-mapper (or something else) was enough, or the
 *       firmware did not need the file this time.
 *   H3: nothing is asked at all and 0x45 never appears -- the modem waits for something else, for
 *       example rmtfs (QMI service 14; VAYU_MODEM_PHASE0.md §3), which this file does not serve either.
 *
 * The tftp server is in the link thread (one callback at a time); its transfer table is shared with
 * vayu_wifi_stop() and is protected by vayu_wifi_lock. No lock is held across a send or a close.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.qmi.h>
#include <sys/systm.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_wifi.h>
#include <sys/vayu_wifi_ce.h>
#include <sys/vayu_wifi_smmu.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
/* The boot image's fw/ directory, whole and in memory that stays: never freed. */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

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
	printf("%s", line);
	return length;
}

/* ---- the file's own lock and counters ------------------------------------------------------------ */

static LCK_GRP_DECLARE(vayu_wifi_lck_grp, "vayu-wifi");
static LCK_MTX_DECLARE(vayu_wifi_lock, &vayu_wifi_lck_grp);

#define VAYU_PDM_SERVICE        0x40    /* SERVREG locator (ref/pdm_servreg_loc.qmi:3) */
#define VAYU_PDM_GET_DOMAIN_LIST 0x21
#define VAYU_PDM_PFR            0x24
#define VAYU_TFTP_SERVICE       4096    /* QRTR tftp (ref/tqftpserv.c:965) */
#define VAYU_WLFW_SERVICE       0x45
#define VAYU_WLFW_VERSION       1
#define VAYU_WLFW_INSTANCE      0

#define VAYU_PDM_LOG_MAX        32
/*
 * 48 lines were used up by the first boot stage alone (96 reads of wlanmdsp.mbn, 66 of the
 * ruleset, both MCFG requests): after that the log went silent and "what does the modem ask for"
 * could not be answered from the run.  The census in tftp_path_dump() gives the list, this only
 * makes sure the individual lines are there too.
 */
#define VAYU_TFTP_LOG_MAX       256
#define VAYU_TFTP_XFERS         8

static uint32_t wifi_level, wifi_hostcap = 1, wifi_cap = 1;
static uint32_t wifi_bdf = 1, wifi_wlan = 1;   /* 1: BDF download / WLAN cfg+mode after cap */
static char wifi_bdf_file[64];            /* vayu_wifi_bdf_file=NAME: that file, and no fallback */
static uint32_t wifi_cal_report = 1;      /* vayu_wifi_cal_report=0: BDF only, skip CAL_REPORT */
static uint32_t wifi_probe;               /* vayu_wifi_probe=N: post-cap variant, 0 = off */
static uint32_t wifi_reattach;            /* vayu_wifi_reattach=N: re-run on a new WLFW server */
static uint32_t wifi_reattach_wait_ms = 20000; /* vayu_wifi_reattach_wait=MS: wait for that port */
static uint32_t wifi_reattach_runs;       /* how many times the sequence was re-run */
static uint32_t wifi_regdb;               /* vayu_wifi_regdb=1: after BDF; 2: after CAL_REPORT */
static uint32_t wifi_dynmask;             /* vayu_wifi_dynmask=1: QMI 0x003B after CAP (stock icnss) */
static uint32_t wifi_stock2;              /* vayu_wifi_stock2=1: stock WLFW with a second client (W12) */
static uint32_t wifi_qdss;                /* vayu_wifi_qdss=1: rails + AOP QDSS clock before the modem (W13) */
static uint32_t wifi_mac;                 /* vayu_wifi_mac=1: WLFW MAC_ADDR (0x33) before CAL_REPORT (W13) */
static bool wifi_qdss_sent;               /* the AOP took the QDSS clock vote (one message per run) */
static bool wifi_power_pre;               /* pre_modem voted the rails before the modem was loaded */
static bool wifi_pre_modem_seen;          /* vayu_wifi_pre_modem() was called (the modem path is in use) */
static uint32_t wifi_dynmask_bits = 1;    /* vayu_wifi_dynmask_bits=N: the 64-bit mask (stock 0x01) */
static uint32_t wifi_msa_fixed_perm;      /* 1: TZ fixes the MSA permissions; skip the assign */
static uint32_t wifi_rail = 1;            /* 1: vote all WCN3990 rails before WLFW */
static uint32_t wifi_smmu;                /* 1: SID 0x640/0x641 translation + MSA before WLFW */
static uint32_t wifi_ce;                  /* 1: W15-A host copy engines up to a real HTC READY */
static uint32_t wifi_bdf_wait_ms = 15000; /* vayu_wifi_bdf=2: how long to watch for FW_READY */
static uint32_t wifi_fw_ready_wait_ms;    /* vayu_wifi_fw_ready_wait=MS: override every post-cap wait */
static uint32_t wifi_cal_wait_ms = 15000; /* vayu_wifi_cal_wait=MS: after CAL_REPORT */
static uint32_t wifi_msa_wait_ms = 5000;  /* vayu_wifi_msa_wait=MS: MSA_READY indication */
static uint32_t wifi_power_settle_ms;     /* vayu_wifi_power_settle=MS: after the last rail/clock vote */
static uint32_t wifi_arc_level;           /* vayu_wifi_arc=LEVEL: RPMh cx.lvl+mx.lvl before WLFW (0: off) */
static uint32_t wifi_cxmx_mv = 752;       /* vayu_wifi_cxmx_mv=MV: the vdd-cx-mx rail's voltage (stock 752) */
static uint32_t wifi_bus;                 /* vayu_wifi_bus=1: an SN0 keepalive vote before WLFW */
static uint32_t wifi_census;              /* vayu_wifi_census=1: the modem's SMEM items after a PFR */
static uint32_t wifi_smp2p_host;           /* (tentative: defined with its default further down; the census above uses it) */
static uint32_t wifi_msa_kb = 1024;       /* vayu_wifi_msa_kb=N: MSA size in KiB, see the header */
static uint32_t wifi_listen_s;
static bool wifi_started;
static bool wifi_stopping;
static volatile bool vayu_pdm_pfr_seen;      /* a pd-mapper PFR was received: no new CE work */

/*
 * The WLFW step in progress, for the pd-mapper's PFR line: the firmware's crash report arrives at
 * the pd-mapper, not here, and "RT:0x4d:PC=0xb0193960" says nothing about what the host had just
 * done.  With the stage in that line a run says "it died while waiting for FW_READY after
 * cal_report" itself, without matching two logs by hand.
 */
static const char *wlfw_stage = "start";

static struct hakios_qrtr_port *pdm_port;
static uint32_t pdm_requests, pdm_log_lines, pdm_busy;
static bool pdm_unknown_logged;
static uint8_t pdm_answer[4096];         /* only the link thread's one pd callback at a time uses it */

static struct hakios_qrtr_port *tftp_port;
static uint32_t tftp_reads, tftp_served, tftp_writes, tftp_log_lines, tftp_busy;
static uint32_t tftp_write_blocks;             /* DATA packets of a write, stored or refused */
static uint32_t tftp_write_refused;            /* WRQs refused: not /readwrite, bad name, no room */
static uint64_t tftp_write_bytes;
static uint64_t tftp_bytes;

#define VAYU_TFTP_PATH_MAX      127     /* a whole request path, as the modem sends it */

/*
 * The RAM overlay behind /readwrite/.  On a phone tqftpserv maps that prefix to a directory of
 * rmtfs, and the modem writes its liveness probe (/readwrite/server_check.txt), a temporary MCFG
 * file and reads them back before it will start wlan_pd.  There is no rmtfs directory here, so the
 * files live in these buffers -- never on any storage, dropped when the service stops.  A WRQ
 * anywhere else is refused and logged with its node and port (tftp_handle_wrq).
 *
 * The write in progress is tracked per file, not per server, because DATA carries no name: the
 * writing node and port are what tie a DATA block to its WRQ.  The block size is the one the WRQ
 * negotiated, or 512; a transfer ends on a block shorter than that (RFC 1350), not on "shorter than
 * 512", which broke every write that used tqftpserv's blksize.
 */
#define VAYU_TFTP_RAM_FILES     4
#define VAYU_TFTP_RAM_MAX       0x4000  /* per file; the probe is 5 bytes, mcfg.tmp is 1 */
static uint8_t tftp_ram_data[VAYU_TFTP_RAM_FILES][VAYU_TFTP_RAM_MAX];
static struct vayu_tftp_ram_file {
	bool used;                      /* what it holds is served back until the service stops */
	bool writing;                   /* a WRQ owns it and its DATA blocks are landing in it */
	char path[VAYU_TFTP_PATH_MAX + 1];
	uint32_t size;                  /* bytes of the last complete write */
	uint32_t written;               /* bytes of the write in progress */
	uint32_t blocks;                /* DATA blocks stored by the write in progress */
	uint32_t limit;                 /* VAYU_TFTP_RAM_MAX, or less for a smaller slot */
	uint16_t expected;              /* the next DATA block of the write in progress */
	uint32_t blksize;               /* as the WRQ negotiated it (RFC 2348) */
	uint32_t node, port;            /* who is writing: the only key a DATA block has */
} tftp_ram[VAYU_TFTP_RAM_FILES];

/*
 * Which paths the modem asked for, in the order it first asked: the run's one-line answer to "what
 * does this firmware want".  A repeat (a retry, a tsize probe, a second boot stage) only counts.
 */
#define VAYU_TFTP_PATHS 16
enum {
	VAYU_TFTP_PATH_SERVED = 0,
	VAYU_TFTP_PATH_REFUSED,
	VAYU_TFTP_PATH_NOT_FOUND,
	VAYU_TFTP_PATH_WRITTEN,
};
static struct vayu_tftp_path {
	char path[VAYU_TFTP_PATH_MAX + 1];
	uint32_t count;
	uint8_t last;
} tftp_paths[VAYU_TFTP_PATHS];

/* ---- small parsing helpers ----------------------------------------------------------------------- */

/* The length of a NUL-terminated string inside `limit` bytes, or `limit` if there is no NUL. */
static size_t
vayu_wifi_strnlen(const char *s, size_t limit)
{
	size_t n = 0;

	while (n < limit && s[n] != 0) {
		n++;
	}
	return n;
}

/* A plain decimal, digits only (no sign, no space), at most `max`; false if it is not one. */
static bool
vayu_wifi_parse_uint(const char *s, uint64_t max, uint64_t *out)
{
	uint64_t value = 0;

	if (*s == 0) {
		return false;
	}
	for (; *s != 0; s++) {
		uint32_t digit;

		if (*s < '0' || *s > '9') {
			return false;
		}
		digit = (uint32_t)(*s - '0');
		if (value > (max - digit) / 10) {
			return false;
		}
		value = value * 10 + digit;
	}
	*out = value;
	return true;
}

/* ===================================================================================================
 * 1. pd-mapper: the SERVREG locator
 *
 * after ref/pdm_pd-mapper.c, ref/pdm_servreg_loc.c and ref/pdm_servreg_loc.qmi.
 * =================================================================================================== */

#define VAYU_PDM_NAME_LEN       64      /* SERVREG_NAME_LENGTH; the C string has the zero too */
#define VAYU_PDM_MAX_DOMAINS    32      /* the response's VAR_LEN_ARRAY */

struct vayu_pdm_domain_entry {
	char name[VAYU_PDM_NAME_LEN + 1];
	uint32_t instance_id;
	uint8_t service_data_valid;
	uint32_t service_data;
};

struct vayu_pdm_get_list_req {
	char name[VAYU_PDM_NAME_LEN + 1];
	uint8_t offset_valid;
	uint32_t offset;
};

struct vayu_pdm_get_list_resp {
	struct qmi_response_type_v01 result;
	uint8_t total_domains_valid;
	uint16_t total_domains;
	uint8_t db_revision_valid;
	uint16_t db_revision;
	uint8_t domain_list_valid;
	uint32_t domain_list_len;               /* a uint32_t in C; one byte on the wire (hakios_qmi.h) */
	struct vayu_pdm_domain_entry domain_list[VAYU_PDM_MAX_DOMAINS];
};

struct vayu_pdm_pfr_req {
	char service[VAYU_PDM_NAME_LEN + 1];
	char reason[256 + 1];
};

struct vayu_pdm_pfr_resp {
	struct qmi_response_type_v01 result;
};

static const struct qmi_elem_info vayu_pdm_domain_entry_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_PDM_NAME_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, name),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, instance_id),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, service_data_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.offset = offsetof(struct vayu_pdm_domain_entry, service_data),
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_get_list_req_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_PDM_NAME_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 1,
		.offset = offsetof(struct vayu_pdm_get_list_req, name),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_req, offset_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_req, offset),
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_get_list_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.tlv_type = 2,
		.offset = offsetof(struct vayu_pdm_get_list_resp, result),
		.ei_array = qmi_response_type_v01_ei,
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_resp, total_domains_valid),
	},
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_pdm_get_list_resp, total_domains),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x11,
		.offset = offsetof(struct vayu_pdm_get_list_resp, db_revision_valid),
	},
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.tlv_type = 0x11,
		.offset = offsetof(struct vayu_pdm_get_list_resp, db_revision),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_pdm_get_list_resp, domain_list_valid),
	},
	{
		.data_type = QMI_DATA_LEN,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_pdm_get_list_resp, domain_list_len),
	},
	{
		.data_type = QMI_STRUCT,
		.elem_len = VAYU_PDM_MAX_DOMAINS,
		.elem_size = sizeof(struct vayu_pdm_domain_entry),
		.array_type = VAR_LEN_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_pdm_get_list_resp, domain_list),
		.ei_array = vayu_pdm_domain_entry_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_pfr_req_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_PDM_NAME_LEN + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 1,
		.offset = offsetof(struct vayu_pdm_pfr_req, service),
	},
	{
		.data_type = QMI_STRING,
		.elem_len = 256 + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 2,
		.offset = offsetof(struct vayu_pdm_pfr_req, reason),
	},
	{}
};

static const struct qmi_elem_info vayu_pdm_pfr_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.tlv_type = 2,
		.offset = offsetof(struct vayu_pdm_pfr_resp, result),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

/* FACT: firmware/vayu/modem/*.jsn of the stock vayu modem partition (ref/pdm_pd-mapper.c:122-194);
 * service = provider/service, domain = soc/domain/subdomain, instance = qmi_instance_id. */
static const struct {
	const char *service;
	const char *domain;
	uint32_t instance;
} vayu_pdm_domains[] = {
	{ "tms/servreg", "msm/modem/root_pd", 180 },		/* modemr.jsn */
	{ "tms/pdr_enabled", "msm/modem/root_pd", 180 },	/* modemr.jsn */
	{ "gps/gps_service", "msm/modem/root_pd", 180 },	/* modemr.jsn */
	{ "kernel/elf_loader", "msm/modem/wlan_pd", 180 },	/* modemuw.jsn */
	{ "tms/servreg", "msm/modem/wlan_pd", 180 },		/* modemuw.jsn */
	{ "wlan/fw", "msm/modem/wlan_pd", 180 },		/* modemuw.jsn */
	{ "tms/servreg", "msm/adsp/root_pd", 74 },		/* adspr.jsn */
	{ "tms/servreg", "msm/adsp/audio_pd", 74 },		/* adspua.jsn */
	{ "avs/audio", "msm/adsp/audio_pd", 74 },		/* adspua.jsn */
	{ "tms/servreg", "msm/cdsp/root_pd", 76 },		/* cdspr.jsn */
	{ "tms/servreg", "msm/slpi/root_pd", 90 },		/* slpir.jsn */
	{ "tms/servreg", "msm/slpi/sensor_pd", 90 },		/* slpius.jsn */
};

#define VAYU_PDM_DOMAIN_COUNT   (sizeof(vayu_pdm_domains) / sizeof(vayu_pdm_domains[0]))

/*
 * A GET_DOMAIN_LIST request. The answer is filled as ref/pdm_pd-mapper.c's handle_get_domain_list():
 * every entry whose service is the one asked for, `offset` matching entries skipped, total_domains
 * is the whole number of matches (not only those in this answer).
 */
static void
pdm_receive(void *context __unused, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	const uint8_t *message = data;
	struct hakios_qrtr_port *port;
	uint16_t msg_id, transaction;
	bool stopping, log;

	lck_mtx_lock(&vayu_wifi_lock);
	stopping = wifi_stopping;
	port = pdm_port;
	if (stopping || port == NULL) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	pdm_busy++;
	lck_mtx_unlock(&vayu_wifi_lock);

	if (length < QMI_HEADER_SIZE) {
		goto out;
	}
	if (message[0] != QMI_REQUEST) {
		goto out;
	}
	msg_id = (uint16_t)(message[3] | (message[4] << 8));
	transaction = (uint16_t)(message[1] | (message[2] << 8));

	{
		/* the first four requests in hex as well (run 033841: two requests that did not decode) */
		static unsigned raw_said;

		if (raw_said++ < 4) {
			char hex[3 * 48 + 1];
			size_t n = length < 48 ? length : 48;

			for (size_t i = 0; i < n; i++) {
				snprintf(hex + 3 * i, 4, " %02x", message[i]);
			}
			hex[3 * n] = 0;
			say("vayu-wifi: pd-mapper: request of %zu bytes:%s%s\n", length, hex, length > n ? " ..." : "");
		}
	}
	if (msg_id == VAYU_PDM_GET_DOMAIN_LIST) {
		struct vayu_pdm_get_list_req req = { 0 };
		struct vayu_pdm_get_list_resp resp = { 0 };
		unsigned int total = 0, first = 0, skip = 0, matching = 0, listed = 0;
		int ret;

		ret = hakios_qmi_decode(message, length, vayu_pdm_get_list_req_ei, &req);
		if (ret < 0) {
			req.name[0] = 0;
			resp.result.result = QMI_RESULT_FAILURE_V01;
			resp.result.error = QMI_ERR_MALFORMED_MSG_V01;
		} else {
			req.name[sizeof(req.name) - 1] = 0;
			for (unsigned int i = 0; i < VAYU_PDM_DOMAIN_COUNT; i++) {
				if (strcmp(vayu_pdm_domains[i].service, req.name) == 0) {
					if (total == 0) {
						first = i;
					}
					total++;
				}
			}
			resp.result.result = QMI_RESULT_SUCCESS_V01;
			resp.total_domains_valid = 1;
			resp.total_domains = (uint16_t)total;
			resp.db_revision_valid = 1;
			resp.db_revision = 1;
			skip = req.offset_valid ? req.offset : 0;
			for (unsigned int i = 0; i < VAYU_PDM_DOMAIN_COUNT; i++) {
				struct vayu_pdm_domain_entry *entry;

				if (strcmp(vayu_pdm_domains[i].service, req.name) != 0) {
					continue;
				}
				if (matching++ < skip) {
					continue;
				}
				if (listed >= VAYU_PDM_MAX_DOMAINS) {
					break;
				}
				entry = &resp.domain_list[listed++];
				snprintf(entry->name, sizeof(entry->name), "%s", vayu_pdm_domains[i].domain);
				entry->instance_id = vayu_pdm_domains[i].instance;
				entry->service_data_valid = 0;
				entry->service_data = 0;
			}
			if (listed != 0) {
				resp.domain_list_valid = 1;
				resp.domain_list_len = listed;
			}
		}

		lck_mtx_lock(&vayu_wifi_lock);
		pdm_requests++;
		log = pdm_log_lines < VAYU_PDM_LOG_MAX;
		if (log) {
			pdm_log_lines++;
		}
		lck_mtx_unlock(&vayu_wifi_lock);

		if (log) {
			char extra[96] = "";

			if (total != 0) {
				snprintf(extra, sizeof(extra), " first %s", vayu_pdm_domains[first].domain);
			}
			say("vayu-wifi: pd-mapper: node %u port %u asks for \"%s\": %u domain(s)%s\n", node,
			    from_port, req.name[0] != 0 ? req.name : "?", total, extra);
		}

		ret = hakios_qmi_encode(QMI_RESPONSE, VAYU_PDM_GET_DOMAIN_LIST, transaction,
		    vayu_pdm_get_list_resp_ei, &resp, pdm_answer, sizeof(pdm_answer));
		if (ret > 0) {
			vayu_qrtr_ops.send(port, node, from_port, pdm_answer, (size_t)ret);
		}
	} else if (msg_id == VAYU_PDM_PFR) {
		struct vayu_pdm_pfr_req req = { 0 };
		struct vayu_pdm_pfr_resp resp = { 0 };
		int ret;

		ret = hakios_qmi_decode(message, length, vayu_pdm_pfr_req_ei, &req);
		if (ret < 0) {
			req.service[0] = 0;
			req.reason[0] = 0;
			resp.result.result = QMI_RESULT_FAILURE_V01;
			resp.result.error = QMI_ERR_MALFORMED_MSG_V01;
		} else {
			req.service[sizeof(req.service) - 1] = 0;
			req.reason[sizeof(req.reason) - 1] = 0;
			resp.result.result = QMI_RESULT_SUCCESS_V01;
		}

		lck_mtx_lock(&vayu_wifi_lock);
		pdm_requests++;
		/* W17: only a real PFR (0x24) gates CE; W15-A set this in GetDomainList (0x21). */
		vayu_pdm_pfr_seen = true;
		log = pdm_log_lines < VAYU_PDM_LOG_MAX;
		if (log) {
			pdm_log_lines++;
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (log) {
			say("vayu-wifi: pd-mapper: pfr: service \"%s\" reason \"%s\" (wlfw stage \"%s\")\n",
			    req.service, req.reason, wlfw_stage);
		}
		/*
		 * vayu_wifi_census=1: the modem's own error log lives in an SMEM item (the modem
		 * firmware's err_smem_log.c, "SMEM alloc for ERR_LOG_SMEM_ITEM failed"); its id is
		 * not in the device tree, so list every item of the modem's partition with its size
		 * and the first text in it.  Read-only, and only after a PFR: the W7 crash is a
		 * user-PD crash, so the modem's crash-reason item 421 stays empty (it did).
		 */
		if (wifi_census != 0) {
			say("vayu-wifi: census: after the PFR of \"%s\": the modem's SMEM items\n", req.service);
			vayu_smem_census("vayu-wifi", (uint16_t)wifi_smp2p_host);
			say("vayu-wifi: census: done\n");
		}

		ret = hakios_qmi_encode(QMI_RESPONSE, VAYU_PDM_PFR, transaction, vayu_pdm_pfr_resp_ei,
		    &resp, pdm_answer, sizeof(pdm_answer));
		if (ret > 0) {
			vayu_qrtr_ops.send(port, node, from_port, pdm_answer, (size_t)ret);
		}
	} else {
		lck_mtx_lock(&vayu_wifi_lock);
		log = !pdm_unknown_logged;
		if (log) {
			pdm_unknown_logged = true;
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (log) {
			say("vayu-wifi: pd-mapper: node %u port %u sends message id %#x: not answered\n", node,
			    from_port, (unsigned int)msg_id);
		}
	}

out:
	lck_mtx_lock(&vayu_wifi_lock);
	pdm_busy--;
	lck_mtx_unlock(&vayu_wifi_lock);
}

/* ===================================================================================================
 * 2. tftp server: read-only
 *
 * after ref/tqftpserv.c (RRQ path, options, OACK, DATA, ACK) and ref/translate.c (paths).
 * =================================================================================================== */

#define VAYU_TFTP_BLKSIZE_DEFAULT       512
#define VAYU_TFTP_MIN_BLKSIZE           8
#define VAYU_TFTP_MAX_BLKSIZE           65464   /* RFC 2348, as ref/tqftpserv.c:23-24 */
#define VAYU_TFTP_MIN_WSIZE             1
#define VAYU_TFTP_MAX_WSIZE             65535   /* RFC 7440, as ref/tqftpserv.c:27-28 */
#define VAYU_TFTP_MAX_RSIZE             (100ULL * 1024 * 1024)
#define VAYU_TFTP_MIN_TIMEOUTMS         1
#define VAYU_TFTP_MAX_TIMEOUTMS         255000
#define VAYU_TFTP_NAME_MAX              88      /* a nested name under fw/: "/fw/" + 88 < the 96 of hakios_image.c */
#define VAYU_TFTP_COMPONENT_MAX         63      /* one component, as a file name on the phone's firmware_mnt */
#define VAYU_TFTP_PACKET_MAX            8192    /* hakios_qrtr.h: a send is up to 8192 bytes */
#define VAYU_TFTP_DATA_MAX              (VAYU_TFTP_PACKET_MAX - 4)
#define VAYU_TFTP_IDLE_NS               (10ULL * NSEC_PER_SEC)

enum {
	OP_RRQ = 1,
	OP_WRQ,
	OP_DATA,
	OP_ACK,
	OP_ERROR,
	OP_OACK,
};

/* RFC 1350 error codes used here (ref/tqftpserv.c:45-57). */
#define TFTP_ERROR_ENOENT       1
#define TFTP_ERROR_EACCESS      2
#define TFTP_ERROR_ENOSPC       3
#define TFTP_ERROR_EBADOP       4
#define TFTP_ERROR_EOPTNEG      8

struct vayu_tftp_options {
	uint32_t blksize;
	uint32_t timeoutms;
	uint32_t wsize;
	uint32_t rsize;
	uint64_t seek;
	bool tsize;
	bool seek_given;
};

struct vayu_tftp_xfer {
	bool used;
	uint32_t busy;                          /* a callback or the first send is using it */
	struct hakios_qrtr_port *port;         /* the transfer's own port, NULL while opening */
	uint32_t node, client_port;
	const uint8_t *file;                    /* the boot image's memory: never freed */
	uint64_t size;
	uint64_t start;                         /* seek, clamped to size */
	uint64_t limit;                         /* bytes to send: rsize or the rest of the file */
	bool knows_length;                      /* rsize is the limit: the client knows the byte count */
	char name[VAYU_TFTP_PATH_MAX + 1];
	uint32_t blksize, wsize;
	bool ending;                            /* the last DATA packet has been sent */
	bool want_zero;                         /* ... after the ACK of this full block, send a zero one */
	bool sent_zero;
	uint64_t sent;                          /* file bytes in DATA packets */
	uint16_t next_block;
	uint16_t expected_ack;
	uint32_t blocks;                        /* DATA packets, the zero one included */
	uint64_t last_ns;
	/* set by handle_reader(), acted on once the callback's busy is given back */
	bool close_now;
	bool client_error;
	uint32_t client_error_code;
	char client_error_text[64];
};

static struct vayu_tftp_xfer tftp_xfer[VAYU_TFTP_XFERS];
static uint8_t tftp_packet[4 + VAYU_TFTP_DATA_MAX];
static uint8_t tftp_oack[512];

static bool
tftp_log_take(void)                             /* with vayu_wifi_lock held */
{
	if (tftp_log_lines >= VAYU_TFTP_LOG_MAX) {
		return false;
	}
	tftp_log_lines++;
	return true;
}

static uint16_t
tftp_get_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void
tftp_put_be16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static int
tftp_send_error(struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, uint16_t code,
    const char *text)
{
	size_t length = strlen(text) + 1;

	if (length > VAYU_TFTP_DATA_MAX) {
		length = VAYU_TFTP_DATA_MAX;
	}
	tftp_put_be16(tftp_packet, OP_ERROR);
	tftp_put_be16(tftp_packet + 2, code);
	memcpy(tftp_packet + 4, text, length);
	if (vayu_qrtr_ops.send(port, node, to_port, tftp_packet, 4 + length) != 0) {
		return -1;
	}
	return 0;
}

/* after ref/tqftpserv.c:167-205: one DATA packet, offset into the file, at most blksize bytes. */
/* A bare ACK; used by the write probe, whose payload is never stored. */
static int
tftp_send_ack(struct hakios_qrtr_port *port, uint32_t node, uint32_t to_port, uint16_t block)
{
	uint8_t ack[4];

	tftp_put_be16(ack, OP_ACK);
	tftp_put_be16(ack + 2, block);
	return vayu_qrtr_ops.send(port, node, to_port, ack, sizeof(ack));
}

static int
tftp_send_data(struct vayu_tftp_xfer *x, uint16_t block, uint64_t offset, uint32_t length)
{
	if (length > x->blksize || offset > x->size || length > x->size - offset) {
		return -1;
	}
	tftp_put_be16(tftp_packet, OP_DATA);
	tftp_put_be16(tftp_packet + 2, block);
	if (length != 0) {
		memcpy(tftp_packet + 4, x->file + offset, length);
	}
	if (vayu_qrtr_ops.send(x->port, x->node, x->client_port, tftp_packet, 4 + length) != 0) {
		return -1;
	}
	return 0;
}

/* One "name" "value" pair of an OACK; false if it does not fit. */
static bool
tftp_put_option(char **p, char *end, const char *name, uint64_t value)
{
	size_t length = strlen(name) + 1;
	char number[24];
	int n;

	if ((size_t)(end - *p) < length + sizeof(number)) {
		return false;
	}
	memcpy(*p, name, length);
	*p += length;
	n = snprintf(*p, (size_t)(end - *p), "%llu", (unsigned long long)value);
	if (n < 0 || (size_t)n >= (size_t)(end - *p)) {
		return false;
	}
	*p += n + 1;
	return true;
}

/* after ref/tqftpserv.c:218-309: the options asked for, in its order. */
static int
tftp_send_oack(struct vayu_tftp_xfer *x, const struct vayu_tftp_options *options)
{
	char *p = (char *)tftp_oack;
	char *end = (char *)tftp_oack + sizeof(tftp_oack);

	*p++ = 0;
	*p++ = OP_OACK;
	if (!tftp_put_option(&p, end, "blksize", x->blksize) ||
	    !tftp_put_option(&p, end, "timeoutms", options->timeoutms) ||
	    (options->tsize && !tftp_put_option(&p, end, "tsize", x->size)) ||
	    !tftp_put_option(&p, end, "wsize", options->wsize) ||
	    (options->rsize != 0 && !tftp_put_option(&p, end, "rsize", options->rsize)) ||
	    (options->seek_given && !tftp_put_option(&p, end, "seek", options->seek))) {
		return -1;
	}
	if (vayu_qrtr_ops.send(x->port, x->node, x->client_port, tftp_oack,
	    (size_t)(p - (char *)tftp_oack)) != 0) {
		return -1;
	}
	return 0;
}

/*
 * after ref/tqftpserv.c:311-415. An option value must be a NUL-terminated string inside the
 * datagram and a plain decimal in its range; an unknown option is ignored, as there.
 */
static int
parse_options(const char *p, const char *end, struct vayu_tftp_options *options)
{
	while (p < end) {
		const char *option = p;
		const char *value;
		size_t option_len, value_len;
		uint64_t parsed;

		option_len = vayu_wifi_strnlen(option, (size_t)(end - p));
		if (option_len == (size_t)(end - p)) {
			return -1;
		}
		p += option_len + 1;
		if (p >= end) {
			return -1;
		}
		value = p;
		value_len = vayu_wifi_strnlen(value, (size_t)(end - p));
		if (value_len == (size_t)(end - p)) {
			return -1;
		}
		p += value_len + 1;

		if (strcmp(option, "blksize") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_BLKSIZE, &parsed) ||
			    parsed < VAYU_TFTP_MIN_BLKSIZE) {
				return -1;
			}
			if (parsed > VAYU_TFTP_DATA_MAX) {
				parsed = VAYU_TFTP_DATA_MAX;    /* our QRTR datagram is at most 8192 bytes */
			}
			options->blksize = (uint32_t)parsed;
		} else if (strcmp(option, "timeoutms") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_TIMEOUTMS, &parsed) ||
			    parsed < VAYU_TFTP_MIN_TIMEOUTMS) {
				return -1;
			}
			options->timeoutms = (uint32_t)parsed;
		} else if (strcmp(option, "tsize") == 0) {
			if (!vayu_wifi_parse_uint(value, 0xffffffffffffffffULL, &parsed)) {
				return -1;
			}
			options->tsize = true;
		} else if (strcmp(option, "rsize") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_RSIZE, &parsed) || parsed < 1) {
				return -1;
			}
			options->rsize = (uint32_t)parsed;
		} else if (strcmp(option, "wsize") == 0) {
			if (!vayu_wifi_parse_uint(value, VAYU_TFTP_MAX_WSIZE, &parsed) || parsed < VAYU_TFTP_MIN_WSIZE) {
				return -1;
			}
			options->wsize = (uint32_t)parsed;
		} else if (strcmp(option, "seek") == 0) {
			if (!vayu_wifi_parse_uint(value, 0xffffffffffffffffULL, &parsed)) {
				return -1;
			}
			options->seek = parsed;
			options->seek_given = true;
		} else {
			/* an unknown option is ignored, as ref/tqftpserv.c:409-411 */
		}
	}
	return 0;
}

/*
 * after ref/translate.c:255-277, read-only part: one of the three prefixes, then a path.  The path
 * may be nested, because the modem's MCFG loader asks for
 * /readonly/firmware/image/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig and .../mcfg_sw/mbn_sw.dig, and
 * on a phone tqftpserv serves the whole /vendor/firmware_mnt tree under that prefix.  Every
 * component must be a plain name: no empty one (so no "//", no leading or trailing "/"), no "." or
 * "..", no control characters.  The tree that answers is the boot image's fw/, where "/" in the
 * path is "/" in the APFS lookup (hakios_image.c apfs_file; apfs_path_lookup walks components).
 * Returns 0 and the name, or -1 and a short reason.
 */
static int
tftp_relative(const char *path, const char *prefix, char *name, size_t name_size, const char **why)
{
	size_t prefix_len = strlen(prefix);
	const char *rest, *component;
	size_t len;

	if (strncmp(path, prefix, prefix_len) != 0) {
		return -1;
	}
	rest = path + prefix_len;
	len = strlen(rest);
	if (len == 0) {
		*why = "no file name";
		return -1;
	}
	if (len > VAYU_TFTP_NAME_MAX || len + 1 > name_size) {
		*why = "name too long";
		return -1;
	}
	if (rest[len - 1] == '/') {
		*why = "empty path component";
		return -1;
	}
	for (const char *c = rest; *c != 0; c++) {
		if ((unsigned char)*c < 0x20 || *c == 0x7f) {
			*why = "control character in the name";
			return -1;
		}
	}
	component = rest;
	while (*component != 0) {
		const char *slash = strchr(component, '/');
		size_t component_len = slash != NULL ? (size_t)(slash - component) : strlen(component);

		if (component_len == 0) {
			*why = "empty path component";
			return -1;
		}
		if ((component_len == 1 && component[0] == '.') ||
		    (component_len == 2 && component[0] == '.' && component[1] == '.')) {
			*why = "path component \".\" or \"..\"";
			return -1;
		}
		if (component_len > VAYU_TFTP_COMPONENT_MAX) {
			*why = "path component too long";
			return -1;
		}
		component = slash != NULL ? slash + 1 : component + component_len;
	}
	memcpy(name, rest, len + 1);
	return 0;
}

static int
tftp_translate(const char *path, char *name, size_t name_size, const char **why)
{
	static const char *const prefixes[] = {
		"/readonly/firmware/image/",
		"/readonly/vendor/firmware/",
		"/readonly/vendor/firmware_mnt/image/",
	};

	for (unsigned int i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0) {
			return tftp_relative(path, prefixes[i], name, name_size, why);
		}
	}
	*why = "path not served";
	return -1;
}

/* ---- the /readwrite/ RAM overlay and the request census ------------------------------------------ */

static struct vayu_tftp_ram_file *
tftp_ram_find(const char *path)                         /* with vayu_wifi_lock held */
{
	for (unsigned int i = 0; i < VAYU_TFTP_RAM_FILES; i++) {
		if (tftp_ram[i].used && strcmp(tftp_ram[i].path, path) == 0) {
			return &tftp_ram[i];
		}
	}
	return NULL;
}

/* The slot for this path: one it already has (a rewrite starts from zero), or a free one. */
static struct vayu_tftp_ram_file *
tftp_ram_open(const char *path)                         /* with vayu_wifi_lock held */
{
	struct vayu_tftp_ram_file *file = tftp_ram_find(path);

	if (file != NULL) {
		file->size = 0;
		return file;
	}
	if (strlen(path) > VAYU_TFTP_PATH_MAX) {
		return NULL;
	}
	for (unsigned int i = 0; i < VAYU_TFTP_RAM_FILES; i++) {
		if (!tftp_ram[i].used) {
			memset(&tftp_ram[i], 0, sizeof(tftp_ram[i]));
			tftp_ram[i].used = true;
			tftp_ram[i].limit = VAYU_TFTP_RAM_MAX;
			snprintf(tftp_ram[i].path, sizeof(tftp_ram[i].path), "%s", path);
			return &tftp_ram[i];
		}
	}
	return NULL;
}

static bool
tftp_path_note(const char *path, uint8_t state)         /* with vayu_wifi_lock held: true on first sight */
{
	unsigned int free_slot = VAYU_TFTP_PATHS, distinct = 0;

	for (unsigned int i = 0; i < VAYU_TFTP_PATHS; i++) {
		if (tftp_paths[i].count != 0) {
			distinct++;
			if (strcmp(tftp_paths[i].path, path) == 0) {
				tftp_paths[i].count++;
				tftp_paths[i].last = state;
				return false;
			}
		} else if (free_slot == VAYU_TFTP_PATHS) {
			free_slot = i;
		}
	}
	if (free_slot == VAYU_TFTP_PATHS || distinct == VAYU_TFTP_PATHS || strlen(path) > VAYU_TFTP_PATH_MAX) {
		return false;                   /* full: the per-request line still names it */
	}
	snprintf(tftp_paths[free_slot].path, sizeof(tftp_paths[free_slot].path), "%s", path);
	tftp_paths[free_slot].count = 1;
	tftp_paths[free_slot].last = state;
	return true;
}

/* The list of paths, in the order the modem first asked for them; callbacks must be stopped. */
static void
tftp_path_dump(void)
{
	static const char *const what[] = { "served", "refused", "not found", "written" };
	unsigned int distinct = 0;

	for (unsigned int i = 0; i < VAYU_TFTP_PATHS; i++) {
		if (tftp_paths[i].count != 0) {
			distinct++;
		}
	}
	if (distinct == 0) {
		say("vayu-wifi: tftp: the modem asked for no path\n");
		return;
	}
	say("vayu-wifi: tftp: the modem asked for %u distinct path(s):\n", distinct);
	for (unsigned int i = 0; i < VAYU_TFTP_PATHS; i++) {
		if (tftp_paths[i].count != 0) {
			say("vayu-wifi: tftp:   \"%s\" x%u: %s\n", tftp_paths[i].path, tftp_paths[i].count,
			    what[tftp_paths[i].last < 4 ? tftp_paths[i].last : 0]);
		}
	}
}

/*
 * after ref/tqftpserv.c:733-814 (handle_reader), with the rsize window understood as "when rsize is
 * the limit, stop once that many bytes have been acknowledged". ref/tqftpserv.c's loop uses
 * `(block + 1) * blksize > rsize` unguarded, which with rsize == 0 stops after one block and makes
 * wsize useless; here that test only applies when rsize is the limit (knows_length). If rsize is
 * larger than the rest of the file, the file's own end is marked the normal way (a short block or a
 * zero-length one).
 *
 * Sends at most wsize DATA packets and leaves expected_ack at the last one. A full block that ends
 * the data (and no rsize, so the client does not know the length) is followed by a zero-length block
 * on the next ACK; a short block ends the transfer by itself.
 */static int
tftp_send_window(struct vayu_tftp_xfer *x)
{
	uint64_t sent = x->sent;
	uint64_t limit = x->limit;
	uint16_t block = x->next_block;
	uint32_t blocks = x->blocks;
	uint32_t window = x->wsize ? x->wsize : 1;
	bool ending = x->ending;
	bool want_zero = x->want_zero;
	bool sent_zero = x->sent_zero;
	int rc = 0;

	for (uint32_t i = 0; i < window; i++) {
		uint32_t length;
		uint64_t offset;

		if (sent >= limit) {
			if (blocks == 0 || (want_zero && !sent_zero)) {
				length = 0;
			} else {
				break;
			}
		} else {
			uint64_t left = limit - sent;

			length = left > x->blksize ? x->blksize : (uint32_t)left;
		}
		offset = x->start + sent;
		rc = tftp_send_data(x, block, offset, length);
		if (rc < 0) {
			break;
		}
		blocks++;
		x->expected_ack = block;
		block++;
		if (length == 0) {
			sent_zero = true;
			want_zero = false;
			ending = true;
			break;
		}
		sent += length;
		if (length < x->blksize) {
			ending = true;
			break;
		}
		if (sent >= limit) {
			if (x->knows_length) {
				ending = true;          /* the client knows rsize: no zero block */
				break;
			}
			want_zero = true;               /* the file ended on a block boundary */
		}
	}

	x->sent = sent;
	x->next_block = block;
	x->blocks = blocks;
	x->ending = ending;
	x->want_zero = want_zero;
	x->sent_zero = sent_zero;
	return rc;
}

static void
tftp_xfer_close(struct vayu_tftp_xfer *x, bool client_error, uint32_t error_code, const char *error_text)
{
	struct hakios_qrtr_port *port;
	char name[VAYU_TFTP_PATH_MAX + 1];
	uint64_t sent;
	uint32_t blocks;
	bool log;

	lck_mtx_lock(&vayu_wifi_lock);
	if (!x->used) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	if (x->busy != 0) {
		/* A callback of this port has it (a sweep can race with one); it will end it. */
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	port = x->port;
	x->port = NULL;
	x->used = false;
	memcpy(name, x->name, sizeof(name));
	sent = x->sent;
	blocks = x->blocks;
	tftp_bytes += sent;
	log = tftp_log_take();
	lck_mtx_unlock(&vayu_wifi_lock);

	if (port != NULL) {
		vayu_qrtr_ops.close(port);
	}
	if (log) {
		if (client_error) {
			say("vayu-wifi: tftp: \"%s\": sent %llu bytes in %u blocks (client error %u: %s)\n",
			    name, (unsigned long long)sent, blocks, error_code,
			    error_text != NULL ? error_text : "");
		} else {
			say("vayu-wifi: tftp: \"%s\": sent %llu bytes in %u blocks\n", name,
			    (unsigned long long)sent, blocks);
		}
	}
}

/* A transfer that has seen nothing for 10 s; called from the server port, no lock held. */
static void
tftp_sweep(void)
{
	uint64_t now;

	clock_get_uptime(&now);
	for (unsigned int i = 0; i < VAYU_TFTP_XFERS; i++) {
		struct vayu_tftp_xfer *x = &tftp_xfer[i];
		bool stale = false;

		lck_mtx_lock(&vayu_wifi_lock);
		if (x->used && x->busy == 0 && x->port != NULL && now > x->last_ns &&
		    now - x->last_ns > VAYU_TFTP_IDLE_NS) {
			stale = true;
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (stale) {
			tftp_xfer_close(x, false, 0, NULL);
		}
	}
}

/* The ACK/ERROR of one read transfer; called with x->busy held. */
static void
handle_reader(struct vayu_tftp_xfer *x, const uint8_t *data, size_t length)
{
	uint16_t opcode, block;
	size_t text_len;
	char text[64];

	if (length < 4) {
		x->close_now = true;
		return;
	}
	opcode = tftp_get_be16(data);
	if (opcode == OP_ERROR) {
		text_len = length - 4;
		if (text_len >= sizeof(text)) {
			text_len = sizeof(text) - 1;
		}
		memcpy(text, data + 4, text_len);
		text[text_len] = 0;
		x->close_now = true;
		x->client_error = true;
		x->client_error_code = tftp_get_be16(data + 2);
		snprintf(x->client_error_text, sizeof(x->client_error_text), "%s", text);
		return;
	}
	if (opcode != OP_ACK) {
		tftp_send_error(x->port, x->node, x->client_port, TFTP_ERROR_EBADOP, "Expected ACK opcode");
		x->close_now = true;
		return;
	}
	block = tftp_get_be16(data + 2);
	if (block != x->expected_ack) {
		return;                                 /* a duplicate or an old ACK: ignore it */
	}
	if (x->ending && !x->want_zero) {
		x->close_now = true;
		return;
	}
	if (tftp_send_window(x) < 0) {
		x->close_now = true;
	}
}

/* The transfer's own port; everything of that transfer is sent from it. */
static void
transfer_receive(void *context, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	struct vayu_tftp_xfer *x = context;
	bool close_now = false, client_error = false;
	uint32_t error_code = 0;
	char error_text[64] = "";

	lck_mtx_lock(&vayu_wifi_lock);
	if (wifi_stopping || !x->used || x->port == NULL || node != x->node || from_port != x->client_port) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	x->busy++;
	x->last_ns = 0;
	clock_get_uptime(&x->last_ns);
	lck_mtx_unlock(&vayu_wifi_lock);

	handle_reader(x, data, length);

	lck_mtx_lock(&vayu_wifi_lock);
	close_now = x->close_now;
	client_error = x->client_error;
	error_code = x->client_error_code;
	snprintf(error_text, sizeof(error_text), "%s", x->client_error_text);
	x->busy--;
	lck_mtx_unlock(&vayu_wifi_lock);

	if (close_now) {
		tftp_xfer_close(x, client_error, error_code, error_text);
	}
}

/*
 * A WRQ is answered by this server itself (no transfer port): the DATA blocks that follow come to
 * the same node and port, and that pair is the only thing that ties them to this file.
 *
 * Only /readwrite/ is writable.  On a phone tqftpserv's translate.c maps exactly that prefix to a
 * directory of rmtfs -- /readonly/ is a firmware tree that nobody writes.  So a WRQ anywhere else
 * is refused with ERROR 2 and a line that says which node, which port and which path, and a WRQ
 * under /readwrite/ is accepted into the RAM slot of that path.  Nothing is opened or written on
 * any storage.
 */
static void
tftp_handle_wrq(struct hakios_qrtr_port *port, uint32_t node, uint32_t from_port, const uint8_t *data,
    size_t length)
{
	const char *p = (const char *)data + 2;
	const char *end = (const char *)data + length;
	const char *filename, *why = "";
	size_t filename_len, shown;
	bool well_formed = true;
	struct vayu_tftp_options options = {
		.blksize = VAYU_TFTP_BLKSIZE_DEFAULT,
		.timeoutms = 1000,
		.wsize = 1,
		.rsize = 0,
		.seek = 0,
		.tsize = false,
		.seek_given = false,
	};
	struct vayu_tftp_ram_file *file = NULL;
	char path[VAYU_TFTP_PATH_MAX + 1] = "";
	const char *refusal = NULL;
	uint16_t refusal_code = TFTP_ERROR_EACCESS;
	bool accepted = false, first = false, octet = true, log;

	filename = p;
	filename_len = vayu_wifi_strnlen(filename, (size_t)(end - p));
	if (filename_len == 0 || filename_len == (size_t)(end - p)) {
		filename = "<malformed>";
		filename_len = strlen(filename);
		well_formed = false;
	}
	shown = filename_len > 100 ? 100 : filename_len;

	/* "octet", then the options: the block size decides where a block goes and which one ends it. */
	p = well_formed ? filename + filename_len + 1 : end;
	if (p < end) {
		const char *mode = p;
		size_t mode_len = vayu_wifi_strnlen(mode, (size_t)(end - p));

		if (mode_len != 0 && mode_len != (size_t)(end - p)) {
			p = mode + mode_len + 1;
			octet = strcasecmp(mode, "octet") == 0;
			if (octet && p < end) {
				(void)parse_options(p, end, &options);   /* a bad option leaves the defaults */
			}
		}
	}

	if (!octet) {
		refusal = "mode is not octet";
	} else if (filename_len < strlen("/readwrite/") ||
	    strncmp(filename, "/readwrite/", strlen("/readwrite/")) != 0) {
		refusal = "not under /readwrite";
	} else if (tftp_relative(filename, "/readwrite/", path, sizeof(path), &why) != 0) {
		refusal = why;
	}

	lck_mtx_lock(&vayu_wifi_lock);
	tftp_writes++;
	if (refusal == NULL) {
		file = tftp_ram_open(path);
		if (file == NULL) {
			refusal = "no RAM slot left";
		}
	}
	if (file != NULL) {
		file->writing = true;
		file->written = 0;
		file->blocks = 0;
		file->expected = 1;
		file->blksize = options.blksize;
		file->node = node;
		file->port = from_port;
		accepted = true;
	} else {
		tftp_write_refused++;
		if (refusal != NULL && strcmp(refusal, "mode is not octet") == 0) {
			refusal_code = TFTP_ERROR_EBADOP;
		} else if (refusal == NULL || strcmp(refusal, "not under /readwrite") != 0) {
			refusal_code = TFTP_ERROR_ENOSPC;
		}
	}
	log = tftp_log_take();
	first = tftp_path_note(filename_len <= VAYU_TFTP_PATH_MAX ? filename : "<path too long>",
	    accepted ? VAYU_TFTP_PATH_WRITTEN : VAYU_TFTP_PATH_REFUSED);
	lck_mtx_unlock(&vayu_wifi_lock);

	if (log) {
		if (accepted) {
			say("vayu-wifi: tftp: node %u port %u writes \"%.*s\": ACCEPTED into RAM "
			    "(blksize %u, max %u bytes)\n", node, from_port, (int)shown, filename,
			    file->blksize, VAYU_TFTP_RAM_MAX);
		} else {
			say("vayu-wifi: tftp: node %u port %u writes \"%.*s\": REFUSED (%s)\n", node,
			    from_port, (int)shown, filename, refusal);
		}
		if (first) {
			say("vayu-wifi: tftp: new path \"%.*s\"\n", (int)shown, filename);
		}
	}
	if (!accepted) {
		tftp_send_error(port, node, from_port, refusal_code, refusal != NULL ? refusal : "refused");
		return;
	}
	(void)tftp_send_ack(port, node, from_port, 0);
}

/*
 * after ref/tqftpserv.c:417-579 (handle_rrq and parse_options). The file is looked up in the boot
 * image's fw/ directory; nothing is opened on any storage.
 */
static void
tftp_handle_rrq(struct hakios_qrtr_port *port, uint32_t node, uint32_t from_port, const uint8_t *data,
    size_t length)
{
	const char *p = (const char *)data + 2;
	const char *end = (const char *)data + length;
	const char *filename, *mode;
	size_t filename_len, mode_len, shown;
	struct vayu_tftp_options options = {
		.blksize = VAYU_TFTP_BLKSIZE_DEFAULT,
		.timeoutms = 1000,
		.wsize = 1,
		.rsize = 0,
		.seek = 0,
		.tsize = false,
		.seek_given = false,
	};
	struct vayu_tftp_xfer *x = NULL;
	struct hakios_qrtr_port *transfer_port = NULL;
	const uint8_t *file = NULL;
	uint64_t size = 0;
	char name[VAYU_TFTP_PATH_MAX + 1] = "";
	char result[80] = "NOT SERVED";
	const char *why = "";
	const char *failure = "too many";
	bool have_options, bad_options = false, log, first = false, found = false, reserved = false,
	    transfer_ready = false;
	int rc;

	filename = p;
	filename_len = vayu_wifi_strnlen(filename, (size_t)(end - p));
	if (filename_len == 0 || filename_len == (size_t)(end - p)) {
		return;                                 /* no NUL inside the datagram: not an RRQ we answer */
	}
	p += filename_len + 1;
	if (p >= end) {
		return;
	}
	mode = p;
	mode_len = vayu_wifi_strnlen(mode, (size_t)(end - p));
	if (mode_len == 0 || mode_len == (size_t)(end - p)) {
		return;
	}
	p += mode_len + 1;
	if (strcasecmp(mode, "octet") != 0) {
		lck_mtx_lock(&vayu_wifi_lock);
		tftp_reads++;
		log = tftp_log_take();
		lck_mtx_unlock(&vayu_wifi_lock);
		if (log) {
			say("vayu-wifi: tftp: node %u port %u reads \"%.*s\" (blksize %u tsize %s rsize %u seek %u "
			    "wsize %u): NOT SERVED (mode %s)\n", node, from_port, (int)filename_len, filename,
			    options.blksize, options.tsize ? "yes" : "no", options.rsize, 0, options.wsize,
			    mode);
		}
		tftp_send_error(port, node, from_port, TFTP_ERROR_EBADOP, "Only octet mode supported");
		return;
	}
	have_options = p < end;
	if (have_options && parse_options(p, end, &options) < 0) {
		bad_options = true;
	}

	if (!bad_options) {
		bool ram = false;

		if (filename_len >= strlen("/readwrite/") &&
		    strncmp(filename, "/readwrite/", strlen("/readwrite/")) == 0) {
			/*
			 * The RAM overlay: what the modem wrote here is served back, as a phone's
			 * tqftpserv serves it from rmtfs.  A path that was never written is a plain
			 * ENOENT, not "not served": the prefix is ours, only this file is missing.
			 */
			struct vayu_tftp_ram_file *slot;

			lck_mtx_lock(&vayu_wifi_lock);
			slot = tftp_ram_find(filename);
			if (slot != NULL && !slot->writing) {
				file = tftp_ram_data[slot - tftp_ram];
				size = slot->size;
				snprintf(name, sizeof(name), "%s", slot->path);
				ram = true;
			}
			lck_mtx_unlock(&vayu_wifi_lock);
			if (ram) {
				found = true;
			} else {
				found = false;
				why = slot != NULL ? "readwrite file is being written" :
				    "readwrite file was never written";
			}
		} else if (tftp_translate(filename, name, sizeof(name), &why) != 0) {
			found = false;
		} else {
			file = hakios_image_file("fw", name, &size, NULL);
			found = file != NULL;
		}
	}

	if (bad_options) {
		snprintf(result, sizeof(result), "NOT SERVED (%s)", "bad options");
	} else if (file == NULL) {
		if (why[0] != 0) {
			snprintf(result, sizeof(result), "NOT SERVED (%s)", why);
		} else {
			snprintf(result, sizeof(result), "NOT FOUND");
		}
	} else if (found) {
		snprintf(result, sizeof(result), "%llu bytes", (unsigned long long)size);
	}

	if (!bad_options && found) {
		lck_mtx_lock(&vayu_wifi_lock);
		if (!wifi_stopping && tftp_port != NULL) {
			for (unsigned int i = 0; i < VAYU_TFTP_XFERS; i++) {
				if (!tftp_xfer[i].used) {
					x = &tftp_xfer[i];
					memset(x, 0, sizeof(*x));
					x->used = true;
					x->busy = 1;            /* the open and the first send, below */
					x->node = node;
					x->client_port = from_port;
					x->file = file;
					x->size = size;
					x->start = options.seek < size ? options.seek : size;
					x->limit = size > x->start ? size - x->start : 0;
					if (options.rsize != 0 && options.rsize <= x->limit) {
						x->limit = options.rsize;
						x->knows_length = true;
					}
					snprintf(x->name, sizeof(x->name), "%s", name);
					x->blksize = options.blksize;
					x->wsize = options.wsize;
					x->next_block = 1;
					x->expected_ack = 0;
					clock_get_uptime(&x->last_ns);
					reserved = true;
					break;
				}
			}
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (!reserved) {
			failure = wifi_stopping ? "stopping" : "too many";
			snprintf(result, sizeof(result), "NOT SERVED (%s)", failure);
		}
	}

	if (reserved) {
		transfer_port = vayu_qrtr_ops.open(transfer_receive, NULL, x);
		if (transfer_port == NULL) {
			lck_mtx_lock(&vayu_wifi_lock);
			x->used = false;
			x->busy = 0;
			lck_mtx_unlock(&vayu_wifi_lock);
			reserved = false;
			failure = "no port";
			snprintf(result, sizeof(result), "NOT SERVED (%s)", failure);
		} else {
			lck_mtx_lock(&vayu_wifi_lock);
			if (wifi_stopping) {
				x->used = false;
				x->busy = 0;
				lck_mtx_unlock(&vayu_wifi_lock);
				vayu_qrtr_ops.close(transfer_port);
				return;
			}
			x->port = transfer_port;
			tftp_served++;
			clock_get_uptime(&x->last_ns);
			lck_mtx_unlock(&vayu_wifi_lock);
			transfer_ready = true;
		}
	}

	shown = filename_len > 100 ? 100 : filename_len;
	lck_mtx_lock(&vayu_wifi_lock);
	tftp_reads++;
	log = tftp_log_take();
	first = tftp_path_note(filename_len <= VAYU_TFTP_PATH_MAX ? filename : "<path too long>",
	    bad_options || (!found && !why[0]) || (found && !transfer_ready) ? VAYU_TFTP_PATH_REFUSED :
	    (found ? VAYU_TFTP_PATH_SERVED : VAYU_TFTP_PATH_NOT_FOUND));
	lck_mtx_unlock(&vayu_wifi_lock);
	if (log) {
		say("vayu-wifi: tftp: node %u port %u reads \"%.*s\" (blksize %u tsize %s rsize %u seek %u "
		    "wsize %u): %s\n", node, from_port, (int)shown, filename, options.blksize,
		    options.tsize ? "yes" : "no", options.rsize, (unsigned int)options.seek, options.wsize,
		    result);
		if (first) {
			say("vayu-wifi: tftp: new path \"%.*s\"\n", (int)shown, filename);
		}
	}

	if (bad_options) {
		tftp_send_error(port, node, from_port, TFTP_ERROR_EOPTNEG, "Option negotiation failed");
		return;
	}
	if (!found) {
		tftp_send_error(port, node, from_port, TFTP_ERROR_ENOENT, "file not found");
		return;
	}
	if (!reserved) {
		tftp_send_error(port, node, from_port, 0, failure);
		return;
	}
	if (!transfer_ready) {
		lck_mtx_lock(&vayu_wifi_lock);
		x->busy = 0;
		lck_mtx_unlock(&vayu_wifi_lock);
		tftp_xfer_close(x, false, 0, NULL);
		return;
	}

	if (!have_options) {
		rc = tftp_send_window(x);
	} else {
		rc = tftp_send_oack(x, &options);
	}
	lck_mtx_lock(&vayu_wifi_lock);
	x->busy = 0;
	lck_mtx_unlock(&vayu_wifi_lock);
	if (rc < 0) {
		tftp_xfer_close(x, false, 0, NULL);
	}
}

/* The server port: RRQ and WRQ only; a read transfer then lives on its own port. */
static void
tftp_receive(void *context __unused, uint32_t node, uint32_t from_port, const void *data, size_t length)
{
	struct hakios_qrtr_port *port;
	uint16_t opcode;

	lck_mtx_lock(&vayu_wifi_lock);
	if (wifi_stopping || tftp_port == NULL) {
		lck_mtx_unlock(&vayu_wifi_lock);
		return;
	}
	port = tftp_port;
	tftp_busy++;
	lck_mtx_unlock(&vayu_wifi_lock);

	tftp_sweep();
	if (length >= 2) {
		opcode = tftp_get_be16(data);
		if (opcode == OP_RRQ) {
			tftp_handle_rrq(port, node, from_port, data, length);
		} else if (opcode == OP_WRQ) {
			tftp_handle_wrq(port, node, from_port, data, length);
		} else if (opcode == OP_DATA) {
			/*
			 * A write's DATA: on this server port, in the name of the WRQ that owns this
			 * node and port.  Stored by block number, not by "append", so a retransmitted
			 * block lands where it belongs and a jump is refused by name, node and port.
			 */
			if (length >= 4) {
				uint16_t block = tftp_get_be16((const uint8_t *)data + 2);
				size_t payload = length - 4;
				const uint8_t *payload_data = (const uint8_t *)data + 4;
				struct vayu_tftp_ram_file *file = NULL;
				const char *refusal = NULL;
				uint32_t offset = 0;
				uint16_t error_code = 0;
				bool duplicate = false, done = false, log;

				lck_mtx_lock(&vayu_wifi_lock);
				tftp_write_blocks++;
				tftp_write_bytes += payload;
				for (unsigned int i = 0; i < VAYU_TFTP_RAM_FILES; i++) {
					if (tftp_ram[i].writing && tftp_ram[i].node == node &&
					    tftp_ram[i].port == from_port) {
						file = &tftp_ram[i];
						break;
					}
				}
				if (file == NULL) {
					refusal = "no write in progress on this node and port";
				} else if (file->blocks != 0 && block == (uint16_t)(file->expected - 1)) {
					duplicate = true;       /* our ACK was lost: the same block again */
				} else if (block != file->expected) {
					file->writing = false;
					refusal = "block out of order";
					error_code = TFTP_ERROR_EBADOP;
				} else if ((uint64_t)(block - 1) * file->blksize + payload > file->limit) {
					file->writing = false;
					refusal = "file too big for its RAM slot";
					error_code = TFTP_ERROR_ENOSPC;
				} else {
					offset = (uint32_t)(block - 1) * file->blksize;
					memcpy(tftp_ram_data[file - tftp_ram] + offset, payload_data, payload);
					file->written = offset + (uint32_t)payload;
					file->blocks++;
					file->expected++;
					if (payload < file->blksize) {
						file->size = file->written;
						file->writing = false;
						done = true;
					}
				}
				log = tftp_log_take();
				lck_mtx_unlock(&vayu_wifi_lock);

				if (error_code != 0) {
					tftp_send_error(port, node, from_port, error_code, refusal);
				} else {
					(void)tftp_send_ack(port, node, from_port, block);
				}
				if (log) {
					if (refusal != NULL && file == NULL) {
						say("vayu-wifi: tftp: node %u port %u write block %u "
						    "(%zu bytes): IGNORED, %s; ACKed, nothing stored\n",
						    node, from_port, block, payload, refusal);
					} else if (refusal != NULL) {
						say("vayu-wifi: tftp: node %u port %u write \"%s\": "
						    "block %u (%zu bytes) REFUSED (%s), ERROR %u sent\n",
						    node, from_port, file->path, block, payload, refusal,
						    (unsigned int)error_code);
					} else if (done) {
						say("vayu-wifi: tftp: node %u port %u write \"%s\": %u bytes "
						    "in %u block(s), done\n", node, from_port, file->path,
						    file->size, file->blocks);
					} else if (!duplicate) {
						say("vayu-wifi: tftp: node %u port %u write \"%s\": block %u, "
						    "%zu bytes (RAM file %u bytes)\n", node, from_port,
						    file->path, block, payload, file->written);
					}
				}
			}
		}
	}

	lck_mtx_lock(&vayu_wifi_lock);
	tftp_busy--;
	lck_mtx_unlock(&vayu_wifi_lock);
}

/* ===================================================================================================
 * 3. WLFW: the read-only probe
 *
 * after ref/qmi.c (ath10k's client) and ref/qmi_wlfw_v01.c / .h (message ids and tables). All the
 * tables and structures here are renamed vayu_wlfw_*; another file of the kernel has some wlfw_*.
 * =================================================================================================== */

#define VAYU_WLFW_CLIENT_ID             0x4b4e454c      /* ath10k's ATH10K_QMI_CLIENT_ID (ref/qmi.c:25) */
/*
 * W12: the second client's id, immediate 0x444d4f4e in the vayu vendor cnss-daemon
 * (wlfw_send_ind_register_req at 0x14628: mov w10,#0x4f4e; movk w10,#0x444d,lsl#16).
 * Little-endian in the TLV: 4e 4f 4d 44 ("NOMD").
 */
#define VAYU_WLFW_CLIENT_ID2            0x444d4f4e
#define VAYU_WLFW_IND_REGISTER_REQ_V01  0x0020
#define VAYU_WLFW_FW_READY_IND_V01      0x0021
#define VAYU_WLFW_CAP_REQ_V01           0x0024
/* W13: the MAC the flame host sends before CAL_REPORT (wlfw.c:325-329; wlan_firmware_service_v01.h:63). */
#define VAYU_WLFW_MAC_ADDR_REQ_V01      0x0033
#define VAYU_WLFW_MSA_READY_IND_V01     0x002b
#define VAYU_WLFW_MSA_READY_REQ_V01     0x002e
#define VAYU_WLFW_PIN_CONNECT_RESULT_IND_V01 0x002c
#define VAYU_WLFW_MSA_INFO_REQ_V01      0x002d
#define VAYU_WLFW_HOST_CAP_REQ_V01      0x0034
#define VAYU_WLFW_FW_INIT_DONE_IND_V01  0x0038
/* The stock icnss's last pre-FW_READY request: icnss_qmi.c:989, wlan_firmware_service_v01.h:45. */
#define VAYU_WLFW_DYNAMIC_FEATURE_MASK_REQ_V01 0x003b

#define VAYU_WLFW_IND_REGISTER_REQ_MAX  50
#define VAYU_WLFW_HOST_CAP_REQ_MAX      189
#define VAYU_WLFW_MSA_INFO_REQ_MAX      18
#define VAYU_WLFW_CAP_REQ_MAX           0
#define VAYU_WLFW_MSA_READY_REQ_MAX     0
#define VAYU_WLFW_BDF_DOWNLOAD_REQ_V01  0x0025
#define VAYU_WLFW_CAL_REPORT_REQ_V01    0x0026
#define VAYU_WLFW_WLAN_MODE_REQ_V01     0x0022
#define VAYU_WLFW_WLAN_CFG_REQ_V01      0x0023
#define VAYU_WLFW_BDF_DOWNLOAD_REQ_MAX  6182
/*
 * HYPOTHESIS: the downstream wlfw_bdf_type_enum_v01 is 0=BDF, 1=REGDB.  ath10k never sets the
 * bdf_type TLV (qmi.c:256-263) and there is no icnss/cnss source on disk to confirm the value.
 * Used only when vayu_wifi_regdb=1/2; the default path stays byte-identical to ath10k.
 */
#define VAYU_WLFW_BDF_TYPE_REGDB_V01    1
#define VAYU_WLFW_CAL_REPORT_REQ_MAX    28
#define VAYU_WLFW_MAC_ADDR_REQ_MAX      9    /* wlan_firmware_service_v01.h:492: TLV 0x10 + 6 bytes */
#define VAYU_WLFW_MAC_ADDR_SIZE_V01     6
#define VAYU_WLFW_WLAN_MODE_REQ_MAX     11
#define VAYU_WLFW_WLAN_CFG_REQ_MAX      803
#define VAYU_WLFW_DYNAMIC_FEATURE_MASK_REQ_MAX 11   /* wlan_firmware_service_v01.h:581 */
#define VAYU_WLFW_MAX_DATA_SIZE_V01     6144
/*
 * ath10k qmi_wlfw_v01.h:66 QMI_WLFW_MAX_DATA_SIZE_V01 is 6144, and qmi.c:265-270 uses it as the
 * BDF segment size.  Keep the two names tied together: the task brief mentioned 6000-byte
 * segments, but neither the upstream header nor this tree has 6000; the firmware accepted all
 * five 6144-byte segments in 143328, so 6144 is the size to keep.
 */
#if VAYU_WLFW_MAX_DATA_SIZE_V01 != 6144
#error "VAYU_WLFW_MAX_DATA_SIZE_V01 must stay 6144 (ath10k QMI_WLFW_MAX_DATA_SIZE_V01)"
#endif
#define VAYU_WLFW_BDF_SEGMENT_SIZE      VAYU_WLFW_MAX_DATA_SIZE_V01
#define VAYU_WLFW_MAX_NUM_CE_V01        12
#define VAYU_WLFW_MAX_NUM_SVC_V01       24
#define VAYU_WLFW_MAX_NUM_SHADOW_REG_V01 24
#define VAYU_WLFW_MAX_NUM_CAL_V01       5
#define VAYU_WLFW_MAX_STR_LEN_V01       32
#define VAYU_WLFW_DRIVER_MODE_MISSION_V01 0
#define VAYU_WLFW_DRIVER_MODE_CALIBRATION_V01 7   /* wlan_firmware_service_v01.h:95 */
#define VAYU_QMI_ERR_MALFORMED_MSG_V01  3
#define VAYU_WLFW_MAX_MEM_REG_V01       2
#define VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 32
#define VAYU_WLFW_MAX_BUILD_ID_LEN_V01  128
#define VAYU_WLFW_MAX_NUM_GPIO_V01      32

#define VAYU_WLFW_FW_READY_V01          0x02ULL         /* ref/qmi_wlfw_v01.h:119 */
#define VAYU_WLFW_MSA_BASE              0x8bc00000ULL   /* stock DT pil_wlan_fw_region@8bc00000 */
#define VAYU_WLFW_MSA_END               0x8bd80000ULL
#define VAYU_WLFW_MSA_SIZE              0x100000U       /* stock DT icnss qcom,wlan-msa-memory = <0x100000> */
/*
 * What the stock icnss driver really passes: qcom,wlan-msa-fixed-region wins over
 * qcom,wlan-msa-memory (icnss.c:3767-3791: with the phandle present msa_mem_size = prop_size,
 * the 0x180000 of pil_wlan_fw_region@8bc00000; the memory property is only read in the else
 * branch).  WLFW_MEM_REG still has to sit inside 0x8bc00000..0x8bd80000, which it does.
 */
#define VAYU_WLFW_MSA_STOCK_SIZE        0x180000U
#define VAYU_WLFW_MSA_MIN_SIZE          0x40000U

struct vayu_wlfw_memory_region_info_s_v01 {
	uint64_t region_addr;
	uint32_t size;
	uint8_t secure_flag;
};

struct vayu_wlfw_rf_chip_info_s_v01 {
	uint32_t chip_id;
	uint32_t chip_family;
};

struct vayu_wlfw_rf_board_info_s_v01 {
	uint32_t board_id;
};

struct vayu_wlfw_soc_info_s_v01 {
	uint32_t soc_id;
};

struct vayu_wlfw_fw_version_info_s_v01 {
	uint32_t fw_version;
	char fw_build_timestamp[VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 + 1];
};

struct vayu_wlfw_ind_register_req_msg_v01 {
	uint8_t fw_ready_enable_valid;
	uint8_t fw_ready_enable;
	uint8_t initiate_cal_download_enable_valid;
	uint8_t initiate_cal_download_enable;
	uint8_t initiate_cal_update_enable_valid;
	uint8_t initiate_cal_update_enable;
	uint8_t msa_ready_enable_valid;
	uint8_t msa_ready_enable;
	uint8_t pin_connect_result_enable_valid;
	uint8_t pin_connect_result_enable;
	uint8_t client_id_valid;
	uint32_t client_id;
	uint8_t request_mem_enable_valid;
	uint8_t request_mem_enable;
	uint8_t mem_ready_enable_valid;
	uint8_t mem_ready_enable;
	uint8_t fw_init_done_enable_valid;
	uint8_t fw_init_done_enable;
	uint8_t rejuvenate_enable_valid;
	uint32_t rejuvenate_enable;
	uint8_t xo_cal_enable_valid;
	uint8_t xo_cal_enable;
	/*
	 * W12: the cnss-daemon's own IND_REGISTER sets a flag the 4.14 header does not have:
	 * the analysis of its QMI IDL element table (descriptor at daemon 0x7972:
	 * ... 41 1a 00 1d 41 1b 00 1f 41 1c 00 21 41 1d 00 23 ...) puts TLV 0x1d's
	 * value at struct offset 0x23, i.e. the
	 * two bytes at 0x22/0x23.  The newer cnss2 header names 0x1d qdss_trace_save_enable;
	 * the name is a HYPOTHESIS, the TLV id and the value 1 are facts from the daemon code
	 * (strh 0x0101 at sp+66 in wlfw_send_ind_register_req, mode 0 = cold boot).  The
	 * field is only set by vayu_wifi_stock2; every other flow leaves it invalid.
	 */
	uint8_t qdss_trace_save_enable_valid;
	uint8_t qdss_trace_save_enable;
};

struct vayu_wlfw_ind_register_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint8_t fw_status_valid;
	uint64_t fw_status;
};

struct vayu_wlfw_host_cap_req_msg_v01 {
	uint8_t daemon_support_valid;
	uint32_t daemon_support;
	uint8_t wake_msi_valid;
	uint32_t wake_msi;
	uint8_t gpios_valid;
	uint32_t gpios_len;
	uint32_t gpios[VAYU_WLFW_MAX_NUM_GPIO_V01];
	uint8_t nm_modem_valid;
	uint8_t nm_modem;
	uint8_t bdf_support_valid;
	uint8_t bdf_support;
	uint8_t bdf_cache_support_valid;
	uint8_t bdf_cache_support;
	uint8_t m3_support_valid;
	uint8_t m3_support;
	uint8_t m3_cache_support_valid;
	uint8_t m3_cache_support;
	uint8_t cal_filesys_support_valid;
	uint8_t cal_filesys_support;
	uint8_t cal_cache_support_valid;
	uint8_t cal_cache_support;
	uint8_t cal_done_valid;
	uint8_t cal_done;
	uint8_t mem_bucket_valid;
	uint32_t mem_bucket;
	uint8_t mem_cfg_mode_valid;
	uint8_t mem_cfg_mode;
};

struct vayu_wlfw_host_cap_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct vayu_wlfw_msa_info_req_msg_v01 {
	uint64_t msa_addr;
	uint32_t size;
};

struct vayu_wlfw_msa_info_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint32_t mem_region_info_len;
	struct vayu_wlfw_memory_region_info_s_v01 mem_region_info[VAYU_WLFW_MAX_MEM_REG_V01];
};

struct vayu_wlfw_cap_req_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_cap_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint8_t chip_info_valid;
	struct vayu_wlfw_rf_chip_info_s_v01 chip_info;
	uint8_t board_info_valid;
	struct vayu_wlfw_rf_board_info_s_v01 board_info;
	uint8_t soc_info_valid;
	struct vayu_wlfw_soc_info_s_v01 soc_info;
	uint8_t fw_version_info_valid;
	struct vayu_wlfw_fw_version_info_s_v01 fw_version_info;
	uint8_t fw_build_id_valid;
	char fw_build_id[VAYU_WLFW_MAX_BUILD_ID_LEN_V01 + 1];
	uint8_t num_macs_valid;
	uint8_t num_macs;
};

struct vayu_wlfw_pin_connect_result_ind_msg_v01 {
	uint8_t pwr_pin_result_valid;
	uint32_t pwr_pin_result;
	uint8_t phy_io_pin_result_valid;
	uint32_t phy_io_pin_result;
	uint8_t rf_pin_result_valid;
	uint32_t rf_pin_result;
};

struct vayu_wlfw_fw_ready_ind_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_msa_ready_ind_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_msa_ready_req_msg_v01 {
	char placeholder;
};

struct vayu_wlfw_msa_ready_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct vayu_wlfw_bdf_download_req_msg_v01 {
	uint8_t valid;
	uint8_t file_id_valid;
	uint32_t file_id;
	uint8_t total_size_valid;
	uint32_t total_size;
	uint8_t seg_id_valid;
	uint32_t seg_id;
	uint8_t data_valid;
	uint32_t data_len;
	uint8_t data[VAYU_WLFW_MAX_DATA_SIZE_V01];
	uint8_t end_valid;
	uint8_t end;
	uint8_t bdf_type_valid;
	uint8_t bdf_type;
};

struct vayu_wlfw_bdf_download_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

/*
 * W13: WLFW MAC_ADDR (QMI 0x0033), wlan_firmware_service_v01.h:488-499.  The address is a fixed
 * six-byte array in TLV 0x10; flame sends it before CAL_REPORT (wlfw.c:326-329).
 */
struct vayu_wlfw_mac_addr_req_msg_v01 {
	uint8_t mac_addr_valid;
	uint8_t mac_addr[VAYU_WLFW_MAC_ADDR_SIZE_V01];
};

struct vayu_wlfw_mac_addr_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct vayu_wlfw_cal_report_req_msg_v01 {
	uint32_t meta_data_len;
	uint32_t meta_data[VAYU_WLFW_MAX_NUM_CAL_V01];
	uint8_t xo_cal_data_valid;
	uint8_t xo_cal_data;
	/*
	 * W12: the stock cnss-daemon's CAL_REPORT is 8 bytes even with no cal files on disk.
	 * Its QMI IDL table puts TLV 0x11's value at struct offset 0x1b (daemon 0x7ae4:
	 * 41 10 00 19 41 11 00 1b), and wlfw_send_cal_report_req writes 0x0101 at sp+42
	 * unconditionally (valid=1, value=0).  So the body is 01 01 00 00 11 01 00 00:
	 * TLV 0x01 (meta_data_len, 1-byte count = 0) then TLV 0x11 = 0.  The newer IDL name
	 * is unknown/cal_done; only the wire bytes are facts.  Set by vayu_wifi_stock2 only.
	 */
	uint8_t tlv11_valid;
	uint8_t tlv11;
};

struct vayu_wlfw_cal_report_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct vayu_wlfw_wlan_mode_req_msg_v01 {
	uint32_t mode;
	uint8_t hw_debug_valid;
	uint8_t hw_debug;
};

struct vayu_wlfw_wlan_mode_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01 {
	uint32_t pipe_num;
	uint32_t pipe_dir;
	uint32_t nentries;
	uint32_t nbytes_max;
	uint32_t flags;
};

struct vayu_wlfw_ce_svc_pipe_cfg_s_v01 {
	uint32_t service_id;
	uint32_t pipe_dir;
	uint32_t pipe_num;
};

struct vayu_wlfw_shadow_reg_cfg_s_v01 {
	uint16_t id;
	uint16_t offset;
};

struct vayu_wlfw_wlan_cfg_req_msg_v01 {
	uint8_t host_version_valid;
	char host_version[VAYU_WLFW_MAX_STR_LEN_V01 + 1];
	uint8_t tgt_cfg_valid;
	uint32_t tgt_cfg_len;
	struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01 tgt_cfg[VAYU_WLFW_MAX_NUM_CE_V01];
	uint8_t svc_cfg_valid;
	uint32_t svc_cfg_len;
	struct vayu_wlfw_ce_svc_pipe_cfg_s_v01 svc_cfg[VAYU_WLFW_MAX_NUM_SVC_V01];
	uint8_t shadow_reg_valid;
	uint32_t shadow_reg_len;
	struct vayu_wlfw_shadow_reg_cfg_s_v01 shadow_reg[VAYU_WLFW_MAX_NUM_SHADOW_REG_V01];
};

struct vayu_wlfw_wlan_cfg_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

/*
 * WLFW DYNAMIC_FEATURE_MASK, QMI 0x003B: the stock icnss's only message between CAP and the
 * FW_READY wait (icnss.c:1042-1043, icnss_qmi.c:942-1010, wlan_firmware_service_v01.h:577-592).
 * ath10k never sends it, so the firmware's need for it is unknown; vayu_wifi_dynmask gates it.
 */
struct vayu_wlfw_dynamic_feature_mask_req_msg_v01 {
	uint8_t mask_valid;
	uint64_t mask;
};

struct vayu_wlfw_dynamic_feature_mask_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
	uint8_t prev_mask_valid;
	uint64_t prev_mask;
	uint8_t curr_mask_valid;
	uint64_t curr_mask;
};

struct vayu_wlfw_fw_init_done_ind_msg_v01 {
	char placeholder;
};

static const struct qmi_elem_info vayu_wlfw_memory_region_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_8_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint64_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_memory_region_info_s_v01, region_addr),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_memory_region_info_s_v01, size),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_memory_region_info_s_v01, secure_flag),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_rf_chip_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_rf_chip_info_s_v01, chip_id),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_rf_chip_info_s_v01, chip_family),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_rf_board_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_rf_board_info_s_v01, board_id),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_soc_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_soc_info_s_v01, soc_id),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_fw_version_info_s_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_fw_version_info_s_v01, fw_version),
	},
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0,
		.offset = offsetof(struct vayu_wlfw_fw_version_info_s_v01, fw_build_timestamp),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_ind_register_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_ready_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_ready_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_download_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_download_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_update_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, initiate_cal_update_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, msa_ready_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, msa_ready_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, pin_connect_result_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, pin_connect_result_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, client_id_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, client_id) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, request_mem_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, request_mem_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, mem_ready_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, mem_ready_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_init_done_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, fw_init_done_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, rejuvenate_enable_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, rejuvenate_enable) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, xo_cal_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01, xo_cal_enable) },
	/* W12: the second (cnss-daemon) client's sixth flag, TLV 0x1d; see the struct comment. */
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1D, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01,
					      qdss_trace_save_enable_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1D, .offset = offsetof(struct vayu_wlfw_ind_register_req_msg_v01,
					      qdss_trace_save_enable) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_ind_register_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_ind_register_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_resp_msg_v01, fw_status_valid) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_ind_register_resp_msg_v01, fw_status) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_host_cap_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, wake_msi_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, wake_msi) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, gpios_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, gpios_len) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = VAYU_WLFW_MAX_NUM_GPIO_V01, .elem_size = sizeof(uint32_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, gpios) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, nm_modem_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, nm_modem) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_cache_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, bdf_cache_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x16, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_cache_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x17, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, m3_cache_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_filesys_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x18, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_filesys_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_cache_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x19, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_cache_support) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_done_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1A, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, cal_done) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1B, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_bucket_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1B, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_bucket) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1C, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_cfg_mode_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x1C, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, mem_cfg_mode) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_host_cap_8bit_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_host_cap_req_msg_v01, daemon_support) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_host_cap_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_host_cap_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_msa_info_req_msg_v01_ei[] = {
	{
		.data_type = QMI_UNSIGNED_8_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint64_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct vayu_wlfw_msa_info_req_msg_v01, msa_addr),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_msa_info_req_msg_v01, size),
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_msa_info_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_msa_info_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{
		.data_type = QMI_DATA_LEN,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.array_type = NO_ARRAY,
		.tlv_type = 0x03,
		.offset = offsetof(struct vayu_wlfw_msa_info_resp_msg_v01, mem_region_info_len),
	},
	{
		.data_type = QMI_STRUCT,
		.elem_len = VAYU_WLFW_MAX_MEM_REG_V01,
		.elem_size = sizeof(struct vayu_wlfw_memory_region_info_s_v01),
		.array_type = VAR_LEN_ARRAY,
		.tlv_type = 0x03,
		.offset = offsetof(struct vayu_wlfw_msa_info_resp_msg_v01, mem_region_info),
		.ei_array = vayu_wlfw_memory_region_info_s_v01_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_cap_req_msg_v01_ei[] = {
	{}
};

static const struct qmi_elem_info vayu_wlfw_cap_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, chip_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_rf_chip_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, chip_info),
		.ei_array = vayu_wlfw_rf_chip_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, board_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_rf_board_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x11,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, board_info),
		.ei_array = vayu_wlfw_rf_board_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, soc_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_soc_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, soc_info),
		.ei_array = vayu_wlfw_soc_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x13, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_version_info_valid) },
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct vayu_wlfw_fw_version_info_s_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x13,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_version_info),
		.ei_array = vayu_wlfw_fw_version_info_s_v01_ei,
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x14, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_build_id_valid) },
	{
		.data_type = QMI_STRING,
		.elem_len = VAYU_WLFW_MAX_BUILD_ID_LEN_V01 + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x14,
		.offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, fw_build_id),
	},
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, num_macs_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x15, .offset = offsetof(struct vayu_wlfw_cap_resp_msg_v01, num_macs) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_pin_connect_result_ind_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, pwr_pin_result_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, pwr_pin_result) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, phy_io_pin_result_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, phy_io_pin_result) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, rf_pin_result_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x12, .offset = offsetof(struct vayu_wlfw_pin_connect_result_ind_msg_v01, rf_pin_result) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_fw_ready_ind_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static const struct qmi_elem_info vayu_wlfw_msa_ready_ind_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static const struct qmi_elem_info vayu_wlfw_msa_ready_req_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static const struct qmi_elem_info vayu_wlfw_msa_ready_resp_msg_v01_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct vayu_wlfw_msa_ready_resp_msg_v01, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

static const struct qmi_elem_info vayu_wlfw_bdf_download_req_msg_v01_ei[] = {
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x01,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, valid) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, file_id_valid) },
	{ .data_type = QMI_SIGNED_4_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, file_id) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, total_size_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, total_size) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, seg_id_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, seg_id) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, data_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint16_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, data_len) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_WLFW_MAX_DATA_SIZE_V01, .elem_size = sizeof(uint8_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, data) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x14,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, end_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x14,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, end) },
	/*
	 * TLV 0x15 is the bdf_type the stock host may use for regdb.bin.  It is in the struct
	 * (qmi_wlfw_v01.h:337-338) but ath10k's element table never carries it, so the default
	 * path must leave bdf_type_valid=0 and stay byte-identical to ath10k.  Only
	 * vayu_wifi_regdb=1/2 sets it (value 1=REGDB, a downstream hypothesis).
	 */
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x15,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, bdf_type_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x15,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_req_msg_v01, bdf_type) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_bdf_download_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01),
	  .array_type = NO_ARRAY, .tlv_type = 0x02,
	  .offset = offsetof(struct vayu_wlfw_bdf_download_resp_msg_v01, resp),
	  .ei_array = qmi_response_type_v01_ei },
	{}
};

/*
 * W13: MAC_ADDR.  Six bytes in one TLV 0x10: elem_len is the array length, elem_size is one
 * byte, as the vendor IDL (wlan_firmware_service_v01.c's mac_addr entry) describes it.
 */
static const struct qmi_elem_info vayu_wlfw_mac_addr_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_mac_addr_req_msg_v01, mac_addr_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = VAYU_WLFW_MAC_ADDR_SIZE_V01,
	  .elem_size = sizeof(uint8_t), .array_type = STATIC_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_mac_addr_req_msg_v01, mac_addr) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_mac_addr_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01),
	  .array_type = NO_ARRAY, .tlv_type = 0x02,
	  .offset = offsetof(struct vayu_wlfw_mac_addr_resp_msg_v01, resp),
	  .ei_array = qmi_response_type_v01_ei },
	{}
};

static const struct qmi_elem_info vayu_wlfw_cal_report_req_msg_v01_ei[] = {
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x01,
	  .offset = offsetof(struct vayu_wlfw_cal_report_req_msg_v01, meta_data_len) },
	{ .data_type = QMI_SIGNED_4_BYTE_ENUM, .elem_len = VAYU_WLFW_MAX_NUM_CAL_V01, .elem_size = sizeof(uint32_t),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x01,
	  .offset = offsetof(struct vayu_wlfw_cal_report_req_msg_v01, meta_data) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_cal_report_req_msg_v01, xo_cal_data_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_cal_report_req_msg_v01, xo_cal_data) },
	/* W12: the stock cnss-daemon's unconditional TLV 0x11 = 0; see the struct comment. */
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_cal_report_req_msg_v01, tlv11_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_cal_report_req_msg_v01, tlv11) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_cal_report_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01),
	  .array_type = NO_ARRAY, .tlv_type = 0x02,
	  .offset = offsetof(struct vayu_wlfw_cal_report_resp_msg_v01, resp),
	  .ei_array = qmi_response_type_v01_ei },
	{}
};

static const struct qmi_elem_info vayu_wlfw_wlan_mode_req_msg_v01_ei[] = {
	{ .data_type = QMI_SIGNED_4_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x01,
	  .offset = offsetof(struct vayu_wlfw_wlan_mode_req_msg_v01, mode) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_wlan_mode_req_msg_v01, hw_debug_valid) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_wlan_mode_req_msg_v01, hw_debug) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_wlan_mode_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01),
	  .array_type = NO_ARRAY, .tlv_type = 0x02,
	  .offset = offsetof(struct vayu_wlfw_wlan_mode_resp_msg_v01, resp),
	  .ei_array = qmi_response_type_v01_ei },
	{}
};

static const struct qmi_elem_info vayu_wlfw_ce_tgt_pipe_cfg_s_v01_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01, pipe_num) },
	{ .data_type = QMI_SIGNED_4_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01, pipe_dir) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01, nentries) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01, nbytes_max) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01, flags) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_ce_svc_pipe_cfg_s_v01_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_svc_pipe_cfg_s_v01, service_id) },
	{ .data_type = QMI_SIGNED_4_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_svc_pipe_cfg_s_v01, pipe_dir) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_ce_svc_pipe_cfg_s_v01, pipe_num) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_shadow_reg_cfg_s_v01_ei[] = {
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_shadow_reg_cfg_s_v01, id) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t),
	  .array_type = NO_ARRAY, .tlv_type = 0,
	  .offset = offsetof(struct vayu_wlfw_shadow_reg_cfg_s_v01, offset) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_wlan_cfg_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, host_version_valid) },
	{ .data_type = QMI_STRING, .elem_len = VAYU_WLFW_MAX_STR_LEN_V01 + 1, .elem_size = sizeof(char),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, host_version) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, tgt_cfg_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, tgt_cfg_len) },
	{ .data_type = QMI_STRUCT, .elem_len = VAYU_WLFW_MAX_NUM_CE_V01, .elem_size = sizeof(struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, tgt_cfg),
	  .ei_array = vayu_wlfw_ce_tgt_pipe_cfg_s_v01_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, svc_cfg_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, svc_cfg_len) },
	{ .data_type = QMI_STRUCT, .elem_len = VAYU_WLFW_MAX_NUM_SVC_V01, .elem_size = sizeof(struct vayu_wlfw_ce_svc_pipe_cfg_s_v01),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x12,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, svc_cfg),
	  .ei_array = vayu_wlfw_ce_svc_pipe_cfg_s_v01_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, shadow_reg_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, shadow_reg_len) },
	{ .data_type = QMI_STRUCT, .elem_len = VAYU_WLFW_MAX_NUM_SHADOW_REG_V01, .elem_size = sizeof(struct vayu_wlfw_shadow_reg_cfg_s_v01),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x13,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_req_msg_v01, shadow_reg),
	  .ei_array = vayu_wlfw_shadow_reg_cfg_s_v01_ei },
	{}
};

static const struct qmi_elem_info vayu_wlfw_wlan_cfg_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01),
	  .array_type = NO_ARRAY, .tlv_type = 0x02,
	  .offset = offsetof(struct vayu_wlfw_wlan_cfg_resp_msg_v01, resp),
	  .ei_array = qmi_response_type_v01_ei },
	{}
};

/* QMI 0x003B element tables, copied field-for-field from wlan_firmware_service_v01.c:2138-2221. */
static const struct qmi_elem_info vayu_wlfw_dynamic_feature_mask_req_msg_v01_ei[] = {
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_dynamic_feature_mask_req_msg_v01, mask_valid) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_dynamic_feature_mask_req_msg_v01, mask) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_dynamic_feature_mask_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01),
	  .array_type = NO_ARRAY, .tlv_type = 0x02,
	  .offset = offsetof(struct vayu_wlfw_dynamic_feature_mask_resp_msg_v01, resp),
	  .ei_array = qmi_response_type_v01_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_dynamic_feature_mask_resp_msg_v01, prev_mask_valid) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = offsetof(struct vayu_wlfw_dynamic_feature_mask_resp_msg_v01, prev_mask) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_dynamic_feature_mask_resp_msg_v01, curr_mask_valid) },
	{ .data_type = QMI_UNSIGNED_8_BYTE, .elem_len = 1, .elem_size = sizeof(uint64_t),
	  .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = offsetof(struct vayu_wlfw_dynamic_feature_mask_resp_msg_v01, curr_mask) },
	{}
};

static const struct qmi_elem_info vayu_wlfw_fw_init_done_ind_msg_v01_ei[] = {
	{ .data_type = QMI_EOTI }
};

static struct qmi_handle wlfw_handle;
static struct {
	volatile bool server;
	volatile bool running;
	uint32_t node, port;
	volatile bool stop;
	volatile bool fw_ready;         /* the FW_READY indication arrived */
	volatile bool msa_ready;        /* the firmware MSA_READY indication arrived */
	bool cap_ok;                    /* CAP answered result SUCCESS (gate for vayu_wifi_dynmask) */
	uint32_t chip_id, board_id, soc_id;
	uint32_t used_port;             /* the port the sequence was run against */
	uint32_t attempts;              /* how many times the pre-FW_READY sequence ran */
	char summary[96];
} wlfw;

static void
wlfw_fail(const char *step)
{
	snprintf(wlfw.summary, sizeof(wlfw.summary), "failed at %s", step);
	vayu_wifi_smmu_fault(step);
}

static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int ret;

	say("vayu-wifi: wlfw: server at node %u port %u\n", service->node, service->port);
	ret = hakios_qmi_connect(qmi->sock, service->node, service->port);
	if (ret != 0) {
		say("vayu-wifi: wlfw: cannot connect to node %u port %u: %d\n", service->node, service->port, ret);
		return ret;
	}
	wlfw.node = service->node;
	wlfw.port = service->port;
	wlfw.server = true;
	return 0;
}

static void
wlfw_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service __unused)
{
	wlfw.server = false;
}

static void
wlfw_ind_fw_ready(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused, struct qmi_txn *txn __unused,
    const void *decoded __unused)
{
	say("vayu-wifi: wlfw: indication FW_READY\n");
	wlfw.fw_ready = true;
}

static void
wlfw_ind_msa_ready(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused, struct qmi_txn *txn __unused,
    const void *decoded __unused)
{
	say("vayu-wifi: wlfw: indication MSA_READY\n");
	wlfw.msa_ready = true;
}

static void
wlfw_ind_fw_init_done(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused,
    struct qmi_txn *txn __unused, const void *decoded __unused)
{
	say("vayu-wifi: wlfw: indication FW_INIT_DONE\n");
}

static void
wlfw_ind_pin_connect_result(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused,
    struct qmi_txn *txn __unused, const void *decoded)
{
	const struct vayu_wlfw_pin_connect_result_ind_msg_v01 *ind = decoded;
	char pwr[16], phy[16], rf[16];

	if (ind->pwr_pin_result_valid) {
		snprintf(pwr, sizeof(pwr), "%u", ind->pwr_pin_result);
	} else {
		snprintf(pwr, sizeof(pwr), "?");
	}
	if (ind->phy_io_pin_result_valid) {
		snprintf(phy, sizeof(phy), "%u", ind->phy_io_pin_result);
	} else {
		snprintf(phy, sizeof(phy), "?");
	}
	if (ind->rf_pin_result_valid) {
		snprintf(rf, sizeof(rf), "%u", ind->rf_pin_result);
	} else {
		snprintf(rf, sizeof(rf), "?");
	}
	say("vayu-wifi: wlfw: indication PIN_CONNECT_RESULT: pwr_pin_result %s phy_io_pin_result %s "
	    "rf_pin_result %s\n", pwr, phy, rf);
}

static const struct qmi_msg_handler wlfw_handlers[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_FW_READY_IND_V01,
		.ei = vayu_wlfw_fw_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_fw_ready_ind_msg_v01),
		.fn = wlfw_ind_fw_ready,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_MSA_READY_IND_V01,
		.ei = vayu_wlfw_msa_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_msa_ready_ind_msg_v01),
		.fn = wlfw_ind_msa_ready,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_PIN_CONNECT_RESULT_IND_V01,
		.ei = vayu_wlfw_pin_connect_result_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_pin_connect_result_ind_msg_v01),
		.fn = wlfw_ind_pin_connect_result,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_FW_INIT_DONE_IND_V01,
		.ei = vayu_wlfw_fw_init_done_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_fw_init_done_ind_msg_v01),
		.fn = wlfw_ind_fw_init_done,
	},
	{}
};

static const struct qmi_ops wlfw_ops = {
	.new_server = wlfw_new_server,
	.del_server = wlfw_del_server,
};

/* ===================================================================================================
 * W12: the second WLFW client (the stock cnss-daemon's role)
 *
 * The stock capture (receipts/2026-10-24-vayu-wifi.md, W12 section) has TWO QMI clients on service
 * 0x45: the kernel icnss client (port 0x4028) and cnss-daemon (port 0x43f6).  The daemon registers
 * its own indications, sends its own CAP, downloads bdwlan.bin with the bdf_type TLV and sends
 * CAL_REPORT.  A QMI client is identified by its QRTR port, so this is a second qmi_handle with
 * its own transport port, exactly like the daemon.  Both lookups are registered before the WLFW
 * server appears, so the same QRTR NEW_SERVER announcement reaches both.
 * =================================================================================================== */

static int wlfw_request_on(struct qmi_handle *handle, const char *who, const char *what,
    uint16_t message, size_t largest, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp);

static struct qmi_handle wlfw2_handle;
static bool wlfw2_live;                  /* qmi_handle_init succeeded; out_release must close it */
static volatile bool wlfw2_server;
static uint32_t wlfw2_node, wlfw2_port;

static int
wlfw2_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int ret;

	say("vayu-wifi: wlfw2: WLFW server at node %u port %u\n", service->node, service->port);
	ret = hakios_qmi_connect(qmi->sock, service->node, service->port);
	if (ret != 0) {
		say("vayu-wifi: wlfw2: cannot connect to node %u port %u: %d\n", service->node,
		    service->port, ret);
		return ret;
	}
	wlfw2_node = service->node;
	wlfw2_port = service->port;
	wlfw2_server = true;
	return 0;
}

static void
wlfw2_del_server(struct qmi_handle *qmi __unused, struct qmi_service *service __unused)
{
	say("vayu-wifi: wlfw2: WLFW server delete\n");
	wlfw2_server = false;
}

static void
wlfw2_ind_fw_ready(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused,
    struct qmi_txn *txn __unused, const void *decoded __unused)
{
	say("vayu-wifi: wlfw2: indication FW_READY\n");
	wlfw.fw_ready = true;
}

static void
wlfw2_ind_msa_ready(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused,
    struct qmi_txn *txn __unused, const void *decoded __unused)
{
	say("vayu-wifi: wlfw2: indication MSA_READY\n");
	wlfw.msa_ready = true;
}

static void
wlfw2_ind_fw_init_done(struct qmi_handle *qmi __unused, struct sockaddr_qrtr *sq __unused,
    struct qmi_txn *txn __unused, const void *decoded __unused)
{
	say("vayu-wifi: wlfw2: indication FW_INIT_DONE\n");
}

static const struct qmi_msg_handler wlfw2_handlers[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_FW_READY_IND_V01,
		.ei = vayu_wlfw_fw_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_fw_ready_ind_msg_v01),
		.fn = wlfw2_ind_fw_ready,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_MSA_READY_IND_V01,
		.ei = vayu_wlfw_msa_ready_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_msa_ready_ind_msg_v01),
		.fn = wlfw2_ind_msa_ready,
	},
	{
		.type = QMI_INDICATION,
		.msg_id = VAYU_WLFW_FW_INIT_DONE_IND_V01,
		.ei = vayu_wlfw_fw_init_done_ind_msg_v01_ei,
		.decoded_size = sizeof(struct vayu_wlfw_fw_init_done_ind_msg_v01),
		.fn = wlfw2_ind_fw_init_done,
	},
	{}
};

static const struct qmi_ops wlfw2_ops = {
	.new_server = wlfw2_new_server,
	.del_server = wlfw2_del_server,
};

/* Own QRTR port and own lookup, created before the WLFW server appears (stock cnss-daemon runs
 * from boot long before the modem PD). */
static int
wlfw2_start(void)
{
	int ret;

	ret = qmi_handle_init(&wlfw2_handle, 8192, &wlfw2_ops, wlfw2_handlers);
	if (ret < 0) {
		say("vayu-wifi: wlfw2: qmi_handle_init: %d\n", ret);
		return ret;
	}
	wlfw2_live = true;
	ret = qmi_add_lookup(&wlfw2_handle, VAYU_WLFW_SERVICE, VAYU_WLFW_VERSION, VAYU_WLFW_INSTANCE);
	if (ret < 0) {
		say("vayu-wifi: wlfw2: qmi_add_lookup: %d\n", ret);
		qmi_handle_release(&wlfw2_handle);
		wlfw2_live = false;
		return ret;
	}
	say("vayu-wifi: wlfw2: second QMI client created (own QRTR port, cnss-daemon role)\n");
	return 0;
}

/* The daemon's IND_REGISTER, byte for byte from its own binary (wlfw_send_ind_register_req at
 * 0x14628 in the vayu vendor cnss-daemon): client id 0x444d4f4e and six one-byte enable flags,
 * TLVs 0x10, 0x11, 0x12, 0x13, 0x1a and 0x1d, all 1 -> body 31 bytes.  The 4.14 icnss header
 * has no 0x1d; the QMI IDL descriptor in the daemon (0x7972) is where the id comes from. */
static int
wlfw2_ind_register(void)
{
	struct vayu_wlfw_ind_register_req_msg_v01 req = { 0 };
	struct vayu_wlfw_ind_register_resp_msg_v01 resp = { 0 };
	int ret;

	req.client_id_valid = 1;
	req.client_id = VAYU_WLFW_CLIENT_ID2;
	req.fw_ready_enable_valid = 1;
	req.fw_ready_enable = 1;
	req.initiate_cal_download_enable_valid = 1;
	req.initiate_cal_download_enable = 1;
	req.initiate_cal_update_enable_valid = 1;
	req.initiate_cal_update_enable = 1;
	req.msa_ready_enable_valid = 1;
	req.msa_ready_enable = 1;
	req.xo_cal_enable_valid = 1;
	req.xo_cal_enable = 1;
	req.qdss_trace_save_enable_valid = 1;
	req.qdss_trace_save_enable = 1;
	ret = wlfw_request_on(&wlfw2_handle, "wlfw2", "ind_register", VAYU_WLFW_IND_REGISTER_REQ_V01,
	    VAYU_WLFW_IND_REGISTER_REQ_MAX, vayu_wlfw_ind_register_req_msg_v01_ei, &req,
	    vayu_wlfw_ind_register_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: wlfw2: ind_register: result %u error %u (client %#x, 6 flags, 31 B)\n",
	    (unsigned int)resp.resp.result, (unsigned int)resp.resp.error,
	    (unsigned int)VAYU_WLFW_CLIENT_ID2);
	return resp.resp.result == QMI_RESULT_SUCCESS_V01 ? 0 : -1;
}

/* The daemon's CAP, the same request the kernel client sends.  Stock sends both before either
 * answer arrives; here they are sequential, which the firmware accepted in every W3-W10 run. */
static int
wlfw2_cap(void)
{
	struct vayu_wlfw_cap_req_msg_v01 req = { 0 };
	struct vayu_wlfw_cap_resp_msg_v01 resp = { 0 };
	int ret;

	ret = wlfw_request_on(&wlfw2_handle, "wlfw2", "cap", VAYU_WLFW_CAP_REQ_V01,
	    VAYU_WLFW_CAP_REQ_MAX, vayu_wlfw_cap_req_msg_v01_ei, &req,
	    vayu_wlfw_cap_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: wlfw2: cap: result %u error %u\n", (unsigned int)resp.resp.result,
	    (unsigned int)resp.resp.error);
	return resp.resp.result == QMI_RESULT_SUCCESS_V01 ? 0 : -1;
}

/*
 * One request on one client, waited 5 s: the QMI result and error are logged; a QMI error answer
 * is not fatal (the caller goes on), a transport failure is (the caller stops the probe).
 * `who` is "wlfw" or "wlfw2", `what` the step's name for the log and the summary.
 */
static int
wlfw_request_on(struct qmi_handle *handle, const char *who, const char *what, uint16_t message,
    size_t largest, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp)
{
	struct qmi_txn txn;
	int ret;

	wlfw_stage = what;
	ret = qmi_txn_init(handle, &txn, resp_ei, resp);
	if (ret < 0) {
		say("vayu-wifi: %s: %s: cannot start a transaction: %d\n", who, what, ret);
		wlfw_fail(what);
		return ret;
	}
	ret = qmi_send_request(handle, NULL, &txn, message, largest, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		say("vayu-wifi: %s: %s: cannot send: %d\n", who, what, ret);
		wlfw_fail(what);
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, 5000);
	if (ret < 0) {
		say("vayu-wifi: %s: %s: no reply: %d\n", who, what, ret);
		wlfw_fail(what);
		return ret;
	}
	return 0;
}

static int
wlfw_request(const char *what, uint16_t message, size_t largest, const struct qmi_elem_info *req_ei,
    const void *req, const struct qmi_elem_info *resp_ei, void *resp)
{
	return wlfw_request_on(&wlfw_handle, "wlfw", what, message, largest, req_ei, req, resp_ei,
	    resp);
}


/*
 * WCN3990 CE target/service/shadow configuration, from ath10k_snoc.c
 * (target_ce_config_wlan, target_service_to_ce_map_wlan, target_shadow_reg_cfg_map).
 * These are the tables the Linux host sends in WLAN_CFG before WLAN_MODE MISSION.
 */
static const struct vayu_wlfw_ce_tgt_pipe_cfg_s_v01 vayu_wlan_tgt_cfg[] = {
	{ 0, 2, 32, 2048, 0 },          /* CE0: host->target HTC control/raw */
	{ 1, 1, 32, 2048, 0 },          /* CE1: target->host HTT + HTC control */
	{ 2, 1, 64, 2048, 0 },          /* CE2: target->host WMI */
	{ 3, 2, 32, 2048, 0 },          /* CE3: host->target WMI */
	{ 4, 2, 256, 256, 8 },          /* CE4: host->target HTT, no intr */
	{ 5, 2, 1024, 64, 8 },          /* CE5: HIF->HTT, no intr */
	{ 6, 3, 32, 16384, 0 },         /* CE6: target autonomous hif_memcpy */
	{ 7, 4, 0, 0, 8 },              /* CE7: host-only diagnostic window */
	{ 8, 1, 32, 2048, 0 },          /* CE8: target to uMC */
	{ 9, 1, 32, 2048, 0 },          /* CE9: target->host HTT */
	{ 10, 1, 32, 2048, 0 },         /* CE10: target->host HTT */
	{ 11, 1, 32, 2048, 0 },         /* CE11: target autonomous qcache memcpy */
};

static const struct vayu_wlfw_ce_svc_pipe_cfg_s_v01 vayu_wlan_svc_cfg[] = {
	{ 0x0104, 2, 3 },  { 0x0104, 1, 2 },   /* WMI_DATA_VO  out/in  */
	{ 0x0102, 2, 3 },  { 0x0102, 1, 2 },   /* WMI_DATA_BK  out/in  */
	{ 0x0101, 2, 3 },  { 0x0101, 1, 2 },   /* WMI_DATA_BE  out/in  */
	{ 0x0103, 2, 3 },  { 0x0103, 1, 2 },   /* WMI_DATA_VI  out/in  */
	{ 0x0100, 2, 3 },  { 0x0100, 1, 2 },   /* WMI_CONTROL  out/in  */
	{ 0x0001, 2, 0 },  { 0x0001, 1, 2 },   /* RSVD_CTRL    out/in  */
	{ 0xfe00, 2, 0 },  { 0xfe00, 1, 2 },   /* TEST_RAW     out/in  */
	{ 0x0300, 2, 4 },  { 0x0300, 1, 1 },   /* HTT_DATA     out/in  */
	{ 0xfe00, 2, 5 },                       /* TEST_RAW out 5      */
	{ 0x0301, 1, 9 },                       /* HTT_DATA2 in 9      */
	{ 0x0302, 1, 10 },                      /* HTT_DATA3 in 10     */
	{ 0x0600, 1, 11 },                      /* HTT_LOG in 11       */
	{ 0x0000, 0, 0 },                       /* ath10k_snoc's sentinel */
};

static const struct vayu_wlfw_shadow_reg_cfg_s_v01 vayu_wlan_shadow_reg[] = {
	{ 0, 0x3c }, { 3, 0x3c }, { 4, 0x3c }, { 5, 0x3c }, { 7, 0x3c },
	{ 1, 0x40 }, { 2, 0x40 }, { 7, 0x40 }, { 8, 0x40 }, { 9, 0x40 },
	{ 10, 0x40 }, { 11, 0x40 },
};

/*
 * The board data file from the boot image's fw/, and what its first bytes say.
 *
 * The stock names are hexadecimal (bdwlan.b0a, bdwlan.b71, bdwlan.102), so the board-id name is
 * formatted %02x; the old %u form remains a fallback only after the hex name misses.  ath10k passes
 * the cap board id (0xFF when the TLV is absent, qmi.c:616-626) into the core fetch (qmi.c:862-
 * 878); core.c is not on disk here, so its exact name stays a HYPOTHESIS.  Our cap answer carries
 * no board_info TLV (the log says "board_id ?"), so only the fallback is reachable
 * unless vayu_wifi_bdf_file names a file explicitly.
 *
 * The first 16 bytes go to the log because they are the only part of the blob a run can show; the
 * BDF header carries bdf_version/ref_design_id/customer_id/project_id/board_data_rev, which the
 * firmware prints in comDev_bdf.c:hw_bdf_info (wlanmdsp.mbn @ 0x3d7558).
 */
static const uint8_t *
vayu_wifi_bdf_open(const char *name, uint64_t *size, uint32_t board_id, const char *why, bool noisy)
{
	const uint8_t *bdf = hakios_image_file("fw", name, size, NULL);
	char hdr[40] = { 0 };
	size_t at = 0;

	if (bdf == NULL) {
		if (noisy) {
			say("vayu-wifi: wlfw: bdf: fw/%s: not in the boot image\n", name);
		}
		return NULL;
	}
	for (unsigned i = 0; i < 16 && i < *size; i++) {
		at += (size_t)snprintf(hdr + at, sizeof(hdr) - at, "%02x", (unsigned int)bdf[i]);
		if (i == 3 || i == 7 || i == 11) {
			at += (size_t)snprintf(hdr + at, sizeof(hdr) - at, " ");
		}
	}
	say("vayu-wifi: wlfw: bdf: fw/%s %llu bytes (%s, board %#x) hdr %s\n", name,
	    (unsigned long long)*size, why, board_id, hdr);
	return bdf;
}

static const uint8_t *
vayu_wifi_bdf_file(uint32_t board_id, uint64_t *size)
{
	char name[64];
	const uint8_t *bdf;
	bool known = board_id != 0 && board_id != 0xff && board_id != 0xffffffffU;

	*size = 0;
	if (wifi_bdf_file[0] != 0) {
		bdf = vayu_wifi_bdf_open(wifi_bdf_file, size, board_id, "vayu_wifi_bdf_file", true);
		if (bdf == NULL) {
			say("vayu-wifi: wlfw: bdf: vayu_wifi_bdf_file=%s is not in the boot image: no "
			    "fallback, so this run tests nothing\n", wifi_bdf_file);
		}
		return bdf;
	}
	if (known) {
		snprintf(name, sizeof(name), "bdwlan.b%02x", (unsigned int)board_id);
		bdf = vayu_wifi_bdf_open(name, size, board_id, "hex board id", true);
		if (bdf != NULL) {
			return bdf;
		}
		snprintf(name, sizeof(name), "bdwlan.%02x", (unsigned int)board_id);
		bdf = vayu_wifi_bdf_open(name, size, board_id, "hex board id without b", true);
		if (bdf != NULL) {
			return bdf;
		}
		snprintf(name, sizeof(name), "bdwlan.b%u", (unsigned int)board_id);
		bdf = vayu_wifi_bdf_open(name, size, board_id, "decimal board id", true);
		if (bdf != NULL) {
			return bdf;
		}
	}
	return vayu_wifi_bdf_open("bdwlan.bin", size, board_id, "fallback", true);
}

static int
vayu_wifi_bdf_download_on(struct qmi_handle *handle, const char *who, const uint8_t *bdf,
    uint64_t size, int bdf_type, const char *what)
{
	struct vayu_wlfw_bdf_download_req_msg_v01 req;
	struct vayu_wlfw_bdf_download_resp_msg_v01 resp;
	uint64_t off = 0;
	uint32_t seg = 0;
	int ret;

	if (size == 0 || size > 0xffffffffULL) {
		say("vayu-wifi: %s: %s: bad size %llu\n", who, what, (unsigned long long)size);
		return -1;
	}
	while (off < size) {
		size_t chunk = (size - off) > VAYU_WLFW_BDF_SEGMENT_SIZE ?
		    VAYU_WLFW_BDF_SEGMENT_SIZE : (size_t)(size - off);
		uint8_t end = (off + chunk == size) ? 1 : 0;

		memset(&req, 0, sizeof(req));
		memset(&resp, 0, sizeof(resp));
		req.valid = 1;
		req.file_id_valid = 1;
		req.file_id = 0;
		req.total_size_valid = 1;
		req.total_size = (uint32_t)size;
		req.seg_id_valid = 1;
		req.seg_id = seg;
		req.data_valid = 1;
		req.data_len = (uint32_t)chunk;
		memcpy(req.data, bdf + off, chunk);
		req.end_valid = 1;
		req.end = end;
		if (bdf_type >= 0) {
			/* Only the opt-in regdb path sets this.  ath10k leaves the TLV out
			 * (qmi.c:256-263); the value 1=REGDB is a downstream hypothesis. */
			req.bdf_type_valid = 1;
			req.bdf_type = (uint8_t)bdf_type;
		}

		ret = wlfw_request_on(handle, who, "bdf_download", VAYU_WLFW_BDF_DOWNLOAD_REQ_V01,
		    VAYU_WLFW_BDF_DOWNLOAD_REQ_MAX, vayu_wlfw_bdf_download_req_msg_v01_ei, &req,
		    vayu_wlfw_bdf_download_resp_msg_v01_ei, &resp);
		if (ret < 0) {
			return ret;
		}
		if (end) {
			/* end=1 also asks the firmware for a CRC check of the BDF; a failure comes
			 * back as MALFORMED and ath10k still uses the file (qmi.c:294-307).  Log the
			 * answer even on success: it is the only evidence that the whole file and its
			 * CRC arrived, and it distinguishes a bad BDF from a BDF the firmware parsed. */
			say("vayu-wifi: %s: %s: last segment %u (%zu bytes): result %u error %u%s\n",
			    who, what, seg, chunk, (unsigned int)resp.resp.result,
			    (unsigned int)resp.resp.error,
			    resp.resp.result == VAYU_QMI_ERR_MALFORMED_MSG_V01 ?
			    " (CRC check said no; ath10k treats that as non-fatal)" : "");
		}
		/* Exactly ath10k's condition (qmi.c:300-307): the result, not resp.error, carries
		 * QMI_ERR_MALFORMED_MSG_V01, and only on the end segment is it non-fatal. */
		if (resp.resp.result != QMI_RESULT_SUCCESS_V01 &&
		    !(end && resp.resp.result == VAYU_QMI_ERR_MALFORMED_MSG_V01)) {
			say("vayu-wifi: %s: %s: segment %u rejected: result %u error %u\n",
			    who, what, seg, (unsigned int)resp.resp.result,
			    (unsigned int)resp.resp.error);
			return -1;
		}
		off += chunk;
		seg++;
	}
	say("vayu-wifi: %s: %s: %llu bytes sent in %u segments (ath10k segment size %u bytes)\n",
	    who, what, (unsigned long long)size, seg, VAYU_WLFW_BDF_SEGMENT_SIZE);
	return 0;
}

static int
vayu_wifi_bdf_download(const uint8_t *bdf, uint64_t size)
{
	return vayu_wifi_bdf_download_on(&wlfw_handle, "wlfw", bdf, size, -1, "bdf");
}

/*
 * vayu_wifi_regdb=1/2: the stock host may download regdb.bin with BDF_DOWNLOAD and the file type
 * REGDB (bdf_type TLV 0x15).  There is no regdb.bin in this boot image and the HL3.0 firmware
 * strings show a WMI regdb offload (wlan_regdb_offload_send_wmi_reg_rules, wlanmdsp.mbn @
 * 0x2bea9d), so this stays opt-in: absent file -> one line, then the normal flow.
 */
static int
vayu_wifi_regdb_download(void)
{
	const uint8_t *regdb;
	uint64_t size = 0;

	regdb = hakios_image_file("fw", "regdb.bin", &size, NULL);
	if (regdb == NULL) {
		say("vayu-wifi: wlfw: regdb: fw/regdb.bin is not in the boot image; skipped\n");
		return 0;
	}
	say("vayu-wifi: wlfw: regdb: fw/regdb.bin %llu bytes (BDF_DOWNLOAD bdf_type %u=REGDB)\n",
	    (unsigned long long)size, VAYU_WLFW_BDF_TYPE_REGDB_V01);
	return vayu_wifi_bdf_download_on(&wlfw_handle, "wlfw", regdb, size,
	    VAYU_WLFW_BDF_TYPE_REGDB_V01, "regdb");
}

static int
vayu_wifi_mac_addr_on(struct qmi_handle *handle, const char *who)
{
	/* flame wlfw.c:327: locally administered 02:50:49:58:45:4c; the value is not the point. */
	static const uint8_t mac[VAYU_WLFW_MAC_ADDR_SIZE_V01] = { 0x02, 0x50, 0x49, 0x58, 0x45, 0x4c };
	struct vayu_wlfw_mac_addr_req_msg_v01 req;
	struct vayu_wlfw_mac_addr_resp_msg_v01 resp;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	req.mac_addr_valid = 1;
	memcpy(req.mac_addr, mac, sizeof(mac));
	ret = wlfw_request_on(handle, who, "mac_addr", VAYU_WLFW_MAC_ADDR_REQ_V01,
	    VAYU_WLFW_MAC_ADDR_REQ_MAX, vayu_wlfw_mac_addr_req_msg_v01_ei, &req,
	    vayu_wlfw_mac_addr_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: %s: mac_addr %02x:%02x:%02x:%02x:%02x:%02x: result %u error %u\n", who,
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
	    (unsigned int)resp.resp.result, (unsigned int)resp.resp.error);
	return resp.resp.result == QMI_RESULT_SUCCESS_V01 ? 0 : -1;
}

static int
vayu_wifi_cal_report_on(struct qmi_handle *handle, const char *who, int stock2_shape)
{
	struct vayu_wlfw_cal_report_req_msg_v01 req;
	struct vayu_wlfw_cal_report_resp_msg_v01 resp;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	req.meta_data_len = 0;                  /* no host calibration data here */
	req.xo_cal_data_valid = 0;
	if (stock2_shape) {
		/* Stock cnss-daemon: no cal files, so TLV 0x01 has a 1-byte count 0;
		 * then its unconditional TLV 0x11 = 0 makes the body 8 bytes. */
		req.tlv11_valid = 1;
		req.tlv11 = 0;
	}
	ret = wlfw_request_on(handle, who, "cal_report", VAYU_WLFW_CAL_REPORT_REQ_V01,
	    VAYU_WLFW_CAL_REPORT_REQ_MAX, vayu_wlfw_cal_report_req_msg_v01_ei, &req,
	    vayu_wlfw_cal_report_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: %s: cal_report%s: result %u error %u\n", who,
	    stock2_shape ? " (stock2 shape: count 0 + TLV 0x11=0, 8 B)" : "",
	    (unsigned int)resp.resp.result, (unsigned int)resp.resp.error);
	return resp.resp.result == QMI_RESULT_SUCCESS_V01 ? 0 : -1;
}

static int
vayu_wifi_cal_report(void)
{
	return vayu_wifi_cal_report_on(&wlfw_handle, "wlfw", 0);
}

/*
 * vayu_wifi_probe=4/5: the two CAL_REPORT request shapes ath10k can produce and we never sent.
 * 4 fills meta_data with the cal ids the host already has cal data for (ath10k walks its
 * cal_data[] and pushes every cal_id with a total_size, ref/qmi.c:341-352); 5 sets the single
 * xo_cal_data byte that ath10k sends when the DT has qcom,xo-cal-data (snoc.c:2330-2345).  Neither
 * is claimed to be the fix: they exist so one run can rule the request shape in or out.
 */
static int
vayu_wifi_cal_report_probe(uint32_t probe)
{
	struct vayu_wlfw_cal_report_req_msg_v01 req;
	struct vayu_wlfw_cal_report_resp_msg_v01 resp;
	uint32_t i;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	if (probe == 4) {
		req.meta_data_len = 4;
		for (i = 0; i < 4; i++) {
			req.meta_data[i] = i;           /* QMI_WLFW_CAL_TEMP_IDX_0..3 */
		}
	} else if (probe == 5) {
		req.xo_cal_data_valid = 1;
		req.xo_cal_data = 0;
	}
	say("vayu-wifi: wlfw: cal_report probe %u: meta_data_len %u xo_cal_data_valid %u\n",
	    probe, (unsigned int)req.meta_data_len, (unsigned int)req.xo_cal_data_valid);
	ret = wlfw_request("cal_report", VAYU_WLFW_CAL_REPORT_REQ_V01,
	    VAYU_WLFW_CAL_REPORT_REQ_MAX, vayu_wlfw_cal_report_req_msg_v01_ei, &req,
	    vayu_wlfw_cal_report_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: wlfw: cal_report probe %u: result %u error %u\n", probe,
	    (unsigned int)resp.resp.result, (unsigned int)resp.resp.error);
	return resp.resp.result == QMI_RESULT_SUCCESS_V01 ? 0 : -1;
}

/*
 * W14 diagnostic only: the service IDs ath10k's target_service_to_ce_map_wlan uses, i.e. the
 * SVC() groups of htc.h (0x0001 RSVD_CTRL, 0x0100..0x0104 WMI, 0x0300..0x0302 HTT, 0x0500
 * TEST_RAW, 0x0600 LOG, 0xfe00 TEST_RAW streams) plus the 0,0,0 sentinel.  An ID outside this
 * set is named in the log; the QMI payload itself is not changed here.
 */
static bool
vayu_wlan_svc_id_known(uint32_t id)
{
	switch (id) {
	case 0x0000:
	case 0x0001:
	case 0x0100:
	case 0x0101:
	case 0x0102:
	case 0x0103:
	case 0x0104:
	case 0x0300:
	case 0x0301:
	case 0x0302:
	case 0x0500:
	case 0x0600:
	case 0xfe00:
		return true;
	default:
		return false;
	}
}

static int
vayu_wifi_wlan_cfg(void)
{
	struct vayu_wlfw_wlan_cfg_req_msg_v01 req;
	struct vayu_wlfw_wlan_cfg_resp_msg_v01 resp;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	req.host_version_valid = 0;
	req.tgt_cfg_valid = 1;
	req.tgt_cfg_len = (uint32_t)(sizeof(vayu_wlan_tgt_cfg) / sizeof(vayu_wlan_tgt_cfg[0]));
	memcpy(req.tgt_cfg, vayu_wlan_tgt_cfg, sizeof(vayu_wlan_tgt_cfg));
	req.svc_cfg_valid = 1;
	req.svc_cfg_len = (uint32_t)(sizeof(vayu_wlan_svc_cfg) / sizeof(vayu_wlan_svc_cfg[0]));
	memcpy(req.svc_cfg, vayu_wlan_svc_cfg, sizeof(vayu_wlan_svc_cfg));
	req.shadow_reg_valid = 1;
	req.shadow_reg_len = (uint32_t)(sizeof(vayu_wlan_shadow_reg) / sizeof(vayu_wlan_shadow_reg[0]));
	memcpy(req.shadow_reg, vayu_wlan_shadow_reg, sizeof(vayu_wlan_shadow_reg));

	/* W14: what goes out, entry by entry, so the log can name the row the firmware dislikes. */
	for (uint32_t i = 0; i < req.svc_cfg_len; i++) {
		say("vayu-wifi: wlfw: wlan_cfg: svc[%u] service %#x %s pipe %u%s\n", i,
		    vayu_wlan_svc_cfg[i].service_id,
		    vayu_wlan_svc_cfg[i].pipe_dir == 1 ? "in " : "out",
		    vayu_wlan_svc_cfg[i].pipe_num,
		    vayu_wlan_svc_id_known(vayu_wlan_svc_cfg[i].service_id) ? "" :
		    "  <- not in the ath10k WCN3990 set");
	}

	ret = wlfw_request("wlan_cfg", VAYU_WLFW_WLAN_CFG_REQ_V01, VAYU_WLFW_WLAN_CFG_REQ_MAX,
	    vayu_wlfw_wlan_cfg_req_msg_v01_ei, &req, vayu_wlfw_wlan_cfg_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: wlfw: wlan_cfg: result %u error %u (tgt %u svc %u shadow %u)\n",
	    (unsigned int)resp.resp.result, (unsigned int)resp.resp.error, req.tgt_cfg_len,
	    req.svc_cfg_len, req.shadow_reg_len);
	/* W15 H3: the CE gate needs result 0 AND error 0; a SUCCESS with an error is not 0/0. */
	return (resp.resp.result == QMI_RESULT_SUCCESS_V01 && resp.resp.error == 0) ? 0 : -1;
}

static int
vayu_wifi_wlan_mode_send(uint32_t mode, const char *name)
{
	struct vayu_wlfw_wlan_mode_req_msg_v01 req;
	struct vayu_wlfw_wlan_mode_resp_msg_v01 resp;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	req.mode = mode;
	req.hw_debug_valid = 1;
	req.hw_debug = 0;
	ret = wlfw_request("wlan_mode", VAYU_WLFW_WLAN_MODE_REQ_V01, VAYU_WLFW_WLAN_MODE_REQ_MAX,
	    vayu_wlfw_wlan_mode_req_msg_v01_ei, &req, vayu_wlfw_wlan_mode_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: wlfw: wlan_mode %s: result %u error %u\n", name,
	    (unsigned int)resp.resp.result, (unsigned int)resp.resp.error);
	/* W15 H3: same rule as wlan_cfg: only result 0 and error 0 pass the CE gate. */
	return (resp.resp.result == QMI_RESULT_SUCCESS_V01 && resp.resp.error == 0) ? 0 : -1;
}

static int
vayu_wifi_wlan_mode(void)
{
	/* normal mode, as ath10k_snoc.c (ref/qmi.c:380-425) */
	return vayu_wifi_wlan_mode_send(VAYU_WLFW_DRIVER_MODE_MISSION_V01, "MISSION");
}

/*
 * WLFW DYNAMIC_FEATURE_MASK (QMI 0x003B): in the stock kernel this is the last request before
 * the FW_READY wait, sent unconditionally right after CAP and with its answer ignored
 * (icnss.c:1042-1043; icnss_qmi.c:942-1010; mask = ICNSS_DEFAULT_FEATURE_MASK = 0x01,
 * icnss.c:68).  This file never sent it.  Whether the HL3.0 firmware gates FW_READY on it is
 * not known statically (ath10k reaches FW_READY on WCN3990 without ever sending 0x003B), so it
 * sits behind vayu_wifi_dynmask=1 and a rejection only stops the run if the link died.
 */
static int
vayu_wifi_dynamic_feature_mask(void)
{
	struct vayu_wlfw_dynamic_feature_mask_req_msg_v01 req;
	struct vayu_wlfw_dynamic_feature_mask_resp_msg_v01 resp;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	req.mask_valid = 1;
	req.mask = wifi_dynmask_bits;
	ret = wlfw_request("dynamic_feature_mask", VAYU_WLFW_DYNAMIC_FEATURE_MASK_REQ_V01,
	    VAYU_WLFW_DYNAMIC_FEATURE_MASK_REQ_MAX, vayu_wlfw_dynamic_feature_mask_req_msg_v01_ei,
	    &req, vayu_wlfw_dynamic_feature_mask_resp_msg_v01_ei, &resp);
	if (ret < 0) {
		return ret;
	}
	say("vayu-wifi: wlfw: dynamic_feature_mask %#x: result %u error %u\n",
	    (unsigned int)wifi_dynmask_bits, (unsigned int)resp.resp.result,
	    (unsigned int)resp.resp.error);
	if (resp.prev_mask_valid || resp.curr_mask_valid) {
		say("vayu-wifi: wlfw: dynamic_feature_mask: prev %#llx curr %#llx\n",
		    (unsigned long long)(resp.prev_mask_valid ? resp.prev_mask : 0),
		    (unsigned long long)(resp.curr_mask_valid ? resp.curr_mask : 0));
	}
	return resp.resp.result == QMI_RESULT_SUCCESS_V01 ? 0 : -1;
}

/*
 * W13: the QDSS clock is not an RPMh resource and not in the icnss node: it is an AOP QMP
 * message.  Mainline sm8150 wifi's second clock is <&aoss_qmp QDSS_CLK>; clk-aop-qmp.c:223
 * defines it as {class: clock, res: qdss, enable=QDSS_CLK_LEVEL_DYNAMIC=1} and its probe
 * votes it once ("Proxy vote on the QDSS clock", clk-aop-qmp.c:301-305).  Stock vayu has
 * the same driver built in (CONFIG_MSM_CLK_AOP_QMP=y, vayu_defconfig:4719), and its icnss
 * node has no clocks property at all (recovery-live.dts:11117-11136), so this is where the
 * stock phone's QDSS vote comes from.  flame needs it before the firmware calibrates
 * (HANDOFF.md:628-634: without it, PFR PC=0xb01976e8 in cal).
 */
static bool
vayu_wifi_qdss_vote(void)
{
	if (wifi_qdss == 0 || wifi_qdss_sent) {
		return true;
	}
	if (vayu_aop_send("{class: clock, res: qdss, val: 1}")) {
		wifi_qdss_sent = true;
		say("vayu-wifi: power: AOP QDSS clock \"{class: clock, res: qdss, val: 1}\" accepted%s\n",
		    wifi_pre_modem_seen ? " (pre-modem path)" : " (after the modem was loaded: no "
		    "pre-modem call happened)");
		return true;
	}
	say("vayu-wifi: power: the AOP did not take the QDSS clock vote\n");
	return false;
}

/*
 * Power on WCN3990 before any WLFW request, as ath10k_snoc's ath10k_hw_power_on()
 * and vayu's stock icnss driver do. The live DT connects vdd-cx-mx to PM8150 L1
 * (752 mV), vdd-1.8-xo to L7, vdd-1.3-rfa to L2 and vdd-3.3-ch0 to L11.
 * BT votes only the last three; no vayu driver voted the WLAN-only L1 before this.
 * RPMh order is the vendor regulator order (vayu_bt.c): +0 voltage, +8 mode, +4 enable.
 * Votes are idempotent and are deliberately not undone: BT and the modem share the rails.
 */
static bool
vayu_wifi_power_on(void)
{
	static const struct { const char *name; uint32_t mv; } rails[] = {
		{ "ldoa1", 0 },         /* PM8150 L1: vdd-cx-mx; the voltage is wifi_cxmx_mv (stock 752) */
		{ "ldoa7", 1800 },      /* PM8150 L7: vdd-1.8-xo */
		{ "ldoc2", 1304 },      /* PM8150L L2: vdd-1.3-rfa */
		{ "ldoc11", 3312 },     /* PM8150L L11: vdd-3.3-ch0 */
	};

	/*
	 * vayu_wifi_arc=LEVEL: cx.lvl is the ARC rail the WCSS/MAC sit on; mx.lvl is its
	 * parent.  The stock kernel proxies vdd_cx for the modem while it boots
	 * (sm8150.dtsi:1662-1671: RPMH_REGULATOR_LEVEL_TURBO = 384) and releases its vote
	 * afterwards; its other consumers (display, UFS, GPU) keep cx high.  This kernel has
	 * no such consumer, so the WCSS can be left on a rail level where its GDSCR never
	 * reports PWR_ON (see the W8-report).  Off by default: old behaviour = W7.
	 */
	if (wifi_arc_level != 0) {
		static const char *const arcs[2] = { "cx.lvl", "mx.lvl" };

		/* One at a time: a resource the cmd-db lacks must not cancel the other's vote. */
		for (unsigned i = 0; i < 2; i++) {
			if (vayu_rpmh_vote(&arcs[i], 1, wifi_arc_level)) {
				say("vayu-wifi: power: %s voted at level %u\n", arcs[i], wifi_arc_level);
			} else {
				say("vayu-wifi: power: the %s vote at level %u did not complete; "
				    "continuing\n", arcs[i], wifi_arc_level);
			}
		}
	}
	/*
	 * vayu_wifi_bus=1: the SNOC's BCM "SN0".  The stock kernel keeps a devbw client on the
	 * SNOC->CNOC path (sm8150.dtsi:1391-1398, qcom,src-dst-ports = <1 627>, active-only,
	 * 1 Hz), so that path never collapses.  A BCM vote is commit|valid plus the two
	 * bandwidth fields (msm_bus_fabric_rpmh.c:30-43); 1/1 is the smallest non-zero vote.
	 * HYPOTHESIS only: the exact unit is the BCM's cmd-db unit, which is logged.
	 */
	if (wifi_bus != 0) {
		uint32_t address = vayu_rpmh_address("SN0"), data = 0x60004001u;

		if (address == 0) {
			say("vayu-wifi: power: no cmd-db address for SN0 (the SNOC BCM); continuing\n");
		} else if (vayu_rpmh_send(&address, &data, 1, "vayu-wifi")) {
			say("vayu-wifi: power: SN0 at %#x: keepalive vote 1/1 accepted\n", address);
		} else {
			say("vayu-wifi: power: the SN0 keepalive vote did not complete\n");
		}
	}

	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		uint32_t address = vayu_rpmh_address(rails[i].name), commands[3], values[3];
		uint32_t mv = i == 0 ? wifi_cxmx_mv : rails[i].mv;

		if (address == 0) {
			say("vayu-wifi: power: no cmd-db address for %s; WLFW not started\n", rails[i].name);
			return false;
		}
		commands[0] = address;
		commands[1] = address + 8;
		commands[2] = address + 4;
		values[0] = mv;
		values[1] = 7;                  /* PMIC5 LDO high power mode */
		values[2] = 1;
		if (!vayu_rpmh_send(commands, values, 3, "vayu-wifi")) {
			say("vayu-wifi: power: %s vote did not complete; WLFW not started\n", rails[i].name);
			return false;
		}
		say("vayu-wifi: power: %s %u mV vote accepted\n", rails[i].name, mv);
	}
	/*
	 * The WCN3990 reference clock: Linux sm8150.dtsi wifi@18800000 has clocks = <&rpmhcc RPMH_RF_CLK2>
	 * ("cxo_ref_clk_pin"), which clk-rpmh.c:362 maps to the VRM-style resource "rfclka2" enabled with 1
	 * at +4. Without it the firmware lived through BDF-only but crashed (same PC) as soon as cal_report
	 * started the RF calibration (W3 runs 011805 vs 012553). vayu_wifi_rfclk=0 skips it.
	 */
	{
		uint32_t rfclk = 1, address, command, value = 1;

		PE_parse_boot_argn("vayu_wifi_rfclk", &rfclk, sizeof(rfclk));
		if (rfclk != 0) {
			address = vayu_rpmh_address("rfclka2");
			if (address == 0) {
				say("vayu-wifi: power: no cmd-db address for rfclka2 (RF_CLK2); continuing without it\n");
			} else {
				command = address + 4;
				if (vayu_rpmh_send(&command, &value, 1, "vayu-wifi")) {
					say("vayu-wifi: power: rfclka2 (RF_CLK2, cxo_ref_clk_pin) vote accepted\n");
				} else {
					say("vayu-wifi: power: rfclka2 vote did not complete\n");
				}
			}
		}
	}
	/*
	 * W13: QDSS after the rails and rfclka2, as flame's wlan_power() ends (wland.c:146-147).
	 * The comment that used to sit here is now above vayu_wifi_qdss_vote().
	 */
	(void)vayu_wifi_qdss_vote();
	return true;
}

/*
 * W13: flame's order, applied to this kernel's split.  In ios-flame one userland program does
 * wlan_power() and then boot_modem() (phone/wifi/wland.c:688-693); here vayu_modem.c loads the
 * modem in step_up() and vayu_wifi_start() runs only afterwards (vayu_modem.c:280), so the rail
 * votes have always happened after the modem was already running.  vayu_modem.c calls this
 * before vayu_pas_load(); with vayu_wifi_qdss=1 it votes exactly what the WLFW thread would vote
 * (the same vayu_wifi_power_on(), so ldoa1/ldoa7/ldoc2/ldoc11 + rfclka2 + the QMP QDSS message),
 * and the thread then skips the duplicate RPMh writes.  With the default 0 it does nothing at all.
 */
void
vayu_wifi_pre_modem(void)
{
	uint32_t qdss = 0;

	wifi_pre_modem_seen = true;
	PE_parse_boot_argn("vayu_wifi_qdss", &qdss, sizeof(qdss));
	if (qdss == 0) {
		return;
	}
	wifi_qdss = qdss;
	/* The power arguments are parsed again here: vayu_wifi_start() has not run yet. */
	PE_parse_boot_argn("vayu_wifi_rail", &wifi_rail, sizeof(wifi_rail));
	PE_parse_boot_argn("vayu_wifi_cxmx_mv", &wifi_cxmx_mv, sizeof(wifi_cxmx_mv));
	PE_parse_boot_argn("vayu_wifi_arc", &wifi_arc_level, sizeof(wifi_arc_level));
	PE_parse_boot_argn("vayu_wifi_bus", &wifi_bus, sizeof(wifi_bus));
	if (wifi_cxmx_mv < 600) wifi_cxmx_mv = 600;
	if (wifi_cxmx_mv > 1000) wifi_cxmx_mv = 1000;
	say("vayu-wifi: pre-modem (vayu_wifi_qdss=1): flame's power step before the modem is loaded "
	    "(wland.c:128-148); rails %s\n", wifi_rail != 0 ? "on" : "off (vayu_wifi_rail=0)");
	if (wifi_rail != 0) {
		wifi_power_pre = vayu_wifi_power_on();
		if (!wifi_power_pre) {
			say("vayu-wifi: pre-modem: a rail vote failed; the WLFW thread will try again\n");
		}
	} else {
		(void)vayu_wifi_qdss_vote();
	}
}

/* ===================================================================================================
 * SMP2P "wlan" in-bits: the two handlers stock icnss registers after cap
 *
 * Stock icnss has exactly one more host-side step between cap and the wait for FW_READY:
 * register_fw_error_notifications() and register_early_crash_notifications() (icnss.c:731-812,
 * called at :1046-1049).  They attach handlers to the inbound SMP2P entry "wlan" of the modem
 * edge: DT qcom,smp2p_map_wlan_1_in under the icnss node has interrupts-extended =
 * <&qcom,smp2p-wlan-1-in 0 0>, <&... 1 0> (live.dts:11133-11136), and the entry itself is
 * qcom,smp2p-wlan-1-in under qcom,smp2p-modem@1799000c with entry-name = "wlan"
 * (live.dts:22516-22520; that node's qcom,smem = <435 428>).  Bit 0 is
 * "qcom,smp2p-force-fatal-error", bit 1 is "qcom,smp2p-early-crash-ind".
 *
 * Linux's inbound path only reads and latches the value (smp2p.c:qcom_smp2p_notify_in,
 * :245-298); it never writes the remote's bits.  This code is read-only by default for the same
 * reason; vayu_wifi_smp2p_clear=1 is the explicit exception, for a bit left over by an earlier
 * boot of the same modem image.
 *
 * The handlers are passive: arming them sends nothing to the firmware and cannot by itself
 * unblock FW_READY.  What they add is the firmware's own crash channel, which is independent of
 * the QMI PFR path and can arrive first.  The stock early-crash handler sets ICNSS_FW_DOWN and
 * stops the wait (icnss.c:706-728); vayu_wifi_smp2p_abort=1 does the same here.
 * =================================================================================================== */
#define VAYU_WIFI_SMP2P_MAGIC   0x504d5324u     /* smp2p.c: SMP2P_MAGIC */
#define VAYU_WIFI_SMP2P_ENTRIES 16
#define VAYU_WIFI_SMP2P_HOST    1               /* vayu_modem.c:122, DT remote-pid 1 */
#define VAYU_WIFI_SMP2P_ITEM    435             /* DT qcom,smp2p-modem@1799000c: qcom,smem = <435 428> */
#define VAYU_WIFI_SMP2P_FATAL   (1u << 0)       /* qcom,smp2p-force-fatal-error */
#define VAYU_WIFI_SMP2P_CRASH   (1u << 1)       /* qcom,smp2p-early-crash-ind */

struct vayu_wifi_smp2p_words {
	uint32_t magic;
	uint32_t version;               /* smp2p.c: version in the low byte, features in the high 24 bits */
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[VAYU_WIFI_SMP2P_ENTRIES];
};

static uint32_t wifi_smp2p;                     /* vayu_wifi_smp2p=1 */
static uint32_t wifi_smp2p_item = VAYU_WIFI_SMP2P_ITEM;
static uint32_t wifi_smp2p_host = VAYU_WIFI_SMP2P_HOST;
static uint32_t wifi_smp2p_abort = 1;           /* 1: an early-crash bit ends the FW_READY wait */
static uint32_t wifi_smp2p_clear;               /* 1: clear bits found at arm time (not what Linux does) */
static bool wifi_smp2p_armed;
static bool wifi_smp2p_crashed;
static uint32_t wifi_smp2p_last;
static uint32_t wifi_smp2p_slave;
static uint32_t wifi_smp2p_changes;

/*
 * Read the "wlan" and "slave-kernel" entries of the modem's SMP2P item.  item_at is the SMEM
 * offset of the item, value_at the offset of the "wlan" value (for the optional clear).
 * Returns -1 if the item is not there or is not an SMP2P item.
 */
static int
vayu_wifi_smp2p_read(uint32_t *item_at, uint32_t *value_at, uint32_t *value, uint32_t *slave)
{
	struct vayu_wifi_smp2p_words words;
	uint32_t size = 0, at = vayu_smem_item((uint16_t)wifi_smp2p_host, wifi_smp2p_item, &size);
	uint32_t base;

	*item_at = 0;
	*value_at = 0;
	*value = 0;
	*slave = 0;
	if (at == 0 || size < sizeof(words) || !vayu_smem_peek(at, &words, sizeof(words)) ||
	    words.magic != VAYU_WIFI_SMP2P_MAGIC || words.valid > VAYU_WIFI_SMP2P_ENTRIES) {
		return -1;
	}
	*item_at = at;
	base = at + (uint32_t)__builtin_offsetof(struct vayu_wifi_smp2p_words, entry);
	for (unsigned i = 0; i < words.valid; i++) {
		if (strncmp(words.entry[i].name, "wlan", sizeof(words.entry[i].name)) == 0) {
			*value = words.entry[i].value;
			*value_at = base + i * (uint32_t)sizeof(words.entry[0]) +
			    (uint32_t)__builtin_offsetof(struct { char name[16]; uint32_t value; }, value);
		} else if (strncmp(words.entry[i].name, "slave-kernel", sizeof(words.entry[i].name)) == 0) {
			*slave = words.entry[i].value;
		}
	}
	return 0;
}

/* One line with every entry the modem has allocated: what "wlan" and the modem state are. */
static void
vayu_wifi_smp2p_arm(void)
{
	struct vayu_wifi_smp2p_words words;
	uint32_t item_at = 0, value_at = 0, value = 0, slave = 0, size = 0;
	uint32_t at = vayu_smem_item((uint16_t)wifi_smp2p_host, wifi_smp2p_item, &size);

	if (wifi_smp2p == 0 || wifi_smp2p_armed) {
		return;
	}
	wifi_smp2p_armed = true;
	if (vayu_wifi_smp2p_read(&item_at, &value_at, &value, &slave) < 0) {
		say("vayu-wifi: smp2p: item %u of host %u is not there or is not SMP2P; no wlan in-bits\n",
		    wifi_smp2p_item, wifi_smp2p_host);
		return;
	}
	if (vayu_smem_peek(at, &words, sizeof(words))) {
		char line[320];
		int used = 0;

		line[0] = 0;
		for (unsigned i = 0; i < words.valid && i < VAYU_WIFI_SMP2P_ENTRIES &&
		    used < (int)sizeof(line) - 40; i++) {
			used += snprintf(line + used, sizeof(line) - (size_t)used, " %.16s = %#x;",
			    words.entry[i].name, words.entry[i].value);
		}
		say("vayu-wifi: smp2p: item %u: version %u, %u of %u entries:%s\n", wifi_smp2p_item,
		    words.version & 0xff, words.valid, words.total, line);
	}
	say("vayu-wifi: smp2p: wlan = %#x (force-fatal-error %u, early-crash-ind %u), "
	    "modem slave-kernel = %#x; watching, read-only%s\n", value,
	    (value & VAYU_WIFI_SMP2P_FATAL) != 0, (value & VAYU_WIFI_SMP2P_CRASH) != 0, slave,
	    wifi_smp2p_clear != 0 ? "" : " (vayu_wifi_smp2p_clear=0)");
	if (wifi_smp2p_clear != 0 && (value & (VAYU_WIFI_SMP2P_FATAL | VAYU_WIFI_SMP2P_CRASH)) != 0) {
		vayu_smem_poke(value_at, value & ~(VAYU_WIFI_SMP2P_FATAL | VAYU_WIFI_SMP2P_CRASH));
		say("vayu-wifi: smp2p: cleared %#x at arm time -> %#x\n", value,
		    vayu_smem_word(value_at));
		value &= ~(VAYU_WIFI_SMP2P_FATAL | VAYU_WIFI_SMP2P_CRASH);
	}
	wifi_smp2p_last = value;
	wifi_smp2p_slave = slave;
}

/*
 * Poll the entry.  Logs a change once, and the modem's slave-kernel transition separately.
 * Returns the current bits.  wifi_smp2p_crashed latches a rising early-crash-ind (bit 1) and
 * ends the wait when vayu_wifi_smp2p_abort is set.
 */
static uint32_t
vayu_wifi_smp2p_poll(const char *where)
{
	uint32_t item_at = 0, value_at = 0, value = 0, slave = 0;
	bool crash_was = (wifi_smp2p_last & VAYU_WIFI_SMP2P_CRASH) != 0;

	if (wifi_smp2p == 0 || !wifi_smp2p_armed) {
		return 0;
	}
	if (vayu_wifi_smp2p_read(&item_at, &value_at, &value, &slave) < 0) {
		return 0;
	}
	if (value != wifi_smp2p_last) {
		say("vayu-wifi: smp2p: wlan %#x -> %#x at %s: force-fatal-error %u, early-crash-ind %u%s\n",
		    wifi_smp2p_last, value, where, (value & VAYU_WIFI_SMP2P_FATAL) != 0,
		    (value & VAYU_WIFI_SMP2P_CRASH) != 0,
		    (value & VAYU_WIFI_SMP2P_CRASH) != 0 && !crash_was ?
		    " (the stock driver treats this as FW down)" : "");
		wifi_smp2p_last = value;
		wifi_smp2p_changes++;
	}
	if (slave != wifi_smp2p_slave) {
		say("vayu-wifi: smp2p: modem slave-kernel %#x -> %#x at %s\n", wifi_smp2p_slave, slave, where);
		wifi_smp2p_slave = slave;
	}
	if ((value & VAYU_WIFI_SMP2P_CRASH) != 0 && !crash_was) {
		wifi_smp2p_crashed = true;
	}
	return value;
}

/* What the wait loops check after vayu_wifi_smp2p_poll(): true means stop waiting. */
static bool
vayu_wifi_smp2p_stop(void)
{
	return wifi_smp2p != 0 && wifi_smp2p_crashed && wifi_smp2p_abort != 0;
}

/*
 * W15-A: what the CE stage must stop on.  A pd-mapper PFR means the firmware PD
 * restarted (the QMI answers belong to a dead instance), an early-crash SMP2P
 * bit means the stock driver would also stop, and wlfw.stop is the teardown.
 * The SMP2P entry is polled live here: during the CE stage nothing else polls
 * it, so reading the latched flag alone would be stale.  Read-only and cheap
 * enough for the 1 ms poll (it logs only on a change).
 *
 * R4: fault reasons take precedence over wlfw.stop.  A stop can race a PFR or
 * an early-crash; if STOP won, the failure cleanup and the timeout diagnostic
 * would treat the block as alive and issue CE MMIO into a possibly collapsed
 * WLAN domain.  Checking PFR and the live crash bit first makes a combined
 * event set ce.block_down and skip every CE register access.  A pure STOP
 * (no fault) still returns VAYU_CE_ABORT_STOP and keeps the normal quiesce.
 */
static enum vayu_ce_abort
vayu_wifi_ce_abort(void)
{
	if (vayu_pdm_pfr_seen) {
		return VAYU_CE_ABORT_PFR;
	}
	if (wifi_smp2p != 0) {
		uint32_t bits = vayu_wifi_smp2p_poll("the CE stage");

		/* The poll latches a rising edge; also treat a level-set crash bit as down so
		 * a bit already set when the stage started cannot be missed. */
		if (vayu_wifi_smp2p_stop() ||
		    ((bits & VAYU_WIFI_SMP2P_CRASH) != 0 && wifi_smp2p_abort != 0)) {
			return VAYU_CE_ABORT_CRASH;
		}
	}
	if (wlfw.stop) {
		return VAYU_CE_ABORT_STOP;
	}
	return VAYU_CE_ABORT_NONE;
}

/*
 * vayu_wifi_reattach: after the firmware's PD restart the WLFW service comes back on a NEW port
 * (the receipt's 2026-10-24 run: "wlan_process" PFR, then service 0x45 again at port 93 while we
 * were still connected to port 86).  The instance that crashed is gone, so the outstanding
 * FW_READY can never arrive.  ath10k treats exactly this as event_server_exit + server_arrive: it
 * drops the MSA permissions, frees the board files, and re-runs the whole sequence on the new
 * connection (ref/qmi.c:906-940, :1030-1075).  This waits for the re-announcement and lets the
 * caller jump back to the first request.  Bounded by vayu_wifi_reattach_wait ms.
 */
static bool
vayu_wifi_wait_new_server(void)
{
	uint32_t waited, limit = wifi_reattach_wait_ms;

	if (wlfw.server && wlfw.port != wlfw.used_port) {
		return true;
	}
	say("vayu-wifi: wlfw: reattach %u/%u: waiting up to %u ms for the WLFW service to come back "
	    "on another port\n", wifi_reattach_runs + 1, wifi_reattach, limit);
	for (waited = 0; waited < limit; waited += 100) {
		delay_for_interval(100, NSEC_PER_MSEC);
		if (wlfw.server && wlfw.port != wlfw.used_port) {
			break;
		}
	}
	if (!wlfw.server || wlfw.port == wlfw.used_port) {
		say("vayu-wifi: wlfw: reattach: no new WLFW server after %u ms (port %u)\n", waited,
		    wlfw.port);
		return false;
	}
	say("vayu-wifi: wlfw: reattach: new WLFW server at port %u after %u ms\n", wlfw.port, waited);
	return true;
}

static void
wlfw_thread(void *arg __unused, wait_result_t wr __unused)
{
	struct vayu_wlfw_ind_register_req_msg_v01 ind_req = { 0 };
	struct vayu_wlfw_ind_register_resp_msg_v01 ind_resp = { 0 };
	struct vayu_wlfw_host_cap_req_msg_v01 cap_req = { 0 };
	struct vayu_wlfw_host_cap_resp_msg_v01 cap_resp = { 0 };
	struct vayu_wlfw_msa_info_req_msg_v01 msa_req = { 0 };
	struct vayu_wlfw_msa_info_resp_msg_v01 msa_resp = { 0 };
	struct vayu_wlfw_cap_req_msg_v01 info_req = { 0 };
	struct vayu_wlfw_cap_resp_msg_v01 info_resp = { 0 };
	uint64_t waited_ms = 0;
	int ret;
	int wlan_cfg_ret = -1, wlan_mode_ret = -1;      /* W14: -1 = not sent */
	bool wlan_tried = false;
	enum vayu_ce_result ce_result = VAYU_CE_OFF;    /* W15-A: only touched when wifi_ce != 0 */

	/*
	 * vayu_wifi_pre_modem() may already have voted everything (vayu_wifi_qdss=1 path).  Skip
	 * the duplicate RPMh writes only if the QDSS vote went out too, so a QMP message the AOP
	 * refused is retried here rather than silently lost.
	 */
	if (wifi_rail != 0 && !(wifi_power_pre && (wifi_qdss == 0 || wifi_qdss_sent)) &&
	    !vayu_wifi_power_on()) {
		wlfw_fail("power_on");
		goto out;
	}
	if (wifi_rail == 0) {
		/* No power_on() call above: the QDSS vote must still happen before the first
		 * WLFW message (unless vayu_wifi_pre_modem() already sent it). */
		(void)vayu_wifi_qdss_vote();
	}
	if (wifi_smmu == 1 && !vayu_wifi_smmu_enable()) {
		/* Keep the unmodified baseline as a control. No success or mapping is
		 * claimed; enable() already printed the exact fault and restored writes. */
		say("vayu-wifi: smmu: unavailable; continuing the unmodified WLFW control\n");
	}
	if (wifi_smp2p != 0) {
		vayu_wifi_smp2p_arm();
	}
	if (wifi_power_settle_ms != 0) {
		say("vayu-wifi: power: waiting %u ms after the last rail/clock vote "
		    "(vayu_wifi_power_settle)\n", wifi_power_settle_ms);
		delay_for_interval(wifi_power_settle_ms, NSEC_PER_MSEC);
	}

	ret = qmi_handle_init(&wlfw_handle, 8192, &wlfw_ops, wlfw_handlers);
	if (ret < 0) {
		say("vayu-wifi: wlfw: qmi_handle_init: %d\n", ret);
		wlfw_fail("qmi_handle_init");
		goto out;
	}
	ret = qmi_add_lookup(&wlfw_handle, VAYU_WLFW_SERVICE, VAYU_WLFW_VERSION, VAYU_WLFW_INSTANCE);
	if (ret < 0) {
		say("vayu-wifi: wlfw: qmi_add_lookup: %d\n", ret);
		wlfw_fail("qmi_add_lookup");
		goto out_release;
	}
	/*
	 * W12: the second client's lookup is registered before the server appears, as the stock
	 * cnss-daemon's is (it runs from boot).  Both handles then get the same NEW_SERVER.
	 */
	if (wifi_stock2 != 0) {
		ret = wlfw2_start();
		if (ret < 0) {
			wlfw_fail("stock2 client");
			goto out_release;
		}
	}

	while (!wlfw.server && !wlfw.stop && waited_ms < (uint64_t)wifi_listen_s * 1000) {
		delay_for_interval(100, NSEC_PER_MSEC);
		waited_ms += 100;
	}
	if (!wlfw.server) {
		if (!wlfw.stop) {
			say("vayu-wifi: wlfw: service 0x45 did not appear in %u s\n", wifi_listen_s);
		}
		snprintf(wlfw.summary, sizeof(wlfw.summary), "never appeared");
		goto out_release;
	}
	say("vayu-wifi: wlfw: service 0x45 at node %u port %u after %llu ms\n", wlfw.node, wlfw.port,
	    (unsigned long long)waited_ms);
	if (wifi_stock2 != 0 && !wlfw2_server) {
		uint64_t waited2 = 0;

		while (!wlfw2_server && !wlfw.stop && waited2 < 3000) {
			delay_for_interval(10, NSEC_PER_MSEC);
			waited2 += 10;
		}
		if (!wlfw2_server) {
			say("vayu-wifi: wlfw2: the second client did not get the WLFW server in 3000 ms\n");
			wlfw_fail("stock2 client server");
			goto out_release;
		}
		say("vayu-wifi: wlfw2: server at node %u port %u (second client, waited %llu ms)\n",
		    wlfw2_node, wlfw2_port, (unsigned long long)waited2);
	}
	if (wlfw.stop) {
		wlfw_fail("ind_register");
		goto out_release;
	}

wlfw_sequence:
	wlfw.attempts++;
	if (wlfw.attempts > 1) {
		/*
		 * A re-run on the new instance: the old instance's indications and answers
		 * belong to a dead connection.  ath10k frees the board files and re-reads
		 * them from scratch (ref/qmi.c:918-934, :1030-1075); here the files come
		 * from the boot image again, and the MSA permission grant is kept (the
		 * permissions do not go away when the PD restarts).
		 */
		wlfw.fw_ready = false;
		wlfw.msa_ready = false;
		wlfw.cap_ok = false;
		memset(&ind_req, 0, sizeof(ind_req));
		memset(&ind_resp, 0, sizeof(ind_resp));
		memset(&cap_req, 0, sizeof(cap_req));
		memset(&cap_resp, 0, sizeof(cap_resp));
		memset(&msa_req, 0, sizeof(msa_req));
		memset(&msa_resp, 0, sizeof(msa_resp));
		memset(&info_req, 0, sizeof(info_req));
		memset(&info_resp, 0, sizeof(info_resp));
		say("vayu-wifi: wlfw: reattach %u/%u: re-running the pre-FW_READY sequence on port %u\n",
		    wlfw.attempts - 1, wifi_reattach, wlfw.port);
	}
	wlfw.used_port = wlfw.port;

	/* 1. IND_REGISTER, as ath10k_qmi_ind_register_send_sync_msg() (ref/qmi.c:750-806). */
	ind_req.client_id_valid = 1;
	ind_req.client_id = VAYU_WLFW_CLIENT_ID;
	ind_req.fw_ready_enable_valid = 1;
	ind_req.fw_ready_enable = 1;
	ind_req.msa_ready_enable_valid = 1;
	ind_req.msa_ready_enable = 1;
	ind_req.pin_connect_result_enable_valid = 1;
	ind_req.pin_connect_result_enable = 1;
	if (wifi_stock2 != 0) {
		/*
		 * Stock K's IND_REGISTER is 26 bytes: the three u8 flags above, client_id and
		 * rejuvenate_enable=1 (icnss_qmi.c:271-281; ring body length 0x1a).  Our normal
		 * flow leaves the field invalid and sends 19 bytes.
		 */
		ind_req.rejuvenate_enable_valid = 1;
		ind_req.rejuvenate_enable = 1;
		say("vayu-wifi: wlfw: stock2: ind_register with rejuvenate_enable=1 (26 B)\n");
	}
	ret = wlfw_request("ind_register", VAYU_WLFW_IND_REGISTER_REQ_V01, VAYU_WLFW_IND_REGISTER_REQ_MAX,
	    vayu_wlfw_ind_register_req_msg_v01_ei, &ind_req, vayu_wlfw_ind_register_resp_msg_v01_ei, &ind_resp);
	if (ret < 0) {
		goto out_release;
	}
	if (ind_resp.fw_status_valid) {
		say("vayu-wifi: wlfw: ind_register: result %u error %u fw_status %#llx\n",
		    (unsigned int)ind_resp.resp.result, (unsigned int)ind_resp.resp.error,
		    (unsigned long long)ind_resp.fw_status);
		if ((ind_resp.fw_status & VAYU_WLFW_FW_READY_V01) != 0) {
			say("vayu-wifi: wlfw: ind_register: fw_status has FW_READY\n");
		}
	} else {
		say("vayu-wifi: wlfw: ind_register: result %u error %u\n", (unsigned int)ind_resp.resp.result,
		    (unsigned int)ind_resp.resp.error);
	}
	/*
	 * FW_READY_IND (0x0021) is expected only after MSA_READY -> CAP -> BDF/cal
	 * (ath10k qmi.c:798-819); ind_register's fw_status 0 is normal here.
	 * The enable flags are already set; do not wait before host_cap.
	 */
	if (wlfw.stop) {
		wlfw_fail("host_cap");
		goto out_release;
	}

	/* 2. HOST_CAP, as ath10k_qmi_host_cap_send_sync() (ref/qmi.c:653-703). */
	if (wifi_bdf == 3) {
		say("vayu-wifi: wlfw: host_cap: not sent (vayu_wifi_bdf=3: the stock icnss sends none)\n");
	} else if (wifi_hostcap != 0) {
		cap_req.daemon_support_valid = 1;
		cap_req.daemon_support = 0;
		if (wifi_hostcap >= 3) {
			/*
			 * vayu_wifi_hostcap=3: the one host_cap difference that is not cosmetic.
			 * TLV 0x14/0x15 are the firmware's "the host can send a board file /
			 * cache it" bits (wlan_firmware_service_v01.h, wlfw_host_cap_req_msg_v01).
			 * ath10k never sets them because its firmware already has a board file,
			 * while we are exactly the host that downloads one: leaving the TLV
			 * absent means "no BDF support" to the firmware.  Only fields with a
			 * certain meaning are set; nothing else changes.
			 */
			cap_req.bdf_support_valid = 1;
			cap_req.bdf_support = 1;
			cap_req.bdf_cache_support_valid = 1;
			cap_req.bdf_cache_support = 0;
			say("vayu-wifi: wlfw: host_cap: declaring bdf_support=1 bdf_cache_support=0 "
			    "(vayu_wifi_hostcap=%u)\n", wifi_hostcap);
		}
		ret = wlfw_request("host_cap", VAYU_WLFW_HOST_CAP_REQ_V01, VAYU_WLFW_HOST_CAP_REQ_MAX,
		    wifi_hostcap == 2 ? vayu_wlfw_host_cap_8bit_req_msg_v01_ei : vayu_wlfw_host_cap_req_msg_v01_ei,
		    &cap_req, vayu_wlfw_host_cap_resp_msg_v01_ei, &cap_resp);
		if (ret < 0) {
			goto out_release;
		}
		say("vayu-wifi: wlfw: host_cap: result %u error %u\n", (unsigned int)cap_resp.resp.result,
		    (unsigned int)cap_resp.resp.error);
	} else {
		say("vayu-wifi: wlfw: host_cap: not sent (vayu_wifi_hostcap=0)\n");
	}
	if (wlfw.stop) {
		wlfw_fail("msa_info");
		goto out_release;
	}

	/* 3. MSA_INFO, as ath10k_qmi_msa_mem_info_send_sync_msg() (ref/qmi.c:121-197). */
	msa_req.msa_addr = VAYU_WLFW_MSA_BASE;
	msa_req.size = wifi_msa_kb * 1024U;
	say("vayu-wifi: wlfw: msa_info: asking for %#x bytes (%u KiB, %s; stock 0x%x)\n",
	    (unsigned int)msa_req.size, (unsigned int)wifi_msa_kb,
	    msa_req.size == VAYU_WLFW_MSA_STOCK_SIZE ? "the stock value" : "not the stock value",
	    (unsigned int)VAYU_WLFW_MSA_STOCK_SIZE);
	ret = wlfw_request("msa_info", VAYU_WLFW_MSA_INFO_REQ_V01, VAYU_WLFW_MSA_INFO_REQ_MAX,
	    vayu_wlfw_msa_info_req_msg_v01_ei, &msa_req, vayu_wlfw_msa_info_resp_msg_v01_ei, &msa_resp);
	if (ret < 0) {
		goto out_release;
	}
	say("vayu-wifi: wlfw: msa_info: result %u error %u\n", (unsigned int)msa_resp.resp.result,
	    (unsigned int)msa_resp.resp.error);
	/* Linux ath10k qmi.c: msleep(20) before setup_msa_permissions(). */
	delay_for_interval(20, NSEC_PER_MSEC);
	if (msa_resp.resp.result == QMI_RESULT_SUCCESS_V01) {
		uint32_t count = msa_resp.mem_region_info_len;

		if (count > VAYU_WLFW_MAX_MEM_REG_V01) {
			count = VAYU_WLFW_MAX_MEM_REG_V01;
		}
		for (uint32_t i = 0; i < count; i++) {
			struct vayu_wlfw_memory_region_info_s_v01 *region = &msa_resp.mem_region_info[i];
			uint64_t end = region->region_addr + region->size;
			bool outside = region->region_addr < VAYU_WLFW_MSA_BASE ||
			    end < region->region_addr || end > VAYU_WLFW_MSA_END;

			say("vayu-wifi: wlfw: msa region %u: %#llx + %#x secure %u%s\n", i,
			    (unsigned long long)region->region_addr, region->size,
			    (unsigned int)region->secure_flag,
			    outside ? " OUTSIDE 0x8bc00000..0x8bd80000" : "");
			/*
			 * Linux ath10k qmi.c:36-62, :825-850: assign EVERY MSA region,
			 * secure ones included, source HLOS -> {MSS_MSA 15, WLAN 0x18},
			 * plus WLAN_CE 0x19 for non-secure; HLOS is not a destination.
			 * This happens before MSA_READY, and a failure aborts the
			 * sequence (no MSA_READY, no cap).
			 */
			if (outside) {
				say("vayu-wifi: wlfw: msa region %u is outside the stock MSA; not assigned\n", i);
			} else if (wifi_msa_fixed_perm != 0 || wlfw.attempts > 1) {
				say("vayu-wifi: wlfw: msa region %u: assign skipped (%s)\n", i,
				    wifi_msa_fixed_perm != 0 ? "fixed-perm flag" :
				    "the PD restart keeps the permissions granted in this boot");
			} else {
				int scm_ret = vayu_scm_assign_msa(region->region_addr, region->size,
				    region->secure_flag != 0);

				say("vayu-wifi: wlfw: msa region %u assign HLOS->{15,0x18%s}: %d%s\n", i,
				    region->secure_flag != 0 ? "" : ",0x19", scm_ret,
				    scm_ret == 0 ? "" : " (MSA assign failed)");
				if (scm_ret != 0) {
					wlfw_fail("msa_assign");
					goto out_release;
				}
			}
		}
	}
	if (wlfw.stop) {
		wlfw_fail("cap");
		goto out_release;
	}

	/*
	 * 3b. MSA_READY: a synchronous REQUEST 0x002E (ath10k qmi.c:199-230),
	 * not the 0x002B host indication.  Wait for result SUCCESS; on failure
	 * abort before cap (cap would be QMI_ERR_INCOMPATIBLE_STATE, 90).
	 */
	{
		struct vayu_wlfw_msa_ready_req_msg_v01 ready_req = { 0 };
		struct vayu_wlfw_msa_ready_resp_msg_v01 ready_resp = { 0 };

		ret = wlfw_request("msa_ready", VAYU_WLFW_MSA_READY_REQ_V01, VAYU_WLFW_MSA_READY_REQ_MAX,
		    vayu_wlfw_msa_ready_req_msg_v01_ei, &ready_req,
		    vayu_wlfw_msa_ready_resp_msg_v01_ei, &ready_resp);
		if (ret < 0) {
			goto out_release;
		}
		say("vayu-wifi: wlfw: msa_ready request: result %u error %u\n",
		    (unsigned int)ready_resp.resp.result, (unsigned int)ready_resp.resp.error);
		if (ready_resp.resp.result != QMI_RESULT_SUCCESS_V01) {
			wlfw_fail("msa_ready");
			goto out_release;
		}
	}
	if (wifi_stock2 != 0) {
		/*
		 * Stock order: the daemon's IND_REGISTER (0x20, 31 B) arrives 0.4 ms after
		 * MSA_READY and before either CAP (ring 186.775143 vs 186.776202).
		 */
		ret = wlfw2_ind_register();
		if (ret < 0) {
			say("vayu-wifi: wlfw2: ind_register failed; stock2 stops here\n");
			goto out_release;
		}
	}
	if (wlfw.stop) {
		wlfw_fail("cap");
		goto out_release;
	}

	if (wifi_stock2 != 0) {
		/*
		 * Stock order: the daemon's CAP (txn 2) goes out at 186.776129, the kernel's
		 * (txn 8) 0.1 ms later at 186.776217, and both answers come back at ~48 ms.
		 * Sequential here: D first, then K; the firmware accepted the same two
		 * requests in the stock capture and in every W3-W10 run.
		 */
		ret = wlfw2_cap();
		if (ret < 0) {
			say("vayu-wifi: wlfw2: cap failed; stock2 stops here\n");
			goto out_release;
		}
	}

	/* 4. CAP, as ath10k_qmi_cap_send_sync_msg() (ref/qmi.c:572-651); no SMEM write of its result. */
	if (wifi_cap != 0) {
		ret = wlfw_request("cap", VAYU_WLFW_CAP_REQ_V01, VAYU_WLFW_CAP_REQ_MAX,
		    vayu_wlfw_cap_req_msg_v01_ei, &info_req, vayu_wlfw_cap_resp_msg_v01_ei, &info_resp);
		if (ret < 0) {
			goto out_release;
		}
		say("vayu-wifi: wlfw: cap: result %u error %u\n", (unsigned int)info_resp.resp.result,
		    (unsigned int)info_resp.resp.error);
		if (info_resp.resp.result == QMI_RESULT_SUCCESS_V01) {
			char chip_id[16], chip_family[16], board_id[16], soc_id[16], fw_version[16];
			char timestamp[VAYU_WLFW_MAX_TIMESTAMP_LEN_V01 + 1];
			char build_id[VAYU_WLFW_MAX_BUILD_ID_LEN_V01 + 1];

			wlfw.cap_ok = true;
			if (info_resp.chip_info_valid) {
				snprintf(chip_id, sizeof(chip_id), "%#x", info_resp.chip_info.chip_id);
				snprintf(chip_family, sizeof(chip_family), "%#x", info_resp.chip_info.chip_family);
				wlfw.chip_id = info_resp.chip_info.chip_id;
			} else {
				snprintf(chip_id, sizeof(chip_id), "?");
				snprintf(chip_family, sizeof(chip_family), "?");
			}
			if (info_resp.board_info_valid) {
				snprintf(board_id, sizeof(board_id), "%#x", info_resp.board_info.board_id);
				wlfw.board_id = info_resp.board_info.board_id;
			} else {
				snprintf(board_id, sizeof(board_id), "?");
			}
			if (info_resp.soc_info_valid) {
				snprintf(soc_id, sizeof(soc_id), "%#x", info_resp.soc_info.soc_id);
				wlfw.soc_id = info_resp.soc_info.soc_id;
			} else {
				snprintf(soc_id, sizeof(soc_id), "?");
			}
			if (info_resp.fw_version_info_valid) {
				info_resp.fw_version_info.fw_build_timestamp[VAYU_WLFW_MAX_TIMESTAMP_LEN_V01] = 0;
				snprintf(timestamp, sizeof(timestamp), "%s", info_resp.fw_version_info.fw_build_timestamp);
				snprintf(fw_version, sizeof(fw_version), "%#x", info_resp.fw_version_info.fw_version);
			} else {
				snprintf(timestamp, sizeof(timestamp), "?");
				snprintf(fw_version, sizeof(fw_version), "?");
			}
			if (info_resp.fw_build_id_valid) {
				info_resp.fw_build_id[VAYU_WLFW_MAX_BUILD_ID_LEN_V01] = 0;
				snprintf(build_id, sizeof(build_id), "%s", info_resp.fw_build_id);
			} else {
				snprintf(build_id, sizeof(build_id), "?");
			}
			say("vayu-wifi: wlfw: cap: chip_id %s chip_family %s board_id %s soc_id %s\n", chip_id,
			    chip_family, board_id, soc_id);
			say("vayu-wifi: wlfw: cap: fw_version %s fw_build_timestamp %s fw_build_id %s\n",
			    fw_version, timestamp, build_id);
		}
	} else {
		say("vayu-wifi: wlfw: cap: not sent (vayu_wifi_cap=0)\n");
	}
	/*
	 * 4b. DYNAMIC_FEATURE_MASK (QMI 0x003B), the stock icnss's only message between CAP and
	 * its FW_READY wait (icnss.c:1042).  Off by default: every W5-W9 run was without it.
	 */
	if (wifi_dynmask != 0) {
		if (!wlfw.cap_ok) {
			say("vayu-wifi: wlfw: dynamic_feature_mask: not sent (CAP did not succeed)\n");
		} else if (vayu_wifi_dynamic_feature_mask() < 0 && !wlfw.stop) {
			say("vayu-wifi: wlfw: dynamic_feature_mask rejected; going on (the stock icnss "
			    "ignores the answer too)\n");
		}
		if (wlfw.stop) {
			wlfw_fail("dynamic_feature_mask");
			goto out_release;
		}
	}

	if (wifi_bdf != 0) {
		uint64_t waited_ms;
		const uint8_t *bdf;
		uint64_t bdf_size = 0;
		/*
		 * vayu_wifi_bdf=3 is the stock flow: the vayu icnss driver sends no HOST_CAP, no
		 * BDF_DOWNLOAD and no CAL_REPORT before FW_READY (icnss.c:955-1050 ends at CAP), so
		 * nothing is downloaded here and the run only watches for FW_READY and for the
		 * firmware's own PFR.
		 */
		bool stock_flow = wifi_bdf == 3;
		bool no_host_bdf = wifi_bdf == 2 || stock_flow;
		uint32_t fw_ready_ms = wifi_fw_ready_wait_ms != 0 ? wifi_fw_ready_wait_ms :
		    (wifi_stock2 != 0 ? 30000 :
		    (no_host_bdf ? wifi_bdf_wait_ms : (wifi_cal_report != 0 ? wifi_cal_wait_ms : 10000)));
		const char *fw_wait_why = wifi_fw_ready_wait_ms != 0 ? "vayu_wifi_fw_ready_wait" :
		    (wifi_stock2 != 0 ? "vayu_wifi_stock2 (30 s)" :
		    (no_host_bdf ? "vayu_wifi_bdf_wait" :
		    (wifi_cal_report != 0 ? "vayu_wifi_cal_wait" : "the BDF-only default")));

		say("vayu-wifi: wlfw: post-cap wait: %u ms (%s)\n", fw_ready_ms, fw_wait_why);

		wlfw_stage = "waiting for MSA_READY";
		for (waited_ms = 0; !wlfw.msa_ready && !wlfw.stop && !vayu_wifi_smp2p_stop() &&
		    waited_ms < wifi_msa_wait_ms; waited_ms += 100) {
			delay_for_interval(100, NSEC_PER_MSEC);
			if (wifi_smp2p != 0) {
				vayu_wifi_smp2p_poll("waiting for MSA_READY");
			}
		}
		say("vayu-wifi: wlfw: MSA_READY indication %s (waited up to %u ms)\n",
		    wlfw.msa_ready ? "seen" : "not seen", wifi_msa_wait_ms);
		if (!wlfw.msa_ready && vayu_wifi_smp2p_stop()) {
			say("vayu-wifi: wlfw: MSA_READY wait ended early: the SMP2P wlan early-crash-ind is set\n");
			wlfw_fail("early_crash (smp2p wlan)");
			goto out_release;
		}
		if (!wlfw.msa_ready) {
			wlfw_fail("msa_ready_ind");
			goto out_release;
		}
		if (no_host_bdf) {
			/*
			 * vayu_wifi_bdf=2: the firmware gets no board file from us and no cal_report.
			 * With bdf=1 it dies about 10 ms after cal_report (PFR of wlan_process, RT 0x4d,
			 * PC 0xb0193960).  This run says whether the host's BDF (or the cal report that
			 * follows it) is what kills it: no BDF, no cal_report, and the PFR line names the
			 * stage the host was in.
			 */
			if (stock_flow) {
				wlfw_stage = "STOCK FLOW: waiting for FW_READY";
				say("vayu-wifi: wlfw: STOCK FLOW: no host_cap, no board file, no cal_report "
				    "(as the stock icnss); watching FW_READY for %u ms\n", fw_ready_ms);
			} else {
				wlfw_stage = "NO HOST BDF: waiting for FW_READY";
				say("vayu-wifi: wlfw: NO HOST BDF: no board file is downloaded and no cal_report is "
				    "sent; watching FW_READY for %u ms\n", fw_ready_ms);
			}
		} else if (wifi_stock2 != 0) {
			/*
			 * W12 stock sequence, part 2: the daemon (second client) downloads
			 * bdwlan.bin with the bdf_type TLV = 0 and sends CAL_REPORT.  The ring
			 * has four 0x1826 segments and one 0x6fe = 26328 B, then an 8-byte
			 * CAL_REPORT; K sends only DYNAMIC_FEATURE_MASK in between.
			 */
			wlfw_stage = "stock2: BDF from the second client";
			bdf = vayu_wifi_bdf_file(wlfw.board_id, &bdf_size);
			if (bdf == NULL) {
				say("vayu-wifi: wlfw2: stock2: no fw/bdwlan.b<board> or "
				    "fw/bdwlan.bin in the boot image\n");
				wlfw_fail("stock2 bdf_file");
				goto out_release;
			}
			say("vayu-wifi: wlfw2: stock2: bdwlan %llu bytes, bdf_type TLV 0x15=0, "
			    "segments 6144 x N + last\n", (unsigned long long)bdf_size);
			ret = vayu_wifi_bdf_download_on(&wlfw2_handle, "wlfw2", bdf, bdf_size, 0,
			    "stock2 bdf");
			if (ret < 0) {
				wlfw_fail("stock2 bdf_download");
				goto out_release;
			}
			wlfw_stage = "stock2: CAL_REPORT from the second client";
			/*
			 * W13: vayu_wifi_mac=1 sends 0x33 on the client that sends the cal
			 * report, i.e. here the cnss-daemon role, so the order is flame's
			 * (wlfw.c:325-334: MAC then CAL_REPORT).
			 */
			if (wifi_mac != 0) {
				(void)vayu_wifi_mac_addr_on(&wlfw2_handle, "wlfw2");
			}
			if (vayu_wifi_cal_report_on(&wlfw2_handle, "wlfw2", 1) < 0) {
				say("vayu-wifi: wlfw2: stock2: cal_report failed; waiting for "
				    "FW_READY anyway\n");
			}
		} else if (wifi_probe == 1) {
			/*
			 * Probe 1: CAL_REPORT and no host board file at all.  Every crash run so
			 * far gave the firmware both, so this is the only way to tell "the cal
			 * path alone kills it" (PFR stays) from "the board file we feed into the
			 * cal is the trigger" (PFR goes away).
			 */
			wlfw_stage = "cal_report (no host BDF)";
			say("vayu-wifi: wlfw: probe 1: CAL_REPORT without any board file; then wait %u ms\n",
			    fw_ready_ms);
			if (vayu_wifi_cal_report() < 0) {
				say("vayu-wifi: wlfw: probe 1: cal_report failed; waiting for FW_READY anyway\n");
			}
		} else {
			wlfw_stage = "bdf_file";
			bdf = vayu_wifi_bdf_file(wlfw.board_id, &bdf_size);
			if (bdf == NULL) {
				say("vayu-wifi: wlfw: bdf: no fw/bdwlan.b<board> or fw/bdwlan.bin in the boot image\n");
				wlfw_fail("bdf_file");
				goto out_release;
			}
			ret = vayu_wifi_bdf_download(bdf, bdf_size);
			if (ret < 0) {
				wlfw_fail("bdf_download");
				goto out_release;
			}
			if (wifi_regdb == 1) {
				if (vayu_wifi_regdb_download() < 0) {
					say("vayu-wifi: wlfw: regdb download failed; going on\n");
				}
			}
			if (wifi_probe == 2) {
				/*
				 * Probe 2: hand the firmware the CE/MAC configuration before it
				 * calibrates.  The crash store is a MAC block register write in WHAL's
				 * attach path; if that path needs the host's WLAN_CFG tables first,
				 * this moves the PFR (or removes it).
				 */
				wlfw_stage = "wlan_cfg+mode before cal";
				say("vayu-wifi: wlfw: probe 2: WLAN_CFG + WLAN_MODE MISSION before CAL_REPORT\n");
				if (vayu_wifi_wlan_cfg() < 0) {
					say("vayu-wifi: wlfw: probe 2: wlan_cfg rejected; going on\n");
				}
				if (vayu_wifi_wlan_mode() < 0) {
					say("vayu-wifi: wlfw: probe 2: wlan_mode rejected; going on\n");
				}
			} else if (wifi_probe == 3) {
				wlfw_stage = "wlan_mode CALIBRATION before cal";
				say("vayu-wifi: wlfw: probe 3: WLAN_MODE CALIBRATION (%u) before CAL_REPORT\n",
				    (unsigned int)VAYU_WLFW_DRIVER_MODE_CALIBRATION_V01);
				if (vayu_wifi_wlan_mode_send(VAYU_WLFW_DRIVER_MODE_CALIBRATION_V01,
				    "CALIBRATION") < 0) {
					say("vayu-wifi: wlfw: probe 3: wlan_mode CALIBRATION rejected; going on\n");
				}
			}
			if (wifi_probe >= 4) {
				wlfw_stage = "cal_report probe";
				if (vayu_wifi_cal_report_probe(wifi_probe) < 0) {
					say("vayu-wifi: wlfw: cal_report probe %u failed; waiting for FW_READY "
					    "anyway\n", wifi_probe);
				}
			} else if (wifi_cal_report != 0) {
				/* W13: flame's order, MAC_ADDR (0x33) then CAL_REPORT (wlfw.c:325-334). */
				if (wifi_mac != 0) {
					(void)vayu_wifi_mac_addr_on(&wlfw_handle, "wlfw");
				}
				if (vayu_wifi_cal_report() < 0) {
					say("vayu-wifi: wlfw: cal_report failed; waiting for FW_READY anyway\n");
				}
			} else {
				say("vayu-wifi: wlfw: cal_report skipped (vayu_wifi_cal_report=0): BDF only, "
				    "then wait for FW_READY\n");
			}
			if (wifi_regdb == 2) {
				if (vayu_wifi_regdb_download() < 0) {
					say("vayu-wifi: wlfw: regdb download failed; going on\n");
				}
			}
		}
		wlfw_stage = "waiting for FW_READY";
		for (waited_ms = 0; !wlfw.fw_ready && !wlfw.stop && !vayu_wifi_smp2p_stop() &&
		    waited_ms < fw_ready_ms; waited_ms += 100) {
			delay_for_interval(100, NSEC_PER_MSEC);
			if (wifi_smp2p != 0) {
				vayu_wifi_smp2p_poll("waiting for FW_READY");
			}
		}
		say("vayu-wifi: wlfw: FW_READY indication %s (waited up to %u ms%s)\n",
		    wlfw.fw_ready ? "seen" : "not seen", fw_ready_ms,
		    wifi_stock2 != 0 ? ", stock2" :
		    (stock_flow ? ", stock flow" : (no_host_bdf ? ", no host BDF" : "")));
		if (!wlfw.fw_ready) {
			say("vayu-wifi: wlfw: timeout summary: stage \"%s\", MSA_READY %u, SMP2P changes %u, "
			    "MSA %u KiB, bdf=%u, cal_report=%u, waited %llu ms\n", wlfw_stage,
			    wlfw.msa_ready ? 1 : 0, wifi_smp2p_changes, wifi_msa_kb, wifi_bdf,
			    wifi_cal_report, (unsigned long long)waited_ms);
		}
		if (!wlfw.fw_ready && vayu_wifi_smp2p_stop()) {
			say("vayu-wifi: wlfw: FW_READY wait ended early: the SMP2P wlan early-crash-ind is set; "
			    "the stock icnss also stops waiting on this bit\n");
			wlfw_fail("early_crash (smp2p wlan)");
			goto out_release;
		}
		if (!wlfw.fw_ready && wifi_stock2 == 0 && wifi_reattach != 0 &&
		    wlfw.attempts <= wifi_reattach && vayu_wifi_wait_new_server()) {
			wifi_reattach_runs++;
			goto wlfw_sequence;
		}
		if (!wlfw.fw_ready) {
			wlfw_fail(wifi_stock2 != 0 ? "fw_ready (stock2)" :
			    (stock_flow ? "fw_ready (stock flow)" :
			    (no_host_bdf ? "fw_ready (no host BDF)" : "fw_ready")));
			if (wifi_stock2 != 0) {
				say("vayu-wifi: wlfw2: stock2: no FW_READY; the PFR line says whether the "
				    "firmware died, and the qmi census says what it asked for\n");
			} else if (stock_flow) {
				say("vayu-wifi: wlfw: stock flow: no FW_READY either; the PFR line above says "
				    "whether the firmware died, and the qmi census says what it asked for\n");
			} else if (no_host_bdf) {
				say("vayu-wifi: wlfw: no host BDF: no FW_READY either; the PFR line above "
				    "says which stage the host was in when the firmware went\n");
			}
			goto out_release;
		}
		if (wifi_stock2 == 0 && wifi_wlan != 0) {
			wlan_tried = true;
			wlan_cfg_ret = vayu_wifi_wlan_cfg();
			if (wlan_cfg_ret < 0) {
				say("vayu-wifi: wlfw: wlan_cfg rejected (host CE config): %d\n",
				    wlan_cfg_ret);
			}
			wlan_mode_ret = vayu_wifi_wlan_mode();
			if (wlan_mode_ret < 0) {
				say("vayu-wifi: wlfw: wlan_mode rejected: %d\n", wlan_mode_ret);
			}
			if (wifi_ce != 0) {
				/*
				 * W15-A.  Only after both QMI answers are 0/0; the stage itself
				 * waits 100 ms after MISSION, requires a stage-1 context that this
				 * driver installed, and refuses on a PFR/early-crash/stop.  At most
				 * one attempt per boot, and no CE register is written before every
				 * gate passed.
				 */
				ce_result = vayu_wifi_ce_stage(
				    wlan_cfg_ret == 0 && wlan_mode_ret == 0, vayu_wifi_ce_abort);
			}
		}
		if (wifi_stock2 != 0) {
			snprintf(wlfw.summary, sizeof(wlfw.summary),
			    "stock2: FW_READY, chip %#x board %#x soc %#x", wlfw.chip_id,
			    wlfw.board_id, wlfw.soc_id);
			say("vayu-wifi: wlfw2: stock2: FW_READY after the byte-for-byte stock sequence "
			    "(K: ind_register rejuv, msa 0x180000, cap, dynmask 0x1; D: ind_register "
			    "6 flags, cap, bdf_type=0, cal 8 B).  Stock icnss sends no WLAN_CFG/MODE; "
			    "skipped.\n");
		} else if (stock_flow) {
			snprintf(wlfw.summary, sizeof(wlfw.summary),
			    "stock flow: FW_READY, wlan sent: chip %#x board %#x soc %#x", wlfw.chip_id,
			    wlfw.board_id, wlfw.soc_id);
			say("vayu-wifi: wlfw: stock flow: the firmware reached FW_READY with no host_cap, no "
			    "board file and no cal_report, which is all the stock host gives it\n");
		} else if (no_host_bdf) {
			snprintf(wlfw.summary, sizeof(wlfw.summary),
			    "no host BDF: FW_READY, wlan sent: chip %#x board %#x soc %#x", wlfw.chip_id,
			    wlfw.board_id, wlfw.soc_id);
			say("vayu-wifi: wlfw: no host BDF: the firmware reached FW_READY without our board "
			    "file, so the bdf=1 crash is in the BDF or in cal_report, not in what follows\n");
		} else {
			snprintf(wlfw.summary, sizeof(wlfw.summary),
			    "BDF+wlan: chip %#x board %#x soc %#x", wlfw.chip_id, wlfw.board_id, wlfw.soc_id);
			/*
			 * W14: no fake "firmware side done".  Only the two QMI answers this thread
			 * actually saw are reported; a PFR arrives out of band (pd-mapper) and can
			 * still follow a result 0/0, so no line here claims its absence.
			 */
			if (!wlan_tried) {
				say("vayu-wifi: wlfw: firmware side INCOMPLETE: %s, WLAN_CFG and "
				    "WLAN_MODE MISSION were NOT sent (vayu_wifi_wlan=0): no host CE "
				    "data path, no internet\n",
				    wifi_cal_report != 0 ? "BDF and cal_report were sent" :
				    "BDF was sent, cal_report was skipped");
			} else if (wlan_cfg_ret == 0 && wlan_mode_ret == 0) {
				if (wifi_ce == 0) {
					say("vayu-wifi: wlfw: firmware side: BDF, %s, WLAN_CFG and WLAN_MODE "
					    "MISSION accepted (QMI result 0/0); the host WCN3990/SNOC/CE/ath10k "
					    "data path is not implemented: no HTC READY, no internet.  A QMI ack "
					    "is not proof that no PFR follows.\n",
					    wifi_cal_report != 0 ? "cal_report" : "no cal_report");
				} else if (ce_result == VAYU_CE_READY && vayu_wifi_ce_ready() != NULL) {
					const struct vayu_htc_ready *rd = vayu_wifi_ce_ready();

					say("vayu-wifi: wlfw: firmware side: BDF, %s, WLAN_CFG and WLAN_MODE "
					    "MISSION accepted (QMI result 0/0); host CE path: real HTC READY on "
					    "ce2 (credits %u x %u, %u endpoints, %u-byte frame); W15-A stops "
					    "there: no HTC CONNECT, no WMI, no scan, no internet.  A QMI ack and "
					    "an HTC READY are not proof that no PFR follows.\n",
					    wifi_cal_report != 0 ? "cal_report" : "no cal_report",
					    (unsigned int)rd->credits, (unsigned int)rd->credit_size,
					    (unsigned int)rd->max_endpoints, (unsigned int)rd->frame_len);
				} else {
					say("vayu-wifi: wlfw: firmware side: BDF, %s, WLAN_CFG and WLAN_MODE "
					    "MISSION accepted (QMI result 0/0); host CE path: %s; no HTC CONNECT, "
					    "no WMI, no scan, no internet.  The ce: lines say where it stopped.\n",
					    wifi_cal_report != 0 ? "cal_report" : "no cal_report",
					    vayu_wifi_ce_result_name(ce_result));
				}
			} else {
				say("vayu-wifi: wlfw: firmware side INCOMPLETE: BDF, %s, WLAN_CFG rc %d, "
				    "WLAN_MODE MISSION rc %d%s: the firmware did not accept the host "
				    "CE/service map: no HTC READY, no internet\n",
				    wifi_cal_report != 0 ? "cal_report" : "no cal_report", wlan_cfg_ret,
				    wlan_mode_ret, vayu_wifi_smp2p_stop() ?
				    ", SMP2P early-crash-ind is set" : "");
			}
		}
	} else {
		snprintf(wlfw.summary, sizeof(wlfw.summary), "answered: chip %#x board %#x soc %#x",
		    wlfw.chip_id, wlfw.board_id, wlfw.soc_id);
		say("vayu-wifi: wlfw: stopped after cap (vayu_wifi_bdf=0); no BDF, no wlan mode\n");
	}

out_release:
	if (wlfw2_live) {
		qmi_handle_release(&wlfw2_handle);
		wlfw2_live = false;
	}
	qmi_handle_release(&wlfw_handle);
out:
	wlfw.running = false;
	thread_terminate(current_thread());
}

/* ===================================================================================================
 * vayu_wifi_start / vayu_wifi_stop
 * =================================================================================================== */

void
vayu_wifi_start(struct vayu_qrtr *q, uint32_t listen_s)
{
	thread_t thread;
	int ret;

	(void)q;                                /* the QMI helpers' transport is already attached to it */
	if (wifi_started) {
		say("vayu-wifi: already started\n");
		return;
	}
	wifi_level = 0;
	wifi_hostcap = 1;
	wifi_cap = 1;
	wifi_bdf = 1;
	wifi_wlan = 1;
	wifi_cal_report = 1;
	wifi_probe = 0;
	wifi_reattach = 0;
	wifi_reattach_wait_ms = 20000;
	wifi_reattach_runs = 0;
	wifi_regdb = 0;
	wifi_bdf_wait_ms = 15000;
	wifi_bdf_file[0] = 0;
	wifi_msa_kb = 1024;
	wifi_stock2 = 0;
	PE_parse_boot_argn("vayu_wifi", &wifi_level, sizeof(wifi_level));
	PE_parse_boot_argn("vayu_wifi_stock2", &wifi_stock2, sizeof(wifi_stock2));
	PE_parse_boot_argn("vayu_wifi_qdss", &wifi_qdss, sizeof(wifi_qdss));
	PE_parse_boot_argn("vayu_wifi_mac", &wifi_mac, sizeof(wifi_mac));
	PE_parse_boot_argn("vayu_wifi_hostcap", &wifi_hostcap, sizeof(wifi_hostcap));
	PE_parse_boot_argn("vayu_wifi_cap", &wifi_cap, sizeof(wifi_cap));
	PE_parse_boot_argn("vayu_wifi_bdf", &wifi_bdf, sizeof(wifi_bdf));
	PE_parse_boot_argn("vayu_wifi_bdf_wait", &wifi_bdf_wait_ms, sizeof(wifi_bdf_wait_ms));
	PE_parse_boot_argn("vayu_wifi_wlan", &wifi_wlan, sizeof(wifi_wlan));
	PE_parse_boot_argn("vayu_wifi_cal_report", &wifi_cal_report, sizeof(wifi_cal_report));
	PE_parse_boot_argn("vayu_wifi_probe", &wifi_probe, sizeof(wifi_probe));
	PE_parse_boot_argn("vayu_wifi_reattach", &wifi_reattach, sizeof(wifi_reattach));
	PE_parse_boot_argn("vayu_wifi_reattach_wait", &wifi_reattach_wait_ms,
	    sizeof(wifi_reattach_wait_ms));
	if (wifi_probe > 5) wifi_probe = 0;
	if (wifi_reattach > 3) wifi_reattach = 3;
	if (wifi_reattach_wait_ms < 1000) wifi_reattach_wait_ms = 1000;
	if (wifi_reattach_wait_ms > 60000) wifi_reattach_wait_ms = 60000;
	PE_parse_boot_argn("vayu_wifi_regdb", &wifi_regdb, sizeof(wifi_regdb));
	PE_parse_boot_argn("vayu_wifi_dynmask", &wifi_dynmask, sizeof(wifi_dynmask));
	PE_parse_boot_argn("vayu_wifi_dynmask_bits", &wifi_dynmask_bits, sizeof(wifi_dynmask_bits));
	PE_parse_boot_argn("vayu_wifi_bdf_file", wifi_bdf_file, sizeof(wifi_bdf_file));
	PE_parse_boot_argn("vayu_wifi_msa_kb", &wifi_msa_kb, sizeof(wifi_msa_kb));
	if (wifi_msa_kb < VAYU_WLFW_MSA_MIN_SIZE / 1024U) {
		wifi_msa_kb = VAYU_WLFW_MSA_MIN_SIZE / 1024U;
	}
	if (wifi_msa_kb > VAYU_WLFW_MSA_STOCK_SIZE / 1024U) {
		wifi_msa_kb = VAYU_WLFW_MSA_STOCK_SIZE / 1024U;
	}
	if (wifi_bdf_file[0] == 0) {
		PE_parse_boot_argn("vayu_wifi_bdf_name", wifi_bdf_file, sizeof(wifi_bdf_file));
	}
	PE_parse_boot_argn("vayu_msa_fixed_perm", &wifi_msa_fixed_perm, sizeof(wifi_msa_fixed_perm));
	PE_parse_boot_argn("vayu_wifi_rail", &wifi_rail, sizeof(wifi_rail));
	PE_parse_boot_argn("vayu_wifi_smmu", &wifi_smmu, sizeof(wifi_smmu));
	PE_parse_boot_argn("vayu_wifi_ce", &wifi_ce, sizeof(wifi_ce));
	PE_parse_boot_argn("vayu_wifi_smp2p", &wifi_smp2p, sizeof(wifi_smp2p));
	PE_parse_boot_argn("vayu_wifi_smp2p_item", &wifi_smp2p_item, sizeof(wifi_smp2p_item));
	PE_parse_boot_argn("vayu_wifi_smp2p_host", &wifi_smp2p_host, sizeof(wifi_smp2p_host));
	PE_parse_boot_argn("vayu_wifi_smp2p_abort", &wifi_smp2p_abort, sizeof(wifi_smp2p_abort));
	PE_parse_boot_argn("vayu_wifi_smp2p_clear", &wifi_smp2p_clear, sizeof(wifi_smp2p_clear));
	if (wifi_smp2p_item == 0 || wifi_smp2p_item > 0xffff) {
		wifi_smp2p_item = VAYU_WIFI_SMP2P_ITEM;
	}
	if (wifi_smp2p_host > 0xff) {
		wifi_smp2p_host = VAYU_WIFI_SMP2P_HOST;
	}
	PE_parse_boot_argn("vayu_wifi_fw_ready_wait", &wifi_fw_ready_wait_ms, sizeof(wifi_fw_ready_wait_ms));
	PE_parse_boot_argn("vayu_wifi_cal_wait", &wifi_cal_wait_ms, sizeof(wifi_cal_wait_ms));
	PE_parse_boot_argn("vayu_wifi_msa_wait", &wifi_msa_wait_ms, sizeof(wifi_msa_wait_ms));
	PE_parse_boot_argn("vayu_wifi_power_settle", &wifi_power_settle_ms, sizeof(wifi_power_settle_ms));
	PE_parse_boot_argn("vayu_wifi_arc", &wifi_arc_level, sizeof(wifi_arc_level));
	PE_parse_boot_argn("vayu_wifi_cxmx_mv", &wifi_cxmx_mv, sizeof(wifi_cxmx_mv));
	PE_parse_boot_argn("vayu_wifi_bus", &wifi_bus, sizeof(wifi_bus));
	PE_parse_boot_argn("vayu_wifi_census", &wifi_census, sizeof(wifi_census));
	if (wifi_cxmx_mv < 600) wifi_cxmx_mv = 600;
	if (wifi_cxmx_mv > 1000) wifi_cxmx_mv = 1000;
	if (wifi_fw_ready_wait_ms > 180000) wifi_fw_ready_wait_ms = 180000;
	if (wifi_cal_wait_ms < 1000) wifi_cal_wait_ms = 1000;
	if (wifi_cal_wait_ms > 180000) wifi_cal_wait_ms = 180000;
	if (wifi_msa_wait_ms < 100) wifi_msa_wait_ms = 100;
	if (wifi_msa_wait_ms > 30000) wifi_msa_wait_ms = 30000;
	if (wifi_power_settle_ms > 5000) wifi_power_settle_ms = 5000;
	/*
	 * W12 stock2: force the knobs that are part of the stock sequence.  The stock icnss
	 * sends no HOST_CAP (0x34), always sends DYNAMIC_FEATURE_MASK 0x1 after CAP, and gets
	 * 0x180000 of MSA; the BDF/CAL path is used by the second client, so the kernel path
	 * must keep wifi_bdf=1 (not 3, whose "stock flow" skips the section entirely).
	 */
	if (wifi_stock2 != 0) {
		say("vayu-wifi: stock2: hostcap %u->0, dynmask %u->1, msa_kb %u->%u, bdf %u->1, "
		    "wlan after FW_READY disabled\n", wifi_hostcap, wifi_dynmask, wifi_msa_kb,
		    VAYU_WLFW_MSA_STOCK_SIZE / 1024U, wifi_bdf);
		wifi_hostcap = 0;
		wifi_dynmask = 1;
		wifi_msa_kb = VAYU_WLFW_MSA_STOCK_SIZE / 1024U;
		wifi_bdf = 1;
		if (wifi_reattach != 0) {
			say("vayu-wifi: stock2: vayu_wifi_reattach=%u ignored (the second client's "
			    "port would not follow a new server)\n", wifi_reattach);
			wifi_reattach = 0;
		}
		if (wifi_ce != 0) {
			say("vayu-wifi: stock2: vayu_wifi_ce=%u ignored (the CE stage never runs in "
			    "the stock2 flow)\n", wifi_ce);
			wifi_ce = 0;
		}
	}
	if (wifi_ce != 0 && wifi_smmu != 1) {
		say("vayu-wifi: ce: vayu_wifi_ce=1 without vayu_wifi_smmu=1: the CE stage will "
		    "refuse to touch a register (no DMA)\n");
	}
	wifi_listen_s = listen_s;
	wlfw_stage = "start";
	say("vayu-wifi: vayu_wifi=%u vayu_wifi_hostcap=%u vayu_wifi_cap=%u vayu_wifi_bdf=%u "
	    "vayu_wifi_bdf_wait=%u vayu_wifi_wlan=%u vayu_wifi_cal_report=%u vayu_wifi_regdb=%u "
	    "vayu_msa_fixed_perm=%u vayu_wifi_rail=%u vayu_wifi_smmu=%u vayu_wifi_ce=%u "
	    "vayu_wifi_smp2p=%u "
	    "smp2p_item=%u smp2p_host=%u smp2p_abort=%u smp2p_clear=%u vayu_wifi_msa_kb=%u "
	    "fw_ready_wait=%u cal_wait=%u msa_wait=%u power_settle=%u bdf_file=%s listen=%u s "
	    "probe=%u reattach=%u reattach_wait=%u arc=%u cxmx_mv=%u bus=%u census=%u "
	    "dynmask=%u dynmask_bits=%#x stock2=%u qdss=%u mac=%u\n",
	    wifi_level, wifi_hostcap, wifi_cap, wifi_bdf, wifi_bdf_wait_ms, wifi_wlan, wifi_cal_report,
	    wifi_regdb, wifi_msa_fixed_perm, wifi_rail, wifi_smmu, wifi_ce, wifi_smp2p,
	    wifi_smp2p_item,
	    wifi_smp2p_host, wifi_smp2p_abort, wifi_smp2p_clear, wifi_msa_kb, wifi_fw_ready_wait_ms,
	    wifi_cal_wait_ms, wifi_msa_wait_ms, wifi_power_settle_ms,
	    wifi_bdf_file[0] != 0 ? wifi_bdf_file : "(default)", listen_s, wifi_probe, wifi_reattach,
	    wifi_reattach_wait_ms, wifi_arc_level, wifi_cxmx_mv, wifi_bus, wifi_census,
	    wifi_dynmask, wifi_dynmask_bits, wifi_stock2, wifi_qdss, wifi_mac);
	if (wifi_level == 0) {
		return;
	}
	wifi_started = true;
	wifi_stopping = false;

	if (wifi_level >= 1) {
		pdm_port = vayu_qrtr_ops.open(pdm_receive, NULL, NULL);
		if (pdm_port == NULL) {
			say("vayu-wifi: pd-mapper: no port\n");
		} else {
			ret = vayu_qrtr_publish(pdm_port, VAYU_PDM_SERVICE, 1, 1);
			if (ret != 0) {
				say("vayu-wifi: pd-mapper: cannot publish service 0x40: %d\n", ret);
				vayu_qrtr_ops.close(pdm_port);
				pdm_port = NULL;
			} else {
				say("vayu-wifi: pd-mapper: service 0x40 version 1 instance 1 at port %u\n",
				    vayu_qrtr_port_number(pdm_port));
			}
		}

		tftp_port = vayu_qrtr_ops.open(tftp_receive, NULL, NULL);
		if (tftp_port == NULL) {
			say("vayu-wifi: tftp: no port\n");
		} else {
			ret = vayu_qrtr_publish(tftp_port, VAYU_TFTP_SERVICE, 1, 0);
			if (ret != 0) {
				say("vayu-wifi: tftp: cannot publish service 4096: %d\n", ret);
				vayu_qrtr_ops.close(tftp_port);
				tftp_port = NULL;
			} else {
				say("vayu-wifi: tftp: service 4096 version 1 instance 0 at port %u\n",
				    vayu_qrtr_port_number(tftp_port));
			}
		}
	}

	if (wifi_level >= 2) {
		memset(&wlfw, 0, sizeof(wlfw));
		snprintf(wlfw.summary, sizeof(wlfw.summary), "never appeared");
		wlfw.running = true;
		if (kernel_thread_start(wlfw_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			wlfw.running = false;
			wlfw_fail("thread_start");
			say("vayu-wifi: wlfw: no thread\n");
		}
	}
}

void
vayu_wifi_stop(void)
{
	struct hakios_qrtr_port *ports[2 + VAYU_TFTP_XFERS];
	unsigned int count = 0;
	uint32_t pdm, reads, served, writes, refused;
	uint64_t bytes;
	char wlfw_summary[96];

	if (!wifi_started || wifi_level == 0) {
		return;
	}
	wifi_started = false;

	lck_mtx_lock(&vayu_wifi_lock);
	wifi_stopping = true;
	lck_mtx_unlock(&vayu_wifi_lock);

	/* The WLFW thread first: its handle's port belongs to the transport vayu_qrtr_detach() takes away. */
	if (wifi_level >= 2 && wlfw.running) {
		uint32_t waited;

		wlfw.stop = true;
		for (waited = 0; waited < 5000 && wlfw.running; waited += 10) {
			delay_for_interval(10, NSEC_PER_MSEC);
		}
		if (wlfw.running) {
			say("vayu-wifi: wlfw: the thread did not end in 5 s\n");
		}
	}
	/*
	 * W15-A: stop the CE rings before the transport goes away.  The pool is
	 * never freed here: once a CE register was written the firmware may still
	 * hold a ring pointer, so vayu_wifi_ce_teardown() revokes the buffers,
	 * clears the ring bases and keeps the memory mapped.  If the WLFW thread
	 * is still alive it may be polling, so leave everything to it.
	 */
	if (wifi_level >= 2 && wlfw.running) {
		say("vayu-wifi: ce: the wlfw thread is still running; the CE rings are left as they are\n");
	} else {
		/*
		 * A PFR/crash after the CE stage (even after a READY) can collapse the
		 * WLAN block; the CE poll is long gone by then, so read SMP2P once more
		 * and tell the CE teardown not to issue MMIO in that case.
		 */
		if (wifi_ce != 0) {
			bool down = vayu_pdm_pfr_seen;

			if (wifi_smp2p != 0) {
				uint32_t bits = vayu_wifi_smp2p_poll("stop");

				down = down || vayu_wifi_smp2p_stop() ||
				    ((bits & VAYU_WIFI_SMP2P_CRASH) != 0 && wifi_smp2p_abort != 0);
			}
			if (down) {
				vayu_wifi_ce_mark_block_down();
			}
		}
		vayu_wifi_ce_teardown();
	}

	/* No callback should be running now; give a slow one (a send may sleep) a second. */
	for (uint32_t waited = 0; waited < 1000; waited += 10) {
		bool busy;

		lck_mtx_lock(&vayu_wifi_lock);
		busy = pdm_busy != 0 || tftp_busy != 0;
		for (unsigned int i = 0; i < VAYU_TFTP_XFERS && !busy; i++) {
			if (tftp_xfer[i].used && tftp_xfer[i].busy != 0) {
				busy = true;
			}
		}
		lck_mtx_unlock(&vayu_wifi_lock);
		if (!busy) {
			break;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
	}

	/* Every transfer port: xfer_close() logs the end line and adds its bytes. */
	for (unsigned int i = 0; i < VAYU_TFTP_XFERS; i++) {
		tftp_xfer_close(&tftp_xfer[i], false, 0, NULL);
	}

	lck_mtx_lock(&vayu_wifi_lock);
	if (tftp_port != NULL && tftp_busy == 0) {
		ports[count++] = tftp_port;
		tftp_port = NULL;
	}
	if (pdm_port != NULL && pdm_busy == 0) {
		ports[count++] = pdm_port;
		pdm_port = NULL;
	}
	pdm = pdm_requests;
	reads = tftp_reads;
	served = tftp_served;
	bytes = tftp_bytes;
	writes = tftp_writes;
	refused = tftp_write_refused;
	memset(tftp_ram, 0, sizeof(tftp_ram));          /* the RAM overlay lives as long as the service */
	if (wifi_level >= 2) {
		if (wlfw.running) {
			snprintf(wlfw_summary, sizeof(wlfw_summary), "failed at stop");
		} else {
			snprintf(wlfw_summary, sizeof(wlfw_summary), "%s", wlfw.summary);
		}
	} else {
		snprintf(wlfw_summary, sizeof(wlfw_summary), "not asked");
	}
	lck_mtx_unlock(&vayu_wifi_lock);

	for (unsigned int i = 0; i < count; i++) {
		vayu_qrtr_ops.close(ports[i]);
	}

	/* The list of what the firmware asked for, one line per path: the answer to "what does it want". */
	tftp_path_dump();
	say("vayu-wifi: done: pd-mapper %u request(s); tftp %u read(s) (%u served, %llu bytes), %u write(s) "
	    "(%u refused); wlfw %s\n", pdm, reads, served, (unsigned long long)bytes, writes, refused,
	    wlfw_summary);
}
