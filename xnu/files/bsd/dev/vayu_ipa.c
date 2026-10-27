/*
 * vayu IPA 4.1 firmware and modem prerequisites, before MPSS starts.
 *
 * vayu_ipa=1 opts in. This stage loads the stock ipa_fws through the same PAS machinery that
 * already loads the modem, enables its RPMh clock, allocates the SMEM filter-table item and
 * publishes the apps SMP2P "ipa" entry. Without that entry the modem's IPA task never answers
 * INIT_MODEM_DRIVER (upstream's decisive finding; vayu DT has the same entry name).
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
#include <sys/vayu_pas.h>

#define IPA_SM_HOST_MODEM       1
#define IPA_SMP2P_OURS          428
#define IPA_SMP2P_VALID         (1u << 0)
#define IPA_SMP2P_CLOCK_ON      (1u << 1)
#define IPA_FILTER_TABLE_ITEM   497
#define IPA_FILTER_TABLE_SIZE   0x2000   /* vayu default: no qcom,ipa-q6-smem-size in DT */

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

static bool
ipa_clock_on(void)
{
	static const char *const rails[] = { "SN8", "SH0", "MC0", "ACV" };
	uint32_t address, value;
	bool ok;

	address = vayu_rpmh_address("IP0");
	/* BCM_VOTE(on, vote), the encoding Linux's clk-rpmh uses for IPA's own clock. */
	value = (1u << 30) | (1u << 29) | 0x3fff;
	ok = address != 0 && vayu_rpmh_send(&address, &value, 1, "ipa clock");
	if (!ok) {
		say("vayu-ipa: IP0 clock vote failed; not starting the IPA firmware\n");
		return false;
	}
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		address = vayu_rpmh_address(rails[i]);
		if (address != 0 && !vayu_rpmh_send(&address, &value, 1, "ipa bus")) {
			say("vayu-ipa: bus %s vote failed; IP0 is confirmed, keeping this visible\n", rails[i]);
		}
	}
	return true;
}

#define IPA_RD(off) (*(volatile uint32_t *)(ipa_regs + (off)))
#define IPA_WR(off, value) do { IPA_RD(off) = (value); __asm__ volatile("dsb sy" ::: "memory"); } while (0)
#define RSRC(xmin, xmax, ymin, ymax) \
	((uint32_t)(xmin) | (uint32_t)(xmax) << 8 | (uint32_t)(ymin) << 16 | (uint32_t)(ymax) << 24)

static void
ipa_hardware_init(void)
{
	/* vayu-r-oss ipa3_init_hw + resource groups + route for IPA 4.1. */
	uint32_t v = IPA_RD(0x74);
	IPA_WR(0x74, (v & ~0xfU) | 8);                 /* DDR QMB max writes 8 */
	v = IPA_RD(0x78);
	IPA_WR(0x78, (v & ~0x00ff000fU) | 12 | 120U << 16); /* DDR reads 12 of 120 */
	IPA_WR(0x44, 3);                              /* CLKON: open global + 2x */
	IPA_WR(0x1fc, IPA_RD(0x1fc) & ~1U);           /* TX PA_MASK_EN off */
	IPA_WR(0x1d0, 0x39);                          /* BCR v4_0 */
	IPA_WR(0x1f0, 3);                             /* aggr granularity 0.5 ms (v4) */
	IPA_WR(0x3c, IPA_RD(0x3c) | 1U << 7 | 1U << 8); /* in-order GSI */
	IPA_WR(0x220, 256);                           /* dynamic idle after 256 */

	/* Source groups (valid only 0x400/0x420 and 0x404/0x424), v4.1:
	 * pkt contexts, descriptor lists, descriptor buffers, ACK entries. */
	IPA_WR(0x400, RSRC(1, 1, 0, 0));   /* GRP01 pkt contexts: UL_DL min/max 1 */
	IPA_WR(0x420, RSRC(1, 1, 0, 0));   /* GRP23 pkt contexts: LWA_DL min/max 1 */
	IPA_WR(0x404, RSRC(10, 10, 0, 0)); /* GRP01 descriptor lists: UL_DL 10 */
	IPA_WR(0x424, RSRC(10, 10, 0, 0)); /* GRP23 descriptor lists: LWA_DL 10 */
	IPA_WR(0x408, RSRC(12, 12, 0, 0)); /* GRP01 descriptor buffers: UL_DL 12 */
	IPA_WR(0x428, RSRC(14, 14, 0, 0)); /* GRP23 descriptor buffers: LWA_DL 14 */
	IPA_WR(0x40c, RSRC(14, 14, 0, 0)); /* GRP01 ACK entries: UL_DL 14 */
	IPA_WR(0x42c, RSRC(20, 20, 0, 0)); /* GRP23 ACK entries: LWA_DL 20 */

	/* Destination groups (valid only 0x500/0x520 and 0x504/0x524):
	 * data sectors and DPS DMARS, UL_DL and LWA_DL. */
	IPA_WR(0x500, RSRC(4, 4, 0, 0));   /* GRP01 data sectors: UL_DL 4 */
	IPA_WR(0x520, RSRC(4, 4, 0, 0));   /* GRP23 data sectors: LWA_DL 4 */
	IPA_WR(0x504, RSRC(3, 3, 0, 0));   /* GRP01 data sectors: UL_DL 3 */
	IPA_WR(0x524, RSRC(3, 3, 0, 0));   /* GRP23 data sectors: LWA_DL 3 */
	IPA_WR(0x508, RSRC(2, 2, 0, 0));   /* GRP01 DPS DMARS: UL_DL 2 */
	IPA_WR(0x528, RSRC(2, 2, 0, 0));   /* GRP23 DPS DMARS: LWA_DL 2 */
	IPA_WR(0x50c, RSRC(1, 1, 0, 0));   /* GRP01 DPS DMARS: UL_DL 1 */
	IPA_WR(0x52c, RSRC(1, 1, 0, 0));   /* GRP23 DPS DMARS: LWA_DL 1 */

	/* IPA local memory: zero the modem's table/proc-ctx areas, write canaries,
	 * and tell the uC where modem processing contexts start. */
	{
		vm_offset_t sram = ml_io_map_unmappable(0x1e50000, 0x4000, VM_WIMG_IO);
		static const uint16_t canaries[] = {
			0x0288, 0x0308, 0x0388, 0x0408, 0x0488, 0x0508, 0x0588, 0x0608,
			0x0688, 0x07d0, 0x0c30, 0x13d0, 0x13f0, 0x23fc,
		};
		if (sram == 0) {
			say("vayu-ipa: IPA SRAM window did not map\n");
			return;
		}
		for (uint32_t at = 0x288; at < 0x2400; at += 4) {
			*(volatile uint32_t *)(sram + at) = 0;
		}
		for (unsigned i = 0; i < sizeof(canaries) / sizeof(canaries[0]); i++) {
			uint32_t at = canaries[i];
			*(volatile uint32_t *)(sram + at - 8) = 0xdeadbeef;
			*(volatile uint32_t *)(sram + at - 4) = 0xdeadbeef;
		}
		IPA_WR(0x1e8, ipa_restricted + 0x7d0); /* LOCAL_PKT_PROC_CNTXT_BASE */
		say("vayu-ipa: IPA SRAM zeroed/canaries written, proc-ctx base %#x\n", IPA_RD(0x1e8));
	}
	/* Exception route: LAN consumer endpoint 10, header table local, retain hdr. */
	IPA_WR(0x48, 0x1120052);
	say("vayu-ipa: hardware init: QSB %#x %#x, CLKON %#x, BCR %#x, COMP %#x, route %#x\n",
	    IPA_RD(0x74), IPA_RD(0x78), IPA_RD(0x44), IPA_RD(0x1d0), IPA_RD(0x3c), IPA_RD(0x48));
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
	if (PE_parse_boot_argn("vayu_ipa_pas", &steps, sizeof(steps))) {
		vayu_pas_set_steps(steps);
	}
	say("vayu-ipa: PAS steps %#x (bit 0 forced shutdown, 1 image, 2 AUTH, 3 hardware init)\n",
	    vayu_pas_steps());
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
	/* SM8150's IPA core clock is the RPMh IP0 resource (Linux: RPMH_IPA_CLK). The GCC branch writes at
	 * +0x89014.. hung the phone on hardware, so they are opt-in: vayu_ipa_gcc=1. */
	if (!ipa_clock_on()) return;
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
	if (vayu_pas_steps() & VAYU_PAS_STEP_HW) {
		ipa_hardware_init();
	} else {
		say("vayu-ipa: hardware/SRAM init skipped (vayu_ipa_pas bit 3 off)\n");
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
	if (ipa_fw.started) {
		(void)vayu_pas_stop(&ipa_fw, "IPA prepare stop");
	}
	ipa_fw_ready = ipa_smp2p_ready = false;
	ipa_prepared = false;
}
