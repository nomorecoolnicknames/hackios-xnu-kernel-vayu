/*
 * vayu IPA 4.1 firmware and modem prerequisites, before MPSS starts.
 *
 * vayu_ipa=1 opts in. This stage loads the stock ipa_fws through the same PAS machinery that
 * already loads the modem, enables its RPMh clock, allocates the SMEM filter-table item and
 * publishes the apps SMP2P "ipa" entry. Without that entry the modem's IPA task never answers
 * INIT_MODEM_DRIVER (upstream's decisive finding; vayu DT has the same entry name).
 *
 * k26-ipahw: the clock and the hardware-init order now follow Linux ipa3 (msm-4.14 sm8150):
 *   - IP0 is voted with a state taken from the command database's own level table instead of the
 *     fixed 0x3fff, and every risky group of IPA accesses re-votes it first (ipa_hw_group);
 *   - ipa3_init_hw's order is kept: IPA_VERSION, BCR, CLKON, TX_CFG, QSB, counter, COMP, DCD;
 *   - vayu_ipa_bus defaults to all four interconnect paths, including IPA->EBI (the uC executes
 *     from DDR after AUTH_AND_RESET; an unvoted IPA->DDR path is a bus stall, not a slow packet);
 *   - the uC shared area is read only after init_hw, as stock does, and its read is guarded.
 * Everything is still behind vayu_ipa=1; vayu_ipa_clk=0 restores the pre-k26 clock word.
 *
 * vayu_ipa_summary=SECONDS (5..600, default 0: none) repeats one "vayu-ipa: SUMMARY" line with
 * the last step reached (also sysctl kern.vayu_ipa_stage) for 30 minutes; every step says
 * "vayu-ipa: STAGE ...". The SUMMARY thread is bound to CPU vayu_ipa_summary_cpu (default 0) and,
 * while it runs, the IPA bring-up to CPU vayu_ipa_cpu (default 1), so a CPU stuck in an IPA read is
 * never the watcher's; the line names the last guarded access, its CPU and its age.
 * vayu_ipa_uc_skip=1: the uC window (0x1e47000) is never read, before or after MPSS. SM8150 is an
 * MSM platform: the AP loads no ipa_uc; the *modem* loads the uC (rmnet_ipa.c "Android platform loads
 * uC", mainline ipa_qmi.c "The modem is responsible for loading firmware on the IPA microcontroller")
 * when INIT_MODEM_DRIVER says it is a first boot (TLV 0x18 absent or 0) and reports it with 0x35.
 * This switch changes only what the AP reads; nothing of it reaches the modem.
 *
 * a11-qmi: the SMP2P "ipa" entries, after Q3 (the old "modem's SMP2P ... READY" line read item 428,
 * which is the AP's own outbound item: vayu_modem.c ours 428, theirs 435; dt qcom,smem = <435>, <428>).
 *   428 entry "ipa" (AP -> modem, dt qcom,smp2p-ipa-1-out): bit 0 clock response valid, bit 1 clock on
 *       (msm ipa_i.h IPA_SMP2P_OUT_CLK_RSP_CMPLT_IDX 0 / _VOTE_IDX 1; mainline "ipa-clock-enabled-valid",
 *       "ipa-clock-enabled"). It is an *answer*: vendor and mainline write it only when the modem asks.
 *   435 entry "ipa" (modem -> AP, dt qcom,smp2p-ipa-1-in, interrupts <0 0>, IRQF_TRIGGER_RISING): bit 0 is
 *       the modem's clock query (ipa.c ipa3_smp2p_modem_clk_query_isr, mainline ipa_smp2p_modem_clk_query_isr).
 * What the old check verified is only that our own item reads back 3; the lines now say so, and the
 * modem's 435 entry is reported separately. Gating is unchanged by default. Two switches, both default 0:
 *   vayu_ipa_smp2p_watch=1     read-only: a thread logs the appearance and every change of both "ipa"
 *                              entries (428 ours, 435 the modem's) with ms since MPSS ("ipa-smp2p: ...").
 * a11 n-series: after INIT_MODEM_DRIVER with skip_uc_load=1 the modem says "init pending 1" and never sends
 * 0x35; every WDS Start on subscription 1 then ends in the modem's "ds_rmnet_meta_sm.c:5461: Assertion 0".
 * skip_uc_load=1 tells it the IPA uC is loaded, and nobody loaded it: on an MSM the AP does not
 * (vayu-r-oss ipa3_load_ipa_fw loads "ipa_uc" via PIL only for IPA_PLAT_TYPE_APQ), and with the TLV
 * absent the modem's own load path never answers INIT_MODEM_DRIVER. The same SoC's external-modem
 * configuration (vayu-r-oss sm8150-sdxprairie.dtsi: qcom,ipa_uc, qcom,pil-tz-generic, pas-id 0x1B,
 * firmware "ipa_uc") has the AP load it through TrustZone, after ipa_fws. Its image is on the phone's
 * /vendor/firmware (ipa_uc.mdt/.b00-.b02: one 64 KiB segment for 0x1e60000, the "IPA uC RAM").
 *   vayu_ipa_uc_pas=1  (default 0) after the GSI data path, before MPSS: PAS 0x1B "ipa_uc" into the same
 *                      reserved region as ipa_fws (0x98f00000, 64 KiB: vayu has no second one that holds it;
 *                      sdxprairie gives the two images two regions), only if TrustZone says it supports
 *                      0x1B. No AOP load_state, as for ipa_fws. The uC window is still not read with
 *                      vayu_ipa_uc_skip=1. Meant with vayu_ipa_skip_uc_load=1 (the APQ shape: the AP has
 *                      loaded the uC, the modem is told so).
 * a11 o-series: TrustZone has no PAS 0x1B on vayu (is_supported 0, INIT_IMAGE refused): on this MSM
 * the modem loads the uC itself, on the cold path (TLV 0x18 absent or 0), where INIT_MODEM_DRIVER is
 * never answered. What the uC does then is unseen while vayu_ipa_uc_skip=1 forbids the old uC poll.
 *   vayu_ipa_uc_watch=1  (default 0) read only, from MPSS start for 30 minutes: every 500 ms the uC
 *                        shared area at the start of the IPA SRAM (mainline ipa_uc.c struct
 *                        ipa_uc_mem_area: command +0, response +0xc, response_param +0x10, event +0x14,
 *                        event_param +0x18, first_error_address +0x1c, hw_state +0x20,
 *                        interface_version +0x24), each only after IPA_VERSION answers; one line per
 *                        change ("ipa-uc: ..."; response 1 = INIT_COMPLETED). Every 10 s (30 times) the
 *                        SMMU fault registers of the IPA streams 0x520/0x521/0x522 ("ipa-fault: uc-watch").
 *                        The uC window reads of vayu_ipa_uc_look stay off with vayu_ipa_uc_skip=1; this is
 *                        the same SRAM the GSI tables are read back from. The IP0/bus votes are never given
 *                        back in this file, so mainline's ipa_uc_power() proxy vote is held already.
 *   vayu_ipa_smp2p_mainline=1  our 428 "ipa" entry is made 0 before MPSS (mainline ipa_smp2p_notify_reset
 *                              at QCOM_SSR_BEFORE_POWERUP, vendor res_sent=false), not the pre-set 3; the
 *                              watcher runs and answers a rising bit 0 of the modem's 435 entry the way
 *                              ipa_smp2p_notify() does: "enabled" (bit 1, the IP0 vote is held) then
 *                              "valid" (bit 0), an APCS ring (bit 14) after each; once per boot, as mainline
 *                              "notified" and vendor "res_sent". The readiness gate then wants the entry to
 *                              exist, not to read 3. vayu_ipa_seq bit 1 (the 0 -> 3 dance) is skipped.
 *
 * vayu_ipa_extra_bus=BITS (a9-ipa, default 0: none, the old vote): the interconnect BCMs of the IPA
 * paths that the old rail table never voted (mainline v6.12 drivers/interconnect/qcom/sm8150.c):
 *   1 SN11 (qnm_aggre2_noc: IPA -> A2NOC -> SNOC, both the memory and the imem path), 2 SN1
 *   (qxs_imem, the OCIMEM end), 4 CN0 (qhs_ipa, the config path), 8 SN0 (qns_gemnoc_sf, SNOC -> GEM).
 * Each one is looked up in the command database by name; its bcm_db aux data gives unit and width,
 * and the X/Y words are computed the way bcm-voter.c's bcm_aggregate() does for the IPA's own
 * requests alone (vote_scale 1000). vayu_ipa_xbus_bw=0 (default) takes the requests of mainline
 * ipa_data-v4.2.c (memory 80000/465000, imem 0/68570, config 0/30000 kB/s), =1 the SVS2 vectors of
 * msm-4.14 sm8150.dtsi (80000/600000, 80000/350000, 40000/40000). The votes go before the first IPA
 * access, again before every group, and are never given back. Lines: "ipa-xbus: ...".
 * vayu_ipa_persist=1 (a9-ipa, default 0): a breadcrumb that outlives a watchdog reset, in the last
 * page of the vendor's ramoops region (see ipa_persist_init). xnuloader prints it at its next start
 * ("ipa-persist: last stage ..."), and this file prints the one it finds before writing its own.
 *
 * After the b9 series (a9 r1-r4/c1, 19.11: the SoC stops between "hardware init done" and the SRAM
 * step even with no SRAM store), three more switches, all default 0:
 *   vayu_ipa_pause_trace=1    every 2.5 s "about to" pause is cut into 100 ms pieces, each said
 *                             ("vayu-ipa: pause N of M ms ...") and left as a breadcrumb;
 *   vayu_ipa_post_init_stop=1 after init_hw no IPA/GSI access at all (no vote either): one line a
 *                             second for 120 s, then the bring-up ends there (IPA half-started). A
 *                             SoC that still stops is stopped by the IPA/uC/modem side, not by us;
 *   vayu_ipa_fault_dump=1     the SMMU's recorded faults for the IPA streams ("ipa-fault: ..."),
 *                             read after AUTH, after init_hw, at every pause piece and every second
 *                             of the post-init stop (reads of the SMMU only; vayu_ipa_smmu.c).
 * No NoC error-logger addresses for SM8150 are in this tree, the lab notes or mainline; they are
 * not read.
 *
 * It does not start a WDS data session and it does not claim a GSI packet path yet.
 * vayu_ipa_firmware_ready() is set only after PAS reports the IPA processor ready. The separate
 * vayu_ipa_qmi.c completes INIT_MODEM_DRIVER/INIT_COMPLETE after QRTR is up.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <sys/vayu_ipa.h>
#include <sys/vayu_ipa_data.h>
#include <sys/vayu_pas.h>

#define IPA_SM_HOST_MODEM       1
#define IPA_SMP2P_OURS          428
#define IPA_SMP2P_VALID         (1u << 0)
#define IPA_SMP2P_CLOCK_ON      (1u << 1)
#define IPA_FILTER_TABLE_ITEM   497
#define IPA_FILTER_TABLE_SIZE   0x2000   /* vayu default: no qcom,ipa-q6-smem-size in DT */
#define IPA_SRAM_V41            0x1e47000u      /* 0x1e00000 + 0x40000 + IPA_SW_AREA_RAM_DIRECT_ACCESS_n v3.0..v4.2 */
#define IPA_SRAM_OLD            0x1e50000u      /* + 0x10000: the IPA v4.5 offset */

extern void loader_log_putc(char c);
extern void loader_checkpoint(uint64_t n);      /* osfmk/arm64/loader_return.c: "xl run N" stops there */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t thread_terminate(thread_t thread);

static struct vayu_rproc ipa_fw = {
	.name = "ipa", .firmware = "ipa_fws", .pas_id = 15,
	.memory = 0x98f00000, .memory_size = 0x10000,
	.smem_host = IPA_SM_HOST_MODEM, .smp2p_theirs = 0, .smp2p_ours = 0,
	.crash_item = 0, .apcs_smp2p = 14,
	.rails = { NULL, NULL }, .rail_level = 384, .rails_shared = false,
};

static bool ipa_enabled, ipa_prepared, ipa_fw_ready, ipa_smp2p_ready;
/* vayu_ipa_uc_pas=1 (a11, see the top): the IPA uC image through TrustZone, as sdxprairie's qcom,ipa_uc. */
static uint32_t ipa_uc_pas;
static struct vayu_rproc ipa_uc_fw = {
	.name = "ipa_uc", .firmware = "ipa_uc", .pas_id = 0x1b,
	.memory = 0x98f00000, .memory_size = 0x10000,
	.smem_host = IPA_SM_HOST_MODEM, .smp2p_theirs = 0, .smp2p_ours = 0,
	.crash_item = 0, .apcs_smp2p = 14,
	.rails = { NULL, NULL }, .rail_level = 384, .rails_shared = false,
};
/* a11-qmi (see the top of this file): ipa_smp2p_ready means "our own 428 entry verified", nothing of
 * the modem. vayu_ipa_smp2p_watch / vayu_ipa_smp2p_mainline, both default 0. */
static uint32_t ipa_smp2p_watch, ipa_smp2p_mainline, ipa_uc_watch;
static volatile bool ipa_watch_running, ipa_watch_stop;
#define IPA_SMP2P_THEIRS        435
#define IPA_APCS_MODEM          14      /* dt: the modem's APCS IPC bit (vayu_modem.c .apcs_smp2p) */
static bool ipa_modem_loader;           /* vayu_ipa_loader=1: no AP PAS; watch what MPSS does */
static uint32_t ipa_filter_item, ipa_our_item, ipa_restricted;
static vm_offset_t ipa_regs;
/* vayu_ipa_bus: which bus BCMs get the IP0 word (1 SN8, 2 SH0, 4 MC0, 8 ACV; default 6, SH0+MC0, what
 * every lab IPA run from k26 on passed; 15 was the default before, and SN8 and ACV hung the host
 * during the pause in runs 0941..0943; ACV stays voted through vayu_ipa_bus_core). vayu_ipa_post_auth_ms:
 * after AUTH_AND_RESET(15) returns, wait this long and announce every later IPA access with a 2.5 s
 * pause. vayu_ipa_sram=1 (default; every lab IPA run from k26 on passed it): canaries in the IPA 4.1 SW
 * SRAM window (wrapper + 0x40000 + 0x7000, ipahal_reg.c v3.0..v4.2); vayu_ipa_sram=0 the old 0x1e50000
 * (the v4.5 offset). Ported from 0029 (w23 line) to this tree. */
static uint32_t ipa_bus_mask = 6, ipa_post_auth_ms, ipa_sram_fix = 1;
/* vayu_ipa_step_ms: the pause after each "about to ..." (default 2500). vayu_ipa_hw_step_ms: the
 * same for the hardware-init/SRAM section, default 0 so CLKON -> SRAM stays milliseconds (run
 * 125713 died at the first SRAM write ~15 s after CLKON; 115037 wrote it ~2.5 s after and passed).
 * vayu_ipa_sram_write=0 skips the SRAM canaries entirely. vayu_ipa_bus_core=1 (default) also
 * votes the ACV rail, the IPA core NoC path that stock keeps voted (use_bw_vote, ipa.c:5140). */
static uint32_t ipa_step_ms = 2500, ipa_hw_step_ms, ipa_sram_write = 1, ipa_bus_core = 1;
/* vayu_ipa_hold_trace=1: cut the post-AUTH hold into 1-second pieces and say each one. Run 1067
 * (k26-camuw, 225957-run.log:8833) never came back from that hold: its last "vayu-ipa" line is
 * the hold itself and the kernel then ran for 20 more minutes (:18483) without a single IPA
 * line, so a thread that never wakes from the sleep and a machine that stops look the same from
 * outside. The trace tells them apart one second at a time. Default 0: one delay, no new lines. */
static uint32_t ipa_hold_trace;
/*
 * k26-ipahw: the IPA clock vote. Stock on vayu has qcom,bandwidth-vote-for-ipa: the clock is the
 * IPA_CORE->IPA_CORE path of the interconnect (sm8150.dtsi:3635-3640, 0/125 at SVS2) and is never
 * a GCC branch (ipa.c:5133-5156 makes ipa3_clk NULL). The RPMh BCM behind it is "IP0"
 * (0x500a4 on this phone).
 *
 * Run 1064 proved the k26 reading of the cmd-db aux words wrong: they are not a level table at
 * all but the 8-byte `struct bcm_db` of Linux's cmd-db (drivers/soc/qcom/cmd-db.h):
 *     __le32 unit; __le16 width; u8 vcd; u8 reserved;
 * The four 16-bit words 40 0 8 7 are unit=40, width=8, vcd=7. Read as a "level table" the first
 * "level" is 40, so the vote became position 0 = a data word of 0 -- no bandwidth at all. With
 * that vote the very next IPA register read (IPA_VERSION at 0x1e40034, ipa_registers_check())
 * wedged the boot core: logs/1064 "THE KERNEL HUNG on core 700" and no line after the filter
 * table. Run 1066, which voted the old word 0x60003fff, read IPA_VERSION 0x8030300 and finished
 * the whole hardware init, so the old word is the field-proven one.
 *
 * Modes, one boot arg:
 *   vayu_ipa_clk=0  (default, restored) the old word 0x60003fff (commit|valid|16383), the value
 *                   1066 ran the complete IPA hardware init with.
 *   vayu_ipa_clk=1  decode the aux words as bcm_db and vote bandwidth/unit, clamped to
 *                   (1<<width)-1; vayu_ipa_clk_kbps (default 125, DT SVS2) is the request.
 *   vayu_ipa_clk_raw=WORD  that exact command word, no questions asked.
 * In every mode the state field is never allowed to be 0: a BCM vote of 0 means no bandwidth and
 * the clock dies under the next MMIO read (the 1064 failure).
 */
static uint32_t ipa_clk_mode = 0, ipa_clk_level = 1, ipa_clk_raw, ipa_clk_raw_set, ipa_cmd_db;
static uint32_t ipa_clk_kbps = 125;
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE_XY(x, y)       (BCM_COMMIT | BCM_VALID | (((uint32_t)(x) & 0x3fff) << 14) | ((uint32_t)(y) & 0x3fff))
/* vayu_ipa_uc_read=1 (default): read the uC shared area (0x1e47000) after a fresh clock vote.
 * Run 123126 hung at such a read when the clock was not held; every read now goes through
 * ipa_hw_group(), which votes the clock again and checks IPA_VERSION first. vayu_ipa_uc_wait_ms
 * is the poll for INIT_COMPLETED (default 2000). */
static uint32_t ipa_uc_reads = 1, ipa_uc_wait_ms = 2000;
/* vayu_ipa_gcc_survey=1 (default): read-only dump of the SM8150 GCC IPA branches, before and
 * after the RPMh vote. No write: the branch write path (vayu_ipa_gcc=1) is unchanged and opt-in. */
static uint32_t ipa_gcc_survey = 1;

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
	for (const char *c = line; *c != 0; c++) loader_log_putc(*c);
	printf("%s", line);
	return length;
}

/*
 * The last step the IPA bring-up reached, as one line: kern.vayu_ipa_stage, and with
 * vayu_ipa_summary=SECONDS a "vayu-ipa: SUMMARY" line every SECONDS for 30 minutes. Run b3-ipa2
 * (18.11) passed the post-AUTH window for the first time and its live capture ended inside the
 * 2 s uC poll; the boot loader's logdump ring had already lost every IPA line. A summary that is
 * said again later cannot be lost that way. Logging only: no register is read for it.
 */
static char ipa_stage_text[160] = "not started";
SYSCTL_STRING(_kern, OID_AUTO, vayu_ipa_stage, CTLFLAG_RD, ipa_stage_text, sizeof(ipa_stage_text),
    "vayu-ipa: the last bring-up step reached");
static uint32_t ipa_summary_s;
static bool ipa_uc_seen_ready;

static void ipa_stage(const char *format, ...) __printflike(1, 2);
static void
ipa_stage(const char *format, ...)
{
	char text[sizeof(ipa_stage_text)];
	va_list list;

	va_start(list, format);
	vsnprintf(text, sizeof(text), format, list);
	va_end(list);
	strlcpy(ipa_stage_text, text, sizeof(ipa_stage_text));
	say("vayu-ipa: STAGE %s\n", text);
}

/*
 * Breadcrumbs: what the IPA thread is about to touch, written before each guarded access and
 * read only by the SUMMARY thread. b3-ipa3 (18.11) stopped inside the uC poll after its fourth
 * re-vote while the rest of the system lived six more minutes; these name the access.
 */
static const char *volatile ipa_bc_what = "none";
static volatile uint32_t ipa_bc_addr, ipa_bc_count, ipa_bc_cpu;
static volatile uint64_t ipa_bc_ms;
static uint32_t ipa_uc_skip;            /* vayu_ipa_uc_skip=1: never read the uC window */

static uint64_t
ipa_now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/*
 * vayu_ipa_persist=1: the same breadcrumbs, in DDR that a watchdog reset does not clear. b3-ipa4
 * (18.11) stopped after init_hw and the 50 s watchdog reset the phone; the SUMMARY variables died
 * with the kernel, and the boot loader's logdump was overwritten by the next start.
 *
 * Where: the last page of the vendor's ramoops region, 0xb0000000 + 0x800000 (vayu.dtb
 * /reserved-memory/ramoops@b0000000, not no-map; xnuloader loader.c vendor_reserved[]). Its zones are
 * record, console, ftrace and pmsg, 0x200000 each, so this page is the tail of the pmsg zone, which
 * only Android's pstore uses -- Linux's pstore kept console-ramoops across a panic reset on this
 * phone (lab receipts/2026-10-24-vayu-wifi.md:484). XNU's window starts at 0xf4000000, so the page is
 * not kernel RAM: VM_WIMG_IO maps it Device-nGnRnE (pmap.c wimg_to_pte: not is_dram_addr), and a
 * store is complete at the DDR controller once the dsb sy after it has finished.
 *
 * Two slots of 128 bytes; the writes alternate between them, so a record cut by the reset leaves the
 * other one valid. Words: 0 magic "IPAP", 1 version, 2 seq, 3 stage, 4 pa, 5 op, 6 cpu, 7 done (0
 * BEFORE the access, 1 AFTER it), 8 boot id, 9 ms since the bring-up started, 10..17 a 32-byte tag,
 * 18 checksum of words 0..17 (ipa_persist_sum), 19 "SEEN" once xnuloader has printed the slot. The
 * same layout is in xnuloader/loader.c (ipa_persist_report).
 */
#define IPA_PERSIST_PA          0xb07ff000u
#define IPA_PERSIST_MAGIC       0x50415049u     /* "IPAP" */
#define IPA_PERSIST_SEEN        0x4e454553u     /* "SEEN" */
#define IPA_PERSIST_VERSION     1u
#define IPA_PERSIST_SLOT        0x80u
#define IPA_PERSIST_WORDS       20u
#define IPA_PERSIST_SUMMED      18u

enum { IPA_PS_VOTE = 1, IPA_PS_CHECK, IPA_PS_PAS, IPA_PS_POSTAUTH, IPA_PS_INIT_HW, IPA_PS_SRAM, IPA_PS_UC,
       IPA_PS_DATA, IPA_PS_READY, IPA_PS_STOP };
enum { IPA_PO_TCS = 1, IPA_PO_READ, IPA_PO_WRITE, IPA_PO_SRAM_ZERO, IPA_PO_SRAM_CANARY, IPA_PO_UC_READ,
       IPA_PO_STEP, IPA_PO_PAUSE };

static uint32_t ipa_persist;
static uint32_t ipa_pause_trace, ipa_post_init_stop, ipa_fault_dump;
/* vayu_ipa_clkon_fix (a9-ipa round 3, default 0: the old word 3). IPA_CLKON_CFG 0x1e40044 bit 0 is
 * CLKON_RX and bit 1 CLKON_PROC; "open global" is bit 28 and "open global 2x" bit 29 (mainline v6.12
 * reg/ipa_reg-v4.2.c:36-70, msm-4.14 ipahal_reg_i.h:328-331 with the v4.0 table at 0x44 that v4.1
 * inherits, ipahal_reg.c:3036). Stock and mainline write only those two (ipa_utils.c:4198-4208,
 * ipa_main.c:231-255 ipa_hardware_config_clkon): 0x30000000. The old 3 opened RX/PROC and left the
 * global clocks gateable. 1: 0x30000000; 2: 0x30000003 (both). */
static uint32_t ipa_clkon_fix;
static volatile uint32_t *ipa_persist_page;
static uint32_t ipa_persist_seq, ipa_persist_boot;
static uint64_t ipa_persist_t0;
static uint32_t ipa_pc_stage;                   /* the stage the IPA_RD/IPA_WR breadcrumbs carry */
static const char *ipa_pc_tag = "";             /* ... and their tag: the group being done */
static struct { uint32_t stage, pa, op; const char *tag; } ipa_pc_last;

static uint32_t
ipa_persist_sum(const uint32_t *w)
{
	uint32_t sum = 0x12345678u;

	for (unsigned i = 0; i < IPA_PERSIST_SUMMED; i++) {
		sum = (sum << 5 | sum >> 27) ^ w[i];
	}
	return sum;
}

/* One record into the next slot: magic cleared first and set last, each step completed (dsb sy). */
static void
ipa_pc(uint32_t stage, const char *tag, uint32_t pa, uint32_t op, bool done)
{
	uint32_t w[IPA_PERSIST_WORDS] = { 0 };
	volatile uint32_t *slot;
	uint32_t seq;

	if (ipa_persist_page == NULL) return;
	seq = __atomic_add_fetch(&ipa_persist_seq, 1, __ATOMIC_RELAXED);
	slot = ipa_persist_page + (seq & 1) * (IPA_PERSIST_SLOT / 4);
	w[0] = IPA_PERSIST_MAGIC;
	w[1] = IPA_PERSIST_VERSION;
	w[2] = seq;
	w[3] = stage;
	w[4] = pa;
	w[5] = op;
	w[6] = (uint32_t)cpu_number();
	w[7] = done ? 1 : 0;
	w[8] = ipa_persist_boot;
	w[9] = (uint32_t)(ipa_now_ms() - ipa_persist_t0);
	strlcpy((char *)&w[10], tag != NULL ? tag : "", 32);
	w[18] = ipa_persist_sum(w);
	slot[0] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	for (unsigned i = 1; i < IPA_PERSIST_WORDS; i++) {
		slot[i] = w[i];
	}
	__asm__ volatile("dsb sy" ::: "memory");
	slot[0] = w[0];
	__asm__ volatile("dsb sy" ::: "memory");
	ipa_pc_last.stage = stage;
	ipa_pc_last.pa = pa;
	ipa_pc_last.op = op;
	ipa_pc_last.tag = tag;
}

/* The access of the last BEFORE record has completed. */
static void
ipa_pc_after(void)
{
	if (ipa_persist_page == NULL) return;
	ipa_pc(ipa_pc_last.stage, ipa_pc_last.tag, ipa_pc_last.pa, ipa_pc_last.op, true);
}

/* vayu_ipa_fault_dump=1: the SMMU's IPA faults now; the status word also goes into the breadcrumb. */
static void
ipa_faults(uint32_t stage, const char *when)
{
	uint32_t fsr;

	if (ipa_fault_dump != 1) return;
	ipa_pc(stage, "SMMU fault read", 0x15000048u, IPA_PO_READ, false);
	fsr = vayu_ipa_smmu_faults(when);
	ipa_pc(stage, fsr != 0 ? "SMMU FAULT (pa = status)" : "SMMU no fault", fsr, IPA_PO_READ, true);
}

static const char *
ipa_persist_stage_name(uint32_t stage)
{
	static const char *const names[] = { "?", "TCS vote", "IPA CFG check", "PAS/AUTH", "post-AUTH look",
		"init_hw", "SRAM", "uC read", "GSI data path", "firmware ready", "stop" };

	return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "?";
}

static const char *
ipa_persist_op_name(uint32_t op)
{
	static const char *const names[] = { "?", "TCS", "read", "write", "SRAM zero", "SRAM canary", "uC read",
		"step", "pause" };

	return op < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
}

/* Map the page, say what the previous run left there (xnuloader may have printed it already), and
 * continue its sequence so that the newest record is always the one with the highest seq. */
static void
ipa_persist_init(void)
{
	uint32_t best = 0, best_seq = 0, max_seq = 0;

	if (ipa_persist != 1 || ipa_persist_page != NULL) return;
	ipa_persist_page = (volatile uint32_t *)ml_io_map_unmappable(IPA_PERSIST_PA, 0x1000, VM_WIMG_IO);
	if (ipa_persist_page == NULL) {
		say("ipa-persist: %#x did not map; no breadcrumbs this run\n", IPA_PERSIST_PA);
		return;
	}
	for (uint32_t s = 0; s < 2; s++) {
		uint32_t w[IPA_PERSIST_WORDS];

		for (unsigned i = 0; i < IPA_PERSIST_WORDS; i++) {
			w[i] = ipa_persist_page[s * (IPA_PERSIST_SLOT / 4) + i];
		}
		if (w[0] != IPA_PERSIST_MAGIC || w[1] != IPA_PERSIST_VERSION || w[18] != ipa_persist_sum(w)) {
			continue;
		}
		if (w[2] > max_seq) max_seq = w[2];
		if (best == 0 || w[2] > best_seq) {
			best = s + 1;
			best_seq = w[2];
		}
	}
	if (best == 0) {
		say("ipa-persist: no valid record of an earlier run at %#x\n", IPA_PERSIST_PA);
	} else {
		volatile uint32_t *p = ipa_persist_page + (best - 1) * (IPA_PERSIST_SLOT / 4);
		char tag[33] = { 0 };

		for (unsigned i = 0; i < 8; i++) {
			uint32_t v = p[10 + i];

			memcpy(tag + 4 * i, &v, 4);
		}
		for (unsigned i = 0; i < 32; i++) {
			if (tag[i] != 0 && (tag[i] < 0x20 || tag[i] > 0x7e)) tag[i] = '?';
		}
		say("ipa-persist: previous run: last stage %u (%s) \"%s\" op %s pa %#x cpu %u %s, seq %u, %u ms, "
		    "boot %#x, slot %u%s\n", p[3], ipa_persist_stage_name(p[3]), tag, ipa_persist_op_name(p[5]), p[4],
		    p[6], p[7] ? "AFTER (done)" : "BEFORE (not done)", p[2], p[9], p[8], best - 1,
		    p[19] == IPA_PERSIST_SEEN ? ", printed by xnuloader" : ", not seen by xnuloader");
	}
	ipa_persist_seq = max_seq + 1;          /* (odd/even keeps alternating from there) */
	ipa_persist_boot = (uint32_t)mach_absolute_time();
	ipa_persist_t0 = ipa_now_ms();
	say("ipa-persist: on: %#x, two slots of %#x bytes, sequence from %u, boot %#x\n", IPA_PERSIST_PA,
	    IPA_PERSIST_SLOT, ipa_persist_seq + 1, ipa_persist_boot);
}

static inline void
ipa_bc(const char *what, uint32_t addr)
{
	ipa_pc(IPA_PS_UC, what, addr, addr != 0 ? IPA_PO_UC_READ : IPA_PO_STEP, false);
	ipa_bc_what = what;
	ipa_bc_addr = addr;
	ipa_bc_count++;
	ipa_bc_cpu = (uint32_t)cpu_number();
	ipa_bc_ms = ipa_now_ms();
	__asm__ volatile("dmb ish" ::: "memory");
}

static uint32_t
ipa_smem_word(uint32_t offset)
{
	uint32_t value = 0;
	vayu_smem_peek(offset, &value, sizeof(value));
	return value;
}

static bool
ipa_gcc_enable(void)
{
	/* The IPA clock branches sit at GCC +0x89004..+0x89020: map the whole SM8150 GCC (0x1f0000), not 0x80000. */
	vm_offset_t gcc = ml_io_map_unmappable(0x100000, 0x1f0000, VM_WIMG_IO);

	if (gcc == 0) {
		say("vayu-ipa: GCC did not map\n");
		return false;
	}
	/* vendor gcc_ipa_clk: core/AHB/APB enable. Keep reset state untouched. */
	*(volatile uint32_t *)(gcc + 0x89014) |= 1;
	*(volatile uint32_t *)(gcc + 0x89018) |= 1;
	*(volatile uint32_t *)(gcc + 0x89020) |= 1;
	__asm__ volatile("dsb sy" ::: "memory");
	if ((*(volatile uint32_t *)(gcc + 0x89014) & 0x80000001) != 1 ||
	    (*(volatile uint32_t *)(gcc + 0x89018) & 1) != 1 ||
	    (*(volatile uint32_t *)(gcc + 0x89020) & 1) != 1) {
		say("vayu-ipa: GCC IPA clock did not turn on (read-back failed)\n");
		return false;
	}
	say("vayu-ipa: GCC IPA clocks: CBCR %#x AHB %#x APB %#x, GDSCR %#x\n",
	    *(volatile uint32_t *)(gcc + 0x89014), *(volatile uint32_t *)(gcc + 0x89018),
	    *(volatile uint32_t *)(gcc + 0x89020), *(volatile uint32_t *)(gcc + 0x89004));
	return true;
}

static void
ipa_smmu_survey(void)
{
	vm_offset_t smmu = ml_io_map_unmappable(0x15000000, 0x20000, VM_WIMG_IO);

	if (smmu == 0) {
		say("vayu-ipa: SMMU did not map; treating as unverified\n");
		return;
	}
	say("vayu-ipa: SMMU IDR0 %#x IDR1 %#x SCR0 %#x\n",
	    *(volatile uint32_t *)(smmu + 0x20), *(volatile uint32_t *)(smmu + 0x24),
	    *(volatile uint32_t *)(smmu + 0x00));
	for (unsigned n = 0; n < 16; n++) {
		uint32_t smr = *(volatile uint32_t *)(smmu + 0x800 + 4 * n);
		uint32_t s2cr = *(volatile uint32_t *)(smmu + 0xc00 + 4 * n);
		uint32_t stream = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

		if ((stream & ~mask) == 0x520 || (stream & ~mask) == 0x522) {
			say("vayu-ipa: SMMU match %u: smr %#x s2cr %#x (stream %#x mask %#x)\n",
			    n, smr, s2cr, stream, mask);
		}
	}
}

uint32_t
vayu_ipa_restricted_bytes(void)
{
	return ipa_restricted;
}

/* The IPA local (SW) SRAM window this boot uses: 0x1e47000 for IPA 4.1 (vayu_ipa_sram=1, the default),
 * 0x1e50000 for the v4.5 offset the older code used (vayu_ipa_sram=0). Table offsets are relative to this window
 * and include the restricted byte count. */
uint32_t
vayu_ipa_sram_base(void)
{
	return ipa_sram_fix == 1 ? IPA_SRAM_V41 : IPA_SRAM_OLD;
}

static bool
ipa_registers_check(void)
{
	uint32_t version, shared;

	/* qcom,ipa@1e00000 is the wrapper; on IPA v4.x the register block starts at wrapper +0x40000
	 * (downstream IPA_REG_BASE_OFST_v4_0), so IPA_VERSION is 0x1e40034. Reading 0x1e00034 gave
	 * VERSION 0 / SHARED_MEM_SIZE 0 on hardware. SHARED_MEM_SIZE +0x54, 8-byte units. */
	ipa_regs = ml_io_map_unmappable(0x1e40000, 0x1000, VM_WIMG_IO);
	if (ipa_regs == 0) {
		say("vayu-ipa: IPA CFG window did not map\n");
		return false;
	}
	version = *(volatile uint32_t *)(ipa_regs + 0x34);
	shared = *(volatile uint32_t *)(ipa_regs + 0x54);
	ipa_restricted = ((shared >> 16) & 0xffff) * 8;
	say("vayu-ipa: IPA_VERSION %#x, SHARED_MEM_SIZE %#x, restricted bytes %#x, SRAM %#x\n",
	    version, shared, ipa_restricted, (shared & 0xffff) * 8);
	if (version == 0 || version == 0xffffffffU || (shared & 0xffff) == 0) {
		say("vayu-ipa: IPA CFG is not alive; no INIT_MODEM_DRIVER\n");
		return false;
	}
	return true;
}

/*
 * The IPA clock: one RPMh BCM, "IP0", voted through TCS 0 like every other rail in this kernel.
 * Linux reaches it only through msm_bus (use_bw_vote): ipa3_enable_clks() -> msm_bus_scale_
 * client_update_request(ipa_bus_hdl, SVS2) (ipa.c:5206, ipa.c:5170-5193). There is no GCC branch
 * in that path, and the old raw word 0x60003fff may be a state the IP0 table does not contain;
 * see the block above for the three modes. Returns false only when RPMh did not answer.
 */
static uint32_t ipa_ip0_address;
static bool ipa_ip0_said;

static bool
ipa_clock_vote(const char *why, bool verbose)
{
	uint16_t levels[64];
	uint32_t address = ipa_ip0_address, value;
	const char *how;
	unsigned state = 0;
	int count = 0;

	if (address == 0) {
		address = vayu_rpmh_address("IP0");
		ipa_ip0_address = address;
	}
	if (address == 0) {
		say("vayu-ipa: %s: the command database has no IP0: no IPA clock vote\n", why);
		return false;
	}
	if (ipa_clk_raw_set) {
		value = ipa_clk_raw;
		state = value & 0x3fff;
		how = "vayu_ipa_clk_raw";
	} else if (ipa_clk_mode == 0) {
		value = BCM_VOTE_XY(0, 0x3fff);
		state = 0x3fff;
		how = "old raw 0x3fff (the word run 1066 used)";
	} else {
		count = vayu_rpmh_levels("IP0", &address, levels, sizeof(levels) / sizeof(levels[0]));
		if (count == 4) {
			/*
			 * The 8-byte bcm_db, not a level table: unit (bytes/s per vote unit), width in
			 * bits, and the VCD index. 1064 voted the "first level" position 0 here, which
			 * is a data word of 0, and the IPA clock stopped.
			 */
			uint32_t unit = (uint32_t)levels[0] | (uint32_t)levels[1] << 16;
			unsigned width = levels[2] & 0xffff;
			unsigned vcd = levels[3] & 0xff;            /* the low byte; the high one is reserved */
			uint32_t want, votes, most;

			if (unit == 0 || unit > 1000000) unit = 1;
			if (width == 0 || width > 16) width = 8;
			most = (1u << width) - 1;
			want = ipa_clk_kbps * 1024u;            /* DT vectors-KBps are KB/s */
			votes = (want + unit - 1) / unit;
			if (votes == 0) votes = 1;              /* never a zero-bandwidth vote */
			state = votes > most ? most : votes;
			value = BCM_VOTE_XY(0, state);
			how = "bcm_db: bandwidth/unit, clamped";
			if (!ipa_ip0_said) {
				say("vayu-ipa: IP0 aux words %u %u %u %u = bcm_db unit %u B/s, width %u bits, vcd %u; "
				    "%u KB/s -> %u vote(s), clamped to %u\n", levels[0], levels[1], levels[2],
				    levels[3], unit, width, vcd, ipa_clk_kbps, votes, state);
			}
		} else if (count > 32) {
			/* A long value table: the DT gives the SVS2 value of the path as 125. */
			state = 125;
			value = BCM_VOTE_XY(0, state);
			how = "long table: DT 125";
		} else {
			/*
			 * Neither a bcm_db nor a value table we can read. Do not guess: the field-proven
			 * raw word keeps the clock on, and the line says so.
			 */
			value = BCM_VOTE_XY(0, 0x3fff);
			state = 0x3fff;
			how = "unreadable aux data: old raw 0x3fff";
		}
		if (!ipa_ip0_said) {
			char line[240];
			int at = 0;

			ipa_ip0_said = true;
			if (count > 0) {
				at = snprintf(line, sizeof(line), "vayu-ipa: IP0 aux (%d words):", count);
				for (unsigned i = 0; i < (unsigned)count && i < 16; i++) {
					at += snprintf(line + at, sizeof(line) - (size_t)at, " %u", levels[i]);
				}
				say("%s%s\n", line, count > 16 ? " ..." : "");
			} else {
				say("vayu-ipa: IP0 has no aux data in the command database (%d)\n", count);
			}
			say("vayu-ipa: IP0 vote: mode %u, level %u -> state %u (%s)\n", ipa_clk_mode,
			    ipa_clk_level, state, how);
		}
	}
	if (state == 0) {
		/* A data word of 0 is "no bandwidth": the clock gates and the next read hangs the SoC. */
		say("vayu-ipa: %s: refusing a zero IP0 vote; using the raw word 0x60003fff\n", why);
		value = BCM_VOTE_XY(0, 0x3fff);
		state = 0x3fff;
		how = "zero vote refused: old raw 0x3fff";
	}
	if (!vayu_rpmh_send_now(&address, &value, 1, "ipa clock")) {
		say("vayu-ipa: %s: IP0 clock vote failed\n", why);
		return false;
	}
	if (verbose) {
		say("vayu-ipa: %s: IP0 %#x = %#x (%s)\n", why, address, value, how);
	}
	return true;
}

/*
 * vayu_ipa_extra_bus (see the top of the file): the BCMs on the IPA's three interconnect paths that
 * the rail table below does not have. Paths, mainline v6.12 drivers/interconnect/qcom/sm8150.c:
 *   memory: qxm_ipa (:154) -> qns_a2noc_snoc (:703) -> qnm_aggre2_noc [SN11] (:632) -> qns_gemnoc_sf
 *           [SN0] (:1225) -> qnm_snoc_sf (:491) -> qns_llcc [SH0] -> llcc_mc -> ebi [MC0, ACV]
 *   imem:   qxm_ipa -> qns_a2noc_snoc -> qnm_aggre2_noc [SN11] -> qxs_imem [SN1] (:1234)
 *   config: apps -> ... -> qhs_ipa [CN0] (:876; CN0 is keepalive, :1403)
 * A node on two paths carries the sum of their averages and the larger peak (icc-rpmh.c
 * qcom_icc_aggregate). The word is bcm-voter.c's bcm_aggregate() for those requests alone:
 *   x = div(div(avg * width, buswidth * channels) * 1000, unit),
 *   y = div(div(peak * width, buswidth) * 1000, unit), div() rounding a nonzero value below 1 up to 1;
 * a keepalive BCM never goes below 1/1; valid only when x or y is nonzero (tcs_cmd_gen).
 * There is no shared aggregation of apps votes in this kernel: TCS 0 writes replace the apps
 * subsystem's earlier vote of the same BCM (whatever UEFI left, or vayu_qup_spi.c's
 * nvt_qup_bcm_sn11=1 for SN11). That is why each bit is its own experiment.
 */
static uint32_t ipa_extra_bus, ipa_xbus_bw;
static unsigned ipa_xbus_failures, ipa_xbus_last_failed;
static struct ipa_xbus {
	const char *name;
	uint32_t bit;
	uint16_t buswidth, channels;    /* the IPA's node in that BCM */
	bool keepalive;
	uint8_t paths;                  /* 1 memory, 2 imem, 4 config */
	uint32_t address, word;         /* 0: not looked up yet */
	bool refused;
} ipa_xbus[] = {
	{ "SN11", 1, 16, 1, false, 1 | 2 },     /* qnm_aggre2_noc, sm8150.c:632-640, bcm_sn11 :1512-1516 */
	{ "SN1", 2, 8, 1, false, 2 },           /* qxs_imem, :1234-1238, bcm_sn1 :1389-1393 */
	{ "CN0", 4, 4, 1, true, 4 },            /* qhs_ipa, :876-881, bcm_cn0 :1403-1427 */
	{ "SN0", 8, 16, 1, true, 1 },           /* qns_gemnoc_sf, :1225-1231, bcm_sn0 :1368-1372 */
};
/* avg/peak in kB/s (icc units) for memory, imem, config. */
static const uint32_t ipa_xbus_kbps[2][3][2] = {
	{ { 80000, 465000 }, { 0, 68570 }, { 0, 30000 } },             /* mainline data/ipa_data-v4.2.c:348-365 */
	{ { 80000, 600000 }, { 80000, 350000 }, { 40000, 40000 } },    /* msm-4.14 sm8150.dtsi:3661-3714, SVS2 */
};

static uint64_t
ipa_bcm_div(uint64_t num, uint64_t base)
{
	if (num != 0 && num < base) return 1;   /* bcm-voter.c bcm_div(): small votes are not lost */
	return num / base;
}

/* Look the BCM up and compute its word once; false (said once): no vote can be made for it. */
static bool
ipa_xbus_prepare(struct ipa_xbus *b)
{
	uint16_t aux[8];
	uint32_t address = 0, unit, width, vcd, avg = 0, peak = 0;
	uint64_t x, y;
	int count;

	if (b->word != 0) return true;
	if (b->refused) return false;
	b->refused = true;
	count = vayu_rpmh_levels(b->name, &address, aux, sizeof(aux) / sizeof(aux[0]));
	if (count < 0 || address == 0) {
		say("ipa-xbus: %s: not in the command database: not voted\n", b->name);
		return false;
	}
	if (((address >> 16) & 0xf) != 5) {    /* cmd-db.h: slave 5, the bus clock managers */
		say("ipa-xbus: %s: %#x is not a BCM address: not voted\n", b->name, address);
		return false;
	}
	if (count < 3) {
		say("ipa-xbus: %s at %#x: %d aux word(s), no bcm_db: not voted\n", b->name, address, count);
		return false;
	}
	unit = (uint32_t)aux[0] | (uint32_t)aux[1] << 16;
	width = aux[2];
	vcd = count > 3 ? aux[3] & 0xff : 0;
	if (unit == 0 || width == 0) {
		say("ipa-xbus: %s at %#x: bcm_db unit %u width %u: not voted\n", b->name, address, unit, width);
		return false;
	}
	for (unsigned p = 0; p < 3; p++) {
		if (!(b->paths & (1u << p))) continue;
		avg += ipa_xbus_kbps[ipa_xbus_bw][p][0];
		if (ipa_xbus_kbps[ipa_xbus_bw][p][1] > peak) peak = ipa_xbus_kbps[ipa_xbus_bw][p][1];
	}
	x = ipa_bcm_div(ipa_bcm_div((uint64_t)avg * width, (uint64_t)b->buswidth * b->channels) * 1000, unit);
	y = ipa_bcm_div(ipa_bcm_div((uint64_t)peak * width, b->buswidth) * 1000, unit);
	if (b->keepalive && x == 0 && y == 0) {
		x = y = 1;
	}
	if (x > 0x3fff) x = 0x3fff;
	if (y > 0x3fff) y = 0x3fff;
	if (x == 0 && y == 0) {
		say("ipa-xbus: %s: the requests make a zero vote: not voted\n", b->name);
		return false;
	}
	b->address = address;
	b->word = BCM_VOTE_XY(x, y);
	b->refused = false;
	say("ipa-xbus: %s at %#x: bcm_db unit %u width %u vcd %u; node buswidth %u x%u%s; avg %u peak %u kB/s "
	    "(vayu_ipa_xbus_bw=%u) -> x %llu y %llu, raw %#x\n", b->name, address, unit, width, vcd, b->buswidth,
	    b->channels, b->keepalive ? " keepalive" : "", avg, peak, ipa_xbus_bw, x, y, b->word);
	return true;
}

/* The extra BCMs, before the IP0 clock (Linux enables the interconnects first, ipa_power.c:94-121).
 * Returns the number of votes RPMh said done; *failed counts the ones asked for and not made. */
static unsigned
ipa_xbus_vote(const char *why, bool verbose, unsigned *failed)
{
	unsigned sent = 0;

	*failed = 0;
	for (unsigned i = 0; i < sizeof(ipa_xbus) / sizeof(ipa_xbus[0]); i++) {
		struct ipa_xbus *b = &ipa_xbus[i];
		bool done;

		if (!(ipa_extra_bus & b->bit)) continue;
		if (!ipa_xbus_prepare(b)) {
			(*failed)++;
			continue;
		}
		ipa_pc(IPA_PS_VOTE, b->name, b->address, IPA_PO_TCS, false);
		done = vayu_rpmh_send_now(&b->address, &b->word, 1, "ipa xbus");
		ipa_pc_after();
		if (!done) {
			(*failed)++;
			ipa_xbus_failures++;
			say("ipa-xbus: %s: %s %#x = %#x: TCS FAILED (%u so far)\n", why, b->name, b->address, b->word,
			    ipa_xbus_failures);
			continue;
		}
		sent++;
		if (verbose) {
			say("ipa-xbus: %s: %s %#x = %#x: TCS done\n", why, b->name, b->address, b->word);
		}
	}
	return sent;
}

/* IP0 (the IPA clock) plus the enabled rail BCMs. Stock on vayu has use_bw_vote: the IPA clock
 * lives in the bus/interconnect vote, not a GCC branch (ipa.c:5140-5152), and the IPA core path
 * (ACV here) is part of that vote at every level. Returns the number of successful sends. */
static unsigned
ipa_vote_rails(const char *why, bool verbose)
{
	static const char *const rails[] = { "SN8", "SH0", "MC0", "ACV" };
	static uint32_t rail_address[4];
	uint32_t address, value;
	unsigned sent = 0, xbus_failed = 0;
	bool done;

	if (ipa_extra_bus != 0) {
		sent += ipa_xbus_vote(why, verbose, &xbus_failed);
		ipa_xbus_last_failed = xbus_failed;
	}
	ipa_pc(IPA_PS_VOTE, "IP0", ipa_ip0_address, IPA_PO_TCS, false);
	done = ipa_clock_vote(why, verbose);
	ipa_pc_after();
	if (!done) {
		return 0;
	}
	if (xbus_failed != 0 && verbose) {
		say("ipa-xbus: %s: %u of the asked BCMs (vayu_ipa_extra_bus=%#x) were not voted\n", why, xbus_failed,
		    ipa_extra_bus);
	}
	sent++;
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		bool core = i == 3;             /* ACV: the IPA core NoC path */

		if (!(ipa_bus_mask & (1u << i)) && !(core && ipa_bus_core == 1)) {
			if (verbose) say("vayu-ipa: bus %s not voted (vayu_ipa_bus=%u, core %u)\n", rails[i],
			    ipa_bus_mask, ipa_bus_core);
			continue;
		}
		if (rail_address[i] == 0) {
			rail_address[i] = vayu_rpmh_address(rails[i]);   /* (the lookup prints once) */
		}
		address = rail_address[i];
		if (address == 0) {
			if (verbose) say("vayu-ipa: bus %s has no BCM address\n", rails[i]);
			continue;
		}
		/* ACV carries the "apps" bit mask, 8 (msm_bus_arb_rpmh.c:31 BCM_TCS_CMD_ACV_APPS,
		 * the same word vayu_cpufreq.c:552 votes); the data paths take the largest bandwidth. */
		value = core ? BCM_VOTE_XY(0, 8) : BCM_VOTE_XY(0, 0x3fff);
		ipa_pc(IPA_PS_VOTE, rails[i], address, IPA_PO_TCS, false);
		done = vayu_rpmh_send_now(&address, &value, 1, "ipa bus");
		ipa_pc_after();
		if (!done) {
			say("vayu-ipa: %s: bus %s vote failed\n", why, rails[i]);
			continue;
		}
		sent++;
		if (core && !(ipa_bus_mask & (1u << i))) {
			say("vayu-ipa: ACV (IPA core) voted by vayu_ipa_bus_core=1, outside vayu_ipa_bus=%u\n",
			    ipa_bus_mask);
		}
	}
	return sent;
}

static bool
ipa_clock_on(void)
{
	unsigned sent = ipa_vote_rails("ipa clock", true);

	if (sent == 0) {
		say("vayu-ipa: IP0 clock vote failed; not starting the IPA firmware\n");
		return false;
	}
	if (ipa_extra_bus != 0 && ipa_xbus_last_failed != 0) {
		/* The run asked for these paths: without them it would only repeat the old experiment. */
		say("ipa-xbus: FAILED: %u asked BCM(s) of vayu_ipa_extra_bus=%#x not voted; not starting the IPA "
		    "firmware\n", ipa_xbus_last_failed, ipa_extra_bus);
		ipa_stage("ipa-xbus: extra bus vote failed; IPA not started");
		return false;
	}
	if (ipa_extra_bus != 0) {
		say("ipa-xbus: OK: vayu_ipa_extra_bus=%#x voted before the first IPA access\n", ipa_extra_bus);
	}
	say("vayu-ipa: %u vote(s) sent (IP0 and the rails)\n", sent);
	return true;
}

/* The IPA register accesses; with vayu_ipa_persist=1 each one is a BEFORE/AFTER pair of records
 * (stage and tag: the group being done, ipa_hw_group). Without it they are the plain accesses. */
static uint32_t
ipa_rd(uint32_t off)
{
	uint32_t value;

	ipa_pc(ipa_pc_stage, ipa_pc_tag, 0x1e40000u + off, IPA_PO_READ, false);
	value = *(volatile uint32_t *)(ipa_regs + off);
	ipa_pc_after();
	return value;
}

static void
ipa_wr(uint32_t off, uint32_t value)
{
	ipa_pc(ipa_pc_stage, ipa_pc_tag, 0x1e40000u + off, IPA_PO_WRITE, false);
	*(volatile uint32_t *)(ipa_regs + off) = value;
	__asm__ volatile("dsb sy" ::: "memory");
	ipa_pc_after();
}
#define IPA_RD(off) ipa_rd(off)
#define IPA_WR(off, value) ipa_wr((off), (value))
#define RSRC(xmin, xmax, ymin, ymax) \
	((uint32_t)(xmin) | (uint32_t)(xmax) << 8 | (uint32_t)(ymin) << 16 | (uint32_t)(ymax) << 24)

#define GSI_BASE                0x1e04000u
#define UC_RESPONSE_INIT_DONE   1u              /* IPA_HW_2_CPU_RESPONSE_INIT_COMPLETED, ipa_uc.c */

/*
 * The one access that has never hung on this phone is IPA_VERSION (every run has printed
 * 0x8030300 there). The hangs all sat at the first *other* access after a gap -- CLKON, the SRAM
 * window, a GSI register -- which is what the IPA block does when its clock is not held any more.
 * So every risky group of accesses starts with a fresh vote and this read. A dead version is not
 * fatal by itself: ipa_hw_group() says why and the caller stops before the access that would wedge
 * the SoC.
 */
/* A hardware-init group: hold the clock again, prove the block answers, then write. */
static bool
ipa_hw_group(const char *what)
{
	unsigned sent;
	uint32_t version;

	ipa_pc_tag = what;
	ipa_pc(ipa_pc_stage, what, 0, IPA_PO_STEP, false);
	sent = ipa_vote_rails(what, false);
	if (sent == 0) {
		say("vayu-ipa: %s: the clock vote did not go through; group skipped\n", what);
		return false;
	}
	version = IPA_RD(0x34);
	if (version == 0 || version == 0xffffffffU) {
		say("vayu-ipa: %s: IPA_VERSION %#x after %u vote(s): the IPA block is not answering; "
		    "group skipped\n", what, version, sent);
		return false;
	}
	say("vayu-ipa: hw: %s (IPA_VERSION %#x, %u vote(s))\n", what, version, sent);
	return true;
}

/* Read-only: the SM8150 GCC IPA branches (GDSCR 0x89004, CBCR 0x89014/18/20). A branch that is
 * still halted here with bit 0 written would say its parent is off -- the other half of the
 * "is the clock really on" question, and unlike the write path (vayu_ipa_gcc=1) it cannot hang. */
static void
ipa_gcc_look(const char *when)
{
	static vm_offset_t gcc;

	if (ipa_gcc_survey != 1) return;
	if (gcc == 0) {
		gcc = ml_io_map_unmappable(0x100000, 0x1f0000, VM_WIMG_IO);
	}
	if (gcc == 0) {
		say("vayu-ipa: GCC survey %s: the block did not map\n", when);
		return;
	}
	say("vayu-ipa: GCC survey %s: GDSCR %#x, CBCR %#x %#x %#x (bit 0 enable, bit 31 off)\n", when,
	    *(volatile uint32_t *)(gcc + 0x89004), *(volatile uint32_t *)(gcc + 0x89014),
	    *(volatile uint32_t *)(gcc + 0x89018), *(volatile uint32_t *)(gcc + 0x89020));
}

/* The IPA GDSC status word (GCC 0x89004, bit 31 PWR_ON) for the uC poll's check before each read
 * of the window. GCC is always clocked, so this read is not the kind that hangs. 0: no mapping. */
static uint32_t
ipa_gdscr(void)
{
	static vm_offset_t gcc;

	if (gcc == 0) {
		gcc = ml_io_map_unmappable(0x100000, 0x1f0000, VM_WIMG_IO);
	}
	return gcc == 0 ? 0 : *(volatile uint32_t *)(gcc + 0x89004);
}

/*
 * vayu_ipa_cmd_db=1: every bus-clock-manager resource of the boot loader's command database, name,
 * address and level count. One boot with it answers which of SN8/SH0/MC0/ACV/IP0 exist, in which
 * order their levels are listed, and whether IP0 is an index table or a value table.
 */
static void
ipa_cmd_db_survey(void)
{
	for (uint32_t n = 0; n < 64; n++) {
		char name[9] = { 0 };
		uint32_t address = 0;
		int count = 0;

		if (!vayu_rpmh_bcm_name(n, name, &address, &count)) {
			break;
		}
		say("vayu-ipa: cmd-db BCM %u: \"%s\" at %#x, %d level(s)\n", n, name, address, count);
	}
}

/* Announce the next IPA access; ms is how long the loader (2 s snapshots) gets to carry the line
 * out before the access happens. */
static void
ipa_step_after(const char *what, uint32_t ms)
{
	if (ipa_post_auth_ms == 0) return;
	say("vayu-ipa: post-auth: about to %s\n", what);
	if (ms != 0 && ipa_pause_trace == 1) {
		/* vayu_ipa_pause_trace=1: 100 ms at a time, each piece said and left as a breadcrumb (pa = ms
		 * done), so the line before a stop says how far into the pause the system still ran. */
		uint64_t begin = ipa_now_ms();

		for (uint32_t done = 0; done < ms;) {
			uint32_t piece = ms - done < 100 ? ms - done : 100;

			ipa_pc(ipa_pc_stage, what, done, IPA_PO_PAUSE, false);
			delay_for_interval(piece, NSEC_PER_MSEC);
			done += piece;
			say("vayu-ipa: pause %u of %u ms before \"%s\" (%llu ms real, mach %llu, cpu %u)\n", done, ms, what,
			    ipa_now_ms() - begin, mach_absolute_time(), (unsigned)cpu_number());
			__asm__ volatile("dsb sy" ::: "memory");
			ipa_faults(ipa_pc_stage, "pause");
		}
		ipa_pc(ipa_pc_stage, what, ms, IPA_PO_PAUSE, true);
	} else if (ms != 0) {
		ipa_pc(ipa_pc_stage, what, 0, IPA_PO_PAUSE, false);
		delay_for_interval(ms, NSEC_PER_MSEC);
		ipa_pc_after();
	}
}

static void
ipa_step(const char *what)
{
	ipa_step_after(what, ipa_step_ms);
}

/* The hardware-init/SRAM section uses no pause of its own (ipa_hw_step_ms = 0): the clock vote
 * and the VERSION check before each group are the guard now, not a timer. */

/* For vayu_ipa_data.c: with vayu_ipa_pause_trace=1 only, the same announced (and traced) pause before
 * each GSI/endpoint group; without it nothing, so the data path's timing is unchanged. */
void
vayu_ipa_data_step(const char *what)
{
	if (ipa_pause_trace != 1) return;
	ipa_pc_stage = IPA_PS_DATA;
	ipa_step(what);
}

/* For vayu_ipa_data.c's per-access trace: a record only, no pause and no line (the caller says). */
void
vayu_ipa_data_mark(const char *tag, uint32_t pa, bool write, bool done)
{
	ipa_pc(IPA_PS_DATA, tag, pa, write ? IPA_PO_WRITE : IPA_PO_READ, done);
}

/* For vayu_ipa_smmu.c: announce a step with the normal pause (other files cannot see ipa_step). */
void
vayu_ipa_step(const char *what)
{
	ipa_step(what);
}

/*
 * Read the uC shared area: responseOp +0xc, responseParams +0x10, hwState +0x20,
 * interfaceVersionCommon +0x24 (struct IpaHwSharedMemCommonMapping_t, first 128 B of the SW SRAM).
 * Polls for INIT_COMPLETED up to `ms`, then says what it saw. Returns true when the uC said
 * IPA_HW_2_CPU_RESPONSE_INIT_COMPLETED (1).
 */
bool
vayu_ipa_uc_look(const char *when, uint64_t ms)
{
	vm_offset_t uc;
	uint32_t response = 0, params = 0, state = 0, iface = 0;
	uint64_t waited = 0, started = mach_absolute_time(), real_ns = 0;

	if (ipa_uc_skip == 1) {
		/* SM8150 is an MSM platform: stock loads no ipa_uc and the modem's INIT does not wait for
		 * the uC (lab VAYU_IPA_MEM_SETUP_AUDIT_20261031.md:99-104, CELLDATA_REPORT.md:348). */
		ipa_stage("%s: uC window not read (vayu_ipa_uc_skip=1)", when);
		return false;
	}
	ipa_pc_stage = IPA_PS_UC;
	if (ipa_uc_reads != 1) {
		static bool said;

		if (!said) {
			said = true;
			say("vayu-ipa: uC reads disabled (vayu_ipa_uc_read=0)\n");
		}
		return false;
	}
	/* The read that run 123126 died at. It is only allowed after a fresh clock vote and a
	 * VERSION read: if the block does not answer, this returns without touching 0x1e47000.
	 * After MPSS the modem's SMP2P "ipa" clock request holds the block; the AP then only looks,
	 * so its 125 MHz vote never overwrites a higher one the modem may have made. */
	if (!ipa_smp2p_ready) {
		if (!ipa_hw_group("uC shared area 0x1e47000")) return false;
	} else {
		uint32_t version = IPA_RD(0x34);

		if (version == 0 || version == 0xffffffffU) {
			say("vayu-ipa: %s: uC not read, IPA_VERSION %#x after MPSS\n", when, version);
			return false;
		}
	}
	uc = ml_io_map_unmappable(IPA_SRAM_V41, 0x1000, VM_WIMG_IO);
	if (uc == 0) {
		say("vayu-ipa: %s: uC shared area did not map\n", when);
		return false;
	}
	/* k31-cell2: announce the window before the first read of it (run 123126 died at exactly
	 * this read, and the old log could only be read backwards from the absence of a line). */
	say("vayu-ipa: %s: about to read the uC shared area at %#x (+0xc responseOp, +0x10 params), "
	    "polling up to %llu ms\n", when, (unsigned)IPA_SRAM_V41, ms);
	{
		/* A poll of seconds must not become the gap the whole fix is about: re-assert the vote
		 * and the VERSION check every 400 ms, before the next read of the window. */
		/* The rhythm and the end of the poll are real time now (b3-ipa2/3: the count of 10 ms
		 * sleeps ran several times slower than the clock). Before every read of the window the
		 * GDSC must still say PWR_ON, and each access leaves a breadcrumb for SUMMARY. */
		uint64_t begin = ipa_now_ms(), next_vote = begin + 400;

		for (;;) {
			uint32_t gdscr = ipa_gdscr();

			if (gdscr != 0 && !(gdscr & 0x80000000u)) {
				say("vayu-ipa: %s: uC poll stopped: IPA GDSC is off (GDSCR %#x) after %llu ms\n",
				    when, gdscr, ipa_now_ms() - begin);
				ipa_stage("%s: uC poll stopped, GDSC off (%#x)", when, gdscr);
				return false;
			}
			ipa_bc("uC window read", IPA_SRAM_V41 + 0xc);
			response = *(volatile uint32_t *)(uc + 0xc) & 0xff;
			ipa_pc_after();
			ipa_bc("uC window read", IPA_SRAM_V41 + 0x10);
			params = *(volatile uint32_t *)(uc + 0x10);
			ipa_pc_after();
			ipa_bc("uC poll sleep", 0);
			waited = ipa_now_ms() - begin;
			if (response == UC_RESPONSE_INIT_DONE || waited >= ms) break;
			delay_for_interval(10, NSEC_PER_MSEC);
			if (ipa_now_ms() >= next_vote && !ipa_smp2p_ready) {
				uint32_t version;

				next_vote = ipa_now_ms() + 400;
				ipa_bc("uC poll re-vote", 0);
				if (ipa_vote_rails("uC poll", false) == 0) return false;
				ipa_bc("uC poll IPA_VERSION read", 0x1e40034);
				version = *(volatile uint32_t *)(ipa_regs + 0x34);
				ipa_pc_after();
				if (version == 0 || version == 0xffffffffU) {
					say("vayu-ipa: %s: uC poll stopped, IPA_VERSION %#x at %llu ms\n",
					    when, version, waited);
					return false;
				}
			}
		}
	}
	ipa_bc("uC window read", IPA_SRAM_V41 + 0x20);
	state = *(volatile uint32_t *)(uc + 0x20) & 0xff;
	iface = *(volatile uint32_t *)(uc + 0x24) & 0xffff;
	ipa_pc_after();
	ipa_bc("uC poll done", 0);
	absolutetime_to_nanoseconds(mach_absolute_time() - started, &real_ns);
	/* "after N ms" counts the 10 ms sleeps; the real time is said too (b3-ipa2: 4 re-votes of the
	 * 400 ms rhythm spread over several seconds of log, so a sleep was much longer than 10 ms). */
	say("vayu-ipa: %s: uC responseOp %#x params %#x hwState %#x interface %#x after %llu ms (%llu ms real): %s\n",
	    when, response, params, state, iface, waited, real_ns / NSEC_PER_MSEC,
	    response == UC_RESPONSE_INIT_DONE ? "uC INIT_COMPLETED" : "uC NOT READY");
	if (response == UC_RESPONSE_INIT_DONE) {
		ipa_uc_seen_ready = true;
	}
	ipa_stage("%s: uC %s (responseOp %#x hwState %#x) after %llu ms real", when,
	    response == UC_RESPONSE_INIT_DONE ? "INIT_COMPLETED" : "not ready", response, state,
	    real_ns / NSEC_PER_MSEC);
	return response == UC_RESPONSE_INIT_DONE;
}

/* Read-only look after AUTH_AND_RESET(15): the block answers, GSI was enabled by TrustZone.
 * vayu_ipa_post_auth_ms is now only the hold *before* the first access (default 0: no gap after
 * AUTH, when the newly released uC starts fetching from DDR and needs the IPA->DDR path voted).
 * The reads themselves always happen, through a fresh vote and a VERSION check. */
static void
ipa_post_auth_look(void)
{
	vm_offset_t gsi;
	uint32_t cfg, status;

	ipa_pc_stage = IPA_PS_POSTAUTH;
	if (ipa_post_auth_ms != 0) {
		ipa_pc(IPA_PS_POSTAUTH, "post-AUTH hold", 0, IPA_PO_PAUSE, false);
		say("vayu-ipa: post-auth: AUTH_AND_RESET(15) RETURNED; holding %u ms before any IPA access\n",
		    ipa_post_auth_ms);
		if (ipa_hold_trace == 1 && ipa_post_auth_ms >= 1000) {
			uint32_t left = ipa_post_auth_ms;

			while (left >= 1000) {
				delay_for_interval(1000, NSEC_PER_MSEC);
				left -= 1000;
				say("vayu-ipa: post-auth: hold %u of %u ms is done (mach %llu, cpu %u)\n",
				    ipa_post_auth_ms - left, ipa_post_auth_ms,
				    mach_absolute_time(), cpu_number());
			}
			if (left != 0) {
				delay_for_interval(left, NSEC_PER_MSEC);
				say("vayu-ipa: post-auth: hold %u of %u ms is done (mach %llu, cpu %u)\n",
				    ipa_post_auth_ms, ipa_post_auth_ms, mach_absolute_time(), cpu_number());
			}
		} else {
			delay_for_interval(ipa_post_auth_ms, NSEC_PER_MSEC);
		}
		ipa_pc_after();
	}
	/* k31-cell2: the hold line above is the last one a live snapshot taken inside the window can
	 * carry. This line says the window is over, so a kernel that dies in the vote or in the read
	 * below leaves a named boundary instead of a silence. Behaviour is unchanged. */
	say("vayu-ipa: post-auth: the %u ms window is over; about to re-vote IP0/rails and read IPA_VERSION 0x1e40034\n",
	    ipa_post_auth_ms);
	if (!ipa_hw_group("post-auth: read IPA_VERSION 0x1e40034")) return;
	say("vayu-ipa: post-auth: IPA_VERSION %#x\n", IPA_RD(0x34));
	gsi = ml_io_map_unmappable(GSI_BASE, 0x20000, VM_WIMG_IO);
	if (gsi == 0) {
		say("vayu-ipa: post-auth: GSI window did not map\n");
		return;
	}
	ipa_pc(IPA_PS_POSTAUTH, "GSI_CFG", GSI_BASE, IPA_PO_READ, false);
	cfg = *(volatile uint32_t *)(gsi + 0x0);
	ipa_pc_after();
	ipa_pc(IPA_PS_POSTAUTH, "GSI_EE0_STATUS", GSI_BASE + 0x1f000, IPA_PO_READ, false);
	status = *(volatile uint32_t *)(gsi + 0x1f000);
	ipa_pc_after();
	say("vayu-ipa: post-auth: GSI_CFG %#x (enable %u), GSI_EE0_STATUS %#x (enabled %u)\n",
	    cfg, cfg & 1, status, status & 1);
	/* The uC shared area is read after ipa3_init_hw, like stock (ipa3_uc_interface_init is part of
	 * post_init, ipa.c:6458): before CLKON the IPA's internal clock domain is still closed. */
	say("vayu-ipa: post-auth: GSI %s; uC is read after the hardware init\n",
	    (status & 1) ? "READY" : "NOT ENABLED");
}

/*
 * Linux's ipa3_init_hw() (ipa_utils.c:4165) in its own order: IPA_VERSION, BCR, CLKON, TX_CFG,
 * QSB, counter, COMP, then ipa3_enable_dcd() (ipa_utils.c:8929 -> IDLE_INDICATION_CFG). The order
 * is not cosmetic: stock writes BCR 0x39 *before* CLKON, and BCR is the register that says what
 * the IPA clock controller may gate. The old code wrote CLKON first and BCR after it, which is the
 * one ordering difference that could re-gate the block right after it was opened.
 * Every group starts with a fresh IP0/rail vote and a VERSION read (ipa_hw_group), so a clock that
 * was lost during an earlier pause stops the sequence at a named line instead of wedging the SoC.
 * The resource groups and the exception route are vayu-r-oss values, unchanged.
 */
static bool
ipa_hardware_init(void)
{
	uint32_t v;

	ipa_pc_stage = IPA_PS_INIT_HW;
	ipa_gcc_look("before the vote");
	if (!ipa_hw_group("init_hw: read IPA_VERSION (ipa_utils.c:4170)")) return false;

	/* BCR v4_0 = 0x39 (IPA_BCR_REG_VAL_v4_0, ipa_utils.c:71). */
	if (!ipa_hw_group("init_hw: BCR 0x1e401d0 = 0x39")) return false;
	IPA_WR(0x1d0, 0x39);

	/* CLKON: open global + 2x (ipa_utils.c:4194-4200); see ipa_clkon_fix for the bits. */
	{
		uint32_t clkon = ipa_clkon_fix == 1 ? 0x30000000u : ipa_clkon_fix == 2 ? 0x30000003u : 3u;

		if (!ipa_hw_group(ipa_clkon_fix == 1 ? "init_hw: CLKON 0x1e40044 = 0x30000000 (vayu_ipa_clkon_fix=1)" :
		    ipa_clkon_fix == 2 ? "init_hw: CLKON 0x1e40044 = 0x30000003 (vayu_ipa_clkon_fix=2)" :
		    "init_hw: CLKON 0x1e40044 = 3")) return false;
		IPA_WR(0x44, clkon);
	}
	say("vayu-ipa: hardware init: CLKON now %#x\n", IPA_RD(0x44));

	/* TX_CFG: PA_MASK_EN off (ipa_utils.c:4202-4207). */
	if (!ipa_hw_group("init_hw: TX_CFG 0x1e401fc")) return false;
	IPA_WR(0x1fc, IPA_RD(0x1fc) & ~1U);

	/* QSB: DDR QMB 8 writes / 12 reads, PCIE QMB 4 / 12 (ipa3_qmb_outstanding[IPA_4_1],
	 * ipa_utils.c:687-688; ipa3_cfg_qsb, ipa_utils.c:4079). */
	if (!ipa_hw_group("init_hw: QSB 0x1e40074/0x1e40078")) return false;
	v = IPA_RD(0x74);
	IPA_WR(0x74, (v & ~0x000f000fU) | 8 | 4U << 16);      /* QMB0 writes 8, QMB1 writes 4 */
	v = IPA_RD(0x78);
	IPA_WR(0x78, (v & ~0x00ff000fU) | 12 | 120U << 16);   /* QMB0 reads 12 of 120 */

	/* Counter: 0.5 ms aggregation granularity (ipa_utils.c:4209-4213). */
	if (!ipa_hw_group("init_hw: counter 0x1e401f0 = 3")) return false;
	IPA_WR(0x1f0, 3);

	/* COMP: GSI multi-inorder reads/writes disabled (ipa_comp_cfg, ipa_utils.c:4035-4066);
	 * the two in-order fields are bits 7 and 8, the value the old code already used. */
	if (!ipa_hw_group("init_hw: COMP 0x1e4003c")) return false;
	v = IPA_RD(0x3c);
	IPA_WR(0x3c, v | 1U << 7 | 1U << 8);

	/* DCD: idle indication, debounce 256, const-non-idle off (ipa3_enable_dcd, ipa_utils.c:8929). */
	if (!ipa_hw_group("init_hw: idle 0x1e40220 = 256")) return false;
	IPA_WR(0x220, 256);
	say("vayu-ipa: hardware init: BCR %#x, CLKON %#x, COMP %#x, idle %#x\n",
	    IPA_RD(0x1d0), IPA_RD(0x44), IPA_RD(0x3c), IPA_RD(0x220));

	/* Source groups (valid only 0x400/0x420 and 0x404/0x424), v4.1:
	 * pkt contexts, descriptor lists, descriptor buffers, ACK entries; and the destination groups
	 * (0x500/0x520, 0x504/0x524): data sectors and DPS DMARS, UL_DL and LWA_DL. */
	if (!ipa_hw_group("resource groups 0x1e40400..0x1e4052c")) return false;
	IPA_WR(0x400, RSRC(1, 1, 0, 0));   /* GRP01 pkt contexts: UL_DL min/max 1 */
	IPA_WR(0x420, RSRC(1, 1, 0, 0));   /* GRP23 pkt contexts: LWA_DL min/max 1 */
	IPA_WR(0x404, RSRC(10, 10, 0, 0)); /* GRP01 descriptor lists: UL_DL 10 */
	IPA_WR(0x424, RSRC(10, 10, 0, 0)); /* GRP23 descriptor lists: LWA_DL 10 */
	IPA_WR(0x408, RSRC(12, 12, 0, 0)); /* GRP01 descriptor buffers: UL_DL 12 */
	IPA_WR(0x428, RSRC(14, 14, 0, 0)); /* GRP23 descriptor buffers: LWA_DL 14 */
	IPA_WR(0x40c, RSRC(14, 14, 0, 0)); /* GRP01 ACK entries: UL_DL 14 */
	IPA_WR(0x42c, RSRC(20, 20, 0, 0)); /* GRP23 ACK entries: LWA_DL 20 */
	IPA_WR(0x500, RSRC(4, 4, 0, 0));   /* GRP01 data sectors: UL_DL 4 */
	IPA_WR(0x520, RSRC(4, 4, 0, 0));   /* GRP23 data sectors: LWA_DL 4 */
	IPA_WR(0x504, RSRC(3, 3, 0, 0));   /* GRP01 data sectors: UL_DL 3 */
	IPA_WR(0x524, RSRC(3, 3, 0, 0));   /* GRP23 data sectors: LWA_DL 3 */
	IPA_WR(0x508, RSRC(2, 2, 0, 0));   /* GRP01 DPS DMARS: UL_DL 2 */
	IPA_WR(0x528, RSRC(2, 2, 0, 0));   /* GRP23 DPS DMARS: LWA_DL 2 */
	IPA_WR(0x50c, RSRC(1, 1, 0, 0));   /* GRP01 DPS DMARS: UL_DL 1 */
	IPA_WR(0x52c, RSRC(1, 1, 0, 0));   /* GRP23 DPS DMARS: LWA_DL 1 */

	if (!ipa_hw_group("exception route 0x1e40048")) return false;
	/* Exception route: LAN consumer endpoint 10, header table local, retain hdr. */
	IPA_WR(0x48, 0x1120052);

	ipa_gcc_look("after the vote and init_hw");
	say("vayu-ipa: hardware init: QSB %#x %#x, CLKON %#x, BCR %#x, COMP %#x, route %#x\n",
	    IPA_RD(0x74), IPA_RD(0x78), IPA_RD(0x44), IPA_RD(0x1d0), IPA_RD(0x3c), IPA_RD(0x48));
	return true;
}

/*
 * IPA local memory: zero the modem's table/proc-ctx area, write the Linux canaries
 * (_ipa_init_sram_v3, ipa.c:4226) and tell the uC where modem processing contexts start. Stock
 * does this after the APPS_CMD_PROD pipe; here it runs right before vayu_ipa_data_prepare(), which
 * owns that pipe. 125713 died at the first SRAM write ~15 s after CLKON; the group check below is
 * the guard that turns that into a named stop. vayu_ipa_sram_write=0 skips it.
 */
static bool
ipa_sram_canaries(void)
{
	uint32_t sram_pa = vayu_ipa_sram_base() + (ipa_sram_fix == 1 ? ipa_restricted : 0);
	static const uint16_t canaries[] = {
		0x0288, 0x0308, 0x0388, 0x0408, 0x0488, 0x0508, 0x0588, 0x0608,
		0x0688, 0x07d0, 0x0c30, 0x13d0, 0x13f0, 0x23fc,
	};
	vm_offset_t sram;

	ipa_pc_stage = IPA_PS_SRAM;
	if (ipa_sram_write != 1) {
		/*
		 * The control of the write (a9-ipa): everything the write path does around the stores --
		 * the 2.5 s pause, the vote and the VERSION read -- happens here too, so that a run with
		 * vayu_ipa_sram_write=0 differs from one with =1 by the SRAM stores and the proc-ctx write
		 * alone. (Before, this returned at once, and a hang in the pause looked like a hang in a store.)
		 */
		ipa_step("skip the SRAM canaries (vayu_ipa_sram_write=0): pause, vote and VERSION read only");
		if (!ipa_hw_group("SRAM canaries skipped")) return false;
		say("vayu-ipa: SRAM canaries skipped (vayu_ipa_sram_write=%u)\n", ipa_sram_write);
		return true;
	}
	/* The one pause that stays: the loader needs the window to carry the preceding lines out.
	 * The vote and the alive check come *after* it, so the pause cannot outlive the clock. */
	ipa_step(ipa_sram_fix == 1 ? "write SRAM canaries at 0x1e47000 (vayu_ipa_sram=1)" :
	    "write SRAM canaries at 0x1e50000 (old v4.5 offset)");
	if (!ipa_hw_group("SRAM canaries")) return false;
	sram = ml_io_map_unmappable(sram_pa, 0x4000, VM_WIMG_IO);
	if (sram == 0) {
		say("vayu-ipa: IPA SRAM window did not map\n");
		return false;
	}
	if (ipa_persist_page != NULL) {
		/* vayu_ipa_persist=1: 64 bytes at a time, each piece completed before its AFTER. */
		for (uint32_t at = 0x288; at < 0x2400; at += 0x40) {
			ipa_pc(IPA_PS_SRAM, "SRAM zero", sram_pa + at, IPA_PO_SRAM_ZERO, false);
			for (uint32_t w = at; w < at + 0x40 && w < 0x2400; w += 4) {
				*(volatile uint32_t *)(sram + w) = 0;
			}
			__asm__ volatile("dsb sy" ::: "memory");
			ipa_pc_after();
		}
	} else {
		for (uint32_t at = 0x288; at < 0x2400; at += 4) {
			*(volatile uint32_t *)(sram + at) = 0;
		}
	}
	ipa_pc(IPA_PS_SRAM, "SRAM canaries", sram_pa + canaries[0] - 8, IPA_PO_SRAM_CANARY, false);
	for (unsigned i = 0; i < sizeof(canaries) / sizeof(canaries[0]); i++) {
		uint32_t at = canaries[i];

		*(volatile uint32_t *)(sram + at - 8) = 0xdeadbeef;
		*(volatile uint32_t *)(sram + at - 4) = 0xdeadbeef;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	ipa_pc_after();
	ipa_pc_tag = "proc-ctx base 0x1e401e8";
	IPA_WR(0x1e8, ipa_restricted + 0x7d0); /* LOCAL_PKT_PROC_CNTXT_BASE */
	say("vayu-ipa: IPA SRAM zeroed/canaries written at %#x, proc-ctx base %#x\n", sram_pa,
	    IPA_RD(0x1e8));
	return true;
}

static bool ipa_smp2p_entry(uint32_t item, uint32_t *value);

static bool
ipa_smp2p_verify(void)
{
	uint32_t counts, name_at, value_at;

	if (ipa_our_item == 0) {
		ipa_our_item = vayu_smem_item(IPA_SM_HOST_MODEM, IPA_SMP2P_OURS, &(uint32_t){ 0 });
	}
	if (ipa_our_item == 0) {
		say("vayu-ipa: our outbound SMP2P item %u (AP -> modem) is absent after MPSS start\n", IPA_SMP2P_OURS);
		return false;
	}
	counts = ipa_smem_word(ipa_our_item + 12);
	name_at = ipa_our_item + 20 + 20;
	value_at = ipa_our_item + 36 + 20;
	/* Item 428 is ours (AP -> modem): reading 3 here proves only that our own pre-set clock answer
	 * is in place, nothing about the modem. The modem's entry is item 435, reported below. With
	 * vayu_ipa_smp2p_mainline=1 the entry starts at 0 and only has to exist. */
	if ((counts >> 16) < 2 || ipa_smem_word(name_at) != 0x00617069 ||
	    (ipa_smp2p_mainline == 0 && (ipa_smem_word(value_at) & 3) != 3)) {
		say("vayu-ipa: our outbound SMP2P item %u entry \"ipa\" is not %s (counts %#x, name %#x, value %#x)\n",
		    IPA_SMP2P_OURS, ipa_smp2p_mainline ? "there" : "the pre-set valid + clock-on",
		    counts, ipa_smem_word(name_at), ipa_smem_word(value_at));
		return false;
	}
	say("vayu-ipa: our outbound SMP2P item %u \"ipa\" = %#x verified after MPSS start (%s; AP side only, "
	    "not a modem signal)\n", IPA_SMP2P_OURS, ipa_smem_word(value_at),
	    ipa_smp2p_mainline ? "vayu_ipa_smp2p_mainline=1: 0 until the modem asks" : "pre-set valid + clock-on");
	{
		uint32_t theirs = 0;
		bool there = ipa_smp2p_entry(IPA_SMP2P_THEIRS, &theirs);

		say("vayu-ipa: the modem's inbound SMP2P item %u \"ipa\" (its clock query, bit 0): %s%#x\n",
		    IPA_SMP2P_THEIRS, there ? "present " : "absent ", theirs);
	}
	ipa_smp2p_ready = true;
	return true;
}

static bool
ipa_filter_table_prepare(void)
{
	ipa_filter_item = vayu_smem_alloc(IPA_SM_HOST_MODEM, IPA_FILTER_TABLE_ITEM,
	    IPA_FILTER_TABLE_SIZE, "vayu-ipa filter table");
	if (ipa_filter_item == 0) {
		say("vayu-ipa: cannot create SMEM item %u (%#x bytes) for the modem's filter tables\n",
		    IPA_FILTER_TABLE_ITEM, IPA_FILTER_TABLE_SIZE);
		return false;
	}
	say("vayu-ipa: SMEM filter-table item %u at %#x (%#x bytes), zeroed\n",
	    IPA_FILTER_TABLE_ITEM, 0x86000000U + ipa_filter_item, IPA_FILTER_TABLE_SIZE);
	return true;
}

bool
vayu_ipa_firmware_up(void)
{
	return ipa_fw_ready;
}

/* The "ipa" entry of SMP2P item `item` (host 1), found by name; false: no item or no entry. */
static bool
ipa_smp2p_entry(uint32_t item, uint32_t *value)
{
	uint32_t size = 0, at = vayu_smem_item(IPA_SM_HOST_MODEM, item, &size), valid;

	if (at == 0 || size < 20 || ipa_smem_word(at) != 0x504d5324) return false;
	valid = ipa_smem_word(at + 12) >> 16;
	for (uint32_t i = 0; i < valid && i < 16 && 20 + 20 * (i + 1) <= size; i++) {
		if (ipa_smem_word(at + 20 + 20 * i) == 0x00617069 && ipa_smem_word(at + 24 + 20 * i) == 0) {
			*value = ipa_smem_word(at + 36 + 20 * i);
			return true;
		}
	}
	return false;
}

/* Ours (428, apps->modem: bit 0 response done, bit 1 clock on) and the modem's (435, its clock query,
 * stock ipa3_smp2p_modem_clk_query_isr). The modem item may carry no "ipa" entry until it asks. */
void
vayu_ipa_smp2p_read(bool *ours_found, uint32_t *ours, bool *theirs_found, uint32_t *theirs)
{
	*ours = *theirs = 0;
	*ours_found = ipa_smp2p_entry(IPA_SMP2P_OURS, ours);
	*theirs_found = ipa_smp2p_entry(435, theirs);
}

/* True only after this boot's MPSS SMP2P verification (vayu_ipa_modem_probe), never from stale SMEM. */
bool
vayu_ipa_modem_started(void)
{
	return ipa_smp2p_ready;
}

bool
vayu_ipa_firmware_ready(void)
{
	return ipa_fw_ready && ipa_smp2p_ready;
}

/* The value our 428 "ipa" entry is made with before MPSS (vayu_pas.c our_item_fill): the old pre-set
 * 3, or 0 with vayu_ipa_smp2p_mainline=1. */
uint32_t
vayu_ipa_smp2p_initial(void)
{
	return ipa_smp2p_mainline ? 0 : IPA_SMP2P_VALID | IPA_SMP2P_CLOCK_ON;
}

bool
vayu_ipa_smp2p_is_mainline(void)
{
	return ipa_smp2p_mainline != 0;
}

/* Our 428 "ipa" entry to `value`; false: no item or no entry. */
static bool
ipa_smp2p_set_ours(uint32_t value)
{
	uint32_t size = 0, at = vayu_smem_item(IPA_SM_HOST_MODEM, IPA_SMP2P_OURS, &size), valid;

	if (at == 0 || size < 20 || ipa_smem_word(at) != 0x504d5324) return false;
	valid = ipa_smem_word(at + 12) >> 16;
	for (uint32_t i = 0; i < valid && i < 16 && 20 + 20 * (i + 1) <= size; i++) {
		if (ipa_smem_word(at + 20 + 20 * i) == 0x00617069 && ipa_smem_word(at + 24 + 20 * i) == 0) {
			vayu_smem_poke(at + 36 + 20 * i, value);
			__asm__ volatile("dsb sy" ::: "memory");
			return true;
		}
	}
	return false;
}

/*
 * vayu_ipa_smp2p_watch=1 / vayu_ipa_smp2p_mainline=1 (see the top of this file). Polls both "ipa"
 * entries every 5 ms for 30 minutes (there is no SMP2P interrupt here: vayu_pas.c has no GIC
 * doorbell handling); every change is one line. With the mainline switch a rising bit 0 of the
 * modem's entry is answered once, as ipa_smp2p_notify() does.
 */
static void
ipa_smp2p_watch_thread(void *argument __unused, wait_result_t wait __unused)
{
	uint64_t started = ipa_now_ms();
	uint32_t ours = 0, theirs = 0;
	bool ours_there = false, theirs_there = false, notified = false, first = true;

	while (!ipa_watch_stop && ipa_now_ms() - started < 30 * 60 * 1000) {
		uint32_t o = 0, t = 0;
		bool ot = ipa_smp2p_entry(IPA_SMP2P_OURS, &o), tt = ipa_smp2p_entry(IPA_SMP2P_THEIRS, &t);

		if (first || ot != ours_there || o != ours) {
			say("ipa-smp2p: +%llu ms: ours 428 \"ipa\" %s%#x\n", ipa_now_ms() - started, ot ? "" : "absent ", o);
		}
		if (first || tt != theirs_there || t != theirs) {
			say("ipa-smp2p: +%llu ms: the modem's 435 \"ipa\" %s%#x%s\n", ipa_now_ms() - started,
			    tt ? "" : "absent ", t, tt && (t & 1) && !(theirs & 1) ? " (bit 0 rose: a clock query)" : "");
		}
		if (ipa_smp2p_mainline && !notified && tt && (t & 1) && !(theirs_there && (theirs & 1))) {
			/* ipa_smp2p_notify(): the enabled flag first, then the valid flag; the IP0 vote is held
			 * for the whole run, so "enabled" is true (vendor: ipa_clk_on). */
			bool done = ipa_smp2p_set_ours(IPA_SMP2P_CLOCK_ON);

			vayu_apcs_ring(IPA_APCS_MODEM);
			done = ipa_smp2p_set_ours(IPA_SMP2P_CLOCK_ON | IPA_SMP2P_VALID) && done;
			vayu_apcs_ring(IPA_APCS_MODEM);
			notified = true;
			say("ipa-smp2p: +%llu ms: ANSWERED the modem's clock query: ours 428 \"ipa\" -> enabled, then valid "
			    "(%s), an APCS ring after each\n", ipa_now_ms() - started, done ? "written" : "ERROR no entry");
			ipa_smp2p_entry(IPA_SMP2P_OURS, &o);
		}
		ours = o;
		ours_there = ot;
		theirs = t;
		theirs_there = tt;
		first = false;
		delay_for_interval(5, NSEC_PER_MSEC);
	}
	say("ipa-smp2p: watcher done (ours %#x, the modem's %s%#x%s)\n", ours, theirs_there ? "" : "absent ", theirs,
	    notified ? ", the query was answered" : "");
	ipa_watch_running = false;
	thread_terminate(current_thread());
}

/* vayu_ipa_uc_watch=1 (see the top of this file). */
static void
ipa_uc_watch_thread(void *argument __unused, wait_result_t wait __unused)
{
	uint64_t started = ipa_now_ms(), next_fault = 0;
	uint32_t last[10] = { 0 }, faults_said = 0;
	bool first = true, init_seen = false;
	vm_offset_t sram = ml_io_map_unmappable(vayu_ipa_sram_base(), 0x1000, VM_WIMG_IO);

	if (sram == 0 || ipa_regs == 0) {
		say("ipa-uc: watch: the SRAM window or the IPA registers did not map\n");
		thread_terminate(current_thread());
		return;
	}
	say("ipa-uc: watch: uC shared area at %#x, every 500 ms for 30 minutes (read only)\n", vayu_ipa_sram_base());
	while (!ipa_watch_stop && ipa_now_ms() - started < 30 * 60 * 1000) {
		uint32_t version = *(volatile uint32_t *)(ipa_regs + 0x34), word[10];

		if (version == 0 || version == 0xffffffffU) {
			say("ipa-uc: +%llu ms: IPA_VERSION %#x: the block does not answer; the uC area is not read\n",
			    ipa_now_ms() - started, version);
			delay_for_interval(2000, NSEC_PER_MSEC);
			continue;
		}
		for (uint32_t i = 0; i < 10; i++) {
			word[i] = *(volatile uint32_t *)(sram + 4 * i);
		}
		if (first || memcmp(word, last, sizeof(word)) != 0) {
			uint8_t response = word[3] & 0xff, event = word[5] & 0xff, hw_state = word[8] & 0xff;

			say("ipa-uc: +%llu ms: command %#x/%#x/%#x, response %u (%s) param %#x, event %u param %#x, "
			    "first error address %#x, hw_state %u warnings %u, interface %#x\n", ipa_now_ms() - started,
			    word[0] & 0xff, word[1], word[2], response, response == 1 ? "INIT_COMPLETED" :
			    response == 2 ? "CMD_COMPLETED" : response == 0 ? "none" : "other", word[4], event, word[6],
			    word[7], hw_state, (word[8] >> 8) & 0xff, word[9] & 0xffff);
			if (response == 1 && !init_seen) {
				init_seen = true;
				ipa_uc_seen_ready = true;
				say("ipa-uc: +%llu ms: THE uC SAYS INIT_COMPLETED (interface %#x)\n", ipa_now_ms() - started,
				    word[9] & 0xffff);
			}
			memcpy(last, word, sizeof(last));
			first = false;
		}
		if (ipa_now_ms() >= next_fault && faults_said < 30) {
			next_fault = ipa_now_ms() + 10000;
			faults_said++;
			(void)vayu_ipa_smmu_faults("uc-watch");
		}
		delay_for_interval(500, NSEC_PER_MSEC);
	}
	say("ipa-uc: watch done (INIT_COMPLETED %s)\n", init_seen ? "seen" : "never seen");
	thread_terminate(current_thread());
}

static void
ipa_uc_watch_start(void)
{
	static bool started;
	thread_t thread;

	if (ipa_uc_watch == 0 || started) return;
	started = true;
	if (kernel_thread_start(ipa_uc_watch_thread, NULL, &thread) != KERN_SUCCESS) {
		say("ipa-uc: ERROR no watch thread\n");
		return;
	}
	thread_deallocate(thread);
}

static void
ipa_smp2p_watch_start(void)
{
	thread_t thread;

	if ((ipa_smp2p_watch == 0 && ipa_smp2p_mainline == 0) || ipa_watch_running) return;
	ipa_watch_stop = false;
	ipa_watch_running = true;
	if (kernel_thread_start(ipa_smp2p_watch_thread, NULL, &thread) != KERN_SUCCESS) {
		ipa_watch_running = false;
		say("ipa-smp2p: ERROR no watcher thread\n");
		return;
	}
	thread_deallocate(thread);
	say("ipa-smp2p: watcher started (vayu_ipa_smp2p_watch=%u, vayu_ipa_smp2p_mainline=%u: %s)\n", ipa_smp2p_watch,
	    ipa_smp2p_mainline, ipa_smp2p_mainline ? "a clock query is answered once" : "read only");
}

/*
 * The SUMMARY thread reads plain variables only (no IPA register, no lock of this file) and runs
 * bound to CPU vayu_ipa_summary_cpu (default 0); with the summary on, the IPA bring-up itself is
 * bound to CPU vayu_ipa_cpu (default 1). b3-ipa3: the one SUMMARY line came at t+20 s and none
 * after, while the system ran six more minutes: a thread whose CPU is stuck in a bus read is never
 * woken, and the summary's own timer and run queue sat on whatever CPU it last ran on. Two
 * distinct bound CPUs keep the watcher off the CPU that does the risky reads.
 */
static uint32_t ipa_summary_cpu, ipa_cpu = 1;
extern processor_t thread_bind(processor_t processor);
extern processor_t cpu_to_processor(int cpu);

static void
ipa_bind_self(uint32_t cpu, const char *who)
{
	processor_t processor = cpu_to_processor((int)cpu);

	if (processor == PROCESSOR_NULL) {
		say("vayu-ipa: %s: no CPU %u to bind to; left unbound\n", who, cpu);
		return;
	}
	(void)thread_bind(processor);
	delay_for_interval(1, NSEC_PER_MSEC);   /* the binding takes effect at the next block */
	say("vayu-ipa: %s bound to CPU %u (now on CPU %u)\n", who, cpu, (unsigned)cpu_number());
}

static void
ipa_summary_thread(void *argument __unused, wait_result_t wait __unused)
{
	ipa_bind_self(ipa_summary_cpu, "SUMMARY");
	for (uint32_t t = ipa_summary_s; t <= 1800; t += ipa_summary_s) {
		uint64_t now;

		delay_for_interval(ipa_summary_s, NSEC_PER_SEC);
		now = ipa_now_ms();
		say("vayu-ipa: SUMMARY t+%us (CPU %u): stage \"%s\"; last access \"%s\" %#x #%u on CPU %u, %llu ms ago; "
		    "fw %u uC %u SMP2P %u; handshake %s; GSI data %s\n",
		    t, (unsigned)cpu_number(), ipa_stage_text, ipa_bc_what, ipa_bc_addr, ipa_bc_count, ipa_bc_cpu,
		    ipa_bc_ms != 0 && now >= ipa_bc_ms ? now - ipa_bc_ms : 0ULL,
		    ipa_fw_ready ? 1 : 0, ipa_uc_seen_ready ? 1 : 0, ipa_smp2p_ready ? 1 : 0,
		    vayu_ipa_ready() ? "READY" : "not ready", vayu_ipa_data_ready() ? "READY" : "not ready");
	}
	thread_terminate(current_thread());
}

static bool ipa_bound;

static void
ipa_prepare_once(void)
{
	uint32_t enabled = 0, steps = 0, loader = 0;

	if (!PE_parse_boot_argn("vayu_ipa", &enabled, sizeof(enabled)) || enabled != 1) {
		if (enabled != 0) say("vayu-ipa: vayu_ipa=%u refused: only 1 enables IPA\n", enabled);
		return;
	}
	if (ipa_prepared) return;
	ipa_enabled = true;
	PE_parse_boot_argn("vayu_ipa_bus", &ipa_bus_mask, sizeof(ipa_bus_mask));
	PE_parse_boot_argn("vayu_ipa_post_auth_ms", &ipa_post_auth_ms, sizeof(ipa_post_auth_ms));
	PE_parse_boot_argn("vayu_ipa_sram", &ipa_sram_fix, sizeof(ipa_sram_fix));
	PE_parse_boot_argn("vayu_ipa_uc_read", &ipa_uc_reads, sizeof(ipa_uc_reads));
	PE_parse_boot_argn("vayu_ipa_step_ms", &ipa_step_ms, sizeof(ipa_step_ms));
	PE_parse_boot_argn("vayu_ipa_hw_step_ms", &ipa_hw_step_ms, sizeof(ipa_hw_step_ms));
	PE_parse_boot_argn("vayu_ipa_sram_write", &ipa_sram_write, sizeof(ipa_sram_write));
	PE_parse_boot_argn("vayu_ipa_bus_core", &ipa_bus_core, sizeof(ipa_bus_core));
	PE_parse_boot_argn("vayu_ipa_clk", &ipa_clk_mode, sizeof(ipa_clk_mode));
	PE_parse_boot_argn("vayu_ipa_hold_trace", &ipa_hold_trace, sizeof(ipa_hold_trace));
	PE_parse_boot_argn("vayu_ipa_clk_level", &ipa_clk_level, sizeof(ipa_clk_level));
	PE_parse_boot_argn("vayu_ipa_clk_kbps", &ipa_clk_kbps, sizeof(ipa_clk_kbps));
	if (ipa_clk_kbps == 0 || ipa_clk_kbps > 1000000) ipa_clk_kbps = 125;
	if (PE_parse_boot_argn("vayu_ipa_clk_raw", &ipa_clk_raw, sizeof(ipa_clk_raw))) ipa_clk_raw_set = 1;
	PE_parse_boot_argn("vayu_ipa_cmd_db", &ipa_cmd_db, sizeof(ipa_cmd_db));
	PE_parse_boot_argn("vayu_ipa_uc_wait_ms", &ipa_uc_wait_ms, sizeof(ipa_uc_wait_ms));
	PE_parse_boot_argn("vayu_ipa_gcc_survey", &ipa_gcc_survey, sizeof(ipa_gcc_survey));
	PE_parse_boot_argn("vayu_ipa_summary", &ipa_summary_s, sizeof(ipa_summary_s));
	PE_parse_boot_argn("vayu_ipa_uc_skip", &ipa_uc_skip, sizeof(ipa_uc_skip));
	PE_parse_boot_argn("vayu_ipa_smp2p_watch", &ipa_smp2p_watch, sizeof(ipa_smp2p_watch));
	PE_parse_boot_argn("vayu_ipa_uc_pas", &ipa_uc_pas, sizeof(ipa_uc_pas));
	PE_parse_boot_argn("vayu_ipa_uc_watch", &ipa_uc_watch, sizeof(ipa_uc_watch));
	if (ipa_uc_watch > 1) ipa_uc_watch = 0;
	if (ipa_uc_pas > 1) ipa_uc_pas = 0;
	PE_parse_boot_argn("vayu_ipa_smp2p_mainline", &ipa_smp2p_mainline, sizeof(ipa_smp2p_mainline));
	if (ipa_smp2p_watch > 1) ipa_smp2p_watch = 0;
	if (ipa_smp2p_mainline > 1) ipa_smp2p_mainline = 0;
	if (ipa_smp2p_watch | ipa_smp2p_mainline) {
		say("vayu-ipa: vayu_ipa_smp2p_watch=%u vayu_ipa_smp2p_mainline=%u: our 428 \"ipa\" starts at %#x%s\n",
		    ipa_smp2p_watch, ipa_smp2p_mainline, vayu_ipa_smp2p_initial(),
		    ipa_smp2p_mainline ? " and the modem's 435 clock query is answered" : "");
	}
	PE_parse_boot_argn("vayu_ipa_extra_bus", &ipa_extra_bus, sizeof(ipa_extra_bus));
	PE_parse_boot_argn("vayu_ipa_xbus_bw", &ipa_xbus_bw, sizeof(ipa_xbus_bw));
	if (ipa_xbus_bw > 1) ipa_xbus_bw = 0;
	if ((ipa_extra_bus & ~0xfu) != 0) {
		say("ipa-xbus: vayu_ipa_extra_bus=%#x: only bits 1/2/4/8 exist; the rest is ignored\n", ipa_extra_bus);
		ipa_extra_bus &= 0xf;
	}
	PE_parse_boot_argn("vayu_ipa_persist", &ipa_persist, sizeof(ipa_persist));
	PE_parse_boot_argn("vayu_ipa_pause_trace", &ipa_pause_trace, sizeof(ipa_pause_trace));
	PE_parse_boot_argn("vayu_ipa_post_init_stop", &ipa_post_init_stop, sizeof(ipa_post_init_stop));
	PE_parse_boot_argn("vayu_ipa_fault_dump", &ipa_fault_dump, sizeof(ipa_fault_dump));
	PE_parse_boot_argn("vayu_ipa_clkon_fix", &ipa_clkon_fix, sizeof(ipa_clkon_fix));
	if (ipa_clkon_fix > 2) ipa_clkon_fix = 0;
	if (ipa_clkon_fix != 0) {
		say("vayu-ipa: vayu_ipa_clkon_fix=%u: CLKON_CFG will be %#x (old: 3)\n", ipa_clkon_fix,
		    ipa_clkon_fix == 1 ? 0x30000000u : 0x30000003u);
	}
	if (ipa_pause_trace | ipa_post_init_stop | ipa_fault_dump) {
		say("vayu-ipa: pause_trace %u, post_init_stop %u, fault_dump %u\n", ipa_pause_trace, ipa_post_init_stop,
		    ipa_fault_dump);
	}
	ipa_persist_init();
	if (ipa_extra_bus != 0) {
		say("ipa-xbus: vayu_ipa_extra_bus=%#x (%s%s%s%s), requests vayu_ipa_xbus_bw=%u (%s)\n", ipa_extra_bus,
		    ipa_extra_bus & 1 ? " SN11" : "", ipa_extra_bus & 2 ? " SN1" : "", ipa_extra_bus & 4 ? " CN0" : "",
		    ipa_extra_bus & 8 ? " SN0" : "", ipa_xbus_bw, ipa_xbus_bw ? "msm-4.14 SVS2" : "mainline ipa_data-v4.2");
	}
	if (ipa_summary_s != 0) {
		thread_t summary;

		if (ipa_summary_s < 5 || ipa_summary_s > 600) ipa_summary_s = 20;
		PE_parse_boot_argn("vayu_ipa_summary_cpu", &ipa_summary_cpu, sizeof(ipa_summary_cpu));
		PE_parse_boot_argn("vayu_ipa_cpu", &ipa_cpu, sizeof(ipa_cpu));
		if (ipa_cpu == ipa_summary_cpu) {
			ipa_cpu = ipa_summary_cpu == 1 ? 2 : 1;
		}
		ipa_bind_self(ipa_cpu, "IPA bring-up");
		ipa_bound = true;
		if (kernel_thread_start(ipa_summary_thread, NULL, &summary) == KERN_SUCCESS) {
			thread_deallocate(summary);
			say("vayu-ipa: vayu_ipa_summary=%u: a SUMMARY line every %u s for 30 minutes\n",
			    ipa_summary_s, ipa_summary_s);
		}
	}
	if (ipa_step_ms > 10000) ipa_step_ms = 2500;
	if (ipa_hw_step_ms > 10000) ipa_hw_step_ms = 0;
	if (ipa_uc_wait_ms > 10000) ipa_uc_wait_ms = 2000;
	if (PE_parse_boot_argn("vayu_ipa_pas", &steps, sizeof(steps))) {
		vayu_pas_set_steps(steps);
	}
	say("vayu-ipa: PAS steps %#x (bit 0 forced shutdown, 1 image, 2 AUTH, 3 hardware init)\n",
	    vayu_pas_steps());
	say("vayu-ipa: bus %#x (core %u), post_auth_ms %u, step_ms %u/%u, sram %u write %u, uc_read %u wait %u\n",
	    ipa_bus_mask, ipa_bus_core, ipa_post_auth_ms, ipa_step_ms, ipa_hw_step_ms, ipa_sram_fix,
	    ipa_sram_write, ipa_uc_reads, ipa_uc_wait_ms);
	say("vayu-ipa: clock: mode %u level %u kbps %u%s, gcc survey %u, cmd-db survey %u\n", ipa_clk_mode,
	    ipa_clk_level, ipa_clk_kbps, ipa_clk_raw_set ? " (+ raw override)" : "", ipa_gcc_survey, ipa_cmd_db);
	if ((vayu_pas_steps() & VAYU_PAS_STEP_AUTH) && !(vayu_pas_steps() & VAYU_PAS_STEP_IMAGE)) {
		say("vayu-ipa: vayu_ipa_pas asks for AUTH without an image: refused\n");
		return;
	}
	loader_checkpoint(101);
	say("vayu-ipa: prepare: IP0, SMEM/SMP2P, PAS ipa_fws before MPSS\n");
	if (!vayu_soc_check("vayu-ipa")) {
		say("vayu-ipa: not vayu's shared memory/command database\n");
		return;
	}
	/* SM8150's IPA core clock is the RPMh IP0 resource, voted here as the interconnect does
	 * (use_bw_vote). The GCC branch writes at +0x89014.. hung the phone on hardware, so they stay
	 * opt-in: vayu_ipa_gcc=1. vayu_ipa_cmd_db=1 lists the BCM resources for the next run. */
	ipa_pc_stage = IPA_PS_VOTE;
	ipa_pc_tag = "ipa clock on";
	if (!ipa_clock_on()) return;
	if (ipa_cmd_db == 1) ipa_cmd_db_survey();
	ipa_gcc_look("after the IP0 vote");
	loader_checkpoint(102);
	{
		uint32_t gcc = 0;

		if (PE_parse_boot_argn("vayu_ipa_gcc", &gcc, sizeof(gcc)) && gcc == 1 && !ipa_gcc_enable()) return;
	}
	if (!vayu_ipa_smmu_enable()) {
		say("vayu-ipa: SMMU prerequisite failed; not starting IPA\n");
		return;
	}
	ipa_smmu_survey();
	loader_checkpoint(103);
	if (!ipa_filter_table_prepare()) return;
	ipa_pc(IPA_PS_CHECK, "IPA_VERSION/SHARED_MEM_SIZE", 0x1e40034, IPA_PO_READ, false);
	if (!ipa_registers_check()) {
		say("vayu-ipa: IPA CFG is not alive before PAS; no firmware start\n");
		return;
	}
	ipa_pc_after();
	loader_checkpoint(104);
	if (PE_parse_boot_argn("vayu_ipa_loader", &loader, sizeof(loader)) && loader == 1) {
		ipa_modem_loader = true;
	}
	if (ipa_modem_loader) {
		/*
		 * vayu_ipa_loader=1: the AP does not touch PAS(15) at all. The modem is started as usual
		 * and vayu_ipa_modem_probe(), after MPSS is up, reads whether MPSS brought the GSI/IPA up
		 * by itself. This is the zero-risk test of the "who loads the GSI" question: on vayu the DT
		 * has qcom,pil-tz-generic (AP loader) and no qcom,gsi-loader, but the pil_ipa_gsi_region at
		 * 0x98f10000 is unused by the AP tree, so the ownership is worth observing, not assuming.
		 */
		say("vayu-ipa: loader=modem: AP PAS(15) skipped; clock/SMMU/SMEM filter are prepared, the modem is watched\n");
		ipa_prepared = true;
		return;
	}
	if (!vayu_pas_supported(&ipa_fw)) {
		say("vayu-ipa: TrustZone does not answer for ipa_fws\n");
		return;
	}
	if (!vayu_pas_load(&ipa_fw)) {
		say("vayu-ipa: ipa_fws was not loaded\n");
		ipa_stage("ipa_fws was not loaded (no image in fw/?)");
		return;
	}
	if (!(vayu_pas_steps() & VAYU_PAS_STEP_AUTH)) {
		say("vayu-ipa: dry stop after the image: AUTH_AND_RESET and hardware init not done; IPA stays down\n");
		ipa_prepared = true;
		return;
	}
	ipa_pc(IPA_PS_PAS, "AUTH_AND_RESET(15)", 0, IPA_PO_STEP, false);
	if (!vayu_pas_start(&ipa_fw, 10000)) {
		say("vayu-ipa: ipa_fws did not come up\n");
		ipa_stage("ipa_fws did not come up (AUTH_AND_RESET)");
		vayu_pas_stop(&ipa_fw, "IPA did not come up");
		return;
	}
	ipa_pc_after();
	ipa_faults(IPA_PS_PAS, "after AUTH_AND_RESET(15)");
	ipa_stage("AUTH_AND_RESET(15) returned; post-AUTH look next");
	ipa_post_auth_look();
	ipa_stage("post-AUTH look done");
	if (vayu_pas_steps() & VAYU_PAS_STEP_HW) {
		if (!ipa_hardware_init()) {
			say("vayu-ipa: hardware init did not finish (a group above did not see the clock); "
			    "SRAM/GSI are not touched, IPA stays half-started\n");
			ipa_stage("hardware init did not finish");
			return;
		}
		ipa_stage("hardware init done");
		ipa_faults(IPA_PS_INIT_HW, "after init_hw");
		if (ipa_post_init_stop == 1) {
			/*
			 * vayu_ipa_post_init_stop=1: nothing of ours touches IPA or GSI from here on -- no vote, no
			 * register, no SRAM. Only lines (and breadcrumbs, pa = seconds). b9's c1 stopped with no SRAM
			 * store; if this stops too, the cause is what runs by itself after AUTH and init_hw.
			 */
			say("vayu-ipa: post-init stop (vayu_ipa_post_init_stop=1): no IPA/GSI access from now on, "
			    "a line a second for 120 s\n");
			for (uint32_t t = 1; t <= 120; t++) {
				ipa_pc(IPA_PS_INIT_HW, "post-init stop", t - 1, IPA_PO_PAUSE, false);
				delay_for_interval(1000, NSEC_PER_MSEC);
				say("vayu-ipa: post-init stop: %u of 120 s alive, no IPA/GSI access since init_hw "
				    "(mach %llu, cpu %u)\n", t, mach_absolute_time(), (unsigned)cpu_number());
				__asm__ volatile("dsb sy" ::: "memory");
				ipa_faults(IPA_PS_INIT_HW, "post-init stop");
			}
			ipa_pc(IPA_PS_INIT_HW, "post-init stop", 120, IPA_PO_PAUSE, true);
			ipa_stage("post-init stop: 120 s alive without an IPA access; bring-up ends here (half-started)");
			return;
		}
		if (!ipa_sram_canaries()) {
			say("vayu-ipa: SRAM canaries were not written (a group above did not see the clock); "
			    "GSI is not touched, IPA stays half-started\n");
			ipa_stage("SRAM canaries not written");
			return;
		}
		ipa_stage("SRAM canaries written; uC poll next");
	} else {
		say("vayu-ipa: hardware/SRAM init skipped (vayu_ipa_pas bit 3 off)\n");
	}
	/* vayu_ipa_data=1: the four GSI channels, endpoints and the empty tables, still before MPSS
	 * (stock ipa3_load_ipa_fw -> ipa3_post_init). The QMI handshake stays separate. */
	if (ipa_uc_reads != 1) {
		say("vayu-data: S0 IPA UC: SKIP uC reads disabled (vayu_ipa_uc_read=0)\n");
	} else if (ipa_uc_skip == 1) {
		(void)vayu_ipa_uc_look("post-init", ipa_uc_wait_ms);      /* (says the skip as a STAGE line) */
		say("vayu-data: S0 IPA UC: SKIP the uC window is not read (vayu_ipa_uc_skip=1)\n");
	} else if (vayu_ipa_uc_look("post-init", ipa_uc_wait_ms)) {
		say("vayu-data: S0 IPA UC: OK uC INIT_COMPLETED after the hardware init\n");
	} else {
		say("vayu-data: S0 IPA UC: ERROR uC NOT READY after the hardware init (stock sees the same "
		    "for ~1 s; the uC can still be alive, its event log arrives later)\n");
	}
	/* k31-cell2: the GSI data path starts with a read of GSI_STATUS/GSI_SW_VERSION; say so before
	 * the first one, so a hang there is not mistaken for a hang in the uC read above. */
	say("vayu-ipa: about to prepare the GSI data path (a no-op unless vayu_ipa_data=1)\n");
	ipa_stage("uC poll done (%s); GSI data path next", ipa_uc_seen_ready ? "INIT_COMPLETED" : "not ready");
	ipa_pc_stage = IPA_PS_DATA;
	/* (with uc_skip=1 the SRAM pause was the last one before the data path: b9 r1-r4/c1 could not
	 * tell its first GSI access from the SRAM step. vayu_ipa_pause_trace=1 puts a pause here.) */
	vayu_ipa_data_step("enter vayu_ipa_data_prepare (GSI data path)");
	ipa_pc(IPA_PS_DATA, "vayu_ipa_data_prepare", 0, IPA_PO_STEP, false);
	if (!vayu_ipa_data_prepare()) {
		say("vayu-ipa: data path preparation failed; IPA stays up, WDS Start will be blocked\n");
		ipa_stage("GSI data path NOT prepared; firmware ready next");
	} else {
		say("vayu-ipa: GSI data path prepare returned (ready %u)\n", vayu_ipa_data_ready() ? 1 : 0);
		ipa_stage("GSI data path %s; firmware ready next", vayu_ipa_data_ready() ? "READY" : "prepared, not ready");
	}
	ipa_pc_after();
	if (ipa_uc_pas == 1) {
		ipa_stage("vayu_ipa_uc_pas=1: PAS 0x1B ipa_uc next");
		if (!vayu_pas_supported(&ipa_uc_fw)) {
			say("vayu-ipa: vayu_ipa_uc_pas=1: TrustZone does not support PAS 0x1B; the uC is not loaded\n");
		} else if (!vayu_pas_load(&ipa_uc_fw)) {
			say("vayu-ipa: vayu_ipa_uc_pas=1: ipa_uc was not loaded (no image in fw/?)\n");
		} else if (!vayu_pas_start(&ipa_uc_fw, 2000)) {
			say("vayu-ipa: vayu_ipa_uc_pas=1: TrustZone did not start ipa_uc\n");
		} else {
			say("vayu-ipa: vayu_ipa_uc_pas=1: ipa_uc AUTHENTICATED AND STARTED by TrustZone (PAS 0x1B)\n");
		}
		ipa_stage("vayu_ipa_uc_pas=1: ipa_uc %s", ipa_uc_fw.started ? "started" : "NOT started");
	}
	loader_checkpoint(112);
	say("vayu-ipa: IPA firmware authenticated/reset (no own SMP2P item); our modem-edge item 428 will carry \"ipa\"\n");
	/*
	 * The modem's SMP2P item 428 does not exist until MPSS runs (a cold boot made the old
	 * verify-before-MPSS circular: no item -> not ready -> no "ipa" entry -> the modem never
	 * starts its IPA task). It is verified in vayu_ipa_modem_probe() after MPSS is up.
	 */
	ipa_fw_ready = true;
	ipa_prepared = true;
	ipa_stage("firmware ready; waiting for MPSS and our 428 \"ipa\" entry");
	ipa_pc(IPA_PS_READY, "firmware ready", 0, IPA_PO_STEP, true);
	say("vayu-ipa: firmware ready; our 428 \"ipa\" entry is verified after MPSS starts, then QRTR INIT_MODEM_DRIVER\n");
}

/* The bring-up, then the modem thread is unbound again: the binding is only for the IPA's own
 * accesses (vayu_ipa_cpu, with vayu_ipa_summary), never for what the modem thread does next. */
void
vayu_ipa_prepare(void)
{
	ipa_prepare_once();
	if (ipa_bound) {
		(void)thread_bind(PROCESSOR_NULL);
		delay_for_interval(1, NSEC_PER_MSEC);
		ipa_bound = false;
		say("vayu-ipa: IPA bring-up unbound (now on CPU %u); STAGE \"%s\"\n", (unsigned)cpu_number(),
		    ipa_stage_text);
	}
}

/*
 * Called by vayu_modem.c after MPSS is up (step 2). -1: vayu_ipa was not asked for; 0: not up;
 * 1: IPA/GSI up. With vayu_ipa_loader=1 the AP did no PAS at all, so "up" can only have come from
 * the modem: the SMP2P "ipa" entry, GSI MCS/STATUS, and IPA_VERSION are read and logged.
 */
int
vayu_ipa_modem_probe(void)
{
	uint32_t enabled = 0, regs = 0, item, counts = 0, value = 0, version = 0, status = 0, mcs = 0, error = 0;
	bool observe_regs = PE_parse_boot_argn("vayu_ipa_observe_regs", &regs, sizeof(regs)) && regs == 1;
	vm_offset_t gsi = 0;
	bool up;

	if (!PE_parse_boot_argn("vayu_ipa", &enabled, sizeof(enabled)) || enabled != 1) {
		return -1;
	}
	if (ipa_smp2p_ready) {
		return 1;
	}
	if (!ipa_modem_loader && !ipa_fw_ready) {
		say("vayu-ipa: after MPSS: the AP's IPA preparation did not complete; its SMP2P entry is not verified\n");
		return 0;
	}
	item = vayu_smem_item(IPA_SM_HOST_MODEM, IPA_SMP2P_OURS, &(uint32_t){ 0 });   /* an SMEM offset */
	if (item != 0) {
		counts = ipa_smem_word(item + 12);
		value = ipa_smem_word(item + 36 + 20);
	}
	ipa_smp2p_watch_start();                /* vayu_ipa_smp2p_watch / _mainline only */
	ipa_uc_watch_start();                   /* vayu_ipa_uc_watch only */
	/*
	 * Registers only when asked (vayu_ipa_observe_regs=1). The SMP2P item is safe to read; a
	 * register read of a block the modem may have gated is exactly the class of access that
	 * hangs this phone, and the observe mode must not become the next hang. Even then GSI is
	 * read only after IPA_VERSION says the IPA block is alive.
	 */
	if (observe_regs && ipa_regs != 0) {
		version = *(volatile uint32_t *)(ipa_regs + 0x34);
	}
	if (observe_regs && version != 0 && version != 0xffffffffU) {
		gsi = ml_io_map_unmappable(0x1e04000, 0x28000, VM_WIMG_IO);
		if (gsi != 0) {
			status = *(volatile uint32_t *)(gsi + 0x0);
			mcs = *(volatile uint32_t *)(gsi + 0xb000);
			error = *(volatile uint32_t *)(gsi + 0x200);
		}
	}
	say("vayu-ipa: after MPSS: loader=%s, our SMP2P item %u (at %#x) counts %#x \"ipa\" %#x; registers %s (IPA_VERSION %#x, GSI_STATUS %#x, MCS_CFG %#x, ERROR_LOG %#x)\n",
	    ipa_modem_loader ? "modem" : "AP", IPA_SMP2P_OURS, item, counts, value,
	    observe_regs ? "read" : "not read (vayu_ipa_observe_regs=1 to read)",
	    version, status, mcs, error);
	up = ipa_smp2p_verify();                /* logs what it sees; sets ipa_smp2p_ready */
	ipa_stage("after MPSS: our 428 \"ipa\" %s (value %#x; AP side only)", up ? "verified" : "NOT verified", value);
	(void)vayu_ipa_uc_look("after-mpss", 0); /* the modem-side IPA task may load/start the uC */
	if (ipa_modem_loader) {
		if (up || (observe_regs && ((mcs & 1) || (status & 1)))) {
			say("vayu-ipa: THE MODEM HAS IPA/GSI UP (loader=modem): SMP2P %s%s%s\n",
			    up ? "valid" : "not valid",
			    observe_regs ? (mcs & 1 ? ", MCS runs" : ", MCS idle") : "",
			    observe_regs ? (status & 1 ? ", GSI enabled" : ", GSI disabled") : "");
			ipa_fw_ready = true;
			return 1;
		}
		say("vayu-ipa: the modem did NOT bring IPA/GSI up (loader=modem, no AP PAS was done)\n");
		return 0;
	}
	return up ? 1 : 0;
}

void
vayu_ipa_prepare_stop(void)
{
	if (!ipa_enabled) return;
	ipa_pc(IPA_PS_STOP, "prepare stop", 0, IPA_PO_STEP, false);
	if (ipa_extra_bus != 0) {
		/* Never given back: SN11/SN0 carry other A2NOC/SNOC masters (QUP, SDC, crypto), and a zero
		 * apps vote could take away what UEFI voted for them. */
		say("ipa-xbus: stop: the vayu_ipa_extra_bus=%#x votes stay (%u TCS failure(s) in this run)\n",
		    ipa_extra_bus, ipa_xbus_failures);
	}
	if (!vayu_ipa_data_stop()) {
		/* A channel that will not stop leaves the IPA firmware up: never PAS-stop under it. */
		say("vayu-ipa: data path did not stop; keeping IPA firmware up\n");
		return;
	}
	if (ipa_uc_fw.started) {
		/* No PAS_SHUTDOWN(0x1B): this phone's PAS_SHUTDOWN of an image has hung it before (vayu_pas.c,
		 * pas_id 15 path); the uC is left for the next boot, as ipa_fws is without vayu_ipa_pas bit 0. */
		say("vayu-ipa: ipa_uc left running (no PAS_SHUTDOWN)\n");
		ipa_uc_fw.started = false;
	}
	if (ipa_fw.started) {
		(void)vayu_pas_stop(&ipa_fw, "IPA prepare stop");
	}
	ipa_fw_ready = ipa_smp2p_ready = false;
	ipa_prepared = false;
	ipa_watch_stop = true;
	ipa_pc_after();
}
