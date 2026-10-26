/*
 * The GPU of the POCO X3 Pro (vayu), an Adreno 640 in a Snapdragon 860 (SM8150-AC), woken as
 * far as this port has been worked out. Nothing here is touched unless two boot-args ask for
 * it: gpu=1 and vayu_gpu=1. Redwood's driver (hakios.adreno.c) and QEMU are unchanged.
 *
 * Why this file is not hakios.adreno.c with other numbers: the two GPUs are both a6xx, and
 * the way the block is woken is the same, but the SoC's addresses and clock controller are
 * not, and the Adreno 640's GMU differs from the 642L's in five places that matter:
 *
 *   - the GMU's data memory is at 0x00040000, not 0x10004000 (a650 and later);
 *   - it needs 240 KB of instruction and 240 KB of data cache in its own address space
 *     (0x4000 and 0x44000), which the firmware file fills;
 *   - its RSCC block is inside the GMU's registers at +0x23000 (0x2c8d000), not a separate
 *     0x3de0000; its sleep/wake program and the PDC program are the a640 ones;
 *   - its HFI version is the older one: on a cold boot the firmware reports 0xbabeface,
 *     not the 0x100 of newer firmware;
 *   - its AHB fence range and chip ID are its own.
 *
 * Steps 1 to 16 are redwood's first half: outside clocks, fuses, the GPU clock controller, CX
 * power, clocks measured with the SoC's counter, the first GMU words, the GMU's RPMh channel, the
 * SMMU, the GMU's memory, TrustZone (asked but not needed), the GMU's firmware, its start, and its
 * message queues. Steps 17 to 22 are the 3D half, the port of redwood's steps 17 to 21:
 *
 *  17  looks   no write: the 3D core's power state from CX registers (the core itself is read
 *              only if GX is found on), the GMU's speed and bus tables made in memory of ours,
 *              fw/a630_sqe.fw checked, and the signed zap files named if they are missing
 *  18  powers  the 3D core through the GMU: speed and bus tables, "core firmware start",
 *              "start", the speed wanted, the out-of-band request; then reads the core
 *  19  writes  the core's SMMU context 0 (streams 0/1, mask 0x401) with its ring, its SQE
 *              microcode (fw/a630_sqe.fw) and a page to write to
 *  20  writes  the core's registers as Linux's hw_init does for an a640, and where its microcode
 *              and ring are; the command processor stays halted
 *  21  starts  the command processor and gives it CP_ME_INIT (Linux and the vendor driver do this
 *              before the zap shader: no signed file is needed up to here)
 *  22  needs   the signed zap shader: a640_zap.mdt and its a640_zap.bNN segments, loaded through
 *              TrustZone into 0x98f15000; then CP_SET_SECURE_MODE 0 and a word written on command.
 *              Without the files it stops first and names them. (vayu_nozap=1: the HYPOTHESIS
 *              test of Linux's no-zap path, see step_core_unsecure.)
 *
 * Every run ends by stopping what it started, in reverse. With `vayu_adreno=22
 * vayu_keep=1` and a step 22 that passed, the block is instead handed to the
 * service HakiosGPU (iokit/Kernel/hakios.gpu.cpp, unchanged) through the
 * vayu_gpu_*() calls at the end of this file.
 *
 *   gpu=1 vayu_gpu=1                 steps 1-22 minus 11 and 12, no pauses, then the GPU kept for
 *                                    HakiosGPU (vayu_keep=0: stopped again); needs a640_gmu.bin,
 *                                    a630_sqe.fw and a640_zap.mdt/.bNN in fw/, else nothing is started
 *   gpu=1 vayu_gpu=1 vayu_adreno=N   staged: steps 1..N exactly, pauses (vayu_pause, default 1000) and
 *                                    nothing kept unless vayu_keep=1; for N > 16 steps 11 and 12 are
 *                                    left out (vayu_skip=1 keeps them)
 *   vayu_skip=MASK                   leave out the steps whose bits are set (bit 2 = step 2)
 *   vayu_pause=MS                    how long the newest step waits after announcing (1000)
 *   vayu_fine=1                      wait before every single access of the newest step
 *   vayu_watch=S                     after the steps, say once a second for S seconds (5)
 *   vayu_speed=N                     step 18's speed: 1 257, 2 345, 3 427, 4 499.2, 5 585, 6 675 MHz
 *                                    (6 on speed bin 1 only); default 4, staged runs 1
 *   vayu_gpu_fence=1                 step 15 AHB fence range as GMU_FENCE_RANGE_MASK, 0x80a008a0 (adreno_a6xx_gmu.c:1001)
 *   vayu_gpu_ecp=1                   step 18's ECP feature control before "core firmware start" (kgsl_hfi.c:740)
 *   vayu_gpu_bw=N                    step 18's speed vote bus index N, 0..0xff; 0xff = INVALID_DCVS_IDX (kgsl_gmu.c:561)
 *   vayu_nozap=1                     step 22 without zap files: write RBBM_SECVID_TRUST_CNTL = 0
 *
 * A step that does not find what it needs ends the sequence; the block is then stopped again
 * (GMU held in reset, its SMMU context off, CX collapsed) and the system goes on starting.
 *
 * Addresses are from the phone's own device tree (uefi/files/Resources/DTBs/vayu.dtb):
 * qcom,kgsl-3d0@2C00000, qcom,gmu@0x2C6A000, qcom,kgsl-iommu@0x02CA0000,
 * kgsl-smmu@0x02ca0000, qcom,gpucc (clock-controller@2c90000), qcom,gdsc@0x2c9106c,
 * qcom,gdsc@0x2c9100c, qfprom@780000. Register layouts from Linux: gpucc-sm8150.c,
 * gcc-sm8150.c, a6xx_gmu.c, a6xx_gpu.c, a6xx_catalog.c, arm-smmu.c and the vendor
 * debugcc-sm8150.c for the frequency counter. The firmware names are Linux's a640 entry:
 * a640_gmu.bin, a630_sqe.fw (the 640 uses the 630 SQE microcode) and a640_zap.mdt/.bNN.
 */
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_log_putc(char c);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static uint32_t last_step, skip_steps, pause_ms = 1000, fine, watch_seconds = 5;
static uint32_t step;
static uint32_t newest_step;
static char last_write[96] = "nothing written";
static bool quiet;
static bool cx_on_by_us;                /* CX was off and this kernel switched it on */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return ml_get_interrupts_enabled() ? printf("%s", line) : (int)strlen(line);   /* (not with interrupts off) */
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- register access ------------------------------------------------------------------------------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static uint32_t __attribute__((noinline))
rd8(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldrb %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

static void
hold(bool single_access)
{
	if (step == newest_step && pause_ms && (!single_access || fine)) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("vayu: step %u: %s\n", step, what);
	hold(false);
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	if (quiet) {
		return rd(b->base + offset);
	}
	if (step == newest_step) {
		printf("vayu: %u: about to read %s %s at %#x\n", step, b->name, name, b->phys + offset);
		hold(true);
	}
	value = rd(b->base + offset);
	printf("vayu: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	if (!quiet) {
		printf("vayu: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
		snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name,
		    b->phys + offset);
		hold(true);
	}
	wr(b->base + offset, value);
}

static uint32_t
set(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	put(b, name, offset, was | bits);
	return get(b, name, offset);
}

static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/* ---- the phone's addresses ------------------------------------------------------------------------- */

/*
 * dt: qcom,kgsl-3d0@2C00000: "kgsl_3d0_reg_memory" 0x2c00000, 0x40000; "cx_dbgc" 0x2c61000, 0x800;
 * qfprom 0x780000, 0x6fff; interrupt kgsl_3d0_irq = GIC SPI 300 (not used: this port polls).
 * dt: qcom,gmu@0x2C6A000: "kgsl_gmu_reg" 0x2c6a000, 0x30000; "kgsl_gmu_pdc_cfg" 0xb290000,
 * 0x10000; "kgsl_gmu_pdc_seq" 0xb490000, 0x10000; interrupts hfi = SPI 304, gmu = SPI 305 (not
 * used). dt: qcom,kgsl-iommu@0x02CA0000 / kgsl-smmu@0x02ca0000: 0x2ca0000, 0x10000; tcu 0x2cc2000.
 * dt: qcom,gpucc: 0x2c90000, 0x9000. dt: qcom,gdsc@0x2c9106c (gpu_cx_gdsc) and
 * qcom,gdsc@0x2c9100c (gpu_gx_gdsc), with domain-addr 0x2c91508 and sw-reset 0x2c91008;
 * gpu_cx_gdsc's hw-ctrl-addr is 0x2c91540.
 */
#define VAYU_GPU_PHYS           0x02c00000u
#define VAYU_GPU_SIZE           0x00040000u
#define VAYU_GMU_PHYS           0x02c6a000u
#define VAYU_GMU_SIZE           0x00030000u
#define VAYU_RSCC_PHYS          0x02c8d000u     /* GMU + 0x23000 (a640: RSCC inside the GMU block) */
#define VAYU_GPUCC_PHYS         0x02c90000u
#define VAYU_GPUCC_SIZE         0x00009000u
#define VAYU_SMMU_PHYS          0x02ca0000u
#define VAYU_SMMU_SIZE          0x00010000u
#define VAYU_PDC_PHYS           0x0b290000u
#define VAYU_PDC_SEQ_PHYS       0x0b490000u
#define VAYU_QFPROM_PHYS        0x00780000u

/* ---- step 1: the clocks from outside, and RPMh's names ------------------------------------------------ */

/*
 * dt: clock-controller@100000 (qcom,gcc-sm8150). The GPU's clocks there have the same offsets as
 * on redwood (gcc-sm8150.c): gcc_gpu_cfg_ahb 0x71004, gcc_gpu_memnoc_gfx 0x7100c,
 * gcc_gpu_snoc_dvm_gfx 0x71018, gcc_ddrss_gpu_axi 0x71154. The two taps of the 600 MHz PLL are
 * voted at 0x52004 (bit 15: gcc_gpu_gpll0_clk_src, bit 16: gcc_gpu_gpll0_div_clk_src), NOT at
 * redwood's 0x52000.
 */
static struct block gcc = { "gcc", 0x00100000, 0x80000 };
#define GCC_PLL_VOTE            0x52004
#define GCC_PLL_VOTE_GPU_DIV    (1u << 16)
#define GCC_GPU_CFG_AHB_CBCR    0x71004
#define GCC_GPU_MEMNOC_GFX_CBCR 0x7100c
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x71018
#define GCC_DDRSS_GPU_AXI_CBCR  0x71154
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)

/*
 * dt: qcom,cmd-db@c3f000c (reg 0xc3f000c, 0x08) -- the vendor SM8150 places the RPMh command
 * database in the AOP's message RAM there, not redwood's 0x80860000. Mainline sm8150.dtsi has
 * it at 0x85f20000 instead. Both are tried, read-only; vayu_cmd_db=ADDRESS overrides. The
 * header's magic is 0xdb 0x30 0x03 0x0c; on the vendor node the magic sits 12 bytes into the
 * page the node points at, so that offset is tried too.
 */
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_MAGIC0           0xdb
#define CMD_DB_MAGIC1           0x30
#define CMD_DB_MAGIC2           0x03
#define CMD_DB_MAGIC3           0x0c

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static bool
cmd_db_magic(const struct cmd_db_header *db)
{
	return db->magic[0] == CMD_DB_MAGIC0 && db->magic[1] == CMD_DB_MAGIC1 && db->magic[2] == CMD_DB_MAGIC2 &&
	       db->magic[3] == CMD_DB_MAGIC3;
}

static void
cmd_db_find(void)
{
	uint32_t skip_db = 0;

	if (PE_parse_boot_argn("vayu_no_cmd_db", &skip_db, sizeof(skip_db)) && skip_db) {
		printf("vayu: RPMh command database skipped by vayu_no_cmd_db=1\n");
		return;
	}
	static const struct {
		uint32_t base, offset;
	} places[] = {
		{ 0x0c3f0000, 0x0c },   /* dt: qcom,cmd-db@c3f000c (vendor SM8150): header at +0xc */
		{ 0x85f20000, 0x00 },   /* mainline sm8150.dtsi */
	};
	uint32_t override = 0;

	if (cmd_db != NULL) {
		return;
	}
	if (PE_parse_boot_argn("vayu_cmd_db", &override, sizeof(override)) && override != 0) {
		uint32_t offset = override & 0xfff;
		const uint8_t *page = (const uint8_t *)ml_io_map_wcomb(override - offset, CMD_DB_SIZE);

		cmd_db = (const struct cmd_db_header *)(page + offset);
		printf("vayu: cmd-db at %#x (vayu_cmd_db), magic %s\n", override,
		    cmd_db_magic(cmd_db) ? "matches" : "DOES NOT MATCH");
		return;
	}
	for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
		const struct cmd_db_header *db =
		    (const struct cmd_db_header *)ml_io_map_wcomb(places[i].base, CMD_DB_SIZE);
		const struct cmd_db_header *at = (const struct cmd_db_header *)((const uint8_t *)db + places[i].offset);

		if (cmd_db_magic(at)) {
			cmd_db = at;
			printf("vayu: cmd-db at %#x+%#x: magic matches\n", places[i].base, places[i].offset);
			return;
		}
	}
	printf("vayu: no RPMh command database found at any known place; rail names will be unknown\n");
}

static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	const struct cmd_db_header *db = cmd_db;
	char id[8] = { 0 };

	if (level_count != NULL) {
		*level_count = 0;
	}
	if (db == NULL) {
		return 0;
	}
	memcpy(id, name, strnlen(name, sizeof(id)));   /* (a cmd-db id fills all 8 bytes with no NUL: "lnbclka2"; strlcpy kept 7) */
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = db->data + slave->data_offset + entry[i].offset;
			unsigned count = entry[i].length / 2u;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				count = 0;
			}
			for (unsigned l = 0; levels != NULL && l < count && l < 16; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*level_count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rpmh_resource(const char *name)
{
	uint16_t level[16];
	unsigned count = 0;
	uint32_t address = rpmh_resource(name, level, &count);
	char levels[160];
	int at = 0;

	if (address == 0) {
		printf("vayu: %u: RPMh has no \"%s\"\n", step, name);
		return;
	}
	levels[0] = 0;
	for (unsigned l = 0; l < count && at < (int)sizeof(levels) - 8; l++) {
		at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level[l]);
	}
	printf("vayu: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, address, levels);
}

static bool
step_outside(void)
{
	announce("read the GPU's clocks in the global clock controller (SM8150 offsets) and the RPMh rails");
	get(&gcc, "gpu pll votes", GCC_PLL_VOTE);
	get(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	get(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR);
	get(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR);
	get(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR);

	announce("look up the GPU's supply rails in the boot loader's RPMh command database");
	cmd_db_find();
	show_rpmh_resource("gfx.lvl");          /* the 3D core's rail */
	show_rpmh_resource("cx.lvl");           /* the CX half */
	show_rpmh_resource("mx.lvl");           /* memories */
	return true;
}

/* ---- step 2: the fuses ------------------------------------------------------------------------------- */

/*
 * dt: qcom,kgsl-3d0's qcom,gpu-speed-bin = <0x4130 0xe0000000 0x1d>: a 32-bit cell at
 * qfprom 0x780000 + 0x4130 (0x784130, the corrected copy), bits 31:29. 0 is the 585 MHz
 * part, 1 the 675 MHz part (dt: qcom,gpu-pwrlevels-0/-1). Redwood reads a byte at 0x7841e9;
 * that is not this phone.
 */
static struct block qfprom = { "qfprom", VAYU_QFPROM_PHYS, 0x7000 };
#define QFPROM_GPU_SPEED_BIN    0x4130
#define QFPROM_GPU_SPEED_MASK   0xe0000000u
#define QFPROM_GPU_SPEED_SHIFT  29

static uint32_t speed_bin;              /* step 18 offers 675 MHz only to bin 1 */

static bool
step_fuses(void)
{
	uint32_t cell, bin;

	announce("read the GPU's speed bin fuse at qfprom 0x784130 (bits 31:29)");
	cell = get(&qfprom, "gpu_speed_bin", QFPROM_GPU_SPEED_BIN);
	bin = (cell & QFPROM_GPU_SPEED_MASK) >> QFPROM_GPU_SPEED_SHIFT;
	speed_bin = bin;
	printf("vayu: %u: speed bin %u: an Adreno 640 rated to %u MHz\n", step, bin, bin == 1 ? 675 : 585);
	return true;
}

/* ---- step 3: the GPU's clock controller ----------------------------------------------------------------- */

/*
 * dt: qcom,gpucc at 0x2c90000, 0x9000. Offsets from gpucc-sm8150.c. There is one PLL (PLL1 at
 * 0x100), not redwood's PLL0/PLL1 pair; there are no CB, HUB or HLOS1 SMMU-vote clocks (redwood's
 * 0x1170, 0x1178, 0x117c, 0x1204 and 0x5000), so none of those is read or written. The 3D core's
 * clamp is at 0x1508, not redwood's 0x158c; the CX switch's hardware status is 0x1540.
 */
static struct block gpucc = { "gpucc", VAYU_GPUCC_PHYS, VAYU_GPUCC_SIZE };
#define GPUCC_PLL1_MODE         0x0100
#define GPUCC_CXO_AON_CBCR      0x1004
#define GPUCC_GX_SW_RESET       0x1008
#define GPUCC_GX_GDSCR          0x100c
#define GPUCC_GX_GMU_CBCR       0x1064
#define GPUCC_CX_GDSCR          0x106c
#define GPUCC_AHB_CBCR          0x1078
#define GPUCC_CX_GMU_CBCR       0x1098
#define GPUCC_CXO_CBCR          0x109c
#define GPUCC_GMU_RCGR          0x1120
#define GPUCC_GX_DOMAIN_MISC    0x1508
#define GPUCC_CX_HW_CTRL        0x1540
#define GPUCC_DEBUG_DIV         0x10fc
#define GPUCC_DEBUG_CBCR        0x1100
#define GPUCC_DEBUG_MUX         0x1568

#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_HW_CONTROL         (1u << 1)
#define GDSC_POWER_ON           (1u << 31)

static bool
step_gpucc(void)
{
	announce("make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)");
	if ((set(&gcc, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR, CBCR_ENABLE) & CBCR_OFF) &&
	    !wait_for(&gcc, GCC_GPU_CFG_AHB_CBCR, CBCR_OFF, 0, 10)) {
		printf("vayu: %u: it stays off (%#x): the GPU's clock controller would not answer\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}

	announce("read the GPU's clock controller (0x2c90000) as it is: the first access to the GPU's part of the SoC");
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
	get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 multiplier", GPUCC_PLL1_MODE + 4);
	get(&gpucc, "gmu_clk_src command", GPUCC_GMU_RCGR);
	get(&gpucc, "gmu_clk_src source", GPUCC_GMU_RCGR + 4);
	get(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
	get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
	get(&gpucc, "ahb", GPUCC_AHB_CBCR);
	get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	get(&gpucc, "cxo", GPUCC_CXO_CBCR);
	return true;
}

/* ---- step 4: CX power --------------------------------------------------------------------------------- */

static bool
gpucc_answers(void)
{
	need(&gcc);
	need(&gpucc);
	if (rd(gcc.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) {
		printf("vayu: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x): step 3\n", step,
		    rd(gcc.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}
	return true;
}

static bool
cx_is_on(void)
{
	return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_HW_CTRL)) & GDSC_POWER_ON;
}

static void stale_stop(void);
static void gx_off(void);

static bool
step_cx_power(void)
{
	uint32_t gdscr;

	if (!gpucc_answers()) {
		return false;
	}
	announce("switch the CX half of the GPU block on (gpu_cx_gdsc at 0x2c9106c): clear its collapse bit");
	gdscr = get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (cx_is_on()) {
		printf("vayu: %u: CX is on already: an earlier kernel left the GPU block running; stopping it first\n", step);
		stale_stop();
		if (cx_is_on()) {
			printf("vayu: %u: CX stays on: going on with it as it is\n", step);
			return true;
		}
		gdscr = rd(gpucc.base + GPUCC_CX_GDSCR);
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("vayu: %u: the switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	put(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx_gdscr", GPUCC_CX_GDSCR);
	get(&gpucc, "cx_hw_ctrl", GPUCC_CX_HW_CTRL);
	if (!cx_is_on()) {
		printf("vayu: %u: CX does not come on\n", step);
		return false;
	}
	cx_on_by_us = true;
	printf("vayu: %u: CX is on\n", step);
	if (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) {
		printf("vayu: %u: THE 3D CORE HAS POWER ALREADY, left on by an earlier kernel: not usable until the phone is "
		    "restarted\n", step);
		return false;
	}
	return true;
}

/* ---- step 5: clocks, and whether they run ------------------------------------------------------------- */

#define RCG_UPDATE              (1u << 0)
#define RCG_SOURCE_MASK         0x371fu
#define RCG_GPLL0_DIV(twice)    (6u << 8 | ((twice) - 1))       /* source 6: GPLL0/2 (300 MHz) */

static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool must)
{
	if (!(set(b, name, cbcr, CBCR_ENABLE) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
		return true;
	}
	printf("vayu: %u: %s %s stays off (%#x)%s\n", step, b->name, name, rd(b->base + cbcr),
	    must ? "" : "; its status bit is not one to rely on, going on");
	return !must;
}

static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
	uint32_t was = get(&gpucc, name, rcgr + 4);

	if ((was & RCG_SOURCE_MASK) == source) {
		return true;
	}
	put(&gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
	put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
	if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 10)) {
		printf("vayu: %u: %s does not take its new source (command %#x)\n", step, name, rd(gpucc.base + rcgr));
		return false;
	}
	get(&gpucc, name, rcgr + 4);
	return true;
}

/*
 * The SoC's frequency counter, from the vendor's debugcc-sm8150.c / clk-debug.c (Linux has no
 * mainline counterpart). SM8150 puts the counter's crystal/4 time base at gcc 0x43008, not at
 * redwood's 0x6200c; the GPU clock controller's clocks are selected with SM8150's numbers.
 * Both debug outputs divide by 2 on the way to the counter, hence the factor of 4.
 */
#define GCC_DEBUG_MUX           0x62000
#define GCC_DEBUG_MUX_GPUCC     0x162           /* SM8150's select for the GPU clock controller's debug output */
#define GCC_DEBUG_DIV           0x62004
#define GCC_DEBUG_CBCR          0x62008
#define GCC_XO_DIV4_CBCR        0x43008         /* SM8150, NOT redwood's 0x6200c */
#define GCC_MEASURE_CTL         0x62038
#define GCC_MEASURE_STATUS      0x6203c
#define MEASURE_RUN             (1u << 20)
#define MEASURE_DONE            (1u << 25)
#define MEASURE_COUNT           0x1ffffffu
#define MEASURE_BRIEF           0x1000          /* ~1 ms of the 4.8 MHz time base */
#define MEASURE_FULL            0x10000         /* ~14 ms */
#define MEASURE_FAILED          0xffffffffu

#define GPUCC_DEBUG_CXO_AON     0x0a
#define GPUCC_DEBUG_GX_GMU      0x0f
#define GPUCC_DEBUG_AHB         0x10
#define GPUCC_DEBUG_CX_GMU      0x18
#define GPUCC_DEBUG_CXO         0x19
#define GPUCC_DEBUG_GX_GFX3D    0x0b

static uint32_t
count_clock(uint32_t ticks)
{
	uint32_t status = 0;

	put(&gcc, "measure control", GCC_MEASURE_CTL, ticks);
	for (int i = 0; i < 1000 && (rd(gcc.base + GCC_MEASURE_STATUS) & MEASURE_DONE); i++) {
		sleep_us(10);
	}
	put(&gcc, "measure control", GCC_MEASURE_CTL, MEASURE_RUN | ticks);
	for (int i = 0; i < 2000 && !(status & MEASURE_DONE); i++) {
		sleep_us(10);
		status = rd(gcc.base + GCC_MEASURE_STATUS);
	}
	put(&gcc, "measure control", GCC_MEASURE_CTL, ticks);
	return status & MEASURE_DONE ? status & MEASURE_COUNT : MEASURE_FAILED;
}

static uint32_t
measure(uint32_t select)
{
	uint32_t gpu_mux = get(&gpucc, "debug mux", GPUCC_DEBUG_MUX), gpu_div = get(&gpucc, "debug divider", GPUCC_DEBUG_DIV);
	uint32_t gpu_cbcr = get(&gpucc, "debug output", GPUCC_DEBUG_CBCR), mux = get(&gcc, "debug mux", GCC_DEBUG_MUX);
	uint32_t div = get(&gcc, "debug divider", GCC_DEBUG_DIV), cbcr = get(&gcc, "debug input", GCC_DEBUG_CBCR);
	uint32_t base = get(&gcc, "crystal/4", GCC_XO_DIV4_CBCR), control = get(&gcc, "measure control", GCC_MEASURE_CTL);
	uint32_t brief, full;

	put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, (gpu_mux & ~0xffu) | select);
	put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, (gpu_div & ~0xfu) | 1);
	put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr | CBCR_ENABLE);
	put(&gcc, "debug mux", GCC_DEBUG_MUX, (mux & ~0x3ffu) | GCC_DEBUG_MUX_GPUCC);
	put(&gcc, "debug divider", GCC_DEBUG_DIV, (div & ~0xfu) | 1);
	put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr | CBCR_ENABLE);
	put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base | CBCR_ENABLE);
	/* a counter whose input stands still keeps its last count: the two counts differ only if the clock runs */
	brief = count_clock(MEASURE_BRIEF);
	full = brief == MEASURE_FAILED ? MEASURE_FAILED : count_clock(MEASURE_FULL);
	put(&gcc, "measure control", GCC_MEASURE_CTL, control);
	put(&gcc, "crystal/4", GCC_XO_DIV4_CBCR, base);
	put(&gcc, "debug input", GCC_DEBUG_CBCR, cbcr);
	put(&gcc, "debug divider", GCC_DEBUG_DIV, div);
	put(&gcc, "debug mux", GCC_DEBUG_MUX, mux);
	put(&gpucc, "debug output", GPUCC_DEBUG_CBCR, gpu_cbcr);
	put(&gpucc, "debug divider", GPUCC_DEBUG_DIV, gpu_div);
	put(&gpucc, "debug mux", GPUCC_DEBUG_MUX, gpu_mux);

	if (full == MEASURE_FAILED) {
		return MEASURE_FAILED;
	}
	if (full == brief) {
		return 0;
	}
	return (uint32_t)(((uint64_t)(full * 10 + 15) * 4800000 / ((uint64_t)MEASURE_FULL * 10 + 35)) * 4);
}

static uint32_t
measure_quietly(uint32_t select)
{
	uint32_t hz;

	quiet = true;
	hz = measure(select);
	quiet = false;
	return hz;
}

static bool
measure_clocks(void)
{
	static const struct {
		const char *name;
		uint32_t select;
	} clocks[] = {
		{ "cx_gmu (200 MHz was asked for)", GPUCC_DEBUG_CX_GMU }, { "cxo", GPUCC_DEBUG_CXO },
		{ "cxo_aon", GPUCC_DEBUG_CXO_AON }, { "gx_gmu (should be off)", GPUCC_DEBUG_GX_GMU },
		{ "ahb, THE ONE IN QUESTION,", GPUCC_DEBUG_AHB },
	};
	uint32_t hz, ahb_hz = MEASURE_FAILED;

	announce("measure the GPU block's clocks with the SoC's frequency counter (SM8150: gcc 0x62000.., 0x43008; "
	    "gpucc 0x10fc, 0x1100, 0x1568)");
	hz = measure(GPUCC_DEBUG_CXO);
	if (hz == MEASURE_FAILED) {
		printf("vayu: %u: the counter did not finish: it cannot be used as it is set up here\n", step);
		return false;
	}
	printf("vayu: %u: measured: cxo runs at %u Hz (the crystal: 19200000)\n", step, hz);
	if (hz < 18600000 || hz > 19800000) {
		printf("vayu: %u: the counter does not show the crystal's rate: nothing it says can be used\n", step);
		return false;
	}
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		hz = measure_quietly(clocks[i].select);
		if (hz == MEASURE_FAILED) {
			printf("vayu: %u: measured: %s: the counter did not finish\n", step, clocks[i].name);
		} else {
			printf("vayu: %u: measured: %s runs at %u Hz\n", step, clocks[i].name, hz);
		}
		if (clocks[i].select == GPUCC_DEBUG_AHB) {
			ahb_hz = hz;
		}
	}
	if (ahb_hz != MEASURE_FAILED && ahb_hz >= 1000000) {
		printf("vayu: %u: THE REGISTER BUS OF THE GPU BLOCK HAS ITS CLOCK, whatever its control register says\n", step);
		return true;
	}
	printf("vayu: %u: the register bus of the GPU block has no clock; that is where this stops\n", step);
	return false;
}

static bool
step_clocks(void)
{
	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on()) {
		printf("vayu: %u: CX is off: step 4\n", step);
		return false;
	}
	announce("in the global clock controller: ask for the 300 MHz tap for the GPU; the GPU's bus and memory clocks on");
	set(&gcc, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
	if (!clock_on(&gcc, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR, false) ||
	    !clock_on(&gcc, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, true) ||
	    !clock_on(&gcc, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR, false)) {
		return false;
	}

	announce("in the GPU's clock controller: the GMU's clock to 200 MHz (GPLL0/2 / 1.5)");
	if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV(3))) {
		return false;
	}

	announce("in the GPU's clock controller: the clocks of the GMU on (ahb, cxo, cx_gmu)");
	if (!clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true) ||
	    !clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false) ||
	    !clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true)) {
		return false;
	}
	return measure_clocks();
}

static bool
cx_ready(void)
{
	uint32_t gmu_clock, ahb_hz;

	if (!gpucc_answers()) {
		return false;
	}
	gmu_clock = rd(gpucc.base + GPUCC_CX_GMU_CBCR);
	if (!cx_is_on() || (gmu_clock & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("vayu: %u: needs CX on (it is %s) and the GMU's clock running (cx_gmu %#x): steps 4 and 5\n", step,
		    cx_is_on() ? "on" : "off", gmu_clock);
		return false;
	}
	ahb_hz = measure_quietly(GPUCC_DEBUG_AHB);
	if (ahb_hz == MEASURE_FAILED) {
		printf("vayu: %u: the frequency counter did not finish (step 5 shows whether it works)\n", step);
	} else {
		printf("vayu: %u: the GPU block's register bus clock (ahb) measures %u Hz\n", step, ahb_hz);
	}
	if (ahb_hz == MEASURE_FAILED || ahb_hz < 1000000) {
		printf("vayu: %u: without that clock a register of the GPU block would not answer: not touching it\n", step);
		return false;
	}
	return true;
}

/* ---- step 6: the first words from the GPU block ------------------------------------------------------- */

static struct block gmu = { "gmu", VAYU_GMU_PHYS, VAYU_GMU_SIZE };
#define GMU_WORD(n)             (4u * (n))
#define GMU_DTCM                GMU_WORD(0x1c00)
#define GMU_DTCM_VERSIONS       GMU_WORD(0x1c00 + 0xff8)
#define GMU_CM3_SYSRESET        GMU_WORD(0x5000)
#define GMU_CM3_BOOT_CONFIG     GMU_WORD(0x5001)
#define GMU_CM3_FW_INIT_RESULT  GMU_WORD(0x501c)
#define GMU_CM3_CFG             GMU_WORD(0x502d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x5088)
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x5089)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x50d0)
#define GMU_RPMH_POWER_STATE    GMU_WORD(0x50ec)
#define GMU_HFI_CTRL_STATUS     GMU_WORD(0x5180)
#define GMU_HFI_VERSION_INFO    GMU_WORD(0x5181)
#define GMU_GENERAL_7           GMU_WORD(0x51cc)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x9305)
#define GMU_RSCC_CONTROL_ACK    GMU_WORD(0x9308)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x930c)
#define GMU_AHB_FENCE_STATUS    GMU_WORD(0x9313)
#define GMU_RSCC_CONTROL_REQ    GMU_WORD(0x9307)

/* dt: kgsl_3d0_reg_memory at 0x2c00000; GBIF is words 0x3c00 on in a6xx.xml, at 0x2c0f000 */
static struct block gbif = { "gbif", VAYU_GPU_PHYS + 4 * 0x3c00, 0x1000 };
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)

static bool
step_first_words(void)
{
	uint64_t start, nanos;
	uint32_t before, hz;

	if (!cx_ready()) {
		return false;
	}
	announce("read registers of the GMU (0x2c6a000): the first words from the GPU block itself");
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "cm3_cfg", GMU_CM3_CFG);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "general_7", GMU_GENERAL_7);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	start = mach_absolute_time();
	before = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	hz = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
	printf("vayu: %u: the GMU's counter counts at %u Hz: %s\n", step, hz, hz > 18000000 && hz < 20500000 ?
	    "the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE" : "not the crystal's 19.2 MHz (firmware may start it): see the rest");

	announce("read the GPU's bus interface (0x2c0f000)");
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	return true;
}

/* ---- step 7: the GMU's channel to RPMh ----------------------------------------------------------------- */

/*
 * On a640 the RSCC is not a separate block: it is the GMU's register page at +0x23000
 * (0x2c8d000). The offsets are the same as redwood's RSCC block (a6xx_gmu.xml), only the
 * sleep/wake program in step 15 is the a640 one.
 */
static struct block rscc = { "rscc", VAYU_RSCC_PHYS, 0x2000 };
#define RSCC_RSC_STATUS0_DRV0   (4 * 0x004)
#define RSCC_PDC_SEQ_START_ADDR (4 * 0x008)
#define RSCC_PDC_MATCH_VALUE_LO (4 * 0x009)
#define RSCC_PDC_MATCH_VALUE_HI (4 * 0x00a)
#define RSCC_PDC_SLAVE_ID_DRV0  (4 * 0x00b)
#define RSCC_HIDDEN_TCS_CMD0_ADDR (4 * 0x00d)
#define RSCC_HIDDEN_TCS_CMD0_DATA (4 * 0x00e)
#define RSCC_OVERRIDE_START_ADDR (4 * 0x100)
#define RSCC_SEQ_BUSY_DRV0      (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0     (4 * 0x180)
#define RSCC_TCS0_DRV0_STATUS   (4 * 0x346)
#define RSCC_ASLEEP             (1u << 16)

static bool
step_rscc(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the GMU's RPMh channel (rscc at 0x2c8d000, the GMU's page +0x23000)");
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	return true;
}

/* ---- steps 8 and 9: the SMMU --------------------------------------------------------------------------- */

/*
 * dt: kgsl-smmu@0x02ca0000 (qcom,qsmmu-v500), 0x10000. The vendor layout: 8 stream match
 * registers and 8 contexts, context bank 0 at +0x8000 (KGSL_IOMMU_CB0_OFFSET), one 0x1000
 * page each. 2 * 8 * 0x1000 = 0x10000 is the whole register range. The GPU's streams (dt):
 * 0 and 1 user (mask 0x401), 2 secure, 4 GMU user, 5 GMU kernel.
 */
static struct block smmu = { "smmu", VAYU_SMMU_PHYS, VAYU_SMMU_SIZE };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58

static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;
static bool smmu_valid;

static bool
smmu_layout(void)
{
	uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);

	smmu_valid = false;
	smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	smmu_contexts = idr1 & 0xff;
	smmu_matches = idr0 & 0xff;
	printf("vayu: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
	    smmu_matches, smmu_contexts, smmu_pages, smmu_page);
	if (2 * smmu_pages * smmu_page != smmu.size) {
		printf("vayu: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
		    2 * smmu_pages * smmu_page, smmu.size);
		return false;
	}
	smmu_valid = true;
	return true;
}

static bool
step_smmu(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the ID registers of the GPU's SMMU (0x2ca0000): global registers, which the hypervisor answers for");
	if (!smmu_layout()) {
		return false;
	}
	get(&smmu, "idr2", SMMU_IDR2);
	get(&smmu, "idr7", SMMU_IDR7);
	get(&smmu, "scr0", SMMU_SCR0);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	announce("read the SMMU's stream table and the kinds of its contexts, as the boot loader left them");
	for (uint32_t i = 0; i < smmu_matches && i < 16; i++) {
		get(&smmu, "smr", SMMU_SMR(i));
		get(&smmu, "s2cr", SMMU_S2CR(i));
	}
	for (uint32_t i = 0; i < smmu_contexts && i < 16; i++) {
		get(&smmu, "cbar", smmu_page + SMMU_CBAR(i));
	}
	return true;
}

static bool
step_smmu_context(void)
{
	uint32_t context;

	if (!cx_ready()) {
		return false;
	}
	announce("read translation context 0 of the SMMU (0x2ca8000): registers of the hardware itself");
	if (!smmu_layout()) {
		return false;
	}
	context = smmu_pages * smmu_page;
	get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
	get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
	get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
	get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
	return true;
}

/* ---- step 10: the GMU's memory -------------------------------------------------------------------------- */

static bool
step_gmu_memory(void)
{
	static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
	uint32_t was[2], back[2], result;

	if (!cx_ready()) {
		return false;
	}
	announce("hold the GMU's processor in reset, write two words into its data memory, read them back");
	result = get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	if (result == 0xbabeface || (result & 0x1ff) == 0x100) {
		printf("vayu: %u: firmware is running in the GMU: leaving its memory alone\n", step);
		return false;
	}
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	for (int i = 0; i < 2; i++) {
		was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
	}
	for (int i = 0; i < 2; i++) {
		back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
	}
	printf("vayu: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
	    "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
	return back[0] == pattern[0] && back[1] == pattern[1];
}

/* ---- memory the GPU block reaches ------------------------------------------------------------------- */

#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_CACHED              (1ull << 2)
#define PTE_UNPRIVILEGED        (1ull << 6)
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)
#define PTE_READ_ONLY           (1ull << 7)

#define SMMU_S2CR_FAULT         (2u << 16)
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_STREAM_MASK        0x400
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe

static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
}

#define SPACE_TABLES            1024

struct gpu_space {
	const char *name;
	uint32_t context;
	uint32_t control;
	uint32_t table_count;
	uint64_t *root;
	struct {
		uint64_t phys;
		uint64_t *table;
		bool changed;
	} tables[SPACE_TABLES];
	uint64_t *last_entry, *last_table;
	uint32_t last_changed;
};

static uint64_t *
space_new_table(struct gpu_space *space)
{
	static vm_offset_t spare;
	static unsigned spare_left;
	uint64_t *table;

	if (space->table_count == SPACE_TABLES) {
		return NULL;
	}
	if (spare_left == 0) {
		if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			return NULL;
		}
		spare_left = PAGE_SIZE / 4096;
	}
	table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
	space->tables[space->table_count].phys = kvtophys((vm_offset_t)table);
	space->tables[space->table_count++].table = table;
	return table;
}

static void
space_changed(struct gpu_space *space, const uint64_t *entry)
{
	if ((uintptr_t)entry - (uintptr_t)space->tables[space->last_changed].table < 4096) {
		space->tables[space->last_changed].changed = true;
		return;
	}
	for (uint32_t i = space->table_count; i-- > 0;) {
		if ((uintptr_t)entry - (uintptr_t)space->tables[i].table < 4096) {
			space->tables[i].changed = true;
			space->last_changed = i;
			return;
		}
	}
}

static void
space_commit(struct gpu_space *space)
{
	for (uint32_t i = 0; i < space->table_count; i++) {
		if (space->tables[i].changed) {
			to_memory((vm_offset_t)space->tables[i].table, 4096);
			space->tables[i].changed = false;
		}
	}
}

static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < space->table_count; i++) {
			if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
				return space->tables[i].table;
			}
		}
		return NULL;
	}
	table = space_new_table(space);
	if (table != NULL) {
		*entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
		space_changed(space, entry);
	}
	return table;
}

static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint64_t *level2, *level3;

	if (address >> 39 || (space->root == NULL && (space->root = space_new_table(space)) == NULL)) {
		return false;
	}
	level2 = space_next(space, &space->root[address >> 30 & 0x1ff]);
	if (level2 == NULL) {
		return false;
	}
	if (space->last_entry == &level2[address >> 21 & 0x1ff]) {
		level3 = space->last_table;
	} else {
		level3 = space_next(space, &level2[address >> 21 & 0x1ff]);
		space->last_entry = &level2[address >> 21 & 0x1ff];
		space->last_table = level3;
	}
	if (level3 == NULL) {
		space->last_entry = NULL;
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
	space_changed(space, level3);
	return true;
}

struct gpu_block {
	uint64_t address;
	uint32_t size;
	vm_offset_t memory;
};

static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
	block->address = address;
	block->size = (size + 4095) & ~4095u;
	if (kmem_alloc(kernel_map, &block->memory, round_page(block->size), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (!space_map(space, address + offset, kvtophys(block->memory + offset), attributes)) {
			return false;
		}
	}
	to_memory(block->memory, block->size);
	printf("vayu: %u: %s sees %#llx..%#llx in memory of ours (%#llx)\n", step, space->name, address,
	    address + block->size - 1, kvtophys(block->memory));
	return true;
}

static vm_offset_t
block_uncached(const struct gpu_block *block)
{
	uint64_t phys = kvtophys(block->memory);

	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (kvtophys(block->memory + offset) != phys + offset) {
			return 0;
		}
	}
	return ml_io_map_wcomb(phys, block->size);
}

static uint32_t
smmu_context(const struct gpu_space *space)
{
	return (smmu_pages + space->context) * smmu_page;
}

static bool
space_faulted(const struct gpu_space *space)
{
	uint32_t context = smmu_context(space), status = rd(smmu.base + context + SMMU_CB_FSR);

	if ((status & SMMU_FSR_ALL) == 0) {
		return false;
	}
	printf("vayu: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
	    (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
	    rd(smmu.base + context + SMMU_CB_FSYNR0));
	return true;
}

static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream, uint32_t mask)
{
	uint32_t context = smmu_context(space);
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;

	for (uint32_t i = 0; i < space->table_count; i++) {
		to_memory((vm_offset_t)space->tables[i].table, 4096);
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, 0);
	put(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context), SMMU_CBA2R_64_BIT);
	put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
	put(&smmu, "cb tcr2", context + SMMU_CB_TCR2, SMMU_TCR2);
	put(&smmu, "cb tcr", context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0, (uint32_t)root);
	put(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&smmu, "cb ttbr1, low", context + SMMU_CB_TTBR1, 0);
	put(&smmu, "cb ttbr1, high", context + SMMU_CB_TTBR1 + 4, 0);
	put(&smmu, "cb mair0", context + SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&smmu, "cb mair1", context + SMMU_CB_MAIR1, 0);
	put(&smmu, "cb fsr", context + SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, space->context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("vayu: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
		    rd(smmu.base + context + SMMU_CB_TLBSTATUS));
		return false;
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | mask << 16 | stream);

	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | mask << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
	    (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
	    get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("vayu: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	get(&smmu, "cb tcr", context + SMMU_CB_TCR);
	get(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context));
	get(&smmu, "cb tcr2", context + SMMU_CB_TCR2);
	get(&smmu, "cb fsr", context + SMMU_CB_FSR);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	return true;
}

static void
space_off(struct gpu_space *space, uint32_t match)
{
	put(&smmu, "smr", SMMU_SMR(match), 0);
	put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
	put(&smmu, "cb sctlr", smmu_context(space) + SMMU_CB_SCTLR, 0);
}

/* ---- step 11: TrustZone -------------------------------------------------------------------------------- */

#define SCM_FAST                (1ull << 31)
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_ONE_VALUE           1
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_THREE_VALUES        3
#define SCM_INTERRUPTED         1
#define PAS_GPU                 13

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	uint64_t session = 0;

	printf("vayu: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	hold(true);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("vayu: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("vayu: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static bool
step_trustzone(void)
{
	static const struct {
		const char *name;
		uint32_t id;
	} images[] = { { "audio", 1 }, { "modem", 4 }, { "video", 9 }, { "GPU zap shader", PAS_GPU } };
	uint64_t answer = 0, how = SCM_FAST | SCM_64 | SCM_SIP;
	uint32_t fast[4] = { 0 }, slow[4] = { 0 };

	announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
	if (scm("whether it can be asked", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE, 0,
	    0, &answer) != 0 || answer != 1) {
		how = SCM_FAST | SCM_SIP;
		if (scm("the same, the 32-bit way", how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
		    0, 0, &answer) != 0 || answer != 1) {
			printf("vayu: %u: TrustZone does not answer the way it answers Linux\n", step);
			return false;
		}
	}
	announce("ask TrustZone which images it says it authenticates");
	for (unsigned i = 0; i < 4; i++) {
		answer = 0;
		fast[i] = scm(images[i].name, how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0, &answer) == 0 && answer;
		answer = 0;
		slow[i] = scm(images[i].name, (how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0,
		    0, &answer) == 0 && answer;
	}
	printf("vayu: %u: \"supported\", asked at once / as Linux asks: audio %u/%u, modem %u/%u, video %u/%u, GPU %u/%u\n",
	    step, fast[0], slow[0], fast[1], slow[1], fast[2], slow[2], fast[3], slow[3]);
	return true;
}

/* ---- step 12: the zap shader's signature ---------------------------------------------------------------- */

/*
 * dt: pil_gpu_region@98f15000, 0x2000 -- the zap shader's memory on this phone. Only the
 * signed header is offered here (a640_zap.mdt); the segment (a640_zap.b02) and the load are
 * left to the stage that powers the 3D core, which this port does not reach.
 */
#define VAYU_ZAP_MEMORY         0x98f15000ull
#define VAYU_ZAP_SIZE           0x2000u

static bool
zap_header(uint64_t *phys)
{
	const uint8_t *header;
	vm_offset_t page = 0;
	uint64_t size = 0;
	uint32_t segments, hashes;

	header = hakios_image_file("fw", "a640_zap.mdt", &size, NULL);
	if (header == NULL) {
		printf("vayu: %u: no fw/a640_zap.mdt in the small image: put the phone's /vendor/firmware/a640_zap.mdt "
		    "into fw-gpu/\n", step);
		return false;
	}
	segments = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || segments < 2 || size > PAGE_SIZE) {
		printf("vayu: %u: that is not such a header (%02x %02x %02x %02x, %u segments)\n", step, header[0], header[1],
		    header[2], header[3], segments);
		return false;
	}
	memcpy(&hashes, header + 52 + 32 + 16, sizeof(hashes));
	printf("vayu: %u: an ELF header of %u segments, %u bytes with its table, then %llu bytes; by the table, %u of hashes\n",
	    step, segments, 52 + 32 * segments, size - (52 + 32 * segments), hashes);
	if (52 + 32 * segments + (uint64_t)hashes != size) {
		printf("vayu: %u: the file is not the header followed by the hashes alone: not what TrustZone takes\n", step);
		return false;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("vayu: %u: no memory\n", step);
		return false;
	}
	/* Linux hands TrustZone a dma_alloc_coherent buffer (qcom_scm_pas_init_image): one physical piece */
	if (kvtophys(page + size - 1) != kvtophys(page) + size - 1) {
		printf("vayu: %u: the header's page is not physically in one piece\n", step);
		return false;
	}
	memcpy((void *)page, header, size);
	to_memory(page, PAGE_SIZE);
	*phys = kvtophys(page);
	return true;
}

static bool
step_zap_signature(void)
{
	uint64_t header = 0, answer = ~0ull;
	int32_t result;

	announce("find a640_zap.mdt in the small image and hand its header to TrustZone (image 13)");
	if (!zap_header(&header)) {
		return false;
	}
	result = scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
	    header, 0, &answer);
	if (result == 0 && answer == 0) {
		printf("vayu: %u: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL\n", step);
		return true;
	}
	printf("vayu: %u: TrustZone does not take it: the call returns %d, its answer is %#llx\n", step, result, answer);
	return false;
}

/* ---- step 13: memory for the GMU ------------------------------------------------------------------------ */

/*
 * The a640 GMU's firmware is a list of blocks with data, not of memory wanted: it fills
 *   0x00000..0x03fff  its code memory (through the GMU's registers)
 *   0x40000..0x43fff  its data memory (through the GMU's registers)
 *   0x04000..0x3ffff  instruction cache, 240 KB, in its own address space
 *   0x44000..0x83fff  data cache, 240 KB
 *   0x60000000        one dummy page (a6xx_gmu.c allocates it at that fixed address)
 * besides the log and the HFI table, which are ours to place. Redwood's 0x10004000 data
 * memory and its single extra block are the a650+ layout.
 */
#define GMU_ITCM_ADDRESS        0x00000000u
#define GMU_DTCM_ADDRESS        0x00040000u
#define GMU_TCM_SIZE            0x4000u
#define GMU_ICACHE_ADDRESS      0x00004000u
#define GMU_DCACHE_ADDRESS      0x00044000u
#define GMU_CACHE_SIZE          (0x40000u - 0x4000u)    /* 240 KB */
#define GMU_DUMMY_ADDRESS       0x60000000u
#define GMU_LOG_ADDRESS         0x60010000u
#define GMU_LOG_SIZE            0x4000u
#define GMU_HFI_ADDRESS         0x60020000u
#define GMU_STREAM              5
#define GMU_MATCH               1

struct gmu_firmware_block {
	uint32_t address, size, type, value;
};
#define GMU_FIRMWARE_NEEDS      1
#define GMU_FIRMWARE_NEEDS_KEPT 7

static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_blocks[12], gmu_icache, gmu_dcache, gmu_dummy, gmu_log, gmu_hfi;
static unsigned gmu_block_count;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;
static bool gmu_space_is_on, gmu_runs, gmu_talks, rscc_programmed;

static bool
in_tcm(uint32_t address)
{
	return address < GMU_TCM_SIZE || address - GMU_DTCM_ADDRESS < GMU_TCM_SIZE;
}

static bool
address_in_block(uint64_t address, uint32_t size, const struct gpu_block *block)
{
	return address >= block->address && address + size <= block->address + block->size;
}

static bool
step_gmu_space(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("find the GMU's firmware, fw/a640_gmu.bin, in the small image, and make the memory it needs");
	gmu_firmware = hakios_image_file("fw", "a640_gmu.bin", &gmu_firmware_size, NULL);
	if (gmu_firmware == NULL) {
		printf("vayu: %u: no fw/a640_gmu.bin in the small image: put the phone's /vendor/firmware/a640_gmu.bin "
		    "into fw-gpu/\n", step);
		return false;
	}
	/* The a640 firmware does not ask for these; a6xx_gmu.c allocates them by name. */
	if (!block_new(&gmu_space, &gmu_icache, GMU_ICACHE_ADDRESS, GMU_CACHE_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_dcache, GMU_DCACHE_ADDRESS, GMU_CACHE_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_dummy, GMU_DUMMY_ADDRESS, PAGE_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_LOG_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, PAGE_SIZE, PTE_GMU)) {
		return false;
	}
	for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
		struct gmu_firmware_block block;

		memcpy(&block, gmu_firmware + at, sizeof(block));
		at += sizeof(block) + block.size;
		if (at > gmu_firmware_size) {
			printf("vayu: %u: the file ends inside a block\n", step);
			return false;
		}
		if (block.size != 0 || (block.type != GMU_FIRMWARE_NEEDS && block.type != GMU_FIRMWARE_NEEDS_KEPT) ||
		    in_tcm(block.address)) {
			continue;
		}
		if (address_in_block(block.address, block.value, &gmu_icache) ||
		    address_in_block(block.address, block.value, &gmu_dcache) ||
		    address_in_block(block.address, block.value, &gmu_dummy)) {
			continue;
		}
		if (gmu_block_count == sizeof(gmu_blocks) / sizeof(gmu_blocks[0]) || (block.address & 4095) ||
		    block.value > 0x100000 || !block_new(&gmu_space, &gmu_blocks[gmu_block_count++], block.address, block.value,
		    PTE_GMU)) {
			printf("vayu: %u: cannot make %#x bytes at %#x for the firmware\n", step, block.value, block.address);
			return false;
		}
	}
	announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it");
	if (!smmu_layout()) {
		return false;
	}
	gmu_space_is_on = true;
	return space_on(&gmu_space, GMU_MATCH, GMU_STREAM, SMMU_STREAM_MASK);   /* dt: gmu_kernel iommus <... 0x05 0x400> */
}

/* ---- step 14: the GMU's firmware ------------------------------------------------------------------------ */

#define GMU_ITCM                GMU_WORD(0x0c00)

static bool
step_gmu_firmware(void)
{
	uint32_t wrong = 0;

	if (!cx_ready() || gmu_firmware == NULL) {
		return false;
	}
	announce("load the GMU's firmware: its 16K of code and of data through the GMU's registers, the rest into memory");
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	for (int pass = 0; pass < 2; pass++) {
		for (uint64_t at = 0; at + sizeof(struct gmu_firmware_block) <= gmu_firmware_size;) {
			struct gmu_firmware_block block;
			const uint8_t *data = gmu_firmware + at + sizeof(block);

			memcpy(&block, gmu_firmware + at, sizeof(block));
			at += sizeof(block) + block.size;
			if (block.size == 0) {
				continue;
			}
			if (in_tcm(block.address) && in_tcm(block.address + block.size - 1)) {
				uint32_t registers = block.address < GMU_DTCM_ADDRESS ? GMU_ITCM + block.address - GMU_ITCM_ADDRESS :
				    GMU_DTCM + block.address - GMU_DTCM_ADDRESS;

				for (uint32_t offset = 0; offset + 4 <= block.size; offset += 4) {
					uint32_t word;

					memcpy(&word, data + offset, sizeof(word));
					if (pass == 0) {
						wr(gmu.base + registers + offset, word);
					} else if (rd(gmu.base + registers + offset) != word) {
						wrong++;
					}
				}
				if (pass == 1) {
					printf("vayu: %u: %#x bytes at the GMU's %#x, through its registers\n", step, block.size,
					    block.address);
				}
				continue;
			}
			bool placed = false;
			for (unsigned i = 0; pass == 0 && i < gmu_block_count; i++) {
				if (address_in_block(block.address, block.size, &gmu_blocks[i])) {
					memcpy((void *)(gmu_blocks[i].memory + (block.address - gmu_blocks[i].address)), data, block.size);
					to_memory(gmu_blocks[i].memory, gmu_blocks[i].size);
					printf("vayu: %u: %#x bytes at the GMU's %#x, in a block the firmware asked for\n", step,
					    block.size, block.address);
					placed = true;
					break;
				}
			}
			if (placed) {
				continue;
			}
			if (address_in_block(block.address, block.size, &gmu_icache)) {
				if (pass == 0) {
					memcpy((void *)(gmu_icache.memory + (block.address - gmu_icache.address)), data, block.size);
					to_memory(gmu_icache.memory, gmu_icache.size);
					printf("vayu: %u: %#x bytes at the GMU's %#x, in its instruction cache\n", step, block.size,
					    block.address);
				}
			} else if (address_in_block(block.address, block.size, &gmu_dcache)) {
				if (pass == 0) {
					memcpy((void *)(gmu_dcache.memory + (block.address - gmu_dcache.address)), data, block.size);
					to_memory(gmu_dcache.memory, gmu_dcache.size);
					printf("vayu: %u: %#x bytes at the GMU's %#x, in its data cache\n", step, block.size, block.address);
				}
			} else if (address_in_block(block.address, block.size, &gmu_dummy)) {
				if (pass == 0) {
					memcpy((void *)(gmu_dummy.memory + (block.address - gmu_dummy.address)), data, block.size);
					to_memory(gmu_dummy.memory, gmu_dummy.size);
					printf("vayu: %u: %#x bytes at the GMU's %#x, in the dummy page\n", step, block.size, block.address);
				}
			} else {
				printf("vayu: %u: the firmware has %#x bytes for %#x, where it asked for no memory\n", step,
				    block.size, block.address);
				return false;
			}
		}
	}
	snprintf(last_write, sizeof(last_write), "step %u wrote the GMU's firmware into its code and data memory", step);
	if (wrong) {
		printf("vayu: %u: %u words of the GMU's own memory do not read back as written\n", step, wrong);
		return false;
	}
	printf("vayu: %u: THE GMU'S FIRMWARE IS LOADED; the versions in its data memory: %#x %#x %#x\n", step,
	    rd(gmu.base + GMU_DTCM_VERSIONS), rd(gmu.base + GMU_DTCM_VERSIONS + 8), rd(gmu.base + GMU_DTCM_VERSIONS + 16));
	return true;
}

struct hfi_queue {
	uint32_t status, start, type, words, message_words, dropped, rx_watermark, tx_watermark, rx_request, tx_request,
	    read, write;
};
struct hfi_table {
	uint32_t version, words, first_queue, queue_words, queues, active_queues;
	struct hfi_queue queue[17];
};
#define HFI_QUEUES              3
#define HFI_QUEUE_WORDS         1024
#define HFI_QUEUE_AT(n)         (((uint32_t)sizeof(struct hfi_table) + 15) / 16 * 16 + 4096 * (n))

static volatile struct hfi_table *hfi;

/*
 * The HFI table and its queue headers, written BEFORE the GMU's processor is let out of reset, as
 * Linux does (a6xx_hfi_init at probe, long before a6xx_gmu_fw_start) and redwood does (its
 * step_gmu_start). The first port filled it in step 16, after the firmware had started: the
 * a640 firmware then acknowledged HFI_CTRL_INIT but never read queue 0 (step 18 on the phone,
 * 2026-10-23 170139: queue 0 read 0, write 60, rx_request 1). INFERENCE: the firmware takes the
 * queue headers when it boots, and saw zeros.
 */
static bool
hfi_table_fill(void)
{
	static const uint8_t priority[HFI_QUEUES] = { 10, 10, 40 };

	hfi = (volatile struct hfi_table *)block_uncached(&gmu_hfi);
	if (hfi == NULL) {
		printf("vayu: %u: the page for the messages is not in one piece\n", step);
		return false;
	}
	hfi->words = sizeof(struct hfi_table) / 4;
	hfi->first_queue = 6;
	hfi->queue_words = sizeof(struct hfi_queue) / 4;
	hfi->queues = hfi->active_queues = HFI_QUEUES;
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		hfi->queue[i].start = GMU_HFI_ADDRESS + HFI_QUEUE_AT(i);
		hfi->queue[i].type = i | (uint32_t)priority[i] << 8;
		hfi->queue[i].words = HFI_QUEUE_WORDS;
		/*
		 * Both Linux (a6xx_hfi.c:984-986) and the vendor driver (kgsl_hfi.c:240-242) ask for an
		 * interrupt when the GMU puts a message in a queue: watermarks 1, rx_request 1. Left at 0
		 * (as redwood does: its a660 firmware answers anyway), the a640 firmware never raised
		 * gmu2host_intr_info for the first HFI message (step 18, 2026-10-23 163634-run.log).
		 */
		hfi->queue[i].rx_watermark = 1;
		hfi->queue[i].tx_watermark = 1;
		hfi->queue[i].rx_request = 1;
		hfi->queue[i].status = 1;
	}

	settle();
	return true;
}

/* ---- step 15: start the GMU ---------------------------------------------------------------------------- */

#define GMU_ICACHE_CONFIG       GMU_WORD(0x4c00)
#define GMU_DCACHE_CONFIG       GMU_WORD(0x4c01)
#define GMU_SYS_BUS_CONFIG      GMU_WORD(0x4c0f)
#define GMU_PWR_COL_INTER_FRAME_CTRL GMU_WORD(0x50c0)
#define GMU_RPMH_CTRL           GMU_WORD(0x50e8)
#define GMU_RPMH_CTRL_ALL       0xf11
#define GMU_PWR_COL_CP_MSG      GMU_WORD(0x5100)
#define GMU_PWR_COL_CP_RESP     GMU_WORD(0x5101)
#define GMU_HFI_SFR_ADDR        GMU_WORD(0x5182)
#define GMU_HFI_QTBL_INFO       GMU_WORD(0x5184)
#define GMU_HFI_QTBL_ADDR       GMU_WORD(0x5185)
#define GMU_HFI_CTRL_INIT       GMU_WORD(0x5186)
#define GMU_GMU2HOST_INTR_CLR   GMU_WORD(0x5191)
#define GMU_GMU2HOST_INTR_INFO  GMU_WORD(0x5192)
#define GMU_HOST2GMU_INTR_SET   GMU_WORD(0x5194)
#define GMU_AHB_FENCE_RANGE_0   GMU_WORD(0x9311)

#define GMU_CHIP_ID             0x06040001u
#define GMU_CHIP_FOR_FW         0x06040100u     /* chip_id with minor/patch packed the GMU's way */
#define GMU_STARTED_OLD         0xbabefaceu     /* firmware at or below core 0x20010004 */
#define GMU_STARTED_MASK        0x1ffu
#define GMU_STARTED             0x100u
#define GMU_FAILED              0xe00u

#define PDC_GPU_ENABLE          (4 * 0x1140)
#define PDC_GPU_SEQ_START_ADDR  (4 * 0x1148)
#define PDC_GPU_TCS1_CONTROL    (4 * 0x1572)
#define PDC_GPU_TCS1_CMD_ENABLE_BANK (4 * 0x1573)
#define PDC_GPU_TCS1_CMD_WAIT_BANK (4 * 0x1574)
#define PDC_GPU_TCS1_CMD0_MSGID (4 * 0x1575)
#define PDC_GPU_TCS1_CMD0_ADDR  (4 * 0x1576)
#define PDC_GPU_TCS1_CMD0_DATA  (4 * 0x1577)
#define PDC_GPU_TCS3_CONTROL    (4 * 0x15d6)
#define PDC_GPU_TCS3_CMD_ENABLE_BANK (4 * 0x15d7)
#define PDC_GPU_TCS3_CMD_WAIT_BANK (4 * 0x15d8)
#define PDC_GPU_TCS3_CMD0_MSGID (4 * 0x15d9)
#define PDC_GPU_TCS3_CMD0_ADDR  (4 * 0x15da)
#define PDC_GPU_TCS3_CMD0_DATA  (4 * 0x15db)
#define PDC_CMD_OFFSET          (4 * 4)
#define PDC_ENABLE_VALUE        0x80000001u
#define PDC_ADDRESS_OFFSET      0x30090         /* a640/a618 (a6xx_gmu.c) */

static struct block pdc = { "pdc", VAYU_PDC_PHYS, 0x10000 };
static struct block pdc_seq = { "pdc_seq", VAYU_PDC_SEQ_PHYS, 0x10000 };

/* The a640 RSCC sleep/wake program (a6xx_gmu.c for a618/a640; NOT the a650+ eaaae5a0 one). */
static const uint32_t rscc_program[5] = { 0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };

static void
rscc_program_blocks(void)
{
	if (get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0) == rscc_program[0] &&
	    get(&rscc, "seq_mem_4_drv0", RSCC_SEQ_MEM_0_DRV0 + 16) == rscc_program[4]) {
		printf("vayu: %u: the RSCC already has the a640 sleep/wake program\n", step);
		rscc_programmed = true;
		return;
	}
	announce("give the GMU's RPMh channel the a640 sleep/wake program (rscc 0x2c8d000)");
	put(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0, 1u << 24);
	put(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0, 1);
	put(&rscc, "hidden_tcs_cmd0_data", RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	put(&rscc, "hidden_tcs_cmd0_addr", RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	put(&rscc, "hidden_tcs_cmd1_data", RSCC_HIDDEN_TCS_CMD0_DATA + 8, 0);
	put(&rscc, "hidden_tcs_cmd1_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 8, 0);
	put(&rscc, "hidden_tcs_cmd2_data", RSCC_HIDDEN_TCS_CMD0_DATA + 16, 0x80000000); /* a640 */
	put(&rscc, "hidden_tcs_cmd2_addr", RSCC_HIDDEN_TCS_CMD0_ADDR + 16, 0);
	put(&rscc, "override_start_addr", RSCC_OVERRIDE_START_ADDR, 0);
	put(&rscc, "pdc_seq_start_addr", RSCC_PDC_SEQ_START_ADDR, 0x4520);
	put(&rscc, "pdc_match_value_lo", RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	put(&rscc, "pdc_match_value_hi", RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	for (unsigned i = 0; i < 5; i++) {
		put(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0 + 4 * i, rscc_program[i]);
	}
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	rscc_programmed = true;
}

static void
pdc_program_blocks(void)
{
	static const uint32_t ucode[5] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };

	announce("give the GPU's part of the always-on sequencer its a640 program (pdc 0xb290000 and 0xb490000)");
	for (unsigned i = 0; i < 5; i++) {
		put(&pdc_seq, "gpu_seq_mem", 4 * i, ucode[i]);
	}
	put(&pdc, "tcs1_control", PDC_GPU_TCS1_CONTROL, 0);
	put(&pdc, "tcs1_enable", PDC_GPU_TCS1_CMD_ENABLE_BANK, 7);
	put(&pdc, "tcs1_wait", PDC_GPU_TCS1_CMD_WAIT_BANK, 0);
	put(&pdc, "tcs1_cmd0_msgid", PDC_GPU_TCS1_CMD0_MSGID, 0x10108);
	put(&pdc, "tcs1_cmd0_addr", PDC_GPU_TCS1_CMD0_ADDR, 0x30010);
	put(&pdc, "tcs1_cmd0_data", PDC_GPU_TCS1_CMD0_DATA, 1);
	put(&pdc, "tcs1_cmd1_msgid", PDC_GPU_TCS1_CMD0_MSGID + PDC_CMD_OFFSET, 0x10108);
	put(&pdc, "tcs1_cmd1_addr", PDC_GPU_TCS1_CMD0_ADDR + PDC_CMD_OFFSET, 0x30000);
	put(&pdc, "tcs1_cmd1_data", PDC_GPU_TCS1_CMD0_DATA + PDC_CMD_OFFSET, 0);
	put(&pdc, "tcs1_cmd2_msgid", PDC_GPU_TCS1_CMD0_MSGID + 2 * PDC_CMD_OFFSET, 0x10108);
	put(&pdc, "tcs1_cmd2_addr", PDC_GPU_TCS1_CMD0_ADDR + 2 * PDC_CMD_OFFSET, PDC_ADDRESS_OFFSET);
	put(&pdc, "tcs1_cmd2_data", PDC_GPU_TCS1_CMD0_DATA + 2 * PDC_CMD_OFFSET, 0);

	put(&pdc, "tcs3_control", PDC_GPU_TCS3_CONTROL, 0);
	put(&pdc, "tcs3_enable", PDC_GPU_TCS3_CMD_ENABLE_BANK, 7);
	put(&pdc, "tcs3_wait", PDC_GPU_TCS3_CMD_WAIT_BANK, 0);
	put(&pdc, "tcs3_cmd0_msgid", PDC_GPU_TCS3_CMD0_MSGID, 0x10108);
	put(&pdc, "tcs3_cmd0_addr", PDC_GPU_TCS3_CMD0_ADDR, 0x30010);
	put(&pdc, "tcs3_cmd0_data", PDC_GPU_TCS3_CMD0_DATA, 2);
	put(&pdc, "tcs3_cmd1_msgid", PDC_GPU_TCS3_CMD0_MSGID + PDC_CMD_OFFSET, 0x10108);
	put(&pdc, "tcs3_cmd1_addr", PDC_GPU_TCS3_CMD0_ADDR + PDC_CMD_OFFSET, 0x30000);
	put(&pdc, "tcs3_cmd1_data", PDC_GPU_TCS3_CMD0_DATA + PDC_CMD_OFFSET, 3);       /* a640 */
	put(&pdc, "tcs3_cmd2_msgid", PDC_GPU_TCS3_CMD0_MSGID + 2 * PDC_CMD_OFFSET, 0x10108);
	put(&pdc, "tcs3_cmd2_addr", PDC_GPU_TCS3_CMD0_ADDR + 2 * PDC_CMD_OFFSET, PDC_ADDRESS_OFFSET);
	put(&pdc, "tcs3_cmd2_data", PDC_GPU_TCS3_CMD0_DATA + 2 * PDC_CMD_OFFSET, 3);

	put(&pdc, "gpu_seq_start", PDC_GPU_SEQ_START_ADDR, 0);
	put(&pdc, "gpu_enable", PDC_GPU_ENABLE, PDC_ENABLE_VALUE);
}

static bool
rscc_wake(void)
{
	if (!(rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {
		return true;
	}
	announce("wake the GMU's RPMh channel (it is asleep)");
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 2);
	if (!wait_for(&gmu, GMU_RSCC_CONTROL_ACK, 2, 2, 100) || !wait_for(&rscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0, 100)) {
		printf("vayu: %u: the GMU's RPMh channel does not wake\n", step);
		put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
		return false;
	}
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	return true;
}

static bool
step_gmu_start(void)
{
	uint32_t want, result = 0, version, fence = 0;
	bool old;

	if (!cx_ready() || !gmu_space_is_on) {
		return false;
	}
	if (get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) != 1) {
		printf("vayu: %u: the GMU's processor is not held in reset: not configuring it while it runs\n", step);
		return false;
	}
	if (!hfi_table_fill()) {
		return false;
	}
	rscc_program_blocks();
	pdc_program_blocks();
	put(&gmu, "general_7", GMU_GENERAL_7, 1);        /* TCM retention, before the RPMh channel wakes */
	if (!rscc_wake()) {
		return false;
	}

	announce("tell the GMU where things are, by its registers: the message table, the log, which GPU this is; its "
	    "caches and its use of RPMh; then let its processor out of reset");
	put(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT, 0);
	put(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG, 2);
	put(&gmu, "hfi_qtbl_addr", GMU_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
	put(&gmu, "hfi_qtbl_info", GMU_HFI_QTBL_INFO, 1);
	/*
	 * The default is Linux's (1 << 31) | (0xa << 18) | 0xa0 = 0x802800a0. The vendor driver writes
	 * GMU_FENCE_RANGE_MASK, (0x1 << 31) | ((0xA << 2) << 18) | 0x8A0 = 0x80a008a0 (vendor
	 * drivers/gpu/msm/adreno_a6xx_gmu.c:1001); vayu_gpu_fence=1 writes that value instead.
	 */
	PE_parse_boot_argn("vayu_gpu_fence", &fence, sizeof(fence));
	put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0,
	    fence ? ((1u << 31) | ((0xa << 2) << 18) | 0x8a0) : ((1u << 31) | (0xa << 18) | 0xa0));
	put(&gmu, "cm3_cfg", GMU_CM3_CFG, 0x4052);
	put(&gbif, "halt", GBIF_HALT, 0);
	put(&gmu, "pwr_col_cp_resp", GMU_PWR_COL_CP_RESP, 0);
	put(&gmu, "hfi_sfr_addr", GMU_HFI_SFR_ADDR, GMU_CHIP_FOR_FW);
	put(&gmu, "pwr_col_cp_msg", GMU_PWR_COL_CP_MSG, GMU_LOG_ADDRESS | (GMU_LOG_SIZE / 4096 - 1));
	put(&gmu, "sys_bus_config", GMU_SYS_BUS_CONFIG, 1);
	put(&gmu, "icache_config", GMU_ICACHE_CONFIG, 1);
	put(&gmu, "dcache_config", GMU_DCACHE_CONFIG, 1);
	put(&gmu, "pwr_col_inter_frame_ctrl", GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
	set(&gmu, "rpmh_ctrl", GMU_RPMH_CTRL, GMU_RPMH_CTRL_ALL);
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, 0xffffffff);

	version = rd(gmu.base + GMU_DTCM_VERSIONS);
	old = version <= 0x20010004;
	want = old ? GMU_STARTED_OLD : GMU_STARTED;
	printf("vayu: %u: firmware version word %#x: expecting %#x (the other family's value is accepted too)\n", step, version,
	    want);

	gmu_runs = true;
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 0);
	for (int i = 0; i < 5000; i++) {
		sleep_us(100);
		result = rd(gmu.base + GMU_CM3_FW_INIT_RESULT);
		if (result == GMU_STARTED_OLD || (result & GMU_STARTED_MASK) == GMU_STARTED) {
			break;
		}
		if (result != GMU_STARTED_OLD && (result & GMU_FAILED)) {
			break;
		}
	}
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	if (space_faulted(&gmu_space) || (result != GMU_STARTED_OLD && (result & GMU_STARTED_MASK) != GMU_STARTED)) {
		printf("vayu: %u: the GMU's firmware does not come up (init result %#x)\n", step, result);
		return false;
	}
	printf("vayu: %u: THE GMU'S FIRMWARE RUNS (init result %#x)\n", step, result);
	return true;
}

/* ---- step 16: the GMU's message queues ----------------------------------------------------------------- */


static bool
step_gmu_queues(void)
{
	if (!cx_ready() || !gmu_runs) {
		return false;
	}
	if (hfi == NULL) {
		printf("vayu: %u: the message table was not made in step 15\n", step);
		return false;
	}
	announce("ask the GMU to start its message queues");
	put(&gmu, "hfi_ctrl_init", GMU_HFI_CTRL_INIT, 1);
	if (!wait_for(&gmu, GMU_HFI_CTRL_STATUS, 1, 1, 500)) {
		get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
		get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
		space_faulted(&gmu_space);
		printf("vayu: %u: the GMU does not start its message queues\n", step);
		return false;
	}
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	gmu_talks = true;
	printf("vayu: %u: THE GMU'S MESSAGE QUEUES ARE UP\n", step);
	return !space_faulted(&gmu_space);
}

static bool image_asked_quietly;

static const uint8_t *
image_file(const char *directory, const char *name, uint64_t *size)
{
	int disk = -1;
	const uint8_t *data = hakios_image_file(directory, name, size, &disk);

	if (data != NULL) {
		if (!image_asked_quietly) {
			printf("vayu: %u: %s/%s: %llu bytes in the image of /dev/md%d\n", step, directory, name, *size, disk);
		}
		return data;
	}
	if (!image_asked_quietly) {
		printf("vayu: %u: no %s/%s in the small image: put the phone's /vendor/firmware/%s into fw-gpu/ and run "
		    "userland/build.sh\n", step, directory, name, name);
	}
	return NULL;
}

/* ---- steps 17 to 22: the 3D core ------------------------------------------------------------------------ */

/*
 * The port of redwood's steps 17 to 21 (hakios.adreno.c), split finer: what needs no write, what
 * powers the core, what fills its memory, what writes its registers, what starts its processor,
 * and -- last, alone -- what needs the signed zap shader. Redwood's ring, HFI and SMMU code is
 * reused; what differs on the Adreno 640 is said where it differs, with the line of Linux 6.18
 * (drivers/gpu/drm/msm/adreno/) or of the phone's device tree it comes from.
 *
 * dt: qcom,kgsl-3d0@2C00000: "kgsl_3d0_reg_memory" 0x2c00000, 0x40000; register numbers are in
 * words (Linux's registers/adreno/a6xx.xml).
 */
static struct block gpu = { "gpu", VAYU_GPU_PHYS, VAYU_GPU_SIZE };
#define GPU_WORD(n)             (4u * (n))
#define RBBM_GBIF_CLIENT_QOS_CNTL GPU_WORD(0x0011)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK      GPU_WORD(0x0017)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define RBBM_CLOCK_CNTL         GPU_WORD(0x00ae)
#define RBBM_INT_0_STATUS       GPU_WORD(0x0201)
#define RBBM_STATUS             GPU_WORD(0x0210)
#define RBBM_PERFCTR_CNTL       GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define RBBM_NC_MODE_CNTL       GPU_WORD(0x0534)
#define CP_RB_BASE              GPU_WORD(0x0800)
#define CP_RB_CNTL              GPU_WORD(0x0802)
#define CP_RB_RPTR              GPU_WORD(0x0806)
#define CP_RB_WPTR              GPU_WORD(0x0807)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define CP_HW_FAULT             GPU_WORD(0x0821)
#define CP_INTERRUPT_STATUS     GPU_WORD(0x0823)
#define CP_PROTECT_STATUS       GPU_WORD(0x0824)
#define CP_SQE_INSTR_BASE       GPU_WORD(0x0830)
#define CP_ROQ_THRESHOLDS_1     GPU_WORD(0x08c1)
#define CP_ROQ_THRESHOLDS_2     GPU_WORD(0x08c2)
#define CP_MEM_POOL_SIZE        GPU_WORD(0x08c3)
#define CP_AHB_CNTL             GPU_WORD(0x098d)
#define UCHE_MODE_CNTL          GPU_WORD(0x0e01)
#define UCHE_WRITE_RANGE_MAX    GPU_WORD(0x0e05)
#define UCHE_WRITE_THRU_BASE    GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE          GPU_WORD(0x0e09)
#define UCHE_GMEM_RANGE_MIN     GPU_WORD(0x0e0b)
#define UCHE_GMEM_RANGE_MAX     GPU_WORD(0x0e0d)
#define UCHE_CACHE_WAYS         GPU_WORD(0x0e17)
#define UCHE_FILTER_CNTL        GPU_WORD(0x0e18)
#define UCHE_CLIENT_PF          GPU_WORD(0x0e19)
#define RB_NC_MODE_CNTL         GPU_WORD(0x8e08)
#define PC_DBG_ECO_CNTL         GPU_WORD(0x9e00)
#define SP_NC_MODE_CNTL         GPU_WORD(0xae02)
#define TPL1_NC_MODE_CNTL       GPU_WORD(0xb604)
#define RBBM_SECVID_TRUST_CNTL  GPU_WORD(0xf400)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL    GPU_WORD(0xf803)
#define GBIF_QSB_SIDE0          (4 * 0x03)      /* of the bus interface (step 6): a6xx.xml 0x3c03 */

/* a6xx_gmu.c:314-323, GMU_OOB_GPU_SET for a GMU that is not "legacy" (only a630 is): set 30, ack 31, clear 31 */
#define GMU_CORE_REQUEST        (1u << 30)
#define GMU_CORE_GRANTED        (1u << 31)
#define GMU_CORE_RELEASE        (1u << 31)
/* a6xx_gmu.xml GMU_SPTPRAC_PWR_CLK_STATUS: bit 6 GX_HM_GDSC_POWER_OFF, bit 7 GX_HM_CLK_OFF (a6xx_gmu.c:108-118) */
#define GMU_GX_OFF              0xc0u

static bool
gx_has_power(void)
{
	need(&gpucc);
	need(&gmu);
	return (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) != 0 &&
	       (rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS) & GMU_GX_OFF) == 0;
}

/*
 * The 3D core's speeds and the RPMh level each needs. dt (vayu.dtb): qcom,gpu-pwrlevels-0 has
 * 585, 499.2, 427, 345, 257 MHz; qcom,gpu-pwrlevels-1 adds 675 MHz; gpu_opp_table_v2 gives the
 * levels as the vendor's 1-based opp-microvolt 0x41, 0x81, 0xc1, 0xe1, 0x101, 0x141. The same
 * speeds with the RPMh levels as RPMh counts them are mainline sm8150.dtsi:2262-2300
 * (gpu_opp_table: LOW_SVS 64, SVS 128, SVS_L1 192, SVS_L2 224, NOM 256, NOM_L1 320;
 * dt-bindings/power/qcom,rpmhpd.h). Entry 0 is "off", as Linux builds it (a6xx_gmu.c:1618).
 * 675 MHz is opp-supported-hw 0x2: speed bin 1 only (step 2's fuse).
 */
static const struct {
	uint32_t khz, level;
} core_speeds[] = {
	{ 0, 0 }, { 257000, 64 }, { 345000, 128 }, { 427000, 192 }, { 499200, 224 }, { 585000, 256 }, { 675000, 320 },
};

#define HFI_BW_TABLE            3
#define HFI_PERF_TABLE          4
#define HFI_START               10
#define HFI_FEATURE_CTRL        11      /* H2F_MSG_FEATURE_CTRL (kgsl_hfi.h:181); HFI_FEATURE_ECP = 1 (kgsl_hfi.h:80) */
#define HFI_CORE_FW_START       14
#define HFI_GX_BW_PERF_VOTE     30
#define HFI_PREPARE_SLUMBER     33
#define HFI_ERROR               100
#define HFI_ANSWER              1               /* in bits 19:16 */
#define HFI_NO_INDEX            0xff

/* a6xx_hfi.h:124-146 (the a640 is not a legacy GMU: a6xx_gmu.c:2073-2085, so the v2 tables) */
struct hfi_perf_table {
	uint32_t header, gpu_levels, gmu_levels;
	struct {
		uint32_t vote, acd, khz;
	} gpu[16];
	struct {
		uint32_t vote, khz;
	} gmu[4];
};
struct hfi_bw_table {
	uint32_t header, levels, cnoc_commands, ddr_commands, cnoc_wait, ddr_wait;
	uint32_t cnoc_address[6], cnoc_data[2][6], ddr_address[8], ddr_data[16][8];
};

static struct hfi_perf_table perf;
static struct hfi_bw_table bw;
static uint32_t hfi_number, core_speed, speed_levels;
static bool staged;                     /* vayu_adreno=N: the step-by-step bring-up, not the default path */
static uint32_t hfi_late, hfi_late_verdict = ~0u;   /* a message whose answer may still come */
static bool core_is_on, core_space_is_on, core_programmed, core_runs, core_stuck, zap_is_loaded, unsecured;

/* Everything waiting in a queue of the GMU's; true if the answer to the message numbered `number` was among it. */
static bool
hfi_receive(unsigned which, uint32_t number, uint32_t *verdict)
{
	volatile struct hfi_queue *queue = &hfi->queue[which];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(which));
	bool answered = false;

	while (queue->read != queue->write) {
		uint32_t message[8] = { 0 }, at = queue->read % HFI_QUEUE_WORDS, words = data[at] >> 8 & 0xff;

		for (uint32_t i = 0; i < words; i++) {
			if (i < 8) {
				message[i] = data[at];
			}
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		queue->read = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
		if ((message[0] >> 16 & 0xf) == HFI_ANSWER && message[1] >> 20 == number) {
			*verdict = message[2];
			answered = true;
		} else if ((message[0] >> 16 & 0xf) == HFI_ANSWER && hfi_late != 0 && message[1] >> 20 == hfi_late) {
			/* the GMU answers the speed vote only once it has carried it out (171211: after GX was up) */
			printf("vayu: %u: the GMU answers message %u (number %u) late: verdict %#x\n", step, message[1] & 0xff,
			    hfi_late, message[2]);
			hfi_late_verdict = message[2];
			hfi_late = 0;
		} else {
			message[7] = 0;
			if ((message[0] & 0xff) == HFI_ERROR) {         /* vendor's adreno_a6xx_receive_err_req */
				printf("vayu: %u: THE GMU REPORTS AN ERROR: %u, %u, \"%s\"\n", step, message[1] >> 16,
				    message[1] & 0xffff, (const char *)&message[2]);
			} else {
				printf("vayu: %u: the GMU says (queue %u, %u words): %#x %#x %#x %#x %#x %#x\n", step, which, words,
				    message[0], message[1], message[2], message[3], message[4], message[5]);
			}
		}
		if (words == 0) {
			break;
		}
	}
	return answered;
}

/* Send a message whose first word is yet to be made and wait for the GMU's verdict on it. */
static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words)
{
	volatile struct hfi_queue *queue = &hfi->queue[0];
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(0));
	uint32_t at = queue->write % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff, verdict = 0, waiting = 0;
	bool answered = false;

	message[0] = id | words << 8 | number << 20;
	printf("vayu: %u: about to send the GMU %s: message %u, %u words, number %u\n", step, what, id, words, number);
	snprintf(last_write, sizeof(last_write), "step %u sent the GMU message %u (%s)", step, id, what);
	hold(true);
	for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
		data[at] = i < words ? message[i] : 0xfafafafa;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	queue->write = at;
	settle();
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, 1);
	for (int i = 0; i < 10000 && !answered; i++) {
		sleep_us(100);
		waiting = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
		if (waiting & 1) {
			wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
		}
		/* the answer queue is looked at whether or not the interrupt bit came: it shows which of the two failed */
		answered = hfi_receive(1, number, &verdict);
		if (answered && !(waiting & 1)) {
			printf("vayu: %u: the GMU answered in its queue without raising gmu2host_intr_info\n", step);
		}
		if (rd(gmu.base + GMU_CM3_FW_INIT_RESULT) & GMU_FAILED) {
			break;
		}
	}
	hfi_receive(2, ~0u, &waiting);
	if (!answered || verdict != 0) {
		printf("vayu: %u: the GMU %s (%#x) to %s; fw_init_result %#x, gmu2host_intr_info %#x\n", step,
		    answered ? "says no" : "does not answer", verdict, what, rd(gmu.base + GMU_CM3_FW_INIT_RESULT),
		    rd(gmu.base + GMU_GMU2HOST_INTR_INFO));
		/* read == write on queue 0: the GMU took the message; read behind: it never looked at the queue */
		for (unsigned q = 0; q < HFI_QUEUES; q++) {
			printf("vayu: %u: queue %u: status %u, type %#x, read %u, write %u, dropped %u, rx_request %u\n", step,
			    q, hfi->queue[q].status, hfi->queue[q].type, hfi->queue[q].read, hfi->queue[q].write,
			    hfi->queue[q].dropped, hfi->queue[q].rx_request);
		}
		get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
		get(&gmu, "host2gmu_intr_raw_info (doorbell still pending if bit 0)", GMU_WORD(0x5196));
		get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
		get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
		get(&gmu, "pwr_col_cp_resp (log index)", GMU_PWR_COL_CP_RESP);
		to_memory(gmu_log.memory, 64);
		printf("vayu: %u: the GMU's log begins %#x %#x %#x %#x %#x %#x %#x %#x\n", step,
		    ((volatile uint32_t *)gmu_log.memory)[0], ((volatile uint32_t *)gmu_log.memory)[1],
		    ((volatile uint32_t *)gmu_log.memory)[2], ((volatile uint32_t *)gmu_log.memory)[3],
		    ((volatile uint32_t *)gmu_log.memory)[4], ((volatile uint32_t *)gmu_log.memory)[5],
		    ((volatile uint32_t *)gmu_log.memory)[6], ((volatile uint32_t *)gmu_log.memory)[7]);
		space_faulted(&gmu_space);
		get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
		get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
		get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
		return false;
	}
	printf("vayu: %u: the GMU accepts %s\n", step, what);
	return true;
}

/*
 * A vote for one level of a rail, as Linux builds it (a6xx_gmu.c:1515-1550): the rail's first
 * RPMh level at or above the one wanted, its place in the rail's list, and the place in the
 * memories' (mx) list of the first level at or above the one wanted -- or the highest non-zero.
 */
static bool
rail_vote(uint32_t level, const uint16_t *rail, unsigned rail_levels, const uint16_t *mx, unsigned mx_levels,
    uint32_t *vote)
{
	unsigned at, with = 0;

	for (at = 0; at < rail_levels && rail[at] < level; at++) {
	}
	if (at == rail_levels || mx_levels == 0) {
		return false;
	}
	for (unsigned j = 0; j < mx_levels; j++) {
		if (mx[j] >= level) {
			with = j;
			break;
		} else if (mx[j]) {
			with = j;
		}
	}
	*vote = (uint32_t)rail[at] << 16 | with << 8 | at;
	return true;
}

/*
 * The two tables the GMU is sent in step 18, made from the boot loader's RPMh command database
 * (as Linux does: a6xx_gmu_rpmh_arc_votes_init with "gfx.lvl"/"mx.lvl" and "cx.lvl"/"mx.lvl").
 * Only memory of ours is written.
 */
static bool
core_tables(void)
{
	uint16_t gfx[16], cx[16], mx[16];
	unsigned gfx_levels, cx_levels, mx_levels;
	uint32_t wanted = 0;

	if (cmd_db == NULL || !cmd_db_magic(cmd_db)) {
		/*
		 * On this phone the automatic search faults on the vendor node 0xc3f000c (AOP message RAM:
		 * the bring-up notes). Mainline sm8150.dtsi:706-710 reserves the database
		 * itself at 0x85f20000, which vayu_cmd_db=0x85f20000 reads directly.
		 */
		printf("vayu: %u: the 3D core's supply votes need the RPMh command database, and this run has %s "
		    "(try vayu_cmd_db=0x85f20000 instead of vayu_no_cmd_db=1): the GMU cannot be told a speed\n", step,
		    cmd_db == NULL ? "none" : "one whose magic does not match");
		return false;
	}
	if (!rpmh_resource("gfx.lvl", gfx, &gfx_levels) || !rpmh_resource("cx.lvl", cx, &cx_levels) ||
	    !rpmh_resource("mx.lvl", mx, &mx_levels)) {
		printf("vayu: %u: RPMh's list lacks one of gfx.lvl, cx.lvl, mx.lvl\n", step);
		return false;
	}
	speed_levels = speed_bin == 1 ? sizeof(core_speeds) / sizeof(core_speeds[0]) :
	    sizeof(core_speeds) / sizeof(core_speeds[0]) - 1;
	/*
	 * vayu_speed=N: N-th speed above "off". Staged runs (vayu_adreno=N) default to the lowest, 257
	 * MHz; the default path (the GPU kept for drawing) to 4, 499.2 MHz: with it and fb_threads=6
	 * the frames over 16.7 ms fell from 37 % to 25 % (motion runs M and M2, 2026-10-23).
	 */
	core_speed = staged ? 1 : 4;
	if (PE_parse_boot_argn("vayu_speed", &wanted, sizeof(wanted)) && wanted >= 1 && wanted < speed_levels) {
		core_speed = wanted;
	}
	perf.gpu_levels = speed_levels;
	for (unsigned i = 0; i < speed_levels; i++) {
		perf.gpu[i].khz = core_speeds[i].khz;
		perf.gpu[i].acd = 0xffffffff;           /* no adaptive clocking */
		if (!rail_vote(core_speeds[i].level, gfx, gfx_levels, mx, mx_levels, &perf.gpu[i].vote)) {
			printf("vayu: %u: RPMh has no level %u for the 3D core's rail\n", step, core_speeds[i].level);
			return false;
		}
	}
	/* the GMU at 200 MHz needs MIN_SVS (48): sm8150.dtsi:2332-2338 gmu_opp_table */
	perf.gmu_levels = 2;
	perf.gmu[0].khz = 0;
	perf.gmu[1].khz = 200000;
	if (!rail_vote(0, cx, cx_levels, mx, mx_levels, &perf.gmu[0].vote) ||
	    !rail_vote(48, cx, cx_levels, mx, mx_levels, &perf.gmu[1].vote)) {
		printf("vayu: %u: RPMh has no level 48 for the GMU's rail\n", step);
		return false;
	}
	printf("vayu: %u: speed bin %u: %u levels; votes:", step, speed_bin, speed_levels);
	for (unsigned i = 0; i < speed_levels; i++) {
		printf(" %u MHz %#x;", core_speeds[i].khz / 1000, perf.gpu[i].vote);
	}
	printf(" GMU %#x %#x; wanted: %u MHz (vayu_speed=%u)\n", perf.gmu[0].vote, perf.gmu[1].vote,
	    core_speeds[core_speed].khz / 1000, core_speed);

	/*
	 * The a640's bus table is fixed in Linux (a6xx_hfi.c:416-447, a640_build_bw_table): one "off"
	 * level, three DDR commands at 0x50000, 0x5003c, 0x5000c and three CNOC commands at 0x50034,
	 * 0x5007c, 0x5004c. Redwood's a660 table (other addresses, one CNOC command) is not used.
	 */
	memset(&bw, 0, sizeof(bw));
	bw.levels = 1;
	bw.ddr_commands = 3;
	bw.ddr_wait = 0x01;
	bw.ddr_address[0] = 0x50000;
	bw.ddr_address[1] = 0x5003c;
	bw.ddr_address[2] = 0x5000c;
	bw.ddr_data[0][0] = bw.ddr_data[0][1] = bw.ddr_data[0][2] = 0x40000000;
	bw.cnoc_commands = 3;
	bw.cnoc_wait = 0x01;
	bw.cnoc_address[0] = 0x50034;
	bw.cnoc_address[1] = 0x5007c;
	bw.cnoc_address[2] = 0x5004c;
	bw.cnoc_data[0][0] = 0x40000000;
	bw.cnoc_data[0][1] = 0x00000000;
	bw.cnoc_data[0][2] = 0x40000000;
	bw.cnoc_data[1][0] = 0x60000001;
	bw.cnoc_data[1][1] = 0x20000001;
	bw.cnoc_data[1][2] = 0x60000001;
	printf("vayu: %u: RPMh's own addresses for comparison (Linux's a640 table is fixed): MC0 %#x, SH0 %#x, ACV %#x, "
	    "CN0 %#x\n", step, rpmh_resource("MC0", NULL, NULL), rpmh_resource("SH0", NULL, NULL),
	    rpmh_resource("ACV", NULL, NULL), rpmh_resource("CN0", NULL, NULL));
	return true;
}

/*
 * The SQE microcode: Linux's a640 entry names a630_sqe.fw (a6xx_catalog.c:940-955), and refuses
 * one too old for the "whereami" security fix (a6xx_gpu.c:1008-1035): patched (low nibble of word
 * 0 is 0xa and of word 2 is >= 1) or version (word 0 & 0xfff) >= 0x190. The file's first word is
 * not microcode (redwood's step 18; Linux's adreno_fw_create_bo copies from byte 4).
 */
static const uint8_t *
sqe_file(uint64_t *size)
{
	const uint8_t *file = image_file("fw", "a630_sqe.fw", size);
	uint32_t word[3];

	if (file == NULL) {
		return NULL;
	}
	if (*size < 16 || *size > 0x40000) {
		printf("vayu: %u: fw/a630_sqe.fw is %llu bytes: not microcode\n", step, *size);
		return NULL;
	}
	memcpy(word, file + 4, sizeof(word));
	if (!(((word[0] & 0xf) == 0xa && (word[2] & 0xf) >= 1) || (word[0] & 0xfff) >= 0x190)) {
		printf("vayu: %u: fw/a630_sqe.fw is version %#x: older than 0x190, which Linux refuses (a6xx_gpu.c:1032)\n",
		    step, word[0] & 0xfff);
		return NULL;
	}
	printf("vayu: %u: fw/a630_sqe.fw: version %#x, new enough\n", step, word[0] & 0xfff);
	return file;
}

/* ---- the signed zap shader: what is there, and loading it --------------------------------------------- */

/*
 * The header a640_zap.mdt and its segments a640_zap.bNN: the phone's /vendor/firmware files
 * (dt: qcom,kgsl-hyp: qcom,pas-id 13, qcom,firmware-name "a640_zap"). Which segments are loaded,
 * and where, is Linux's mdt_loader.c: every PT_LOAD whose type is not "hash" (flags & 7 << 24 !=
 * 2 << 24) and whose memsz is not 0 (mdt_loader.c:54-66), at p_paddr less the lowest p_paddr of
 * those when the image is relocatable (bit 27), file NAME.bNN for program header NN
 * (mdt_loader.c:386-432). The place is the phone's reserved pil_gpu_region@98f15000, 0x2000
 * (vayu.dtb; mainline sm8150.dtsi has 0x98715000 for other boards).
 */
#define ZAP_TYPE_MASK           (7u << 24)
#define ZAP_TYPE_HASH           (2u << 24)
#define ZAP_RELOCATABLE         (1u << 27)
#define ZAP_SEGMENTS            8

struct zap_segment {
	uint32_t index, offset, filesz, memsz;
	char name[24];
};
static struct zap_segment zap_segments[ZAP_SEGMENTS];
static unsigned zap_segment_count;
static uint32_t zap_span;
static bool zap_init_refused;           /* PAS_INIT_IMAGE said no: on a second run in one boot, it is loaded already */

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Which signed files the image has; says by name what is missing. True if all are there. */
static bool
zap_files(bool loud)
{
	const uint8_t *header;
	uint64_t size = 0, segment_size = 0;
	uint32_t phoff, phnum, low = ~0u, high = 0;
	bool relocatable = false, all = true;

	image_asked_quietly = true;
	header = image_file("fw", "a640_zap.mdt", &size);
	image_asked_quietly = false;
	zap_segment_count = 0;
	if (header == NULL) {
		if (loud) {
			printf("vayu: %u: no fw/a640_zap.mdt: the signed zap shader is needed to take the 3D core out of secure "
			    "mode. Wanted from the phone's /vendor/firmware: a640_zap.mdt and its a640_zap.bNN segment "
			    "files (signed by Qualcomm/Xiaomi; nothing here can make them)\n", step);
		}
		return false;
	}
	if (size < 52 || memcmp(header, "\177ELF\1", 5) != 0) {
		printf("vayu: %u: fw/a640_zap.mdt is not a 32-bit ELF header\n", step);
		return false;
	}
	phoff = le32(header + 28);
	phnum = header[44] | header[45] << 8;
	if ((header[42] | header[43] << 8) != 32 || phoff + 32ull * phnum > size) {
		printf("vayu: %u: fw/a640_zap.mdt's program headers do not fit in it\n", step);
		return false;
	}
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = header + phoff + 32 * i;
		uint32_t flags = le32(ph + 24);

		if (le32(ph) != 1 || (flags & ZAP_TYPE_MASK) == ZAP_TYPE_HASH || le32(ph + 20) == 0) {
			continue;
		}
		relocatable |= (flags & ZAP_RELOCATABLE) != 0;
		low = (le32(ph + 12) < low ? le32(ph + 12) : low);
		high = (le32(ph + 12) + le32(ph + 20) > high ? le32(ph + 12) + le32(ph + 20) : high);
		if (zap_segment_count == ZAP_SEGMENTS) {
			printf("vayu: %u: more than %u segments to load\n", step, ZAP_SEGMENTS);
			return false;
		}
		struct zap_segment *s = &zap_segments[zap_segment_count++];
		s->index = i;
		s->offset = le32(ph + 12);
		s->filesz = le32(ph + 16);
		s->memsz = le32(ph + 20);
		snprintf(s->name, sizeof(s->name), "a640_zap.b%02u", i);
	}
	if (zap_segment_count == 0 || !relocatable) {
		printf("vayu: %u: fw/a640_zap.mdt has %u segments to load, %s: not the relocatable image this port loads\n", step,
		    zap_segment_count, relocatable ? "relocatable" : "not relocatable");
		return false;
	}
	/*
	 * The span TrustZone is given for the image ends on a 4 KB boundary: Linux mdt_loader.c:275-276
	 * (max_addr = ALIGN(p_paddr + p_memsz, SZ_4K)) and the vendor PIL (peripheral-loader.c:902-903,
	 * "XPUs that operate on 4K chunks"). Without it this phone's TrustZone refused MEM_SETUP
	 * (0x98f15000, 0x7b0 -> -3145798, 2026-10-23 171928); with it the a640 zap's span is 0x1000.
	 */
	zap_span = ((high + 0xfff) & ~0xfffu) - low;
	if (zap_span > VAYU_ZAP_SIZE) {
		printf("vayu: %u: the zap image spans %#x, more than the %#x reserved at %#llx\n", step, zap_span,
		    VAYU_ZAP_SIZE, VAYU_ZAP_MEMORY);
		return false;
	}
	for (unsigned i = 0; i < zap_segment_count; i++) {
		struct zap_segment *s = &zap_segments[i];

		s->offset -= low;
		if (s->filesz > s->memsz || s->offset + s->memsz > VAYU_ZAP_SIZE) {
			printf("vayu: %u: segment %u (%#x bytes at +%#x) does not fit the %#x bytes at %#llx\n", step, s->index,
			    s->memsz, s->offset, VAYU_ZAP_SIZE, VAYU_ZAP_MEMORY);
			return false;
		}
		if (s->filesz == 0) {
			continue;
		}
		image_asked_quietly = true;
		if (image_file("fw", s->name, &segment_size) == NULL || segment_size < s->filesz) {
			printf("vayu: %u: no fw/%s of %#x bytes (the phone's /vendor/firmware/%s)\n", step, s->name, s->filesz,
			    s->name);
			all = false;
		}
		image_asked_quietly = false;
	}
	if (all && loud) {
		printf("vayu: %u: the signed zap files are in the image: a640_zap.mdt and %u segment(s), %#x bytes from "
		    "%#llx\n", step, zap_segment_count, zap_span, VAYU_ZAP_MEMORY);
	}
	return all;
}

/*
 * Linux's order (adreno_gpu.c zap_shader_load_mdt, mdt_loader.c qcom_mdt_pas_init/qcom_mdt_load):
 * header to TrustZone, the place, the segments copied there, authenticate and reset. Before that,
 * whatever an earlier kernel may have left is given up (redwood's step 19 explains why).
 */
static bool
zap_load(void)
{
	volatile uint8_t *memory;
	uint64_t header = 0, answer = ~0ull, size = 0;

	/*
	 * No PAS_SHUTDOWN first. Redwood asks for one before every load; on this phone that call
	 * (smc 0x42000206, pas 13) never returned and took the phone off USB (2026-10-23 171242).
	 * Linux never shuts the zap image down (adreno_gpu.c zap_shader_load_mdt loads it once per
	 * boot; a6xx_zap_shader_init keeps a static "loaded"), so neither does this port: one zap load
	 * per phone boot. A second run without restarting the phone finds TrustZone refusing the
	 * header below, and stops there.
	 */
	if (!zap_header(&header)) {
		return false;
	}
	if (scm("to take the zap shader's signature", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
	    header, 0, &answer) != 0 || answer != 0) {
		printf("vayu: %u: TrustZone does not take the zap shader's signature (%#llx)\n", step, answer);
		zap_init_refused = true;
		return false;
	}
	if (scm("where the image will be", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, VAYU_ZAP_MEMORY,
	    zap_span, &answer) != 0 || answer != 0) {
		printf("vayu: %u: TrustZone does not take the place %#llx, %#x (answer %#llx)\n", step, VAYU_ZAP_MEMORY, zap_span,
		    answer);
		return false;
	}
	memory = (volatile uint8_t *)ml_io_map_wcomb(VAYU_ZAP_MEMORY, VAYU_ZAP_SIZE);
	printf("vayu: %u: about to write the zap shader's %u segment(s) at %#llx\n", step, zap_segment_count, VAYU_ZAP_MEMORY);
	snprintf(last_write, sizeof(last_write), "step %u wrote the zap shader into memory at %#llx", step, VAYU_ZAP_MEMORY);
	hold(true);
	for (uint32_t b = 0; b < zap_span; b++) {        /* the whole span, as the vendor PIL clears it (pil_memset_io) */
		memory[b] = 0;
	}
	for (unsigned i = 0; i < zap_segment_count; i++) {
		const struct zap_segment *s = &zap_segments[i];
		const uint8_t *data = s->filesz ? image_file("fw", s->name, &size) : NULL;

		for (uint32_t b = 0; b < s->memsz; b++) {
			memory[s->offset + b] = b < s->filesz && data != NULL ? data[b] : 0;
		}
	}
	settle();
	if (scm("to authenticate the zap shader and run it", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_GPU, 0,
	    0, &answer) != 0 || answer != 0) {
		printf("vayu: %u: TrustZone does not authenticate the zap shader (%#llx)\n", step, answer);
		return false;
	}
	printf("vayu: %u: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n", step);
	zap_is_loaded = true;
	return true;
}

/* ---- step 17: what the 3D half needs, looked at; no write ------------------------------------------ */

/*
 * Reading a register of the 3D core while its rail (GX) is off is a read the bus cannot answer,
 * so the core itself is read only if GX is found on. Everything else here is CX registers, the
 * command database, and the files in the image: the tables for step 18 are made in memory of
 * ours, the SQE microcode for step 19 is checked, and step 22's signed files are named.
 */
static bool
step_core_look(void)
{
	uint64_t size = 0;

	if (!cx_ready() || !gmu_talks) {
		printf("vayu: %u: needs the GMU's message queues up (step 16)\n", step);
		return false;
	}
	announce("read the 3D core's power state (GX switch, the GMU's view of it); no write of any kind");
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gpucc, "gx_domain_misc", GPUCC_GX_DOMAIN_MISC);
	get(&gpucc, "gx_sw_reset", GPUCC_GX_SW_RESET);
	get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "gmu2host_intr_info", GMU_GMU2HOST_INTR_INFO);
	get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	if (gx_has_power()) {
		printf("vayu: %u: GX IS ON before anyone asked for it; its registers are read, not written\n", step);
		get(&gpu, "rbbm_status", RBBM_STATUS);
		get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
		get(&gpu, "cp_rb_base", CP_RB_BASE);
		get(&gpu, "cp_sqe_cntl", CP_SQE_CNTL);
	} else {
		printf("vayu: %u: GX is off, as it should be: no register of the 3D core is read\n", step);
	}

	announce("make the GMU's speed and bus tables from RPMh's command database (in memory of ours)");
	if (!core_tables()) {
		return false;
	}

	announce("look for the files of the steps to come: fw/a630_sqe.fw (step 19), the signed zap shader (step 22)");
	if (sqe_file(&size) == NULL) {
		printf("vayu: %u: step 19 will stop: put linux-firmware's qcom/a630_sqe.fw (or the phone's "
		    "/vendor/firmware/a630_sqe.fw) into fw-gpu/\n", step);
	}
	if (!zap_files(true)) {
		printf("vayu: %u: STEP 22 WILL STOP before secure mode is left: steps 18-21 do not need the zap shader\n", step);
	}
	printf("vayu: %u: THE 3D HALF HAS WHAT STEP 18 NEEDS (GX off, tables made)\n", step);
	return true;
}

/* ---- step 18: the 3D core powered, through the GMU ----------------------------------------------------- */

/*
 * Linux's order (a6xx_hfi.c:905-942, then a6xx_gmu_set_initial_freq, then hw_init's
 * a6xx_gmu_set_oob(GPU_SET) at a6xx_gpu.c:1177-1181): the speed table, the bus table, "core
 * firmware start", "start", the speed wanted; then the request to keep the 3D core powered.
 * The GMU raises GX itself; this kernel never writes the GX switch on.
 */
static bool
step_core_power(void)
{
	uint32_t core_start[2] = { 0, 0 }, start[1] = { 0 }, vote[4] = { 0, 1, 0, 0 }, granted = 0;
	uint32_t fence = 0, ecp_control = 0, bus_index = 0;
	char what[256];

	if (!cx_ready() || !gmu_talks) {
		printf("vayu: %u: needs the GMU's message queues up (step 16)\n", step);
		return false;
	}
	if (gx_has_power()) {
		printf("vayu: %u: GX is on already, not by this kernel: not adding a request on top\n", step);
		return false;
	}
	if (speed_levels == 0 && !core_tables()) {
		return false;
	}
	vote[2] = core_speed;
	PE_parse_boot_argn("vayu_gpu_fence", &fence, sizeof(fence));
	PE_parse_boot_argn("vayu_gpu_ecp", &ecp_control, sizeof(ecp_control));
	if (PE_parse_boot_argn("vayu_gpu_bw", &bus_index, sizeof(bus_index)) && bus_index > HFI_NO_INDEX) {
		bus_index = 0;                  /* only 0..0xff is a bus index: anything else leaves today's 0 */
	}
	snprintf(what, sizeof(what), "send the GMU its a640 tables (speeds and supply levels, bus votes), "
	    "\"core firmware start\", \"start\", and the speed wanted; from here on it votes in RPMh by itself "
	    "(bus index %u, vayu_gpu_bw)", bus_index);
	announce(what);
	printf("vayu: options: fence %s, ECP control %s, bus index %u\n", fence ? "on" : "off", ecp_control ? "on" : "off",
	    bus_index);
	if (!hfi_send("the table of speeds", HFI_PERF_TABLE, &perf.header, sizeof(perf) / 4) ||
	    !hfi_send("the table of bus votes", HFI_BW_TABLE, &bw.header, sizeof(bw) / 4)) {
		return false;
	}
	if (ecp_control) {
		uint32_t ecp[3] = { 1, 0, 0 };

		/*
		 * The vendor driver sends this before "core firmware start" when the HFI major version is
		 * 2 or later (vendor kgsl_hfi.c:740). HFI_FEATURE_ECP is 1 and the payload after the
		 * header is { feature, enable, data } (vendor kgsl_hfi.h:80, 304-309). An unanswered
		 * feature control does not fail the step: one line is printed and the step goes on.
		 */
		if (!hfi_send("feature control: ECP off (as the vendor driver sends it)", HFI_FEATURE_CTRL, ecp, 3)) {
			printf("vayu: %u: the GMU did not answer the ECP feature control; going on\n", step);
		}
	}
	if (!hfi_send("\"core firmware start\"", HFI_CORE_FW_START, core_start, 2) ||
	    !hfi_send("\"start\"", HFI_START, start, 1)) {
		return false;
	}
	/*
	 * The speed wanted: { ack_type 1 (blocking), speed index, bus index } (a6xx_hfi.c:848-858). The
	 * bus index is 0, as Linux sends it for the a640 (a6xx_gmu.c a6xx_gmu_set_freq: bw_index stays 0
	 * without .bcms, and the a640 entry in a6xx_catalog.c has none), not redwood's 0xff -- which the
	 * vendor driver does send (kgsl_gmu.c:539-562, INVALID_DCVS_IDX), so this is a HYPOTHESIS: on
	 * the phone (170514) the GMU took message 30 with 0xff from its queue and never answered.
	 * vayu_gpu_bw=N sends N instead, so vayu_gpu_bw=255 is the vendor's INVALID_DCVS_IDX
	 * (kgsl_gmu_core.h:36).
	 * An unanswered vote does not end the step: Linux sends it after hfi_start and before the GX
	 * request too, but the GX request (below) is what powers the core; a GMU that is stuck in the
	 * vote will not grant that either, and the step then fails there, with the RPMh channel shown.
	 */
	vote[3] = bus_index;                    /* vayu_gpu_bw=N; the vendor's INVALID_DCVS_IDX is 0xff (kgsl_gmu_core.h:36) */
	if (!hfi_send("the speed wanted", HFI_GX_BW_PERF_VOTE, vote, 4)) {
		uint32_t strict = 0;

		hfi_late = hfi_number & 0xfff;          /* its answer is looked for until the end of the step */

		printf("vayu: %u: THE SPEED VOTE IS NOT ANSWERED; the GMU's RPMh channel as it is now:\n", step);
		get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
		get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
		get(&rscc, "tcs0_drv0_status", 4 * 0x346);       /* a6xx_gmu.xml:232-235; bit 16 = idle */
		get(&rscc, "tcs1_drv0_status", 4 * 0x3ee);
		get(&rscc, "tcs2_drv0_status", 4 * 0x496);
		get(&rscc, "tcs3_drv0_status", 4 * 0x53e);
		get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
		get(&gmu, "pwr_col_cp_resp (log index)", GMU_PWR_COL_CP_RESP);
		PE_parse_boot_argn("vayu_strict_vote", &strict, sizeof(strict));
		if (strict) {
			return false;
		}
		printf("vayu: %u: going on to the GX request anyway (vayu_strict_vote=1 stops here)\n", step);
	}

	announce("ask the GMU to power the 3D core and keep it powered (out-of-band GPU_SET: bit 30)");
	put(&gmu, "host2gmu_intr_set", GMU_HOST2GMU_INTR_SET, GMU_CORE_REQUEST);
	for (int i = 0; i < 5000 && !(granted & GMU_CORE_GRANTED); i++) {
		sleep_us(100);
		granted = rd(gmu.base + GMU_GMU2HOST_INTR_INFO);
	}
	core_is_on = true;                      /* asked for, at least: to be given back */
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, GMU_CORE_GRANTED);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gpucc, "gx_gdscr", GPUCC_GX_GDSCR);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	if (!(granted & GMU_CORE_GRANTED) || !gx_has_power()) {
		printf("vayu: %u: the GMU does not power the 3D core (gmu2host_intr_info %#x, fw_init_result %#x)\n", step,
		    granted, rd(gmu.base + GMU_CM3_FW_INIT_RESULT));
		space_faulted(&gmu_space);
		return false;
	}
	printf("vayu: %u: measured: the 3D core's clock runs at %u Hz (%u MHz was asked for), the GMU's in it at %u Hz\n",
	    step, measure_quietly(GPUCC_DEBUG_GX_GFX3D), core_speeds[core_speed].khz / 1000,
	    measure_quietly(GPUCC_DEBUG_GX_GMU));
	if (hfi_late != 0) {
		uint32_t ignored = 0;

		for (int i = 0; i < 1000 && hfi_late != 0; i++) {
			hfi_receive(1, ~0u, &ignored);
			if (hfi_late != 0) {
				sleep_us(1000);
			}
		}
		printf("vayu: %u: the speed vote: %s (verdict %#x)\n", step, hfi_late == 0 ? "answered late" :
		    "still no answer after a further second", hfi_late_verdict);
		wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
	}

	announce("read registers of the 3D core itself (0x2c00000): the first time it has power");
	get(&gpu, "rbbm_status", RBBM_STATUS);
	get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
	get(&gpu, "rbbm_clock_cntl", RBBM_CLOCK_CNTL);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "cp_sqe_cntl", CP_SQE_CNTL);
	printf("vayu: %u: THE 3D CORE HAS POWER AND ANSWERS\n", step);
	return !space_faulted(&gmu_space);
}

/* ---- step 19: the 3D core's memory and its microcode ------------------------------------------------ */

/*
 * The core's requests reach memory through the SMMU as stream 0 with mask 0x401 (dt:
 * qcom,kgsl-iommu@0x02CA0000/gfx3d_user: iommus = <... 0x00 0x401>; mainline sm8150.dtsi:2246
 * the same), which covers streams 0 and 1 in one match; the a640 has no LPAC, so redwood's
 * separate stream-1 context is not needed. Stream 2 (gfx3d_secure) is not touched. Context 0
 * holds the ring, the microcode (read-only to the core, as Linux maps it) and a page to write to.
 */
#define CORE_STREAM             0
#define CORE_STREAM_MASK        0x401
#define CORE_MATCH              0
#define PTE_CORE                (PTE_GMU | PTE_UNPRIVILEGED)
#define CORE_RING_ADDRESS       0x01000000u
#define CORE_RING_SIZE          0x8000u         /* Linux's; 8192 words */
#define CORE_SQE_ADDRESS        0x01100000u
#define CORE_SCRATCH_ADDRESS    0x01200000u
#define SMMU_SCTLR_SERVE_AFTER_FAULT (1u << 8)   /* Linux's qcom_adreno_smmu_write_sctlr: else the core hangs on a fault */

static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = SMMU_SCTLR_SERVE_AFTER_FAULT };
static struct gpu_block core_ring, core_sqe, core_scratch;
#define RING_WORDS              (CORE_RING_SIZE / 4)
static uint32_t ring_at, ring_sent;

static bool
step_core_space(void)
{
	const uint8_t *microcode;
	uint64_t size = 0;

	if (!cx_ready() || !core_is_on || !gx_has_power()) {
		printf("vayu: %u: needs the 3D core powered (step 18)\n", step);
		return false;
	}
	announce("make the 3D core's memory: its ring, its microcode (fw/a630_sqe.fw), a page to try writing to");
	microcode = sqe_file(&size);
	if (microcode == NULL) {
		printf("vayu: %u: no usable fw/a630_sqe.fw: the 3D core has no microcode to run\n", step);
		return false;
	}
	if (!block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE)) {
		return false;
	}
	memcpy((void *)core_sqe.memory, microcode + 4, size - 4);
	to_memory(core_sqe.memory, core_sqe.size);

	announce("give the SMMU's context 0 these tables and send the 3D core's requests (streams 0/1, mask 0x401) "
	    "through it");
	if (!smmu_layout()) {
		return false;
	}
	core_space_is_on = true;
	if (!space_on(&core_space, CORE_MATCH, CORE_STREAM, CORE_STREAM_MASK)) {
		return false;
	}
	get(&gpu, "rbbm_status", RBBM_STATUS);
	return !space_faulted(&core_space);
}

/* ---- step 20: the 3D core's registers, the ring placed; the command processor not started ---------------- */

/*
 * Linux's hw_init (a6xx_gpu.c:1168-1440) for the a640, less what only matters once it draws for
 * programs (clock gating a6xx_set_hwcg at :1229, the register protection a6xx_set_cp_protect at
 * :1367, interrupts, counters' selects -- as redwood leaves them out). Where the a640 differs
 * from redwood's a660:
 *   - GBIF QSB sides and client QoS 3 (:1231-1239: a640 family included); QoS is NOT then set
 *     back to 0, CP_CHICKEN_DBG is not written (:1369-1375: a660 family only);
 *   - UCHE GMEM range 1 MB .. 2 MB - 1 (:1261-1273: a640 is not a650 family; gmem SZ_1M in
 *     a6xx_catalog.c:946);
 *   - no CP_LPAC_PROG_FIFO_SIZE (:1295-1296, a660 only), no CP_APRIV_CNTL (:1394: the a640 has
 *     no ADRENO_QUIRK_HAS_HW_APRIV in a6xx_catalog.c:945-965), so CP_RB_CNTL gets NO_UPDATE
 *     (:1435-1439);
 *   - PC_DBG_ECO_CNTL = prim_fifo_threshold 0x00180000 (a6xx_catalog.c:960), not 0x00200200;
 *   - UBWC from SM8150's data (drivers/soc/qcom/ubwc_config.c:159-165: UBWC 3.0, swizzle levels
 *     2 and 3, highest bank bit 15) through a6xx_set_ubwc_config (:772-824): hbb 2 -> hbb_lo 2,
 *     hbb_hi 0; amsbc 1; ubwc_mode 0; rgb565 predicator 0; uavflagprd_inv 0 (a650+ only).
 *     The vendor DT agrees: qcom,highest-bank-bit = 0xf, qcom,ubwc-mode = 3.
 *   - redwood's RB_DBG_ECO_CNTL (a Mesa value for the a660) is not written: Mesa sets it itself.
 */
#define CP_RB_CNTL_VALUE        (12u | 2u << 8 | 1u << 27)      /* 2^12 pairs of words; NO_UPDATE */

static void
put64(struct block *b, const char *name, uint32_t offset, uint64_t value)
{
	put(b, name, offset, (uint32_t)value);
	put(b, name, offset + 4, (uint32_t)(value >> 32));
}

static bool
step_core_registers(void)
{
	static const uint32_t address_modes[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0x0e00, 0xae01,
		                                  0xb601, 0xf810 };     /* a6xx_gpu.c:1211-1225, a6xx.xml ..._ADDR_MODE_CNTL */
	const uint64_t trap = 0x1fffffffff000ull;                       /* a6xx_gpu.c:2718 uche_trap_base */

	if (!cx_ready() || !core_is_on || !core_space_is_on || !gx_has_power()) {
		printf("vayu: %u: needs the 3D core powered with its memory (steps 18 and 19)\n", step);
		return false;
	}
	announce("write the 3D core's registers as Linux does for an a640 before it starts the command processor: the "
	    "first writes to the core itself");
	put(&gbif, "halt", GBIF_HALT, 0);
	put(&gpu, "rbbm_gbif_halt", RBBM_GBIF_HALT, 0);
	put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
	put64(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	put(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	for (unsigned i = 0; i < sizeof(address_modes) / sizeof(address_modes[0]); i++) {
		put(&gpu, "address mode: 64 bits", GPU_WORD(address_modes[i]), 1);
	}
	for (unsigned i = 0; i < 4; i++) {
		put(&gbif, "qsb_side", GBIF_QSB_SIDE0 + 4 * i, 0x00071620);
	}
	put(&gpu, "rbbm_gbif_client_qos_cntl", RBBM_GBIF_CLIENT_QOS_CNTL, 3);
	put(&gpu, "rbbm_perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	put64(&gpu, "uche_write_range_max", UCHE_WRITE_RANGE_MAX, trap + 0xfc0);
	put64(&gpu, "uche_trap_base", UCHE_TRAP_BASE, trap);
	put64(&gpu, "uche_write_thru_base", UCHE_WRITE_THRU_BASE, trap);
	put64(&gpu, "uche_gmem_range_min", UCHE_GMEM_RANGE_MIN, 0x100000);
	put64(&gpu, "uche_gmem_range_max", UCHE_GMEM_RANGE_MAX, 0x100000 + 0x100000 - 1);
	put(&gpu, "uche_filter_cntl", UCHE_FILTER_CNTL, 0x804);
	put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 0x4);
	put(&gpu, "cp_roq_thresholds_2", CP_ROQ_THRESHOLDS_2, 0x02000140);
	put(&gpu, "cp_roq_thresholds_1", CP_ROQ_THRESHOLDS_1, 0x8040362c);
	put(&gpu, "cp_mem_pool_size", CP_MEM_POOL_SIZE, 128);
	put(&gpu, "pc_dbg_eco_cntl", PC_DBG_ECO_CNTL, 0x00180000);
	put(&gpu, "cp_ahb_cntl", CP_AHB_CNTL, 1);
	put(&gpu, "rbbm_perfctr_cntl", RBBM_PERFCTR_CNTL, 1);
	put(&gpu, "rb_nc_mode_cntl", RB_NC_MODE_CNTL, 1u << 4 | 2u << 1);
	put(&gpu, "tpl1_nc_mode_cntl", TPL1_NC_MODE_CNTL, 2u << 1);
	put(&gpu, "sp_nc_mode_cntl", SP_NC_MODE_CNTL, 2u << 1);
	put(&gpu, "uche_mode_cntl", UCHE_MODE_CNTL, 2u << 21);
	put(&gpu, "rbbm_nc_mode_cntl", RBBM_NC_MODE_CNTL, 0);
	put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x1fffff);
	put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);

	announce("tell the 3D core where its microcode and its ring are");
	put64(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE, CORE_SQE_ADDRESS);
	put64(&gpu, "cp_rb_base", CP_RB_BASE, CORE_RING_ADDRESS);
	put(&gpu, "cp_rb_cntl", CP_RB_CNTL, CP_RB_CNTL_VALUE);
	if (get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE) != CORE_SQE_ADDRESS ||
	    get(&gpu, "cp_rb_base", CP_RB_BASE) != CORE_RING_ADDRESS ||
	    get(&gpu, "cp_rb_cntl", CP_RB_CNTL) != CP_RB_CNTL_VALUE) {
		printf("vayu: %u: the 3D core did not keep where its microcode and ring are\n", step);
		return false;
	}
	get(&gpu, "rbbm_status", RBBM_STATUS);
	get(&gpu, "cp_sqe_cntl", CP_SQE_CNTL);
	core_programmed = true;
	printf("vayu: %u: THE 3D CORE'S REGISTERS TAKE WHAT IS WRITTEN; its command processor is still halted\n", step);
	return !space_faulted(&core_space);
}

/* ---- step 21: the command processor started; its first command ---------------------------------------- */

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((uint32_t *)core_ring.memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

/* The header of a command (a "type 7" packet, Mesa's adreno_pm4.xml) with so many words after it. */
static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

static void
ring_send(void)
{
	if (ring_at >= ring_sent) {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	} else {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
		to_memory(core_ring.memory, 4 * ring_at);
	}
	ring_sent = ring_at;
	wr(gpu.base + CP_RB_WPTR, ring_at);
}

/* Let the 3D core have what is in the ring, and wait until it has taken all of it and rests. */
static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	printf("vayu: %u: about to have the 3D core %s: the ring up to word %u\n", step, what, ring_at);
	snprintf(last_write, sizeof(last_write), "step %u had the 3D core %s", step, what);
	hold(true);
	ring_send();
	for (int i = 0; i < 20000 && (taken != ring_at || (status & ~1u)); i++) {
		sleep_us(100);
		taken = rd(gpu.base + CP_RB_RPTR);
		status = rd(gpu.base + RBBM_STATUS);
	}
	if (taken == ring_at && !(status & ~1u) && rd(gpu.base + CP_HW_FAULT) == 0 && !space_faulted(&core_space)) {
		printf("vayu: %u: the 3D core has done it (ring read up to %u, status %#x)\n", step, taken, status);
		return true;
	}
	core_stuck = taken != ring_at;
	printf("vayu: %u: the 3D core does not get through: ring read up to %u of %u, status %#x, interrupts %#x, "
	    "command processor: fault %#x, interrupts %#x, protection %#x\n", step, taken, ring_at, status,
	    rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS),
	    rd(gpu.base + CP_PROTECT_STATUS));
	space_faulted(&core_space);
	return false;
}

#define CP_MEM_WRITE            0x3d
#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66

/*
 * a6xx_gpu.c:1464 starts the SQE, then a6xx_cp_init (:907-934) sends CP_ME_INIT -- both before the
 * zap shader is loaded (:1482). The vendor driver has the same order (adreno_a6xx.c a6xx_rb_start:
 * SQE start, a6xx_send_cp_init, then adreno_set_unsecured_mode). So nothing in this step needs the
 * signed zap shader; the core runs in secure mode until step 22.
 */
static bool
step_core_start(void)
{
	if (!cx_ready() || !core_programmed || !gx_has_power()) {
		printf("vayu: %u: needs the 3D core's registers written (step 20)\n", step);
		return false;
	}
	announce("start the 3D core's command processor (cp_sqe_cntl = 1)");
	core_runs = true;
	put(&gpu, "cp_sqe_cntl", CP_SQE_CNTL, 1);
	sleep_ms(20);
	printf("vayu: %u: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
	    "protection %#x, ring read up to %u\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    rd(gpu.base + CP_RB_RPTR));
	get(&smmu, "sgfsr", SMMU_SGFSR);
	if (space_faulted(&core_space)) {
		return false;
	}

	announce("give the 3D core its first command, CP_ME_INIT");
	ring_command(CP_ME_INIT, 8);            /* a6xx_cp_init */
	ring(0x0000002f);
	ring(0x00000003);                       /* several hardware contexts */
	ring(0x20000000);                       /* error detection */
	ring(0);
	ring(0);
	ring(0);
	ring(0);
	ring(0);
	if (!ring_run("initialize its command processor")) {
		return false;
	}
	printf("vayu: %u: THE 3D CORE TAKES COMMANDS (still in secure mode)\n", step);
	return true;
}

/* ---- step 22: out of secure mode, with the zap shader; a word written on command ---------------------- */

/*
 * The 3D core comes up in secure mode. Linux (a6xx_gpu.c:1474-1503) and the vendor driver
 * (adreno.c adreno_set_unsecured_mode) leave it the same two ways: with a zap shader loaded,
 * CP_SET_SECURE_MODE 0 from the ring; without one, a direct write of 0 to RBBM_SECVID_TRUST_CNTL
 * -- which, Linux says, a TrustZone that locks the GPU answers with "a permissions violation".
 *
 * This step takes the first way when the signed files are in the image. Without them it stops
 * before anything that needs them and names the files. The second way is not taken unless
 * boot-arg vayu_nozap=1 asks for it: it is the test of the HYPOTHESIS that this phone's
 * TrustZone has not locked RBBM_SECVID_TRUST_CNTL (expected on a retail phone: it has, and the
 * write ends the run). Nothing here signs, fakes or patches a zap shader.
 */
static bool
step_core_unsecure(void)
{
	volatile uint32_t *scratch = (volatile uint32_t *)core_scratch.memory;
	uint32_t nozap = 0;

	if (!cx_ready() || !core_runs || !gx_has_power()) {
		printf("vayu: %u: needs the 3D core taking commands (step 21)\n", step);
		return false;
	}
	PE_parse_boot_argn("vayu_nozap", &nozap, sizeof(nozap));
	announce("check for the signed zap shader before anything that needs it");
	if (zap_files(true)) {
		announce("load the zap shader through TrustZone (image 13) into 0x98f15000, then command the core out of "
		    "secure mode");
		if (!zap_load()) {
			/*
			 * A second run in one boot: image 13 is TrustZone's already (171928/172532: one load
			 * per boot, and no PAS_SHUTDOWN, which hung this phone). Linux does the same --
			 * a6xx_zap_shader_init() loads once and asks for CP_SET_SECURE_MODE 0 after every
			 * start. Whether the zap shader is really there is what the secure-mode exit and the
			 * word written below show; a refusal for another reason fails there.
			 */
			if (!zap_init_refused) {
				return false;
			}
			printf("vayu: %u: TAKEN TO MEAN the zap shader was loaded by an earlier run in this boot; leaving secure "
			    "mode with it\n", step);
			zap_is_loaded = true;
		}
		ring_command(CP_SET_SECURE_MODE, 1);
		ring(0);
		if (!ring_run("leave secure mode")) {
			return false;
		}
	} else if (nozap) {
		announce("vayu_nozap=1: TEST OF A HYPOTHESIS: write 0 to rbbm_secvid_trust_cntl as Linux does without a zap "
		    "shader; on a locked phone this is a permissions violation and the run ends here");
		put(&gpu, "rbbm_secvid_trust_cntl", RBBM_SECVID_TRUST_CNTL, 0);
		get(&gpu, "rbbm_secvid_trust_cntl", RBBM_SECVID_TRUST_CNTL);
		get(&gpu, "cp_protect_status", CP_PROTECT_STATUS);
		get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
	} else {
		printf("vayu: %u: STOPPED BEFORE SECURE MODE: no signed zap shader in the image. Needed: fw/a640_zap.mdt and "
		    "its fw/a640_zap.bNN segments from the phone's /vendor/firmware. The 3D core stays in secure mode; "
		    "no CP_SET_SECURE_MODE, no SECVID write was made\n", step);
		return false;
	}

	announce("have the 3D core write a word into a page of ours");
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS);
	ring(0);
	ring(0x600dc0de);
	if (!ring_run("write a word to memory")) {
		return false;
	}
	to_memory(core_scratch.memory, 64);
	printf("vayu: %u: the page reads %#x: %s\n", step, scratch[0], scratch[0] == 0x600dc0de ?
	    "THE 3D CORE WRITES OUR MEMORY ON COMMAND, OUT OF SECURE MODE" : "not what it was told to write");
	unsecured = scratch[0] == 0x600dc0de;
	return unsecured;
}

/*
 * The GPU block's bus interface told to let what is under way finish and start nothing more
 * (Linux a6xx_bus_clear_pending_transactions, a6xx_gpu.c:2132-2170; redwood's core_bus_halt).
 * `for_good`: the 3D core's side halted too and left so.
 */
static void
core_bus_halt(bool for_good)
{
	if (for_good) {
		wr(gpu.base + RBBM_GBIF_HALT, 1);
		for (int i = 0; i < 100000 && !(rd(gpu.base + RBBM_GBIF_HALT_ACK) & 1); i++) {
		}
	}
	wr(gbif.base + GBIF_HALT, 1);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 1); i++) {
	}
	wr(gbif.base + GBIF_HALT, 2);
	for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 2); i++) {
	}
	if (!for_good) {
		wr(gbif.base + GBIF_HALT, 0);
	}
}

/* ---- the GPU kept on, for the system to draw with (boot-arg vayu_keep=1) ---------------------------------- */

/*
 * With vayu_adreno=22 vayu_keep=1, a run whose step 22 has proved the core out of secure mode does
 * not stop the block: the core stays powered and taking commands, and the service HakiosGPU
 * (iokit/Kernel/hakios.gpu.cpp, unchanged) hands it to programs through the interface of Linux's
 * msm driver, which Mesa's freedreno speaks. That service calls hakios_gpu_...(), and those
 * (hakios.adreno.c) call the vayu_gpu_...() below while vayu_gpu_kept() is true. What is here
 * is redwood's "GPU kept on" (hakios.adreno.c) for this phone: the same address allocator, page
 * tables, ring and fence, with the a640's answers to msm's GET_PARAM. Recovery of a hung core
 * (redwood's gpu_recover=1) is not ported: a submission that never finishes is reported and the
 * GPU stays stopped for the run.
 */
#define GPU_ADDRESSES_START     0x20000000ull   /* above the steps' blocks and the GMEM window 1..2 MB; below 4 GB */
#define GPU_ADDRESSES_SIZE      0xe0000000ull
#define GPU_ADDRESS_GRANULE     0x10000ull
#define GPU_FENCE_AT            0x40            /* in the scratch page */
#define GPU_TIMES_AT            0x400
#define GPU_TIMES               64
#define CP_ALWAYS_ON_COUNTER    0x0980
#define CP_SCRATCH_REG2         0x0885
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)
#define CP_IB2_BASE             GPU_WORD(0x092b)
#define CP_IB2_REM_SIZE         GPU_WORD(0x092d)
#define CP_WAIT_FOR_IDLE        0x26
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_EVENT_WRITE          0x46
#define EVENT_CACHE_FLUSH       0x04
#define EVENT_INVALIDATE_DEPTH  0x18
#define EVENT_INVALIDATE_COLOR  0x19

extern uint32_t gpu_log;                /* hakios.adreno.c's boot-arg, shared with hakios.gpu.cpp */
uint32_t gpu_va;                        /* boot-arg gpu_va: 0 as before, 1 describe, 2 also guard */
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern void HakiosGPUStart(void);

static bool gpu_kept;
static uint32_t gpu_fence, gpu_timed, gpu_timed_count, gpu_timed_by[7], gpu_exact = 1;
static uint64_t gpu_ticks;
static struct {
	uint64_t start, size;
} gpu_free[1024];
static unsigned gpu_free_count, gpu_va_holes;
static uint64_t gpu_va_used, gpu_va_lost;
static uint32_t gpu_va_live, gpu_va_refused;

bool
vayu_gpu_kept(void)
{
	return gpu_kept;
}

/* A register of the 3D core (by its word number) written by command (a "type 4" packet). */
static void
ring_register(uint32_t number, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	ring(value);
}

/*
 * msm's GET_PARAM for the a640, as Linux answers it (adreno_gpu.c:364-440): GPU_ID is the revn,
 * 640 (a6xx_catalog.c a640 entry), and with a revn the speed bin is NOT put above the chip ID;
 * GMEM 1 MB at 0x100000 (not a650 family); UBWC from SM8150's data (ubwc_config.c:159-165:
 * highest bank bit 15, swizzle levels 2|3 = 6, no macrotile mode); UCHE trap base
 * a6xx_gpu.c:2718. Redwood's numbers (revn 0, 512 KB, bank bit 14) are the a642L's.
 */
bool
vayu_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID */
		*value = 640;
		return true;
	case 0x02:                              /* GMEM_SIZE */
		*value = 0x100000;
		return true;
	case 0x03:                              /* CHIP_ID */
		*value = GMU_CHIP_ID;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = core_speeds[core_speed].khz * 1000ull;
		return true;
	case 0x05:                              /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x06:                              /* GMEM_BASE */
		*value = 0x100000;
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING */
	case 0x13:                              /* MACROTILE_MODE */
		*value = 0;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT */
		*value = 15;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE */
		*value = 6;
		return true;
	case 0x14:                              /* UCHE_TRAP_BASE */
		*value = 0x1fffffffff000ull;
		return true;
	case 0x0e:                              /* VA_START: refused without gpu_va, as redwood (one shared context) */
	case 0x0f:                              /* VA_SIZE */
		if (gpu_va == 0) {
			return false;
		}
		*value = which == 0x0e ? GPU_ADDRESSES_START : GPU_ADDRESSES_SIZE;
		return true;
	default:
		return false;
	}
}

static uint64_t
gpu_address_span(uint64_t size)
{
	uint64_t span = (size + (GPU_ADDRESS_GRANULE - 1)) & ~(GPU_ADDRESS_GRANULE - 1);

	return gpu_va >= 2 ? span + GPU_ADDRESS_GRANULE : span;
}

/* hakios_gpu_address(): first fit out of the free list, lowest first. */
uint64_t
vayu_gpu_address(uint64_t size)
{
	size = gpu_address_span(size);
	for (unsigned i = 0; i < gpu_free_count; i++) {
		if (gpu_free[i].size >= size) {
			uint64_t address = gpu_free[i].start;

			gpu_free[i].start += size;
			gpu_free[i].size -= size;
			if (gpu_free[i].size == 0) {
				memmove(&gpu_free[i], &gpu_free[i + 1], (--gpu_free_count - i) * sizeof(gpu_free[0]));
			}
			gpu_va_used += size;
			gpu_va_live++;
			return address;
		}
	}
	gpu_va_refused++;
	return 0;
}

void
vayu_gpu_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = gpu_address_span(size);
	gpu_va_used -= size;
	gpu_va_live--;
	for (at = 0; at < gpu_free_count && gpu_free[at].start < address; at++) {
	}
	if (at > 0 && gpu_free[at - 1].start + gpu_free[at - 1].size == address) {
		gpu_free[at - 1].size += size;
		if (at < gpu_free_count && address + size == gpu_free[at].start) {
			gpu_free[at - 1].size += gpu_free[at].size;
			memmove(&gpu_free[at], &gpu_free[at + 1], (--gpu_free_count - at) * sizeof(gpu_free[0]));
		}
	} else if (at < gpu_free_count && address + size == gpu_free[at].start) {
		gpu_free[at].start = address;
		gpu_free[at].size += size;
	} else if (gpu_free_count < sizeof(gpu_free) / sizeof(gpu_free[0])) {
		memmove(&gpu_free[at + 1], &gpu_free[at], (gpu_free_count++ - at) * sizeof(gpu_free[0]));
		gpu_free[at].start = address;
		gpu_free[at].size = size;
	} else {
		gpu_va_lost += size;            /* no room to remember the hole: lost rather than handed out twice */
	}
	if (gpu_free_count > gpu_va_holes) {
		gpu_va_holes = gpu_free_count;
	}
}

void
vayu_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most, uint64_t *lost,
    uint32_t *refused)
{
	*largest = 0;
	for (unsigned i = 0; i < gpu_free_count; i++) {
		if (gpu_free[i].size > *largest) {
			*largest = gpu_free[i].size;
		}
	}
	*used = gpu_va_used;
	*live = gpu_va_live;
	*holes = gpu_free_count;
	*most = gpu_va_holes;
	*lost = gpu_va_lost;
	*refused = gpu_va_refused;
}

bool
vayu_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

void
vayu_gpu_mapped(void)
{
	space_commit(&core_space);
}

void
vayu_gpu_unmap(uint64_t address, uint64_t size)
{
	uint32_t context = smmu_context(&core_space);

	for (uint64_t offset = 0; offset < size; offset += 4096) {
		space_map(&core_space, address + offset, 0, 0);
	}
	space_commit(&core_space);
	wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

void
vayu_gpu_to_memory(void *start, uint64_t size)
{
	to_memory((vm_offset_t)start, size);
}

bool
vayu_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
vayu_gpu_last(void)
{
	return gpu_fence;
}

static void
ring_time(uint32_t fence, uint32_t end)
{
	ring_command(CP_REG_TO_MEM, 3);
	ring(CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);       /* two words, a 64-bit register */
	ring(CORE_SCRATCH_ADDRESS + GPU_TIMES_AT + 16 * (fence % GPU_TIMES) + 8 * end);
	ring(0);
}

static void
gpu_times(void)
{
	volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + GPU_TIMES_AT);
	uint32_t done;

	to_memory(core_scratch.memory, 4096);
	done = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
	if (done - gpu_timed > GPU_TIMES) {
		gpu_timed = done - GPU_TIMES;
	}
	while (gpu_timed != done) {
		uint32_t slot = ++gpu_timed % GPU_TIMES;

		if (gpu_timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
			static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };
			uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
			unsigned by = 0;

			if (gpu_log) {
				hakios_tl("tl: q %u took %llu\n", gpu_timed, ticks * 10 / 192);
			}
			while (by < 6 && ticks >= under[by]) {
				by++;
			}
			gpu_timed_by[by]++;
			gpu_ticks += ticks;
			gpu_timed_count++;
		}
	}
}

void
vayu_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	gpu_times();
	*microseconds = gpu_ticks * 10 / 192;
	*submissions = gpu_timed_count;
	memcpy(by, gpu_timed_by, sizeof(gpu_timed_by));
	bzero(gpu_timed_by, sizeof(gpu_timed_by));
	gpu_ticks = 0;
	gpu_timed_count = 0;
}

/* Command buffers in order, as Linux's a6xx_submit puts them in the ring (hakios_gpu_submit()). */
uint32_t
vayu_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 26 + 4 * count, room = 0;

	for (int i = 0; i < 20000; i++) {
		room = (rd(gpu.base + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		sleep_us(100);
	}
	if (room < wanted) {
		return 0;
	}
	gpu_times();
	if (++gpu_fence == 0) {
		gpu_fence = 1;
	}
	ring_time(gpu_fence, 0);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_COLOR);
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	if (gpu_exact) {
		ring_command(CP_WAIT_FOR_IDLE, 0);
	}
	ring_time(gpu_fence, 1);
	ring_register(CP_SCRATCH_REG2, gpu_fence);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_FLUSH);
	ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
	ring(0);
	ring(gpu_fence);
	ring_send();
	return gpu_fence;
}

void
vayu_gpu_report(const char *why)
{
	uint32_t context = smmu_context(&core_space);

	printf("vayu: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u, written up to %u; status "
	    "%#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x\n", why, gpu_fence,
	    *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), rd(gpu.base + CP_RB_RPTR), ring_at,
	    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT),
	    rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	printf("vayu: gpu:   in the first command buffer at %#llx, %u words to go; in the second at %#llx, %u to go\n",
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_IB2_BASE + 4) << 32 | rd(gpu.base + CP_IB2_BASE), rd(gpu.base + CP_IB2_REM_SIZE));
	step = 0;
	if (space_faulted(&core_space)) {
		wr(smmu.base + context + SMMU_CB_FSR, SMMU_FSR_ALL);
	}
}

bool
vayu_gpu_recover(void)
{
	printf("vayu: gpu: recovery of a hung 3D core is not ported to vayu: the GPU stays stopped for this run\n");
	return false;
}

static void vayu_block_stop(void);

/* HakiosGPUStop() holds the clients' lock around this: what the core was given, it finishes first. */
void
vayu_gpu_stop(void)
{
	uint32_t waited = 0;

	if (!gpu_kept) {
		return;
	}
	gpu_kept = false;
	quiet = false;
	while (!vayu_gpu_done(gpu_fence) && waited < 5000) {
		sleep_us(100);
		waited++;
	}
	core_stuck = !vayu_gpu_done(gpu_fence);
	printf("vayu: the run is about to end: the GPU is given back (%s after %u us)\n",
	    core_stuck ? "STILL AT WORK" : "it had finished what it was given", 100 * waited);
	vayu_block_stop();
}

/* Called by vayu_adreno_init() when step 22 has passed and vayu_keep=1 asks for it. */
static bool
gpu_keep(void)
{
	if (!unsecured || !core_runs) {
		printf("vayu: not kept: the 3D core is not out of secure mode taking commands (step 22)\n");
		return false;
	}
	gpu_free[0].start = GPU_ADDRESSES_START;
	gpu_free[0].size = GPU_ADDRESSES_SIZE;
	gpu_free_count = 1;
	bzero((void *)core_scratch.memory, 4096);
	to_memory(core_scratch.memory, 4096);
	PE_parse_boot_argn("gpu_exact", &gpu_exact, sizeof(gpu_exact));
	/* hakios_adreno_init() returns before it reads these on vayu; they belong to hakios.gpu.cpp too */
	PE_parse_boot_argn("gpu_log", &gpu_log, sizeof(gpu_log));
	PE_parse_boot_argn("gpu_va", &gpu_va, sizeof(gpu_va));     /* before any client: spans must not change */
	gpu_kept = true;
	quiet = true;
	printf("vayu: THE GPU STAYS ON for the system to draw with: the service HakiosGPU (Adreno 640, chip id %#x, %u MHz)\n",
	    GMU_CHIP_ID, core_speeds[core_speed].khz / 1000);
	HakiosGPUStart();
	return true;
}

/* ---- the way back ------------------------------------------------------------------------------------- */

static void
gx_off(void)
{
	uint32_t gx;

	if (gpucc.base == 0 || gcc.base == 0) {
		return;
	}
	gx = rd(gpucc.base + GPUCC_GX_GDSCR);
	if (gx & GDSC_POWER_ON) {
		wr(gpucc.base + GPUCC_GX_GDSCR, gx | GDSC_SW_COLLAPSE);
		wait_for(&gpucc, GPUCC_GX_GDSCR, GDSC_POWER_ON, 0, 100);
	}
}

static void
stale_stop(void)
{
	need(&gmu);
	need(&rscc);
	need(&smmu);
	need(&gpucc);
	wr(gmu.base + GMU_CM3_SYSRESET, 1);
	gx_off();
	if (rd(rscc.base + RSCC_SEQ_MEM_0_DRV0) == rscc_program[0] &&
	    !(rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {
		wr(gmu.base + GMU_RSCC_CONTROL_REQ, 1);
		wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100);
		wr(gmu.base + GMU_RSCC_CONTROL_REQ, 0);
	}
	if (smmu_layout()) {
		for (uint32_t match = 0; match < 3; match++) {
			wr(smmu.base + SMMU_SMR(match), 0);
			wr(smmu.base + SMMU_S2CR(match), SMMU_S2CR_FAULT);
			wr(smmu.base + (smmu_pages + match) * smmu_page + SMMU_CB_SCTLR, 0);
		}
	}
	wr(gpucc.base + GPUCC_CX_GDSCR, rd(gpucc.base + GPUCC_CX_GDSCR) | GDSC_SW_COLLAPSE);
	for (int i = 0; i < 1000 && cx_is_on(); i++) {
		sleep_us(100);
	}
}

static void
vayu_block_stop(void)
{
	uint32_t slumber[3] = { 0, 0, 0 };

	step = 0;
	newest_step = ~0u;
	/* In the order of redwood's gpu_block_stop and Linux's a6xx_gmu_shutdown: the 3D core first. */
	if (core_runs && core_is_on) {
		printf("vayu: 0: the 3D core before it is stopped: status %#x, interrupts %#x, ring read up to %u of %u\n",
		    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_RB_RPTR), ring_at);
		core_bus_halt(core_stuck);
		printf("vayu: 0: the GPU block's bus has been halted%s\n", core_stuck ? ", the 3D core's side too, and stays so" :
		    " and let go again");
	}
	if (core_is_on) {
		wr(gmu.base + GMU_HOST2GMU_INTR_SET, GMU_CORE_RELEASE);
		core_is_on = false;
	}
	if (gmu_talks && hfi_number != 0) {   /* only a GMU that was given step 18's tables is told to slumber */
		hfi_send("\"slumber\"", HFI_PREPARE_SLUMBER, slumber, 3);
	}
	gmu_talks = false;
	if (gmu_runs) {
		wr(gmu.base + GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
		gx_off();
	}
	if (zap_is_loaded) {
		/* the next kernel loads its own (redwood's step 19 explains why) */
		printf("vayu: 0: the zap shader stays loaded in TrustZone (no PAS_SHUTDOWN: it hung this phone); restart the "
		    "phone before another step-22 run\n");
		zap_is_loaded = false;
	}
	if (core_space_is_on) {
		if (smmu_valid) {
			space_faulted(&core_space);
			space_off(&core_space, CORE_MATCH);
		}
		core_space_is_on = core_programmed = core_runs = unsecured = false;
	}
	if (rscc_programmed && rscc.base != 0 && !(rd(rscc.base + RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {
		wr(gmu.base + GMU_RSCC_CONTROL_REQ, 1);
		if (!wait_for(&rscc, RSCC_RSC_STATUS0_DRV0, RSCC_ASLEEP, RSCC_ASLEEP, 100)) {
			printf("vayu: 0: the GMU's RPMh channel does not go to sleep\n");
		}
		wr(gmu.base + GMU_RSCC_CONTROL_REQ, 0);
	}
	if (gmu_space_is_on) {
		if (smmu_valid) {
			space_faulted(&gmu_space);
			space_off(&gmu_space, GMU_MATCH);
		}
		gmu_space_is_on = false;
	}
	if (cx_on_by_us && gpucc.base != 0) {
		if (rd(gpucc.base + GPUCC_GX_GDSCR) & GDSC_POWER_ON) {
			/* Collapsing CX under a 3D core that still has power has hung the phone on redwood (its step 4). */
			printf("vayu: 0: the 3D core still has power: leaving CX on rather than collapsing it underneath\n");
		} else {
			wr(gpucc.base + GPUCC_CX_GDSCR, rd(gpucc.base + GPUCC_CX_GDSCR) | GDSC_SW_COLLAPSE);
			for (int i = 0; i < 1000 && cx_is_on(); i++) {
				sleep_us(100);
			}
			printf("vayu: 0: CX is %s again\n", cx_is_on() ? "still on" : "off");
		}
	}
	cx_on_by_us = false;
}

void
vayu_adreno_stop(void)
{
	if (gpu_kept) {
		return;                         /* hakios_adreno_stop() gives it back through HakiosGPUStop() */
	}
	if (gmu.base == 0 && !gmu_space_is_on && !gmu_runs && !cx_on_by_us) {
		return;
	}
	printf("vayu: the run is about to end: the GPU block is stopped again\n");
	vayu_block_stop();
}

void
vayu_adreno_quiesce(void)
{
	if (gmu_runs) {
		wr(gmu.base + GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
	}
	if (core_runs) {
		core_bus_halt(true);
		core_runs = false;
	}
	if (core_space_is_on && smmu_valid && smmu.base != 0) {
		wr(smmu.base + smmu_context(&core_space) + SMMU_CB_SCTLR, 0);
		core_space_is_on = false;
	}
	if (gmu_space_is_on && smmu_valid && smmu.base != 0) {
		wr(smmu.base + smmu_context(&gmu_space) + SMMU_CB_SCTLR, 0);
		gmu_space_is_on = false;
	}
}

/* ---- all of it together ------------------------------------------------------------------------------- */

static bool(*const steps[])(void) = {
	step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks, step_first_words, step_rscc, step_smmu,
	step_smmu_context, step_gmu_memory, step_trustzone, step_zap_signature, step_gmu_space, step_gmu_firmware,
	step_gmu_start, step_gmu_queues, step_core_look, step_core_power, step_core_space, step_core_registers,
	step_core_start, step_core_unsecure,
};
#define STEPS                   ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

extern vm_offset_t pe_arm_get_soc_base_phys(void);

static bool
start_by_itself(void)
{
	const char *missing = NULL;
	uint64_t size;
	bool want_zap = !(skip_steps & 1u << 12);

	if (pe_arm_get_soc_base_phys() != 0x00400000ul) {
		printf("vayu: THE GPU IS NOT STARTED: vayu_gpu=1 is for the phone, and this is not it\n");
		return false;
	}
	image_asked_quietly = true;
	if (last_step >= 13 && !(skip_steps & 1u << 13) && image_file("fw", "a640_gmu.bin", &size) == NULL) {
		missing = "a640_gmu.bin";
	}
	if (missing == NULL && want_zap && last_step >= 12 && image_file("fw", "a640_zap.mdt", &size) == NULL) {
		missing = "a640_zap.mdt";
	}
	if (missing == NULL && last_step >= 19 && image_file("fw", "a630_sqe.fw", &size) == NULL) {
		missing = "a630_sqe.fw";
	}
	if (missing == NULL && last_step >= 22 && image_file("fw", "a640_zap.mdt", &size) == NULL) {
		missing = "a640_zap.mdt (and its a640_zap.bNN)";
	}
	image_asked_quietly = false;
	if (missing != NULL) {
		printf("vayu: THE GPU IS NOT STARTED: the small image has no fw/%s (the phone's /vendor/firmware/%s goes into "
		    "fw-gpu/); the system draws with its software renderer\n", missing, missing);
		return false;
	}
	return true;
}

/*
 * Called from hakios_input_init(), beside hakios_adreno_init(), while the system starts. It
 * does nothing at all unless both gpu=1 and vayu_gpu=1 are on the boot line.
 */
void
vayu_adreno_init(void)
{
	uint32_t on = 0, steps_wanted = 0, skip = 0, done = 0, failed = 0, keep = 0;

	if (!PE_parse_boot_argn("gpu", &on, sizeof(on)) || !on) {
		return;
	}
	if (!PE_parse_boot_argn("vayu_gpu", &on, sizeof(on)) || !on) {
		return;
	}
	/*
	 * gpu=1 vayu_gpu=1 alone: the GPU for the system to draw with, as redwood's gpu=1 is -- steps 1
	 * to 22 without pauses (all passed on the phone, 2026-10-23 172532..195916), then kept for the
	 * service HakiosGPU (vayu_keep=0 stops it again instead). vayu_adreno=N is the staged bring-up
	 * as before: steps 1..N, the pauses, and nothing kept unless vayu_keep=1.
	 */
	last_step = 22;
	skip_steps = 1u << 11 | 1u << 12;       /* step 22 checks and loads the whole zap shader itself */
	pause_ms = 0;
	watch_seconds = 0;
	keep = 1;
	if (PE_parse_boot_argn("vayu_adreno", &steps_wanted, sizeof(steps_wanted)) && steps_wanted != 0) {
		last_step = steps_wanted;
		skip_steps = steps_wanted > 16 ? 1u << 11 | 1u << 12 : 0;
		pause_ms = 1000;
		watch_seconds = 5;
		keep = 0;
		staged = true;
	}
	PE_parse_boot_argn("vayu_skip", &skip, sizeof(skip));
	if (skip != 0) {
		skip_steps = skip;
	}
	PE_parse_boot_argn("vayu_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("vayu_fine", &fine, sizeof(fine));
	PE_parse_boot_argn("vayu_watch", &watch_seconds, sizeof(watch_seconds));
	if (last_step == 0) {
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	if (!start_by_itself()) {
		return;
	}
	for (newest_step = last_step; newest_step > 0 && (skip_steps & 1u << newest_step); newest_step--) {
	}

	printf("vayu: steps up to %u, without those in mask %#x; step %u is the new one and waits %u ms %s\n", last_step,
	    skip_steps, newest_step, pause_ms, fine ? "before every access" : "after each announcement");
	for (step = 1; step <= last_step; step++) {
		if (skip_steps & 1u << step) {
			continue;
		}
		if (!steps[step - 1]()) {
			printf("vayu: step %u: FAILED; the steps after it are not attempted\n", step);
			failed = step;
			break;
		}
		printf("vayu: step %u: done\n", step);
		done = step;
	}
	if (done >= 22) {
		printf("vayu: THE 3D CORE RAN COMMANDS OUT OF SECURE MODE. Stopping it again: this port does not hand it to the "
		    "window server yet\n");
	} else if (done >= 21) {
		printf("vayu: THE 3D CORE TAKES COMMANDS (secure mode not left). Stopping it again\n");
	} else if (done >= 16) {
		printf("vayu: THE GPU BLOCK IS UP (GMU firmware runs, HFI queues up). Stopping it again: this port does not hand "
		    "it to the window server yet\n");
	}
	PE_parse_boot_argn("vayu_keep", &keep, sizeof(keep));
	if (keep && done >= 22 && gpu_keep()) {
		return;                         /* the block is the service's now; HakiosGPUStop() gives it back */
	}
	vayu_block_stop();
	for (uint32_t second = 1; second <= watch_seconds; second++) {
		sleep_ms(1000);
		printf("vayu: alive %u s after step %u; %s\n", second, done, last_write);
	}
	printf("vayu: over (%s at step %u); the system goes on starting with its software renderer\n",
	    failed != 0 ? "FAILED" : "reached", failed != 0 ? failed : done);
}
