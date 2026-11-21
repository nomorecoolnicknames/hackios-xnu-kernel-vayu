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

static struct vayu_rproc ipa_fw = {
	.name = "ipa", .firmware = "ipa_fws", .pas_id = 15,
	.memory = 0x98f00000, .memory_size = 0x10000,
	.smem_host = IPA_SM_HOST_MODEM, .smp2p_theirs = 0, .smp2p_ours = 0,
	.crash_item = 0, .apcs_smp2p = 14,
	.rails = { NULL, NULL }, .rail_level = 384, .rails_shared = false,
};

static bool ipa_enabled, ipa_prepared, ipa_fw_ready, ipa_smp2p_ready;
static bool ipa_modem_loader;           /* vayu_ipa_loader=1: no AP PAS; watch what MPSS does */
static uint32_t ipa_filter_item, ipa_our_item, ipa_restricted;
static vm_offset_t ipa_regs;
/* vayu_ipa_bus: which bus BCMs get the IP0 word. vayu_ipa_post_auth_ms: after AUTH_AND_RESET(15)
 * returns, wait this long and announce every later IPA access with a 2.5 s pause. vayu_ipa_sram=1:
 * canaries in the IPA 4.1 SW SRAM window (wrapper + 0x40000 + 0x7000, ipahal_reg.c v3.0..v4.2)
 * instead of 0x1e50000 (the v4.5 offset). Ported from 0029 (w23 line) to this tree. */
static uint32_t ipa_bus_mask = 15, ipa_post_auth_ms, ipa_sram_fix;
/* vayu_ipa_step_ms: the pause after each "about to ..." (default 2500). vayu_ipa_hw_step_ms: the
 * same for the hardware-init/SRAM section, default 0 so CLKON -> SRAM stays milliseconds (run
 * 125713 died at the first SRAM write ~15 s after CLKON; 115037 wrote it ~2.5 s after and passed).
 * vayu_ipa_sram_write=0 skips the SRAM canaries entirely. vayu_ipa_bus_core=1 (default) also
 * votes the ACV rail, the IPA core NoC path that stock keeps voted (use_bw_vote, ipa.c:5140). */
static uint32_t ipa_step_ms = 2500, ipa_hw_step_ms, ipa_sram_write = 1, ipa_bus_core = 1;
/*
 * k26-ipahw: the IPA clock vote. Stock on vayu has qcom,bandwidth-vote-for-ipa: the clock is the
 * IPA_CORE->IPA_CORE path of the interconnect (sm8150.dtsi:3635-3640, 0/125 at SVS2) and is never
 * a GCC branch (ipa.c:5133-5156 makes ipa3_clk NULL). The RPMh BCM behind it is "IP0"
 * (0x500a4 on this phone). Three ways to write it, one boot arg:
 *   vayu_ipa_clk=1  (default) ask the cmd-db level table: a short table is an index table and the
 *                   command gets the position of the first level >= vayu_ipa_clk_level; a long
 *                   table is a value table and the command gets the DT's SVS2 value (125).
 *   vayu_ipa_clk=0  the old word 0x60003fff (commit|valid|16383) -- kept for an A/B run.
 *   vayu_ipa_clk_raw=WORD  that exact command word, no questions asked.
 * The old word is a valid command with a state of 16383; if IP0's table is short that state is
 * outside it, which is one of the ways a BCM vote ends up not turning the clock on at all.
 */
static uint32_t ipa_clk_mode = 1, ipa_clk_level = 1, ipa_clk_raw, ipa_clk_raw_set, ipa_cmd_db;
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

/* The IPA local (SW) SRAM window this boot uses: 0x1e47000 for IPA 4.1 (vayu_ipa_sram=1),
 * 0x1e50000 for the v4.5 offset the older code used. Table offsets are relative to this window
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
		how = "old raw 0x3fff";
	} else {
		count = vayu_rpmh_levels("IP0", &address, levels, sizeof(levels) / sizeof(levels[0]));
		if (count > 0 && count <= 32) {
			/* A short table: the BCM state is a position in it, the way every voltage rail of
			 * this kernel is written (vayu_pas.c rail_position). Take the first level at least
			 * vayu_ipa_clk_level: SVS2, the rate stock starts at (ipa.c:7134). */
			for (unsigned i = 0; i < (unsigned)count; i++) {
				if (levels[i] >= ipa_clk_level) {
					state = i;
					break;
				}
			}
			value = BCM_VOTE_XY(0, state);
			how = "table position";
		} else {
			/* A long table (or none): a value BCM, and the DT gives the SVS2 value of the
			 * IPA_CORE->IPA_CORE path as 125 (sm8150.dtsi, vectors-KBps case 1). */
			state = 125;
			value = BCM_VOTE_XY(0, state);
			how = count > 0 ? "long table: DT 125" : "no table: DT 125";
		}
		if (!ipa_ip0_said) {
			char line[240];
			int at = 0;

			ipa_ip0_said = true;
			if (count > 0) {
				at = snprintf(line, sizeof(line), "vayu-ipa: IP0 levels (%d):", count);
				for (unsigned i = 0; i < (unsigned)count && i < 16; i++) {
					at += snprintf(line + at, sizeof(line) - (size_t)at, " %u", levels[i]);
				}
				say("%s%s\n", line, count > 16 ? " ..." : "");
			} else {
				say("vayu-ipa: IP0 has no level table in the command database (%d)\n", count);
			}
			say("vayu-ipa: IP0 vote: mode %u, level %u -> state %u (%s)\n", ipa_clk_mode,
			    ipa_clk_level, state, how);
		}
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

/* IP0 (the IPA clock) plus the enabled rail BCMs. Stock on vayu has use_bw_vote: the IPA clock
 * lives in the bus/interconnect vote, not a GCC branch (ipa.c:5140-5152), and the IPA core path
 * (ACV here) is part of that vote at every level. Returns the number of successful sends. */
static unsigned
ipa_vote_rails(const char *why, bool verbose)
{
	static const char *const rails[] = { "SN8", "SH0", "MC0", "ACV" };
	static uint32_t rail_address[4];
	uint32_t address, value;
	unsigned sent = 0;

	if (!ipa_clock_vote(why, verbose)) {
		return 0;
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
		if (!vayu_rpmh_send_now(&address, &value, 1, "ipa bus")) {
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
	say("vayu-ipa: %u vote(s) sent (IP0 and the rails)\n", sent);
	return true;
}

#define IPA_RD(off) (*(volatile uint32_t *)(ipa_regs + (off)))
#define IPA_WR(off, value) do { IPA_RD(off) = (value); __asm__ volatile("dsb sy" ::: "memory"); } while (0)
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
	unsigned sent = ipa_vote_rails(what, false);
	uint32_t version;

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
	if (ms != 0) delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
ipa_step(const char *what)
{
	ipa_step_after(what, ipa_step_ms);
}

/* The hardware-init/SRAM section uses no pause of its own (ipa_hw_step_ms = 0): the clock vote
 * and the VERSION check before each group are the guard now, not a timer. */

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
	uint64_t waited = 0;

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
	{
		/* A poll of seconds must not become the gap the whole fix is about: re-assert the vote
		 * and the VERSION check every 400 ms, before the next read of the window. */
		uint64_t next_vote = 400;

		for (;;) {
			response = *(volatile uint32_t *)(uc + 0xc) & 0xff;
			params = *(volatile uint32_t *)(uc + 0x10);
			if (response == UC_RESPONSE_INIT_DONE || waited >= ms) break;
			delay_for_interval(10, NSEC_PER_MSEC);
			waited += 10;
			if (waited >= next_vote && !ipa_smp2p_ready) {
				uint32_t version;

				next_vote += 400;
				if (ipa_vote_rails("uC poll", false) == 0) return false;
				version = IPA_RD(0x34);
				if (version == 0 || version == 0xffffffffU) {
					say("vayu-ipa: %s: uC poll stopped, IPA_VERSION %#x at %llu ms\n",
					    when, version, waited);
					return false;
				}
			}
		}
	}
	state = *(volatile uint32_t *)(uc + 0x20) & 0xff;
	iface = *(volatile uint32_t *)(uc + 0x24) & 0xffff;
	say("vayu-ipa: %s: uC responseOp %#x params %#x hwState %#x interface %#x after %llu ms: %s\n",
	    when, response, params, state, iface, waited,
	    response == UC_RESPONSE_INIT_DONE ? "uC INIT_COMPLETED" : "uC NOT READY");
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

	if (ipa_post_auth_ms != 0) {
		say("vayu-ipa: post-auth: AUTH_AND_RESET(15) RETURNED; holding %u ms before any IPA access\n",
		    ipa_post_auth_ms);
		delay_for_interval(ipa_post_auth_ms, NSEC_PER_MSEC);
	}
	if (!ipa_hw_group("post-auth: read IPA_VERSION 0x1e40034")) return;
	say("vayu-ipa: post-auth: IPA_VERSION %#x\n", IPA_RD(0x34));
	gsi = ml_io_map_unmappable(GSI_BASE, 0x20000, VM_WIMG_IO);
	if (gsi == 0) {
		say("vayu-ipa: post-auth: GSI window did not map\n");
		return;
	}
	cfg = *(volatile uint32_t *)(gsi + 0x0);
	status = *(volatile uint32_t *)(gsi + 0x1f000);
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

	ipa_gcc_look("before the vote");
	if (!ipa_hw_group("init_hw: read IPA_VERSION (ipa_utils.c:4170)")) return false;

	/* BCR v4_0 = 0x39 (IPA_BCR_REG_VAL_v4_0, ipa_utils.c:71). */
	if (!ipa_hw_group("init_hw: BCR 0x1e401d0 = 0x39")) return false;
	IPA_WR(0x1d0, 0x39);

	/* CLKON: open global + 2x (ipa_utils.c:4194-4200). */
	if (!ipa_hw_group("init_hw: CLKON 0x1e40044 = 3")) return false;
	IPA_WR(0x44, 3);
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

	if (ipa_sram_write != 1) {
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
	for (uint32_t at = 0x288; at < 0x2400; at += 4) {
		*(volatile uint32_t *)(sram + at) = 0;
	}
	for (unsigned i = 0; i < sizeof(canaries) / sizeof(canaries[0]); i++) {
		uint32_t at = canaries[i];

		*(volatile uint32_t *)(sram + at - 8) = 0xdeadbeef;
		*(volatile uint32_t *)(sram + at - 4) = 0xdeadbeef;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	IPA_WR(0x1e8, ipa_restricted + 0x7d0); /* LOCAL_PKT_PROC_CNTXT_BASE */
	say("vayu-ipa: IPA SRAM zeroed/canaries written at %#x, proc-ctx base %#x\n", sram_pa,
	    IPA_RD(0x1e8));
	return true;
}

static bool
ipa_smp2p_verify(void)
{
	uint32_t counts, name_at, value_at;

	if (ipa_our_item == 0) {
		ipa_our_item = vayu_smem_item(IPA_SM_HOST_MODEM, IPA_SMP2P_OURS, &(uint32_t){ 0 });
	}
	if (ipa_our_item == 0) {
		say("vayu-ipa: modem SMP2P item %u is absent after MPSS start\n", IPA_SMP2P_OURS);
		return false;
	}
	counts = ipa_smem_word(ipa_our_item + 12);
	name_at = ipa_our_item + 20 + 20;
	value_at = ipa_our_item + 36 + 20;
	if ((counts >> 16) < 2 || ipa_smem_word(name_at) != 0x00617069 ||
	    (ipa_smem_word(value_at) & 3) != 3) {
		say("vayu-ipa: modem's SMP2P entry \"ipa\" is not valid+clock-on (counts %#x, name %#x, value %#x)\n",
		    counts, ipa_smem_word(name_at), ipa_smem_word(value_at));
		return false;
	}
	say("vayu-ipa: modem's SMP2P \"ipa\" entry verified after MPSS start: valid + clock-on\n");
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

void
vayu_ipa_prepare(void)
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
	PE_parse_boot_argn("vayu_ipa_clk_level", &ipa_clk_level, sizeof(ipa_clk_level));
	if (PE_parse_boot_argn("vayu_ipa_clk_raw", &ipa_clk_raw, sizeof(ipa_clk_raw))) ipa_clk_raw_set = 1;
	PE_parse_boot_argn("vayu_ipa_cmd_db", &ipa_cmd_db, sizeof(ipa_cmd_db));
	PE_parse_boot_argn("vayu_ipa_uc_wait_ms", &ipa_uc_wait_ms, sizeof(ipa_uc_wait_ms));
	PE_parse_boot_argn("vayu_ipa_gcc_survey", &ipa_gcc_survey, sizeof(ipa_gcc_survey));
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
	say("vayu-ipa: clock: mode %u level %u%s, gcc survey %u, cmd-db survey %u\n", ipa_clk_mode,
	    ipa_clk_level, ipa_clk_raw_set ? " (+ raw override)" : "", ipa_gcc_survey, ipa_cmd_db);
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
	if (!ipa_registers_check()) {
		say("vayu-ipa: IPA CFG is not alive before PAS; no firmware start\n");
		return;
	}
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
		return;
	}
	if (!(vayu_pas_steps() & VAYU_PAS_STEP_AUTH)) {
		say("vayu-ipa: dry stop after the image: AUTH_AND_RESET and hardware init not done; IPA stays down\n");
		ipa_prepared = true;
		return;
	}
	if (!vayu_pas_start(&ipa_fw, 10000)) {
		say("vayu-ipa: ipa_fws did not come up\n");
		vayu_pas_stop(&ipa_fw, "IPA did not come up");
		return;
	}
	ipa_post_auth_look();
	if (vayu_pas_steps() & VAYU_PAS_STEP_HW) {
		if (!ipa_hardware_init()) {
			say("vayu-ipa: hardware init did not finish (a group above did not see the clock); "
			    "SRAM/GSI are not touched, IPA stays half-started\n");
			return;
		}
		if (!ipa_sram_canaries()) {
			say("vayu-ipa: SRAM canaries were not written (a group above did not see the clock); "
			    "GSI is not touched, IPA stays half-started\n");
			return;
		}
	} else {
		say("vayu-ipa: hardware/SRAM init skipped (vayu_ipa_pas bit 3 off)\n");
	}
	/* vayu_ipa_data=1: the four GSI channels, endpoints and the empty tables, still before MPSS
	 * (stock ipa3_load_ipa_fw -> ipa3_post_init). The QMI handshake stays separate. */
	if (ipa_uc_reads != 1) {
		say("vayu-data: S0 IPA UC: SKIP uC reads disabled (vayu_ipa_uc_read=0)\n");
	} else if (vayu_ipa_uc_look("post-init", ipa_uc_wait_ms)) {
		say("vayu-data: S0 IPA UC: OK uC INIT_COMPLETED after the hardware init\n");
	} else {
		say("vayu-data: S0 IPA UC: ERROR uC NOT READY after the hardware init (stock sees the same "
		    "for ~1 s; the uC can still be alive, its event log arrives later)\n");
	}
	if (!vayu_ipa_data_prepare()) {
		say("vayu-ipa: data path preparation failed; IPA stays up, WDS Start will be blocked\n");
	}
	loader_checkpoint(112);
	say("vayu-ipa: IPA firmware authenticated/reset (no own SMP2P item); modem item will carry \"ipa\"\n");
	/*
	 * The modem's SMP2P item 428 does not exist until MPSS runs (a cold boot made the old
	 * verify-before-MPSS circular: no item -> not ready -> no "ipa" entry -> the modem never
	 * starts its IPA task). It is verified in vayu_ipa_modem_probe() after MPSS is up.
	 */
	ipa_fw_ready = true;
	ipa_prepared = true;
	say("vayu-ipa: firmware ready; the modem's SMP2P \"ipa\" entry is verified after MPSS starts, then QRTR INIT_MODEM_DRIVER\n");
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
	item = vayu_smem_item(IPA_SM_HOST_MODEM, IPA_SMP2P_OURS, &(uint32_t){ 0 });
	if (item != 0) {
		counts = ipa_smem_word(item + 12);
		value = ipa_smem_word(item + 36 + 20);
	}
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
	say("vayu-ipa: after MPSS: loader=%s, SMP2P item %u counts %#x \"ipa\" %#x; registers %s (IPA_VERSION %#x, GSI_STATUS %#x, MCS_CFG %#x, ERROR_LOG %#x)\n",
	    ipa_modem_loader ? "modem" : "AP", item, counts, value,
	    observe_regs ? "read" : "not read (vayu_ipa_observe_regs=1 to read)",
	    version, status, mcs, error);
	up = ipa_smp2p_verify();                /* logs what it sees; sets ipa_smp2p_ready */
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
	if (!vayu_ipa_data_stop()) {
		/* A channel that will not stop leaves the IPA firmware up: never PAS-stop under it. */
		say("vayu-ipa: data path did not stop; keeping IPA firmware up\n");
		return;
	}
	if (ipa_fw.started) {
		(void)vayu_pas_stop(&ipa_fw, "IPA prepare stop");
	}
	ipa_fw_ready = ipa_smp2p_ready = false;
	ipa_prepared = false;
}
